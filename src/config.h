#ifndef CONFIG_H
#define CONFIG_H

#define MAX_RTP_BUFFER_SIZE 8192
#define MAX_UDP_PACKET_SIZE 1536
#define MAX_CONNECTIONS 3

struct server_config
{
    int port;
    int enable_nat;
    int max_rtp_buffer_size;
    int max_udp_packet_size;
};

static struct server_config g_config = {0, 0, 8192, 1536};

void init_server_config(void);

const struct server_config *get_server_config(void);

void set_server_port(int port);

void set_enable_nat(int enable);

void set_max_rtp_buffer_size(int size);
void set_max_udp_packet_size(int size);

#endif