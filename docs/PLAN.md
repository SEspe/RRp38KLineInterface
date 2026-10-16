# Project plan

Phased, with explicit entry and exit criteria for each stage. The ordering
is deliberate: every phase either proves the hardware or narrows the
protocol, so that when something fails you always know which of the two it
was.

**Status: Phase 1 complete. Phase 2 in progress — a plain TJA1021 breakout
with no MCU (marking 1021T2C) received 2026-10-16. It replaces the
MCU-fronted U7X54911 from 2026-09-30, so no bypass is needed. The ESP32
turned out to be an ESP32-C6; the firmware is retargeted and runs v0.4.0.
Track H (web HMI, OTA from GitHub) is done. Next: the powered bench
checks in HARDWARE.md, then Phase 3.**

---

## Phase 0 — Establish what the car actually uses ✅ DONE

**Goal:** confirm the physical layer before spending money.

- [x] Confirm the P38A has no CAN bus
- [x] Confirm GEMS uses ISO 9141 K-line at 10400 baud
- [x] Identify the diagnostic socket pinout (pin 7 K, pin 15 L, 4/5 GND, 16 +12 V)
- [x] Identify the safety constraint (BeCM lockout / EKA code)

**Outcome:** CAN abandoned; SN65HVD230 shelved. See
[SESSION-NOTES.md](SESSION-NOTES.md).

---

## Phase 1 — Firmware transport ✅ DONE

**Goal:** a correct ISO 9141 transport and a console to drive it, building
clean, before any hardware exists.

- [x] UART at 10400 baud with half-duplex echo consumption and verification
- [x] ISO 9141 timing constants (W1–W5, P1–P4)
- [x] Bit-banged 5-baud slow init with glitch-free TX pin handover
- [x] KWP2000 fast init
- [x] Passive sniffer with gap-based frame splitting
- [x] Address scanner
- [x] Two independent transmit gates
- [x] Builds against ESP-IDF v6.0.1 — 185 KB, 82 % partition free
- [x] Retargeted to the **ESP32-C6** actually in hand (2026-10-16): console
      over built-in USB, pins TX 18 / RX 19 / EN 20 / MOSFET 14. 213 KB,
      79 % free. **Flashed; `status` answers on COM14**

**Outcome:** `RRp38KLineInterface.bin` builds and runs on the ESP32-C6.
Not yet connected to a transceiver.

---

## Phase 2 — Procure hardware ⬅ CURRENT

**Goal:** get two independent transceivers so hardware faults and protocol
faults can be told apart.

- [x] Order a **LIN transceiver breakout** — a **TJA1021** TTL-to-LIN module,
      ordered 2026-09-09, **received 2026-09-30**. Datasheet-checked; see
      [INTERFACE-MODULES.md](INTERFACE-MODULES.md)
- [x] ~~Bypass the U7X54911's MCU~~ — superseded: a **plain TJA1021
      breakout** with no MCU arrived 2026-10-16
