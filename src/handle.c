#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <strings.h>
#include "stun.h"
#include "logs.h"

#define BUF_SIZE 4096
#define UDP_BUF 2048

static int rtp_base_port = 20000;
static pthread_mutex_t port_lock = PTHREAD_MUTEX_INITIALIZER;
static const unsigned char TRIGGER_PKT[12] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

typedef struct
{
    int client_fd;
    struct sockaddr_in client_addr;

    int local_rtp;
    int local_rtcp;

    int local_rtp_sock;
    int local_rtcp_sock;

    int orig_client_rtp;
    int orig_client_rtcp;

    int server_rtp;
    int server_rtcp;

    char up_host[128];
    char up_path[256];
    int up_port;

    int nat_port;

} client_info_t;

typedef struct
{
    int sockfd;
    char up_ip[64];
    int up_port;
    char client_ip[64];
    int client_port;
} oneway_relay_t;

int create_bound_udp(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in b = {0};
    b.sin_family = AF_INET;
    b.sin_addr.s_addr = INADDR_ANY;
    b.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&b, sizeof(b)) < 0)
    {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}

int send_trigger_from_port(const char *up_ip, int up_port, int local_src_port,
                           const unsigned char *payload, int payload_len)
{
    int s = -1;
    struct sockaddr_in bind_addr = {0}, to = {0};
    int rv = -1;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
    {
        LOG_ERROR("Failed to create trigger socket: %s", strerror(errno));
        return -1;
    }

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(local_src_port);
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        LOG_ERROR("Failed to bind trigger socket: %s", strerror(errno));
        close(s);
        return -1;
    }

    to.sin_family = AF_INET;
    to.sin_port = htons(up_port);
    if (inet_pton(AF_INET, up_ip, &to.sin_addr) != 1)
    {
        LOG_ERROR("Invalid upstream IP: %s", up_ip);
        close(s);
        return -1;
    }

    ssize_t sent = sendto(s, payload, payload_len, 0, (struct sockaddr *)&to, sizeof(to));
    if (sent != payload_len)
    {
        if (sent < 0)
            LOG_ERROR("Failed to send trigger: %s", strerror(errno));
        else
            LOG_ERROR("Partial trigger sent %zd/%d bytes", sent, payload_len);
        rv = -1;
    }
    else
    {
        rv = 0;
    }

    close(s);
    return rv;
}

void *oneway_udp_relay(void *arg)
{
    oneway_relay_t *r = (oneway_relay_t *)arg;
    int sock = r->sockfd;
    struct sockaddr_in client_addr = {0};
    if (sock < 0)
    {
        free(r);
        return NULL;
    }

    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(r->client_port);
    if (inet_pton(AF_INET, r->client_ip, &client_addr.sin_addr) != 1)
    {
        LOG_ERROR("Invalid client IP: %s", r->client_ip);
        close(sock);
        free(r);
        return NULL;
    }

    LOG_INFO("One-way UDP relay: sock=%d listening -> forward to %s:%d (up=%s:%d)",
             sock, r->client_ip, r->client_port, r->up_ip[0] ? r->up_ip : "-", r->up_port);

    unsigned char buf[UDP_BUF];
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);

    while (1)
    {
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
        if (n <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(100000); // 100ms
                continue;
            }
            if (errno == EINTR)
                continue;
            perror("recvfrom(relay)");
            break;
        }

        ssize_t s = sendto(sock, buf, n, 0, (struct sockaddr *)&client_addr, sizeof(client_addr));
        if (s < 0)
        {
            perror("sendto -> client");
        }
        else
        {
            // printf("fwd %zd bytes from %s:%d -> %s:%d\n",
            //        n, inet_ntoa(src.sin_addr), ntohs(src.sin_port), r->client_ip, r->client_port);
        }
    }

    close(sock);
    free(r);
    return NULL;
}

