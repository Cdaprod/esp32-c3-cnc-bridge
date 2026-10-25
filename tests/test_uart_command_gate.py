"""Static security regression: legacy v1 UART_COMMAND must never raw-forward.

This checks the intended fail-closed transition until the dedicated MPG
receiver path is implemented, reviewed and tested with actual GRBL hardware.
"""
from pathlib import Path

source = Path("src/espnow_handler.c").read_text()
start = source.index("        case ESPNOW_MSG_UART_COMMAND: {", source.index("static void espnow_rx_task(void *pvParameters)\n{"))
end = source.index("        case ESPNOW_MSG_HEARTBEAT:", start)
handler = source[start:end]
assert "serial_send_data(" not in handler, "unsafe raw UART transmit bypass restored"
assert "ESPNOW_RESULT_ERROR" in handler, "UART command must reject by default"
legacy_start = source.index("static void espnow_legacy_rx_task(void *pvParameters)\n{")
legacy_end = source.index("static bool parse_legacy_command(", legacy_start)
legacy_handler = source[legacy_start:legacy_end]
assert "serial_send_data(" not in legacy_handler, "legacy ESP-NOW raw UART bypass restored"
print("v1 and legacy UART_COMMAND fail-closed regressions passed")