- [ ] **Bench-check the plain breakout** — `RX` idles at 3.3 V, `TX`/`SLP`
      defaults, and the powered 1 kΩ-load pull-up test with the jumper cap on
      `HOST`, on `SLAVE`, and off. See [HARDWARE.md](HARDWARE.md#the-module-to-use--plain-tja1021-breakout-no-mcu)
- [ ] Order a **KKL 409.1 cable, FTDI FT232RL version**
- [ ] Order the supporting parts: automotive 12 V→5 V buck, 510 Ω, 100 nF ×2,
      1 A inline fuse, TVS on the 12 V rail
- [ ] **For the TJA1021:** a 2N7002 or BSS138, 1 kΩ and 100 kΩ for the
      bypass MOSFET (5-baud init only — fast init clears its timeout), and a
      4.7 kΩ RXD pull-up **to 3.3 V** if the board lacks one
- [ ] Optionally, a bare L9637D for a permanent build later
- [ ] **Find and write down the EKA code for the car**

**Transceiver caveat:** LIN parts with a TxD dominant timeout cannot do a
5-baud init unmodified. Measured against their datasheets: the NCV7329 at
14/25/46 ms and the TJA1021 at 27/55/90 ms, versus a 200 ms bit cell. The
firmware supports a bypass MOSFET (`CONFIG_RRP38_INIT_DRIVER_GPIO`) to work
around this — see [INTERFACE-MODULES.md](INTERFACE-MODULES.md) and
[HARDWARE.md](HARDWARE.md). The TJA1021's 27 ms guaranteed minimum does
clear the 25 ms low of a **fast** init, so that path needs no MOSFET.

Full reasoning in [INTERFACE-MODULES.md](INTERFACE-MODULES.md).

**Exit criteria:** parts in hand, EKA code recorded somewhere that is not
inside the vehicle.

---

## Phase 3 — Bench bring-up (no car)

**Goal:** prove the interface electrically, with zero risk to the vehicle.

**Step-by-step procedure, with expected readings:**
[HARDWARE.md — Bench testing before the car](HARDWARE.md#bench-testing-before-the-car).

1. Flash the firmware. Confirm the console banner and `p38>` prompt.
2. `status` — verify the pin mapping matches how you actually wired it.
3. Wire the transceiver to a 12 V bench supply, K pulled up through 510 Ω,
   nothing else on the bus.
4. Rebuild with `CONFIG_RRP38_ALLOW_TX=y`.
5. `arm --yes`, then `send 55` — the byte must come back as an echo.

**Why this works:** the K-line is half-duplex, so the echo is inherent. No
loopback jig needed. `kline_write()` verifies it on every byte.

**Exit criteria:** `send` succeeds with no echo mismatch. This means level
shifting, grounding, pull-up and baud rate are all correct.

**If it fails:**

| Symptom | Likely cause |
|---------|-------------|
| "no echo for byte" | Enable/SLP pin not driven high; no 12 V; missing pull-up |
| "echo mismatch" | Baud rate wrong; 5 V logic feeding a 3.3 V pin; bad ground |
| Nothing at all, console dead | Wrong COM port — the C6's console is its built-in USB (COM14), not a UART |

---

## Phase 4 — Passive capture in the car

**Goal:** see real traffic. Still zero transmit risk.

Build **passive** for this phase (`CONFIG_RRP38_ALLOW_TX=n`) so transmission
is not merely disarmed but absent from the binary.

1. Connect to socket pin 7, ground to 4/5, 12 V to 16 through the fuse.
2. `sniff -t 60` across several states: ignition off, ignition on, engine
   cranking, engine running, and while operating things (windows, EAS
   height change, climate).

**Expected result: possibly nothing.** The P38 K-line is not chatty when
idle — it answers rather than broadcasts. Silence here is *normal* and does
not mean the interface is broken, which is exactly why Phase 3 comes first.

**Exit criteria:** either real bytes captured, or confident (because Phase 3
passed) that the bus is simply quiet.

---

## Phase 5 — First handshake

**Goal:** get one module to talk. This is the first phase with real risk.

**Preconditions:** Phase 3 passed. EKA code written down. Engine off.

1. Rebuild with `CONFIG_RRP38_ALLOW_TX=y`.
2. `arm --yes`
3. `slowinit 33` — the ISO 9141-2 standard address, and the only one that is
   specified rather than guessed.

**The confirmation signal:** on a healthy P38, when a tool successfully
establishes communications the dash message centre displays
**"Diagnostics Mode"**. Connect with the ignition OFF to see it. This is a
free, zero-ambiguity confirmation that the init sequence is correct, before
a single byte of payload has been decoded.

**Exit criteria:** `slowinit` returns sync `0x55` plus two keyword bytes,
and/or "Diagnostics Mode" appears on the dash.

**If `0x55` never arrives:** try `fastinit`; try with ignition on; check the
W1 timing the firmware reports against the 60–300 ms spec window.

---

## Phase 6 — Map the modules

**Goal:** replace the unverified candidate address list with real data.

1. `scan` — walks the candidate list (0x11, 0x12, 0x13, 0x16, 0x18, 0x1A).
2. If that finds little, `scan -f 00 -t 3F` for a systematic sweep. Roughly
   3 s per address, so a 64-address sweep is about 3 minutes.
3. Record which addresses answer, and their keyword bytes — KW1/KW2 identify
   the protocol variant the module speaks.

**Exit criteria:** a verified address table. Update the list in
`kline_console.c` and correct §9 of SESSION-NOTES.md.

**Note:** scanning transmits. It is the riskiest routine operation in the
project, because it addresses modules somewhat blindly. Do it once,
carefully, with the EKA code to hand — not casually.

---

## Phase 7 — Decode the protocol

**Goal:** turn bytes into meaning. The open-ended phase.

1. With a module initialised, capture its responses to known stimuli.
2. Work out the framing: header, address bytes, length encoding, and whether
   the checksum really is a plain 8-bit sum (assumed, unconfirmed).
3. Correlate values against things you can measure independently — coolant
   temperature against a thermometer, RPM against the tacho, road speed
   against GPS.
4. Cross-check against published Nanocom/Faultmate behaviour and the forum
   reverse-engineering work.

**Exit criteria:** a documented frame format and a handful of parameters
decoded with confidence.

**This is where the project stops being an electronics job and becomes a
reverse-engineering one.** Budget accordingly.

---

## Phase 8 — Application

**Goal:** something useful. Options, roughly in increasing effort:

- **Fault code reader** — read and clear DTCs. The most immediately useful.
- **Data logger** — SD card or SPIFFS, timestamped captures for offline
  analysis.
- **Live gauge display** — reuse the OLED/display work from the esp32_clock
  project.
- **WiFi telemetry** — web page or MQTT, same pattern as esp32_clock.
- **EAS diagnostics** — the P38's most failure-prone system and arguably the
  highest-value target, if it answers.

---

## Track H — Web HMI and OTA ✅ DONE

**Goal:** use the board from a phone in the car, and update it without a
USB cable. Independent of the K-line phases; can start any time. Decisions
24–27 in [SESSION-NOTES.md](SESSION-NOTES.md#19-web-hmi-and-ota--planned).

**Template: RemoteStart's SlaveWallas**, which already runs on an
ESP32-C6 under ESP-IDF v6.0.1. Copy and adapt, do not re-derive:

| From `RemoteStart/SlaveWallas/` | What it gives |
|---|---|
| `partitions.csv` | `nvs` + `otadata` + two 1.875 MB OTA slots; fits the 4 MB C6 |
| `main/version.h` + top-level `CMakeLists.txt` | `FIRMWARE_VERSION` baked into the app descriptor, so the inactive slot's version is readable |
| `main/web_server.c` | WiFi portal and NVS credentials, `/api/status`, `/api/sysinfo`, `/api/reboot`, `/ota/upload`, GitHub Releases install with progress, Firmware Versions card |
| `main/idf_component.yml` | `espressif/cjson`, `espressif/mdns` |
| `.github/workflows/release.yml`, `webflash-pages.yml` | Tag → release `.bin`; esp-web-tools browser flasher on GitHub Pages |

**Network: AP + STA.** The board always runs its own access point
(WPA2, password set in NVS) so a phone can reach it at `192.168.4.1` in the
car. When home WiFi is in range it also joins as a station, which is what
GitHub-release OTA needs. File-upload OTA works over the AP alone.

**K-line access from the web: read-only.** First version shows status, a
live sniff view and the OTA tab. `arm`, `send`, `slowinit`, `fastinit` and
`scan` stay on the USB console until at least Phase 5 has passed. Adding
them to the web later is a separate decision.

Steps:

1. ✅ Partitions, `version.h`, `PROJECT_VER` from the header.
2. ✅ WiFi AP + STA, NVS, mDNS (`rrp38.local`). No separate portal: the
   always-on AP serves the normal page, which has the WiFi tab.
3. ✅ Web server: status page, `/api/status` (the console's `status`
   fields), WiFi and AP-password settings, OTA upload, reboot.
   *Page checked from a phone 2026-10-16. OTA upload verified over home
   WiFi: v0.2.0 → v0.3.0, 1.07 MB in 4 s, back up in 2 s.*
4. ✅ Sniff view: one sniffer (`sniff.c`) shared by the console and a
   **Sniff** tab, a 64-frame ring the page polls via `/api/sniff`, start and
   stop from the page (default 10 min, max 60), download as `.txt`. A bus
   claim keeps a web sniff and console init/send off the UART at the same
   time. Passive by construction. *State machine and claim verified;
   frame display untested until there are bytes on RX.*
5. ✅ Install from GitHub on the OTA tab: lists every passive and tx image
   in the newest releases, preselects the board's own kind, warns when
   the pick would switch build, shows download progress. The Firmware card
   shows the variant and the newest version on GitHub. `/ota/upload` and
   the GitHub install both refuse while the K-line is claimed.
6. ✅ CI: `.github/workflows/release.yml` builds both variants on a `v*`
   tag (failing if the tag ≠ `version.h`) and attaches
   `RRp38KLine_passive_v<ver>.bin` and `RRp38KLine_tx_v<ver>.bin`.
   `webflash-pages.yml` publishes an esp-web-tools flasher with both
   variants at <https://sespe.github.io/RRp38KLineInterface/>. The repo was
   made public for this (decision 32).

**Checks specific to this project:**

- K-line timing with WiFi running on the C6's single core: re-run the
  Phase 3 echo test and the fast-init check with the AP up.
- An OTA reboot while plugged into the car must not disturb the bus: TXD is
  released, SLP falls to its pull-down, the MOSFET gate has its 100 kΩ.
  Verify on the bench by watching `LIN` through a reboot.
- Never start an OTA while a K-line session is open. Every reboot from the
  web side first calls `kline_release_bus()`: disarm, MOSFET gate low, SLP
  low.
- **First flash of a board needs a full erase.** Without it the board
  logged a running SoftAP that never appeared on air (§21 of
  SESSION-NOTES; RemoteStart FSD §11.1 saw the same).

**Exit criteria:** phone on the AP sees live status and sniff data; an OTA
upload and a GitHub install both succeed and the board reboots into the
new image.

---

## Parallel track — a lower-risk alternative

If the goal turns out to be *a gauge display* rather than *diagnostics*,
tapping the analogue sensors directly (oil pressure, temperatures, voltage)
with the ESP32's ADCs is far less work and carries **no BeCM risk at all**.

Worth keeping in mind if Phase 5 or 7 stalls. The two approaches are not
mutually exclusive — sensor tapping could deliver a working display while
K-line decoding proceeds at its own pace.

---

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| BeCM immobiliser lockout | Low | **Severe** — car will not start | EKA code recorded; two TX gates; passive-first workflow |
| LIN transceiver's TXD timeout breaks 5-baud init | Medium | Low | Second transceiver (KKL cable) as fallback |
| Module's onboard MCU transmits on the car's K-line, bypassing both TX gates | None with the plain breakout; low once bypassed | **Severe** | Use the plain breakout (no MCU). If the U7X54911 is ever used: MCU held in reset via `RESET` pad; 1 kΩ series on TXD/SLP_N; never connect the board to the car with the MCU running |
| Bus is simply quiet; nothing to sniff | High | Low | Phase 3 bench test proves hardware independently |
| GEMS framing resists decoding | Medium | Medium | Phase 8 parallel track; community resources |
| Corroded kick-panel connectors block EAS | Medium | Low | Known failure point — inspect before blaming the circuit |
| Someone on the WiFi AP transmits on the K-line | Low | **Severe** | Web HMI read-only (decision 25); WPA2 AP password; compile-time TX gate unchanged |
| WiFi load disturbs K-line timing on the single-core C6 | Low | Medium | UART times bytes in hardware; re-run the echo and fast-init tests with WiFi up |
| 12 V transient damages the ESP32 | Low | Medium | Automotive-rated buck, inline fuse, never a bare LDO |
