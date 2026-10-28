#ifndef MPG_UART_ARBITER_H
#define MPG_UART_ARBITER_H

#include <stdbool.h>
#include <stdint.h>

/* The caller serializes access (the firmware uses one short FreeRTOS
 * critical section). Reservations span enqueue through physical TX finish. */
typedef struct {
    uint32_t normal_tx_pending;
    bool mpg_owned;
} mpg_uart_arbiter_t;

bool mpg_uart_arbiter_reserve_normal(mpg_uart_arbiter_t *state);
void mpg_uart_arbiter_complete_normal(mpg_uart_arbiter_t *state);
bool mpg_uart_arbiter_claim_mpg(mpg_uart_arbiter_t *state);
void mpg_uart_arbiter_release_mpg(mpg_uart_arbiter_t *state);
bool mpg_uart_arbiter_mpg_owned(const mpg_uart_arbiter_t *state);
bool mpg_uart_arbiter_normal_idle(const mpg_uart_arbiter_t *state);

#endif
