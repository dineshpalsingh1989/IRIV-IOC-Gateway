# Security audit

A LAN security auditor for the IRIV IOC Gateway. It scans the device over the network, probes the services it
finds for weak spots, and prints a ranked report so you can understand and close them. Python standard library
only, nothing to install.

> ⚠️ **Only run this against a device you own or are authorised to test.** Scanning other people's equipment
> without permission is likely illegal. This tool is for checking your own gateway on your own network.

## Run it

```sh
python audit.py 192.168.0.206                 # read-only: scan + probe + report
python audit.py 192.168.0.206 --json out.json # also save the full report as JSON
python audit.py 192.168.0.206 --active        # also prove weaknesses that change state (see below)
```

- **Default (read-only):** scans ports, reads Modbus and the web API, checks which endpoints answer without a
  password, and reports. It does not change anything on the device.
- **`--active`:** additionally switches output **DO0** over Modbus and switches it straight back, to prove that
  an unauthenticated write really does move a physical output. Anything wired to DO0 will click. The 8-second
  firmware watchdog is not affected.

Exit code is 1 when any **high** finding is reported, so it can gate a CI check.

## What it checks

| Area | Looks for |
| --- | --- |
| Ports | Open industrial and web/remote ports (502, 80, 443, 1883, 8883, 22, 23, 102, 44818, …) |
| Modbus TCP (502) | That it answers with no authentication; that writes can switch the real outputs (with `--active`) |
| Web page (80) | Plain HTTP; endpoints that leak device/network info without a password; whether stored passwords are blanked; whether any write is accepted without a password; the open RS485 read endpoint |
| Remote access | Telnet / FTP / RDP being open at all |

Each finding has a severity (high / medium / low / info), what it means, and how to fix it. Most of what it
reports on a healthy gateway is **by design** — Modbus has no login, and the page is readable so you can view it
— so the real message is usually "keep this device on a trusted, isolated network, never on the open internet".

## How it decides

The checks match the firmware in [`../firmware/src/main.cpp`](../firmware/src/main.cpp): the open endpoints
(`/api/status`, `/api/selftest`, `/api/config`, `/api/rs485/read`) and the password-protected ones
(`/api/config` POST, `/api/update`, `/api/testoutputs`, `/api/identify`). If you change the firmware's auth,
update the checks here too.

## Test it without a device

```sh
python selftest.py
```

Starts a mock IOC (a fake web API and a tiny Modbus server) in the same process, runs the auditor against it and
asserts it reports the expected findings and ranks them by severity. Runs in CI; needs no hardware. (The Modbus
part binds port 502, which some systems only allow with elevated privileges; the self-check skips it and still
tests the web checks if it cannot.)
