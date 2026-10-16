# Interface modules — options, comparison, and what to check

Notes on choosing the K-line transceiver. The circuit itself is in
[HARDWARE.md](HARDWARE.md); this document is about *which part to buy* and
why.

---

## Why a transceiver is needed at all

The K-line is a single wire that idles **high at battery voltage** and is
pulled low to signal. An ESP32 GPIO is 3.3 V and would be destroyed by 12 V.
The transceiver's whole job is:

- Level-shift 12 V bus ↔ 3.3 V logic.
- Provide an open-drain output that can pull the bus low without fighting
  other modules.
- Survive automotive transients, load dump, and reverse polarity.

It does **not** do protocol. Framing, timing and checksums are all handled in
firmware. That means almost any part that does the electrical job will work,
which is what makes the alternatives below viable.

## The ruled-out part

**SN65HVD230 — CAN transceiver. Not usable on this car.**

It is a differential CAN physical layer. The P38A has no CAN bus and pins 6
and 14 of the diagnostic socket are not populated. Nothing is wrong with the
part; it simply has nothing to connect to here. Keep it for a future
project. (See [SESSION-NOTES.md](SESSION-NOTES.md) §2.)

## Comparison

| Option | Cost | Effort | Robustness | Availability | Verdict |
|--------|------|--------|------------|--------------|---------|
| **LIN transceiver breakout** (MCP2003/4, TJA1020/21) | €5–15 | Low — wire 4 pins | Good, automotive-grade | Excellent, eBay/AliExpress/RS | **Buy** |
| **KKL 409.1 cable, gutted** | €5–10 | Medium — identify the chip yourself | Good | Excellent | **Buy** — gives connector + harness too |
| **Bare L9637D + own board** | €2 + board | High — build the circuit | Best; purpose-built for this | Patchy (obsolete at some distributors) | Optional, cleanest end result |
| **MikroE ISO 9141 Click** | €25–30 | Low | Best | Good | If you want it pre-built and correct |
| **Discrete MOSFET + pull-up** | ~€1 | Medium | Poor — no protection | Junk box | Bench only |

---

## Option 1 — LIN transceiver breakout (recommended)