int parse_rtsp_url(const char *url, char *host, int *port, char *path)
{
    char full_path[512] = {0};
    if (sscanf(url, "rtsp://%*[^/]%511s", full_path) == 1)
    {
        if (strncmp(full_path, "/rtp/", 5) == 0)
        {
            if (sscanf(full_path, "/rtp/%127[^:]:%d/%511[^\n]", host, port, path) == 3)
                return 0;
        }
    }

    if (sscanf(url, "rtsp://%127[^:/]:%d%127[^\n]", host, port, path) == 3)
        return 0;
    if (sscanf(url, "rtsp://%127[^/]%127[^\n]", host, path) == 2)
    {
        *port = 554;
        return 0;
    }
    return -1;
}

int rewrite_client_request(client_info_t *info, char *buf, int buf_size)
{
    char method[16], url[512], version[32];
    if (sscanf(buf, "%15s %511s %31s", method, url, version) != 3)
        return -1;

    char up_host[128] = {0}, up_path[256] = {0};
    int up_port = 0;
    if (parse_rtsp_url(url, up_host, &up_port, up_path) != 0)
        return -1;

    strncpy(info->up_host, up_host, sizeof(info->up_host) - 1);
    strncpy(info->up_path, up_path, sizeof(info->up_path) - 1);
    info->up_port = up_port;

    char new_url[512];
    snprintf(new_url, sizeof(new_url), "rtsp://%s:%d/%s", info->up_host, info->up_port, info->up_path);

    char *end_line = strstr(buf, "\r\n");
    if (!end_line)
        return -1;
    char *suffix = end_line;

    char tmp[BUF_SIZE];
    snprintf(tmp, sizeof(tmp), "%s %s %s%s", method, new_url, version, suffix);

    if ((int)strlen(tmp) >= buf_size)
        return -1;
    strcpy(buf, tmp);

    if (strncasecmp(method, "SETUP", 5) == 0)
    {
        char *p = strcasestr(buf, "Transport:");
        if (p)
        {
            char *end = strstr(p, "\r\n");
            if (end)
            {

                end += 2;

                if (sscanf(p, "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d",
                           &info->orig_client_rtp, &info->orig_client_rtcp) != 2)
                {
                    info->orig_client_rtp = info->orig_client_rtcp = 0;
                }

                char new_transport[128];
                snprintf(new_transport, sizeof(new_transport),
                         "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n",
                         info->nat_port, info->nat_port + 1);

                char tmp2[BUF_SIZE];
                int prefix_len = p - buf;
                snprintf(tmp2, sizeof(tmp2), "%.*s%s%s", prefix_len, buf, new_transport, end);
                strcpy(buf, tmp2);
                LOG_INFO("Rewrite client_port=%d-%d -> client_port=%d-%d",
                         info->orig_client_rtp, info->orig_client_rtcp, info->local_rtp, info->local_rtcp);
            }
        }
    }

    return (int)strlen(buf);
}

int parse_server_port(const char *resp, int *server_rtp, int *server_rtcp)
{
    const char *p = strcasestr(resp, "Transport:");
    if (!p)
        return -1;
    const char *sp = strstr(p, "server_port=");
    if (!sp)
        return -1;
    if (sscanf(sp, "server_port=%d-%d", server_rtp, server_rtcp) != 2)
        return -1;
    return 0;
}

int recv_rtsp_full(int fd, char *buf, int maxlen)
{
    int total = 0;
    while (1)
    {
        int n = recv(fd, buf + total, maxlen - total, 0);
        if (n <= 0)
            return n;
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n"))
            break;
        if (total >= maxlen - 1)
            break;
    }

    char *cl = strcasestr(buf, "Content-Length:");
    if (cl)
    {
        int len;
        if (sscanf(cl, "Content-Length:%d", &len) == 1)
        {
            char *body = strstr(buf, "\r\n\r\n") + 4;
            int body_len = total - (body - buf);
            while (body_len < len)
            {
                int n = recv(fd, buf + total, maxlen - total, 0);
                if (n <= 0)
                    break;
                total += n;
                body_len += n;
                buf[total] = 0;
            }
        }
    }
    return total;
}

