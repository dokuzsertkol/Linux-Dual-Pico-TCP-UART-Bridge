#include <cyw43_country.h>
#include <hardware/gpio.h>
#include <lwip/err.h>
#include <lwip/ip4_addr.h>
#include <pico/multicore.h>
#include <pico/platform/common.h>
#include <pico/stdlib.h>
#include <pico/cyw43_arch.h>
#include <lwip/tcp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "../shared/protocol.h"
#include "tcp2uart-client.h"

#define VERSION "1.1.0"

static volatile size_t totalBytesSentUart = 0, totalBytesRecvTcp = 0;
static char *lostIDs[5] = {0};
static mutex_t uart_mutex;

static struct tcp_pcb *pcb = NULL;
static volatile bool tcp_connected = false, reconnect_needed = true, heartbeat_waiting = false;

static void time_to_str(char *buffer, size_t size) {
    uint64_t us = time_us_64();

    uint64_t seconds = us / 1000000;
    uint64_t milliseconds = (us / 1000) % 1000;

    snprintf(buffer, size, "%llu.%03llu", (unsigned long long)seconds, (unsigned long long)milliseconds);
}

static void updateLostIDs(Message *msg, size_t bytes, size_t reason_type) {
    if (reason_type >= ERROR_TYPE_COUNT) return;

    size_t index = error_total[reason_type] % ERROR_LOG_CAPACITY;

    error_logs[reason_type][index] = (ErrorRecord) {
        .id = msg ? msg->Header.Counter : 0,
        .bytes = bytes,
        .timestamp = time_us_64(),
        .valid = true
    };

    error_total[reason_type]++;

    if (error_count[reason_type] < ERROR_LOG_CAPACITY) {
        error_count[reason_type]++;
    }
}

static void uart_heartbeat(void) {
    uint32_t last_heartbeat_ms = 0;

    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if ((uint32_t)(now - last_heartbeat_ms) >= UART_HEARTBEAT_INTERVAL_MS) {

            uint8_t uart_buffer[HEADER_SIZE];
            Message ping = {0};

            if (NewMessage(&ping, M_0B, 0)) {

                if (MessageToBytes(&ping, uart_buffer, HEADER_SIZE)) {
                    mutex_enter_blocking(&uart_mutex);
                    uart_write_blocking(UART_ID, uart_buffer, HEADER_SIZE);
                    mutex_exit(&uart_mutex);
                }

                free(ping.Payload);
            }

            last_heartbeat_ms = now;
        }

        tight_loop_contents();
    }
}

static int send_uart_message(Message *msg) {
    uint8_t uart_buffer[MESSAGE_BUFFER_SIZE];

    size_t n = MessageToSize(msg);

    if (!MessageToBytes(msg, uart_buffer, n)) {
        updateLostIDs(msg, n, 2);
        return 0;
    }
    
    mutex_enter_blocking(&uart_mutex);
    uart_write_blocking(UART_ID, uart_buffer, n);
    mutex_exit(&uart_mutex);

    totalBytesSentUart += n;

    return 1;
}

static void print_errors(void) {
    static const char *reasons[ERROR_TYPE_COUNT] = {
        "TCP message corrupted",
        "TCP TX queue full",
        "UART message corrupted",
        "UART RX corrupted",
        "TCP PBUF copy error",
        "TCP RX corrupted"
    };

    for (size_t i = 0; i < ERROR_TYPE_COUNT; i++) {
        if (error_total[i] == 0) continue;

        printf("\n%s (Total: %zu):\n",
            reasons[i], error_total[i]);

        size_t count = error_count[i];
        size_t start = error_total[i] >= ERROR_LOG_CAPACITY
            ? error_total[i] % ERROR_LOG_CAPACITY
            : 0;

        for (size_t j = 0; j < count; j++) {
            size_t index = (start + j) % ERROR_LOG_CAPACITY;
            ErrorRecord *record = &error_logs[i][index];

            if (!record->valid) continue;

            printf("  ");

            if (record->id != 0) {
                printf("Unique ID: %zu, ", record->id);
            }

            printf("%zu B at %llu.%03llu\n",
                record->bytes,
                (unsigned long long)(record->timestamp / 1000000),
                (unsigned long long)((record->timestamp / 1000) % 1000));
        }
    }
}

static void print_stats(void) {
    static size_t lastMsgSentUart = 0;
    static size_t lastMsgRecvTcp = 0;
    static uint32_t lastTime = 0;

    uint32_t currentTime = to_ms_since_boot(get_absolute_time());

    if (lastTime == 0) {
        lastTime = currentTime;
        return;
    }

    uint32_t elapsed = currentTime - lastTime;

    if (elapsed < 100) return;

    size_t sentPerInterval = totalBytesSentUart - lastMsgSentUart;
    size_t recvPerInterval = totalBytesRecvTcp - lastMsgRecvTcp;

    double tcpKB = (double)totalBytesRecvTcp / 1000.0;
    double uartKB = (double)totalBytesSentUart / 1000.0;

    double tcpKbps = (double)recvPerInterval * 8.0 / elapsed;
    double uartKbps = (double)sentPerInterval * 8.0 / elapsed;

    printf("\033[2J\033[H");
    printf("TCP2UART PICO CLIENT %s\n============\n", VERSION);

    printf("TCP Read:  %.2f MB | %.3f Kbps\n", tcpKB / 1000, tcpKbps);
    printf("UART Sent: %.2f MB | %.3f Kbps\n", uartKB / 1000, uartKbps);

    printf("TCP Status: %s\n", tcp_connected ? "CONNECTED" : "DISCONNECTED");

    print_errors();
    fflush(stdout);

    lastMsgSentUart = totalBytesSentUart;
    lastMsgRecvTcp = totalBytesRecvTcp;
    lastTime = currentTime;
}

