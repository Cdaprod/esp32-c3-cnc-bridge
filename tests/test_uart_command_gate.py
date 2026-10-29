"""Static security regressions for legacy UART rejection and MPG ownership."""
from pathlib import Path

source = Path("src/espnow_handler.c").read_text()
assert "#define MPG_DRY_RUN 0" in source, \
       "missing dry-run configuration must compile the MPG path out"
assert "#define MPG_DRY_RUN 1" not in source, \
       "missing configuration must not silently enable MPG diagnostics"
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
assert "event=uart_tx_rejected" in tx_path
writer_start = source.index("static int mpg_uart_writer(")
writer_end = source.index("static bool mpg_dry_run_logger(", writer_start)
assert "serial_uart_mpg_owned()" in source[writer_start:writer_end], \
       "physical jog writer must require the MPG UART lease"
cancel_start = source.index("static mpg_output_result_t mpg_stop_uart(")
cancel_end = source.index("\n}\n", cancel_start) + 3
cancel_path = source[cancel_start:cancel_end]
assert "MPG_ENABLE_MOTION && !MPG_DRY_RUN" in cancel_path, \
       "realtime jog cancel must be physically gated by motion-enabled mode"
assert "mpg_motion_output_cancel" in cancel_path
assert "uart_write_bytes" not in cancel_path, \
       "cancel must pass through the mode gate before UART output"

jog_start = source.index("        case ESPNOW_MSG_MPG_JOG: {")
jog_end = source.index("        case ESPNOW_MSG_MPG_STOP:", jog_start)
jog_path = source[jog_start:jog_end]
preprocessor_stack = []
for line in jog_path.splitlines():
    stripped = line.strip()
    if "mpg_motion_output_command(cmd, MPG_DRY_RUN" in line:
        assert "#if MPG_ENABLE_MOTION || MPG_DRY_RUN" in preprocessor_stack, \
            "MPG jog output must be compiled out when motion and dry-run are off"
    if stripped.startswith("#if "):
        preprocessor_stack.append(stripped)
    elif stripped.startswith("#endif"):
        assert preprocessor_stack, "unbalanced MPG jog preprocessor guard"
        preprocessor_stack.pop()
assert not preprocessor_stack, "unbalanced MPG jog preprocessor guard"

watchdog_start = source.index("static void mpg_watchdog_task(")
watchdog_end = source.index("\n}\n", watchdog_start) + 3
watchdog = source[watchdog_start:watchdog_end]
assert "tcp_server_get_client_count() > 0" in watchdog
lock_take = watchdog.index("xSemaphoreTake(mpg_mutex")
revoke = watchdog.index("mpg_motion_revoke(&mpg_lease)")
cancel = watchdog.index("mpg_stop_uart()")
release = watchdog.index("serial_uart_mpg_release()")
lock_give = watchdog.index("xSemaphoreGive(mpg_mutex)")
assert lock_take < revoke < cancel < release < lock_give, \
       "TCP takeover must cancel and relinquish MPG ownership under one mutex"
assert "bool lease_ended = false;" in watchdog
assert "if (lease_ended)" in watchdog
assert watchdog.index("if (lease_ended)") < watchdog.index(
    "if (cancel_result == MPG_OUTPUT_SENT)"), \
    "watchdog cancellation logs must be emitted only when a lease ends"
print("v1 and legacy UART_COMMAND fail-closed regressions passed")
