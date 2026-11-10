#include <cyw43_country.h>
#include <hardware/uart.h>
#include <lwip/ip4_addr.h>
#include <pico/platform/common.h>
#include <pico/stdlib.h>
#include <pico/cyw43_arch.h>
#include <lwip/tcp.h>
#include <stddef.h>
#include <stdio.h>
#include "hardware/dma.h"
#include "uart2tcp-client.h"

#define VERSION "1.1.0"

_Alignas(UART_RX_BUFFER_SIZE) static volatile uint8_t uart_rx_buffer[UART_RX_BUFFER_SIZE];
static int uart_rx_dma_channel;
static volatile size_t totalBytesSentTcp = 0, totalBytesRecvUart = 0;
static char *lostIDs[5] = {0};

static struct tcp_pcb *pcb = NULL;
static volatile bool tcp_connected = false, reconnect_needed = true;

static int enqueue_tcp(const uint8_t *data, size_t len) {
    if (tx_count >= TCP_TX_QUEUE_CAPACITY) return 0;

    TcpQueueItem *item = &tcp_tx_queue[tx_tail];

    memcpy(item->data, data, len);
    item->len = len;

    tx_tail = (tx_tail + 1) % TCP_TX_QUEUE_CAPACITY;
    tx_count++;

    return 1;
}

static void flush_tcp_queue(struct tcp_pcb *conn) {
    if (!conn || !tcp_connected) return;
    while (tx_count > 0) {
        TcpQueueItem *item = &tcp_tx_queue[tx_head];

        err_t err = tcp_write(conn, item->data, item->len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) break;

        if (err != ERR_OK) {
            // printf("tcp_write error: %d\n", err);
            break;
        }

        tx_head = (tx_head + 1) % TCP_TX_QUEUE_CAPACITY;
        tx_count--;
    }

    tcp_output(conn);
}

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

static int send_tcp_message(Message *msg) {
    uint8_t tx_buffer[MESSAGE_BUFFER_SIZE];

    size_t n = MessageToSize(msg);
    if (!MessageToBytes(msg, tx_buffer, n)) {
        if (pcb) {
            tcp_abort(pcb);
        }

        pcb = NULL;
        tcp_connected = false;
        reconnect_needed = true;

        updateLostIDs(msg, n, 0);

        return 0;
    }

    if (!enqueue_tcp(tx_buffer, n) && msg->Header.ID != M_0B) {
        updateLostIDs(msg, MessageToSize(msg), 1);
        return 0;
    }
    
    flush_tcp_queue(pcb);

    totalBytesSentTcp += n;

    return 1;
} 

