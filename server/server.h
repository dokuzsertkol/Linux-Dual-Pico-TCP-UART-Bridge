#ifndef SERVER_H
#define SERVER_H

#include <netinet/in.h>
#include <stdatomic.h>
#include <poll.h>

#define PORT 3000
#define CLIENT_COUNT 2

#define UART_HEARTBEAT_TIMEOUT 2

typedef struct {
    uint8_t ID;
    atomic_int connected;
    struct sockaddr_in addr;
    struct pollfd pfd;
} Client;

typedef struct {
    int server_fd;
} ReadThreadArgs;

#endif