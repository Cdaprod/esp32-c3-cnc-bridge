# Wireless MPG commissioning (ESP32-C3 receiver)

Companion pendant implementation: [pendant PR #16](https://github.com/Cdaprod/lolin-s2-mini-cnc-mpg-pendant-controller/pull/16).

## What is implemented
- ESP-NOW v1 pairing and sequenced binary `MPG_JOG` / `MPG_STOP` with one-byte ACKs.
- Dedicated 8-byte jog payload: ASCII X/Y/Z, deadman flag, signed int32 microns LE, uint16 feed mm/min LE.
- GRBL 1.1 status query `?` every 250 ms, strict full-line parser, and <250 ms freshness gate before any jog.
- Native C receiver ownership lease with replay rejection, 250 ms expiry, 25 ms watchdog tick and GRBL real-time jog cancel `0x85`.
- Unrestricted v1/legacy ESP-NOW raw UART forwarding disabled.
- New radio peers receive framed UART_RESPONSE chunks; old clients continue receiving raw responses.
- **Motion OFF by default** via ESP-IDF `CONFIG_MPG_ENABLE_MOTION=n`. A commissioning engineer can use `idf.py menuconfig` → **Wireless CNC MPG receiver** to explicitly enable experimental jogging and enter the approved unicast Wi-Fi MAC (`AA:BB:CC:DD:EE:FF`). Invalid/broadcast/multicast MACs deny motion. This is *not* permission to operate before physical commissioning, UART arbitration review and an independently functioning machine safety circuit.

## Important limitations before activating physical movement
1. ESP-NOW sender MAC allowlisting is not cryptographic authentication. Pairing by itself is insufficient. Provision keys / encrypted peers and an operator arming control for production.
2. This bridge also exposes a Telnet/UART path. The initial MPG motion writer can interleave with other host senders. Machine-control ownership must be exclusive before motion is enabled.
3. A successful `UART_ACK` or `MPG_JOG_ACK` means UART enqueue/write accepted; it does *not* prove that GRBL executed or completed motion. Verify GRBL responses and current positions.
4. GRBL real-time jog cancel is **not** a safety-rated emergency stop and is not a guarantee of IEC stop-category compliance. A hardwired independent emergency-stop circuit remains necessary.
5. XIAO ESP32-C3 physical UART pin accessibility and 3.3-V logic compatibility with the Doesbot's `V/G/R/T` header have **not** been verified. `V` is +5 V; never feed +5 V to a 3.3-V GPIO.
6. The current sender MAC and firmware settings do not support safe multi-machine auto-arming. Require explicit disarmed machine selection.
7. ESP-NOW channel must match actual Wi-Fi STA/AP channel, not merely a protocol constant.
8. Do not expose local Wi-Fi credentials or `include/MyWiFiData.h` changes in commits.

## Bench verification order
- First run CI host unit tests and full ESP-IDF build with motion disabled.
- Flash C3 without attaching CNC motors; check serial boot and ESP-NOW channel.
- Inspect paired radio session and confirm status reports are truly from a connected GRBL UART, not mere radio pairing.
- Verify `MPG_JOG_ACK=ERROR` by default and test `MPG_STOP` UART 0x85 on a logic analyzer.
- Rehearse radio loss, deadman release, stale status, duplicate/wrapped sequences, rapid multiplier changes, and machine alarms using a serial GRBL simulator.
- Only after independent E-stop and operator controls are verified, test very small incremental jogging with motors energized in a clear workspace.
