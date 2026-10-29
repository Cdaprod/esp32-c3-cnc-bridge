"""Static guards for bounded, payload-safe C3 USB console telemetry."""
from pathlib import Path


root = Path(__file__).resolve().parents[1]
handler = (root / "src/espnow_handler.c").read_text()
assert "event=uart_tx operation=mpg_jog" in handler
assert "event=mpg_stop result=sent physical_uart=0x85" in handler
ingest_start = handler.index("static void mpg_telemetry_ingest(")
ingest_end = handler.index("\nstatic void mpg_watchdog_task(", ingest_start)
ingest = handler[ingest_start:ingest_end]
parse = ingest.index("mpg_grbl_status_parse(")
report = ingest.index('event=grbl_status source=uart0')
unlock = ingest.index("xSemaphoreGive(mpg_mutex)")
assert parse < unlock < report, \
    "only validated GRBL status may be logged, outside the MPG mutex"
assert "state_changed ||" in ingest and ">= 1000" in ingest, \
    "steady status logging must be bounded to at most once per second"

serial = (root / "src/serial_handler.c").read_text()
logger_start = serial.index("static void log_uart_activity(")
logger_end = serial.index("\nbool serial_uart_tx_idle(", logger_start)
logger = serial[logger_start:logger_end]
assert "event=uart_%s" in logger
assert "bytes_window=" in logger and "payload=omitted" in logger
assert "now_ms - *since) >= 1000" in logger
assert "log_uart_activity(false, len)" in serial
assert "log_uart_activity(true, written)" in serial
assert "data->data" not in logger, "USB diagnostics must not expose raw UART bytes"

main = (root / "src/main.c").read_text()
assert "PWD:  %s" not in main
assert "AP Password: %s" not in main
assert "Password configured: %s" in main
print("C3 console telemetry is validated, rate-limited, and payload-safe")
