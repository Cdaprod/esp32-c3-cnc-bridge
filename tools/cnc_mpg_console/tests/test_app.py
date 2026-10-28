import asyncio

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