LIN is electrically almost identical to K-line: single wire, 12 V,
open-drain, pull-up to battery. NXP's own
[TJA1020 application note](https://www.nxp.com/docs/en/application-note/AN00093.pdf)
states the part supports the K-line bus, and that **its pinout was
deliberately chosen to be compatible with standard K-line transceivers**.
That is a vendor statement, not a community workaround.

**Search terms:** `LIN bus breakout board`, `MCP2004 LIN module`,
`TJA1020 LIN transceiver board`, `LIN slave node board`

**Well-documented option:**
[Copperhill LIN Bus Breakout Board (MCP2004A)](https://copperhilltech.com/lin-bus-breakout-board/)

**Typical wiring:**

```
ESP32 GPIO18 ──> TXD / TX
ESP32 GPIO19 <── RXD / RX
ESP32 3V3    ──> Vcc  (check: some boards want 5V logic supply)
ESP32 GND    ──> GND  (common ground with the car)
        12V  ──> VBAT / Vsup      (socket pin 16, fused)
        BUS  ──> K-line           (socket pin 7)
```

**Check on arrival:**

- **Enable / SLP / CS pin.** Most LIN transceivers have one and idle in
  sleep until it is driven high. Budget one extra GPIO — this is the most
  common reason a LIN board appears dead.
- **Logic supply voltage.** Some boards are 5 V-only on the logic side. The
  ESP32 RX pin tolerates 3.3 V logic; if the board outputs 5 V you need a
  divider on RX. TX at 3.3 V into a 5 V input usually works but check the
  input threshold.
- **TXD dominant timeout.** The transceiver forces the bus recessive if TXD
  is held low too long. Harmless for normal byte framing, but it *will*
  interfere with a bit-banged 5-baud init, where TX is held low for 200 ms
  at a time. If slow init fails on a LIN board but works on an L9637D, this
  is the first thing to suspect.
- **Onboard master pull-up.** Some LIN boards fit a ~1 kΩ pull-up for master
  mode. On a K-line that is roughly right (510 Ω–1 kΩ), but confirm the bus
  is not being loaded too hard when other modules are attached.

**The 5-baud caveat is the real risk with this option.** Everything else is
straightforward.

### Worked example — Soldered NCV7329 breakout (evaluated 2026-09-09)

Checked against the [onsemi NCV7329 datasheet](https://www.onsemi.com/pdf/datasheet/ncv7329-d.pdf).
This is the concrete case of the caveat above.

**Verdict: usable for sniffing; needs a bypass MOSFET for the init sequences.**

| Parameter | Value | Consequence |
|-----------|-------|-------------|
| `tTxD_TIMEOUT` | **14 / 25 / 46 ms** (min/typ/max) | **Blocks 5-baud init.** A bit cell is 200 ms — over 4× the *maximum* timeout. The bus is released mid-bit and the address is corrupted |
| Transmission rate | 1 kbps – 20 kbps | The 1 kbps *lower* limit is the timeout, stated outright by onsemi |
| Pin 1 RxD | Open-drain output | **Needs an external pull-up to 3.3 V.** Check whether the breakout fits one; if not, ~4.7 kΩ |
| Pin 2 EN | Normal mode when high; internal 100–650 kΩ pull-down | **Must be driven high** or the part looks dead. `V_IH` = 2 V, so a 3.3 V GPIO works |
| Pin 4 TxD | `V_IH` 2 V, `V_IL` 0.8 V, internal 50–325 kΩ pull-down | 3.3 V logic fine |
| Pin 7 VBB | 5–18 V | **18 V ceiling.** Fine at 12–14.4 V nominal; a jump start or load dump will exceed it. Fit a TVS |
| LIN pin | Internal 30 kΩ + reverse-protection diode to VBB | Slave termination. Still fit the external 510 Ω for K-line |
| Bus rating | ±42 V | The bus pin itself is robust; it is VBB that is the weak point |

Pinout: 1 RxD, 2 EN, 3 NC, 4 TxD, 5 GND, 6 LIN, 7 VBB, 8 NC.

**Fast init is also marginal** — it holds the bus low for 25 ms, against a
14 ms *minimum* timeout. Do not rely on it either.

#### Three fixes, in order of preference

**a) Bypass MOSFET (recommended, ~€0.20).** Drive the init bits through an
external MOSFET, bypassing the transceiver's timeout entirely, while TxD is
held recessive. Normal byte traffic still goes through the transceiver, so
its protection and slope control are preserved on everything except the
init. RX path untouched. Circuit in [HARDWARE.md](HARDWARE.md).

Firmware support is built in — set `CONFIG_RRP38_INIT_DRIVER_GPIO` to the
gate GPIO, and `CONFIG_RRP38_EN_GPIO` to whatever drives EN.

**b) Swap the chip for an NCV7327.** onsemi state it *"differs from NCV7329
only by absence of TxD Timeout function"* — same 8-pin SOIC, same pinout.
Desolder and swap if you are comfortable with SOIC rework, or use it in a
board of your own. This is the cleanest electrical answer.

**c) Use the board for RX only** and transmit through a second transceiver
(the gutted KKL cable). Works, but doubles the wiring.

### Worked example — TJA1021 TTL-to-LIN module (ordered 2026-09-09, received 2026-09-30)

