
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "rtp.h"
#include <errno.h>
#include "config.h"
#include "logs.h"
#include "rtsp.h"
#include <fcntl.h>
#include "config.h"
#include <stdlib.h>
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

int rtp_open(int client_port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(client_port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return -1;
    return s;
}

int init_rtp_buffer(struct rtp_buffer *rtp_buf)
{
    const struct server_config *config = get_server_config();

    rtp_buf->buffer = (uint8_t **)malloc(config->max_rtp_buffer_size * sizeof(uint8_t *));
    rtp_buf->payload_sizes = (size_t *)malloc(config->max_rtp_buffer_size * sizeof(size_t));

    if (!rtp_buf->buffer || !rtp_buf->payload_sizes)
    {

        LOG_ERROR("Failed to allocate memory for RTP buffer or payload_sizes");
        return -1;
    }

    for (int i = 0; i < config->max_rtp_buffer_size; i++)
    {
        rtp_buf->buffer[i] = (uint8_t *)malloc(config->max_udp_packet_size);
        if (!rtp_buf->buffer[i])
        {
            for (int j = 0; j < i; j++)
            {
                free(rtp_buf->buffer[j]);
            }
            free(rtp_buf->buffer);
            free(rtp_buf->payload_sizes);
            LOG_ERROR("Failed to allocate memory for RTP packet");
            return -2;
        }
    }

    rtp_buf->head = 0;
    rtp_buf->tail = 0;

    return 0;
}

void free_rtp_buffer(struct rtp_buffer *rtp_buf)
{
    if (rtp_buf == NULL)
        return;

    const struct server_config *config = get_server_config();

    for (int i = 0; i < config->max_rtp_buffer_size; i++)
    {
        free(rtp_buf->buffer[i]);
    }
    free(rtp_buf->buffer);
    free(rtp_buf->payload_sizes);
}

int rtp_send_trigger(int sockfd, struct sockaddr_in *server, uint32_t ssrc)
{
    uint8_t buf[12] = {0};

    buf[0] = 0x80;      // Version=2, no padding, no extension, CC=0
    buf[1] = 96 & 0x7F; // Payload type=96
    buf[2] = 0;         // Sequence number high byte
    buf[3] = 0;         // Sequence number low byte
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;                   // Timestamp
    buf[8] = (ssrc >> 24) & 0xFF; // SSRC
    buf[9] = (ssrc >> 16) & 0xFF;
    buf[10] = (ssrc >> 8) & 0xFF;
    buf[11] = ssrc & 0xFF;

    ssize_t n = sendto(sockfd, buf, sizeof(buf), 0,
                       (struct sockaddr *)server, sizeof(*server));
    if (n != sizeof(buf))
    {
        return -1;
    }
    return 0;
}

void rtp_close(int sockfd) { close(sockfd); }

void send_http_response(int sock)
{
    const char *response_header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/mp2t\r\n"
        "Connection: close\r\n"
        "\r\n";

    send(sock, response_header, strlen(response_header), 0);
}

int get_rtp_payload(uint8_t *buf, int recv_len, uint8_t **payload, int *size, uint16_t *seqn)
{
    int payloadstart, payloadlength;
    uint8_t flags;

    if (likely(recv_len >= 12) && likely((buf[0] & 0xC0) == 0x80))
    {

        if (seqn)
        {
            *seqn = ntohs(*(uint16_t *)(buf + 2));
        }

        flags = buf[0];

        payloadstart = 12;
        payloadstart += (flags & 0x0F) * 4;

        if (unlikely(flags & 0x10))
        {

            if (unlikely(payloadstart + 4 > recv_len))
            {
                printf("Malformed RTP packet: extension header truncated\n");
                return -1;
            }
            payloadstart += 4 + 4 * ntohs(*((uint16_t *)(buf + payloadstart + 2)));
        }

        payloadlength = recv_len - payloadstart;

        if (unlikely(flags & 0x20))
        {
            payloadlength -= buf[recv_len - 1];
        }

        if (unlikely(payloadlength <= 0) || unlikely(payloadstart + payloadlength > recv_len))
        {
            printf("Malformed RTP packet: invalid payload length\n");
            return -1;
        }

        *payload = buf + payloadstart;
        *size = payloadlength;
        return 1;
    }
    else
    {
        *payload = buf;
        *size = recv_len;
        return 0;
    }
}

void *rtp_receive_thread(void *arg)
{
    struct play_ctx *ctx = (struct play_ctx *)arg;
    struct rtp_buffer *rtp_buf = ctx->rtp_buf;

    uint8_t *buf = (uint8_t *)malloc(ctx->max_udp_packet_size);
    if (buf == NULL)
    {
        LOG_ERROR("Failed to allocate memory for UDP receive buffer.");
        return NULL;
    }

    ssize_t n = 0;
    const struct server_config *config = get_server_config();

    uint16_t seqn = 0;
    uint16_t not_first = 0;

    int first_package = 0;

    send_http_response(ctx->http_sock);

    if (!config->enable_nat)
        rtp_send_trigger(ctx->rtp_sock, &ctx->rtp_server, ctx->ssrc);

    while (!ctx->stop)
    {
        n = recv(ctx->rtp_sock, buf, ctx->max_udp_packet_size, 0);
        if (n <= 0)
        {
            if (n == 0)
            {
                LOG_WARN("RTP socket closed by peer");
            }
            else
            {
                LOG_WARN("Error receiving RTP data: %s", strerror(errno));
            }
            break;
        }

        uint8_t *payload = NULL;
        int payload_size = 0;

        int is_rtp = get_rtp_payload(buf, n, &payload, &payload_size, &seqn);
        if (is_rtp <= 0)
        {
            LOG_WARN("Non-RTP packet received, skipping");
            continue;
        }
        else
        {

        }

        while ((rtp_buf->head + 1) % ctx->max_rtp_buffer_size == rtp_buf->tail)
        {
            usleep(1000);
        }

        if (ctx->play)
        {
            memcpy(rtp_buf->buffer[rtp_buf->head], payload, payload_size);
            rtp_buf->payload_sizes[rtp_buf->head] = payload_size;
            rtp_buf->head = (rtp_buf->head + 1) % ctx->max_rtp_buffer_size;
        }
        else
        {
        }
    }

    free(buf);

    return NULL;
}

void *rtp_send_thread(void *arg)
{
    struct play_ctx *ctx = (struct play_ctx *)arg;
    struct rtp_buffer *rtp_buf = ctx->rtp_buf;

    while (!ctx->stop)
    {
        while (rtp_buf->head == rtp_buf->tail)
        {
            usleep(1000);
        }

        ssize_t sent = send(ctx->http_sock,
                            rtp_buf->buffer[rtp_buf->tail],
                            rtp_buf->payload_sizes[rtp_buf->tail],
                            0);
        if (sent < 0)
        {
            ctx->stop = 1;
            break;
        }

        rtp_buf->tail = (rtp_buf->tail + 1) % ctx->max_rtp_buffer_size;
    }

    return NULL;
}