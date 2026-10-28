from pathlib import Path


source = Path("src/espnow_handler.c").read_text()

ingest_start = source.index("static void mpg_telemetry_ingest(")
ingest_end = source.index("static void mpg_watchdog_task(", ingest_start)
ingest = source[ingest_start:ingest_end]
assert "mpg_grbl_ack_consume_line(" in ingest
assert "mpg_finish_grbl_ack(&completed, completion" in ingest

watchdog_start = source.index("static void mpg_watchdog_task(")
watchdog_end = source.index("static const char *TAG", watchdog_start)
watchdog = source[watchdog_start:watchdog_end]
assert "mpg_grbl_ack_expire(" in watchdog
assert "mpg_finish_grbl_ack(&completed_ack, ack_result" in watchdog

jog_start = source.index("case ESPNOW_MSG_MPG_JOG: {")
jog_end = source.index("case ESPNOW_MSG_MPG_STOP:", jog_start)
jog = source[jog_start:jog_end]
assert "#if MPG_ENABLE_MOTION && !MPG_DRY_RUN" in jog
assert "mpg_grbl_ack_begin(" in jog
assert "if (!defer_ack)" in jog
assert "reason=grbl_ack_pending" in jog

stop_start = source.index("case ESPNOW_MSG_MPG_STOP:", jog_end)
stop_end = source.index("case ESPNOW_MSG_UART_COMMAND:", stop_start)
stop = source[stop_start:stop_end]
assert "mpg_grbl_ack_cancel(" in stop
assert "mpg_finish_grbl_ack(&cancelled_ack" in stop

print("Physical jog ACK is deferred to GRBL response and cleared on timeout/STOP")
