"""Keep the C3 MPG replay window anchored to the current S3 session."""
from pathlib import Path

source = Path("src/espnow_handler.c").read_text()
pair_start = source.index("if (new_packet.type == ESPNOW_MSG_PAIR)")
pair_end = source.index("// PAIR_ACK", pair_start)
pair_handler = source[pair_start:pair_end]
assert "reset_new_sequence(mac, new_packet.session_id, new_packet.seq)" in pair_handler, \
    "MPG replay baseline must follow the sequence in the current PAIR frame"

reset_start = source.index("static void reset_new_sequence(")
reset_end = source.index("static bool legacy_sequence_is_duplicate", reset_start)
reset = source[reset_start:reset_end]
assert "session_id, initial_sequence" in reset
print("MPG replay baseline follows the active pairing sequence")
