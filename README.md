# RRp38KLineInterface

ESP32 K-line diagnostic interface for the **Range Rover P38A (1995â2002)**.

## What this is

The P38A has **no CAN bus**. Its modules are reached over a single-wire
**ISO 9141 K-line at 10400 baud**, on pin 7 of the diagnostic socket.

This firmware gives you:

- A correct ISO 9141 transport â half-duplex echo handling, spec timing
  parameters, checksums.
- **5-baud slow init** (bit-banged, since 5 baud is below the UART's minimum
  divider) and **KWP2000 fast init**.
- A **passive sniffer** that timestamps bytes and splits frames on inter-byte
  gaps, so framing becomes visible.
- An **address scanner** that walks slow init across candidate ECU addresses
  and reports which ones answer.
- An interactive **console** over USB serial.

## What this deliberately is not

There is no table of GEMS parameter IDs here. The Land Rover message format
on this car is proprietary, and the PID values circulating on forums are not
consistent enough to hard-code without risking sending nonsense to a module.

This tool is built for the honest workflow: **capture first, decode second.**
Once you have real captures from your own car, decoding is easy to add.

## Safety â read this before connecting to the car

The P38 BeCM is genuinely fragile about diagnostic traffic. Malformed or
misaddressed messages are a documented cause of immobiliser lockout that
needs the **EKA (Emergency Key Access) code** â or a BeCM reflash â to
recover the vehicle.

Two hard rules:

1. **Know your EKA code before you plug anything in.** Write it down. This
   is worth doing whether or not you build this project.
2. **Start passive.** `sniff` never transmits. Prove your hardware works
   before you consider arming transmission.

The firmware enforces this with two independent gates:

| Gate | Where | Default |
|------|-------|---------|
| `CONFIG_RRP38_ALLOW_TX` | compile time (menuconfig) | **off** â TX code is not even built |
| `arm --yes` | runtime console command | **off** on every boot |

Both must be open before a single byte leaves the ESP32.

## Documentation

| Document | Contents |
|----------|----------|
| [docs/PLAN.md](docs/PLAN.md) | Phased roadmap with entry/exit criteria, and a risk register. **Start here.** |
| [docs/SESSION-NOTES.md](docs/SESSION-NOTES.md) | What was investigated, every decision and its rationale, and what is still unverified |
| [docs/INTERFACE-MODULES.md](docs/INTERFACE-MODULES.md) | Which transceiver to buy, comparison table, what to check on arrival |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Socket pinout, reference circuits, wiring for the TJA1021 module in hand, bench test procedure |

Current status: **firmware retargeted to ESP32-C6, flashed, and answering
on the console; a plain TJA1021 breakout (no MCU) received 2026-10-16,
ready for bench checks. Web HMI with OTA and a live sniff view running as v0.3.0 (Track H steps 1–4)** (PLAN.md Phase 2 and Track H, HARDWARE.md).

## Hardware

See **[docs/HARDWARE.md](docs/HARDWARE.md)** for the socket pinout and the
L9637D reference circuit, and
**[docs/INTERFACE-MODULES.md](docs/INTERFACE-MODULES.md)** for sourcing â
LIN transceiver breakouts, gutted KKL 409.1 cables, bare L9637D â plus a
bench test procedure that does not need the car.

Target board: **ESP32-C6** (ESP32-C6FH4, 4 MB flash). The console is the
chip's built-in USB serial port. Pin mapping (change in `menuconfig`):

| Signal | GPIO |
|--------|------|
| K-line TX â transceiver TXD | 18 |
| K-line RX â transceiver RXD | 19 |
| Transceiver enable (TJA1021 `SLP`) | 20 |
| Slow-init bypass MOSFET gate | 14 |
| L-line TX (optional) | disabled |

## Build

`idf.py` is not on PATH on this machine; use explicit paths:

```powershell
$python = "D:\Espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe"
$env:IDF_PATH = "D:\esp\v6.0.1\esp-idf"
$env:IDF_TOOLS_PATH = "D:\Espressif"
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\python_env\idf6.0_py3.11_env"
$env:ESP_IDF_VERSION = "6.0.1"
$env:PYTHONIOENCODING = "utf-8"
$riscv = "D:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin"
$env:PATH = "D:\Espressif\tools\cmake\4.0.3\bin;D:\Espressif\tools\ninja\1.12.1;$riscv;" + $env:PATH

cd D:\SteinsRootMappe\Claude\RRp38KLineInterface
# TJA1021 config (EN pin, bypass MOSFET, TX compiled in):
$cfg = @("-B","build_ncv","-D","SDKCONFIG=sdkconfig.ncv","-D","SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ncv.defaults")
& $python "D:\esp\v6.0.1\esp-idf\tools\idf.py" @cfg build
& $python "D:\esp\v6.0.1\esp-idf\tools\idf.py" @cfg -p COM14 flash monitor
```

The C6 enumerates as **COM14** (USB `303A:1001`). After changing the
target, run `idf.py @cfg set-target esp32c6` once; it clears the build
directory and regenerates `sdkconfig.ncv` itself. The plain `build/`
directory (no `@cfg`) is the passive config with TX compiled out; it was
last built for the classic ESP32 and needs the same one-time
`set-target esp32c6` before it builds again.

