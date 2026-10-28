import json

from cnc_mpg_console.events import Event
from cnc_mpg_console.recording import Recorder


def test_jsonl_recording_preserves_source_ids_and_host_order(tmp_path):
    path = tmp_path / "session.jsonl"
    recorder = Recorder(path)
    recorder.write(Event.from_line("C3", "MPG reject seq=9 session=14", monotonic_ns=100))
    recorder.close()
    record = json.loads(path.read_text().strip())
    assert record["source"] == "C3"
    assert record["host_monotonic_ns"] == 100
    assert record["fields"] == {"seq": "9", "session": "14"}
