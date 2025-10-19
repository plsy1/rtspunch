#define _GNU_SOURCE
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "rtp.h"
#include "rtcp.h"
#include "logs.h"
#include <errno.h>
#include "stun.h"
#include "rtsp.h"
#include "config.h"

struct rtsp_uri
{
    char host[256];
    int port;
    char path[512];
};

static void *rtp_thread(void *arg)
{
    struct play_ctx *ctx = (struct play_ctx *)arg;

    ctx->rtp_buf->head = 0;
    ctx->rtp_buf->tail = 0;

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, rtp_receive_thread, ctx) != 0)
    {
        fprintf(stderr, "Failed to create receive thread\n");
        return NULL;
    }
    else
    {
        // LOG_INFO("Start rtp receive thread");
    }

    pthread_t send_thread;
    if (pthread_create(&send_thread, NULL, rtp_send_thread, ctx) != 0)
    {
        fprintf(stderr, "Failed to create send thread\n");
        return NULL;
    }
    else
    {
        // LOG_INFO("Start rtp send thread");
    }

    pthread_join(recv_thread, NULL);
    pthread_join(send_thread, NULL);

    return NULL;
}

static void *rtcp_thread(void *arg)
{

    return NULL;
}

static int parse_rtsp_uri(const char *uri, struct rtsp_uri *out)
{
    if (!uri || strncmp(uri, "rtsp://", 7) != 0)
        return -1;
    const char *p = uri + 7;
    const char *slash = strchr(p, '/');
    if (!slash)
        return -1;
    size_t hostlen = slash - p;
    char hostport[320];
    if (hostlen >= sizeof(hostport))
        return -1;
    memcpy(hostport, p, hostlen);
    hostport[hostlen] = '\0';
    char *colon = strchr(hostport, ':');
    if (colon)
    {
        *colon = '\0';
        strncpy(out->host, hostport, sizeof(out->host) - 1);
        out->port = atoi(colon + 1);
        if (out->port <= 0)
            out->port = 554;
    }
    else
    {
        strncpy(out->host, hostport, sizeof(out->host) - 1);
        out->port = 554;
    }
    snprintf(out->path, sizeof(out->path), "%s", slash); // include leading '/'
    return 0;
}

static int connect_host(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;
    int s = -1;
    for (rp = res; rp; rp = rp->ai_next)
    {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0)
            continue;
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);

    return s;
}

static ssize_t send_all(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    const char *p = buf;
    while (sent < len)
    {
        ssize_t r = send(fd, p + sent, len - sent, 0);
        if (r <= 0)
            return -1;
        sent += r;
    }
    return sent;
}

static int recv_response(char *buf, size_t bufsz, void *ctx)
{

    struct play_ctx *_control = ctx;
    memset(buf, 0, bufsz);
    size_t total = 0;

    while (total < bufsz - 1)
    {
        ssize_t n = recv(_control->sockfd, buf + total, bufsz - 1 - total, 0);
        if (n <= 0)
            return -1;
        total += n;
        buf[total] = '\0';

        if (strstr(buf, "\r\n\r\n"))
            break;
    }

    return (int)total;
}

static int parse_status_code(const char *resp)
{
    int code = 0;
    if (sscanf(resp, "RTSP/%*s %d", &code) == 1)
        return code;
    return -1;
}

static char *get_header_value(const char *resp, const char *header)
{
    // returns pointer into resp where value begins. Caller should not free.
    char *h = strcasestr(resp, header);
    if (!h)
        return NULL;
    char *colon = strchr(h, ':');
    if (!colon)
        return NULL;
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    // trim end of line
    char *eol = strchr(v, '\r');
    if (!eol)
        eol = strchr(v, '\n');
    if (eol)
        *eol = '\0';
    return v;
}

static int send_request(const char *method, const char *uri, const char *extra_headers, const char *body, char *resp, size_t resp_sz, void *ctx)
{
    struct play_ctx *_control = ctx;
    char req[4096];
    int len = 0;
    len += snprintf(req + len, sizeof(req) - len, "%s %s RTSP/1.0\r\n", method, uri);
    len += snprintf(req + len, sizeof(req) - len, "CSeq: %d\r\n", _control->seq++);
    if (_control->session_id[0])
    {
        len += snprintf(req + len, sizeof(req) - len, "Session: %s", _control->session_id);
    }
    if (extra_headers)
        len += snprintf(req + len, sizeof(req) - len, "%s", extra_headers);
    if (body)
    {
        len += snprintf(req + len, sizeof(req) - len, "Content-Length: %zu\r\n", strlen(body));
        len += snprintf(req + len, sizeof(req) - len, "\r\n");
        len += snprintf(req + len, sizeof(req) - len, "%s", body);
    }
    else
    {
        len += snprintf(req + len, sizeof(req) - len, "\r\n");
    }
    if (send_all(_control->sockfd, req, len) != len)
        return -1;
    int r = recv_response(resp, resp_sz, ctx);
    if (r <= 0)
        return -1;

    return r;
}

