#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mpg_motion.h"

typedef struct {
    unsigned uart_calls;
    unsigned cancel_calls;
    uint8_t cancel_byte;
    unsigned log_calls;
    bool log_success;
    char logged[64];
} output_probe_t;

static int probe_uart(const char *command, size_t length, void *context)
{
    output_probe_t *probe = context;
    (void)command;
    probe->uart_calls++;
    return (int)length;
}

static int probe_cancel(uint8_t command, void *context)
{
    output_probe_t *probe = context;
    probe->cancel_calls++;
    probe->cancel_byte = command;
    return 1;
}

static bool probe_logger(const char *command, void *context)
{
    output_probe_t *probe = context;
    probe->log_calls++;
    snprintf(probe->logged, sizeof(probe->logged), "%s", command);
    return probe->log_success;
}

/* Host-buildable pure admission tests:
 * cc -std=c11 -Wall -Wextra -Werror -Iinclude src/mpg_motion.c tests/mpg_motion_test.c -o /tmp/mpg_motion_test && /tmp/mpg_motion_test
 */
int main(void)
{
    mpg_motion_lease_t lease = {0};
    const uint8_t a[6] = {1,2,3,4,5,6}, b[6] = {6,5,4,3,2,1};
    char cmd[64] = {0};
    assert(mpg_motion_format_jog(cmd, sizeof(cmd), 'X', -10, 250));
    assert(strcmp(cmd, "$J=G91 G21 X-0.010 F250\n") == 0);
    assert(mpg_motion_format_jog(cmd, sizeof(cmd), 'A', 10, 250));
    assert(strcmp(cmd, "$J=G91 G21 A0.010 F250\n") == 0);
    assert(!mpg_motion_format_jog(cmd, sizeof(cmd), 'Q', 10, 250));
    assert(!mpg_motion_format_jog(cmd, sizeof(cmd), 'B', 10, 250));
    assert(!mpg_motion_format_jog(cmd, sizeof(cmd), 'X', 1001, 250));
    assert(!mpg_motion_format_jog(cmd, sizeof(cmd), 'X', 10, 0));
    assert(mpg_motion_admit(&lease, a, 123, 65535, true, true, true, 1000, 250) == MPG_MOTION_OK);
    assert(mpg_motion_admit(&lease, a, 123, 65535, true, true, true, 1001, 250) == MPG_MOTION_REJECT);
    assert(mpg_motion_admit(&lease, a, 123, 0, true, true, true, 1002, 250) == MPG_MOTION_OK);
    assert(mpg_motion_admit(&lease, a, 123, 32768, true, true, true, 1003, 250) == MPG_MOTION_REJECT);
    assert(mpg_motion_admit(&lease, b, 123, 1, true, true, true, 1004, 250) == MPG_MOTION_REJECT);
    assert(mpg_motion_expired(&lease, 1252));
    assert(mpg_motion_admit(&lease, b, 999, 1, true, true, true, 1252, 250) == MPG_MOTION_EXPIRED);
    assert(!lease.active);
    assert(mpg_motion_admit(&lease, b, 999, 1, true, true, true, 1253, 250) == MPG_MOTION_OK);
    mpg_motion_revoke(&lease);
    assert(!lease.active);

    /* Session-bound wrap-safe sequence guard rejects replay after lease revoke. */
    mpg_motion_sequence_t sequence = {0};
    uint8_t duplicate_result = 0;
    mpg_motion_sequence_reset(&sequence, a, 123, 65534);
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 123, 65535,
                                      &duplicate_result) == MPG_SEQUENCE_NEW);
    mpg_motion_sequence_result(&sequence, 123, 65535, 1);
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 123, 65535,
                                      &duplicate_result) == MPG_SEQUENCE_DUPLICATE);
    assert(duplicate_result == 1);
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 123, 65534,
                                      &duplicate_result) == MPG_SEQUENCE_REJECT_STALE);
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 999, 0,
                                      &duplicate_result) == MPG_SEQUENCE_REJECT_SESSION);
    assert(mpg_motion_sequence_accept(&sequence, b, 123, 123, 0,
                                      &duplicate_result) == MPG_SEQUENCE_REJECT_SESSION);
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 123, 0,
                                      &duplicate_result) == MPG_SEQUENCE_NEW);
    mpg_motion_sequence_result(&sequence, 123, 0, 0);
    /* A replay after STOP/revoke remains a duplicate, never a fresh jog. */
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 123, 0,
                                      &duplicate_result) == MPG_SEQUENCE_DUPLICATE);
    assert(duplicate_result == 0);

    /* A C3 that re-pairs with a long-running S3 seeds from the PAIR packet's
     * sequence. A receiver reset to zero would misclassify this fresh value
     * as stale once the S3 counter had advanced beyond half the uint16 range.
     */
    mpg_motion_sequence_reset(&sequence, a, 123, 50000);
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 123, 50002,
                                      &duplicate_result) == MPG_SEQUENCE_NEW);
    mpg_motion_sequence_result(&sequence, 123, 50002, 1);
    assert(mpg_motion_sequence_accept(&sequence, a, 123, 123, 50002,
                                      &duplicate_result) == MPG_SEQUENCE_DUPLICATE);
    assert(duplicate_result == 1);

    /* Dry-run output is physically isolated, including logger error paths. */
    output_probe_t probe = { .log_success = true };
    assert(mpg_motion_output_command(cmd, true, probe_uart, probe_logger,
                                     &probe) == MPG_OUTPUT_DRY_RUN);
    assert(probe.uart_calls == 0 && probe.log_calls == 1);
    assert(strcmp(probe.logged, cmd) == 0);
    probe.log_success = false;
    assert(mpg_motion_output_command(cmd, true, probe_uart, probe_logger,
                                     &probe) == MPG_OUTPUT_FAILED);
    assert(probe.uart_calls == 0 && probe.log_calls == 2);
    assert(mpg_motion_output_command(cmd, true, probe_uart, NULL,
                                     &probe) == MPG_OUTPUT_FAILED);
    assert(probe.uart_calls == 0);
    assert(mpg_motion_output_command(cmd, false, probe_uart, probe_logger,
                                     &probe) == MPG_OUTPUT_SENT);
    assert(probe.uart_calls == 1);

    /* Disarmed/dry-run leases must never inject realtime bytes into GRBL. */
    assert(mpg_motion_output_cancel(false, probe_cancel, &probe) ==
           MPG_OUTPUT_SUPPRESSED);
    assert(probe.cancel_calls == 0);
    assert(mpg_motion_output_cancel(true, probe_cancel, &probe) ==
           MPG_OUTPUT_SENT);
    assert(probe.cancel_calls == 1 && probe.cancel_byte == 0x85);
    assert(mpg_motion_output_cancel(true, NULL, &probe) ==
           MPG_OUTPUT_FAILED);
    assert(probe.cancel_calls == 1);
    puts("mpg_motion tests passed");
    return 0;
}
