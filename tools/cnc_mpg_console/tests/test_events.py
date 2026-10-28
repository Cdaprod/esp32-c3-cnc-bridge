from datetime import datetime, timezone

from cnc_mpg_console.events import Event, classify_line, extract_grbl_state, is_fresh


def test_classification_and_fields_are_observed_not_invented():
    line = "I (99) MPG: intent rejected reason=stale MAC=aa:bb seq=145 session=22"
    event = Event.from_line("C3", line, monotonic_ns=123, wall_time=datetime(2026, 1, 1, tzinfo=timezone.utc))
    assert event.category == "safety"
    assert event.fields == {"reason": "stale", "mac": "aa:bb", "seq": "145", "session": "22", "firmware_tick_ms": "99"}
    assert event.host_monotonic_ns == 123
    assert event.host_time.startswith("2026-01-01T00:00:00")


def test_lines_without_available_telemetry_are_generic():
    assert classify_line("plain diagnostic message") == "log"
    assert classify_line("Controller offline") == "controller"


def test_dry_run_does_not_get_reported_as_uart_delivery():
    assert classify_line("MPG DRY_RUN command=$J=G91X0.1F100") == "dry-run"
    assert classify_line("W (101) MPG: event=motion_admission result=accepted seq=9") == "safety"
    assert classify_line("I (102) GRBL: event=uart_tx operation=mpg_jog seq=9") == "controller"


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


def test_grbl_state_requires_explicit_status_event_and_expires():
    line = "I (100) GRBL: event=grbl_status source=uart0 age_ms=0 line=<Idle|MPos:0,0,0|FS:0,0>"
    assert extract_grbl_state(line) == "Idle"
    assert extract_grbl_state("controller paired") is None
    assert is_fresh(1_000, 1_500, max_age_ns=500)
    assert not is_fresh(1_000, 1_501, max_age_ns=500)
