#include "mpg_grbl_ack.h"
#include <string.h>

static mpg_grbl_ack_result_t complete(mpg_grbl_ack_t *pending,
    mpg_grbl_ack_result_t result, mpg_grbl_ack_t *completed)
{
    if (completed) *completed = *pending;
    memset(pending, 0, sizeof(*pending));
    return result;
}

bool mpg_grbl_ack_begin(mpg_grbl_ack_t *pending, const uint8_t mac[6],
    uint32_t session, uint16_t sequence, uint32_t now_ms,
    uint32_t timeout_ms)
{
    if (!pending || !mac || pending->active || session == 0 ||
        timeout_ms == 0 || timeout_ms > 500)
        return false;
    memcpy(pending->mac, mac, sizeof(pending->mac));
    pending->session = session;
    pending->sequence = sequence;
    pending->deadline_ms = now_ms + timeout_ms;
    pending->active = true;
    return true;
}

bool mpg_grbl_ack_matches(const mpg_grbl_ack_t *pending,
    const uint8_t mac[6], uint32_t session, uint16_t sequence)
{
    return pending && pending->active && mac &&
        pending->session == session && pending->sequence == sequence &&
        memcmp(pending->mac, mac, sizeof(pending->mac)) == 0;
}

mpg_grbl_ack_result_t mpg_grbl_ack_consume_line(mpg_grbl_ack_t *pending,
    const char *line, size_t length, uint32_t now_ms,
    mpg_grbl_ack_t *completed, uint8_t *error_code)
{
    if (error_code) *error_code = 0;
    if (!pending || !pending->active || !line) return MPG_GRBL_ACK_NONE;
    if ((int32_t)(now_ms - pending->deadline_ms) >= 0)
        return complete(pending, MPG_GRBL_ACK_TIMED_OUT, completed);

    while (length && (*line == ' ' || *line == '\t')) {
        line++;
        length--;
    }
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t'))
        length--;

    if (length == 2 && memcmp(line, "ok", 2) == 0)
        return complete(pending, MPG_GRBL_ACK_ACCEPTED, completed);

    if (length < 7 || memcmp(line, "error:", 6) != 0 || length > 9)
        return MPG_GRBL_ACK_NONE;
    unsigned code = 0;
    for (size_t i = 6; i < length; i++) {
        if (line[i] < '0' || line[i] > '9') return MPG_GRBL_ACK_NONE;
        code = code * 10u + (unsigned)(line[i] - '0');
    }
    if (code == 0 || code > 255) return MPG_GRBL_ACK_NONE;
    if (error_code) *error_code = (uint8_t)code;
    return complete(pending, MPG_GRBL_ACK_REJECTED, completed);
}

mpg_grbl_ack_result_t mpg_grbl_ack_expire(mpg_grbl_ack_t *pending,
    uint32_t now_ms, mpg_grbl_ack_t *completed)
{
    if (!pending || !pending->active ||
        (int32_t)(now_ms - pending->deadline_ms) < 0)
        return MPG_GRBL_ACK_NONE;
    return complete(pending, MPG_GRBL_ACK_TIMED_OUT, completed);
}

mpg_grbl_ack_result_t mpg_grbl_ack_cancel(mpg_grbl_ack_t *pending,
    mpg_grbl_ack_t *completed)
{
    if (!pending || !pending->active) return MPG_GRBL_ACK_NONE;
    return complete(pending, MPG_GRBL_ACK_CANCELLED, completed);
}
