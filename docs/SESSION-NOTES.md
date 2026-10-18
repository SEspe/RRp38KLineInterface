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

## 15. Where this stands — stopping point, 2026-09-30

**The transceiver is in hand, and it is the right chip on the wrong
board.** A genuine NXP TJA1021T/20 sits behind an APM32F103 LIN
protocol converter, and the plan to get at it directly is decided and
documented. Nothing is blocked on a decision; the next steps are bench
work.

**Settled this session:**

- Board identified from photos: Geehy APM32F103C8T6 MCU, NXP TJA1021T/20,
  3.3 V LDO. The seller's description confirms a LIN-master protocol
  converter with no documented raw mode.
- Approach: hold the MCU in reset (`RESET` pad to GND) and wire the ESP32
  to TJA1021 pins 1, 2, 4, with 1 kΩ on 2 and 4 (§14, decisions 14–16).
- Pin 1 location confirmed from the package marking. Annotated photo at
  `docs/images/tja1021-module-pins.jpg`.
- The 5 V-on-RXD hazard from §13 is largely gone: the board's logic is
  3.3 V throughout.
- The unpowered LIN–VCC reading (~10 MΩ) cannot find a diode-isolated
  master pull-up; a powered 1 kΩ-load test replaces it.
- HARDWARE.md now has a full connection reference: every wire, what it
  carries and why, including VBAT, the bypass MOSFET and the bench setup.
- Side note: a CJMCU-230 is also on hand. It is an SN65HVD230 CAN
  transceiver, unusable here for the reasons in §2 — kept for a future CAN
  project.

**Next moves, in order** — all bench, none near the car:

1. **Powered master pull-up test** (§14 item 3, HARDWARE.md): 12 V on
   `VCC`, measure `LIN`, then again with 1 kΩ `LIN`→`GND`. Decides
   between adding 1 kΩ or the full 510 Ω.
2. **Unpowered checks:** pin 5 ↔ `GND` terminal continuity; pin 1 ↔ `3V3`
   header resistance (if open, fit 4.7 kΩ RXD→3.3 V).
3. **Bypass wiring:** `RESET`→GND, GPIO17→pin 4 via 1 kΩ, GPIO21→pin 2 via
   1 kΩ, GPIO16→pin 1, common GND.
4. Then §13's moves 2 and 3 unchanged: flash, `status`, then `arm --yes`
   and `send 55` for the echo test.

**Still waiting on:** bypass MOSFET parts (2N7002/BSS138, 1 kΩ, 100 kΩ),
510 Ω, TVS, buck converter, inline fuse; the KKL 409.1 cable; and **the
EKA code**, still the one outstanding item that costs nothing.

**Still open from §13:** whether fast init genuinely survives the
TJA1021's 27 ms guaranteed minimum. Settle it on the bench once the echo
test passes.

---

# Addendum — 2026-10-16

## 16. A plain TJA1021 breakout arrived — no MCU

A second LIN module arrived. Terminals `VCC / LIN / GND` and
`RX / TX / GND`, header `HOST / SLAVE / GND / 3V3 / SLP / WAKE / INH`.
Close-up photos (zip from Downloads, cropped and contrast-boosted with
System.Drawing) showed **one SO8 and no microcontroller**, so this is the
bare transceiver breakout §12 originally assumed it was buying.

- **U2: NXP TJA1021**, marking *NXP / 1021T2C* (the fourth digit was
  confirmed by eye, since the photos left it ambiguous between 1020 and
  1021). Most likely ordering code TJA1021T/20/C: 20 kBd slope variant,
  revision C. §12's datasheet analysis applies unchanged: fast init works
  through the chip, slow init needs the bypass MOSFET.
- The 1020/1021 distinction mattered: the TJA1020's TxD timeout is much
  shorter (about 6 ms minimum, from memory and not checked), which would
  have broken fast init too.
- **D3 = SS34** Schottky, probably reverse-polarity protection on `VCC`.
  This fits §14's explanation of the inconclusive 10 MΩ reading.
- **R8 = `472`** (4.7 kΩ), plausibly the RXD pull-up. Unverified.
- The header has 8 pins. **Pins 1–3 are a `HOST` / common / `SLAVE`
  jumper block** (cap 1–2 = HOST), separated by silkscreen from
  `GND / 3V3 / SLP / WAKE / INH` on pins 4–8. There are **no RX or TX pins
  on the header**, so those stay on the screw terminal. `HOST` is presumed
  to switch a 1 kΩ + diode master pull-up onto `LIN`. This is inferred
  from the labels, not traced.

Photos: `docs/images/tja1021-breakout.png` (whole board),
`docs/images/tja1021-breakout-u2.jpg` (chip and passives).

### Decisions added

| # | Decision | Rationale |
|---|----------|-----------|
| 17 | **Use the plain breakout; the U7X54911 becomes a fallback** | No MCU means decisions 14–16 (reset wire, leg soldering, 1 kΩ series resistors) are unnecessary, and the risk of an MCU transmitting on the car's bus is gone |
| 18 | ESP32 wires to the **screw terminals and `SLP` header pin directly** | GPIO17→`TX`, GPIO16→`RX`, GPIO21→`SLP`, common GND. Nothing else drives those lines, so no series resistors. *Pin numbers superseded by decision 21 (ESP32-C6)* |
| 19 | **Run the pull-up load test with the cap on HOST, on SLAVE, and off** | One test shows what the `HOST/SLAVE` jumper actually does *and* settles the pull-up value, rather than trusting the silkscreen |

## 17. The ESP32 is an ESP32-C6 — firmware retargeted and flashed

Choosing pins exposed it: GPIO21 and GPIO22 were not available, and the
board breaks out **0–9, 14–16, 18–20**. An original ESP32 has no GPIO20 and
cannot free 6–11 (flash), so the board could not be the `esp32` target the
firmware was built for. The USB port enumerated as COM14 with Espressif's
own ID `303A:1001` (built-in USB), and `esptool chip-id` settled it:
**ESP32-C6FH4 (QFN32) rev v0.2**, 4 MB embedded flash, USB-Serial/JTAG.

Changes:

- `sdkconfig.defaults`: target `esp32c6`, `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`,
  4 MB flash, TX 18 / RX 19.
- `sdkconfig.ncv.defaults`: EN 20, bypass MOSFET gate 14.
- `main.c`: the REPL uses `esp_console_new_repl_usb_serial_jtag()` when
  the console is USB-Serial/JTAG; the UART REPL remains for the classic
  ESP32. Nothing else in the code needed touching: the K-line stays on
  UART1, which the C6 has, and `U1TXD_OUT_IDX` exists in its signal map.
- Retargeted with `idf.py set-target esp32c6` on `build_ncv`, which
  regenerates `sdkconfig.ncv` and clears the build directory itself.
  The plain `build/` is still classic-ESP32 and stale.

**Result:** builds clean, 213 KB with 79 % of the app partition free.
Flashed over COM14 with hash verified. `status` answers on the console and
reports TX 18, RX 19, EN GPIO20 (high), init bypass GPIO14, UART1,
10400 baud, TX compiled in, not armed. The boot banner was not captured:
the USB port drops during reset and the script reconnected too late.

