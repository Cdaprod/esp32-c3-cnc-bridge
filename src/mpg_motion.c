#include "mpg_motion.h"
#include <stdio.h>
#include <string.h>

bool mpg_motion_expired(const mpg_motion_lease_t *lease, uint32_t now_ms)
{
    return lease && lease->active &&
        (int32_t)(now_ms - lease->expires_ms) >= 0;
}

void mpg_motion_revoke(mpg_motion_lease_t *lease)
{
    if (lease) memset(lease, 0, sizeof(*lease));
}

void mpg_motion_sequence_reset(mpg_motion_sequence_t *state,
    const uint8_t owner[6], uint32_t session, uint16_t initial_sequence)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    if (!owner || session == 0) return;
    memcpy(state->owner, owner, sizeof(state->owner));
    state->session = session;
    state->sequence = initial_sequence;
    state->last_result = 0;
    state->valid = true;
}

mpg_sequence_result_t mpg_motion_sequence_accept(mpg_motion_sequence_t *state,
    const uint8_t owner[6], uint32_t active_session,
    uint32_t packet_session, uint16_t sequence, uint8_t *duplicate_result)
{
    if (duplicate_result) *duplicate_result = 0;
    if (!state || !owner || active_session == 0 ||
        packet_session != active_session)
        return MPG_SEQUENCE_REJECT_SESSION;
    if (!state->valid) {
        mpg_motion_sequence_reset(state, owner, active_session,
                                  (uint16_t)(sequence - 1));
    }
    if (state->session != active_session ||
        memcmp(state->owner, owner, sizeof(state->owner)) != 0)
        return MPG_SEQUENCE_REJECT_SESSION;

    const uint16_t delta = (uint16_t)(sequence - state->sequence);
    if (delta == 0) {
        if (duplicate_result) *duplicate_result = state->last_result;
        return MPG_SEQUENCE_DUPLICATE;
    }
    if (delta >= 0x8000u) return MPG_SEQUENCE_REJECT_STALE;

    state->sequence = sequence;
    state->last_result = 0; /* A duplicate before completion fails closed. */
    return MPG_SEQUENCE_NEW;
}

void mpg_motion_sequence_result(mpg_motion_sequence_t *state,
    uint32_t session, uint16_t sequence, uint8_t result)
{
    if (state && state->valid && state->session == session &&
        state->sequence == sequence)
        state->last_result = result;
}

mpg_motion_result_t mpg_motion_admit(mpg_motion_lease_t *lease,
    const uint8_t owner[6], uint32_t session, uint16_t sequence,
    bool authorized, bool deadman, bool grbl_idle,
    uint32_t now_ms, uint32_t lifetime_ms)
{
    if (!lease || !owner || !authorized || !deadman || !grbl_idle ||
        session == 0 || lifetime_ms == 0 || lifetime_ms > 500)
        return MPG_MOTION_REJECT;
    if (mpg_motion_expired(lease, now_ms)) {
        mpg_motion_revoke(lease);
        return MPG_MOTION_EXPIRED; /* caller MUST cancel physical jogging */
    }
    if (lease->active) {
        const uint16_t delta = (uint16_t)(sequence - lease->sequence);
        if (memcmp(lease->owner, owner, 6) != 0 ||
            lease->session != session || delta == 0 || delta >= 32768)
            return MPG_MOTION_REJECT;
    }
    memcpy(lease->owner, owner, 6);
    lease->session = session;
    lease->sequence = sequence;
    lease->expires_ms = now_ms + lifetime_ms;
    lease->active = true;
    return MPG_MOTION_OK;
}

bool mpg_motion_format_jog(char *output, size_t capacity,
    char axis, int32_t distance_microns, uint16_t feed_mm_min)
{
    if (!output || capacity < 48 ||
        !(axis == 'X' || axis == 'Y' || axis == 'Z' || axis == 'A') ||
        distance_microns == 0 || distance_microns < -1000 ||
        distance_microns > 1000 ||
        feed_mm_min == 0 || feed_mm_min > 1000)
        return false;
    /* Integer formatting avoids floating point locale, NaN and injection. */
    int32_t abs_um = distance_microns < 0 ? -distance_microns : distance_microns;
    int n = snprintf(output, capacity, "$J=G91 G21 %c%s%ld.%03ld F%u\n",
        axis, distance_microns < 0 ? "-" : "",
        (long)(abs_um / 1000), (long)(abs_um % 1000),
        (unsigned)feed_mm_min);
    return n > 0 && (size_t)n < capacity;
}

bool mpg_motion_axis_allowed(char axis, bool physical_motion,
                             bool allow_a_axis_motion)
{
    if (axis == 'X' || axis == 'Y' || axis == 'Z') return true;
    if (axis == 'A') return !physical_motion || allow_a_axis_motion;
    return false;
}

mpg_output_result_t mpg_motion_output_command(const char *command,
    bool dry_run, mpg_uart_writer_t uart_writer,
    mpg_dry_run_logger_t dry_run_logger, void *context)
{
    if (!command || command[0] == '\0') return MPG_OUTPUT_FAILED;
    const size_t length = strlen(command);
    if (dry_run) {
        if (!dry_run_logger || !dry_run_logger(command, context))
            return MPG_OUTPUT_FAILED;
        return MPG_OUTPUT_DRY_RUN;
    }
    if (!uart_writer || uart_writer(command, length, context) != (int)length)
        return MPG_OUTPUT_FAILED;
    return MPG_OUTPUT_SENT;
}

mpg_output_result_t mpg_motion_output_cancel(bool allow_physical_output,
    mpg_realtime_writer_t writer, void *context)
{
    if (!allow_physical_output) return MPG_OUTPUT_SUPPRESSED;
    if (!writer || writer(0x85, context) != 1) return MPG_OUTPUT_FAILED;
    return MPG_OUTPUT_SENT;
}
