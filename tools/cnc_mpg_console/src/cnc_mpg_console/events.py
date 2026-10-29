"""Serial line parsing and host-side event correlation primitives."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import math
import re
import time
from typing import Any

_FIELD = re.compile(r"\b([a-z][a-z0-9_]*)=([^\s,]+)", re.I)
_FIRMWARE_TICK = re.compile(r"\b[EWIDV] \((\d+)\)")
_ESP_IDF_LOG_LEVEL = re.compile(r"^\s*([EWIDV]) \(\d+\)")
_SENSITIVE_VALUE = re.compile(
    r"(?i)([\"']?\b(?:password|passwd|pwd|token|secret|api[_-]?key)\b"
    r"[\"']?\s*[:=]\s*[\"']?)([^\s,;\"'}]+)([\"']?)"
)


def redact_sensitive_text(text: str) -> str:
    """Mask credential-like values before display, parsing, or recording."""
    return _SENSITIVE_VALUE.sub(r"\1[REDACTED]\3", str(text))


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
        safe_line = redact_sensitive_text(line)
        fields = {key.lower(): value for key, value in _FIELD.findall(safe_line)}
        tick = _FIRMWARE_TICK.search(safe_line)
        if tick:
            fields["firmware_tick_ms"] = tick.group(1)
        return cls(source=source, category=classify_line(safe_line),
                   message=safe_line.rstrip("\r\n"),
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
    # CircuitPython uses ReloadException as ordinary control flow for a
    # requested soft reload. Keep it visible as lifecycle activity without
    # inflating the dashboard's error count. A separately reported application
    # exception is a real fault and must remain prominent.
    if "event=application_reload" in text:
        return "lifecycle"
    if "event=startup_context" in text:
        return "lifecycle"
    if "event=application_exception" in text:
        return "error"
    if re.search(r"\bevent=mpg_stop\b", text):
        if "result=suppressed" in text or "result=failed" in text:
            return "safety"
        return "controller"
    if re.search(r"\bevent=mpg_feed_hold\b", text):
        if "result=rejected" in text:
            return "safety"
        if "result=dry_run_proposed" in text or "physical_uart=not_sent" in text:
            return "dry-run"
        return "controller"
    if re.search(r"\bevent=motion_rearm\b", text):
        return "controller"
    if "event=grbl_jog_ack" in text:
        return "controller" if "result=accepted" in text else "safety"
    if "event=motion_admission" in text:
        if "result=rejected" in text:
            return "safety"
        if "result=accepted" in text and "mode=dry_run" in text:
            return "dry-run"
        if "result=accepted" in text and "mode=uart" in text:
            return "motion"
        return "safety"
    if ("event=uart_tx_rejected" in text or
            "event=input_rejected" in text or
            "event=uart_tx operation=mpg_stop_cancel" in text or
            "event=encoder_overrun" in text or
            ("event=input_validity" in text and "valid=false" in text)):
        return "safety"
    if re.search(r"\bevent=uart_tx\b", text):
        return "controller"
    if re.search(r"\bevent=dry_run\b", text):
        return "dry-run" if "result=proposed" in text else "log"
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
    # ESP-IDF diagnostic payloads often contain words such as "timeout" in
    # field names (for example bcn_timeout). Preserve the domain classifications
    # above, then trust firmware severity instead of incidental substrings.
    level = _ESP_IDF_LOG_LEVEL.match(line)
    if level:
        if level.group(1) == "E":
            return "error"
        if level.group(1) == "W":
            return "warning"
        return "log"
    if any(word in text for word in ("error", "failed", "fault", "timeout", "rejected")):
        return "error"
    if "[boot]" in text or "[ready]" in text:
        return "lifecycle"
    return "log"


def extract_grbl_state(line: str) -> str | None:
    """Extract a state only from the C3's explicit validated status event."""
    match = re.search(r"event=grbl_status[^\n]*line=<([^|>]+)\|[^>]*>", line)
    return match.group(1) if match else None


