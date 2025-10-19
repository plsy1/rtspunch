#ifndef RTSP_H
#define RTSP_H

#include <stdint.h>
#include <stddef.h>
#include "rtp.h"
struct play_ctx
{
    struct rtp_buffer *rtp_buf;
    int play;
    int stop;
    int seq;
    int sockfd;
    char session_id[256];
    char last_location[512];
    int rtp_sock;
    int rtcp_sock;
    struct sockaddr_in rtp_server;
    struct sockaddr_in rtcp_server;
    uint32_t ssrc;
    int http_sock;
    const char *rtsp_url;
    int max_rtp_buffer_size;
    int max_udp_packet_size;
};

void rtsp_play_stream(const char *rtsp_url, int http_fd);

#endif