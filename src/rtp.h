#ifndef RTP_H
#define RTP_H
#include <stdint.h>
#include <netinet/in.h>
#include "config.h"

struct rtp_buffer
{
    uint8_t **buffer;      // 动态分配的缓冲区指针数组
    size_t *payload_sizes; // 用于存储每个 RTP 包的负载大小
    int head;              // 接收包的指针
    int tail;              // 发送包的指针
};

int rtp_open(int client_port);
int rtp_send_trigger(int sockfd, struct sockaddr_in *server, uint32_t ssrc);
void rtp_close(int sockfd);

void send_http_response(int sock);
int get_rtp_payload(uint8_t *buf, int recv_len, uint8_t **payload, int *size, uint16_t *seqn);

void *rtp_send_thread(void *arg);
void *rtp_receive_thread(void *arg);

int init_rtp_buffer(struct rtp_buffer *rtp_buf);
void free_rtp_buffer(struct rtp_buffer *rtp_buf);

#endif