static int do_options(const char *uri, char *resp, size_t resp_sz, void *ctx)
{
    return send_request("OPTIONS", uri, NULL, NULL, resp, resp_sz, ctx);
}

static int do_describe(const char *uri, char *resp, size_t resp_sz, void *ctx)
{
    struct play_ctx *_control = ctx;
    char headers[256];
    snprintf(headers, sizeof(headers), "Accept: application/sdp\r\n");
    int r = send_request("DESCRIBE", uri, headers, NULL, resp, resp_sz, ctx);
    if (r > 0)
    {
        // extract Content-Base or Location
        char *cb = get_header_value(resp, "Content-Base");
        if (!cb)
            cb = get_header_value(resp, "Content-Location");
        if (cb)
            strncpy(_control->last_location, cb, sizeof(_control->last_location) - 1);
    }
    return r;
}

static int do_setup(const char *uri, int client_rtp_port, char *resp, size_t resp_sz, void *ctx)
{

    struct play_ctx *_control = ctx;
    char headers[256];
    snprintf(headers, sizeof(headers), "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n", client_rtp_port, client_rtp_port + 1);
    int r = send_request("SETUP", uri, headers, NULL, resp, resp_sz, ctx);
    if (r > 0)
    {
        char *sess_line = strstr(resp, "Session:");
        if (sess_line)
        {
            sess_line += strlen("Session:");
            while (*sess_line == ' ')
                sess_line++;

            char *line_end = strchr(sess_line, '\n');
            if (!line_end)
                line_end = sess_line + strlen(sess_line);

            char *semi = strchr(sess_line, ';');
            if (semi && semi < line_end)
                line_end = semi;

            size_t len = line_end - sess_line;
            if (len >= sizeof(_control->session_id))
                len = sizeof(_control->session_id) - 1;
            strncpy(_control->session_id, sess_line, len);
            _control->session_id[len] = '\0';
        }
    }
    return r;
}

static int do_play(const char *uri, const char *range, char *resp, size_t resp_sz, void *ctx)
{
    char headers[256] = {0};
    if (range && range[0])
        snprintf(headers, sizeof(headers), "Range: %s\r\n", range);
    return send_request("PLAY", uri, headers, NULL, resp, resp_sz, ctx);
}

static int do_GET_PARAMETER(const char *uri, char *resp, size_t resp_sz, void *ctx)
{
    char headers[256] = {0};
    return send_request("GET_PARAMETER", uri, headers, NULL, resp, resp_sz, ctx);
}

static int do_pause(const char *uri, char *resp, size_t resp_sz, void *ctx)
{
    return send_request("PAUSE", uri, NULL, NULL, resp, resp_sz, ctx);
}

static int do_teardown(const char *uri, char *resp, size_t resp_sz, void *ctx)
{
    return send_request("TEARDOWN", uri, NULL, NULL, resp, resp_sz, ctx);
}

int random_rtp_port()
{
    static int initialized = 0;
    if (!initialized)
    {
        srand(time(NULL) ^ getpid());
        initialized = 1;
    }

    int port = 10000 + (rand() % 25000) * 2;
    return port;
}

static void *get_parameter_thread(void *arg)
{
    struct play_ctx *_control = arg;
    char resp[8192];

    time_t last_time = time(NULL);

    while (!_control->stop)
    {
        if (_control->stop)
        {
            break;
        }

        time_t current_time = time(NULL);
        if (difftime(current_time, last_time) >= 10)
        {
            if (do_GET_PARAMETER(_control->last_location, resp, sizeof(resp), _control) <= 0)
            {
                LOG_ERROR("Failed to send GET_PARAMETER request");
                break;
            }

            last_time = current_time;
        }
        sleep(1);
    }

    // LOG_INFO("RTP Keepalive thread: quit.");
    return NULL;
}

