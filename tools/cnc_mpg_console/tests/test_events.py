from datetime import datetime, timezone

from cnc_mpg_console.events import (
    Event, classify_line, extract_grbl_state, extract_grbl_telemetry, is_fresh,
)


def test_classification_and_fields_are_observed_not_invented():
    line = "I (99) MPG: intent rejected reason=stale MAC=aa:bb seq=145 session=22"
    event = Event.from_line("C3", line, monotonic_ns=123, wall_time=datetime(2026, 1, 1, tzinfo=timezone.utc))
    assert event.category == "safety"
    assert event.fields == {"reason": "stale", "mac": "aa:bb", "seq": "145", "session": "22", "firmware_tick_ms": "99"}
    assert event.host_monotonic_ns == 123
    assert event.host_time.startswith("2026-01-01T00:00:00")


def test_s3_invalid_input_and_encoder_overrun_are_safety_events():
    assert classify_line(
        "[event] event=input_validity domain=selectors valid=False reason=not_one_hot"
    ) == "safety"
    assert classify_line(
        "[event] event=input_validity domain=hardware valid=False "
        "reason=mcp23017_read_failed") == "safety"
    overrun = Event.from_line(
        "S3", "[event] event=encoder_overrun delta=100 max_detents=4",
        monotonic_ns=444)
    assert overrun.category == "safety"
    assert overrun.fields == {
        "event": "encoder_overrun", "delta": "100", "max_detents": "4"}
    assert classify_line(
        "[event] event=input_rejected reason=selector_invalid") == "safety"


def test_lines_without_available_telemetry_are_generic():
    assert classify_line("plain diagnostic message") == "log"
    assert classify_line("Controller offline") == "controller"


def test_dry_run_does_not_get_reported_as_uart_delivery():
    assert classify_line("MPG DRY_RUN command=$J=G91X0.1F100") == "dry-run"
    assert classify_line("W (101) MPG: event=motion_admission result=accepted seq=9") == "safety"
    assert classify_line("I (102) GRBL: event=uart_tx operation=mpg_jog seq=9") == "controller"


def test_controller_uart_ownership_rejection_is_visible_as_safety_event():
    event = Event.from_line(
        "C3", "W (105) SERIAL_HANDLER: event=uart_tx_rejected "
        "reason=mpg_owns_controller bytes=12 total=3", monotonic_ns=989)
    assert event.category == "safety"
    assert event.fields["reason"] == "mpg_owns_controller"
    assert event.fields["bytes"] == "12"
    assert event.fields["total"] == "3"


def test_grbl_realtime_jog_cancel_is_visible_as_safety_event():
    event = Event.from_line(
        "C3", "I (106) GRBL: event=uart_tx operation=mpg_stop_cancel "
        "session=14 seq=9 bytes=1", monotonic_ns=990)
    assert event.category == "safety"
    assert event.fields["operation"] == "mpg_stop_cancel"
    assert event.fields["bytes"] == "1"
    # Jog command transmission stays classified as controller output.
    assert classify_line(
        "I (107) GRBL: event=uart_tx operation=mpg_jog seq=10") == "controller"


def test_c3_dry_run_proposal_keeps_packet_identity_and_exact_command():
    line = (
        "I (103) MPG: event=dry_run result=proposed "
        "mac=02:11:22:33:44:55 session=305419896 seq=256 axis=X "
        "microns=-10 feed=250 command_hex=244a3d4739312047323120582d302e30313020463235300a"
    )
    event = Event.from_line("C3", line, monotonic_ns=987)
    assert event.category == "dry-run"
    assert event.fields["session"] == "305419896"
    assert event.fields["seq"] == "256"
    assert event.fields["axis"] == "X"
    assert event.fields["microns"] == "-10"
    assert event.fields["feed"] == "250"
    assert bytes.fromhex(event.fields["command_hex"]).decode() == "$J=G91 G21 X-0.010 F250\n"


def test_c3_a_axis_dry_run_is_observed_proposal_not_uart_delivery():
    line = (
        "I (104) MPG: event=dry_run result=proposed "
        "mac=02:11:22:33:44:55 session=305419896 seq=257 axis=A "
        "microns=10 feed=500 command_hex=244a3d473931204732312041302e30313020463530300a"
    )
    event = Event.from_line("C3", line, monotonic_ns=988)
    assert event.category == "dry-run"
    assert event.fields["axis"] == "A"
    assert (event.fields["session"], event.fields["seq"]) == (
        "305419896", "257")
    assert bytes.fromhex(event.fields["command_hex"]).decode() == "$J=G91 G21 A0.010 F500\n"


def test_grbl_state_requires_explicit_status_event_and_expires():
    line = "I (100) GRBL: event=grbl_status source=uart0 age_ms=0 line=<Idle|MPos:0,0,0|FS:0,0>"
    assert extract_grbl_state(line) == "Idle"
    assert extract_grbl_state("controller paired") is None
    assert is_fresh(1_000, 1_500, max_age_ns=500)
    assert not is_fresh(1_000, 1_501, max_age_ns=500)


def test_grbl_telemetry_parses_only_finite_observed_fields():
    line = (
        "I (100) GRBL: event=grbl_status source=uart0 age_ms=0 "
        "line=<Idle|MPos:1.25,-2,3|WPos:0.25,0,3|FS:1200,8000>"
    )
    assert extract_grbl_telemetry(line) == {
        "state": "Idle",
        "mpos": "X=1.250 Y=-2.000 Z=3.000",
        "wpos": "X=0.250 Y=0.000 Z=3.000",
        "feed": "1200.0",
        "spindle": "8000.0",
    }
    assert extract_grbl_telemetry(
        "plain log line=<Idle|MPos:1,2,3|FS:4,5>") is None
    # Invalid individual fields are omitted; the validated state remains
    # visible and no NaN/inf or malformed coordinates leak into the UI.
    assert extract_grbl_telemetry(
        "event=grbl_status line=<Run|MPos:nan,2,3|FS:inf,5>") == {
            "state": "Run",
        }


def test_grbl_telemetry_supports_rotary_axis_and_feed_only_reports():
    assert extract_grbl_telemetry(
        "event=grbl_status line=<Idle|MPos:1,2,3,4|F:99>") == {
            "state": "Idle",
            "mpos": "X=1.000 Y=2.000 Z=3.000 A=4.000",
            "feed": "99.0",
        }


def test_correlation_key_normalizes_packet_identity_and_rejects_incomplete_fields():
    decimal = Event.from_line(
        "S3", "event=espnow_tx session=123 seq=17", monotonic_ns=1)
    hexadecimal = Event.from_line(
        "C3", "event=dry_run session=0x7b seq=0x11", monotonic_ns=2)
    incomplete = Event.from_line(
        "C3", "event=dry_run session=123", monotonic_ns=3)
    assert decimal.correlation_key == "123:17"
    assert hexadecimal.correlation_key == decimal.correlation_key
    assert decimal.to_json()["correlation_key"] == "123:17"
    assert incomplete.correlation_key is None
