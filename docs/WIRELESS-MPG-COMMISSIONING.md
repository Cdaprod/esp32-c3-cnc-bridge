# Wireless MPG commissioning (ESP32-C3 receiver)

Companion pendant implementation: [pendant PR #22](https://github.com/Cdaprod/lolin-s2-mini-cnc-mpg-pendant-controller/pull/22).

## What is implemented
- ESP-NOW v1 pairing and sequenced binary `MPG_JOG` / `MPG_STOP` with one-byte ACKs.
- `MPG_STOP_ACK` result `0` means output failed, `1` means GRBL's realtime
  cancel byte was written, and `2` means the stop was consumed and the MPG
  lease revoked while physical UART output was suppressed by a disarmed/dry-run
  build. Result `2` does not prove the machine stopped. The packet shape and
  protocol version are unchanged; older S3 firmware treats result `2` as
  unconfirmed and remains inhibited.
- C3 serial diagnostics emit `event=mpg_stop` with session and sequence for a
  sent `0x85`, an explicitly suppressed output (`physical_uart=not_sent`), or
  a failed write. The dashboard classifies suppression and failure as safety
  events; a suppressed ACK only releases the S3's local stop latch so a
  non-actuating diagnostic session can continue. Normal controller freshness,
  deadman, selector, and C3 admission checks still gate every subsequent jog.
- Dedicated 8-byte jog payload: ASCII X/Y/Z/A, deadman flag, signed int32 microns LE, uint16 feed mm/min LE. The S3 manufacturer map labels selector contact `4` as the 4th/A axis; positions `5` and `6` remain configured as UI functions on the current S3 profile.
- GRBL 1.1 status query `?` every 250 ms, strict full-line parser, and <250 ms freshness gate before any jog.
- Native C receiver ownership lease with 250 ms expiry, 25 ms watchdog tick and GRBL real-time jog cancel `0x85`. MPG sequence state is bound to the paired sender MAC and active session; duplicate and stale jog packets remain rejected after the lease is revoked or expires.
- Unrestricted v1/legacy ESP-NOW raw UART forwarding disabled.
- New radio peers receive framed UART_RESPONSE chunks; old clients continue receiving raw responses.
- The XIAO ESP32-C3 diagnostics console is explicitly configured for its native USB Serial/JTAG interface. The Doesbot 4-axis controller transport remains UART0 on C3 GPIO21 (TX) and GPIO20 (RX); per the documented harness, those signals pass through the 5 V↔3.3 V level shifter. This keeps the USB TUI stream separate from controller traffic. The repository's reproducible build sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` rather than relying on an ignored local `sdkconfig`.
- **Motion OFF by default** via ESP-IDF `CONFIG_MPG_ENABLE_MOTION=n`. `CONFIG_MPG_DRY_RUN=y` is independent and default-on: with a locally provisioned unicast peer MAC, production packet parsing, session/sequence checks, GRBL status admission and `$J` formatting run while jog commands are logged without calling the UART writer. The normal motion-disabled configuration therefore supports end-to-end dry-run diagnostics without enabling physical jog output. A dry-run ACK confirms only that the intent passed those checks and was logged; no jog command reached GRBL. Host tests verify the dry-run output cannot fall through to UART when logging fails or is unavailable. The firmware also defaults to dry-run when the configuration macro is absent. No physical commissioning configuration is part of this change.

For a reproducible disarmed dry-run build, keep `CONFIG_MPG_ENABLE_MOTION=n`, keep `CONFIG_MPG_DRY_RUN=y`, and enter the approved unicast Wi-Fi MAC (`AA:BB:CC:DD:EE:FF`). A fresh GRBL status stream is still required for controller-state admission. Invalid/broadcast/multicast MACs deny admission. This is *not* permission to operate before physical commissioning, UART arbitration review and an independently functioning machine safety circuit.

## Important limitations before activating physical movement
1. ESP-NOW sender MAC allowlisting is not cryptographic authentication. Pairing by itself is insufficient. Provision keys / encrypted peers and an operator arming control for production.
2. This bridge also exposes a Telnet/UART path. The MPG follow-up adds an atomic UART ownership arbiter: queued bridge/TCP bytes reserve the normal writer through TX completion, an MPG lease can claim only after that queue drains, and normal TX is rejected while MPG owns the UART. A newly connected TCP client triggers the existing watchdog cancellation/release path. This prevents C3-managed senders from interleaving; it cannot arbitrate an independent device wired directly to GRBL.
3. A successful `UART_ACK` or `MPG_JOG_ACK` means UART enqueue/write accepted; it does *not* prove that GRBL executed or completed motion. Verify GRBL responses and current positions.
4. GRBL real-time jog cancel is **not** a safety-rated emergency stop and is not a guarantee of IEC stop-category compliance. A hardwired independent emergency-stop circuit remains necessary.
5. The user-specified Doesbot connection uses the 5 V↔3.3 V level shifter. Pin-level continuity and signal levels have not been independently measured in this session. Never connect the Doesbot's `V` (+5 V) directly to an ESP32 GPIO.
6. The current sender MAC and firmware settings do not support safe multi-machine auto-arming. Require explicit disarmed machine selection.
7. ESP-NOW channel must match actual Wi-Fi STA/AP channel, not merely a protocol constant.
8. Do not expose local Wi-Fi credentials or `include/MyWiFiData.h` changes in commits.
9. The pendant maps Doesbot selector position 4 to logical axis `A`, and the
   bridge can format an `A` dry-run proposal. That does **not** establish that
   the installed controller accepts A-axis jogging: upstream GRBL 1.1 documents
   `$J=` axis words as XYZ and its parser handles X/Y/Z words only. An OEM
   Doesbot firmware may extend this, but that capability is unverified. The
   vendor's published generic GRBL 1.1 settings page lists XYZ steps/mm, rates,
   acceleration, and travel fields; it does not specify the fourth-axis
   command or telemetry contract for this controller revision. Keep A-axis
   output in disarmed dry-run until the exact installed firmware or a matching
   vendor specification confirms A support; do not infer support from the
   4-axis harness or from the C3 formatter. See the [GRBL 1.1 jog command
   specification](https://github.com/gnea/grbl/blob/master/doc/markdown/commands.md#jogging),
   [upstream G-code axis parser](https://github.com/gnea/grbl/blob/master/grbl/gcode.c),
   and [Doesbot's published GRBL 1.1 settings](https://www.doesbot.com/2025/11/21/grbl-v1-1-configuration/).
   The C3 enforces this boundary: dry-run may report an A-axis proposal, but
   physical A-axis output requires the explicit
   `CONFIG_MPG_ALLOW_A_AXIS_MOTION=y` Kconfig opt-in. It is off by default and
   depends on motion being enabled.

## Bench verification order
- First run CI host unit tests and full ESP-IDF build with motion disabled.
- Run the isolated host MPG tests with the CI dry-run configuration; confirm accepted/rejected intents and inspect the formatted command log while the fake UART writer remains untouched.
- Flash C3 without attaching CNC motors; check serial boot and ESP-NOW channel.
- Inspect paired radio session and confirm status reports are truly from a connected GRBL UART, not mere radio pairing.
- Verify `MPG_JOG_ACK=ERROR` by default and test `MPG_STOP` UART 0x85 on a logic analyzer.
- Rehearse radio loss, deadman release, stale status, duplicate/wrapped sequences, rapid multiplier changes, and machine alarms using a serial GRBL simulator.
- Only after independent E-stop and operator controls are verified, test very small incremental jogging with motors energized in a clear workspace.
