"""Serial line parsing and host-side event correlation primitives."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import re
import time
from typing import Any

_FIELD = re.compile(r"\b([a-z][a-z0-9_]*)=([^\s,]+)", re.I)
_FIRMWARE_TICK = re.compile(r"\b[EWIDV] \((\d+)\)")


@dataclass(frozen=True)
class Event:
    source: str
    category: str
    message: str
    host_monotonic_ns: int
    host_time: str
    fields: dict[str, str]

    @classmethod
    def from_line(cls, source: str, line: str, *, monotonic_ns: int | None = None,
                  wall_time: datetime | None = None) -> "Event":
        mono = time.monotonic_ns() if monotonic_ns is None else monotonic_ns
        now = datetime.now(timezone.utc) if wall_time is None else wall_time
        fields = {key.lower(): value for key, value in _FIELD.findall(line)}
        tick = _FIRMWARE_TICK.search(line)
        if tick:
            fields["firmware_tick_ms"] = tick.group(1)
        return cls(source=source, category=classify_line(line), message=line.rstrip("\r\n"),
                   host_monotonic_ns=mono, host_time=now.isoformat(timespec="milliseconds"),
                   fields=fields)

    @property
    def correlation_key(self) -> str | None:
        """Normalize a packet identity only when both fields are observed."""
        session = self.fields.get("session")
        sequence = self.fields.get("seq")
        if session is None or sequence is None:
            return None
        try:
            session_number = int(session, 0)
        except ValueError:
            try:
                session_number = int(session, 10)
            except ValueError:
                return None
        try:
            sequence_number = int(sequence, 0)
        except ValueError:
            try:
                sequence_number = int(sequence, 10)
            except ValueError:
                return None
        if not (1 <= session_number <= 0xFFFFFFFF and
                0 <= sequence_number <= 0xFFFF):
            return None
        return f"{session_number}:{sequence_number}"

    def to_json(self) -> dict[str, Any]:
        result = asdict(self)
        result["correlation_key"] = self.correlation_key
        return result


def classify_line(line: str) -> str:
    """Classify only what a log line says; never infer missing device telemetry."""
    text = line.lower()
    if ("event=motion_admission" in text or "event=input_rejected" in text or
            "event=encoder_overrun" in text or
            ("event=input_validity" in text and "valid=false" in text)):
        return "safety"
    if "dry_run" in text or "dry-run" in text:
        return "dry-run"
    if "mpg" in text and any(word in text for word in ("reject", "denied", "inhibit", "expired")):
        return "safety"
    if "mpg" in text and any(word in text for word in ("accepted", "proposed")):
        return "input"
    if any(word in text for word in ("esp-now", "espnow", "heartbeat", "pair", "session", "peer")):
        return "radio"
    if any(word in text for word in ("uart", "grbl", "controller")):
        return "controller"
    if any(word in text for word in ("touch", "gesture", "encoder", "selector", "input")):
        return "input"
    if any(word in text for word in ("error", "failed", "fault", "timeout", "rejected")):
        return "error"
    if "[boot]" in text or "[ready]" in text:
        return "lifecycle"
    return "log"


def extract_grbl_state(line: str) -> str | None:
    """Extract a state only from the C3's explicit validated status event."""
    match = re.search(r"event=grbl_status[^\n]*line=<([^|>]+)\|[^>]*>", line)
    return match.group(1) if match else None


def is_fresh(timestamp_ns: int | None, now_ns: int | None = None,
             max_age_ns: int = 1_500_000_000) -> bool:
    if timestamp_ns is None:
        return False
    now = time.monotonic_ns() if now_ns is None else now_ns
    age = now - timestamp_ns
    return 0 <= age <= max_age_ns
