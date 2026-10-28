"""Stable-ish serial discovery helpers. Ambiguous ports are intentionally not guessed."""

from __future__ import annotations

from dataclasses import dataclass
from serial.tools import list_ports


@dataclass(frozen=True)
class Device:
    device: str
    description: str
    manufacturer: str | None
    serial_number: str | None
    vid: int | None
    pid: int | None
    hwid: str

    @property
    def stable_id(self) -> str:
        identity = self.serial_number or self.hwid or self.device
        return f"{self.vid:04x}:{self.pid:04x}:{identity}" if self.vid is not None and self.pid is not None else f"{self.device}:{identity}"

    @property
    def search_text(self) -> str:
        return " ".join((self.device, self.description, self.manufacturer or "", self.serial_number or "", self.hwid)).lower()


def discover_devices() -> list[Device]:
    return [Device(p.device, p.description or "", p.manufacturer, p.serial_number,
                   p.vid, p.pid, p.hwid or "") for p in list_ports.comports()]


def choose_device(devices: list[Device], role: str) -> tuple[Device | None, str]:
    """Pick only clear role matches; never assign the same port to both devices."""
    role = role.lower()
    hints = ("circuitpython", "adafruit", "8248583b090f") if role == "s3" else ("esp32-c3", "esp32c3", "serial/jtag")
    matches = [d for d in devices if any(h in d.search_text for h in hints)]
    if len(matches) == 1:
        return matches[0], "matched descriptor/identity"
    if not matches:
        return None, "no unambiguous role match; select a port explicitly"
    return None, f"{len(matches)} matching ports; select one explicitly"
