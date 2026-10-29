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
from .events import (Event, extract_grbl_state, extract_grbl_telemetry,
                     extract_radio_link_event, is_fresh,
                     proposed_dry_run_command)
from .recording import Recorder
from .serial_reader import SerialReader


class ConsoleApp(App):
    TITLE = "CNC MPG Development Console"
    CSS = """
    Screen { layout: vertical; }
    #status { height: 6; padding: 0 1; }
    #panels { height: 1fr; }
    .panel { width: 1fr; border: round $accent; }
    .panel-title { height: 1; text-style: bold; padding: 0 1; }
    RichLog { height: 1fr; scrollbar-size: 1 1; }
    #timeline-title { height: 1; text-style: bold; padding: 0 1; }
    #mpg-trace { height: 1; padding: 0 1; color: $text-muted; }
    #timeline { height: 8; border: round $secondary; }
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
        self.radio_link_events: dict[str, tuple[dict[str, str], int] | None] = {
            "S3": None, "C3": None,
        }
        self.grbl_status: str | None = None
        self.grbl_status_host_ns: int | None = None
        self.grbl_telemetry: dict[str, str] | None = None
        self.s3_input_status: tuple[str, str, str, str, str, str, str] | None = None
        self.s3_input_status_host_ns: int | None = None
        self.s3_touch_status: tuple[str, str, str, str, str, str] | None = None
        self.s3_touch_status_host_ns: int | None = None
        self.s3_loop_status: tuple[str, ...] | None = None
        self.s3_loop_status_host_ns: int | None = None
        self.recent: deque[Event] = deque(maxlen=2000)
        self.paused_events: deque[Event] = deque(maxlen=2000)
        self.mpg_s3_submissions = 0
        self.mpg_c3_proposals = 0
        self.mpg_matched = 0
        self._mpg_correlations: dict[str, int] = {}
        self._mpg_correlation_order: deque[str] = deque()
        self._mpg_trace_dirty = False
        self._rendering_batch = False
        self._render_buffer: dict[str, list[str]] = {
            "S3": [], "C3": [], "timeline": [],
        }
        self._filter_timer = None
        self.filter_text = ""
        self.paused = False
        self.recorder: Recorder | None = None
        self.log_dir = log_dir
        self.s3_repl_enabled = enable_s3_repl
        self.device_hints = {"S3": s3_port, "C3": c3_port}

    def _loop_summary(self, compact: bool = False) -> str:
        if self.s3_loop_status is None:
            return "unreported"
        work, overruns, maxima, max_loop, totals, sample_count = self.s3_loop_status
        labels = ("input", "touch", "transport", "wifi", "display", "mirror")
        max_index = max(range(len(maxima)), key=lambda index: int(maxima[index]))
        prefix = (f"app={work}ms ov={overruns}/10s" if compact else
                  f"loop work={work}ms overruns={overruns}/10s")
        if totals is not None and sample_count:
            avg_index = max(range(len(totals)),
                            key=lambda index: int(totals[index]))
            average = int(totals[avg_index]) / int(sample_count)
            return (f"{prefix} avg={labels[avg_index]}:{average:.1f}ms "
                    f"max={labels[max_index]}:{maxima[max_index]}ms "
                    f"peak-loop={max_loop}ms")
        return (f"{prefix} slowest={labels[max_index]}:"
                f"{maxima[max_index]}ms")

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
        yield Static("MPG observed: S3 submissions=0 | C3 dry-run proposals=0 | matched session/seq=0", id="mpg-trace")
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
                       "Port disabled: none selected. Pass --s3-port/--c3-port, or omit that flag for unambiguous discovery.")
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
        input_status = "unreported"
        if self.s3_input_status_host_ns is not None:
            if not is_fresh(self.s3_input_status_host_ns,
                            time.monotonic_ns(), max_age_ns=30_000_000_000):
                input_status = "stale"
            else:
                backend, inputs, selectors, axis, position, increment, deadman = self.s3_input_status
                input_status = (f"{backend}/{inputs}/{selectors} axis={axis} "
                                f"selector={position} "
                                f"step={increment} deadman={deadman}")
        telemetry_status = "unreported"
        if self.grbl_status_host_ns is not None:
            if not is_fresh(self.grbl_status_host_ns, time.monotonic_ns()):
                telemetry_status = "stale"
            elif self.grbl_telemetry:
                fields = []
                if "mpos" in self.grbl_telemetry:
                    fields.append("MPos " + self.grbl_telemetry["mpos"])
                if "wpos" in self.grbl_telemetry:
                    fields.append("WPos " + self.grbl_telemetry["wpos"])
                if "feed" in self.grbl_telemetry:
                    fields.append("F=" + self.grbl_telemetry["feed"])
                if "spindle" in self.grbl_telemetry:
                    fields.append("S=" + self.grbl_telemetry["spindle"])
                telemetry_status = " | ".join(fields) if fields else "no position/feed fields"
        touch_status = "unreported"
        if self.s3_touch_status_host_ns is not None:
            if not is_fresh(self.s3_touch_status_host_ns, time.monotonic_ns(),
                            max_age_ns=30_000_000_000):
                touch_status = "stale"
            else:
                polls, active, reads, busy, irq_idle, errors = self.s3_touch_status
                touch_status = (f"polls={polls} active={active} reads={reads} "
                                f"bus-busy={busy} IRQ-idle={irq_idle} errors={errors}")
        loop_status = "unreported"
        if self.s3_loop_status_host_ns is not None:
            if not is_fresh(self.s3_loop_status_host_ns, time.monotonic_ns(),
                            max_age_ns=30_000_000_000):
                loop_status = "stale"
            else:
                loop_status = self._loop_summary()
        radio_status = []
        now_ns = time.monotonic_ns()
        for role in ("S3", "C3"):
            observed = self.radio_link_events[role]
            if observed is None:
                radio_status.append(f"{role}=unreported")
                continue
            fields, observed_ns = observed
            age_s = max(0, now_ns - observed_ns) / 1_000_000_000
            detail = fields["state"]
            if "session" in fields:
                detail += f" session={fields['session']}"
            if "peer" in fields:
                detail += f" peer={fields['peer']}"
            if fields.get("reason"):
                detail += f" reason={fields['reason']}"
            radio_status.append(f"{role} last={detail} ({age_s:.0f}s ago)")
        status = self.query_one("#status", Static)
        if self.size.width < 100:
            status.styles.height = 6
            self.query_one("#timeline").styles.height = 4
            compact_inputs = "unreported"
            if self.s3_input_status is not None:
                backend, inputs, selectors, axis, position, increment, deadman = self.s3_input_status
                compact_inputs = (f"{backend}:{inputs}/{selectors} ax={axis} "
                                  f"sel={position} inc={increment} dm={deadman}")
                if not is_fresh(self.s3_input_status_host_ns,
                                time.monotonic_ns(),
                                max_age_ns=30_000_000_000):
                    compact_inputs = "stale"
            if self.s3_touch_status is None or self.s3_loop_status is None:
                compact_touch = "unreported"
            elif (not is_fresh(self.s3_touch_status_host_ns,
                               time.monotonic_ns(), max_age_ns=30_000_000_000) or
                  not is_fresh(self.s3_loop_status_host_ns,
                               time.monotonic_ns(), max_age_ns=30_000_000_000)):
                compact_touch = "stale"
            else:
                polls, active, reads, busy, irq_idle, errors = self.s3_touch_status
                compact_touch = (f"p={polls} a={active} r={reads} b={busy} "
                                 f"i={irq_idle} e={errors} "
                                 f"{self._loop_summary(compact=True)}")
            compact_radio = []
            now_ns = time.monotonic_ns()
            for role in ("S3", "C3"):
                observed = self.radio_link_events[role]
                if observed is None:
                    compact_radio.append(f"{role}=unreported")
                else:
                    fields, observed_ns = observed
                    age_s = max(0, now_ns - observed_ns) / 1_000_000_000
                    compact_radio.append(
                        f"{role}={fields['state']}({age_s:.0f}s)")
            status.update(
                f"S3 {s3} {self.states['S3']} {self.counts['S3']}L/{self.errors['S3']}E  "
                f"C3 {c3} {self.states['C3']} {self.counts['C3']}L/{self.errors['C3']}E\n"
                f"Inputs {compact_inputs}\n"
                f"Touch/loop {compact_touch}\n"
                f"Radio {' '.join(compact_radio)}\n"
                f"GRBL {grbl_state} | Machine {telemetry_status}")
        else:
            status.styles.height = 6
            self.query_one("#timeline").styles.height = 8
            status.update(
                f"S3 {s3} {self.states['S3']} ({self.counts['S3']} lines, {self.errors['S3']} errors)    "
                f"C3 {c3} {self.states['C3']} ({self.counts['C3']} lines, {self.errors['C3']} errors)\n"
                f"Radio log lines S3/C3 {self.radio_log_lines['S3']}/{self.radio_log_lines['C3']}    "
                f"Inputs {input_status}    GRBL {grbl_state} (C3 status log)\n"
                f"Touch S3 {touch_status} | loop {loop_status}\n"
                f"Radio last observed: {' | '.join(radio_status)}\n"
                f"Machine telemetry {telemetry_status}")

    def drain_events(self) -> None:
        processed = 0
        self._rendering_batch = True
        while processed < 256:
            try:
                item = self.bus.get_nowait()
            except queue.Empty:
                break
            processed += 1
            role = item["source"]
            if item["kind"] == "status":
                message = f"{role} {'connected' if item.get('connected') else 'disconnected'} port={item.get('port')}"
                if item.get("error"):
                    message += f" error={item['error']}"
                    self.errors[role] += 1
                self._add_event(Event.from_line(role, message,
                    monotonic_ns=item.get("host_monotonic_ns"), wall_time=item.get("host_time")))
                if item.get("connected"):
                    self.query_one(f"#{role.lower()}-log", RichLog).write(
                        "Passive stream attached. Output emitted before attachment (including boot logs) is not replayed.")
            elif item["kind"] == "overflow":
                self._add_event(Event.from_line(role, f"reader queue overflow; dropped={item['dropped']}",
                    monotonic_ns=item.get("host_monotonic_ns"), wall_time=item.get("host_time")))
            elif item["kind"] == "line":
                self.counts[role] += 1
                event = Event.from_line(role, item["line"],
                    monotonic_ns=item.get("host_monotonic_ns"), wall_time=item.get("host_time"))
                link_event = extract_radio_link_event(role, item["line"])
                if link_event:
                    self.radio_link_events[role] = (
                        link_event, event.host_monotonic_ns)
                if (role == "S3" and event.fields.get("event") == "runtime" and
                        event.fields.get("input_backend") in ("disabled", "configured") and
                        event.fields.get("input_validity") in
                        ("unknown", "valid", "invalid") and
                        event.fields.get("selector_validity") in
                        ("unknown", "valid", "invalid") and
                        event.fields.get("axis") in
                        ("OFF", "X", "Y", "Z", "A", "invalid",
                         "unknown", "unavailable") and
                        event.fields.get("selector_position") in
                        ("OFF", "X", "Y", "Z", "4", "5", "6", "invalid",
                         "unknown", "unavailable") and
                        event.fields.get("increment") in
                        ("X1", "X10", "X100", "invalid", "unknown",
                         "unavailable") and
                        event.fields.get("deadman") in
                        ("active", "inactive", "unknown", "unavailable")):
                    self.s3_input_status = (
                        event.fields["input_backend"],
                        event.fields["input_validity"],
                        event.fields["selector_validity"],
                        event.fields["axis"],
                        event.fields["selector_position"],
                        event.fields["increment"],
                        event.fields["deadman"],
                    )
                    self.s3_input_status_host_ns = event.host_monotonic_ns
                touch_fields = (
                    "touch_polls", "touch_active_samples", "touch_i2c_reads",
                    "touch_i2c_busy", "touch_irq_idle", "touch_errors",
                )
                if role == "S3" and event.fields.get("event") == "runtime" and all(
                        field in event.fields and event.fields[field].isdigit()
                        for field in touch_fields):
                    self.s3_touch_status = tuple(
                        event.fields[field] for field in touch_fields)
                    self.s3_touch_status_host_ns = event.host_monotonic_ns
                loop_fields = (
                    "loop_work_ms", "loop_overruns", "max_input_ms",
                    "max_touch_ms", "max_transport_ms", "max_wifi_ms",
                    "max_display_ms", "max_mirror_ms",
                )
                if role == "S3" and event.fields.get("event") == "runtime" and all(
                        field in event.fields and event.fields[field].isdigit()
                        for field in loop_fields):
                    maxima = tuple(event.fields[field] for field in loop_fields[2:])
                    total_fields = (
                        "total_input_ms", "total_touch_ms",
                        "total_transport_ms", "total_wifi_ms",
                        "total_display_ms", "total_mirror_ms",
                    )
                    profile_loops = event.fields.get("profile_loops", "0")
                    max_loop = event.fields.get("max_loop_work_ms", "0")
                    has_totals = (
                        profile_loops.isdigit() and int(profile_loops) > 0 and
                        max_loop.isdigit() and all(
                            field in event.fields and event.fields[field].isdigit()
                            for field in total_fields))
                    totals = (tuple(event.fields[field] for field in total_fields)
                              if has_totals else None)
                    self.s3_loop_status = (
                        event.fields["loop_work_ms"],
                        event.fields["loop_overruns"], maxima,
                        max_loop, totals,
                        profile_loops if has_totals else "0",
                    )
                    self.s3_loop_status_host_ns = event.host_monotonic_ns
                if event.category == "radio":
                    self.radio_log_lines[role] += 1
                elif event.category in ("error", "safety"):
                    self.errors[role] += 1
                if role == "C3":
                    status = extract_grbl_state(item["line"])
                    if status:
                        self.grbl_status = status
                        self.grbl_status_host_ns = event.host_monotonic_ns
                        self.grbl_telemetry = extract_grbl_telemetry(item["line"])
                self._add_event(event)
        self._rendering_batch = False
        self._flush_render_buffer()
        if self._mpg_trace_dirty:
            self._refresh_mpg_trace()

    def _add_event(self, event: Event) -> None:
        self._track_mpg_correlation(event)
        self.recent.append(event)
        if self.recorder:
            self.recorder.write(event)
        if self.paused:
            self.paused_events.append(event)
            return
        self._render_event(event)

    def _track_mpg_correlation(self, event: Event) -> None:
        event_type = event.fields.get("event")
        bit = 0
        if (event.source == "S3" and event_type == "espnow_tx" and
                event.fields.get("packet_type") == "mpg_jog"):
            self.mpg_s3_submissions += 1
            bit = 1
        elif (event.source == "C3" and event_type == "dry_run" and
              event.fields.get("result") == "proposed"):
            self.mpg_c3_proposals += 1
            bit = 2
        key = event.correlation_key
        if bit and key is not None:
            old = self._mpg_correlations.get(key, 0)
            if not old:
                if len(self._mpg_correlation_order) >= 2000:
                    expired = self._mpg_correlation_order.popleft()
                    self._mpg_correlations.pop(expired, None)
                self._mpg_correlation_order.append(key)
            new = old | bit
            self._mpg_correlations[key] = new
            if old != 3 and new == 3:
                self.mpg_matched += 1
        if bit and old != new:
            self._mpg_trace_dirty = True
            if not self._rendering_batch:
                self._refresh_mpg_trace()

    def _refresh_mpg_trace(self) -> None:
        self.query_one("#mpg-trace", Static).update(
            "MPG observed: S3 submissions={} | C3 dry-run proposals={} | "
            "matched session/seq={}".format(
                self.mpg_s3_submissions, self.mpg_c3_proposals,
                self.mpg_matched))
        self._mpg_trace_dirty = False

    def _flush_render_buffer(self) -> None:
        for key, selector in (("S3", "#s3-log"), ("C3", "#c3-log"),
                              ("timeline", "#timeline")):
            lines = self._render_buffer[key]
            if lines:
                self.query_one(selector, RichLog).write("\n".join(lines))
                lines.clear()

    def _render_many(self, events) -> None:
        was_batching = self._rendering_batch
        self._rendering_batch = True
        for event in events:
            self._render_event(event)
        self._rendering_batch = was_batching
        if not was_batching:
            self._flush_render_buffer()

    def _render_event(self, event: Event) -> None:
        term = self.filter_text
        command = proposed_dry_run_command(event)
        proposal = (f" | proposed GRBL (dry-run): {command}"
                    if command is not None else "")
        searchable = event.message.lower() + proposal.lower()
        if term and term not in searchable and term not in event.category.lower():
            return
        identity = (f" [session/seq {event.correlation_key}]"
                    if event.correlation_key else "")
        if event.source in ("S3", "C3"):
            panel_line = (
                f"{event.host_time} {event.category.upper():<10}{identity} "
                f"{event.message}{proposal}")
            if self._rendering_batch:
                self._render_buffer[event.source].append(panel_line)
            else:
                self.query_one(f"#{event.source.lower()}-log", RichLog).write(panel_line)
        timeline_line = (
            f"{event.host_time} {event.source:<2} {event.category.upper():<10}"
            f"{identity} {event.message}{proposal}")
        if self._rendering_batch:
            self._render_buffer["timeline"].append(timeline_line)
        else:
            self.query_one("#timeline", RichLog).write(timeline_line)

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "filter":
            self.filter_text = event.value.lower().strip()
            if self._filter_timer is not None:
                self._filter_timer.stop()
            self._filter_timer = self.set_timer(0.12, self._apply_filter)

    def _apply_filter(self) -> None:
        self.query_one("#s3-log", RichLog).clear()
        self.query_one("#c3-log", RichLog).clear()
        self.query_one("#timeline", RichLog).clear()
        self._render_many(self.recent)

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
            pending, self.paused_events = self.paused_events, deque(maxlen=2000)
            self._render_many(pending)
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
