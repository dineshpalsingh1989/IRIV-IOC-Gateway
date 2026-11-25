# IRIV IOC LAN gateway firmware

Arduino-Pico (RP2350A) firmware for the Cytron IRIV IO Controller. It publishes every pin to your LAN over **Modbus TCP** and **MQTT**, and polls **RS485 Modbus RTU** devices so their values appear on the LAN too. All settings come from the dashboard (`../index.html`) as `ioc_config.json`.

**Status: experimental.** The firmware compiles for `cytron_iriv_io_controller` (158 KB flash, 99 KB RAM). It has **not** been run on a controller yet. The library tests run and pass on every push (GitHub Actions, Ubuntu g++). Validate on the bench before connecting equipment.

## Our libraries

Everything protocol- and board-specific is in our own MIT libraries under `lib/`, so nothing depends on third-party Modbus or MQTT code. The only outside dependency is ArduinoJson 6, used to read the config file.

| Library | Version | What it does | Origin |
| --- | --- | --- | --- |
| [IRIVModbus](lib/IRIVModbus/README.md) | 0.1.0-alpha.1 | Modbus TCP server (all four tables, FC 01–06/15/16, sparse address blocks) and RTU master (FC 01–06) | New |
| [IRIVIOCIo](lib/IRIVIOCIo/README.md) | 0.1.0-alpha.1 | DI with debounce, pulse counters, DO, analog 0–10 V / 0–20 / 4–20 mA scaling with NE43 faults, buzzer rules | New |
| [IRIVIOCHardware](lib/IRIVIOCHardware/README.md) | 0.2.0-alpha.1 | W5500 Ethernet, now with static IP (`begin(hostname, Address)`), plus LittleFS settings | From ILA210-and-883M-Library 0.1.0, static IP added |
| [WilcoxonMqtt](lib/WilcoxonMqtt/README.md) | 0.1.0-alpha.1 | MQTT 3.1.1 client (QoS 0 publish, QoS 0/1 subscribe) | From ILA210-and-883M-Library, unchanged |

## How it runs

- **Core 0**: Ethernet, I/O sampling and the buzzer, Modbus TCP server (4 clients), MQTT.
- **Core 1**: RS485 polling. RTU requests block until the reply or timeout, so they are kept off the network core.
- An 8 s hardware watchdog restarts the controller if the main loop stops. The USR LED blinks while the loop runs.
- Hold **USER** while powering up to start on `192.168.4.1/24` instead of the saved address.

## Install

**Device check:** the installer only writes firmware built for the processor it finds. It reads the USB vendor ID of the serial port (refusing ESP32 and other non-RP2040/RP2350 boards), `INFO_UF2.TXT` on the install-mode drive (RP2040 or RP2350), the UF2 family ID inside the firmware file, and the chip the running firmware reports.

