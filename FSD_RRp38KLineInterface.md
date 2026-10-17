# FSD: RR P38 K-line Interface

**Functional specification.** Describes what the firmware does, what it must
not do, and how each part is checked. Hardware bring-up steps live in
[docs/HARDWARE.md](docs/HARDWARE.md), the phased plan in
[docs/PLAN.md](docs/PLAN.md), and the reasoning behind decisions in
[docs/SESSION-NOTES.md](docs/SESSION-NOTES.md). This document states the
requirements; those explain them.

| | |
|---|---|
| Firmware | `FIRMWARE_VERSION` in `main/version.h` (0.4.0 at time of writing) |
| Target | ESP32-C6FH4, 4 MB flash |
| Vehicle | Range Rover P38A, 1995–2002, GEMS engine management |
| Status | Passive capture, web HMI and OTA done. Waveform Debug tab added, not yet bench-tested. Awaiting bench checks and the EKA code |

---

## 1. Purpose

The P38A has no CAN bus. Its modules are reached over a single-wire
**ISO 9141-2 K-line at 10400 baud** on pin 7 of the diagnostic socket.

The interface must:

1. Give a correct, spec-timed ISO 9141 transport on the bench and in the car.
2. Listen to the bus passively, and show what is on it.
3. Show the **electrical shape** of the signal, so a missing or broken
   conversation can be traced to the wire, the transceiver, or the protocol.
4. Initialise a module (5-baud or fast init) and scan for addresses, only
   once the owner has deliberately armed transmission.

Protocol decoding (PIDs, parameter meanings) is out of scope until real
captures exist. The project is "capture first, decode second".

## 2. Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-C6FH4. Console is the built-in USB serial port (COM14 on the development PC) |
| Transceiver | Plain **TJA1021** breakout (marking 1021T2C, no MCU). `HOST`/`SLAVE` jumper block, `SLP` on pin 6, `RX`/`TX` on the screw terminal |
| Pins | K-line TX → GPIO18, K-line RX ← GPIO19, transceiver enable (`SLP`) → GPIO20, slow-init bypass MOSFET gate → GPIO14. L-line TX optional, disabled |
| Bypass | 2N7002/BSS138 + 1 kΩ/100 kΩ, needed only for 5-baud init. The TJA1021's 27 ms TxD timeout clears fast init without it |
| Socket | Pin 7 K, pin 15 L, pins 4/5 GND, pin 16 +12 V through an inline fuse |

All pin numbers are set in `menuconfig` (`main/Kconfig.projbuild`).

## 3. Safety requirements

These come first because a malformed or misaddressed request can lock the
BeCM (immobiliser) until the **EKA code** or a reflash recovers it.

| ID | Requirement |
|---|---|
| S1 | The passive build (`CONFIG_RRP38_ALLOW_TX=n`) must not contain any transmit code. It is the build for listening in the car |
| S2 | In the transmit build, transmission needs **both** the compile-time gate and the runtime gate `arm --yes` (console only). The runtime gate is off on every boot |
| S3 | Nothing on the web interface can arm, init, send or scan. The web interface may only start and stop a passive capture and take a waveform sample |
| S4 | Only one task may use the K-line at a time. A bus claim (`kline_claim`) is taken by console commands, web captures and waveform samples. A second claimant is refused, never queued |
| S5 | OTA installs are refused while the bus is claimed, because the reboot at the end would cut a session off mid-frame |
| S6 | The EKA code is recorded outside the vehicle before any transmit test |

## 4. Functional requirements

### 4.1 Transport (`kline.c`)

| ID | Requirement |
|---|---|
| T1 | UART at 10400 baud, 8N1. Half-duplex: every byte written is read back as an echo, and the echo is consumed and verified. A mismatch is an error |
| T2 | ISO 9141 timing constants are defined in `kline.h` (W1–W5, P1–P4). Bench-checked values are the reference, not the forum figures |
| T3 | 5-baud slow init is bit-banged on the TX pin, with glitch-free handover between UART and GPIO. Optional bypass MOSFET via `CONFIG_RRP38_INIT_DRIVER_GPIO` |
| T4 | KWP2000 fast init: wake-up pattern, then StartCommunication, with the W4 window (KW2 → inverted KW2, 25–50 ms) respected |
| T5 | The transceiver enable is driven high only while the bus is in use, and released when the claim ends |

### 4.2 Passive capture (`sniff.c`)

| ID | Requirement |
|---|---|
| P1 | Bytes are timestamped and split into frames on inter-byte gaps. Default gap 10 ms, range 1–1000 ms. Framing is not assumed |
| P2 | Capture length 1 s to 3600 s. Web defaults to 10 min, console to 30 s |
| P3 | The web side keeps the last 64 frames, each with up to 48 bytes kept and a `truncated` marker beyond that. A session counter lets the page detect a new capture |
| P4 | Console `sniff` prints frames live. Web `Sniff` polls every 700 ms and can download the session as `.txt` |
| P5 | The capture never transmits |

