#include "mpg_grbl_status.h"
#include <string.h>

bool mpg_grbl_status_parse(mpg_grbl_status_t *out, const char *line,
                           size_t length, uint32_t now_ms)
{
    if (!out || !line || length < 7 || length >= sizeof(out->line) ||
        line[0] != '<' || line[length - 1] != '>')
        return false;
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126)
            return false;
    }
    const char *state = line + 1;
    const char *separator = memchr(state, '|', length - 2);
    if (!separator) return false;
    size_t state_len = (size_t)(separator - state);
    const char *substate = memchr(state, ':', state_len);
    size_t base_len = substate ? (size_t)(substate - state) : state_len;
    const bool idle = base_len == 4 && memcmp(state, "Idle", 4) == 0;
    const bool alarm = base_len == 5 && memcmp(state, "Alarm", 5) == 0;
    const bool hold = base_len == 4 && memcmp(state, "Hold", 4) == 0;
    const bool door = base_len == 4 && memcmp(state, "Door", 4) == 0;
    const bool known = idle || alarm ||
                 (base_len == 3 && memcmp(state, "Jog", 3) == 0) ||
                 (base_len == 3 && memcmp(state, "Run", 3) == 0) ||
                 hold ||
                 (base_len == 5 && memcmp(state, "Check", 5) == 0) ||
                 door ||
                 (base_len == 4 && memcmp(state, "Home", 4) == 0) ||
                 (base_len == 5 && memcmp(state, "Sleep", 5) == 0);
    if (!known) return false;
    /* GRBL v1.1 reports Hold:0/1 and Door:0..3 substates. Preserve the
     * complete state text, but validate the suffix before refreshing status. */
    if (substate) {
        if (substate + 2 != state + state_len || substate[1] < '0' ||
            substate[1] > '9')
            return false;
        const char code = substate[1];
        if ((hold && code > '1') || (door && code > '3') ||
            (!hold && !door))
            return false;
    }
    memcpy(out->line, line, length);
    out->line[length] = '\0';
    out->seen = true;
    out->idle = idle;
    out->alarm = alarm;
    out->updated_ms = now_ms;
    return true;
}

bool mpg_grbl_status_fresh_idle(const mpg_grbl_status_t *status,
                                uint32_t now_ms, uint32_t max_age_ms)
{
    return status && status->seen && status->idle &&
           (uint32_t)(now_ms - status->updated_ms) <= max_age_ms &&
           max_age_ms <= 1000;
}
