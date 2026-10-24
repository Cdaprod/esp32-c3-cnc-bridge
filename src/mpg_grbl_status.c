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
    bool idle = state_len == 4 && memcmp(state, "Idle", 4) == 0;
    bool alarm = state_len == 5 && memcmp(state, "Alarm", 5) == 0;
    bool known = idle || alarm ||
                 (state_len == 3 && memcmp(state, "Jog", 3) == 0) ||
                 (state_len == 3 && memcmp(state, "Run", 3) == 0) ||
                 (state_len == 4 && memcmp(state, "Hold", 4) == 0) ||
                 (state_len == 5 && memcmp(state, "Check", 5) == 0) ||
                 (state_len == 4 && memcmp(state, "Door", 4) == 0) ||
                 (state_len == 4 && memcmp(state, "Home", 4) == 0) ||
                 (state_len == 5 && memcmp(state, "Sleep", 5) == 0);
    if (!known) return false;
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
