#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "mpg_grbl_ack.h"

int main(void)
{
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t other[6] = {6, 5, 4, 3, 2, 1};
    mpg_grbl_ack_t pending = {0};
    mpg_grbl_ack_t completed = {0};
    uint8_t error_code = 99;

    assert(mpg_grbl_ack_begin(&pending, mac, 123, 65535, 1000, 150));
    assert(!mpg_grbl_ack_begin(&pending, mac, 123, 0, 1000, 150));
    assert(mpg_grbl_ack_matches(&pending, mac, 123, 65535));
    assert(!mpg_grbl_ack_matches(&pending, other, 123, 65535));
    assert(!mpg_grbl_ack_matches(&pending, mac, 999, 65535));
    assert(!mpg_grbl_ack_matches(&pending, mac, 123, 0));

    /* Status reports and unrelated text cannot complete a jog request. */
    assert(mpg_grbl_ack_consume_line(&pending, "<Jog|MPos:0,0,0>", 16,
        1001, &completed, &error_code) == MPG_GRBL_ACK_NONE);
    assert(mpg_grbl_ack_consume_line(&pending, "ok extra", 8,
        1001, &completed, &error_code) == MPG_GRBL_ACK_NONE);
    assert(mpg_grbl_ack_consume_line(&pending, "error:x", 7,
        1001, &completed, &error_code) == MPG_GRBL_ACK_NONE);
    assert(pending.active && error_code == 0);
    assert(mpg_grbl_ack_consume_line(&pending, " ok \t", 5,
        1001, &completed, &error_code) == MPG_GRBL_ACK_ACCEPTED);
    assert(!pending.active && completed.active);
    assert(completed.sequence == 65535 && completed.session == 123);

    assert(mpg_grbl_ack_begin(&pending, mac, 123, 0, 2000, 150));
    assert(mpg_grbl_ack_consume_line(&pending, "error:33", 8,
        2010, &completed, &error_code) == MPG_GRBL_ACK_REJECTED);
    assert(error_code == 33 && completed.sequence == 0);

    assert(mpg_grbl_ack_begin(&pending, mac, 123, 7, 3000, 150));
    assert(mpg_grbl_ack_consume_line(&pending, "error:256", 9,
        3010, &completed, &error_code) == MPG_GRBL_ACK_NONE);
    assert(pending.active);
    assert(mpg_grbl_ack_consume_line(&pending, "error:7", 7,
        3150, &completed, &error_code) == MPG_GRBL_ACK_TIMED_OUT);
    assert(!pending.active && completed.sequence == 7);

    assert(mpg_grbl_ack_begin(&pending, mac, 123, 1,
        UINT32_MAX - 50u, 150));
    assert(mpg_grbl_ack_expire(&pending, 98, &completed) == MPG_GRBL_ACK_NONE);
    assert(mpg_grbl_ack_expire(&pending, 99, &completed) ==
           MPG_GRBL_ACK_TIMED_OUT);
    assert(!pending.active && completed.sequence == 1);

    assert(mpg_grbl_ack_begin(&pending, mac, 123, 2, 3000, 150));
    assert(mpg_grbl_ack_cancel(&pending, &completed) == MPG_GRBL_ACK_CANCELLED);
    assert(!pending.active && completed.sequence == 2);

    assert(!mpg_grbl_ack_begin(&pending, mac, 0, 1, 0, 150));
    assert(!mpg_grbl_ack_begin(&pending, mac, 123, 1, 0, 0));
    assert(!mpg_grbl_ack_begin(&pending, mac, 123, 1, 0, 501));
    return 0;
}