static err_t recv_callback(void *arg, struct tcp_pcb *conn, struct pbuf *p, err_t err) {
    static size_t rx_used = 0;
    static uint8_t rx_buffer[MESSAGE_BUFFER_SIZE];
    
    print_stats();

    if (err != ERR_OK) {
        if (p) pbuf_free(p);

        pcb = NULL;
        tcp_connected = false;
        reconnect_needed = true;

        tcp_abort(conn);
        return ERR_ABRT;
    }

    if (!p) {
        err_t close_err = tcp_close(conn);

        pcb = NULL;
        tcp_connected = false;
        reconnect_needed = true;

        rx_used = 0;

        if (close_err != ERR_OK) {
            tcp_abort(conn);
            return ERR_ABRT;
        }

        return ERR_OK;
    }

    size_t offset = 0;
    while (offset < p->tot_len) {
        uint8_t c;

        u16_t copied = pbuf_copy_partial(p, &c, 1, offset++);
        if (copied != 1) {
            pbuf_free(p);
            tcp_abort(conn);
            //printf("pbuf copy error\n");
            
            updateLostIDs(NULL, copied, 4);

            return ERR_ABRT;
        }

        if (rx_used >= MESSAGE_BUFFER_SIZE) {
            pbuf_free(p);
            tcp_abort(conn);
            // printf("receive buffer overflow\n");
            
            updateLostIDs(NULL, rx_used, 5);
            rx_used = 0;
            
            return ERR_ABRT;
        }

        rx_buffer[rx_used++] = c;

        if (rx_used < HEADER_SIZE) continue;

        size_t payload_size = (uint16_t)rx_buffer[1] | ((uint16_t)rx_buffer[2] << 8);
        if (payload_size == 0) {
            pbuf_free(p);
            tcp_abort(conn);
            rx_used = 0;
            pcb = NULL;
            tcp_connected = false;
            reconnect_needed = true;
            return ERR_ABRT;
        }

        size_t expected_size = HEADER_SIZE + payload_size;
        if (rx_used < expected_size) continue;

        Message msg = {0};
        if(!BytesToMessage(rx_buffer, rx_used, &msg)) {
            updateLostIDs(NULL, rx_used, 5);
            
            pbuf_free(p);
            tcp_abort(conn);
            return ERR_ABRT;
        }

        totalBytesRecvTcp += rx_used;

        rx_used = 0;
        rx_buffer[0] = '\0';

        // send
        send_uart_message(&msg);
        free(msg.Payload);
    }

    tcp_recved(conn, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

static void error_callback(void *arg, err_t err) {
    pcb = NULL;
    tcp_connected = false;
    reconnect_needed = true;
}

static err_t connected_callback(void *arg, struct tcp_pcb *conn, err_t err) {
    if (err != ERR_OK) {
        printf("TCP connection failed: %d\n", err);

        pcb = NULL;
        tcp_connected = false;
        reconnect_needed = true;

        return err;
    }

    uint8_t id = 0;

    err_t result = tcp_write(conn, &id, sizeof(id), TCP_WRITE_FLAG_COPY);
    if (result != ERR_OK) {
        tcp_abort(conn);

        pcb = NULL;
        tcp_connected = false;
        reconnect_needed = true;

        return ERR_ABRT;
    }

    tcp_output(conn);

    tcp_recv(conn, recv_callback);

    ip_set_option(conn, SOF_KEEPALIVE);
    conn->keep_idle = 1000;
    conn->keep_intvl = 3000;
    conn->keep_cnt = 5;

    pcb = conn;
    tcp_connected = true;
    reconnect_needed = false;

    printf("TCP connected\n");

    return ERR_OK;
}

static int connect_to_server(void) {
    ip4_addr_t server_addr;

    if (!ip4addr_aton(SERVER_IP, &server_addr)) {
        printf("ip4addr error\n");
        return 0;
    }

    struct tcp_pcb *new_pcb = tcp_new();

    if (!new_pcb) {
        printf("pcb allocation failed\n");
        return 0;
    }

    tcp_err(new_pcb, error_callback);

    pcb = new_pcb;

    err_t err = tcp_connect(
        pcb,
        &server_addr,
        SERVER_PORT,
        connected_callback
    );

    if (err != ERR_OK) {
        printf("tcp_connect failed: %d\n", err);

        tcp_abort(pcb);
        pcb = NULL;

        return 0;
    }

    return 1;
}

int main() {
    stdio_init_all();

    printf("=== ready ===\n");

    // uart init
    uart_init(UART_ID, UART_BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(UART_ID, true);
    while (uart_is_readable(UART_ID)) uart_getc(UART_ID);
    mutex_init(&uart_mutex);
    
    // wifi init
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_TURKEY) != 0) {
        printf("init error\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    while (1) {
        int wifi_err = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 15000
        );

        if (wifi_err == 0) {
            printf("=== wifi connected ===\n");
            break;
        }
        else {
            printf("wifi connection failed: %d. trying again...\n", wifi_err);
            sleep_ms(1000);
        }
    }

    // uart heartbeat
    multicore_launch_core1(uart_heartbeat);

    static uint32_t next_retry_time = 0;
    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (reconnect_needed && (int32_t)(now - next_retry_time) >= 0) {

            reconnect_needed = false;

            cyw43_arch_lwip_begin();
            int result = connect_to_server();
            cyw43_arch_lwip_end();

            if (!result) {
                reconnect_needed = true;
                next_retry_time = now + 2000;
            }
        }

        print_stats();

        tight_loop_contents();
    }

    printf("=== end ===\n");

    cyw43_arch_deinit();
    return 0;
}