void *tcp_forward(void *arg)
{
    client_info_t *info = (client_info_t *)arg;
    int client_fd = info->client_fd;
    int up_fd = -1;
    char buf[BUF_SIZE];

    LOG_INFO("New Client connect: %s:%d",
             inet_ntoa(info->client_addr.sin_addr),
             ntohs(info->client_addr.sin_port));
    LOG_INFO("Assigned local ports: RTP=%d RTCP=%d (socks: %d/%d)",
             info->local_rtp, info->local_rtcp, info->local_rtp_sock, info->local_rtcp_sock);

    int n = recv_rtsp_full(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        goto cleanup;
    buf[n] = 0;

    int new_len = rewrite_client_request(info, buf, sizeof(buf));
    if (new_len <= 0)
    {
        LOG_ERROR("Failed to rewrite first request");
        goto cleanup;
    }

    char url[256], host[128], path[128];
    int port;
    if (sscanf(buf, "%*s %255s %*s", url) != 1)
    {
        LOG_ERROR("Failed to parse URL");
        goto cleanup;
    }
    if (parse_rtsp_url(url, host, &port, path) != 0)
    {
        LOG_ERROR("Failed to parse URL: %s", url);
        goto cleanup;
    }
    LOG_INFO("Upstream host=%s port=%d path=%s", host, port, path);

    up_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (up_fd < 0)
    {
        perror("socket up_fd");
        goto cleanup;
    }
    struct sockaddr_in up_addr;
    up_addr.sin_family = AF_INET;
    up_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &up_addr.sin_addr) != 1)
    {
        LOG_ERROR("Invalid upstream host: %s", host);
        goto cleanup;
    }
    if (connect(up_fd, (struct sockaddr *)&up_addr, sizeof(up_addr)) != 0)
    {
        LOG_ERROR("Failed to connect to upstream: %s", strerror(errno));
        goto cleanup;
    }
    LOG_INFO("Successfully connected to upstream RTSP %s:%d", host, port);

    if (send(up_fd, buf, new_len, 0) < 0)
    {
        perror("send to upstream");
        goto cleanup;
    }

    n = recv_rtsp_full(up_fd, buf, sizeof(buf) - 1);
    if (n > 0)
    {
        buf[n] = 0;
        send(client_fd, buf, n, 0);
    }

    while (1)
    {
        n = recv_rtsp_full(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        buf[n] = 0;

        char method[16], url2[512], version[32];
        if (sscanf(buf, "%15s %511s %31s", method, url2, version) != 3)
        {
            send(up_fd, buf, n, 0);
            int m = recv_rtsp_full(up_fd, buf, sizeof(buf) - 1);
            if (m > 0)
            {
                buf[m] = 0;
                send(client_fd, buf, m, 0);
            }
            continue;
        }

        int is_play = (strncasecmp(method, "PLAY", 4) == 0);

        int new_len2 = rewrite_client_request(info, buf, sizeof(buf));
        if (new_len2 <= 0)
            new_len2 = n;

        if (send(up_fd, buf, new_len2, 0) < 0)
        {
            LOG_ERROR("Failed to send to upstream: %s", strerror(errno));
            break;
        }

        int m = recv_rtsp_full(up_fd, buf, sizeof(buf) - 1);
        if (m <= 0)
        {
            LOG_ERROR("Upstream closed connection or encountered an error");
            break;
        }
        buf[m] = 0;
        send(client_fd, buf, m, 0);

        if (parse_server_port(buf, &info->server_rtp, &info->server_rtcp) == 0)
        {
        }

        if (is_play)
        {
            if (strstr(buf, "RTSP/1.0 200") != NULL)
            {
                if (info->server_rtp > 0)
                {
                    char client_ip_str[64];
                    inet_ntop(AF_INET, &info->client_addr.sin_addr, client_ip_str, sizeof(client_ip_str));

                    struct sockaddr_in to = {0};
                    to.sin_family = AF_INET;
                    to.sin_port = htons(info->server_rtp);

                    // 这里发不发都行

                    // if (inet_pton(AF_INET, info->up_host, &to.sin_addr) == 1)
                    // {
                    //     ssize_t sent = sendto(info->local_rtp_sock, TRIGGER_PKT, sizeof(TRIGGER_PKT), 0,
                    //                           (struct sockaddr *)&to, sizeof(to));
                    //     if (sent == sizeof(TRIGGER_PKT))
                    //         printf("trigger sent from src sock %d (port %d) to %s:%d\n",
                    //                info->local_rtp_sock, info->local_rtp, info->up_host, info->server_rtp);
                    //     else
                    //         perror("trigger send failed");
                    // }
                    // else
                    // {
                    //     printf("invalid up_host (not IP): %s, trigger skipped\n", info->up_host);
                    // }

                    // start oneway relay: listen mitm_port=local_rtp (use dup of socket so relay owns its fd)

                    oneway_relay_t *relay = malloc(sizeof(oneway_relay_t));
                    if (relay)
                    {
                        memset(relay, 0, sizeof(*relay));
                        int dupfd = dup(info->local_rtp_sock);
                        if (dupfd < 0)
                        {
                            perror("dup socket for relay");
                            free(relay);
                        }
                        else
                        {
                            relay->sockfd = dupfd;
                            strncpy(relay->up_ip, info->up_host, sizeof(relay->up_ip) - 1);
                            relay->up_port = info->server_rtp;
                            strncpy(relay->client_ip, client_ip_str, sizeof(relay->client_ip) - 1);
                            relay->client_port = (info->orig_client_rtp > 0) ? info->orig_client_rtp : info->local_rtp;

                            pthread_t tid;
                            pthread_create(&tid, NULL, oneway_udp_relay, relay);
                            pthread_detach(tid);

                            LOG_INFO("One-way relay started: mitm_port=%d (sock=%d dup) -> %s:%d (up=%s:%d)",
                                     info->local_rtp, dupfd, relay->client_ip, relay->client_port, relay->up_ip, relay->up_port);
                        }
                    }
                }
                else
                {
                    LOG_INFO("PLAY: no server_rtp parsed, cannot trigger or start relay");
                }
            }
            else
            {
                LOG_INFO("PLAY: upstream did not return 200 OK, skipping trigger");
            }
        }
    }

cleanup:
    LOG_INFO("Client disconnected: %s", inet_ntoa(info->client_addr.sin_addr));
    if (up_fd > 0)
        close(up_fd);
    close(client_fd);

    if (info->local_rtp_sock > 0)
        close(info->local_rtp_sock);
    if (info->local_rtcp_sock > 0)
        close(info->local_rtcp_sock);

    free(info);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <port>\n", argv[0]);
        return 1;
    }
    int listen_port = atoi(argv[1]);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(sock, 16) < 0)
    {
        perror("listen");
        return 1;
    }

    LOG_INFO("Rtspunch listening on TCP port: %d", listen_port);

    while (1)
    {
        struct sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);
        int client_fd = accept(sock, (struct sockaddr *)&cli_addr, &len);
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        client_info_t *info = calloc(1, sizeof(client_info_t));
        if (!info)
        {
            close(client_fd);
            continue;
        }
        info->client_fd = client_fd;
        info->client_addr = cli_addr;

        pthread_mutex_lock(&port_lock);
        info->local_rtp = rtp_base_port;
        info->local_rtcp = rtp_base_port + 1;
        rtp_base_port += 2;
        if (rtp_base_port > 60000)
            rtp_base_port = 20000;
        pthread_mutex_unlock(&port_lock);

        info->local_rtp_sock = create_bound_udp(info->local_rtp);
        info->local_rtcp_sock = create_bound_udp(info->local_rtcp);
        if (info->local_rtp_sock < 0 || info->local_rtcp_sock < 0)
        {
            LOG_ERROR("create_bound_udp failed");
            if (info->local_rtp_sock >= 0)
                close(info->local_rtp_sock);
            if (info->local_rtcp_sock >= 0)
                close(info->local_rtcp_sock);
            free(info);
            close(client_fd);
            continue;
        }

        char pub_ip[64];
        int wan_port = get_wan_port_existing_socket(info->local_rtp_sock, pub_ip, sizeof(pub_ip));
        if (wan_port >= 0)
            LOG_INFO("Public mapping obtained: %s:%d", pub_ip, wan_port);
        else
            LOG_ERROR("STUN failed to get public mapping");

        info->nat_port = wan_port;

        pthread_t tid;
        pthread_create(&tid, NULL, tcp_forward, info);
        pthread_detach(tid);
    }

    close(sock);
    return 0;
}