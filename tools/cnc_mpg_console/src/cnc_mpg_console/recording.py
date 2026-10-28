"""Session JSONL recording for parsed device events and connection changes."""

import json
from pathlib import Path

from .events import Event


class Recorder:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.stream = path.open("a", encoding="utf-8")

    def write(self, event: Event) -> None:
        self.stream.write(json.dumps(event.to_json(), ensure_ascii=False) + "\n")
        self.stream.flush()

    def close(self) -> None:
        self.stream.close()
