# Session notes — 2026-09-08

Record of what was investigated, what was decided, and what is still
unverified. Written so a future session (or a future you) does not have to
re-derive any of it.

---

## 1. Starting point

**Original question:** read the CAN bus in the car using an ESP32 and an
**SN65HVD230 CAN Bus Transceiver Communication Module**.

That combination is entirely sound *in general* — the ESP32 has a CAN
controller in silicon (Espressif call it TWAI) and lacks only the physical
layer, which is exactly what the SN65HVD230 provides at 3.3 V logic levels.
The generic gotchas identified were:

- The onboard 120 Ω termination on most blue breakout modules must be
  removed — the vehicle bus is already terminated at both ends, and a third
  resistor causes error frames. This is the single most common failure.
- The Rs / slope-control pin needs to be near 0 Ω for high-speed mode.
- The classic ESP32 TWAI peripheral does **not** support CAN-FD.

None of this ended up mattering, because of finding 2.

## 2. Key finding — the vehicle has no CAN bus

**Vehicle: 1996 Range Rover P38A.**

Land Rover's first CAN vehicle was the L322 (2002, BMW-derived electronics).
The P38A (1995–2002) predates it. Confirmed by web research rather than
recall, because a hardware purchase depended on it.

**Direct evidence:** Nanocom — who manufacture the main aftermarket
diagnostic tool for exactly this ECU — state that *"the GEMS engine
communications uses K-Line ISO-9141 protocols."* That is the vendor whose
product has to actually talk to this ECU, so it is about as authoritative as
it gets short of the factory manual.

**Consequence:** the SN65HVD230 has nothing to connect to on this car.
Pins 6 and 14 of the diagnostic socket are not populated. The part goes in
the drawer for a future project.

> Pedantic note on a claim repeated throughout the P38 forums: *"the P38
> predates CAN"* is not quite true. CAN existed from 1986 and shipped in the
> W140 Mercedes in 1991. The accurate statement is that **Rover / Land Rover**
> did not adopt it until the L322. Same practical conclusion.

## 3. What the P38A actually uses

Two separate things, often confused:

**a) Diagnostics — ISO 9141 K-line, 10400 baud.**
Single wire, half-duplex, open-drain, idles high at battery voltage. This is
what diagnostic tools use and what this project targets.

**b) Inter-module communication — proprietary clocked serial data bus.**
Roughly signal ground, clock, data, and sometimes direction, over 4 wires,
each wire duplicated for redundancy. Carries BeCM ↔ instrument pack traffic,
fuel gauge, message centre, door outstations, switch panels.

