#ifndef RTCP_H
#define RTCP_H
#include <stdint.h>
#include <netinet/in.h>

int rtcp_open(int client_port);
int rtcp_send_rr(int sockfd, struct sockaddr_in *server, uint32_t ssrc);
void rtcp_close(int sockfd);

#endif