The part actually bought: a generic *"TTL to LIN Bus TJA1021 Module /
Automotive Bus"* breakout. Checked against the
[NXP TJA1021 datasheet, rev. 9](https://www.nxp.com/docs/en/data-sheet/TJA1021.pdf).

**Verdict: the best of the LIN options here.** NXP list **"K-line
compatible"** as an explicit feature bullet (§2.1), not an inference. The
TxD timeout still blocks 5-baud init, but it is long enough that **fast
init works through the transceiver unmodified** — which the NCV7329's does
not.

| Parameter | Value | Consequence |
|-----------|-------|-------------|
| `tto(dom)TXD` | **27 / 55 / 90 ms** (min/typ/max) | **Blocks 5-baud init** — a bit cell is 200 ms. But the datasheet guarantees the timer *will not* trigger before `tmin`, so the 25 ms low of a **fast init is safe**, with 2 ms of margin |
| Baud rate | 1 kBd – 20 kBd | As with the NCV7329, the 1 kBd *lower* limit is the timeout |
| Variants | `T/10`, `AT` = low slope, optimised for **10.4 kBd** (SAE J2602); `T/20`, `BT` = 20 kBd | **`/10` is the exactly-right variant for a 10400 baud K-line.** Read the chip marking; cheap modules fit whichever was in the bin |
| Pin 1 RXD | Open-drain output, `IOL` 1.5 mA at 0.4 V | **Needs an external pull-up.** The chip has *no logic supply pin at all*, so the pull-up rail *is* the logic level — pull it to **3.3 V** and the ESP32 sees clean 3.3 V |
| Pin 2 SLP_N | Sleep when low; internal 140–1200 kΩ pull-down; `V_IH` 2 V | **Must be driven high**, and Normal mode is entered **on the rising edge**. A 3.3 V GPIO works. This is `CONFIG_RRP38_EN_GPIO` |
| Pin 3 WAKE_N | Local wake-up, active low; internal pull-up of only **1–30 µA** to VBAT | Tie to VBAT if the board leaves it floating — 12 µA typ is weak enough for noise to fake a local wake-up |
| Pin 4 TXD | `V_IH` 2 V, `V_IL` 0.8 V, internal 140–1200 kΩ pull-down | 3.3 V logic fine. Note: entering Normal mode **with TXD low leaves the transmitter off** until TXD goes high — so bring SLP_N high *after* the UART owns the TX pin |
| Pin 7 VBAT | Limiting **−0.3 to +40 V**; specified 5.5–27 V; transmitter guaranteed only to 18 V | Far more headroom than the NCV7329's 18 V ceiling. Still fit a TVS, but a jump start will not destroy it |
| Pin 8 INH | Inhibit output, high in Normal mode, 20–50 Ω to VBAT | Convenient source for the master pull-up — NXP's own recommendation is a resistor + series diode from INH or VBAT to LIN |
| LIN pin | Limiting ±40 V; `IBUS_LIM` 40–100 mA; internal responder termination 20/30/47 kΩ to VBAT | Bus pin is robust. 24 mA through a 510 Ω pull-up is well inside the driver limit. The external 510 Ω is still needed for K-line |
| ESD | ±6 kV IEC 61000-4-2 on LIN, VBAT, WAKE_N | — |

Pinout (SO8): 1 RXD, 2 SLP_N, 3 WAKE_N, 4 TXD, 5 GND, 6 LIN, 7 VBAT,
8 INH. Pin-to-pin compatible with the TJA1020 and MC33662(B), so a module
sold as "TJA1020/TJA1021" is wired the same either way.

#### What this means for the firmware

The existing config covers it exactly — `sdkconfig.ncv.defaults` applies
verbatim:

```
CONFIG_RRP38_EN_GPIO=21             # -> SLP_N (pin 2)
CONFIG_RRP38_INIT_DRIVER_GPIO=22    # -> bypass MOSFET gate, for slow init
```

Still needed: the bypass MOSFET (2N7002/BSS138 + 1 kΩ + 100 kΩ) for
**5-baud init only**, and a 4.7 kΩ RXD pull-up **to 3.3 V** if the board
does not fit one. Fast init needs neither.

#### What it turned out to be (received 2026-09-30)

**The module has a microcontroller on it** — a Geehy APM32F103C8T6 sits
between the screw terminals and the TJA1021 and speaks LIN framing, so the
terminals are no use for K-line. The transceiver itself is a genuine
**NXP TJA1021T/20**, so everything above still applies once the MCU is held
in reset and the ESP32 is wired to the chip's legs. Wiring, annotated photo
and remaining measurements are in
[HARDWARE.md — the module in hand](HARDWARE.md#the-module-in-hand--u7x54911-ttl-to-lin-bus-tja1021-module).

**Lesson for buying:** a listing that says "protocol converter", "host
request frame" or shows a large chip beside the SO8 has an MCU. Look for a
board where the only IC is the transceiver.

#### What to check when it arrives

- **Whether there is an MCU on the board.** If there is, see above.
- **What the RXD pull-up goes to.** If the module pulls RXD up to an
  onboard 5 V rail, that 5 V lands on an ESP32 pin rated 3.6 V absolute
  max. Lift the resistor and pull RXD to 3.3 V instead, or fit a divider.
  This is the one way this board can damage the ESP32.
- **The chip marking** — `/10` or `/20`. Either works at 10400 baud; `/10`
  is wave-shaped for exactly this rate.
- **Whether SLP_N is broken out** or strapped high on the board. If it is
  strapped high, set `CONFIG_RRP38_EN_GPIO=-1` and save a GPIO.
- **Whether a master pull-up is fitted**, and its value. Typical LIN master
  boards use 1 kΩ; K-line wants 510 Ω. Adding 1 kΩ in parallel with an
  existing 1 kΩ gives 500 Ω, which is close enough. **An ohmmeter cannot
  find it** — master pull-ups have a series diode, so they read open
  unpowered. Power the board and load LIN with 1 kΩ instead (HARDWARE.md).
- **Whether WAKE_N is tied to VBAT.** If it floats, tie it high.

---

## Option 2 — gut a KKL 409.1 cable (cheapest, most practical)

A "KKL VAG-COM 409.1" cable is a USB-serial chip plus a K-line transceiver
in a moulded plug. Open it and wire the ESP32 UART straight to the
transceiver's logic-side TX/RX pins, bypassing the USB chip entirely.

**What you get for €5–10:** the OBD connector, the harness, the 12 V pickup
*and* a transceiver. Even if the transceiver turns out to be unusable, the
connector alone is worth the money — sourcing a J1962 plug separately often
costs more.

**Buy the FTDI FT232RL version, not a CH340 clone.** Better built, and if
you abandon the teardown you still own a working diagnostic cable.

**Search terms:** `KKL 409.1 FTDI FT232RL`, `VAG-COM 409.1 K-line cable`

**Downside:** you have to identify the transceiver on the board yourself.
Common finds are an L9637D, a discrete transistor pair, or an unmarked
SOIC-8. Trace from the K-line pin (socket pin 7) back to the first IC.

**Procedure:**

1. Open the plug shell (usually clipped, sometimes glued).
2. Find the pin 7 trace and follow it to the transceiver.
3. Identify the transceiver's logic-side TX and RX pins — on an L9637D these
   are pins 1 (RX out) and 4 (TX in).
4. Cut or lift those two traces where they run to the USB chip.
5. Wire them to your ESP32 GPIOs. Share ground.

Keep the 12 V and ground pickups from the cable — they are already correct.

---

## Option 3 — bare L9637D

The purpose-built part. ST list it around $2 in their own store and DigiKey
stock it. Some distributors flag it obsolete, but
[OBD2-Shop.eu](https://www.obd2-shop.eu/product_info.php/products_id/520/language/en)
state it is back in volume production.

Cleanest end result, and no ambiguity about 5-baud behaviour or enable pins.
Costs you a board layout or some protoboard work. Circuit is in
[HARDWARE.md](HARDWARE.md).

**Pre-built alternative:** [MikroElektronika ISO 9141 Click](https://www.mikroe.com/iso-9141-click)
— an L9637D on a carrier, correct by construction, but €25–30 and needs a
mikroBUS-to-jumper adapter for breadboard use.

---

## Option 4 — discrete circuit

At 10.4 kbaud you can get away with a MOSFET pulling K low for TX, and a
resistor divider (optionally into a comparator) for RX. Roughly five
components.

Fine for bench work. **Not recommended for anything that lives under a
dash** — no overvoltage clamping, no thermal shutdown, no reverse-polarity
protection. Automotive rails will eventually find the weakness.

---

## Recommendation

**Status:** the LIN half of this is now ordered — a **TJA1021** TTL-to-LIN
module (see the worked example above). The KKL cable is still worth buying
for the connector and as the second, independent transceiver.

**Buy both a KKL 409.1 cable and a LIN transceiver breakout.** Under €20
together.

Rationale:

- The cable gives you the OBD connector and harness regardless of which
  transceiver you end up using.
- Two independent transceivers make it possible to tell a **hardware fault**
  from a **protocol misunderstanding** when nothing answers. With one
  transceiver, a silent bus is ambiguous and you can burn days on it.
- If the LIN board's TXD dominant timeout breaks the 5-baud init, the
  cable's transceiver is the fallback — and vice versa if the cable turns
  out to contain something unusable.

Add a bare L9637D to the order if you want to build a permanent version
later; it is cheap enough to buy speculatively.

## Things to buy alongside

| Item | Why |
|------|-----|
| 12 V → 5 V buck converter (automotive-rated) | Never power the ESP32 from socket pin 16 through a bare LDO — transients will kill it |
| 510 Ω resistor | K-line pull-up to 12 V; some modules are marginal without it |
| 100 nF ceramics | Decoupling on Vcc and Vs |
| Inline fuse, 1 A | On the 12 V tap. Non-negotiable — this is a permanent-live pin |
| TVS diode, ~16–24 V standoff | Protects the transceiver's supply pin. The NCV7329 tops out at 18 V; the TJA1021 survives 40 V but only guarantees transmission to 18 V |
| J1962 breakout or spare socket | Lets you bench-test without the car |

If using a transceiver with a TxD dominant timeout (NCV7329, TJA1021, and
most other LIN parts), add:

| Item | Why |
|------|-----|
| 2N7002 or BSS138 N-channel MOSFET | Bypass driver for the init sequences |
| 1 kΩ resistor | Gate series resistor |
| 100 kΩ resistor | Gate pull-down. **Not optional** — a floating gate during ESP32 reset could hold the car's diagnostic line dominant |
| 4.7 kΩ resistor | RxD pull-up, if the breakout does not fit one. Open-drain on both the NCV7329 and the TJA1021 — pull it to **3.3 V**, not 5 V |

## Bench test before the car

You do not need the vehicle to prove the interface works. With the
transceiver powered from a 12 V bench supply and K pulled up through 510 Ω,
anything the ESP32 transmits comes straight back on RX — the K-line is
half-duplex, so this echo is inherent, not a loopback you have to build.

`kline_write()` verifies that echo on every byte. **A successful `send` on
the bench means the wiring is correct.** Only then move to the car, and
start with `sniff`.
