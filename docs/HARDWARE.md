# Hardware

## Why not a CAN transceiver

The P38A (1995–2002) has no CAN bus. Land Rover's first CAN vehicle was the
L322 in 2002. On the P38 the modules talk to each other over a proprietary
clocked serial data bus (ground, clock, data, direction — each wire
duplicated for redundancy), and they talk to *diagnostic tools* over a
single-wire ISO 9141 K-line at 10400 baud.

An SN65HVD230 or any other CAN transceiver has nothing to connect to here.

## The diagnostic socket

16-pin J1962 connector, under the dash on the driver's side.

| Pin | Wire colour       | Function |
|-----|-------------------|----------|
| 7   | Pink/Red          | **K-line** — main diagnostic line: BeCM, HEVAC, ABS, engine ECU, gearbox ECU |
| 15  | Light Green/Red   | L-line — also used in ABS / HEVAC / BeCM comms |
| 4,5 | Black             | Ground |
| 16  | —                 | Battery +12V (permanent) |

Verify against a P38 wiring diagram for your exact car before cutting
anything. EAS diagnostic lines route through the LH footwell kick panel
connectors, which are notorious for corroding — if EAS is the only module
that will not answer, look there before suspecting your circuit.

## Reference circuit — L9637D

The L9637D is a purpose-built ISO 9141 K-line transceiver. Minimal circuit:

```
                       +12V (pin 16, fused 1A)
                            |
                            +--[510R]--+
                            |          |
   ESP32                 +--+--+       |
   GPIO17 (TX) --------> | Vs  |       |
   GPIO16 (RX) <-------- | RX  K |-----+-----> pin 7  (K-line)
   3V3        ---------> | Vcc   |
   GND        ---------> | GND L |-----------> pin 15 (L-line, optional)
                         +-------+
                            |
                           GND (pins 4/5)
```

- 100nF decoupling on Vcc and Vs.
- The 510R pull-up from K to +12V is required; some modules are marginal
  without it even though the L9637D has an internal pull-up.
- Do **not** power the ESP32 from pin 16 directly. Use a proper 12V→5V buck
  converter; automotive rails carry transients that will kill a bare LDO.
- Add a common ground between the ESP32 and the car before connecting K.

## Bypass MOSFET — for transceivers with a TxD dominant timeout

Needed when the transceiver releases the bus if TxD is held low too long.
The onsemi NCV7329 does this after 14–46 ms, against a 200 ms bit cell in a
5-baud init. See [INTERFACE-MODULES.md](INTERFACE-MODULES.md) for which
parts are affected.

The fix is to drive the init bits through an external MOSFET that the
transceiver's timer knows nothing about, and hold TxD recessive meanwhile.
Normal byte traffic is short enough to go through the transceiver as usual.

```
                                    K-line (socket pin 7)
                                          |
                                          |
   ESP32 GPIO22 ──[1k]──┐              ┌──┘
   (init driver)        │            D │
                        ├──── gate ────┤  2N7002 / BSS138
                      [100k]           │  (N-channel, Vds >= 60V)
                        │            S │
                       GND            GND
```

- **2N7002** (60 V, 115 mA) or **BSS138** (50 V, 200 mA). Either is ample —
  pulling the K-line low through a 510 Ω pull-up to 12 V draws about 24 mA.
- The 1 kΩ series gate resistor limits inrush; the 100 kΩ pull-down keeps
  the MOSFET off while the ESP32 is in reset. **Do not omit the pull-down** —
  a floating gate during boot can hold the car's diagnostic line dominant.
- Logic is active-high: GPIO high turns the MOSFET on and pulls the bus
  dominant. The firmware handles the inversion.

Enable with:

```
CONFIG_RRP38_INIT_DRIVER_GPIO=22    # gate
CONFIG_RRP38_EN_GPIO=21             # transceiver EN, if it has one
```

With `CONFIG_RRP38_INIT_DRIVER_GPIO` set, the firmware leaves the UART
attached to the TX pin throughout (idle high = recessive) and bit-bangs the
init through the MOSFET instead, so the two drivers never fight.

