# Install

You need **Chrome or Edge on a computer** and a **USB-C data cable** (some cables only charge).

1. Open the [web installer](https://dineshpalsingh1989.github.io/IRIV-IOC-Gateway/#install).
2. Plug in the controller and click **Install**, then pick it in the list Chrome shows.
3. Watch the **install log**. It ends with the controller's IP address:

```
==== IRIV-IOC_886257 · IRIV IOC Gateway 0.3.3 is running ====
IP address : 192.168.0.206 (DHCP)
Modbus TCP : port 502
MQTT       : connected to 192.168.0.138 (TLS, broker checked)
Self-test  : no problems found
```

The newest firmware comes with the page from GitHub: there is nothing to download.

## What Install does

| On the controller | What happens |
| --- | --- |
| Older gateway, **0.3.1 or newer** | Updated straight over USB. The controller checks the file (MD5), installs it and restarts. |
| Older gateway, **0.3.0 or older** | Put in install mode. Pick the **RP2350** drive with **Write firmware to the RP2350 drive**, once. |
| **Up to date** | Nothing is installed. The hardware is checked and the report shown. |
| **Other firmware** (factory, CircuitPython, MicroPython) | You are asked before it is deleted, then install mode and the drive. |
| **New or empty** | It appears as a drive called **RP2350**, not a USB port. Open **New controller, or install by hand** and click **Write firmware to the RP2350 drive**. |

After every install the installer reconnects, runs the full check (Ethernet chip, clock, flash, temperature) and writes the result to the log. Browsers only write to a drive you pick yourself, which is why the drive cases need that one extra click.

## Install mode by hand

Hold **BOOT**, tap **RESET**, release BOOT. Both are pinholes on the front: use a paperclip. A drive called **RP2350** appears.

## Send your settings

Set up channels, MQTT, Modbus, RS485 and the web password in the dashboard, then on the Install page click **Connect over USB** and **Send settings**. Tick **Include the passwords** so the controller gets the MQTT and web passwords. The controller checks the file, saves it and restarts.

Settings survive firmware updates. After any restart, DO0–DO3 start **off**.

## After the first install

Every later update can come over the LAN from the [[controller's own page|Controller-Web-Page]]: **Install → Update from GitHub**. No USB needed.

## Other ways

- **Download .uf2** and drag it onto the RP2350 drive (any browser).
- Release files: https://github.com/dineshpalsingh1989/IRIV-IOC-Gateway/releases
- Developers: build with PlatformIO, see the main README.
