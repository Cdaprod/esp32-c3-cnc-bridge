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
        !(axis == 'X' || axis == 'Y' || axis == 'Z') ||
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
