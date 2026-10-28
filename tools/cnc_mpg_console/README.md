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

Controls: `Record` starts a JSONL session, `Pause` pauses panel rendering while retaining events, `Filter` matches visible lines, `Export JSONL` exports the recent event cache, and `Reconnect` closes/reopens the existing readers. The C3 log parser classifies observed text; it does not claim an event or machine state unless the firmware printed it. Host timestamps are the common ordering reference; firmware timestamps, session IDs, peers, and sequence numbers are preserved when present. For MPG traffic, the timeline annotates events with normalized `session/seq` when both are present. Its trace summary counts S3 driver-accepted submissions, C3 dry-run proposals, and exact identity matches even when USB readers deliver the lines in opposite order. A match proves the same identity appeared in both logs; it does not turn the S3 driver's send result into an OTA acknowledgment or imply that GRBL executed a command.

## C3 console and controller UART

The source currently defines controller UART as UART0, GPIO21 TX and GPIO20 RX, at 115200 baud (`include/config.h`). The repository did not previously track `sdkconfig`, so the active console route of an already flashed C3 could not be proven from source alone. This branch adds `sdkconfig.defaults` selecting ESP32-C3 USB Serial/JTAG for ESP-IDF logs while retaining the controller's UART0 pins. A fresh build therefore gives diagnostics an independent USB interface. This build configuration does not establish what is currently flashed; validate the build image/config before flashing. Espressif documents the USB console option at <https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32c3/api-guides/usb-serial-jtag-console.html>.

GRBL traffic does not appear on USB automatically. This branch adds rate-limited `event=grbl_status` logs for validated status responses (state changes immediately, otherwise at most once per second), sampled `event=uart_tx operation=status_query` diagnostics, and metadata-only MPG jog/stop TX logs. The raw serial bridge carries arbitrary legacy bytes, so this dashboard does not log or replay arbitrary controller-bound command payloads. The status line includes the GRBL status payload, but no arbitrary G-code TX content is printed. These logs are not proof of physical execution; a successful UART write only means the ESP-IDF driver accepted bytes. `GRBL` status in the dashboard is fresh for 1.5 seconds after the last explicit validated status event, then shown as stale.

The S3 CircuitPython console is the CDC device at `/dev/cu.usbmodem8248583B090F1` for UID `8248583B090F`; the application does not disable `usb_cdc.console` or redirect stdout. Its earlier dashboard session attached after boot, while the firmware printed only boot/subsystem messages and had no periodic runtime/input diagnostics, so the USB connection succeeded but no new lines arrived. Boot output is not replayed when a monitor attaches late. The companion S3 change adds low-rate runtime status and event lines for normalized encoder changes, selector/button/E-stop/deadman state, touch gestures/errors, display render counts, Wi-Fi/controller transitions, and ESP-NOW packet/session/sequence activity. These logs are written by CircuitPython `print()` to USB CDC. The status bar reports backend/input/selector health only from the S3's explicit 10-second runtime event; it shows `unreported` before one arrives and `stale` after 30 seconds without a fresh report. Disconnected selector/encoder wiring still yields no physical events; the console reports configured backend and detected MCP23017 state but does not invent values.

The C3 USB diagnostic device is `/dev/cu.usbmodem101` on this Mac (USB Serial/JTAG, VID:PID `303a:1001`, serial `34:85:18:03:42:34`). Its Doesbot connection is a separate UART0 at GPIO21 TX/GPIO20 RX, 115200 baud. The repo now emits a boot line identifying these endpoints, keeps core ESP-NOW/GRBL/MPG/UART diagnostics visible even if NVS `debug_level` is zero, and aggregates UART byte counts without exposing arbitrary command payloads. Validated GRBL status reports, motion admission outcomes/reasons, pairing/session/heartbeat packets, and UART metadata carry packet/session identifiers where available. `event=uart_tx` for a jog indicates a hardware write attempt; it is not presented as dry-run or proof that the controller acted. The actual route in an already flashed image remains unverified until boot output is captured from that image; `sdkconfig.defaults` selects USB Serial/JTAG on a fresh build.

## Tests

```sh
python -m pip install -e '.[test]'
python -m pytest
```

Tests cover observed-line classification, session/sequence normalization and cross-device MPG correlation, identity ambiguity, passive serial receive, reconnection, JSONL recording/export, and prevention of C3 REPL writes. They do not validate physical input movement, ESP-NOW over the air, GRBL telemetry on the current image, or console routing in an already flashed firmware image.
