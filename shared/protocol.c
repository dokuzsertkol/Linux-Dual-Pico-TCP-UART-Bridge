
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "protocol.h"

int NewMessage(Message *msg, MessageID id, uint32_t counter) {
    if (!msg || id >= M_COUNT) return 0;

    uint16_t payload_size = (uint16_t)id * 100;

    uint8_t *payload = NULL;
    if (payload_size > 0) {
        payload = malloc(payload_size);
        if (!payload) return 0;

        memset(payload, 'X', payload_size);
    }

    *msg = (Message) {
        .Header = {
            .ID = id,
            .PayloadSize = payload_size,
            .Counter = counter,
        },
        .Payload = payload,
    };

    return 1;
}

int MessageToBytes(const Message *msg, uint8_t *buffer, const uint16_t len) {
    if (!msg || !buffer || msg->Header.ID >= M_COUNT) return 0;

    uint16_t payload_size = msg->Header.PayloadSize;

    if (payload_size != (uint16_t)msg->Header.ID * 100) return 0;
    if (len < HEADER_SIZE + payload_size) return 0;
    if (payload_size > 0 && !msg->Payload) return 0;

    buffer[0] = msg->Header.ID;
    buffer[1] = (uint8_t)payload_size;
    buffer[2] = (uint8_t)(payload_size >> 8);

    uint32_t counter = msg->Header.Counter;

    buffer[3] = (uint8_t)counter;
    buffer[4] = (uint8_t)(counter >> 8);
    buffer[5] = (uint8_t)(counter >> 16);
    buffer[6] = (uint8_t)(counter >> 24);

    if (payload_size > 0) memcpy(buffer + HEADER_SIZE, msg->Payload, payload_size);

    return 1;
}

int BytesToMessage(const uint8_t *buffer, const uint16_t len, Message *msg) {
    if (!msg || !buffer || len < HEADER_SIZE || buffer[0] >= M_COUNT) return 0;

    uint16_t payload_size = (uint16_t)buffer[1] | ((uint16_t)buffer[2] << 8);

    uint16_t expected_size = (uint16_t)buffer[0] * 100;

    if (payload_size != expected_size) return 0;
    if (len < HEADER_SIZE + payload_size) return 0;

    uint32_t counter = (uint32_t)buffer[3] | ((uint32_t)buffer[4] << 8) 
        | ((uint32_t)buffer[5] << 16) | ((uint32_t)buffer[6] << 24);

    uint8_t *payload = NULL;
    if (payload_size > 0) {
        payload = malloc(payload_size);

        if (!payload) return 0;

        memcpy(payload, buffer + HEADER_SIZE, payload_size);
    }

    *msg = (Message) {
        .Header = {
            .ID = buffer[0],
            .PayloadSize = payload_size,
            .Counter = counter,
        },
        .Payload = payload,
    };

    return 1;
}

size_t MessageToSize(const Message *msg) {
    if (!msg || msg->Header.ID >= M_COUNT || msg->Header.PayloadSize != (uint16_t)msg->Header.ID * 100
        || msg->Header.PayloadSize > 0 && !msg->Payload) {
        return 0;
    }

    return HEADER_SIZE + msg->Header.PayloadSize;
}
