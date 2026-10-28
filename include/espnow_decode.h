#ifndef ESPNOW_DECODE_H
#define ESPNOW_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include "espnow_protocol.h"

typedef struct {
    uint8_t mac[6];
    uint8_t type;
    uint32_t session_id;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t payload[ESPNOW_MAX_PAYLOAD];
} espnow_decoded_packet_t;

/* Pure protocol-v1 decoder shared by the receiver and host interoperability tests. */
bool espnow_decode_frame(const uint8_t *data, int data_len,
                         const uint8_t mac[6],
                         espnow_decoded_packet_t *out);

#endif
