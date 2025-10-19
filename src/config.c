// config.c
#include "config.h"
#include <stdio.h>

void init_server_config(void)
{
    g_config.port = 3250;
    g_config.enable_nat = 0;
}

const struct server_config *get_server_config(void)
{
    return &g_config;
}

void set_server_port(int port)
{
    g_config.port = port;
}

void set_enable_nat(int enable)
{
    g_config.enable_nat = enable;
}

void set_max_rtp_buffer_size(int size)
{
    g_config.max_rtp_buffer_size = size;
}

void set_max_udp_packet_size(int size)
{
    g_config.max_udp_packet_size = size;
}