#ifndef MPG_MOTION_H
#define MPG_MOTION_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Pure, unit-testable admission state. A paired radio is NOT an authorized
 * machine controller. Explicit operator authorization must be added before
 * calling mpg_motion_admit(). */
typedef struct {
    uint8_t owner[6];
    uint32_t session;
    uint16_t sequence;
    uint32_t expires_ms;
    bool active;
} mpg_motion_lease_t;

typedef enum {
    MPG_MOTION_OK = 1,
    MPG_MOTION_REJECT = 0,
    MPG_MOTION_EXPIRED = -1
} mpg_motion_result_t;

typedef struct {
    uint8_t owner[6];
    uint32_t session;
    uint16_t sequence;
    uint8_t last_result;
    bool valid;
} mpg_motion_sequence_t;

typedef enum {
    MPG_SEQUENCE_NEW = 1,
    MPG_SEQUENCE_DUPLICATE = 0,
    MPG_SEQUENCE_REJECT_SESSION = -1,
    MPG_SEQUENCE_REJECT_STALE = -2
} mpg_sequence_result_t;

typedef int (*mpg_uart_writer_t)(const char *command, size_t length,
                                 void *context);
typedef int (*mpg_realtime_writer_t)(uint8_t command, void *context);
typedef bool (*mpg_dry_run_logger_t)(const char *command, void *context);

typedef enum {
    MPG_OUTPUT_SENT = 1,
    MPG_OUTPUT_DRY_RUN = 2,
    MPG_OUTPUT_SUPPRESSED = 3,
    MPG_OUTPUT_FAILED = 0
} mpg_output_result_t;

/* Absolute receiver monotonic time, in milliseconds, with wrap-safe expiry.
 * All motion payload values are decoded separately by the receiver. */
mpg_motion_result_t mpg_motion_admit(mpg_motion_lease_t *lease,
    const uint8_t owner[6], uint32_t session, uint16_t sequence,
    bool authorized, bool deadman, bool grbl_idle,
    uint32_t now_ms, uint32_t lifetime_ms);
bool mpg_motion_expired(const mpg_motion_lease_t *lease, uint32_t now_ms);
void mpg_motion_revoke(mpg_motion_lease_t *lease);

/* Bind accepted MPG sequence numbers to the current paired MAC/session. */
void mpg_motion_sequence_reset(mpg_motion_sequence_t *state,
    const uint8_t owner[6], uint32_t session, uint16_t initial_sequence);
mpg_sequence_result_t mpg_motion_sequence_accept(mpg_motion_sequence_t *state,
    const uint8_t owner[6], uint32_t active_session,
    uint32_t packet_session, uint16_t sequence, uint8_t *duplicate_result);
void mpg_motion_sequence_result(mpg_motion_sequence_t *state,
    uint32_t session, uint16_t sequence, uint8_t result);

/* Strict input to GRBL $J parser; no embedded newline or arbitrary G-code. */
bool mpg_motion_format_jog(char *output, size_t capacity,
    char axis, int32_t distance_microns, uint16_t feed_mm_min);
/* Keep OEM-specific A-axis jogs out of physical UART mode until verified. */
bool mpg_motion_axis_allowed(char axis, bool physical_motion,
                             bool allow_a_axis_motion);

/* Dry-run never falls back to the UART writer when its logger is absent/fails. */
mpg_output_result_t mpg_motion_output_command(const char *command,
    bool dry_run, mpg_uart_writer_t uart_writer,
    mpg_dry_run_logger_t dry_run_logger, void *context);
/* Realtime jog-cancel output is also suppressed unless physical motion is
 * explicitly enabled; diagnostic builds must not affect another UART owner. */
mpg_output_result_t mpg_motion_output_cancel(bool allow_physical_output,
    mpg_realtime_writer_t writer, void *context);
/* Map physical cancel outcome to the one-byte MPG_STOP_ACK result. */
uint8_t mpg_motion_stop_ack_result(mpg_output_result_t result);
#endif
