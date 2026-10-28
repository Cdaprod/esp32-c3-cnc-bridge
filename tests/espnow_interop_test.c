#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "espnow_decode.h"
#include "mpg_motion.h"

typedef struct { unsigned uart_calls; unsigned log_calls; char command[64]; } probe_t;

static int uart_sink(const char *command, size_t length, void *context)
{
    (void)command;
    probe_t *probe = context;
    probe->uart_calls++;
    return (int)length;
}

static bool dry_run_sink(const char *command, void *context)
{
    probe_t *probe = context;
    probe->log_calls++;
    snprintf(probe->command, sizeof(probe->command), "%s", command);
    return true;
}

int main(void)
{
    /* Golden S3 vector from tests/test_c3_mpg_wire.py. */
    const uint8_t frame[] = {
        0x01,0x30,0x78,0x56,0x34,0x12,0x00,0x01,0x08,0x00,
        0x58,0x01,0xf6,0xff,0xff,0xff,0xfa,0x00
    };
    const uint8_t peer[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
    espnow_decoded_packet_t packet;
    assert(espnow_decode_frame(frame, sizeof(frame), peer, &packet));
    assert(packet.type == ESPNOW_MSG_MPG_JOG);
    assert(packet.session_id == 0x12345678 && packet.seq == 0x0100);
    assert(packet.payload_len == 8 && memcmp(packet.payload, frame + 10, 8) == 0);
    assert(packet.payload[0] == 'X' && packet.payload[1] == 1);

    const uint32_t microns_bits = (uint32_t)packet.payload[2] |
        ((uint32_t)packet.payload[3] << 8) |
        ((uint32_t)packet.payload[4] << 16) |
        ((uint32_t)packet.payload[5] << 24);
    char command[64];
    assert(mpg_motion_format_jog(command, sizeof(command),
        (char)packet.payload[0], (int32_t)microns_bits,
        (uint16_t)packet.payload[6] | ((uint16_t)packet.payload[7] << 8)));
    assert(strcmp(command, "$J=G91 G21 X-0.010 F250\n") == 0);
    /* Existing v1 frame shape carries the manufacturer's fourth/A selector
     * as an ASCII axis byte; no protocol version or payload change is needed. */
    const uint8_t a_frame[] = {
        0x01,0x30,0x78,0x56,0x34,0x12,0x01,0x01,0x08,0x00,
        0x41,0x01,0x0a,0x00,0x00,0x00,0xfa,0x00
    };
    espnow_decoded_packet_t fourth_axis_packet;
    assert(espnow_decode_frame(a_frame, sizeof(a_frame), peer,
                               &fourth_axis_packet));
    assert(fourth_axis_packet.payload[0] == 'A');
    assert(mpg_motion_format_jog(command, sizeof(command),
        (char)fourth_axis_packet.payload[0], 10, 250));
    assert(strcmp(command, "$J=G91 G21 A0.010 F250\n") == 0);

    /* Exercise the same trust/deadman/fresh-status/session-sequence gate used
     * by the receiver before the production dry-run output seam. */
    mpg_motion_sequence_t sequence = {0};
    uint8_t duplicate_result = 0;
    mpg_motion_sequence_reset(&sequence, packet.mac, packet.session_id,
                              (uint16_t)(packet.seq - 1));
    assert(mpg_motion_sequence_accept(&sequence, packet.mac, packet.session_id,
        packet.session_id, packet.seq, &duplicate_result) == MPG_SEQUENCE_NEW);
    mpg_motion_lease_t lease = {0};
    assert(mpg_motion_admit(&lease, packet.mac, packet.session_id, packet.seq,
        true, packet.payload[1] == 1, true, 1000, 250) == MPG_MOTION_OK);

    probe_t probe = {0};
    assert(mpg_motion_output_command(command, true, uart_sink, dry_run_sink,
        &probe) == MPG_OUTPUT_DRY_RUN);
    assert(probe.uart_calls == 0 && probe.log_calls == 1);
    assert(strcmp(probe.command, command) == 0);
    mpg_motion_sequence_result(&sequence, packet.session_id, packet.seq, 1);
    assert(mpg_motion_sequence_accept(&sequence, packet.mac, packet.session_id,
        packet.session_id, packet.seq, &duplicate_result) == MPG_SEQUENCE_DUPLICATE);
    assert(duplicate_result == 1);
    assert(mpg_motion_sequence_accept(&sequence, packet.mac, packet.session_id,
        packet.session_id + 1, (uint16_t)(packet.seq + 1),
        &duplicate_result) == MPG_SEQUENCE_REJECT_SESSION);

    /* Replaying that packet fails admission and has no output path to invoke. */
    assert(mpg_motion_admit(&lease, packet.mac, packet.session_id, packet.seq,
        true, packet.payload[1] == 1, true, 1010, 250) == MPG_MOTION_REJECT);
    mpg_motion_lease_t denied_lease = {0};
    assert(mpg_motion_admit(&denied_lease, packet.mac, packet.session_id,
        packet.seq, true, packet.payload[1] == 1, false, 1010, 250) == MPG_MOTION_REJECT);
    assert(mpg_motion_admit(&denied_lease, packet.mac, packet.session_id,
        packet.seq, false, packet.payload[1] == 1, true, 1010, 250) == MPG_MOTION_REJECT);
    assert(probe.uart_calls == 0 && probe.log_calls == 1);

    uint8_t malformed[sizeof(frame)];
    memcpy(malformed, frame, sizeof(frame));
    malformed[0]++;
    assert(!espnow_decode_frame(malformed, sizeof(malformed), peer, &packet));
    memcpy(malformed, frame, sizeof(frame));
    malformed[8] = 7; /* MPG_JOG length must remain exactly eight. */
    assert(!espnow_decode_frame(malformed, sizeof(malformed) - 1, peer, &packet));
    memcpy(malformed, frame, sizeof(frame));
    assert(!espnow_decode_frame(frame, sizeof(frame) - 1, peer, &packet));

    puts("S3 MPG v1 vector decoded and translated through isolated dry-run output");
    return 0;
}
