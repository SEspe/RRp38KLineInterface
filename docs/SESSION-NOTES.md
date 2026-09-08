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

## Sources

- [Nanocom NCOM05 — Range Rover P38 GEMS (Petrol to 1999)](https://www.nanocom-diagnostics.com/product/ncom05-range-rover-p38-gems-petrol-to-1999-kit)
- [Range Rovers Forum — diagnostic socket wiring](https://www.rangerovers.net/threads/diagnostic-socket-wiring-get-help.255153/)
- [Range Rovers Forum — serial link, BeCM to instrument cluster](https://www.rangerovers.net/threads/serial-link-becm-to-instrument-cluster-temp-fuel.325628/)
- [Land Rover workshop manual — P38 BeCM description](https://workshop-manuals.com/landrover/p38/86_electrical/description_and_operation/body_electrical_control_module_(becm)_description/)
- [BeaDy — P38 BECM communications](https://www.beady.com/blog/p38-becm-communications/)
- [rangerovers.pub — OBD connection](https://rangerovers.pub/topic/65-obd-connection)
- [NXP AN00093 — TJA1020 LIN transceiver](https://www.nxp.com/docs/en/application-note/AN00093.pdf)
- [ST L9637 datasheet](https://www.st.com/resource/en/datasheet/l9637.pdf)
