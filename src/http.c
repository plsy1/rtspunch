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
#include <signal.h>
#include <getopt.h>

#include "rtsp.h"
#include "rtp.h"
#include "rtcp.h"
#include "logs.h"
#include "config.h"


typedef struct
{
    int client_fd;
    struct sockaddr_in client_addr;
} client_info_t;

int create_listen_socket(int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, 5) < 0)
    {
        perror("listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int parse_http_url(const char *url, char *host, int *port, char *path)
{

    if (strncmp(url, "/rtp/", 5) == 0)
    {
        if (sscanf(url + 5, "%127[^:]:%d/%511[^\n]", host, port, path) == 3)
        {
            return 0;
        }
    }
    return -1;
}

void *handle_http_request(void *arg)
{
    char buf[4096];
    client_info_t *info = (client_info_t *)arg;
    int client_fd = info->client_fd;

    int n = recv(client_fd, buf, sizeof(buf) - 1, 0);

    if (n <= 0)
    {
        LOG_ERROR("Failed to receive HTTP request or client disconnected.");
        goto cleanup;
    }

    buf[n] = 0;

    char url[512], host[128], path[256];
    int port;

    if (sscanf(buf, "GET %511s HTTP/1.1", url) != 1)
    {
        LOG_ERROR("Failed to parse HTTP request URL");
        goto cleanup;
    }
    if (parse_http_url(url, host, &port, path) != 0)
    {
        LOG_ERROR("Failed to parse URL: %s", url);
        goto cleanup;
    }

    char rtsp_url[512];
    snprintf(rtsp_url, sizeof(rtsp_url), "rtsp://%s:%d/%s", host, port, path);

    LOG_INFO("New Client connect: %s:%d -> %s",
             inet_ntoa(info->client_addr.sin_addr),
             ntohs(info->client_addr.sin_port),
             rtsp_url);

    rtsp_play_stream(rtsp_url, client_fd);

    goto cleanup;

cleanup:
    LOG_INFO("Client disconnected: %s:%d -> %s", inet_ntoa(info->client_addr.sin_addr), ntohs(info->client_addr.sin_port), rtsp_url);
    close(client_fd);
    free(info);
    return NULL;
}

void start_http_server(const void *args)
{

    const struct server_config *config = (const struct server_config *)args;

    int server_sock = create_listen_socket(config->port);
    if (server_sock < 0)
    {
        LOG_ERROR("Failed to create HTTP server socket on port %d", config->port);
        return;
    }

    LOG_INFO("HTTP server listening on port %d", config->port);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0)
        {
            perror("accept");
            continue;
        }

        client_info_t *info = malloc(sizeof(client_info_t));
        if (!info)
        {
            LOG_ERROR("Failed to allocate memory for client_info_t");
            close(client_sock);
            continue;
        }

        info->client_fd = client_sock;
        info->client_addr = client_addr;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_http_request, info) != 0)
        {
            LOG_ERROR("Failed to create thread for client");
            close(client_sock);
            free(info);
            continue;
        }
        pthread_detach(thread_id);
    }
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    init_server_config();

    struct option long_options[] = {
        {"port", required_argument, NULL, 'p'},
        {"enable-nat", no_argument, NULL, 'n'},
        {"set-rtp-buffer-size", required_argument, NULL, 'r'},
        {"set-max-udp-packet-size", required_argument, NULL, 'u'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "p:nr:u:", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'p':
            set_server_port(atoi(optarg));
            break;
        case 'n':
            set_enable_nat(1);
            break;
        case 'r':
            set_max_rtp_buffer_size(atoi(optarg));
            break;
        case 'u':
            set_max_udp_packet_size(atoi(optarg));
            break;
        default:
            fprintf(stderr, "Usage: %s [-p port] [-n enable nat punch] [--rtp-buffer-size size] [--udp-packet-size size]\n", argv[0]);
            exit(EXIT_FAILURE);
            break;
        }
    }

    const struct server_config *config = get_server_config();

    start_http_server(config);

    return 0;
}