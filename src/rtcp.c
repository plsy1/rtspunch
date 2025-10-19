#include "rtcp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

struct rtcp_rr {
    uint8_t v_p_count;
    uint8_t pt;
    uint16_t length;
    uint32_t ssrc;
} __attribute__((packed));

int rtcp_open(int client_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(client_port + 1);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) return -1;
    return s;
}

int rtcp_send_rr(int sockfd, struct sockaddr_in *server, uint32_t ssrc) {
    struct rtcp_rr rr;
    rr.v_p_count = (2 << 6);  // V=2, P=0, count=0
    rr.pt = 201;              // RTCP RR packet type
    rr.length = htons(1);     // 1 word (4 bytes after header)
    rr.ssrc = htonl(ssrc);
    return sendto(sockfd, &rr, sizeof(rr), 0, (struct sockaddr*)server, sizeof(*server));
}

void rtcp_close(int sockfd) { close(sockfd); }