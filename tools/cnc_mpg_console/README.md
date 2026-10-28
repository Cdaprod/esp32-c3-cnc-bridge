# CNC MPG Development Console

One Python Textual dashboard owns each configured USB serial connection and passively displays S3 CircuitPython output beside C3 ESP-IDF diagnostics. It merges lines into a host-timestamped event timeline and can record/export JSONL. It does not connect to or transmit on the C3 GRBL UART.

## Install and run

```sh
cd tools/cnc_mpg_console
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e .
python -m cnc_mpg_console.app --list-devices
python -m cnc_mpg_console.app --s3-port /dev/cu.usbmodem8248583B090F1 --c3-port /dev/cu.usbmodem101
```

Use the port paths reported by `--list-devices`. Automatic discovery selects a role only when USB descriptors/serial identity uniquely match. Ambiguous devices remain disconnected until their ports are specified. Defaults are 115200 baud; USB CDC / USB Serial-JTAG transports ignore UART baud rate, while a UART-backed console uses the configured rate. Each reader opens one port exclusively where the OS supports it, with DTR/RTS held low, and automatically retries after disconnect/reset. Do not run `idf.py monitor`, `tio`, REPL tools, or another serial reader against the same device while the dashboard owns it.

The S3 stream is passive by default. `--enable-s3-repl` reveals an input line that sends Python only to the selected S3 REPL. This can interrupt CircuitPython application execution; it is disabled unless explicitly requested. The dashboard never writes to the C3 stream and contains no GRBL command sender. It does not issue motion, spindle, status-query, or realtime bytes to the physical controller.

Controls: `Record` starts a JSONL session, `Pause` pauses panel rendering while retaining events, `Filter` matches visible lines, `Export JSONL` exports the recent event cache, and `Reconnect` closes/reopens the existing readers. The C3 log parser classifies observed text; it does not claim an event or machine state unless the firmware printed it. Host timestamps are the common ordering reference; firmware timestamps, session IDs, peers, and sequence numbers are preserved when present.

## C3 console and controller UART

The source currently defines controller UART as UART0, GPIO21 TX and GPIO20 RX, at 115200 baud (`include/config.h`). The repository did not previously track `sdkconfig`, so the active console route of an already flashed C3 could not be proven from source alone. This branch adds `sdkconfig.defaults` selecting ESP32-C3 USB Serial/JTAG for ESP-IDF logs while retaining the controller's UART0 pins. A fresh build therefore gives diagnostics an independent USB interface. This build configuration does not establish what is currently flashed; validate the build image/config before flashing. Espressif documents the USB console option at <https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32c3/api-guides/usb-serial-jtag-console.html>.

GRBL traffic does not appear on USB automatically. This branch adds rate-limited `event=grbl_status` logs for validated status responses (state changes immediately, otherwise at most once per second), sampled `event=uart_tx operation=status_query` diagnostics, and metadata-only MPG jog/stop TX logs. The raw serial bridge carries arbitrary legacy bytes, so this dashboard does not log or replay arbitrary controller-bound command payloads. The status line includes the GRBL status payload, but no arbitrary G-code TX content is printed. These logs are not proof of physical execution; a successful UART write only means the ESP-IDF driver accepted bytes. `GRBL` status in the dashboard is fresh for 1.5 seconds after the last explicit validated status event, then shown as stale.

Current S3 firmware prints boot/subsystem diagnostics, but does not emit a complete structured stream of touch/encoder/selector/ESP-NOW events. The dashboard displays only lines it receives; those missing events are not synthesized. Physical input event correlation requires a compatible S3 diagnostics build and connected controls. This branch changes C3 logging and default console routing only; it does not alter S3 configuration or deploy firmware.

## Tests

```sh
python -m pip install -e '.[test]'
python -m pytest
```

Tests cover observed-line classification, identity ambiguity, passive serial receive, and prevention of C3 REPL writes. They do not validate a physical device, ESP-NOW over the air, touch behavior, GRBL telemetry, or console routing in an already flashed firmware image.