### Decisions added

| # | Decision | Rationale |
|---|----------|-----------|
| 20 | **Retarget to ESP32-C6** rather than buy a classic ESP32 | It is the board in hand; the code was portable except the console |
| 21 | **TX 18, RX 19, EN/SLP 20, MOSFET gate 14** | All free on this board. Avoids the C6 strapping pins 4, 5, 8, 9 and 15 (on SuperMini-style boards 8 and 15 are often LEDs too) and UART0 on 16/17 |
| 22 | **Console over built-in USB** | The C6 enumerates as its own COM port; no USB-UART bridge on the board |

## 18. Passive build retargeted, and an EN bug caught

The plain `build/` (TX compiled out, for Phase 4 in the car) was
retargeted with `set-target esp32c6` and builds (206 KB, 80 % free).
Its generated config showed **`CONFIG_RRP38_EN_GPIO=-1`**: EN lived only
in `sdkconfig.ncv.defaults`, so the passive build would never have raised
SLP. The TJA1021 would have stayed asleep and `sniff` would have seen
nothing in the car, which Phase 4 explicitly accepts as a possibly quiet
bus. EN=20 now sits in the shared `sdkconfig.defaults`;
`sdkconfig.ncv.defaults` keeps only the MOSFET gate and `ALLOW_TX`.

| # | Decision | Rationale |
|---|----------|-----------|
| 23 | **EN=20 in the shared defaults** | The transceiver needs SLP high in every build, passive included. A missing EN is a silent failure that looks exactly like a quiet bus |

## 19. Web HMI and OTA — planned

