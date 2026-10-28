import asyncio
import time

from cnc_mpg_console.app import ConsoleApp


def test_dashboard_smoke_updates_observed_grbl_state_and_exports(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "I (100) GRBL: event=grbl_status source=uart0 age_ms=0 line=<Idle|MPos:0,0,0|FS:0,0>",
            })
            await pilot.pause(0.12)
            assert app.grbl_status == "Idle"
            await pilot.click("#record")
            assert app.recorder is not None
            await pilot.click("#record")
            await pilot.click("#export")
            assert list(tmp_path.glob("*.jsonl"))
    asyncio.run(scenario())


def test_dashboard_correlates_observed_events_from_both_devices(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=encoder_delta delta=1 axis=X increment=X10",
                "host_monotonic_ns": 101,
            })
            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "I (99) MPG: event=motion_admission result=rejected reason=motion_disabled session=22 seq=145",
                "host_monotonic_ns": 102,
            })
            await pilot.pause(0.12)
            assert [event.source for event in app.recent if event.source in ("S3", "C3")] == ["S3", "C3"]
            c3_event = next(event for event in app.recent
                            if event.source == "C3" and "seq=145" in event.message)
            assert c3_event.fields["seq"] == "145"
            assert c3_event.fields["session"] == "22"
            assert c3_event.host_monotonic_ns == 102
    asyncio.run(scenario())


def test_mpg_trace_matches_s3_submission_to_c3_proposal_by_session_and_sequence(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            # Deliberately deliver C3 first to prove correlation does not rely
            # on USB arrival order and never treats a TX call as OTA delivery.
            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "I (104) MPG: event=dry_run result=proposed session=0x7b seq=0x11 axis=A",
                "host_monotonic_ns": 102,
            })
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=espnow_tx packet_type=mpg_jog session=123 seq=17 result=driver_accepted_not_ota_ack",
                "host_monotonic_ns": 101,
            })
            await pilot.pause(0.12)
            assert app.mpg_s3_submissions == 1
            assert app.mpg_c3_proposals == 1
            assert app.mpg_matched == 1
            assert "matched session/seq=1" in str(app.query_one("#mpg-trace").render())
    asyncio.run(scenario())


def test_s3_input_health_uses_explicit_runtime_event_and_expires(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)):
            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "event=runtime input_backend=configured "
                        "input_validity=valid selector_validity=valid",
                "host_monotonic_ns": time.monotonic_ns(),
            })
            app.drain_events()
            assert app.s3_input_status is None

            observed_at = time.monotonic_ns()
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=runtime input_backend=disabled "
                        "input_validity=unknown selector_validity=unknown",
                "host_monotonic_ns": observed_at,
            })
            app.drain_events()
            app.refresh_status()
            assert app.s3_input_status == ("disabled", "unknown", "unknown")
            assert "Inputs disabled/unknown/unknown" in str(
                app.query_one("#status").render())

            app.s3_input_status_host_ns = time.monotonic_ns() - 31_000_000_000
            app.refresh_status()
            assert "Inputs stale" in str(app.query_one("#status").render())

    asyncio.run(scenario())