void rtsp_play_stream(const char *rtsp_url, int http_fd)
{

    int th_rtp_created = 0, th_rtcp_created = 0, th_get_param_created = 0;

    const struct server_config *config = get_server_config();
    int rtp_port = random_rtp_port();

    struct rtsp_uri uri;
    char resp[8192];

    struct play_ctx *ctx = (struct play_ctx *)malloc(sizeof(struct play_ctx));
    if (ctx == NULL)
    {
        LOG_ERROR("Failed to allocate memory for play_ctx.");
        return;
    }

    ctx->rtp_buf = (struct rtp_buffer *)malloc(sizeof(struct rtp_buffer));
    if (ctx->rtp_buf == NULL)
    {
        LOG_ERROR("Failed to allocate memory for rtp_buffer.");
        free(ctx); // 释放 ctx 内存
        return;    // 或者其他错误处理
    }

    if (init_rtp_buffer(ctx->rtp_buf) < 0)
    {
        LOG_ERROR("Failed to allocate memory for rtp_buffer.");
        free(ctx->rtp_buf);
        free(ctx);
        return;
    }

    pthread_t th_rtp, th_rtcp, th_get_param;

    ctx->rtp_sock = rtp_open(rtp_port);
    ctx->rtcp_sock = rtcp_open(rtp_port + 1);

    ctx->max_rtp_buffer_size = config->max_rtp_buffer_size;
    ctx->max_udp_packet_size = config->max_udp_packet_size;

    int wan_port = 0;
    int setup_rtp_port;
    char pub_ip[64];

    if (config->enable_nat)
    {

        wan_port = get_wan_port_existing_socket(ctx->rtp_sock, pub_ip, sizeof(pub_ip));

        if (wan_port >= 0)
            LOG_DEBUG("Public mapping obtained: %s:%d -> %d", pub_ip, wan_port, rtp_port);
        else
        {
            LOG_ERROR("STUN failed to get public mapping");
            goto cleanup;
        }
    }

    if (wan_port == 0)
        setup_rtp_port = rtp_port;
    else
        setup_rtp_port = wan_port;

    if (parse_rtsp_uri(rtsp_url, &uri) != 0)
    {
        LOG_ERROR("Invalid RTSP URI");
        goto cleanup;
    }

    ctx->sockfd = connect_host(uri.host, uri.port);
    ctx->seq = 1;
    ctx->stop = 0;

    if (ctx->sockfd < 0)
    {
        LOG_ERROR("Failed to connect host");
        goto cleanup;
    }

    if (do_options(rtsp_url, resp, sizeof(resp), ctx) <= 0)
    {
        LOG_ERROR("Failed to do OPTIONS request, response: %s", resp);
        goto cleanup;
    }

    if (do_describe(rtsp_url, resp, sizeof(resp), ctx) <= 0)
    {
        LOG_ERROR("Failed to do DESCRIBE request, response: %s", resp);
        goto cleanup;
    }

    if (do_setup(rtsp_url, setup_rtp_port, resp, sizeof(resp), ctx) <= 0)
    {
        LOG_ERROR("Failed to do SETUP request, response: %s", resp);
        goto cleanup;
    }

    int server_rtp = 0, server_rtcp = 0;
    char *tp = get_header_value(resp, "Transport");
    if (tp)
    {
        char *sp = strstr(tp, "server_port=");
        if (sp)
        {
            sscanf(sp + strlen("server_port="), "%d-%d", &server_rtp, &server_rtcp);
        }
    }

    // Initialize context for RTP and RTCP
    ctx->http_sock = http_fd;
    ctx->ssrc = 0x11223344;
    ctx->rtsp_url = rtsp_url;
    ctx->play = 0;

    // Initialize RTP and RTCP server addresses
    memset(&ctx->rtcp_server, 0, sizeof(ctx->rtcp_server));
    ctx->rtcp_server.sin_family = AF_INET;
    ctx->rtcp_server.sin_port = htons(server_rtcp);
    inet_pton(AF_INET, uri.host, &ctx->rtcp_server.sin_addr);

    memset(&ctx->rtp_server, 0, sizeof(ctx->rtp_server));
    ctx->rtp_server.sin_family = AF_INET;
    ctx->rtp_server.sin_port = htons(server_rtp);
    inet_pton(AF_INET, uri.host, &ctx->rtp_server.sin_addr);

    if (pthread_create(&th_rtp, NULL, rtp_thread, ctx) != 0)
    {
        LOG_ERROR("Failed to create RTP thread");
        goto cleanup;
    }
    else
    {
    }

    if (pthread_create(&th_rtcp, NULL, rtcp_thread, ctx) != 0)
    {
        LOG_ERROR("Failed to create RTCP thread");
        goto cleanup;
    }
    else
    {
    }

    if (do_play(rtsp_url, "npt=0.000-", resp, sizeof(resp), ctx) <= 0)
    {
        LOG_ERROR("Failed to do PLAY request, response: %s", resp);
        goto cleanup;
    }
    else
    {
        ctx->play = 1;
    }

    if (pthread_create(&th_get_param, NULL, get_parameter_thread, ctx) != 0)
    {
        LOG_ERROR("Failed to create parameter thread");
        goto cleanup;
    }
    else
    {
    }

    pthread_join(th_rtp, NULL);

    pthread_join(th_rtcp, NULL);

    pthread_join(th_get_param, NULL);

    do_teardown(rtsp_url, resp, sizeof(resp), ctx);

cleanup:
    close(ctx->sockfd);
    rtp_close(ctx->rtp_sock);
    rtcp_close(ctx->rtcp_sock);
    free_rtp_buffer(ctx->rtp_buf);
    free(ctx);
}
