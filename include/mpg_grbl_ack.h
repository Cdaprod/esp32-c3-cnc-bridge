#ifndef MPG_GRBL_ACK_H
#define MPG_GRBL_ACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MPG_GRBL_ACK_TIMEOUT_MS 150u

typedef struct {
    bool active;
    uint8_t mac[6];
    uint32_t session;
    uint16_t sequence;
    uint32_t deadline_ms;
} mpg_grbl_ack_t;

typedef enum {
    MPG_GRBL_ACK_NONE = 0,
    MPG_GRBL_ACK_ACCEPTED = 1,
    MPG_GRBL_ACK_REJECTED = 2,
    MPG_GRBL_ACK_TIMED_OUT = 3,
    MPG_GRBL_ACK_CANCELLED = 4,
} mpg_grbl_ack_result_t;

bool mpg_grbl_ack_begin(mpg_grbl_ack_t *pending, const uint8_t mac[6],
    uint32_t session, uint16_t sequence, uint32_t now_ms,
    uint32_t timeout_ms);
bool mpg_grbl_ack_matches(const mpg_grbl_ack_t *pending,
    const uint8_t mac[6], uint32_t session, uint16_t sequence);
mpg_grbl_ack_result_t mpg_grbl_ack_consume_line(mpg_grbl_ack_t *pending,
    const char *line, size_t length, uint32_t now_ms,
    mpg_grbl_ack_t *completed, uint8_t *error_code);
mpg_grbl_ack_result_t mpg_grbl_ack_expire(mpg_grbl_ack_t *pending,
    uint32_t now_ms, mpg_grbl_ack_t *completed);
mpg_grbl_ack_result_t mpg_grbl_ack_cancel(mpg_grbl_ack_t *pending,
    mpg_grbl_ack_t *completed);

#endif
