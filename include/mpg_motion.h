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

/* Absolute receiver monotonic time, in milliseconds, with wrap-safe expiry.
 * All motion payload values are decoded separately by the receiver. */
mpg_motion_result_t mpg_motion_admit(mpg_motion_lease_t *lease,
    const uint8_t owner[6], uint32_t session, uint16_t sequence,
    bool authorized, bool deadman, bool grbl_idle,
    uint32_t now_ms, uint32_t lifetime_ms);
bool mpg_motion_expired(const mpg_motion_lease_t *lease, uint32_t now_ms);
void mpg_motion_revoke(mpg_motion_lease_t *lease);

/* Strict input to GRBL $J parser; no embedded newline or arbitrary G-code. */
bool mpg_motion_format_jog(char *output, size_t capacity,
    char axis, int32_t distance_microns, uint16_t feed_mm_min);
#endif