This second bus is *synchronous and clocked* — architecturally nothing like
CAN, which is asynchronous and differential. A CAN transceiver would make no
sense on it either. The deepest public reverse-engineering of it is
[BeaDy's write-up](https://www.beady.com/blog/p38-becm-communications/).
Out of scope for this project, but worth knowing it exists.

## 4. Diagnostic socket pinout

16-pin J1962 connector, under the dash, driver's side.

| Pin | Wire colour     | Function |
|-----|-----------------|----------|
| 7   | Pink/Red        | **K-line** — main diagnostic line |
| 15  | Light Green/Red | L-line |
| 4,5 | Black           | Ground |
| 16  | —               | Battery +12 V (permanent) |

**Correction made mid-session.** An earlier statement that different systems
would sit on different pins, and that not all modules would be reachable
from one line, was largely **wrong**. Pin 7 is the main diagnostic line and
carries BeCM, HEVAC, ABS, engine ECU *and* autobox ECU. This makes the build
simpler: a single transceiver on pin 7 reaches most of the car.

The L-line on pin 15 is wired to a second GPIO as an option (disabled by
default) because ISO 9141 traditionally carries the 5-baud address on L as
well as K. Most ECUs accept K-only initialisation.

**Known weak point:** EAS (air suspension) diagnostic lines route through the
LH footwell kick panel connectors, which are notorious for corroding green.
If EAS is the *only* module that will not answer, look there before
suspecting the interface circuit.

## 5. Protocol status

**Engine management on a 1996 car is GEMS** (Generic Engine Management
System). Bosch Motronic 5.2.1 arrived with the 1999 Thor engines, so it is
not relevant here.

GEMS and the BeCM use Land Rover's own message format, **not KWP2000**. It
has been substantially reverse-engineered by the community — the aftermarket
tools (Nanocom Evolution, Faultmate MSV-2) are built on that work, and the
factory tool was Testbook/T4 — but the public documentation is scattered
across forums and varies in quality.

**Decision taken:** ship no GEMS PID table. The values circulating online are
not consistent enough to hard-code without risking sending nonsense at a
module. Capture first, decode second. See §7.

**Market note:** EU OBD-II was not mandated until 2001 for petrol. A 1996
P38 sold in Norway has the J1962-shaped connector but runs Land Rover
proprietary diagnostics, so generic ELM327 dongles will not get far. (US
1996 cars are closer to ISO 9141-2 compliant on the engine ECU, but the
other modules are still proprietary either way.)

## 6. Safety — the constraint that shaped the firmware

The P38 BeCM is genuinely fragile about diagnostic traffic. Malformed or
misaddressed messages are a documented cause of **immobiliser lockout**
requiring the **EKA (Emergency Key Access) code** — or a BeCM reflash — to
recover the vehicle.

Two rules adopted, and both are enforced in code:

1. **Know the EKA code before plugging anything in.** Worth doing regardless
   of this project.
2. **Start passive.** `sniff` never transmits.

| Gate | Where | Default |
|------|-------|---------|
| `CONFIG_RRP38_ALLOW_TX` | compile time (menuconfig) | **off** — TX code is not compiled at all |
| `arm --yes` | runtime console command | **off** on every boot |

Both must be open before a byte leaves the ESP32. The `arm` command without
`--yes` prints the EKA warning rather than arming.

## 7. Design decisions

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | Target K-line, abandon CAN | The car has no CAN bus (§2) |
| 2 | Single transceiver on pin 7 | Reaches BeCM, HEVAC, ABS, engine, autobox (§4) |
| 3 | Ship no GEMS PID table | Public values not reliable enough to hard-code (§5) |
| 4 | Mark the candidate address list "unverified" | Only `0x33` is specified rather than observed |
| 5 | Two independent TX gates | BeCM lockout risk (§6) |
| 6 | Verify the echo on every transmitted byte | K-line is half-duplex, so the echo is free — it doubles as a continuous wiring self-test |
| 7 | Sniffer splits frames on inter-byte gaps | Framing is unknown; gap timing is the main clue to where frames start and end |
| 8 | `scan` command | Establish the real address map empirically instead of trusting forum values |
| 9 | Bit-bang the 5-baud init | 5 baud is far below the minimum the UART clock divider can reach |
| 10 | Buy two different transceivers | Makes it possible to distinguish a hardware fault from a protocol misunderstanding |

## 8. Implementation notes worth remembering

- **TX pin handover.** For the 5-baud init the TX pin is moved between the
  UART peripheral and plain GPIO via `esp_rom_gpio_connect_out_signal()`.
  The output register is driven **high before** the handover, so the K-line
  never glitches low — a low glitch looks like a start bit to every module
  on the bus.
- **ESP-IDF v6 moved things.** `uart_periph_signal` migrated into the
  `esp_hal_uart` component and the `SOC_UART_*_PIN_IDX` macros are gone, so
  the TX output signal index is taken directly from `soc/gpio_sig_map.h`
  (`U1TXD_OUT_IDX` / `U2TXD_OUT_IDX`).
- **IDF v6 split the driver component.** `driver/uart.h` is no longer
  reachable via `REQUIRES driver`; it needs `esp_driver_uart`, and GPIO
  needs `esp_driver_gpio`. This was the one build failure encountered.
- `CONFIG_FREERTOS_HZ=1000` is set so `vTaskDelay` gives 1 ms resolution,
  which matters for the ISO 9141 P4/W4 timing parameters.

## 9. Still unverified — do not treat as fact

- **Every candidate ECU address except `0x33`.** The list in
  `kline_console.c` (0x11 engine, 0x12 gearbox, 0x13 ABS, 0x16 BeCM,
  0x18 EAS, 0x1A HEVAC) is drawn from community reports and is a starting
  point for `scan`, nothing more.
- **The exact GEMS message framing** — header layout, addressing, whether
  the checksum is a plain sum. The firmware assumes a plain 8-bit sum
  because that is what ISO 9141 and most Land Rover framing use, but this is
  unconfirmed for GEMS specifically.
- **Whether the L-line is needed** for initialisation on any P38 module.
- **The exact pin 7 wire colour** on a Norwegian-market car — verify against
  a wiring diagram before cutting.

## 10. Build environment

ESP-IDF **v6.0.1**, target esp32. `idf.py` is not on PATH on this machine;
the working invocation is recorded in the README and in the
`feedback-esp32-build-invocation` memory.

Build verified working: `RRp38KLineInterface.bin`, 185 KB, 82 % of the app
partition free. Not yet flashed to hardware.

---

# Addendum — 2026-09-09

## 11. NCV7329 evaluation — the TxD timeout is real

A **Soldered NCV7329 LIN Transceiver Breakout** was considered. Checked
against the [onsemi datasheet](https://www.onsemi.com/pdf/datasheet/ncv7329-d.pdf)
rather than assumed.

**`tTxD_TIMEOUT` = 14 / 25 / 46 ms (min/typ/max).**

This confirms, with a number, the risk flagged generically in
INTERFACE-MODULES.md. A 5-baud init holds the bus dominant for **200 ms per
bit cell** — over 4× the *maximum* timeout. The transceiver would release
the bus partway through every low bit and corrupt the address. onsemi
themselves spec the part as "1 kbps to 20 kbps"; that lower limit *is* the
timeout.

Fast init is also marginal: 25 ms low against a 14 ms minimum.

Other specs worth recording:

- **Pin 2 EN must be driven high** (internal 100–650 kΩ pull-down). Without
  it the part appears completely dead. `V_IH` = 2 V, so 3.3 V works.
- **Pin 1 RxD is open-drain** — needs an external pull-up to 3.3 V.
- **VBB is 5–18 V.** The bus pin is rated ±42 V, but the supply is not.
  A jump start or load dump exceeds it; fit a TVS.
- Internal 30 kΩ + reverse diode to VBB (slave termination). The external
  510 Ω is still needed for K-line.
- Pinout: 1 RxD, 2 EN, 3 NC, 4 TxD, 5 GND, 6 LIN, 7 VBB, 8 NC.

**Useful discovery:** the **NCV7327** is documented by onsemi as differing
from the NCV7329 *only* by the absence of the TxD timeout — same 8-pin SOIC,
same pinout. It is the clean drop-in answer if a chip swap is acceptable.

### Decisions added

| # | Decision | Rationale |
|---|----------|-----------|
| 11 | Support an optional **bypass MOSFET** for init sequences | Lets timeout-limited LIN transceivers do a 5-baud init; ~€0.20 of parts; keeps the transceiver's protection on all normal traffic |
| 12 | Add `CONFIG_RRP38_EN_GPIO` | Transceivers like the NCV7329 need EN driven high or they look dead — a silent-failure trap worth removing |
| 13 | Verify both config paths build | The `#if` branches are mutually exclusive, so a single build only ever proves one of them |

Implementation: with `CONFIG_RRP38_INIT_DRIVER_GPIO` set, the UART stays
attached to the TX pin throughout (idle high = recessive) and the init is
bit-banged through the MOSFET, so the two drivers never contend. With it
unset, behaviour is unchanged — the original TX pin handover.

**Gate pull-down is not optional.** A floating MOSFET gate while the ESP32
is in reset could hold the car's diagnostic line dominant. 100 kΩ to GND.

## 12. TJA1021 ordered — the timeout is shorter than feared, but still there

A generic **TTL-to-LIN TJA1021 module** was ordered on 2026-09-09. Checked
against the [NXP datasheet, rev. 9](https://www.nxp.com/docs/en/data-sheet/TJA1021.pdf)
the same way the NCV7329 was.

**`tto(dom)TXD` = 27 / 55 / 90 ms (min/typ/max).**

Roughly double the NCV7329's numbers, and the difference is decisive in one
place. The datasheet states the timer "will not be triggered before `tmin`",
so:

- **Slow init still fails.** 200 ms per bit cell against a 90 ms maximum.
  The bypass MOSFET is required, exactly as for the NCV7329.
- **Fast init now works through the transceiver.** It holds the bus low for
  25 ms, and 27 ms is guaranteed. 2 ms of margin is thin but it is a
  guaranteed minimum, not a typical. On the NCV7329 the same 25 ms sat
  *above* a 14 ms minimum, so it was unusable.

The other reason to prefer this part: NXP list **"K-line compatible"** as an
explicit feature bullet in §2.1 of the datasheet. Same standing as the
TJA1020 application note already cited, but on the exact part in hand.

Other specs worth recording:

- **No logic supply pin exists.** RXD is open-drain, so the pull-up rail
  *is* the logic level — pull it to 3.3 V and the ESP32 needs no divider.
  The corollary is the one real hazard: if the module fits its own pull-up
  to an onboard 5 V rail, that is 5 V into a 3.6 V-max ESP32 pin.
- **Pin 2 SLP_N must be driven high**, and Normal mode is entered on the
  *rising edge* (internal 140–1200 kΩ pull-down). `CONFIG_RRP38_EN_GPIO`
  already does this. Note [4] adds that entering Normal with TXD low leaves
  the transmitter off — `kline_start()` happens to get this right, since it
  configures the UART before raising EN.
- **Pin 3 WAKE_N** has only a 1–30 µA pull-up. Tie it to VBAT.
- **VBAT: −0.3 to +40 V limiting**, specified to 27 V, transmitter
  guaranteed to 18 V. A big improvement on the NCV7329's hard 18 V ceiling.
- Responder termination 20/30/47 kΩ to VBAT — still needs the external 510 Ω.
- Pinout: 1 RXD, 2 SLP_N, 3 WAKE_N, 4 TXD, 5 GND, 6 LIN, 7 VBAT, 8 INH.
  Pin-to-pin compatible with the TJA1020 and MC33662(B).
- **Variants matter slightly:** `T/10` and `AT` are wave-shaped for
  10.4 kBd (SAE J2602); `T/20` and `BT` for 20 kBd. Either works at 10400,
  but `/10` is the exact fit. Read the chip marking on arrival.

**No firmware change needed.** `sdkconfig.ncv.defaults` applies verbatim —
the EN pin maps to SLP_N and the bypass MOSFET covers slow init. The file
name is now a misnomer; the config it holds is the general
"timeout-limited LIN transceiver" case.

## 13. Where this stands — stopping point, 2026-09-09

**Everything that can be done without hardware is done.** Nothing is
blocked on a decision; the project is waiting on a delivery.

**Settled:**

- Physical layer confirmed: ISO 9141 K-line, 10400 baud, socket pin 7.
  No CAN on this car.
- Firmware transport, both init sequences, sniffer, scanner and console
  build clean against ESP-IDF v6.0.1. Never flashed.
- Transceiver chosen and datasheet-verified: **NXP TJA1021**, ordered
  2026-09-09. The TxD-timeout workaround it needs is already in the
  firmware, and `sdkconfig.ncv.defaults` is the config for it as-is.
- Both remaining unknowns are hardware ones, and both have a written
  check: what the module's RxD pull-up connects to, and whether the chip
  is a `/10` or a `/20`.

**Waiting on:**

- The TJA1021 module itself.
- A KKL 409.1 cable — still worth buying, for the J1962 connector and as a
  second, independent transceiver.
- Bypass MOSFET parts (2N7002/BSS138, 1 kΩ, 100 kΩ), 510 Ω, TVS, buck
  converter, inline fuse.
- **The EKA code.** This is the only outstanding item that costs nothing
  and is not gated on a delivery. Do it before the parts arrive.

**First three moves when the parts land**, in order — all of Phase 3, none
of them near the car:

1. Before wiring anything to the ESP32, check where the module's RxD
   pull-up goes. If it lands on an onboard 5 V rail, lift it and re-pull to
   3.3 V. This is the one way the board can damage the ESP32.
2. Flash, run `status`, and confirm the reported pin mapping matches the
   wiring as built.
3. Bench supply at 12 V, K pulled up through 510 Ω, nothing else on the
   bus: `arm --yes` then `send 55`. The echo is inherent to a half-duplex
   line, so a clean echo proves level shifting, grounding, pull-up and baud
   rate in one step.

Only after that does the car get involved, and the first thing run there is
`sniff`, which cannot transmit.

**One open question worth settling on the bench, not in the car:** whether
fast init genuinely survives the TJA1021's 27 ms guaranteed minimum in
practice. The datasheet says it must, with 2 ms of margin. If it does, slow
init through the bypass MOSFET is a fallback rather than the main path.

---

# Addendum — 2026-09-30

## 14. The TJA1021 module arrived — and has an MCU on it

The *"TTL to LIN Bus TJA1021 Module / Automotive Bus U7X54911"* arrived.
Photos of both sides showed a 48-pin chip beside the SO8, which changes the
plan: **this is a UART-to-LIN protocol converter, not a transceiver
breakout.**

Identified from the package markings:

- **MCU: Geehy APM32F103C8T6**, an STM32F103C8T6 clone, on a 3.3 V LDO fed
  from the 12 V terminal. Programming header `3V3 / TX / RX / GND` and a
  `RESET` pad are broken out.
- **Transceiver: NXP TJA1021T/20** — marking *NXP / 1021T/20 / HNNRBA /
  B321*. Genuine NXP, and the 20 kBd slope variant rather than `/10`; §12
  already established either works at 10400. All of §12's datasheet
  analysis stands.
- **Pin 1 confirmed** from the marking orientation, not just inferred from
  layout: read upright, pin 1 is bottom-left. An earlier annotated photo
  that placed it from the silkscreen bar alone turned out correct.

The seller's description (fetched from eBay's description endpoint; the
listing page itself refuses automated fetches) confirms the reading: an
*"intelligent protocol converter"* supporting *"host request control frame
and host request status frame"*, with *"built-in timeout functions"*. No
raw or transparent mode, command set, baud setting or manual is given.

**Why the MCU rules out using it as designed:**

- It generates LIN framing — a ~1.3 ms break, 0x55 sync, PID, checksum. It
  cannot hold the bus low for 25 ms (fast init) or 200 ms (slow init).
- K-line traffic has no LIN break, so the MCU will most likely discard it:
  the sniffer would be blind.
- As a LIN master it may transmit unprompted at power-up. The firmware's
  two TX gates cannot stop that, so **the board must not meet the car with
  the MCU running**.

The concern that drove §13's first move — a 5 V RXD pull-up reaching the
ESP32 — largely goes away: the board's logic is 3.3 V throughout.

### Decisions added

| # | Decision | Rationale |
|---|----------|-----------|
| 14 | **Bypass the MCU**: `RESET` to GND, ESP32 wired directly to TJA1021 pins 1 (RXD), 2 (SLP_N), 4 (TXD) | In reset the APM32's GPIOs float, releasing the transceiver. The board then becomes the planned TJA1021 breakout, keeping its LDO, protection and terminals. Three wires, no firmware change |
| 15 | **1 kΩ in series on TXD and SLP_N** | If the reset wire ever comes off, the MCU and ESP32 contend through a resistor instead of directly. Negligible against the 140 kΩ internal pull-downs |
| 16 | Not pursuing the MCU's native protocol or reflashing it | No documentation, no evidence of a raw mode; reflashing is a project of its own for the same result as three wires |

Wiring table and annotated photo:
[HARDWARE.md — the module in hand](HARDWARE.md#the-module-in-hand--u7x54911-ttl-to-lin-bus-tja1021-module),
image at `docs/images/tja1021-module-pins.jpg`.

### Still to do before Phase 3

Three resistance readings with the board unpowered:

1. **Pin 5 ↔ `GND` terminal** — continuity; confirms orientation once more
   before soldering.
2. **Pin 1 ↔ `3V3` header pin** — the RXD pull-up. If open, the board was
   relying on the MCU's internal pull-up, which is gone in reset: fit
   4.7 kΩ to 3.3 V.
3. **`LIN` ↔ `VCC` terminal** — the master pull-up. Decides whether to add
   1 kΩ in parallel or fit the full 510 Ω.

   **Measured 2026-09-30: ~10 MΩ, unpowered — inconclusive.** A LIN
   master pull-up is normally 1 kΩ *with a series diode*, and the board
   probably also has a reverse-polarity diode on `VCC`. A meter's ohms
   range drives well under a volt, too little to forward-bias one diode,
   let alone two, so it reads open whether or not a resistor is there. The
   chip's own 30 kΩ termination hides behind a diode the same way.

   **Replacement test (powered, bench only):** 12 V on `VCC`/`GND`,
   nothing else on `LIN`, ESP32 not connected. Measure `LIN`–`GND` (expect
   ~10–11 V), then load `LIN` with 1 kΩ to `GND` and measure again:

   | LIN under 1 kΩ load | Meaning | Action |
   |---|---|---|
   | ~5 V | 1 kΩ master pull-up fitted | Add 1 kΩ in parallel → ~500 Ω |
   | < 1 V | Only the 30 kΩ termination, or nothing | Fit the full 510 Ω |
   | ~2–3 V | Something like 2.2–4.7 kΩ | Calculate the parallel value |

   **Why it matters:** a hidden 1 kΩ plus a full 510 Ω is ~340 Ω. At
   14.4 V with the engine running that is ~42 mA, past the TJA1021's
   40 mA minimum `IBUS_LIM`, so the driver could current-limit and fail to
   pull the bus fully low.

   Diode mode (`12V`→`LIN`, then reversed) is a quicker partial check: a
   0.5–1.3 V reading in one direction shows a diode path exists, but not
   whether it leads to 1 kΩ or 30 kΩ.

   Measurements 1 and 2 are still to be taken.

Then the bypass wiring, then §13's moves 2 and 3 unchanged. The EKA code is
still outstanding.

## Sources

- [NXP TJA1021 datasheet, rev. 9](https://www.nxp.com/docs/en/data-sheet/TJA1021.pdf)
- [onsemi NCV7329 datasheet](https://www.onsemi.com/pdf/datasheet/ncv7329-d.pdf)
- [onsemi NCV7327 — same part without the TxD timeout](https://www.onsemi.com/pdf/datasheet/ncv7327-d.pdf)
- [Soldered NCV7329 breakout](https://soldered.com/products/lin-transceiver-ncv7329-breakout)
- [Nanocom NCOM05 — Range Rover P38 GEMS (Petrol to 1999)](https://www.nanocom-diagnostics.com/product/ncom05-range-rover-p38-gems-petrol-to-1999-kit)
- [Range Rovers Forum — diagnostic socket wiring](https://www.rangerovers.net/threads/diagnostic-socket-wiring-get-help.255153/)
- [Range Rovers Forum — serial link, BeCM to instrument cluster](https://www.rangerovers.net/threads/serial-link-becm-to-instrument-cluster-temp-fuel.325628/)
- [Land Rover workshop manual — P38 BeCM description](https://workshop-manuals.com/landrover/p38/86_electrical/description_and_operation/body_electrical_control_module_(becm)_description/)
- [BeaDy — P38 BECM communications](https://www.beady.com/blog/p38-becm-communications/)
- [rangerovers.pub — OBD connection](https://rangerovers.pub/topic/65-obd-connection)
- [NXP AN00093 — TJA1020 LIN transceiver](https://www.nxp.com/docs/en/application-note/AN00093.pdf)
- [ST L9637 datasheet](https://www.st.com/resource/en/datasheet/l9637.pdf)
- [eBay 287315145186 — TTL to LIN Bus TJA1021 Module U7X54911](https://www.ebay.com/itm/287315145186)