static void read_uart(void) {
    static uint32_t uart_rx_consumed = 0;
    static uint8_t buffer[MESSAGE_BUFFER_SIZE] = {0};
    static size_t message_used = 0;

    uint32_t remaining = dma_channel_hw_addr(uart_rx_dma_channel)->transfer_count;

    uint32_t produced = (UART_RX_BUFFER_SIZE - remaining) & (UART_RX_BUFFER_SIZE - 1);

    while (uart_rx_consumed != produced) {
        uint8_t c = uart_rx_buffer[uart_rx_consumed];

        uart_rx_consumed = (uart_rx_consumed + 1) & (UART_RX_BUFFER_SIZE - 1);

        if (message_used >= sizeof(buffer)) {
            updateLostIDs(NULL, message_used, 3);
            message_used = 0;
            continue;
        }

        buffer[message_used++] = c;

        if (message_used < HEADER_SIZE) continue;

        size_t payload_size = (uint16_t)buffer[1] | ((uint16_t)buffer[2] << 8);

        size_t expected_size = HEADER_SIZE + payload_size;

        if (expected_size > sizeof(buffer)) {
            updateLostIDs(NULL, message_used, 3);
            message_used = 0;
            continue;
        }

        if (message_used < expected_size) continue;

        Message msg = {0};
        if (expected_size == HEADER_SIZE) {
            if (buffer[0] == M_0B) {
                if (NewMessage(&msg, M_0B, 0)) {
                    cyw43_arch_lwip_begin();
                    int result = send_tcp_message(&msg);
                    cyw43_arch_lwip_end();

                    if (!result) {
                        printf("Heartbeat TCP enqueue failed\n");
                    }
                }
            }
        }
        else {
            if (BytesToMessage(buffer, expected_size, &msg)) {
                cyw43_arch_lwip_begin();

                send_tcp_message(&msg);

                cyw43_arch_lwip_end();

                totalBytesRecvUart += expected_size;

                free(msg.Payload);
            }
            else {
                updateLostIDs(NULL, message_used, 3);
            }
        }

        message_used = 0;
    }
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
    static size_t lastMsgRecvUart = 0;
    static size_t lastMsgSentTcp = 0;
    static uint32_t lastTime = 0;

    uint32_t currentTime = to_ms_since_boot(get_absolute_time());

    if (lastTime == 0) {
        lastTime = currentTime;
        return;
    }

    uint32_t elapsed = currentTime - lastTime;

    if (elapsed < 100) return;

    size_t recvPerInterval = totalBytesRecvUart - lastMsgRecvUart;
    size_t sentPerInterval = totalBytesSentTcp - lastMsgSentTcp;

    double tcpKB = (double)totalBytesSentTcp / 1000.0;
    double uartKB = (double)totalBytesRecvUart / 1000.0;

    double tcpKbps = (double)sentPerInterval * 8.0 / elapsed;
    double uartKbps = (double)recvPerInterval * 8.0 / elapsed;

    printf("\033[2J\033[H\rUART2TCP PICO CLIENT %s\n============\n", VERSION);

    printf("UART Read: %.2f MB | %.3f Kbps\n", uartKB / 1000, uartKbps);
    printf("TCP Sent:  %.2f MB | %.3f Kbps\n", tcpKB / 1000, tcpKbps);

    printf("TCP Status: %s\n", tcp_connected ? "CONNECTED" : "DISCONNECTED");
    
    print_errors();
    fflush(stdout);

    lastMsgRecvUart = totalBytesRecvUart;
    lastMsgSentTcp = totalBytesSentTcp;
    lastTime = currentTime;
}

static err_t sent_callback(void *arg, struct tcp_pcb *conn, u16_t len) {
    flush_tcp_queue(conn);

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

    uint8_t id = 1;

    err_t result = tcp_write(conn, &id, sizeof(id), TCP_WRITE_FLAG_COPY);
    if (result != ERR_OK) {
        tcp_abort(conn);

        pcb = NULL;
        tcp_connected = false;
        reconnect_needed = true;

        return ERR_ABRT;
    }

    tcp_output(conn);
    
    tcp_sent(conn, sent_callback);

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

    err_t err = tcp_connect(pcb, &server_addr, SERVER_PORT, connected_callback);
    if (err != ERR_OK) {
        printf("tcp_connect failed: %d\n", err);

        tcp_abort(pcb);
        pcb = NULL;

        return 0;
    }

    return 1;
}

static void dma_handler(void) {
    if (dma_channel_get_irq0_status(uart_rx_dma_channel)) {
        dma_channel_acknowledge_irq0(uart_rx_dma_channel);

        dma_channel_set_write_addr(uart_rx_dma_channel, uart_rx_buffer, false);
        dma_channel_set_trans_count(uart_rx_dma_channel, UART_RX_BUFFER_SIZE, true);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== ready ===\n");

    // uart init
    uart_init(UART_ID, UART_BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(UART_ID, true);
    while (uart_is_readable(UART_ID)) uart_getc(UART_ID);
    
    // dma init
    uart_rx_dma_channel = dma_claim_unused_channel(true);
    
    dma_channel_config config = dma_channel_get_default_config(uart_rx_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_ring(&config, true, 13);
    channel_config_set_dreq(&config, uart_get_dreq(UART_ID, false));

    dma_channel_set_irq0_enabled(uart_rx_dma_channel, true);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_configure(uart_rx_dma_channel, &config,
        (void *)uart_rx_buffer, &uart_get_hw(UART_ID)->dr,
        UART_RX_BUFFER_SIZE, true
    );

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
    
    static uint32_t next_retry_time = 0;
    while (1) {
        read_uart();
        
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