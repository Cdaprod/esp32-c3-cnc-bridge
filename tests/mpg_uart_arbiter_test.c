#include <assert.h>
#include "mpg_uart_arbiter.h"

int main(void)
{
    mpg_uart_arbiter_t state = {0};
    assert(mpg_uart_arbiter_normal_idle(&state));
    assert(mpg_uart_arbiter_reserve_normal(&state));
    assert(!mpg_uart_arbiter_normal_idle(&state));
    assert(!mpg_uart_arbiter_claim_mpg(&state));

    /* Queue completion is the boundary at which the pendant may claim GRBL. */
    mpg_uart_arbiter_complete_normal(&state);
    assert(mpg_uart_arbiter_claim_mpg(&state));
    assert(mpg_uart_arbiter_mpg_owned(&state));
    assert(!mpg_uart_arbiter_reserve_normal(&state));
    assert(mpg_uart_arbiter_claim_mpg(&state)); /* same MPG lease */

    mpg_uart_arbiter_release_mpg(&state);
    assert(!mpg_uart_arbiter_mpg_owned(&state));
    assert(mpg_uart_arbiter_reserve_normal(&state));
    assert(mpg_uart_arbiter_reserve_normal(&state));
    assert(!mpg_uart_arbiter_normal_idle(&state));
    mpg_uart_arbiter_complete_normal(&state);
    mpg_uart_arbiter_complete_normal(&state);
    assert(mpg_uart_arbiter_normal_idle(&state));
    return 0;
}
