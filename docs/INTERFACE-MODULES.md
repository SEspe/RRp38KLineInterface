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
ESP32 GPIO17 ──> TXD / TX
ESP32 GPIO16 <── RXD / RX
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
| J1962 breakout or spare socket | Lets you bench-test without the car |

## Bench test before the car

You do not need the vehicle to prove the interface works. With the
transceiver powered from a 12 V bench supply and K pulled up through 510 Ω,
anything the ESP32 transmits comes straight back on RX — the K-line is
half-duplex, so this echo is inherent, not a loopback you have to build.

`kline_write()` verifies that echo on every byte. **A successful `send` on
the bench means the wiring is correct.** Only then move to the car, and
start with `sniff`.