**Easiest: the [web installer](https://dineshpalsingh1989.github.io/IRIV-IOC-Gateway/#install)** (Chrome or Edge). It writes the firmware to the RP2350 drive and sends the dashboard's settings over USB. No PlatformIO needed.

The USB setup protocol it uses (115200 baud, one line per command; replies are `@IOC ` + JSON):

| Command | Reply |
| --- | --- |
| `HELLO` | `{"type":"hello","product":"iriv-ioc-gateway","version","host","ip","config","chip","chipRevision","builtFor","board","uid","rtc"}`. `chip` is read from the silicon: `RP2350A` (QFN60, as on the IRIV IOC) or `RP2350B`. `rtc` is `{"state":"ok","time":<unix UTC>}`, `{"state":"lost"}` (the PCF85063A stopped while unpowered: CR2032 flat or missing, or never set) or `{"state":"missing"}`. |
| `STATUS` | `{"type":"status", ip, link, online, modbus{clients,requests}, mqtt{connected}, rs485[{id,value,errors}], rtc}` |
| `TIME <unix UTC>` | sets the RTC and clears its lost flag; replies `{"type":"time","rtc":{...}}`. The installer sends this computer's time when the clock is lost or more than 2 s off. |
| `CONFIG <bytes>` then exactly that many bytes of JSON | `{"type":"saved"}`, then a restart. The file is checked with the real loader before it replaces the old one. |
| `REBOOT` / `BOOTLOADER` | restart / restart into install mode (RP2350 drive) |

**With PlatformIO:**

1. Build in the dashboard, then **Export setup**. This downloads `ioc_config.json`. Include the MQTT password when asked, or type it into the file's `mqtt.pass`.
2. Put the file at `firmware/data/ioc_config.json`. `data/ioc_config.example.json` shows the format.
3. Connect the controller over USB-C and run:

```sh
pio run -t upload     # firmware
pio run -t uploadfs   # data/ -> LittleFS (the settings file)
pio device monitor    # 115200 baud: IP address, MQTT and RS485 messages
```

Without a settings file the gateway starts with DHCP, Modbus TCP on port 502 (unit 255), MQTT and RS485 off, and AN0/AN1 in 0–10 V mode.

## Modbus TCP map

Addresses are 0-based protocol addresses. Rows marked **Cytron** are identical to Cytron's `iriv_ioc_modbus.py` v2.0.0, so existing SCADA tags work.

| Address | Table | Content | |
| --- | --- | --- | --- |
| 0x0000–0x000A | Discrete inputs (FC02) | DI0–DI10 (a counting input reads 0) | Cytron |
| 0x0100–0x0103 | Coils (FC01/05/15) | DO0–DO3 | Cytron |
| 0x0200–0x0201 | Input registers (FC04) | AN0/AN1 terminal voltage, mV | Cytron |
| 0x0210–0x0211 | Input registers | AN0/AN1 terminal current, µA | Cytron |
| 0x0300–0x0304 | Coils | Counter enable, DI1/3/5/7/9 | Cytron |
| 0x0310–0x0314 | Coils | Counter reset (write 1; clears itself) | Cytron |
| 0x0400–0x0409 | Input registers | 32-bit counts, high word first | Cytron (fixed) |
| 0x0F00–0x0F01, 0x0F10–0x0F12 | Input registers | Model "IOC", firmware version | Cytron |
| 0x0220–0x0223 | Input registers | AN0/AN1 scaled value, float32, NaN on a fault | Gateway |
| 0x0500 | Coils | Buzzer remote command | Gateway |
| 0x1000–0x107F | Input registers | RS485 points, float32 (scaled), NaN after 3 failed polls | Gateway |

Unit ID filter: the configured unit (default 255) and 255 are answered; 0 answers every unit. Float32 word order follows the Modbus setting (ABCD by default).

**Fixed from Cytron's firmware:** the counter high word. Cytron v2.0.0 sends `count >> 8`, which is wrong above 65,535 pulses; this firmware sends `count >> 16`.

## MQTT

Topics come from the dashboard's topic map (`map.mqtt` in the config). On/off states (DI, DO, buzzer) publish when they change; analog values, pulse counts and RS485 points publish on the interval. Payloads are `1`/`0`, numbers, or `NaN` for a fault or a device that stopped answering. Outputs and the buzzer accept `1`, `0`, `ON`, `OFF`, `true` or `false` on their command topic. Plain MQTT only: TLS does not hold a session on this board (see the Wilcoxon gateway notes).

## Checks

```sh
python tools/check_register_map.py   # dashboard and firmware use the same addresses
python tools/build_site.py           # Pages site: dashboard + installer + firmware/manifest.json
python tools/run_native_tests.py     # IRIVModbus core + IRIVIOCIo logic on a PC; needs g++, clang++ or zig (runs in CI)
pio run                              # compile for the IRIV IOC (passes)
```

GitHub Actions runs all three checks on every push and pull request, then republishes the dashboard, installer and firmware to GitHub Pages.

Hardware validation still needed: DI debounce and counters at 5 kHz, analog readings against a calibrated source (the ADC reference is the nominal 3.32 V), DO switching, buzzer, Modbus TCP with a real master (including 4 concurrent clients), RTU polling against real slaves at each baud rate and parity, MQTT reconnects, static IP, and watchdog recovery.
