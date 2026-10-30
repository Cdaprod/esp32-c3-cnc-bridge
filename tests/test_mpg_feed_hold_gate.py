"""Keep feed hold distinct and fail closed through the C3 output gate."""
from pathlib import Path

source = Path("src/espnow_handler.c").read_text()
start = source.index("        case ESPNOW_MSG_MPG_FEED_HOLD: {")
end = source.index("        case ESPNOW_MSG_UART_COMMAND:", start)
handler = source[start:end]

assert "peer_accepts_mpg_sequence(&packet" in handler
assert "mpg_trusted_ready" in handler and "mpg_trusted_peer" in handler
assert "tcp_server_get_client_count() != 0" in handler
assert "serial_uart_tx_idle()" in handler
assert 'mpg_motion_output_command("!", true' in handler
assert 'mpg_motion_output_command("!", false' in handler
assert "MPG_ENABLE_MOTION && !MPG_DRY_RUN" in handler
assert "ESPNOW_RESULT_SUPPRESSED" in handler
assert "MPG_FEED_HOLD_ACK" in handler
assert "physical_uart=not_sent" in source
assert "mpg_rearm_wait_idle = true" in source
assert "post_stop_idle_required" in handler or "post_stop_idle_required" in source
assert any(f"{timestamp} - mpg_rearm_after_ms) >= 250" in source
           for timestamp in ("line_time_ms", "now_ms"))

physical = handler.index("#if MPG_ENABLE_MOTION && !MPG_DRY_RUN")
dry_run = handler.index("#elif MPG_DRY_RUN", physical)
uart_path = handler[physical:dry_run]
assert "serial_uart_mpg_claim()" in uart_path
assert "mpg_uart_writer" in uart_path
assert "serial_uart_mpg_release()" in uart_path
print("Feed hold uses trusted sequenced input and the shared fail-closed UART gate")
