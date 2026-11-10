#include <stddef.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "server.h"
#include <netinet/tcp.h>
#include <errno.h>
#include "../shared/protocol.h"

#define VERSION "1.1.0"

static Client clients[CLIENT_COUNT] = {0};

static volatile size_t totalBytesSent = 0, totalBytesRecv = 0, totalMessagesRecv = 0, totalMessagesLost = 0;
static char *lostIDs = NULL; 
static FILE *file;
static pthread_mutex_t lostIDs_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int uart_connected = 0;
static time_t last_heartbeat = 0;

static void time_to_str(char *buffer, size_t size) {
    struct timespec ts;
    struct tm *t;

    clock_gettime(CLOCK_REALTIME, &ts);
    t = localtime(&ts.tv_sec);

    strftime(buffer, size, "%d/%m/%Y %H:%M:%S", t);

    size_t len = strlen(buffer);
    snprintf(buffer + len, size - len, ".%03ld", ts.tv_nsec / 1000000);
}

static void fprint_log(int sent, size_t ID, size_t Counter) {
    char timestamp[32];

    time_to_str(timestamp, sizeof(timestamp));

    fprintf(file, "[%s] %s ID %zu #%zu\n", timestamp, sent ? "SENT" : "READ", ID, Counter);

    fflush(file);
}

static void check_heartbeat(void) {
    if (last_heartbeat == 0 || time(NULL) - last_heartbeat >= UART_HEARTBEAT_TIMEOUT) {
        uart_connected = 0;
    }
}

static void disconnect_client(uint8_t id, int expected_fd) {
    pthread_mutex_lock(&clients_mutex);

    if (clients[id].pfd.fd != expected_fd) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    clients[id].pfd.fd = -1;
    clients[id].pfd.revents = 0;

    atomic_store(&clients[id].connected, 0);

    if (expected_fd >= 0) {
        close(expected_fd);
    }

    pthread_mutex_unlock(&clients_mutex);

    printf("Client %u disconnected\n", id);
}

static int connect_to_client(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int sock_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (sock_fd < 0) {
        perror("accept error");
        return 0;
    }

    uint8_t id;
    ssize_t received = read(sock_fd, &id, sizeof(id));

    if (received != sizeof(id) || id >= CLIENT_COUNT) {
        close(sock_fd);
        return 0;
    }

    pthread_mutex_lock(&clients_mutex);

    int old_fd = clients[id].pfd.fd;

    clients[id].ID = id;
    clients[id].addr = client_addr;
    clients[id].pfd.fd = sock_fd;
    clients[id].pfd.events = POLLIN;
    clients[id].pfd.revents = 0;
    atomic_store(&clients[id].connected, 1);

    if (old_fd >= 0) {
        close(old_fd);
    }

    pthread_mutex_unlock(&clients_mutex);

    int enabled = 1;
    int idle = 5;
    int interval = 3;
    int count = 5;
    unsigned int user_timeout = 3000;

    if (setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled))) {
        perror("keepalive error");
    }
    if (setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle))) {
        perror("keepidle error");
    }
    if (setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval))) {
        perror("keepintvl error");
    }
    if (setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count))) {
        perror("keepcnt error");
    }
    if (setsockopt(sock_fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout)) < 0) {
        perror("user timeout error");
    }
    printf("Client %u connected (fd: %d)\n", id, sock_fd);

    return 1;
}

static int send_message(void) {
    if (!atomic_load(&clients[0].connected) || !atomic_load(&clients[1].connected) || !uart_connected) return 0;

    static uint32_t message_id = 1;

    Message msg = {0};
    
    if (!NewMessage(&msg, rand() % (M_COUNT - 1) + 1, message_id++)) {
        perror("new message failed");
        return 0;
    }

    uint8_t buffer[MESSAGE_BUFFER_SIZE] = {0};

    if (!MessageToBytes(&msg, buffer, MESSAGE_BUFFER_SIZE)) {
        free(msg.Payload);
        return 0;
    }

    size_t n = MessageToSize(&msg);

    free(msg.Payload);

    pthread_mutex_lock(&clients_mutex);

    int client_fd = clients[0].pfd.fd;

    if (client_fd < 0 || !atomic_load(&clients[0].connected)) {
        pthread_mutex_unlock(&clients_mutex);
        return 0;
    }

    size_t total_sent = 0;
    int failed = 0;

    while (total_sent < n) {
        ssize_t sent = send(client_fd, buffer + total_sent, n - total_sent, MSG_NOSIGNAL);

        if (sent <= 0) {
            perror("send error");
            failed = 1;
            break;
        }

        total_sent += sent;
    }
    pthread_mutex_unlock(&clients_mutex);

    if (failed) {
        disconnect_client(0, client_fd);
        return 0;
    }

    totalBytesSent += total_sent;
    fprint_log(1, msg.Header.ID, msg.Header.Counter);

    return 1;
}
 
