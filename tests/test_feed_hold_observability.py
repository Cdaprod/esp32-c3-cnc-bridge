from cnc_mpg_console.events import Event, classify_line


def test_feed_hold_log_is_classified_without_claiming_machine_execution():
    assert classify_line(
        "I MPG: event=mpg_feed_hold result=dry_run_proposed byte=0x21 "
        "physical_uart=not_sent session=7 seq=9"
    ) == "dry-run"
    assert classify_line(
        "W MPG: event=mpg_feed_hold result=rejected reason=uart_busy"
    ) == "safety"
    assert classify_line(
        "I MPG: event=mpg_feed_hold result=uart_write_accepted byte=0x21 "
        "session=7 seq=9"
    ) == "controller"


def test_feed_hold_identity_remains_correlatable():
    event = Event.from_line(
        "C3", "I MPG: event=mpg_feed_hold result=dry_run_proposed "
        "byte=0x21 physical_uart=not_sent session=7 seq=9"
    )
    assert event.fields["session"] == "7"
    assert event.fields["seq"] == "9"
    assert event.correlation_key == "7:9"
