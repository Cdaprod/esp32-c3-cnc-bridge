import asyncio
import time
from unittest.mock import patch

from cnc_mpg_console.app import ConsoleApp
from textual.widgets import RichLog


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
            app.refresh_status()
            status = str(app.query_one("#status").render())
            assert "GRBL Idle" in status
            assert "Machine telemetry MPos X=0.000 Y=0.000 Z=0.000" in status
            assert "F=0.0" in status and "S=0.0" in status
            app.grbl_status_host_ns = time.monotonic_ns() - 2_000_000_000
            app.refresh_status()
            assert "Machine telemetry stale" in str(
                app.query_one("#status").render())
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


def test_dashboard_shows_last_observed_radio_transitions_without_claiming_live_link(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=espnow_link state=paired session=123 peer=aa",
                "host_monotonic_ns": time.monotonic_ns(),
            })
            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "I (123) ESPNOW: PAIR v2 accettato da aa:bb:cc:dd:ee:ff "
                        "session=0x0000007B",
                "host_monotonic_ns": time.monotonic_ns(),
            })
            await pilot.pause(0.12)
            app.refresh_status()
            status = str(app.query_one("#status").render())
            assert "S3 last=paired session=123 (0s ago)" in status
            assert "C3 last=pair_accepted session=0x0000007B peer=aa:bb:cc:dd:ee:ff (0s ago)" in status
            assert "Radio last observed" in status

            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "W (124) ESPNOW: Peer aa:bb:cc:dd:ee:ff rimosso per timeout",
                "host_monotonic_ns": time.monotonic_ns(),
            })
            app.drain_events()
            app.refresh_status()
            assert "C3 last=peer_removed peer=aa:bb:cc:dd:ee:ff reason=timeout" in str(
                app.query_one("#status").render())
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


def test_successful_dry_run_admission_does_not_increment_device_errors(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)):
            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "I (103) MPG: event=motion_admission result=accepted "
                        "mode=dry_run session=3 seq=9",
            })
            app.drain_events()
            assert app.errors["C3"] == 0
            assert app.recent[-1].category == "dry-run"
    asyncio.run(scenario())


def test_dry_run_panel_shows_verified_proposed_grbl_command(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        rendered = []
        original_write = RichLog.write

        def capture_write(widget, renderable, *args, **kwargs):
            rendered.append(str(renderable))
            return original_write(widget, renderable, *args, **kwargs)

        proposal = (
            "I (103) MPG: event=dry_run result=proposed mac=02:11:22:33:44:55 "
            "session=123 seq=17 axis=X microns=-10 feed=250 "
            "command_hex=244a3d4739312047323120582d302e30313020463235300a"
        )
        with patch.object(RichLog, "write", capture_write):
            async with app.run_test(size=(120, 40)) as pilot:
                app.bus.put_nowait({
                    "kind": "line", "source": "C3", "line": proposal,
                })
                await pilot.pause(0.12)
        assert any("proposed GRBL (dry-run): $J=G91 G21 X-0.010 F250" in line
                   for line in rendered)

    asyncio.run(scenario())


def test_s3_input_health_uses_explicit_runtime_event_and_expires(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)):
            app.bus.put_nowait({
                "kind": "line", "source": "C3",
                "line": "event=runtime input_backend=configured "
                        "input_validity=valid selector_validity=valid axis=X "
                        "selector_position=X increment=X10 deadman=active",
                "host_monotonic_ns": time.monotonic_ns(),
            })
            app.drain_events()
            assert app.s3_input_status is None

            observed_at = time.monotonic_ns()
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=runtime input_backend=disabled "
                        "input_validity=unknown selector_validity=unknown "
                        "axis=unknown selector_position=unknown "
                        "increment=unknown deadman=unknown",
                "host_monotonic_ns": observed_at,
            })
            app.drain_events()
            app.refresh_status()
            assert app.s3_input_status == (
                "disabled", "unknown", "unknown", "unknown", "unknown",
                "unknown", "unknown")
            assert "Inputs disabled/unknown/unknown axis=unknown selector=unknown step=unknown deadman=unknown" in str(
                app.query_one("#status").render())

            observed_at = time.monotonic_ns()
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=runtime input_backend=configured "
                        "input_validity=valid selector_validity=valid axis=A "
                        "selector_position=4 increment=X10 deadman=active",
                "host_monotonic_ns": observed_at,
            })
            app.drain_events()
            app.refresh_status()
            assert app.s3_input_status == (
                "configured", "valid", "valid", "A", "4", "X10", "active")
            assert "Inputs configured/valid/valid axis=A selector=4 step=X10 deadman=active" in str(
                app.query_one("#status").render())

            app.s3_input_status_host_ns = time.monotonic_ns() - 31_000_000_000
            app.refresh_status()
            assert "Inputs stale" in str(app.query_one("#status").render())

    asyncio.run(scenario())