static int read_message(int server_fd) {
    static uint8_t buffers[CLIENT_COUNT][MESSAGE_BUFFER_SIZE] = {0};
    static size_t used[CLIENT_COUNT] = {0};
    static uint32_t lastReadCounter = 0;

    check_heartbeat();

    struct pollfd poll_fds[CLIENT_COUNT + 1] = {0};

    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < CLIENT_COUNT; i++) poll_fds[i] = clients[i].pfd;
    pthread_mutex_unlock(&clients_mutex);

    poll_fds[CLIENT_COUNT].fd = server_fd;
    poll_fds[CLIENT_COUNT].events = POLLIN;

    int result = poll(poll_fds, CLIENT_COUNT + 1, 100);

    if (result < 0) {
        if (errno == EINTR) return 1;

        perror("poll error");
        return 0;
    }

    if (poll_fds[CLIENT_COUNT].revents & POLLIN) {
        connect_to_client(server_fd);
    }

    for (size_t i = 0; i < CLIENT_COUNT; i++) {
        short events = poll_fds[i].revents;

        if (poll_fds[i].fd < 0) continue;

        if (events & POLLIN) {
            uint8_t temp[MESSAGE_BUFFER_SIZE * 2];

            ssize_t received = read(poll_fds[i].fd, temp, sizeof(temp));

            if (received <= 0) {
                if (received == 0) {
                    printf("Client %zu disconnected\n", i);
                } else {
                    perror("read error");
                }

                disconnect_client(i, poll_fds[i].fd);
                used[i] = 0;
                continue;
            }

            totalBytesRecv += received;

            for (ssize_t j = 0; j < received; j++) {
                uint8_t c = temp[j];

                if (used[i] >= MESSAGE_BUFFER_SIZE) {
                    fprintf(stderr, "receive buffer overflow: client %zu\n", i);
                    used[i] = 0;
                    break;
                }

                buffers[i][used[i]++] = c;

                if (used[i] < HEADER_SIZE) continue;

                if (buffers[i][0] >= M_COUNT) {
                    fprintf(stderr, "invalid id: client %zu\n", i);
                    used[i] = 0;
                    break;
                }

                size_t payload_size = (uint16_t)buffers[i][1] | ((uint16_t)buffers[i][2] << 8);

                size_t expected_size = HEADER_SIZE + payload_size;
                if (expected_size > MESSAGE_BUFFER_SIZE || expected_size < HEADER_SIZE) {
                    fprintf(stderr, "invalid message size: client %zu\n", i);
                    used[i] = 0;
                    break;
                }

                if (used[i] < expected_size) continue;

                Message msg = {0};
                if (!BytesToMessage(buffers[i], expected_size, &msg)) {
                    fprintf(stderr, "message parsing failed\n");
                    used[i] = 0;
                    break;
                }

                used[i] = 0;

                if (msg.Header.ID == M_0B) {
                    last_heartbeat = time(NULL);
                    uart_connected = 1;

                    free(msg.Payload);
                    continue;
                }

                char timestamp[32] = {0};
                time_to_str(timestamp, sizeof(timestamp));

                size_t expected_counter = lastReadCounter + 1;

                if (msg.Header.Counter > expected_counter) {
                    size_t missing = msg.Header.Counter - expected_counter;

                    totalMessagesLost += missing;

                    char log_buffer[128];

                    pthread_mutex_lock(&lostIDs_mutex);
                    int len = snprintf(
                        log_buffer,
                        sizeof(log_buffer),
                        "\n * IDs #%zu to #%u, at %s",
                        expected_counter,
                        msg.Header.Counter - 1,
                        timestamp
                    );

                    size_t oldLen = lostIDs ? strlen(lostIDs) : 0;

                    char *newPtr = realloc(lostIDs, oldLen + len + 1);
                    if (newPtr) {
                        lostIDs = newPtr;
                        memcpy(lostIDs + oldLen, log_buffer, len + 1);
                    }
                    pthread_mutex_unlock(&lostIDs_mutex);
                }

                lastReadCounter = msg.Header.Counter;
                totalMessagesRecv++;

                fprint_log(0, msg.Header.ID, msg.Header.Counter);

                free(msg.Payload);
            }
        }

        if (events & (POLLHUP | POLLERR | POLLNVAL)) {
            disconnect_client(i, poll_fds[i].fd);
            used[i] = 0;
        }
    }

    return 1;
}

