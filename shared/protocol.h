#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <inttypes.h>
#include <stddef.h>

#define HEADER_SIZE sizeof(Header)
#define MESSAGE_BUFFER_SIZE (500 + HEADER_SIZE)

typedef enum : uint8_t {
    M_0B,
    M_100B,
    M_200B,
    M_300B,
    M_400B,
    M_500B,

    M_COUNT,
} MessageID;

typedef struct __attribute__((packed)) {
    MessageID ID;
    uint16_t PayloadSize;
    uint32_t Counter;
} Header;

typedef struct Message {
    Header Header;
    uint8_t *Payload;
} Message;

int NewMessage(Message *msg, MessageID ID, uint32_t counter);
int MessageToBytes(const Message *msg, uint8_t *buffer, const uint16_t len);
int BytesToMessage(const uint8_t *buffer, const uint16_t len, Message *msg);
size_t MessageToSize(const Message *msg);

#endif