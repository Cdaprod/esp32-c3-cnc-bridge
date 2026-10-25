#ifndef MPG_GRBL_STATUS_H
#define MPG_GRBL_STATUS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Conservative GRBL 1.1 telemetry cache; non-status lines never authorize jog. */
typedef struct {
    uint32_t updated_ms;
    bool seen;
    bool idle;
    bool alarm;
    char line[128];
} mpg_grbl_status_t;

/* Accept a single complete '<...>' status line; no dynamic allocations. */
bool mpg_grbl_status_parse(mpg_grbl_status_t *out, const char *line,
                           size_t length, uint32_t now_ms);
bool mpg_grbl_status_fresh_idle(const mpg_grbl_status_t *status,
                                uint32_t now_ms, uint32_t max_age_ms);
#endif