def extract_grbl_telemetry(line: str) -> dict[str, str] | None:
    """Parse only finite values carried by an explicit C3 GRBL status event."""
    match = re.search(r"\bevent=grbl_status\b[^\n]*\bline=(<[^>]*>)", line)
    if not match:
        return None
    report = match.group(1)
    parts = report[1:-1].split("|")
    if not parts or not parts[0]:
        return None
    result = {"state": parts[0].split(":", 1)[0]}
    axes = ("X", "Y", "Z", "A")
    for field in parts[1:]:
        if ":" not in field:
            continue
        name, value = field.split(":", 1)
        if name in ("MPos", "WPos"):
            raw_values = value.split(",")
            if len(raw_values) not in (3, 4):
                continue
            try:
                numbers = [float(item) for item in raw_values]
            except ValueError:
                continue
            if not all(math.isfinite(item) for item in numbers):
                continue
            result[name.lower()] = " ".join(
                f"{axis}={number:.3f}"
                for axis, number in zip(axes, numbers)
            )
        elif name == "FS":
            raw_values = value.split(",")
            if len(raw_values) != 2:
                continue
            try:
                feed, spindle = (float(item) for item in raw_values)
            except ValueError:
                continue
            if math.isfinite(feed) and math.isfinite(spindle):
                result["feed"] = f"{feed:.1f}"
                result["spindle"] = f"{spindle:.1f}"
        elif name == "F" and "feed" not in result:
            try:
                feed = float(value)
            except ValueError:
                continue
            if math.isfinite(feed):
                result["feed"] = f"{feed:.1f}"
    return result


def extract_radio_link_event(source: str, line: str) -> dict[str, str] | None:
    """Return only explicit pairing/loss transitions present in device logs.

    The C3 currently logs pairing and timeout transitions in its native
    ESP-IDF messages rather than the S3's structured event format. These are
    historical observations, not a live-link assertion.
    """
    if source == "S3":
        match = re.search(
            r"\bevent=espnow_link\b[^\n]*\bstate=(paired|lost)\b"
            r"(?:[^\n]*\bsession=([^\s]+))?",
            line,
            re.IGNORECASE,
        )
        if match:
            result = {"state": match.group(1).lower()}
            if match.group(2):
                result["session"] = match.group(2)
            return result
    elif source == "C3":
        match = re.search(
            r"\bPAIR v\d+ accettato da ([0-9A-Fa-f:]+)\s+session=(0x[0-9A-Fa-f]+)",
            line,
        )
        if match:
            return {"state": "pair_accepted", "peer": match.group(1),
                    "session": match.group(2)}
        match = re.search(
            r"\bPeer ([0-9A-Fa-f:]+) rimosso per timeout\b", line
        )
        if match:
            return {"state": "peer_removed", "peer": match.group(1),
                    "reason": "timeout"}
    return None


def proposed_dry_run_command(event: Event) -> str | None:
    """Decode a C3 proposal for display after checking its observed metadata.

    This is deliberately limited to the production dry-run event shape. The
    returned string is presentation data only; this module has no transport.
    """
    fields = event.fields
    if (event.source != "C3" or fields.get("event") != "dry_run" or
            fields.get("result") != "proposed"):
        return None
    try:
        axis = fields["axis"]
        microns = int(fields["microns"], 10)
        feed = int(fields["feed"], 10)
        encoded = fields["command_hex"]
    except (KeyError, ValueError):
        return None
    if (axis not in ("X", "Y", "Z", "A") or
            not 1 <= abs(microns) <= 1000 or not 1 <= feed <= 1000 or
            len(encoded) > 128 or
            re.fullmatch(r"(?:[0-9a-fA-F]{2})+", encoded) is None):
        return None
    try:
        raw = bytes.fromhex(encoded)
    except ValueError:
        return None
    sign = "-" if microns < 0 else ""
    magnitude = abs(microns)
    expected = ("$J=G91 G21 {}{}{}.{:03d} F{}\n".format(
        axis, sign, magnitude // 1000, magnitude % 1000, feed
    )).encode("ascii")
    if raw != expected:
        return None
    return expected[:-1].decode("ascii")


def is_fresh(timestamp_ns: int | None, now_ns: int | None = None,
             max_age_ns: int = 1_500_000_000) -> bool:
    if timestamp_ns is None:
        return False
    now = time.monotonic_ns() if now_ns is None else now_ns
    age = now - timestamp_ns
    return 0 <= age <= max_age_ns