### 4.3 Waveform (`waveform.c`, Debug tab)

This is the feature added for the bus-signal problem: a missing or
corrupted conversation is often an electrical fault, and frames alone do
not show the shape of the signal.

| ID | Requirement |
|---|---|
| W1 | The Debug tab samples the **RX** pin (the bus side, after the transceiver) or the **TX** pin (the ESP32 side). Default RX |
| W2 | Window selectable from the page: 10 ms, 50 ms (default) or 200 ms. The firmware clamps to 1 ms to 200 ms |
| W3 | Every level change is recorded with a time in µs from the start of the window. Resolution is set by the polling loop and is about 1–2 µs. Time base is calibrated on each sample against the timer |
| W4 | Edge buffer holds 2048 edges. If it fills, the sample is marked `truncated` and the plot says so. Shorten the window to see the rest |
| W5 | Summary per sample: level at start, total high and low time, shortest complete high and low pulse, and an **estimated baud rate** from the shortest pulse (1 000 000 ÷ shortest pulse µs). At 10400 baud a single bit is 96 µs, so a clean bus reads close to 10 400. The estimate is a diagnostic, not a measurement standard |
| W6 | The sampler reads GPIO input only. It enables the input buffer on the TX pin for reading and never sets an output level. It holds the bus claim for the whole window |
| W7 | Sample returns **409 Conflict** with the owner's name while a capture, console command, or OTA is using the bus |
| W8 | **Sample** takes one reading. **Scan for activity** repeats a reading each second, for up to 60 tries, and stops at the first sample with edges. Stop cancels it |
| W9 | The plot draws the level against time, with the summary table beside it. A quiet bus is a flat high line on RX; a module talking appears as a pulse train |

**Limits to know before reading the plot:**

- Sampling runs in a polling loop on a single core with WiFi active. Short
  glitches under about 2 µs can be missed, and an occasional long gap can
  be caused by interrupts. The window is not a logic analyser.
- Samples cannot run while a console `send`, `slowinit`, `fastinit` or
  `scan` holds the bus. Those are the only times a TX pin carries traffic,
  so a TX sample shows idle level and wiring, not a transmitted frame. Seeing
  the bench echo as a pulse train needs a read-only sample during `send`,
  which W6–W7 currently forbid (see §7, item 1).
- The sample runs in the HTTP task, so the page waits for the whole window
  (up to 200 ms) before it gets an answer.
- The console runs `wave [-p rx|tx] [-t ms]`, and the Debug tab's command
  box takes the same syntax. Both print or draw the same sample.

### 4.4 Active diagnostics (console only)

Transmit commands run from the USB console and are refused by the web
interface (S3). Each takes the bus claim, and each needs `arm --yes` in the
transmit build.

| Command | Purpose |
|---|---|
| `status` | Pin mapping, TX gates, build type |
| `sniff [-t secs] [-g gap_ms]` | Passive capture, always safe |
| `arm --yes` / `disarm` | Unlock or lock transmission |
| `slowinit <hex>` | 5-baud init to one address, e.g. `slowinit 33` |
| `fastinit` | KWP2000 wake-up pattern |
| `scan` / `scan -f 00 -t 3F` | Slow init across the candidate list, or a range |
| `send <hex...> [-c] [-w ms]` | Raw frame; `-c` appends checksum, `-w` sets response wait |

The candidate address list in `kline_console.c` is unverified apart from
`0x33`, the ISO 9141-2 OBD address. `scan` exists to establish the real map.

### 4.5 Web interface (`web_server.c`)

Single page, five tabs, served from the board.

| Tab | Function |
|---|---|
| Status | Build (passive or TX), armed state, transport, pins, network, uptime, heap. Refreshes every 2 s |
| Sniff | Start/stop passive capture; live frame list; download |
| Debug | Waveform sample and scan (§4.3) |
| WiFi | Scan networks, set home WiFi (optional), change the AP password, reboot |
| OTA Update | Check GitHub releases and install one; or upload a `.bin` |

| Route | Method | Purpose |
|---|---|---|
| `/` | GET | The page |
| `/api/status` | GET | Status JSON |
| `/api/version` | GET | Running and inactive-slot versions |
| `/api/scan` | GET | Nearby WiFi networks |
| `/api/sniff` | GET | Frames since `?since=`, plus state |
| `/api/sniff/start?secs=&gap=` | POST | Start a passive capture (409 if busy) |
| `/api/sniff/stop` | POST | Stop any capture |
| `/api/wave?pin=rx\|tx&ms=` | GET | One waveform sample: summary and edge list (409 if busy) |
| `/wifi-save`, `/ap-save` | POST | Save settings, reboot |
| `/api/reboot` | POST | Reboot |
| `/ota/upload` | POST | Flash an uploaded image |
| `/api/ota/github/releases` | GET | List releases |
| `/api/ota/github/install` | POST | Install a release (refused while the bus is claimed) |
| `/api/ota/github/progress` | GET | Install progress |

