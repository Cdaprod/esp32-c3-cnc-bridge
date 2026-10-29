from pathlib import Path


serial = Path("src/serial_handler.c").read_text()
wireless = Path("src/espnow_handler.c").read_text()

# UART diagnostics aggregate metadata once per second and never serialize the
# arbitrary command bytes that may pass through the bridge.
assert 'event=uart_%s source=uart0 result=%s chunks=%lu bytes=%lu window_ms=%lu' in serial
assert 'transmit ? "accepted" : "observed"' in serial
assert 'if (elapsed_ms < 1000) return;' in serial
assert "log_uart_activity(true, written);" in serial
assert "log_uart_activity(false, data->length);" in serial

# MPG jog TX includes the packet identity but no G-code payload. In dry-run,
# this writer is never called; the separate proposal event remains explicit.
assert 'event=uart_tx operation=mpg_jog source=uart0 result=accepted bytes=%u session=%lu seq=%u' in wireless

# GRBL status is logged only after the existing strict parser accepts it, at
# most once per second, and after the MPG mutex is released.
valid = wireless.index("const bool valid_status = mpg_grbl_status_parse(")
status_log = wireless.index('event=grbl_status source=uart0 age_ms=0 line=%s')
unlock = wireless.index("xSemaphoreGive(mpg_mutex);", valid)
assert valid < unlock < status_log
assert "mpg_grbl_status_log_ms) >= 1000" in wireless
