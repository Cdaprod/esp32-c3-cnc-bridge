"""Regression check for dedicated GRBL jog-cancel path."""
from pathlib import Path

source = Path("src/espnow_handler.c").read_text()
motion = Path("src/mpg_motion.c").read_text()
rx = source.index("static void espnow_rx_task(void *pvParameters)\n{")
start = source.index("        case ESPNOW_MSG_MPG_STOP: {", rx)
end = source.index("        case ESPNOW_MSG_UART_COMMAND:", start)
handler = source[start:end]
assert "mpg_stop_uart()" in handler
assert "uart_write_bytes(" not in handler
assert "MPG_OUTPUT_SUPPRESSED" in handler
assert "MPG_OUTPUT_SENT" in handler
assert "mpg_motion_stop_ack_result(cancel_result)" in handler
assert "ESPNOW_RESULT_SUPPRESSED" in motion
assert 'event=mpg_stop result=suppressed physical_uart=not_sent' in handler
assert 'event=mpg_stop result=sent physical_uart=0x85' in handler
assert "\n        serial_send_data(" not in handler
assert "MPG_STOP_ACK" in handler
print("Dedicated MPG STOP is mode-gated and fails closed when suppressed")
