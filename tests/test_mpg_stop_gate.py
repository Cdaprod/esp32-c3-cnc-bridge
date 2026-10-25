"""Regression check for dedicated GRBL jog-cancel path."""
from pathlib import Path

source = Path("src/espnow_handler.c").read_text()
rx = source.index("static void espnow_rx_task(void *pvParameters)\n{")
start = source.index("        case ESPNOW_MSG_MPG_STOP: {", rx)
end = source.index("        case ESPNOW_MSG_UART_COMMAND:", start)
handler = source[start:end]
assert "jog_cancel = 0x85" in handler
assert "uart_write_bytes(" in handler
assert "\n        serial_send_data(" not in handler
assert "MPG_STOP_ACK" in handler
print("Dedicated MPG STOP uses GRBL realtime jog cancel")
