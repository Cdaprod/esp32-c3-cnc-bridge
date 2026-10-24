#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mpg_motion.h"

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
    assert(!mpg_motion_format_jog(cmd, sizeof(cmd), 'Q', 10, 250));
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
    puts("mpg_motion tests passed");
    return 0;
}
