"""Textual dashboard. Passive by default; only optional writes target S3 REPL."""

from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
import queue
import time

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, Footer, Header, Input, RichLog, Static

from .devices import choose_device, discover_devices
from .events import Event, extract_grbl_state, is_fresh
from .recording import Recorder
from .serial_reader import SerialReader


class ConsoleApp(App):
    TITLE = "CNC MPG Development Console"
    CSS = """
    Screen { layout: vertical; }
    #status { height: 1; padding: 0 1; }
    #panels { height: 1fr; }
    .panel { width: 1fr; border: round $accent; }
    .panel-title { height: 1; text-style: bold; padding: 0 1; }
    RichLog { height: 1fr; scrollbar-size: 1 1; }
    #timeline-title { height: 1; text-style: bold; padding: 0 1; }
    #timeline { height: 12; border: round $secondary; }
    #controls { height: 3; }
    #filter { width: 1fr; }
    #repl { height: 3; display: none; }
    #repl.enabled { display: block; }
    """
    BINDINGS = [("q", "quit", "Quit"), ("f", "focus_filter", "Filter"),
                ("r", "toggle_record", "Record"), ("p", "toggle_pause", "Pause"),
                ("e", "export", "Export")]

    def __init__(self, s3_port: str | None, c3_port: str | None,
                 s3_baud: int, c3_baud: int, enable_s3_repl: bool,
                 log_dir: Path, *, auto_s3: bool = False, auto_c3: bool = False):
        super().__init__()
        self.bus: queue.Queue = queue.Queue(maxsize=5000)
        self.readers = {
            "S3": SerialReader("S3", s3_port, s3_baud, self.bus,
                               allow_s3_repl=enable_s3_repl,
                               auto_role="s3" if auto_s3 else None),
            "C3": SerialReader("C3", c3_port, c3_baud, self.bus,
                               auto_role="c3" if auto_c3 else None),
        }
        self.states = {"S3": "disconnected", "C3": "disconnected"}
        self.counts = {"S3": 0, "C3": 0}
        self.errors = {"S3": 0, "C3": 0}
        self.radio_log_lines = {"S3": 0, "C3": 0}
        self.grbl_status: str | None = None
        self.grbl_status_host_ns: int | None = None
        self.recent: deque[Event] = deque(maxlen=2000)
        self.paused_events: deque[Event] = deque(maxlen=2000)
        self.filter_text = ""
        self.paused = False
        self.recorder: Recorder | None = None
        self.log_dir = log_dir
        self.s3_repl_enabled = enable_s3_repl
        self.device_hints = {"S3": s3_port, "C3": c3_port}

    def compose(self) -> ComposeResult:
        yield Header()
        yield Static("S3 ○ Disconnected    C3 ○ Disconnected    GRBL ○ Unverified", id="status")
        with Horizontal(id="panels"):
            with Vertical(classes="panel"):
                yield Static("ESP32-S3 PENDANT · PASSIVE", classes="panel-title")
                yield RichLog(id="s3-log", wrap=True, markup=False, highlight=True, max_lines=2000)
            with Vertical(classes="panel"):
                yield Static("ESP32-C3 BRIDGE · PASSIVE", classes="panel-title")
                yield RichLog(id="c3-log", wrap=True, markup=False, highlight=True, max_lines=2000)
        yield Static("CORRELATED EVENT TIMELINE", id="timeline-title")
        yield RichLog(id="timeline", wrap=True, markup=False, highlight=True, max_lines=2000)
        with Horizontal(id="controls"):
            yield Button("Record", id="record")
            yield Button("Pause", id="pause")
            yield Input(placeholder="Filter events (substring)", id="filter")
            yield Button("Export JSONL", id="export")
            yield Button("Reconnect", id="reconnect")
        yield Input(placeholder="S3 CircuitPython REPL (explicitly enabled; may interrupt runtime)", id="repl")
        yield Footer()

    def on_mount(self) -> None:
        for reader in self.readers.values():
            reader.start()
        repl = self.query_one("#repl", Input)
        if self.s3_repl_enabled:
            repl.add_class("enabled")
            repl.placeholder = "S3 REPL enabled — Enter sends Python to S3 only"
        self.set_interval(0.05, self.drain_events)
        self.set_interval(1.0, self.refresh_status)
        for role, port in self.device_hints.items():
            reader = self.readers[role]
            message = (f"Configured port: {port}" if port else
                       "Automatic discovery active; role matching must be unambiguous." if reader.auto_role else
                       "No unambiguous port configured. Use --s3-port / --c3-port; this console will not guess.")
            self.query_one(f"#{role.lower()}-log", RichLog).write(message)

    def on_unmount(self) -> None:
        for reader in self.readers.values():
            reader.stop()
        for reader in self.readers.values():
            reader.join(timeout=1.0)
        if self.recorder:
            self.recorder.close()

    def refresh_status(self) -> None:
        for role, reader in self.readers.items():
            self.states[role] = "connected" if reader.connected.is_set() else "disconnected"
        s3 = "●" if self.states["S3"] == "connected" else "○"
        c3 = "●" if self.states["C3"] == "connected" else "○"
        grbl_state = "unverified"
        if self.grbl_status_host_ns is not None:
            grbl_state = self.grbl_status if is_fresh(self.grbl_status_host_ns, time.monotonic_ns()) else "stale"
        self.query_one("#status", Static).update(
            f"S3 {s3} {self.states['S3']} ({self.counts['S3']} lines, {self.errors['S3']} errors)    "
            f"C3 {c3} {self.states['C3']} ({self.counts['C3']} lines, {self.errors['C3']} errors)    "
            f"Radio log lines S3/C3 {self.radio_log_lines['S3']}/{self.radio_log_lines['C3']}    "
            f"GRBL {grbl_state} (from C3 status log only)")

    def drain_events(self) -> None:
        while True:
            try:
                item = self.bus.get_nowait()
            except queue.Empty:
                break
            role = item["source"]
            if item["kind"] == "status":
                message = f"{role} {'connected' if item.get('connected') else 'disconnected'} port={item.get('port')}"
                if item.get("error"):
                    message += f" error={item['error']}"
                    self.errors[role] += 1
                self._add_event(Event.from_line(role, message,
                    monotonic_ns=item.get("host_monotonic_ns"), wall_time=item.get("host_time")))
            elif item["kind"] == "overflow":
                self._add_event(Event.from_line(role, f"reader queue overflow; dropped={item['dropped']}",
                    monotonic_ns=item.get("host_monotonic_ns"), wall_time=item.get("host_time")))
            elif item["kind"] == "line":
                self.counts[role] += 1
                event = Event.from_line(role, item["line"],
                    monotonic_ns=item.get("host_monotonic_ns"), wall_time=item.get("host_time"))
                if event.category == "radio":
                    self.radio_log_lines[role] += 1
                elif event.category in ("error", "safety"):
                    self.errors[role] += 1
                status = extract_grbl_state(item["line"])
                if status:
                    self.grbl_status = status
                    self.grbl_status_host_ns = event.host_monotonic_ns
                self._add_event(event)

    def _add_event(self, event: Event) -> None:
        self.recent.append(event)
        if self.recorder:
            self.recorder.write(event)
        if self.paused:
            self.paused_events.append(event)
            return
        term = self.filter_text
        if term and term not in event.message.lower() and term not in event.category.lower():
            return
        if event.source in ("S3", "C3"):
            self.query_one(f"#{event.source.lower()}-log", RichLog).write(
                f"{event.host_time} {event.category.upper():<10} {event.message}")
        self.query_one("#timeline", RichLog).write(
            f"{event.host_time} {event.source:<2} {event.category.upper():<10} {event.message}")

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "filter":
            self.filter_text = event.value.lower().strip()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id == "repl" and event.value:
            reader = self.readers["S3"]
            if reader.send_repl_line(event.value):
                self._add_event(Event.from_line("S3", "REPL input sent to S3 (dashboard does not send CNC commands)"))
                event.input.value = ""
            else:
                self._add_event(Event.from_line("S3", "REPL input blocked: --enable-s3-repl is not active or S3 is disconnected"))

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "record":
            self.action_toggle_record()
        elif event.button.id == "pause":
            self.action_toggle_pause()
        elif event.button.id == "export":
            self.action_export()
        elif event.button.id == "reconnect":
            self.action_reconnect()

    def action_toggle_record(self) -> None:
        if self.recorder:
            path = self.recorder.path
            self.recorder.close()
            self.recorder = None
            self.query_one("#record", Button).label = "Record"
            self._add_event(Event.from_line("HOST", f"recording stopped path={path}"))
        else:
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            path = self.log_dir / f"cnc-mpg-{stamp}.jsonl"
            self.recorder = Recorder(path)
            self.query_one("#record", Button).label = "Stop recording"
            self._add_event(Event.from_line("HOST", f"recording started path={path}"))

    def action_toggle_pause(self) -> None:
        self.paused = not self.paused
        self.query_one("#pause", Button).label = "Resume" if self.paused else "Pause"
        if not self.paused:
            pending, self.paused_events = self.paused_events, []
            for event in pending:
                term = self.filter_text
                if term and term not in event.message.lower() and term not in event.category.lower():
                    continue
                if event.source in ("S3", "C3"):
                    self.query_one(f"#{event.source.lower()}-log", RichLog).write(
                        f"{event.host_time} {event.category.upper():<10} {event.message}")
                self.query_one("#timeline", RichLog).write(
                    f"{event.host_time} {event.source:<2} {event.category.upper():<10} {event.message}")
            self._add_event(Event.from_line("HOST", "display resumed"))

    def action_export(self) -> None:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        path = self.log_dir / f"cnc-mpg-export-{stamp}.jsonl"
        path.parent.mkdir(parents=True, exist_ok=True)
        import json
        with path.open("w", encoding="utf-8") as stream:
            for event in self.recent:
                stream.write(json.dumps(event.to_json(), ensure_ascii=False) + "\n")
        self._add_event(Event.from_line("HOST", f"exported {len(self.recent)} events to {path}"))

    def action_reconnect(self) -> None:
        for reader in self.readers.values():
            reader.reconnect()
        self._add_event(Event.from_line("HOST", "reconnect requested; ports will be reopened by their existing readers"))

    def action_focus_filter(self) -> None:
        self.query_one("#filter", Input).focus()


