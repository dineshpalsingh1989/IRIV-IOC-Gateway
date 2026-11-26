#!/usr/bin/env python3
"""LAN security audit for an IRIV IOC Gateway (or any host). Scans ports, probes the services it finds
(Modbus TCP, the controller web page) for weak spots, and prints a ranked report.

    python audit.py 192.168.0.206
    python audit.py 192.168.0.206 --active --json report.json

Only run this against a device you own or are authorised to test. By default it only reads; --active also
runs tests that change state briefly (it switches a Modbus output and switches it back) to prove a weakness.
Stdlib only: no packages to install.
"""
import argparse, concurrent.futures, http.client, json, socket, struct, sys, time

# Ports worth checking on an industrial LAN device: web, industrial protocols, remote access, MQTT.
DEFAULT_PORTS = [21, 22, 23, 25, 53, 80, 102, 443, 502, 587, 623, 1883, 2404, 4840, 8000,
                 8080, 8443, 8883, 9600, 20000, 44818, 47808]
SEVERITY_ORDER = {"high": 0, "medium": 1, "low": 2, "info": 3}


def tcp_open(ip, port, timeout):
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect((ip, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def scan_ports(ip, ports, timeout):
    open_ports = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
        for port, is_open in zip(ports, pool.map(lambda p: tcp_open(ip, p, timeout), ports)):
            if is_open:
                open_ports.append(port)
    return open_ports


# ---- Modbus TCP (port 502) ----
def modbus_request(ip, fc, payload, unit=255, timeout=3):
    s = socket.create_connection((ip, 502), timeout=timeout)
    try:
        pdu = bytes([fc]) + payload
        s.sendall(struct.pack(">HHHB", 1, 0, len(pdu) + 1, unit) + pdu)
        head = s.recv(7)
        if len(head) < 7:
            return None
        length = struct.unpack(">HHHB", head)[2]
        body = b""
        while len(body) < length - 1:
            chunk = s.recv(length - 1 - len(body))
            if not chunk:
                break
            body += chunk
        return body
    finally:
        s.close()


def modbus_read(ip, fc, addr, count, unit=255):
    b = modbus_request(ip, fc, struct.pack(">HH", addr, count), unit)
    if not b or b[0] & 0x80:
        return None
    return b[2:2 + b[1]]


def probe_modbus(ip, active):
    findings = []
    model = modbus_read(ip, 4, 0x0F00, 2)  # 0x0F00 = "IOC" on this gateway
    version = modbus_read(ip, 4, 0x0F10, 3)
    name = model.decode(errors="replace").strip("\0 ") if model else "?"
    ver = ".".join(str(x) for x in struct.unpack(">3H", version)) if version and len(version) == 6 else "?"
    findings.append(dict(id="modbus-open", severity="high", title="Modbus TCP 502 is open with no authentication",
        detail=f"The device answered Modbus TCP (model {name!r}, version {ver}). Modbus has no login: anyone on "
               "the LAN can read every register and write the coils, which switch the real outputs DO0-DO3, sound "
               "the buzzer and reset the pulse counters.",
        fix="Put the device on an isolated OT VLAN, allow port 502 only from the SCADA/PLC host (firewall or "
            "managed switch ACL), and never expose it to the internet or Wi-Fi guests."))
    # Unit-id filter: does unit 0 (broadcast) get an answer it should not?
    if modbus_read(ip, 4, 0x0F10, 3, unit=1) is not None:
        findings.append(dict(id="modbus-unit-any", severity="info", title="Modbus answers unit IDs other than its own",
            detail="The server replied to a request for unit 1. The firmware answers the configured unit, 255 and 0 "
                   "by design; this only matters if you rely on the unit ID to separate devices on one gateway.",
            fix="If unit filtering matters to you, set a specific unit ID and have masters use it."))
    if active:
        before = modbus_read(ip, 1, 0x0100, 4)  # DO0-DO3 coils
        if before is not None:
            state0 = before[0] & 1
            modbus_request(ip, 5, struct.pack(">HH", 0x0100, 0xFF00 if not state0 else 0x0000))  # flip DO0
            after = modbus_read(ip, 1, 0x0100, 4)
            modbus_request(ip, 5, struct.pack(">HH", 0x0100, 0xFF00 if state0 else 0x0000))  # restore
            if after is not None and (after[0] & 1) != state0:
                findings.append(dict(id="modbus-write-proven", severity="high",
                    title="An unauthenticated Modbus write switched a physical output",
                    detail="The audit toggled coil 0x0100 (output DO0) over the LAN with no credentials and saw the "
                           "state change, then set it back. Anything wired to DO0-DO3 could be switched by any host.",
                    fix="Same as above: restrict who can reach port 502. Consider a Modbus-aware firewall that blocks "
                        "write function codes (5, 6, 15, 16) from hosts that only need to read."))
    return findings


# ---- Controller web page (port 80) ----
def http_get(ip, path, timeout=5, method="GET", port=80, body=None):
    conn = http.client.HTTPConnection(ip, port, timeout=timeout)
    try:
        conn.request(method, path, body=body)
        r = conn.getresponse()
        data = r.read(65536)
        return r.status, dict(r.getheaders()), data
    except OSError:
        return None, {}, b""
    finally:
        conn.close()


def probe_web(ip, port, active):
    findings = []
    root_code, _, _ = http_get(ip, "/", port=port)
    status_code, headers, body = http_get(ip, "/api/status", port=port)
    if root_code is None and status_code is None:
        return []  # nothing answered plain HTTP here (closed, or HTTPS-only)
    is_ioc = status_code == 200 and b'"type"' in body and b'"io"' in body
    findings.append(dict(id="web-plain-http", severity="medium", title=f"The web page is plain HTTP on port {port}",
        detail="The controller serves its dashboard and API over unencrypted HTTP. Settings, live values and the "
               "firmware upload travel in clear text; a password check uses HTTP digest (not sent in the clear) but "
               "the rest of the session can be read or changed by anyone sharing the network path.",
        fix="Keep it on a trusted, isolated LAN. Do not port-forward it. For remote access use a VPN, not a public "
            "port."))
    # Open, no-auth endpoints that leak information.
    leaks = []
    for path, what in [("/api/status", "IP, uptime, MQTT broker address and live I/O"),
                       ("/api/selftest", "flash usage, chip temperature and input states"),
                       ("/api/config", "network settings, MQTT broker/topics and the RS485 device map")]:
        code, _, data = http_get(ip, path, port=port)
        if code == 200 and data.strip().startswith(b"{"):
            leaks.append(f"{path} ({what})")
    if leaks:
        findings.append(dict(id="web-open-info", severity="medium", title="Device information is readable without a password",
            detail="These endpoints answer anyone on the LAN with no login: " + "; ".join(leaks) + ".",
            fix="This is by design so the page can be viewed read-only, but it tells an attacker the network layout "
                "and the broker to target. Keep the device off untrusted networks."))
    # Does /api/config leak the stored passwords? It should blank them.
    code, _, data = http_get(ip, "/api/config", port=port)
    if code == 200:
        try:
            cfg = json.loads(data)
            if (cfg.get("mqtt", {}).get("pass")) or cfg.get("web", {}).get("password"):
                findings.append(dict(id="web-config-passwords", severity="high",
                    title="Stored passwords are returned by /api/config",
                    detail="The settings endpoint returned a non-empty MQTT or web password. These should be blanked "
                           "before the settings leave the controller.",
                    fix="Update the firmware: webGetConfig() must clear mqtt.pass and remove the web section."))
        except ValueError:
            pass
    # Write endpoints must reject an unauthenticated request.
    no_auth_writes = []
    for path in ["/api/testoutputs", "/api/identify", "/api/config", "/api/update"]:
        code, _, _ = http_get(ip, path, port=port, method="POST", body=b"")
        if code is not None and code not in (401, 403):
            no_auth_writes.append(f"{path} -> HTTP {code}")
    if no_auth_writes:
        findings.append(dict(id="web-write-noauth", severity="high",
            title="A state-changing request was accepted without a password",
            detail="These POST endpoints did not answer 401/403 to a request with no credentials: "
                   + "; ".join(no_auth_writes) + ". They can change settings, switch outputs or replace the firmware.",
            fix="Set a web password (Connections -> Web page) and send it over USB. If a write still works without "
                "one, update the firmware so every write calls webAllowed()."))
    elif is_ioc:
        # Is a web password set at all?
        try:
            if json.loads(body).get("webPassword") is False:
                findings.append(dict(id="web-no-password", severity="medium", title="No web password is set",
                    detail="Writes are refused (good), but no password is configured, so the page is read-only and "
                           "nobody can manage the device over the LAN until one is set.",
                    fix="Set a web password under Connections -> Web page and send the settings over USB once."))
        except ValueError:
            pass
    # The RS485 read endpoint is open and talks to the field bus.
    code, _, _ = http_get(ip, "/api/rs485/read?slave=1&fn=3&reg=0&count=1&baud=9600&parity=None&stop=1&timeout=200", port=port)
    if code == 200:
        findings.append(dict(id="web-rs485-open", severity="low", title="The RS485 bus can be probed without a password",
            detail="/api/rs485/read lets anyone on the LAN send Modbus RTU reads onto the field bus. It cannot write, "
                   "but it can scan the bus and add traffic.",
            fix="Keep the device on a trusted network; restrict port 80 to the operators who need the page."))
    if is_ioc and "strict-transport-security" not in {k.lower() for k in headers}:
        findings.append(dict(id="web-headers", severity="info", title="No extra HTTP security headers",
            detail="The page sets no HSTS/CSP headers. Minor on a plain-HTTP LAN device, listed for completeness.",
            fix="Not worth changing for a LAN tool; relevant only if the page is ever put behind HTTPS."))
    return findings


def rank(findings):
    return sorted(findings, key=lambda f: SEVERITY_ORDER.get(f["severity"], 9))


def audit(ip, ports, timeout, active):
    report = {"target": ip, "time": time.strftime("%Y-%m-%d %H:%M:%S"), "open_ports": [], "findings": []}
    if not any(tcp_open(ip, p, timeout) for p in (80, 502, 22, 443)):
        if not tcp_open(ip, 80, timeout * 2):
            report["error"] = f"{ip} did not answer on any checked port. Is it powered on and on this network?"
            return report
    report["open_ports"] = scan_ports(ip, ports, timeout)
    findings = []
    if 502 in report["open_ports"]:
        findings += probe_modbus(ip, active)
    # Try the web probe on any open port that is not a known non-HTTP service; probe_web is a no-op
    # on ports that do not answer plain HTTP, so an HTTP server on an unusual port is still found.
    non_web = {502, 22, 23, 21, 1883, 8883, 102, 2404, 20000, 44818, 47808, 623, 9600, 4840, 53, 25, 587}
    for web_port in report["open_ports"]:
        if web_port not in non_web:
            findings += probe_web(ip, web_port, active)
    for port in report["open_ports"]:
        if port in (23, 21) or port in (3389,):
            findings.append(dict(id=f"port-{port}", severity="high",
                title=f"Port {port} is open ({'Telnet' if port == 23 else 'FTP' if port == 21 else 'RDP'})",
                detail="This is a cleartext or remote-access service and should not be on an industrial device.",
                fix="Turn the service off, or block the port at the firewall."))
    report["findings"] = rank(findings)
    return report


def print_report(report):
    print(f"\nSecurity audit of {report['target']} at {report['time']}")
    if report.get("error"):
        print("  " + report["error"])
        return
    print(f"Open ports: {', '.join(str(p) for p in report['open_ports']) or 'none'}")
    counts = {}
    for f in report["findings"]:
        counts[f["severity"]] = counts.get(f["severity"], 0) + 1
    print("Findings: " + (", ".join(f"{counts[s]} {s}" for s in ('high', 'medium', 'low', 'info') if s in counts) or "none") + "\n")
    marks = {"high": "[HIGH]", "medium": "[MED ]", "low": "[LOW ]", "info": "[INFO]"}
    for f in report["findings"]:
        print(f"{marks[f['severity']]} {f['title']}")
        print(f"        {f['detail']}")
        print(f"        Fix: {f['fix']}\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description="LAN security audit for an IRIV IOC Gateway.")
    ap.add_argument("ip", help="target IP address (a device you own or may test)")
    ap.add_argument("--active", action="store_true", help="also run tests that briefly change state (switch DO0 and back)")
    ap.add_argument("--ports", help="comma-separated ports to scan (default: common industrial + web ports)")
    ap.add_argument("--timeout", type=float, default=1.5, help="per-port connect timeout in seconds")
    ap.add_argument("--json", help="also write the full report to this JSON file")
    args = ap.parse_args(argv)
    ports = [int(p) for p in args.ports.split(",")] if args.ports else DEFAULT_PORTS
    report = audit(args.ip, ports, args.timeout, args.active)
    print_report(report)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print(f"Full report written to {args.json}")
    return 1 if any(f["severity"] == "high" for f in report["findings"]) else 0


if __name__ == "__main__":
    sys.exit(main())