## Sourcing — what to buy

Ranked by how little work is left for you.

### 1. LIN transceiver breakout — easiest to actually buy

LIN is electrically almost identical to K-line: single wire, 12V,
open-drain, pull-up. NXP's own TJA1020 application note states the part
supports K-line, and that its pinout was deliberately chosen to be
compatible with standard K-line transceivers.

- **MCP2003 / MCP2004** breakout boards — widely stocked, e.g. Copperhill
  Technologies' LIN Bus Breakout Board (MCP2004A).
- **TJA1020 / TJA1021** modules — common on eBay/AliExpress, often sold as
  "LIN bus module" or "LIN slave node board".

Search terms: `LIN bus breakout board`, `MCP2004 LIN module`,
`TJA1020 LIN transceiver board`.

Caveats: LIN transceivers are wave-shaped for 20 kbaud max — fine at 10.4k.
Check whether the board has a TXD dominant timeout (it will force the bus
recessive if you hold it low too long; irrelevant for normal framing) and
whether it needs an enable/SLP pin driven high. Budget one extra GPIO.

### 2. Gut a KKL 409.1 cable — cheapest, ~€5–10

A "KKL VAG-COM 409.1" cable is a USB-serial chip plus a K-line transceiver
in a plug. Buy one, open it, and wire your ESP32 UART straight to the
transceiver's logic-side TX/RX pins, bypassing the USB chip entirely. You
get the OBD connector, the 12V pickup and the transceiver for the price of
a coffee.

Buy the **FTDI FT232RL** version, not a CH340 clone — the FTDI ones are
better built, and if the transceiver turns out to be unusable you still have
a working cable. Search: `KKL 409.1 FTDI FT232RL`.

Downside: you have to identify the transceiver on the board yourself. Common
ones are the L9637D, a discrete transistor pair, or an unmarked SOIC-8.

### 3. Bare L9637D + your own board — cleanest result

ST list the L9637D at around $2 in their own store, and it is stocked by
DigiKey. Some distributors flag it obsolete, but OBD2-Shop.eu state it is
back in volume production. Also available as the MikroElektronika
**ISO 9141 Click** board if you want it pre-built on a carrier.

Search terms: `L9637D`, `ISO 9141 Click`, `K-line transceiver module`.

### 4. Discrete circuit — if you have a junk box

At 10.4 kbaud you can get away with a MOSFET pulling K low for TX and a
resistor divider plus comparator (or just a divider into a GPIO) for RX.
Roughly five components. Fine for bench work; less robust in a car, and no
overvoltage or thermal protection. Not recommended for something that will
live under the dash.

### Recommendation

Buy **both** a KKL 409.1 cable and a LIN breakout. Together they are under
€20, the cable gives you the connector and harness regardless, and having
two independent transceivers makes it much easier to tell a hardware fault
from a protocol misunderstanding when nothing answers.

## Bench testing before the car

You do not need the vehicle to prove the firmware works:

1. Power the transceiver from a 12V bench supply.
2. Tie K through the 510R pull-up to 12V.
3. Loop back: with nothing else on the bus, anything the ESP32 transmits
   should come straight back on RX. `kline_write()` verifies exactly this
   echo, so a successful `send` on the bench means the wiring is right.
4. Only then move to the car, and start with `sniff`.

## Sources

- [NXP AN00093 — TJA1020 LIN transceiver application note](https://www.nxp.com/docs/en/application-note/AN00093.pdf)
- [ST L9637 datasheet](https://www.st.com/resource/en/datasheet/l9637.pdf)
- [MikroElektronika ISO 9141 Click](https://www.mikroe.com/iso-9141-click)
- [Copperhill LIN Bus Breakout Board (MCP2004A)](https://copperhilltech.com/lin-bus-breakout-board/)
- [OBD2-Shop.eu — L9637 ISO transceiver](https://www.obd2-shop.eu/product_info.php/products_id/520/language/en)
- [Range Rovers Forum — diagnostic socket wiring](https://www.rangerovers.net/threads/diagnostic-socket-wiring-get-help.255153/)
