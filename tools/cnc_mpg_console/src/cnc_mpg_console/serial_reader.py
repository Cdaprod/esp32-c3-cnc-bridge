"""One passive, exclusive serial owner per configured device."""

from __future__ import annotations

import queue
import threading
import time
from datetime import datetime, timezone
from typing import Callable

import serial
from .devices import choose_device, discover_devices

MAX_SERIAL_LINE_BYTES = 8192


class SerialReader(threading.Thread):
    def __init__(self, source: str, port: str | None, baud: int,
                 output: queue.Queue, *, allow_s3_repl: bool = False,
                 auto_role: str | None = None):
        super().__init__(name=f"serial-{source.lower()}", daemon=True)
        self.source, self.port, self.baud, self.output = source, port, baud, output
        self.allow_s3_repl = allow_s3_repl and source.upper() == "S3"
        self.auto_role = auto_role
        self.stop_event = threading.Event()
        self.reconnect_event = threading.Event()
        self.write_queue: queue.Queue[bytes] = queue.Queue()
        self.connected = threading.Event()
        self.error: str | None = None
        self.lines = 0
        self.reconnects = 0
        self.dropped = 0

    def send_repl_line(self, line: str) -> bool:
        if not self.allow_s3_repl or not self.connected.is_set():
            return False
        self.write_queue.put(line.encode("utf-8") + b"\r\n")
        return True

    def _publish(self, kind: str, **payload: object) -> None:
        payload.setdefault("host_monotonic_ns", time.monotonic_ns())
        payload.setdefault("host_time", datetime.now(timezone.utc))
        try:
            self.output.put_nowait({"kind": kind, "source": self.source, **payload})
        except queue.Full:
            self.dropped += 1
            try:
                self.output.get_nowait()
                self.output.put_nowait({"kind": "overflow", "source": self.source, "dropped": self.dropped})
            except (queue.Empty, queue.Full):
                pass

    def run(self) -> None:
        while not self.stop_event.is_set():
            if not self.port:
                if self.auto_role:
                    device, _reason = choose_device(discover_devices(), self.auto_role)
                    self.port = device.device if device else None
                self.stop_event.wait(1.0 if self.auto_role else 0.25)
                continue
            try:
                # Keep DTR/RTS low from open to avoid toggling reset/boot straps.
                # exclusive=True prevents a second POSIX process from owning this port.
                stream = serial.Serial(baudrate=self.baud, timeout=0.1,
                                       write_timeout=0.2, exclusive=True,
                                       dsrdtr=False, rtscts=False)
                stream.dtr = False
                stream.rts = False
                stream.port = self.port
                stream.open()
                with stream:
                    self.reconnect_event.clear()
                    self.connected.set()
                    self.error = None
                    self.reconnects += 1
                    self._publish("status", connected=True, port=self.port, reconnects=self.reconnects)
                    pending = bytearray()
                    while not self.stop_event.is_set():
                        if self.reconnect_event.is_set():
                            self.reconnect_event.clear()
                            break
                        try:
                            while True:
                                stream.write(self.write_queue.get_nowait())
                        except queue.Empty:
                            pass
                        chunk = stream.read(512)
                        if not chunk:
                            continue
                        pending.extend(chunk)
                        if len(pending) > MAX_SERIAL_LINE_BYTES and b"\n" not in pending:
                            prefix = pending[:MAX_SERIAL_LINE_BYTES].decode("utf-8", errors="replace")
                            self._publish("line", line=prefix + " … [unterminated line truncated]")
                            pending.clear()
                        while b"\n" in pending:
                            raw, _, rest = pending.partition(b"\n")
                            pending = bytearray(rest)
                            truncated = len(raw) > MAX_SERIAL_LINE_BYTES
                            line = raw[:MAX_SERIAL_LINE_BYTES].decode("utf-8", errors="replace").rstrip("\r")
                            if truncated:
                                line += " … [line truncated]"
                            self.lines += 1
                            self._publish("line", line=line)
                    self.connected.clear()
                    self._publish("status", connected=False, port=self.port)
            except (serial.SerialException, OSError, ValueError) as exc:
                self.connected.clear()
                self.error = str(exc)
                self._publish("status", connected=False, port=self.port, error=self.error)
                if self.auto_role:
                    device, _reason = choose_device(discover_devices(), self.auto_role)
                    self.port = device.device if device else None
                self.stop_event.wait(1.0)

    def stop(self) -> None:
        self.stop_event.set()

    def reconnect(self) -> None:
        self.reconnect_event.set()
