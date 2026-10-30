#include "mpg_uart_arbiter.h"

bool mpg_uart_arbiter_reserve_normal(mpg_uart_arbiter_t *state)
{
    if (!state || state->mpg_owned || state->normal_tx_pending == UINT32_MAX)
        return false;
    state->normal_tx_pending++;
    return true;
}

void mpg_uart_arbiter_complete_normal(mpg_uart_arbiter_t *state)
{
    if (state && state->normal_tx_pending)
        state->normal_tx_pending--;
}

bool mpg_uart_arbiter_claim_mpg(mpg_uart_arbiter_t *state)
{
    if (!state) return false;
    if (state->mpg_owned) return true;
    if (state->normal_tx_pending) return false;
    state->mpg_owned = true;
    return true;
}

void mpg_uart_arbiter_release_mpg(mpg_uart_arbiter_t *state)
{
    if (state) state->mpg_owned = false;
}

bool mpg_uart_arbiter_mpg_owned(const mpg_uart_arbiter_t *state)
{
    return state && state->mpg_owned;
}

bool mpg_uart_arbiter_normal_idle(const mpg_uart_arbiter_t *state)
{
    return state && !state->normal_tx_pending;
}
