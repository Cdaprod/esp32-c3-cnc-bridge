import os
import pty
import queue
import time

from cnc_mpg_console.serial_reader import SerialReader


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