To enable transmission:

```powershell
& $python "D:\esp\v6.0.1\esp-idf\tools\idf.py" menuconfig
# RR P38 K-Line Interface -> Allow transmitting on the K-line
```

## Web interface and OTA

The board runs its own WiFi access point, always:

| | |
|---|---|
| Network | `RRp38-KLine` (WPA2) |
| Default password | `rrp38kline`. **Change it** on the WiFi tab |
| Page | <http://192.168.4.1/> |

The **Status** tab shows the K-line build, armed state and pins, the
network, and heap. The **Sniff** tab runs a passive capture and shows the
frames live, split on silent gaps; it can download them as a `.txt`. The
page is otherwise **read-only**: nothing on it can arm, init, send or
scan. Those stay on the USB console, and while a web capture runs the
console refuses them (`K-line busy (web)`).

The **WiFi** tab sets the access point password, and optionally a home
network to join as well (then also <http://rrp38.local/>). The **OTA
Update** tab installs a release straight from GitHub (needs home WiFi with
internet), or takes an uploaded `.bin` (`build/` or
`build_ncv/RRp38KLineInterface.bin`). The previous image stays in the other
slot. Updates are refused while a capture, init or send is using the
K-line.

## Releases and first flash

Pushing a tag `vX.Y.Z` that matches `FIRMWARE_VERSION` in `main/version.h`
makes GitHub Actions build and publish a release with two app images:

| Asset | Build |
|---|---|
| `RRp38KLine_passive_vX.Y.Z.bin` | Transmit compiled out. For listening in the car |
| `RRp38KLine_tx_vX.Y.Z.bin` | Transmit compiled in; still needs `arm --yes` on the USB console |

Installing the other kind from the OTA tab switches a board between them.

**First flash of a new board:** use the browser flasher at
<https://sespe.github.io/RRp38KLineInterface/> (Chrome or Edge on desktop),
rebuilt from `master` on every push, and **accept Erase device**. Or with
the toolchain: `esptool erase-flash`, then flash. A board flashed over old
firmware logged a running access point that never appeared on air.

## Console commands

Connect to the C6's USB port (COM14; baud rate is ignored on USB). Prompt is `p38>`.

| Command | Purpose |
|---------|---------|
| `status` | Pin mapping, TX gates |
| `sniff [-t secs] [-g gap_ms]` | Passive capture. Always safe. Default 30s, 10ms split |
| `arm --yes` / `disarm` | Unlock / lock transmission |
| `slowinit <hex>` | 5-baud init to one address, e.g. `slowinit 33` |
| `fastinit` | KWP2000 wake-up pattern |
| `scan` | Slow init across the candidate address list |
| `scan -f 00 -t 3F` | Slow init across an address range |
| `send <hex...> [-c] [-w ms]` | Raw frame; `-c` appends checksum, `-w` sets response wait |

## Suggested first session

```
p38> status                 # confirm pins
p38> sniff -t 60            # ignition off, then on, then cranking
```

On a healthy P38, when a tool successfully establishes comms the dash
message centre displays **"Diagnostics Mode"**. That is a free, zero-risk
confirmation that your hardware and init sequence are correct, before you
have decoded a single byte.

If `sniff` shows nothing at all, that is expected â the P38 K-line is not
chatty when idle; it answers rather than broadcasts. Bench-test the echo
loopback (docs/HARDWARE.md) to separate "quiet bus" from "broken wiring".

Then, with the EKA code to hand:

```
p38> arm --yes
p38> slowinit 33            # ISO 9141-2 standard address
p38> scan                   # candidate module addresses
```

## ECU addresses

The only address in this firmware that is *specified* rather than *observed*
is `0x33`, the ISO 9141-2 standard OBD-II address. The rest of the candidate
list in `kline_console.c` is a starting point drawn from community reports â
**treat it as unverified** and confirm against your own car. The `scan`
command exists precisely so you can establish the real map empirically.

## Layout

```
main/kline.h          transport API and ISO 9141 timing constants
main/kline.c          UART setup, 5-baud bit-bang, init sequences, sniffer
main/kline_console.c  console commands and the candidate address table
main/main.c           banner, startup, WiFi + web server, REPL
main/net.c            WiFi: always-on access point, optional home network, mDNS
main/sniff.c          passive capture shared by the console and the web page
main/web_server.c     web HMI (read-only), WiFi settings, OTA upload
main/version.h        firmware version (also fed into the app descriptor)
partitions.csv        nvs + otadata + two 1.875 MB OTA slots
main/Kconfig.projbuild  pins, baud, safety gates
docs/HARDWARE.md      circuit, pinout, sourcing, bench test
```

## Vehicle

1996 Range Rover P38A, 4.0/4.6 Rover V8 with **GEMS** engine management
(Bosch Motronic 5.2.1 arrived with the 1999 Thor engines). European-market
cars of this age are not OBD-II compliant â the connector is J1962-shaped
but the protocol is Land Rover proprietary, so generic ELM327 dongles will
not get far.