def main() -> None:
    parser = argparse.ArgumentParser(description="Passive S3/C3 serial development console")
    parser.add_argument("--s3-port", help="S3 CircuitPython USB CDC port")
    parser.add_argument("--c3-port", help="C3 ESP-IDF USB Serial/JTAG port")
    parser.add_argument("--s3-baud", type=int, default=115200)
    parser.add_argument("--c3-baud", type=int, default=115200)
    parser.add_argument("--enable-s3-repl", action="store_true", help="Enable explicitly selected S3 REPL input; can interrupt the running app")
    parser.add_argument("--log-dir", type=Path, default=Path("logs"))
    parser.add_argument("--list-devices", action="store_true")
    args = parser.parse_args()
    devices = discover_devices()
    if args.list_devices:
        for d in devices:
            print(f"{d.device}\t{d.stable_id}\t{d.description} [{d.manufacturer or 'unknown'}]")
        return
    auto_s3 = args.s3_port is None
    auto_c3 = args.c3_port is None
    if auto_s3:
        device, _reason = choose_device(devices, "s3")
        args.s3_port = device.device if device else None
    if auto_c3:
        device, _reason = choose_device(devices, "c3")
        if device and device.device != args.s3_port:
            args.c3_port = device.device
    if args.s3_port and args.s3_port == args.c3_port:
        parser.error("S3 and C3 cannot share the same serial port")
    ConsoleApp(args.s3_port, args.c3_port, args.s3_baud, args.c3_baud,
               args.enable_s3_repl, args.log_dir, auto_s3=auto_s3,
               auto_c3=auto_c3).run()


if __name__ == "__main__":
    main()