### 4.6 Networking (`net.c`)

| ID | Requirement |
|---|---|
| N1 | The board always runs its own access point: SSID `RRp38-KLine`, WPA2, address 192.168.4.1 |
| N2 | The AP password defaults to `rrp38kline` and must be changed on the WiFi tab. It must be 8–63 characters |
| N3 | Optional home WiFi. When joined with internet, the board is also at `http://rrp38.local/` and can fetch releases |

### 4.7 Firmware updates

| ID | Requirement |
|---|---|
| U1 | Two OTA slots of 1.875 MB each. The previous image stays in the other slot |
| U2 | A release is a `v*` tag matching `FIRMWARE_VERSION`. GitHub Actions publishes two images: `RRp38KLine_passive_vX.Y.Z.bin` and `RRp38KLine_tx_vX.Y.Z.bin` |
| U3 | Installing the other kind from the OTA tab switches the board between the passive and TX images |
| U4 | First flash of a new board needs a full erase (browser flasher, or `esptool erase-flash` first). A board flashed over older firmware logged an AP that never appeared on air |

## 5. Non-functional requirements

| ID | Requirement |
|---|---|
| NF1 | The web interface is read-only towards the car, except for the passive capture and waveform sample in §4.3 and §4.2 |
| NF2 | Build passes with no new warnings. Current app image: 1.27 MB, about 35% of the app partition free |
| NF3 | Web pages are a single embedded HTML string. No external scripts or CDN fonts |
| NF4 | The HTTP server stays responsive during a capture. Capture runs in its own task at a higher priority than HTTP |
| NF5 | No credentials, keys or the EKA code are stored in the repository |

## 6. Verification

| Check | How | Pass |
|---|---|---|
| Build | `idf.py -B build_ncv ... build` (README, Build) | Clean build, no new warnings |
| Passive build has no TX | `build/` passive config; `status` shows TX compiled out | Shown |
| Pins and gates | `status` on the board | Matches §2 |
| Echo and wiring | HARDWARE.md, bench step 5: `arm --yes`, `send 55` | Clean echo |
| Waveform idle | Debug tab, RX, 50 ms, bus idle | Flat high line, 0 edges, no complete pulse |
| Waveform traffic | Debug tab, RX, scan during ignition-on or a tool session | Pulse train, estimate near 10400 baud |
| Waveform busy | Start a web sniff, then Sample | 409 with owner `web` |
| Waveform truncation | Sample 200 ms on a busy bus | `truncated` shown if more than 2048 edges |
| Capture | Sniff tab, 60 s | Frames listed with timestamps; download works |
| OTA gate | Start a sniff, then a GitHub install | Refused while the bus is claimed |

Bench checks: [HARDWARE.md — Bench testing before the car](docs/HARDWARE.md#bench-testing-before-the-car).

## 7. Open questions

1. **Sampling during transmit.** §4.3 forbids a waveform sample while a
   console `send` holds the bus. A read-only RX sample during `send` would
   show the echo pulse train, which is the most useful bench view. Decide
   whether to allow it.
2. **Glitch resolution.** The polling loop misses very short glitches. If
   they matter, move edge capture to GPIO interrupts or the RMT peripheral.
3. **Fast init against the TJA1021 27 ms minimum.** Open from SESSION-NOTES
   §13. Settle it once the echo test passes.
4. **Address table.** Replace the candidate list with a verified one after
   Phase 6.

## 8. Out of scope

- Protocol decoding of PIDs, parameters or DTCs (Phase 7).
- CAN, LIN frames, and the L-line transmitter.
- Any feature that writes to a module without the arm gate.
- Logging to SD or telemetry (Phase 8 options).

## 9. Repository layout

```
FSD_RRp38KLineInterface.md   this document
main/kline.h / kline.c       transport, init, bus claim
main/kline_console.c         console commands, candidate address list
main/sniff.h / sniff.c       passive capture, shared by console and web
main/waveform.h / waveform.c edge sampler for the Debug tab
main/web_server.c            web HMI, WiFi settings, OTA, JSON routes
main/net.c                   access point, home WiFi, mDNS
main/main.c                  banner, startup, REPL
main/version.h               firmware version
main/Kconfig.projbuild       pins, baud, safety gates
partitions.csv               nvs, otadata, two OTA slots
docs/                        PLAN, HARDWARE, INTERFACE-MODULES, SESSION-NOTES
```

## 10. Changelog

| Version | Change |
|---|---|
| 0.4.0 | Web HMI (Status, Sniff, WiFi, OTA); OTA from GitHub releases; release workflow with passive and TX images; browser flasher; ESP32-C6 retarget |
| Unreleased | **Waveform sampler and Debug tab** (§4.3, W1–W9). Route `/api/wave`. Web interface otherwise unchanged. Version not yet bumped |
