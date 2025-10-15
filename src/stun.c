#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include "stun.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#define STUN_MSG_BINDING_REQUEST 0x0001
#define STUN_ATTR_XOR_MAPPED_ADDR 0x0020
#define STUN_ATTR_MAPPED_ADDR 0x0001
#define STUN_MAGIC_COOKIE 0x2112A442

const char *stun_host = "stun.l.google.com";
int stun_port = 19302;

static void gen_tid(unsigned char tid[12])
{
    int i;
    srand((unsigned)time(NULL) ^ (uintptr_t)&tid);
    for (i = 0; i < 12; ++i)
        tid[i] = rand() & 0xFF;
}

static void put16(unsigned char *p, uint16_t v) { *(uint16_t *)p = htons(v); }
static void put32(unsigned char *p, uint32_t v) { *(uint32_t *)p = htonl(v); }

int stun_get_mapping(int s, char *out_pub_ip, int *out_pub_port)
{
    if (s < 0)
        return -1;

    int rv = -1;
    struct addrinfo hints, *res = NULL;
    char sport[16];
    snprintf(sport, sizeof(sport), "%d", stun_port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(stun_host, sport, &hints, &res) != 0)
        return -1;
    struct sockaddr_in stun_addr = *(struct sockaddr_in *)res->ai_addr;

    unsigned char tid[12];
    gen_tid(tid);

    unsigned char req[20];
    put16(req + 0, STUN_MSG_BINDING_REQUEST);
    put16(req + 2, 0); // length 0
    put32(req + 4, STUN_MAGIC_COOKIE);
    memcpy(req + 8, tid, 12);

    int tries = 2;
    for (int t = 0; t < tries; ++t)
    {
        ssize_t sent = sendto(s, req, sizeof(req), 0,
                              (struct sockaddr *)&stun_addr, sizeof(stun_addr));
        if (sent != (ssize_t)sizeof(req))
            continue;

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        struct timeval tv = {2, 0};
        int sel = select(s + 1, &rf, NULL, NULL, &tv);
        if (sel <= 0)
            continue;

        unsigned char rsp[1500];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        ssize_t n = recvfrom(s, rsp, sizeof(rsp), 0, (struct sockaddr *)&src, &sl);
        if (n < 20)
            continue;

        uint16_t msg_type = ntohs(*(uint16_t *)(rsp + 0));
        uint16_t msg_len = ntohs(*(uint16_t *)(rsp + 2));
        uint32_t cookie = ntohl(*(uint32_t *)(rsp + 4));
        if (cookie != STUN_MAGIC_COOKIE)
            continue;
        if (memcmp(rsp + 8, tid, 12) != 0)
            continue;

        int offset = 20;
        while (offset + 4 <= 20 + msg_len && offset + 4 <= n)
        {
            uint16_t attr_type = ntohs(*(uint16_t *)(rsp + offset));
            uint16_t attr_len = ntohs(*(uint16_t *)(rsp + offset + 2));
            int val_off = offset + 4;
            if (val_off + attr_len > n)
                break;

            if (attr_type == STUN_ATTR_XOR_MAPPED_ADDR && attr_len >= 8)
            {
                unsigned char family = rsp[val_off + 1];
                if (family == 0x01)
                {
                    uint16_t xport = ntohs(*(uint16_t *)(rsp + val_off + 2));
                    uint32_t xaddr = ntohl(*(uint32_t *)(rsp + val_off + 4));
                    uint16_t port = xport ^ (STUN_MAGIC_COOKIE >> 16);
                    uint32_t ip = xaddr ^ STUN_MAGIC_COOKIE;
                    struct in_addr ina;
                    ina.s_addr = htonl(ip);
                    inet_ntop(AF_INET, &ina, out_pub_ip, 64);
                    *out_pub_port = port;
                    rv = 0;
                    goto done;
                }
            }
            else if (attr_type == STUN_ATTR_MAPPED_ADDR && attr_len >= 8)
            {
                unsigned char family = rsp[val_off + 1];
                if (family == 0x01)
                {
                    uint16_t port = ntohs(*(uint16_t *)(rsp + val_off + 2));
                    uint32_t ip = ntohl(*(uint32_t *)(rsp + val_off + 4));
                    struct in_addr ina;
                    ina.s_addr = htonl(ip);
                    inet_ntop(AF_INET, &ina, out_pub_ip, 64);
                    *out_pub_port = port;
                    rv = 0;
                    goto done;
                }
            }

            int adv = 4 + attr_len;
            if (attr_len % 4)
                adv += (4 - (attr_len % 4));
            offset += adv;
        }
    }

done:
    if (res)
        freeaddrinfo(res);
    return rv;
}

int get_wan_port_existing_socket(int sock, char *out_pub_ip, size_t ip_len)
{
    if (sock < 0)
        return -1;

    char ipbuf[64] = {0};
    int wan_port = 0;

    if (stun_get_mapping(sock, ipbuf, &wan_port) == 0)
    {
        if (out_pub_ip && ip_len > 0)
        {
            strncpy(out_pub_ip, ipbuf, ip_len - 1);
            out_pub_ip[ip_len - 1] = '\0';
        }
        return wan_port;
    }

    return -1;
}