static void *read_worker(void *arg) {
    ReadThreadArgs *args = (ReadThreadArgs *)arg;

    while (1) read_message(args->server_fd);
}

static void *print_worker(void *arg) {
    #define STATS_WINDOW_NS 1000000000ULL
    #define PRINT_INTERVAL_NS 100000000ULL

    #define HISTORY_SIZE 11

    struct {
        struct timespec time;
        size_t sent;
        size_t recv;
    } history[HISTORY_SIZE] = {0};

    size_t history_index = 0;

    while (1) {
        usleep(100000);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        size_t currentSent = totalBytesSent;
        size_t currentRecv = totalBytesRecv;

        history_index = (history_index + 1) % HISTORY_SIZE;

        history[history_index].time = now;
        history[history_index].sent = currentSent;
        history[history_index].recv = currentRecv;

        size_t oldest_index = (history_index + 1) % HISTORY_SIZE;

        struct timespec oldestTime = history[oldest_index].time;

        double elapsed = (now.tv_sec - oldestTime.tv_sec) + (now.tv_nsec - oldestTime.tv_nsec) / 1e9;

        double sentKbps = 0.0;
        double recvKbps = 0.0;

        if (elapsed >= 0.1) {
            sentKbps = (double)(currentSent - history[oldest_index].sent) * 8.0 / (elapsed * 1000.0);

            recvKbps = (double)(currentRecv - history[oldest_index].recv) * 8.0 / (elapsed * 1000.0);
        }

        double sentMB = (double)currentSent / 1000000.0;
        double recvMB = (double)currentRecv / 1000000.0;

        pthread_mutex_lock(&lostIDs_mutex);
        char *_lostIDs = lostIDs ? strdup(lostIDs) : NULL;
        pthread_mutex_unlock(&lostIDs_mutex);

        size_t totalExpectedMessages = totalMessagesRecv + totalMessagesLost;

        double successRate = totalExpectedMessages == 0 ? 0.0 : (double)totalMessagesRecv / totalExpectedMessages * 100.0;

        printf(
            "\033[2J\033[H"
            "LINUX SERVER %s (Success: %.2f%%)\n"
            "============\n"
            "TCP Sent:  %.2f MB | %.3f Kbps\n"
            "TCP Read:  %.2f MB | %.3f Kbps\n\n",
            VERSION,
            successRate,
            sentMB, sentKbps,
            recvMB, recvKbps
        );

        printf("TCP2UART Client Status: %s\n",
            atomic_load(&clients[0].connected) ? "CONNECTED" : "DISCONNECTED");

        printf("           |\n");
        printf("UART Connection Status: %s\n",
            uart_connected ? "CONNECTED" : "DISCONNECTED");

        printf("           |\n");
        printf("UART2TCP Client Status: %s\n",
            atomic_load(&clients[1].connected) ? "CONNECTED" : "DISCONNECTED");

        if (_lostIDs) {
            printf("\nLost Message IDs: %s", _lostIDs);
            free(_lostIDs);
        }

        fflush(stdout);
    }

    return NULL;
}

int main(int argc, char **argv) {
    srand((unsigned int)time(NULL));

    // init
    for (size_t i = 0; i < CLIENT_COUNT; i++) {
        clients[i].pfd.fd = -1;
        clients[i].pfd.events = POLLIN;
        atomic_store(&clients[i].connected, 0);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket error");
        return 1;
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt error");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr = htonl(INADDR_ANY),
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        perror("bind error");
        return 1;
    }

    if (listen(server_fd, CLIENT_COUNT)) {
        close(server_fd);
        perror("listen error");
        return 1;
    }

    file = fopen("output.txt", "w");
    if (file == NULL) {
        perror("file error");
        return 1;
    }

    printf("=== ready ===\n");

    // read thread
    ReadThreadArgs args = {
        .server_fd = server_fd,
    };

    pthread_t read_thread;
    pthread_create(&read_thread, NULL, read_worker, &args);

    // print
    pthread_t print_thread;
    pthread_create(&print_thread, NULL, print_worker, &args);

    sleep(2);

    while (1) {
        // send
        send_message();

        usleep(1);
    }

    fclose(file);
    close(server_fd);
    for (size_t i = 0; i < CLIENT_COUNT; i++) close(clients[i].pfd.fd);
    return 0;
}
