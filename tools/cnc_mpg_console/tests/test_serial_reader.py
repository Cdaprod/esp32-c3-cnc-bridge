import os
import pty
import queue
import time
from unittest.mock import patch

from cnc_mpg_console.serial_reader import (
    MAX_SERIAL_READ_BYTES, SERIAL_READ_TIMEOUT_S, SerialReader, dtr_for_role,
)


def test_dtr_enables_circuitpython_console_without_toggling_c3_controls():
    assert dtr_for_role("S3")
    assert dtr_for_role("s3")
    assert not dtr_for_role("C3")


def test_reader_receives_lines_passively_and_stops_cleanly():
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    bus = queue.Queue(maxsize=100)
    reader = SerialReader("C3", port, 115200, bus)
    try:
        reader.start()
        deadline = time.monotonic() + 2
        while not reader.connected.is_set() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert reader.connected.is_set()
        os.write(master, b"I (10) MPG: DRY_RUN seq=7 session=3\r\n")
        deadline = time.monotonic() + 2
        found = None
        while time.monotonic() < deadline:
            try:
                item = bus.get(timeout=0.1)
            except queue.Empty:
                continue
            if item.get("kind") == "line":
                found = item["line"]
                break
        assert found == "I (10) MPG: DRY_RUN seq=7 session=3"
        assert reader.write_queue.empty()  # passive mode never writes
        reader.reconnect()
        deadline = time.monotonic() + 2
        while reader.reconnects < 2 and time.monotonic() < deadline:
            time.sleep(0.01)
        assert reader.reconnects >= 2
    finally:
        reader.stop()
        reader.join(timeout=2)
        os.close(master)
        os.close(slave)
    assert not reader.is_alive()


def test_c3_never_accepts_repl_writes():
    reader = SerialReader("C3", "/dev/null", 115200, queue.Queue(), allow_s3_repl=True)
    assert not reader.allow_s3_repl
    assert not reader.send_repl_line("anything")


def test_reader_uses_short_idle_wait_and_drains_buffered_bytes():
    class Stream:
        def __init__(self):
            self.read_sizes = []
            self.in_waiting = MAX_SERIAL_READ_BYTES + 20
            self.port = None
            self.dtr = False
            self.rts = False

        def open(self):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            pass

        def read(self, size):
            self.read_sizes.append(size)
            self.in_waiting = 0
            time.sleep(0.001)
            return b""

        def write(self, _data):
            pass

    stream = Stream()
    bus = queue.Queue(maxsize=100)
    reader = SerialReader("C3", "/dev/fake", 115200, bus)
    try:
        with patch("cnc_mpg_console.serial_reader.serial.Serial",
                   return_value=stream) as serial_factory:
            reader.start()
            deadline = time.monotonic() + 1
            while not reader.connected.is_set() and time.monotonic() < deadline:
                time.sleep(0.001)
            assert reader.connected.is_set()
            reader.stop()
            reader.join(timeout=1)

        assert not reader.is_alive()
        assert serial_factory.call_args.kwargs["timeout"] == SERIAL_READ_TIMEOUT_S
        assert SERIAL_READ_TIMEOUT_S <= 0.01
        assert stream.read_sizes
        assert stream.read_sizes[0] == MAX_SERIAL_READ_BYTES
        assert set(stream.read_sizes[1:]) <= {1}
        assert MAX_SERIAL_READ_BYTES == 512
    finally:
        reader.stop()
        if reader.is_alive():
            reader.join(timeout=1)
