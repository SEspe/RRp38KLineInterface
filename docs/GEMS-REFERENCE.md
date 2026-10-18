# GEMS engine ECU reference (adopted from landrover-gems_t4)

Facts taken from `howeypeter/landrover-gems_t4` (public, no license file, so
reference only; no code copied). That repo's bench notes are on a 1996 NAS P38
4.6 GEMS engine ECU. This file records which facts we adopt, where they come
from, and how far each is verified. Our firmware targets the BeCM, so the
engine-ECU facts are context, not a spec for our current commands.

## Killer bug: the keybyte W4 window

After the ECU sends KW2, the tester must send back the inverted KW2 within
25-50 ms (ISO 9141-2 W4). If it misses the window, the ECU hands over the
keybytes but never reaches the initialised state, and it silently ignores every
request. The other repo found this to be the blocker on first hardware contact.

- Our `main/kline.c` `kline_slow_init()` sends `~KW2` after a 30 ms delay
  (`KLINE_W4_MS`). With `CONFIG_FREERTOS_HZ=1000` (sdkconfig) that delay is
  accurate to about 1 ms, so it sits inside the window.
- Not yet verified on hardware against a real module.

## Protocol facts

| Fact | Value | Source | Status |
|---|---|---|---|
| NAS GEMS speaks ISO 9141-2 (OBD-II style) | K-line, 5-baud slow init | other repo `memory/real-gems-protocol.md` | confirmed on bench (their Pico) |
| ECU address | 0x33 | same; our `README.md` | 0x33 is the OBD-II standard address |
| Sync and keybytes | `55 08 08` | same | confirmed on bench |
| Keybyte reply | `~KW2` within W4 | same | see killer bug above |
| ECU inverted address | `~0x33` = `0xCC` | same | confirmed on bench |
| Request frame | `68 6A F1 <svc> <data> <sum>` | same | confirmed on bench |
| Response frame | `48 6B E8 <svc+0x40> <data> <sum>` | same | confirmed on bench |
| Checksum | modulo-256 sum of all preceding bytes | verified here, `tests/test_gems_frames.py` | matches `kline_checksum()` |
| Mode 01 PID 00 | bitmask, e.g. `41 00 BF 9F F9 91` | captured frame, same | fixture in tests |
| Mode 01 PID 05 | coolant, `A - 40` C | captured frame, same | fixture in tests |
| Mode 03 / 07 | stored / pending DTCs; ECU silent when no codes | same | not implemented here |
| Mode 04 clear | ECU reboots its diagnostic side, 1-2 min unresponsive | same | do not clear on a car we need running |

## Connector pinout (engine ECU, context only)

From the other repo's `docs/gems-ecu-pinout.md`:

- Red 36-way (C1017 / P38 C507): K-line is pin 23, L-line is pin 20.
- Black 18-way (C1033 / C509): power and grounds. Pins 7 and 8 still need a
  meter check; their sources disagree.

## Not adopted

- Their 10AS (Disco 1 alarm/immobiliser) pinout and Security-Learn notes. The
  P38 BeCM is a different module with different addressing.
- Their `virtual_ecu.py` and `protocol/framing.py`. Their own notes say these use
  a "stylized" frame format that is wrong for real ECUs.

## Open

- A bench responder for the post-init phase is not in the repo yet. An earlier
  attempt was stopped and removed, so nothing is written.
- No firmware frame parser exists yet. The fixture tests cover the framing rules
  in Python only.