def test_s3_touch_diagnostics_are_summarized_and_expire(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        observed_at = time.monotonic_ns()
        async with app.run_test(size=(120, 40)):
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=runtime touch_polls=120 "
                        "touch_active_samples=0 touch_i2c_reads=0 "
                        "touch_i2c_busy=0 touch_irq_idle=120 touch_errors=0",
                "host_monotonic_ns": observed_at,
            })
            app.drain_events()
            app.refresh_status()
            assert app.s3_touch_status == ("120", "0", "0", "0", "120", "0")
            assert "Touch S3 polls=120 active=0 reads=0 bus-busy=0 IRQ-idle=120 errors=0" in str(
                app.query_one("#status").render())

            app.s3_touch_status_host_ns = time.monotonic_ns() - 31_000_000_000
            app.refresh_status()
            assert "Touch S3 stale" in str(app.query_one("#status").render())

    asyncio.run(scenario())


def test_s3_loop_work_and_overruns_are_summarized_and_expire(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)):
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=runtime loop_work_ms=3 loop_overruns=2",
                "host_monotonic_ns": time.monotonic_ns(),
            })
            app.drain_events()
            app.refresh_status()
            assert app.s3_loop_status == ("3", "2")
            assert "loop work=3ms overruns=2/10s" in str(
                app.query_one("#status").render())

            app.s3_loop_status_host_ns = time.monotonic_ns() - 31_000_000_000
            app.refresh_status()
            assert "loop stale" in str(app.query_one("#status").render())

    asyncio.run(scenario())


def test_compact_terminal_layout_keeps_device_logs_visible(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(80, 24)) as pilot:
            await pilot.pause()
            app.refresh_status()
            await pilot.pause()
            assert app.query_one("#status").size.height == 6
            assert app.query_one("#timeline").region.height >= 4
            assert app.query_one("#s3-log").size.height >= 3
            assert app.query_one("#c3-log").size.height >= 3
            assert app.query_one("#controls").region.bottom <= 24

    asyncio.run(scenario())


def test_serial_burst_renders_in_widget_batches_and_bounds_each_drain(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        writes = []
        original_write = RichLog.write

        def capture_write(widget, renderable, *args, **kwargs):
            writes.append((widget.id, str(renderable)))
            return original_write(widget, renderable, *args, **kwargs)

        with patch.object(RichLog, "write", capture_write):
            async with app.run_test(size=(120, 40)):
                writes.clear()  # Ignore one-time configured-port panel messages.
                for index in range(300):
                    app.bus.put_nowait({
                        "kind": "line", "source": "S3",
                        "line": f"event=runtime sample={index}",
                    })
                app.drain_events()
                assert len(app.recent) == 256
                assert app.bus.qsize() == 44
                assert [widget for widget, _ in writes] == ["s3-log", "timeline"]
                assert writes[0][1].count("event=runtime") == 256
                writes.clear()
                app.drain_events()
                assert len(app.recent) == 300
                assert [widget for widget, _ in writes] == ["s3-log", "timeline"]
    asyncio.run(scenario())


def test_s3_runtime_reports_unconfigured_deadman_as_unavailable(tmp_path):
    async def scenario():
        app = ConsoleApp(None, None, 115200, 115200, False, tmp_path)
        async with app.run_test(size=(120, 40)):
            app.bus.put_nowait({
                "kind": "line", "source": "S3",
                "line": "[event] event=runtime input_backend=configured "
                        "input_validity=valid selector_validity=invalid "
                        "axis=OFF selector_position=OFF increment=invalid "
                        "deadman=unavailable",
                "host_monotonic_ns": time.monotonic_ns(),
            })
            app.drain_events()
            app.refresh_status()
            assert app.s3_input_status == (
                "configured", "valid", "invalid", "OFF", "OFF",
                "invalid", "unavailable")
            assert "deadman=unavailable" in str(
                app.query_one("#status").render())

    asyncio.run(scenario())
