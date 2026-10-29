#include "espnow_decode.h"
#include <string.h>

static bool valid_type_length(uint8_t type, uint16_t payload_len)
{
    switch (type) {
    case ESPNOW_MSG_PAIR: return payload_len >= 6 && payload_len <= 32;
    case ESPNOW_MSG_PAIR_ACK: return payload_len == ESPNOW_PAIR_ACK_PAYLOAD_LEN;
    case ESPNOW_MSG_HEARTBEAT:
    case ESPNOW_MSG_HEARTBEAT_ACK:
    case ESPNOW_MSG_GPIO_STATUS_REQ: return payload_len == 0;
    case ESPNOW_MSG_GPIO_COMMAND: return payload_len == 2;
    case ESPNOW_MSG_GPIO_ACK: return payload_len == 3;
    case ESPNOW_MSG_GPIO_STATUS: return payload_len == 4;
    case ESPNOW_MSG_SENSOR_ASSIGNMENT: return payload_len == 1;
    case ESPNOW_MSG_MPG_JOG: return payload_len == 8;
    case ESPNOW_MSG_MPG_STOP: return payload_len == 0;
    case ESPNOW_MSG_MPG_FEED_HOLD: return payload_len == 0;
    case ESPNOW_MSG_MPG_JOG_ACK:
    case ESPNOW_MSG_MPG_STOP_ACK:
    case ESPNOW_MSG_MPG_FEED_HOLD_ACK:
    case ESPNOW_MSG_UART_ACK: return payload_len == 1;
    case ESPNOW_MSG_UART_COMMAND:
    case ESPNOW_MSG_UART_RESPONSE:
    case ESPNOW_MSG_ERROR: return payload_len > 0;
    default: return false;
    }
}

bool espnow_decode_frame(const uint8_t *data, int data_len,
                         const uint8_t mac[6],
                         espnow_decoded_packet_t *out)
{
    if (!data || !mac || !out || data_len < ESPNOW_HEADER_SIZE ||
        data_len > ESPNOW_MAX_FRAME_SIZE ||
        data[0] != ESPNOW_PROTOCOL_VERSION)
        return false;

    const uint16_t payload_len = (uint16_t)data[8] |
        ((uint16_t)data[9] << 8);
    if (payload_len > ESPNOW_MAX_PAYLOAD ||
        payload_len != (uint16_t)(data_len - ESPNOW_HEADER_SIZE) ||
        !valid_type_length(data[1], payload_len))
        return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->mac, mac, sizeof(out->mac));
    out->type = data[1];
    out->session_id = (uint32_t)data[2] |
        ((uint32_t)data[3] << 8) |
        ((uint32_t)data[4] << 16) |
        ((uint32_t)data[5] << 24);
    out->seq = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    out->payload_len = payload_len;
    if (payload_len)
        memcpy(out->payload, data + ESPNOW_HEADER_SIZE, payload_len);
    return true;
}