Raised at the end of the session: a web interface on the C6 for use from a
phone, and OTA updates. Planned, not implemented. Full plan in
[PLAN.md, Track H](PLAN.md#track-h--web-hmi-and-ota--done).

The template is **RemoteStart's SlaveWallas**, which already runs on an
ESP32-C6 under IDF v6.0.1: dual-slot partitions, `version.h` into the app
descriptor, a web server with WiFi portal, `/ota/upload`, install from
GitHub Releases, a Firmware Versions card, and the release and web-flasher
workflows. BirdBox has a similar setup but more of it; SlaveWallas is the
closer fit.

| # | Decision | Rationale |
|---|----------|-----------|
| 24 | **Copy SlaveWallas's OTA and web server** rather than write new ones | Same chip, same IDF version, already proven in use |
| 25 | **Web HMI is read-only at first**: status, live sniff, OTA | A web page that can transmit lets anyone on the AP talk to the BeCM. `arm`/`send`/`slowinit`/`fastinit`/`scan` stay on the USB console until at least Phase 5 |
| 26 | **WiFi AP + STA** | No router in the car, so the board runs its own WPA2 AP for the phone; it also joins home WiFi when in range, which GitHub-release OTA needs |
| 27 | **Each release carries a passive and a TX binary** | OTA can then switch between the Phase 4 passive build and the TX build without USB |

Open checks for when it is built: K-line timing with WiFi running on the
single-core C6, and the bus staying released through an OTA reboot.

## 20. Track H steps 1–3 — built and flashed

Same session, straight after the plan. Adapted from SlaveWallas, not
copied wholesale: its web server is 1600 lines of Wallas-, timer- and
ESP-NOW-specific code, so only the generic handlers came across.

| Piece | Where | Taken from SlaveWallas |
|---|---|---|
| Dual-slot partitions | `partitions.csv` | Verbatim |
| Version into the app descriptor | `main/version.h`, top-level `CMakeLists.txt` | Verbatim mechanism; version **0.2.0** |
| WiFi | `main/net.c` | NVS credentials, mDNS, `WIFI_PS_NONE`. **Changed:** AP always on instead of a fallback portal |
| Web server | `main/web_server.c` | Form decoding, scan, OTA upload loop, `/api/version` with the inactive slot's version, httpd settings. **New:** page, `/api/status`, AP password route |

`net.c` differs from SlaveWallas in one behaviour worth knowing: AP+STA
shares one radio, and each STA connect attempt scans all channels, which
drops the phone's link for a moment. So STA retries are five quick ones,
then one a minute, and **none at all while a phone is on the AP**. In the
car, where home WiFi is never in range, the board never scans while you use
it.

**Safety additions:**

- `kline_release_bus()` disarms, drops the MOSFET gate and SLP, without
  touching the UART. Every reboot from the web side (OTA, settings save,
  reboot button) calls it first. `kline_stop()` now calls it too.
- No route arms, inits, sends or scans (decision 25). The page says so.
- The AP password is changeable from the page (NVS key `appass`); the
  default `rrp38kline` is printed in the README, so change it.

**Build:** both configs build clean, about 1.02 MB each, 46 % of the
1.875 MB slot free; WiFi is most of the growth from 213 KB. The generated
configs had to be regenerated (`set-target esp32c6` again) because
existing `sdkconfig` files do not pick up new defaults such as the custom
partition table.

## 21. A silent access point, fixed by a full erase

After flashing, the boot log showed `AP "RRp38-KLine" up at 192.168.4.1`
and the HTTP server started, but the network never appeared in the PC's
WiFi list over 2½ minutes, while BirdBox's `BirdAudio-Config` did.
RemoteStart's FSD §11.1 records exactly this: a board flashed over older
firmware whose *"logs reported a running SoftAP while its radio
transmitted nothing"*, cured by a full erase. This C6 came with unknown
firmware on it.

`esptool erase-flash`, then the same flash: **`RRp38-KLine` appeared
within 10 s**, WPA2, channel 1, 99 % signal.

| # | Decision | Rationale |
|---|----------|-----------|
| 28 | **Always erase fully on a board's first flash** | Twice now (RemoteStart, here) leftover flash contents produced a radio that claims to be up and is silent. A plain flash only writes its own regions |
| 29 | **STA retries pause while a phone is on the AP** | Each retry scans every channel and drops the phone; in the car home WiFi is never there to find |

**Not yet tested:** the page itself and an OTA upload. Joining the AP from
this PC would cut it off the home network, so that test is for a phone.

## 22. Phone test, first OTA, and the sniff view (Track H step 4)

The page works on a phone. The board was then put on home WiFi
(`192.168.10.218`), so this PC can reach its API directly.

**First OTA, over home WiFi:** v0.3.0 uploaded with `curl` to
`/ota/upload`. 1.07 MB in 4 s, the board answered again 2 s later with
Installed 0.3.0, Stored 0.2.0, running from `app1`. Step 3 is verified.

**Step 4, the sniff view:**

- `main/sniff.c` replaces `kline_sniff()`. One capture loop for both the
  console and the page. It splits on gaps as before, keeps the last 64
  frames (up to 48 bytes each; longer ones are marked with their real
  length) in a ring buffer, and prints in the old format when run from the
  console.
- **Sniff** tab: duration (1 to 60 min, default 10) and split gap, Start
  and Stop, the frames live, and Download as `.txt`. `GET /api/sniff?since=N`
  returns new frames in batches of 32; `POST /api/sniff/start|stop`.
- **Bus claim** (`kline_claim()` / `kline_unclaim()` / `kline_owner()`):
  whoever reads or writes the UART holds it. The console's `slowinit`,
  `fastinit`, `scan` and `send` claim it for their whole run, and `sniff`
  claims it inside `sniff_run()`. It is a binary semaphore rather than a
  mutex, because a web capture is claimed from the HTTP task's request
  and released by the sniff task when it ends.
- Web Stop ends a console `sniff` too. The console cannot stop its own
  sniff before its time is up, so this is useful.
- The capture task runs at priority 6, above the HTTP server, so a busy
  page cannot delay byte reads. Timestamps are taken when the task reads a
  byte, not when it arrived, so WiFi adds a few ms of jitter near the
  split threshold.

**Verified:** start → `running`, owner `web`, time counting down; a second
start → HTTP 409 `K-line busy (web)`; the console's `slowinit 33` and
`sniff` refused with the same message during a web capture; Stop frees the
bus and a console `sniff` then runs; a 3 s capture ends by itself.
**Not verified:** the frame display, because nothing drives RX yet.

A tooling note: the `console_check.py` helper resets the board on purpose
(to catch the banner). A second helper, `console_cmd.py`, opens the port
with DTR/RTS held low and does not reset it. The first test of the claim
was spoiled by using the wrong one.

| # | Decision | Rationale |
|---|----------|-----------|
| 30 | **One sniffer for console and web, plus a bus claim** | Two readers on one UART would each get half the bytes. A claim, named by owner, makes the refusal explicit |
| 31 | **Web captures stop themselves** (default 10 min, max 60) | A forgotten capture would otherwise hold the bus and lock out the console indefinitely |

## 23. Install from GitHub and release CI (Track H steps 5–6), v0.4.0

**The repo was made public** (decision 32). It had been private, which
breaks the RemoteStart pattern: the board fetches the release listing and
the `.bin` without credentials, and Pages is not free for private repos.
Before the switch the whole history was scanned for secrets: the only
password is the documented AP default, the only personal detail the commit
email RemoteStart already publishes, and the home network name is not in
the repo. Pages was then set to build from Actions.

**Board side** (`web_server.c`, adapted from SlaveWallas):

- `GET /api/ota/github/releases` streams the GitHub listing byte by byte
  (no JSON tree, so its cost does not grow with the release count) and
  returns every `RRp38KLine_<variant>_v<ver>.bin`, both variants.
- `POST /api/ota/github/install` (form body `url`, `size`) accepts only
  this repo's release-download path, then `esp_https_ota` runs in its own
  task with SlaveWallas's 4 KB TX buffer for GitHub's 2 KB signed redirect.
  `GET /api/ota/github/progress` gives percent and an HTTP event trace.
- `/api/version` now reports `variant` (`passive` / `tx`). The OTA tab
  preselects the board's own kind and warns when an install would switch
  build.
- Both OTA paths return **409 while the K-line is claimed** (§22).
- `CONFIG_LWIP_MAX_SOCKETS=16`, `max_open_sockets=7`: RemoteStart's fix
  for TLS fetches failing when polling browsers held every socket.
- Image grew to 1.24 MB (TLS, certificate bundle); 36 % of the slot free.

**CI:**

- `release.yml`: a `v*` tag builds passive and tx in a matrix, **fails if
  the tag ≠ `FIRMWARE_VERSION`**, and attaches both images. The tx config
  is `sdkconfig.defaults` + `sdkconfig.ncv.defaults` concatenated into one
  file, so the build command needs no quoted `;` list. A local build with
  that exact command matched `build_ncv` in size and config; the binaries
  differ only in the timestamp.
- `webflash-pages.yml` + `docs/webflash/`: esp-web-tools page with a
  passive and a TX button, erase prompt on, built from `master`.

**Verified, end to end:**

| Check | Result |
|---|---|
| Push `master`, tag `v0.4.0` | Both workflows green; builds ~3¼ min each |
| Release assets | `RRp38KLine_passive_v0.4.0.bin` 1 263 200 B, `RRp38KLine_tx_v0.4.0.bin` 1 266 848 B |
| Pages | <https://sespe.github.io/RRp38KLineInterface/> HTTP 200, both manifests served |
| Board lists releases | Both images, from the board itself |
| Install passive from GitHub | ~13 s, rebooted into `app1`, status `txBuild:false`, MOSFET pin −1 |
| Install tx from GitHub | Back to `txBuild:true`, MOSFET pin 14 |
| Install or upload during a web sniff | HTTP 409 `K-line busy (web)` |
| Install URL from another host | HTTP 400 |
| Heap low-water after all this | 240 KB |

Not tested: flashing a board through the Pages flasher (needs Web Serial
in a desktop browser).

CI warnings, non-blocking: `actions/checkout@v4` and `upload-artifact@v4`
run on deprecated Node 20, and `ubuntu-latest` moves to Ubuntu 26 from
2026-10-19. RemoteStart's workflows have the same.

| # | Decision | Rationale |
|---|----------|-----------|
| 32 | **Repo public** | Same pattern as RemoteStart: credential-free release fetch and free Pages. History checked for secrets first |
| 33 | **Release tag must equal `version.h`** | The board compares versions from asset names; a mismatched tag would publish an image that reports a different version than its name says |
| 34 | **OTA refused while the K-line is claimed** | The reboot at the end of an install would cut a capture or session off mid-frame |

## 24. Where this stands — stopping point, 2026-10-16

**Both halves of the interface are in hand and the software side is
finished for now.** A genuine NXP TJA1021 breakout with no MCU in the way,
and an ESP32-C6 running v0.4.0 with a web page, live sniff view and OTA
from GitHub releases. Nothing is blocked on a decision. The next steps are
bench measurements on the transceiver board.

**Settled this session:**

- **Transceiver:** a plain TJA1021 breakout (1021T2C, §16) replaces the
  MCU-fronted U7X54911. Header mapped: pins 1–3 are the `HOST`/`SLAVE`
  jumper block, `SLP` is pin 6, and RX/TX are only on the screw terminal.
- **ESP32:** identified as an ESP32-C6FH4 (§17). Firmware retargeted,
  console on built-in USB, pins TX 18 / RX 19 / SLP 20 / MOSFET gate 14.
  Every build now drives EN (§18).
- **Track H, web HMI and OTA, done** (§19–§23, decisions 24–34):
  - always-on access point `RRp38-KLine` at 192.168.4.1, plus home WiFi
    (the board is at 192.168.10.218);
  - Status, Sniff, WiFi and OTA tabs, read-only towards the K-line except
    for starting and stopping a passive capture;
  - a bus claim so console commands and a web capture never share the
    UART, and no OTA while the line is in use;
  - releases from a `v*` tag with a passive and a tx image each, installed
    from the board itself; a browser flasher on GitHub Pages;
  - the repo is now public.
- **First flash of any board needs a full erase** (§21, decision 28).
- **Bench test procedure written down** in
  [HARDWARE.md — Bench testing before the car](HARDWARE.md#bench-testing-before-the-car):
  six steps with expected readings and a failure table, replacing the old
  four-line version. Linked from PLAN Phase 3 and the README.
- **README encoding repaired** (`86b8bd3`). A `perl -0pi` edit in
  `0c23007` read the file as Latin-1 and wrote it back as UTF-8, so all 13
  dashes and arrows showed as `â` + garbage on GitHub. Fixed by decoding
  only the runs that round-trip back to valid UTF-8; the result matches the
  README before that commit character for character. No other tracked file
  was affected. Docs are edited with the editor tool from now on, not perl.

**Next moves, in order.** All on the bench, none near the car; the full
procedure is the HARDWARE.md section above:

1. **Board checks, unpowered side first:** 12 V on `VCC`/`GND`, nothing on
   the logic side. `RX` terminal must read ~3.3 V, not 5 V. Note `TX` and
   `SLP` open-circuit voltages.
2. **Pull-up load test**, jumper cap on `HOST`, on `SLAVE`, then none:
   `LIN`→`GND` voltage, then again with 1 kΩ `LIN`→`GND`. Decides between
   adding 1 kΩ in parallel and fitting the full 510 Ω.
3. **Wire it:** GPIO18→`TX`, GPIO19→`RX`, GPIO20→`SLP`, GND→`GND`.
   `status`, then `arm --yes` and `send 55`: a clean echo is Phase 3's exit
   criterion.
4. **With WiFi up:** repeat the echo test, then a web capture while
   `send`ing from the console is refused (expected), then fast init.

**Small things for the web side:**

- **Change the AP password.** The default `rrp38kline` is now public.
- The Sniff tab's frame list has not yet shown a frame. Touching GND to
  GPIO19 during a capture should produce `00` frames; real traffic will
  come with the transceiver.
- New release: bump `FIRMWARE_VERSION`, commit, push, tag `v<same>`.

**Still waiting on:** bypass MOSFET parts (2N7002/BSS138, 1 kΩ, 100 kΩ),
510 Ω, TVS, buck converter, inline fuse; the KKL 409.1 cable; and **the
EKA code**.

**Still open from §13:** whether fast init survives the TJA1021's 27 ms
guaranteed minimum in practice. Settle it once the echo test passes.

## 25. Waveform sampler, OTA deploy, and an RX pin that reads low — stopping point, 2026-10-17

**Built and deployed this session.** A pin sampler, `main/waveform.c` and
`main/waveform.h`, with the console command `wave [-p rx|tx] [-t ms]` and a
web Debug tab (`/api/wave`) that takes the same syntax in its command box.
Committed as `acfec62`. The project's own functional spec is now
`FSD_RRp38KLineInterface.md` at the repo root; it replaces the earlier
reference to RemoteStart's FSD.

- Samples a GPIO input for 1 to 200 ms, records each level change against
  the CPU cycle counter, and reports level at start, high and low time,
  shortest complete pulses, and an estimated baud rate. Holds the bus claim,
  so it refuses during a capture, console command or OTA.
- **Deployed by OTA** to the board at **192.168.10.218** (home WiFi). The
  access point at 192.168.4.1 was not reachable from the PC, which was on
  home WiFi. Image: `build_ncv/RRp38KLineInterface.bin`, 1,274,224 bytes,
  tx variant. The reboot went to `app1`. Version string still reads 0.4.0,
  so the new image can't be told apart by version. Bump it before the next
  release if that matters.
- Console `wave` was checked over COM14 (TX sample, correct format). The web
  and console RX readings agree.

**Limits, settled.** The tab is a digital-level diagnostic. It cannot show
rise time, overshoot or ringing: the polling loop's timing is an estimate of
1 to 2 µs that I haven't measured, and the input is a logic threshold. Edge
shape needs a scope. The ADS1220 breakout offered on eBay is a 24-bit
delta-sigma part at roughly 2 kSPS, so it suits DC and slow levels only.

**Bench finding, still open.** RX (GPIO19) reads a steady low for the full
50 ms window with no edges. TX (GPIO18) reads high, as it should.

- **Wiring confirmed by continuity:** board RX terminal to GPIO19, board TX
  terminal to GPIO18. The user calls the whole bus side of the breakout
  "LIN" (12 V, LIN, GND pads); the logic side is GND, RX, TX.
- **EN (GPIO20)** is driven high at boot. `kline_release_bus` lowers it
  only before a reboot, so the transceiver should be enabled.
- **The "unplugged" test was not clean.** The RX wire was still on the
  board end at first, as the user clarified later. Redo it with the wire
  off both ends before drawing a conclusion.

**Gotcha worth remembering:** opening COM14 resets the ESP32-C6, because
DTR toggles on open. Set `DtrEnable` and `RtsEnable` to false before
opening, and the console stays up.

**Next moves, in order:**

1. Disconnect the RX wire from GPIO19 at both ends. Run `wave -p rx -t 50`
   on COM14 with DTR and RTS off. High means the transceiver or its wire is
   holding RX low. Low means the board is holding it low.
2. Bench supply at 12 V, current limit 0.2 to 0.5 A, on the bus side (VCC
   and GND). Keep LIN open to the car. Join the bench ground to the board
   ground. Measure the board's RX terminal to GND: about 3.3 V is expected.
   Near 0 V points at the 4.7 kΩ RXD pull-up or the transceiver's mode.
3. Put the RX wire back on GPIO19 and repeat `wave -p rx`.
4. Confirm the 510 Ω pull-up from 12 V to LIN is fitted (HARDWARE.md).
5. Digital scope: separate project. Minimum 10 MHz, 2 channels. Build or
   buy is still open. Buying is the practical route; a 2204A-class or
   100 MHz USB scope fits the spec.

**Still waiting on:** the EKA code, the bypass MOSFET parts, 510 Ω, TVS,
buck converter, inline fuse and KKL 409.1 cable (§24).

**Unverified, do not treat as checked:**

- The Debug tab's page JavaScript has not been syntax-checked (no Node on
  this PC), and the page has not been opened in a browser.
- The on-chip ADC sample rate of the ESP32-C6 has not been checked against
  the datasheet, and no part of this session relies on it.
- The ~1 to 2 µs sampling resolution is an estimate, not a measurement.

**Decisions**

| # | Decision | Rationale |
|---|----------|-----------|
| 35 | **Waveform sampler reads, never drives, and takes the bus claim** | A sample during a capture, console command or OTA would mix readers on one line; the claim makes the refusal explicit |
| 36 | **The waveform tool is a digital diagnostic, not an analog scope** | A logic threshold and a polling loop cannot show slope, ringing or overshoot. Those need a scope, and the tab says so |
| 37 | **Console `wave` and the web Debug tab share one syntax and one route** | One command, two front ends. The web command box only sets the pin and window, then runs the same sample |
| 38 | **Digital scope is a separate project, minimum 10 MHz, 2 channels** | The interface stays passive and read-only; the scope gets its own front end and display |
| 39 | **Deploy to the board by OTA over home WiFi; COM14 is for recovery and console** | The board is reachable at a fixed home address, and a serial session resets the board unless DTR and RTS are held off |

## 26. Bench procedure run on the plain breakout — stopping point, 2026-10-17

**Steps 1 to 4 of the bench procedure (HARDWARE.md) are done** on the plain
TJA1021 breakout, with 12 V on `VCC`/`GND`, the car side open, and the
ESP32 disconnected. Nothing has been wired to GPIO18, GPIO19 or GPIO20 yet.
The next move is the wiring step (§24, step 3), not the car.

**Readings, in order:**

| Check | Reading | Verdict |
|-------|---------|---------|
| RX terminal to GND | 3.3 V (GPIO19 wire off) | Pass. Logic side is 3.3 V, safe to connect |
| TX terminal to GND, open | 3.3 V (first reading 0 V, probe contact) | Pass. TXD pulled up |
| SLP to GND, open | 3.2 V | Pass. The board holds the chip awake by itself |
| `HOST` cap, LIN open / 1 kΩ | 11.3 V / 1.8 V | Pull-up present, about 5 to 6 kΩ (estimate) |
| `SLAVE` cap, LIN open / 1 kΩ | 10.7 V / 0.3 V | Chip termination only, about 35 kΩ |
| No cap, LIN open / 1 kΩ | 10.45 V / 0.3 V | Chip only. First open reading 2.0 V was a bad contact; re-measured |

**Pull-up fitted:** 560 Ω from the 12 V rail to `LIN`, in parallel with the
`HOST` pull-up (not to GND). Readings with the `HOST` cap on:

- LIN open: 11.6 V
- LIN with 1 kΩ to GND: 7.0 V

From those, the total source resistance is about 650 Ω. That is a little
above the 500 Ω target, and it was accepted as is. Adding a second part to
reach 500 Ω was judged not needed. No further change to the bus side.

**Correction to the procedure advice.** The step-4 advice given during
this session said a 1 kΩ test after fitting should read 2 to 3 V. That was
wrong. The 2 to 3 V band describes the pull-up *before* fitting. A total of
about 500 Ω gives about 7.7 V on the 1 kΩ test, so 7.0 V is on target. HARDWARE.md
should be corrected the next time it is edited.

**Not recorded:** the bench supply's current draw at 12 V. It was asked for
several times and not given. Take it on the next run.

**Still open:**

- **RX on GPIO19 is still unexplained.** The §25 steady low was not
  reproduced in this session: the wire was off and the line read 3.3 V.
  The high-with-no-edges sample at the start of this session, before the
  bench work, did not record the wire state, so it is not a clean test.
  Reconnect and repeat `wave -p rx`.
- **LAN-only operation** (AP off, home WiFi only) is still undecided. Options
  A, B and C are in the previous session's reply; no code has changed.
- The 12 V current draw (above).

**Next moves, in order:**

1. Switch the bench supply off. Wire GPIO18 to `TX`, GPIO19 to `RX`, GPIO20
   to `SLP`, and GND to `GND`. The `HOST` cap stays on.
2. Switch on. Run `status` on the console (DTR and RTS held off) and check
   `kline` reports the pins and EN as expected.
3. `arm --yes`, then `send 55`. A clean echo is Phase 3's exit criterion.
4. With WiFi up, repeat the echo test over the web, check that a web capture
   refuses `send` while running (expected), then try fast init.

**Decisions**

| # | Decision | Rationale |
|---|----------|-----------|
| 40 | **Master pull-up is 560 Ω from 12 V to `LIN`, in parallel with the `HOST` pull-up** | Measured total is about 650 Ω, within the usual K-line range. Adding more was judged unnecessary |
| 41 | **Bench procedure steps 1 to 4 are signed off on the plain breakout** | Every reading matches the expected band, apart from the two readings that were re-taken for contact |

## 27. Echo test passes on the new breakout — stopping point, 2026-10-17

**Phase 3's exit criterion is met.** On a new TJA1021 breakout, with the
bench wiring from §26 (GPIO18 to `TX`, GPIO19 to `RX`, GPIO20 to `SLP`, GND
to `GND`), 12 V on `VCC`, the `HOST` cap on, a 560 Ω from 12 V to `LIN`, and
nothing connected to the car, every byte sent came back on RX and was
consumed by `kline_write`:

- `send 55`, `send FF`, `send 00`, `send A5`, `send 5A`: each one printed
  its `TX:` line and no echo error.
- `send 81 12 F1 81 00`: the five-byte frame echoed cleanly.
- `bbsend 55`: bit-banged on GPIO, echo `55` (match).

The `RX: (no response within 500ms)` line after each send is expected: there
is no ECU on the bench, so nothing answers. It is not an echo fault.

**What was wrong, in order.**

1. **Stale RX before each byte.** `kline_write` wrote without clearing the
   UART receive buffer, so a late echo was read as a later byte's echo. Fixed
   in `60f5131`: `kline_flush_rx()` before each write. This is why the first
   retries showed `0x00` and `0x55` for the wrong bytes.
2. **The first board's RX path.** On the first breakout, RX at the terminal
   never went low enough to read as a 0. With the transmitter pulsed by
   `txlow`, the minimum on the `RX` terminal was 1.277 V with GPIO19
   connected and 1.9 V with it unplugged. The idle level was 3.3 V. The
   3.3 V logic supply stayed at 3.3 V under load. The `RX` terminal showed
   2.0 V to `3V3` and 3.3 V to `GND` at one stage, which is a divider, not a
   clean pull-up. The first breakout was replaced. This is the working
   hypothesis for its fault; it was not confirmed by a scope or by a second
   measurement on the old board.
3. **The `send` path itself was fine.** A GPIO18 to GPIO19 loopback
   (with the transceiver unplugged from both pins) echoed `55` through
   `bbsend` and gave no error from `send`. The UART TX and RX paths on the
   ESP32 both work.

**Bench-only tools added this session.** These are in the working tree
(`main/kline.c`, `main/kline.h`, `main/kline_console.c`), not yet committed:

- `txlow [n]`: `n` pulses of 20 ms low and 20 ms high on TXD, `1..100`,
  default 10. Under the bus claim; armed only. The 20 ms low stays under the
  TJA1021's 27 ms dominant timeout. A multimeter on `LIN` dips during the
  run, which showed the transceiver drives the bus.
- `bbsend <hh>`: one byte bit-banged on GPIO at 10400 baud, then its echo
  read. Armed only.

Both are gated by `arm` and `CONFIG_RRP38_ALLOW_TX`, and both were built into
`build_ncv` and deployed by OTA. The board on the bench runs this build,
variant `tx`, and reports `0.4.0` in `/api/version`, which doesn't change
between builds.

**Not recorded:**

- The bench current draw at 12 V (asked for several times; never given).
- The MIN reading on the new breakout's `RX` terminal during `txlow`. The
  echo test passed, so it is no longer the deciding measurement, but the
  number is still missing from the log.

**Still open:**

- **LAN-only operation** (AP off, home WiFi only): options A, B and C from
  the earlier session are unchanged; nothing has been built.
- **Scope:** the digital scope is still a separate project (§25 decision 38).
  The echo test does not need one.
- **Wiring to the car** has not been done, and the car has not been
  touched. The next move is the car test, not more bench work.

**Decisions**

| # | Decision | Rationale |
|---|----------|-----------|
| 42 | **Clear the UART receive buffer before every byte written** | The echo of a byte that timed out must not be read as the echo of a later byte. The fix is one call and costs nothing on the bus |
| 43 | **Bench-only `txlow` and `bbsend` stay in the firmware, behind `arm` and `ALLOW_TX`** | They are the only way to test the transceiver and the GPIO path without a car. Both are refused when unarmed and are compiled out when `ALLOW_TX` is off |
| 44 | **Replace the first breakout rather than chase its RX path** | Its RX could not be brought to a clean low, and a new breakout passed the same test with no other changes |

## 28. Second breakout fails, first passes again — stopping point, 2026-10-18

**The echo test is the bench gate, and it is passing on the first breakout.**
On 2026-10-18, with the wiring from §26 and 12 V on `VCC`, the 560 Ω from
12 V to `LIN` and the `HOST` cap on, every byte sent came back on RX:

- `send 55`, `FF`, `00`, `55`: each printed its `TX:` line with no echo error.
- `bbsend 55`, `FF`, `00`: each read back the same byte (match).

**The second breakout failed the same test.** It was fitted in place of the
first, with the same wiring and the same 560 Ω. Result: `send 55` gave
`no echo for byte 0 (0x55)`, and `bbsend 55` gave `ESP_ERR_TIMEOUT`. Idle RX
and TX were 3.3 V, so the ESP32 side was not at fault. Before this it had
been replaced after an RX-low fault (§26). It was not checked further, and
the cause is not known. It had been powered with 12 V on `LIN` during a
wiring mistake, so that is one candidate; a fault from new is another.

**Update, later on 2026-10-18: both breakouts pass.** The board wired at
the time of the check passed the echo test (`send 55` with no echo error,
and `bbsend 55` matched). The user reports both boards OK. Which board was
in for that check was not recorded at the time, so the second breakout's
earlier failure is still unexplained. It has not been re-tested as its own
board, and the note above stands as the record of what was seen.

**The first breakout failed once, then passed.** On the swap back, the first
breakout's `send 55` and `bbsend 55` failed in one session and passed in the
next, with no change to the wiring recorded. The failure has not been
explained. The likely causes are a loose wire or terminal on the bench, or an
intermittent joint on the breakout. The fault has not recurred across four
sessions of testing since, which is the best evidence so far. Treat it as
unresolved until the car test runs without it.

**Not changed this session:**

- No firmware changes. The working tree has `main/kline.c` with the W4
  comment edit (not mine, not committed), and the bench commands are in
  `965e636`.
- The 12 V current draw at the bench is still not recorded.

**Still open:**

- **Car test.** Nothing has been connected to the car. Use the first
  breakout, with the bench echo test run immediately before each car session.
- **LAN-only operation** (AP off, home WiFi only): options A, B and C are
  unchanged; nothing has been built.
- **Second breakout:** keep it out of the car. Its cause is not known.

**Decisions**

| # | Decision | Rationale |
|---|----------|-----------|
| 45 | **The first breakout is the car-test board** | It passed the echo test on every session except one, and the one failure has not recurred. The second has failed and is not trusted |
| 46 | **Run the bench echo test before each car session** | An intermittent fault is only caught if the test runs first; a passing echo is the only gate we have |

## 29. Webasto Thermo Top V — possible K-line device, not verified — 2026-10-18

Raised as a question: does a Webasto Thermo Top V heater use the K-line?
Checked against two sources. Neither is manufacturer documentation.

- **Name and bus.** [experimental-engineering.co.uk, post 4599](https://www.experimental-engineering.co.uk/?p=4599)
  calls the heater's diagnostic interface the **W-Bus** and says it runs
  over the K-line. A [UK kit seller](https://fnltd.co.uk/shop/diagnostic-kit-for-webasto-d3-d4-rrs-thermotop-v-vevo-p66.html)
  sells a Thermo Test kit with a USB-to-OBD2 adapter for "K-line
  communications".
- **Wiring.** The blog gives OBD pin 16 to the heater's positive supply and
  OBD pin 7 (K-line) to heater pin 2, "valid for all TT-V heaters". The
  author tested it on their own unit only.
- **Not stated anywhere checked:** baud rate, protocol standard (ISO 9141,
  KWP2000 or other), init sequence. The W-Bus is not identified as ISO 9141,
  so the GEMS init sequence in `kline.c` should not be assumed to apply.
- **Vehicle list.** The kit's vehicles are Discovery 3, Discovery 4 and
  Range Rover Sport, 2005 to 2013. The P38A is not on it.

**Status:** a lead, not a fact. Not connected, and not part of the car
test. Before wiring the heater to this interface, get the W-Bus protocol
specification from Webasto service documentation or a verified source.

**Safety:** the K-line is shared with the car's modules. `send` and
`bbsend` transmit, so a test against the heater on the car's bus could
disturb the P38 ECU. Keep the heater off this bus until its protocol is
confirmed.

**Update: the W-Bus framing does not match our K-line.** A summary of the
[H4jen/webasto repository](https://github.com/H4jen/webasto) (a fetch
summary, not read line by line) says:

- The W-Bus is a K-line, citing ISO 9141, with 0 to 12 V signalling. LIN
  is not mentioned.
- Serial format: **2400 baud, 8E1** (even parity). Our K-line is 10400
  baud, 8N1, the ISO 9141 setting in `kline.c`. The W-Bus would not work
  with the current firmware settings even if the wiring matched.
- Init: fixed packets such as `F4 03 51 0A AC`, with `F4` on requests and
  `4F` on responses. That is not the ISO 9141 5-baud init we implement.
- Tested on an Air Top Evo 40 in a boat cabin. The repository does not say
  whether any other heater, or any car, was tested.

So the W-Bus is a K-line in the physical sense, but its framing and init
differ from ours. Section 29's "protocol not verified" still stands, and
the heater's bus cannot share our K-line settings.

**Decision, 2026-10-18: the Thermo Top V is dropped.** The heater is not
part of this project. The interface stays on the P38's K-line: the car test
continues on the first breakout, with the bench echo test run before each
session. The heater's W-Bus, its CAN lines (pins 4 and 7) and its K-line
pin (pin 2) are not to be connected to this interface. If a heater is ever
taken up again, it needs its own protocol capture and its own design note.

## 30. Ready for the car, passive sniff first — stopping point, 2026-10-18

**Bench is done. The car is next, and the first session is passive only.**

**Bench state at the end of 2026-10-18:**
- The first TJA1021 breakout passes the echo test (`send 55`, `bbsend 55`)
  with the bench wiring and the 560 Ω pull-up from 12 V to `LIN`.
- The second breakout failed the same test on 2026-10-18, and later both
  passed. Which board was in for the passing check was not recorded. For
  the car, use the first breakout only.
- The board runs the `tx` build (`0.4.0`, running `app0`), transmitter
  disarmed. The bench-only `txlow` and `bbsend` are in `965e636`.

**Car-side status:**
- **EKA code:** available. It is kept out of the repo and these notes.
- **Power:** 12 V from the car's diagnostic socket (pin 16) through an
  inline fuse to the transceiver's `VCC`. The ESP32 runs from a powerbank,
  not the PC's USB (corrected in §32; the earlier "from the PC's USB" was
  wrong).
- **Ground:** the car's ground (socket pin 5) goes to the board's `GND`.
  Whether the PC's ground should be isolated from it is still open; use a
  battery laptop, or an isolator, if one is available. (With the powerbank
  powering the board, the PC is not on the board's supply; see §32.)
- **Not yet confirmed:** ignition state, which path the PC uses to reach the
  board, and whether the fuse is in place. These were asked and not answered.

**Plan for the first session:**
1. Remove the 560 Ω from 12 V to `LIN`, and set the `LIN` jumper to
   `SLAVE` (chip termination only). The car's own pull-up is the one that
   should be on the line. Re-check the wiring before the car is plugged in.
2. Ignition off. Start `sniff` (console or the web Sniff tab) and record
   whether the bus is quiet.
3. Ignition on. Record whether the bus wakes and what it carries.
4. Then, with the sniff running, connect the Nanocom and run a read, so its
   requests and the ECU's replies can be matched afterwards.

**Rules for this session:**
- **Passive only.** Do not run `arm`, `send`, `slowinit`, `fastinit` or
  `scan`. The sniff only listens, and the transmitter stays disarmed.
- **Do not arm while the Nanocom is talking.** Both would share the K-line.
- **EKA stays available** in case the ECU or immobiliser needs recovery.
  Sending from this interface is not part of the first session.

**Still open:**
- The bench current draw at 12 V (asked for many times; never given).
- LAN-only operation (AP off, home WiFi only): options A, B and C from the
  earlier session are unchanged; nothing has been built.
- The Thermo Top V is dropped (section 29).

## 31. Web echo test, waveform capture and per-byte logging — 2026-10-18

**The web now transmits, on one request, with a confirmation.** This
reverses the earlier rule that the web only listens. It was a deliberate
choice, made so the bench wiring can be tested from the Debug tab. The rule
that replaces it: the web can send only the echo test, only with the exact
confirmation `confirm=car-unplugged`, and only when the bus is free.

**What was added, in order:**

1. **Per-byte echo log** (`90a8b82`). Each byte logs whether its echo came
   back and after how long. On the bench, every echo arrived about 10.6 ms
   after the read started.
2. **`txlow` and `bbsend` committed** (`965e636`), bench-only, armed-only.
3. **Web echo test** (`38ccf37`, then `6e8245b`, `4724011`, `77c7ce1`):
   - `POST /api/echo-test`, body `confirm=car-unplugged&hex=...`. The
     `hex` field takes up to 16 bytes, with or without spaces (`55 FF`,
     `00FF00FF`). The exact confirmation is required on the server, and the
     page asks before it sends.
   - The bytes go out as one write with normal K-line spacing. The
     transmitter is armed only for that write and put back afterwards.
   - RX is sampled over a 200 ms window in a background task while the
     bytes go out. The capture is drawn in the Pulse train and decoded at
     10400 baud (start bit, eight data bits LSB first, stop bit), with each
     byte's start time listed.
   - **Clear** empties the status, the decoded list and the Pulse train.
4. **`wave_capture()`** (`6e8245b`): the sampler without the bus claim, for
   the echo test, which already holds it. `wave_sample()` keeps its claim.

**Validated on the bench, car unplugged, first breakout:**
- `55 FF 00 A5 5A` echoed on every byte. The RX capture shows the frames at
  about 96 µs per bit (10400 baud). Decoding the capture recovers the five
  bytes in order.
- `00 FF 00 FF` and `00FF00FF` both echo on every byte.
- `ZZ` and `0FF` are refused with 400. A request without the confirmation
  is refused with 400.

**Not tested:** the page's JavaScript was not run in a browser here. The
decode and the byte parsing were checked against captured data and the
board's responses, not on screen. The Pulse train was checked from a
screenshot only.

**Still open:**
- The car has not been connected. The echo test is a bench check, and the
  car test starts with a passive sniff (section 30).
- The bench current draw at 12 V has never been recorded.
- LAN-only operation: options A, B and C are unchanged, and nothing has been
  built.
- The W-Bus question (section 29): the Thermo Top V is dropped, so this is
  parked.
- A zoom control on the Pulse train, for seeing one byte's edges up close,
  has been asked about but not built.
- `main/kline.c` has your W4 comment, which is not committed and not part of
  this push.

**Decisions**

| # | Decision | Rationale |
|---|----------|-----------|
| 47 | **The web may transmit, but only the echo test, and only with the exact confirmation** | The bench wiring is easiest to check from the Debug tab. The request must carry the confirmation text, and the page asks first. Nothing else transmits from the web |
| 48 | **The echo test samples RX while it transmits, under its own claim** | A transmit and a capture on the same claim show the frames on the wire, which the echo alone cannot. The sampler reads a GPIO input and never drives a pin |

## 32. First passive sniff in the car — 2026-10-18

**The sniff works on the car's K-line, with ignition on.** The bus is quiet
with ignition off, and the sniff heard the Nanocom's requests and the ECU's
replies once ignition was on. Nothing was armed or sent.

**What was done:**
1. Ignition off. The sniff was running and the bus was quiet.
2. Ignition on. The bus carried traffic, so the sniff is not hearing a dead
   line.
3. The Nanocom was connected, and its engine ECU status pages were opened.
   The sniff ran throughout.

**Capture:** `Downloads\kline-sniff-2026-10-18-13-25-22.txt`, about 64
lines, covering roughly 56 to 84 seconds of the sniff's clock. It is not
copied into the repo.

**What the capture shows:**
- **Two message styles on the same line:**
  - KWP-style reads (service `0x22`). Requests look like `03 22 00 15 3A`
    and are answered with `62`, the same two-byte ID, e.g.
    `04 62 00 15 C0 3B`. Most of the traffic is this kind. IDs seen include
    `00 15`, `00 10`, `00 0C`, `00 18`, `00 1A`, `00 3B`, `23 34`, `23 32`,
    `23 47`, `23 37`, `23 36`, `23 46`, `23 33`, and `01 2C`.
  - OBD-style reads (service `01`). Requests look like `02 01 05 08`, and
    replies carry the ISO 9141 header `48 6B E8` then `41`, e.g.
    `48 6B E8 41 05 4F 30`. PIDs seen: `03`, `04`, `05`, `06`, `07`, `08`,
    `09`, `0F`, `10`, `12`, `13`, `14` and `18`.
- **Checksums are right.** Every frame's last byte is the low byte of the
  sum of the bytes before it, so the framing is correct.
- **Each line is the tester's request followed by the ECU's reply.** The
  K-line is half-duplex, so the tester sees its own bytes come back.
- **Polling rhythm:** frames are about 225 ms apart. Requests repeat two or
  three times, which fits the Nanocom re-polling the same values.
- **Two oddities:** `02 01 0F 12` at 63.1 s is cut short, with no reply
  after it. It is probably a dropped byte, not a protocol event. The frame
  `01 A4 A5 01 E4 E5` at 83.4 s has no reply and is not identified.

**Not yet done:**
- The values have not been decoded. The `0x22` IDs are not identified in
  any source here, and the `01` PIDs have not been converted (the coolant
  reply `4F` is a candidate reading, not a confirmed one).
- Which Nanocom screen produced which request is not recorded. Matching
  each request to a screen needs the screen list from the session.
- The sniffer's log lines were not checked against the Nanocom's own
  display.

**Still open** (from section 30, unchanged unless noted):
- Ignition-on was needed. The session 30 question "ignition state" is now
  answered: the bus needs ignition on to carry traffic.
- **Power, corrected:** the ESP32 is powered by a powerbank, not from the
  PC over USB. The PC's ground is therefore not on the board's supply. Still
  to confirm: how the PC reaches the board (WiFi or USB data), and whether
  the powerbank is charging from anything during the session.
- **Pin 16 is live with ignition off** (measured on the car, voltage not
  yet recorded). So the quiet bus with ignition off is a real result: the
  transceiver had 12 V.
- **Fuse:** the inline fuse on the 12 V lead is not yet fitted. The car's
  own fuse is not checked here.
- The bench current draw at 12 V has never been recorded.
- LAN-only operation: options A, B and C, not built.

**Decisions**

| # | Decision | Rationale |
|---|----------|-----------|
| 49 | **The car sniff stays passive until the screen-to-request mapping is recorded** | The sniff heard the traffic cleanly on the first try, but the values cannot be trusted until the requests are matched to the screens that produced them. Nothing is armed until then |

## 33. Looking for the GEMS init; no reply from the ECU — 2026-10-18

**The unit cannot yet ask the ECU for a value. The init is not found.** The
car shows coolant through the passive path only: the boot capture listens,
and the status page shows the last PID 05 reply the board has seen (§32,
`6924bc7`). Nothing is sent to the car.

**What was tried, all with the car connected (K-line, pin 7; ground, pin 5):**
- **Five-baud init:** `slowinit E8` was silent twice. A scan of the seven
  candidate addresses (0x33, 0x11, 0x12, 0x13, 0x16, 0x18, 0x1A) was silent
  on all seven.
- **Fast init:** `fastinit` followed by a KWP StartCommunication, in one
  console write so there is no gap. Three targets, each with the same
  five-byte frame and checksum: `81 11 F1 81 04`, `81 E8 F1 81 DB`,
  `81 10 F1 81 03`. Every byte echoed back on each try; none got a reply
  within 500 ms.
- **Recordings:** three cold-start recordings with the 3 s RX sampler
  (ignition off, then on, Nanocom plugged in). Each shows fast bursts from
  the 10400 traffic. The slow edges cluster at about 230 ms, with some near
  430 ms. No byte decodes at 200, 215 or 230 ms per bit.
- **Passive 10400 capture from cold start:** the first frame is
  `03 22 04 BF E8` at 394 s after boot. Nothing is on the line before it.
  The five-baud init would be invisible at 10400, so this does not show
  whether an init happened.

**What was ruled out:**
- **The L-line:** the diagram shows the petrol ECU on the K-line only. Its
  connector pin 9 is marked "Not used", and the L-line goes to the diesel
  ECU and the other modules. The L-line sampler idea is dropped, and
  socket pin 15 stays unwired.
- **Timing:** the StartCommunication went out in the same console write as
  the wake-up, so the gap was not the cause.

**1 kHz RX logger, cold start:** a passive logger (`main/slowlog.c`) samples
the RX pin once per millisecond and keeps each level change with its time.
It was run through a cold start (ignition off, then on, Nanocom plugged in).
First activity came 104.7 s after the log started. Before that the line was
empty. Between 104.7 s and 110 s, there were sparse bursts of fast flips,
1.3 to 2.1 s apart. From 110 s, the same bursts came every ~220 ms, matching
the 10400 polling frames in the sniff (about 225 ms apart). The 1 kHz
sampling aliases the 10400 bytes, so the bursts look like flips, not bits.
No run of 200 ms bits was seen, and nothing decoded to `55`. The five-baud
init was not on the K-line in this recording. An earlier estimate of a
210 ms bit period came from these bursts, and it was wrong.

**What the manuals and web say:** the GEMS system manual, the GEMS OBD
manual, the Nanocom GEMS technical data, the 14CUX manual and the fuel
injection overview have no init, baud rate, keyword or address. They confirm
socket pin 7 is K, shared with the BeCM, HVAC and ABS, and that pin 15 is L.
Web searches found only generic ISO 9141 and KWP descriptions. The Nanocom
GEMS guide link redirected to a general support page.

**What the replies show:** the header `48 6B E8` on every OBD-style reply
suggests `E8` is the ECU's address, but the five-baud init to `E8` was
silent. The session start has a security exchange, seed `7B 10` accepted
with `67 02` on the first attempt, and it appears in every session. Our
interface sends none of it.

**Safety for this step:** the transmitter was armed only for each test and
disarmed afterwards. The Nanocom was out for every transmit. Opening COM14
restarts the board, so the boot capture restarts with it. The boot capture
has to be stopped before the OTA upload and before any console arm.

**Still open:**
- The init method, the address and the keyword bytes.
- Whether the ECU needs something other than ignition on to answer.
- A logic analyser would settle the five-baud timing. We do not have one.
- **Task: check continuity from socket pin 15 to the ECU.** Car off, battery
  disconnected. Test between socket pin 15 and the petrol ECU's pins 9 and 10
  (C506 / C106, LGR). The Nanocom GEMS table lists pin 15 as "L-GEMS, BeCM,
  HVAC, ABS", but the wiring diagram marks the petrol ECU's L-line "Not used".
  If pin 15 reaches the ECU, the L-line divider plan applies again.

**Decisions**

| # | Decision | Rationale |
|---|----------|-----------|
| 50 | **Stop guessing init targets; the unit shows coolant from the Nanocom's own traffic** | Each target is a transmit to the car. None of the eight five-baud addresses or the three fast-init targets got a reply. The passive path already gives coolant whenever a tester polls the ECU, and nothing is sent. The init can be taken up again with a logic analyser or an init reference |

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
- [SEspe/RemoteStart](https://github.com/SEspe/RemoteStart) — SlaveWallas, the template for the web server, OTA, release and flasher workflows (FSD §7, §8, §11.1)
- [esp-web-tools](https://esphome.github.io/esp-web-tools/) — the browser flasher
