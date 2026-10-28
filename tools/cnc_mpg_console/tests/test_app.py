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
