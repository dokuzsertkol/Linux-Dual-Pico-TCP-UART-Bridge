#ifndef TCP2UART_CLIENT_H
#define TCP2UART_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define WIFI_SSID "PicoNet"
#define WIFI_PASSWORD "PicoTest123"
#define SERVER_IP "10.42.0.1"
#define SERVER_PORT 3000

#define UART_ID uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define UART_BAUD_RATE 1000000

#define UART_HEARTBEAT_INTERVAL_MS 1000
#define UART_HEARTBEAT_TIMEOUT_MS 500

#define ERROR_TYPE_COUNT 6
#define ERROR_LOG_CAPACITY 4

typedef struct {
    size_t id;
    size_t bytes;
    uint64_t timestamp;
    int valid;
} ErrorRecord;

static ErrorRecord error_logs[ERROR_TYPE_COUNT][ERROR_LOG_CAPACITY];

static size_t error_count[ERROR_TYPE_COUNT] = {0};
static size_t error_total[ERROR_TYPE_COUNT] = {0};

#endif