"""Static security regressions for legacy UART rejection and MPG ownership."""
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

serial = Path("src/serial_handler.c").read_text()
tx_start = serial.index("void serial_send_data(")
tx_end = serial.index("void serial_send_string(", tx_start)
tx_path = serial[tx_start:tx_end]
assert "mpg_uart_arbiter_reserve_normal" in tx_path
assert "mpg_uart_arbiter_complete_normal" in tx_path
assert "if (!reserved)" in tx_path, "normal TX must fail closed while MPG owns UART"
writer_start = source.index("static int mpg_uart_writer(")
writer_end = source.index("static bool mpg_dry_run_logger(", writer_start)
assert "serial_uart_mpg_owned()" in source[writer_start:writer_end], \
       "physical jog writer must require the MPG UART lease"
print("v1 and legacy UART_COMMAND fail-closed regressions passed")
