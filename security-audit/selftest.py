#!/usr/bin/env python3
"""Self-check for audit.py: starts a mock IOC (web API on an open port + a tiny Modbus server), runs the
auditor against it and asserts it reports the weaknesses we expect. No real device needed.

    python selftest.py
"""
import http.server, json, socket, struct, threading, audit


class MockWeb(http.server.BaseHTTPRequestHandler):
    STATUS = {"type": "status", "version": "0.3.3", "ip": "127.0.0.1", "uptime": 10,
              "mqtt": {"enabled": True, "host": "10.0.0.5"}, "io": {"di": [0] * 11, "do": [0, 0, 0, 0], "an": [0, 0]},
              "webPassword": True}
    CONFIG = {"network": {"dhcp": True}, "mqtt": {"host": "10.0.0.5", "pass": ""}, "rs485": {"points": []}}

    def _send(self, code, body=b"", ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/api/status":
            self._send(200, json.dumps(self.STATUS).encode())
        elif path == "/api/selftest":
            self._send(200, b'{"type":"selftest","ethernetChip":true}')
        elif path == "/api/config":
            self._send(200, json.dumps(self.CONFIG).encode())
        elif path == "/api/rs485/read":
            self._send(200, b'{"type":"rturead","ok":false}')
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        # Writes require a password (like the real firmware with one set): always refuse here.
        self._send(401, b"auth required", "text/plain")

    def log_message(self, *a):
        pass


def mock_modbus(sock):
    """Answers a model/version read so the auditor identifies a Modbus device; ignores the rest."""
    while True:
        try:
            conn, _ = sock.accept()
        except OSError:
            return
        with conn:
            head = conn.recv(7)
            if len(head) < 7:
                continue
            body = conn.recv(256)
            if len(body) < 5:
                continue
            fc, addr, count = body[0], struct.unpack(">H", body[1:3])[0], struct.unpack(">H", body[3:5])[0]
            if fc == 4 and addr == 0x0F00:
                regs = struct.pack(">HH", 0x494F, 0x4300)  # "IO" "C"
            elif fc == 4 and addr == 0x0F10:
                regs = struct.pack(">3H", 0, 3, 3)
            else:
                regs = b"\x00\x00" * count
            pdu = bytes([fc, len(regs)]) + regs
            conn.sendall(struct.pack(">HHHB", 1, 0, len(pdu) + 1, head[6]) + pdu)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    web_port = free_port()
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", web_port), MockWeb)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    mb = socket.socket()
    mb.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        mb.bind(("127.0.0.1", 502))
        mb.listen(4)
        threading.Thread(target=mock_modbus, args=(mb,), daemon=True).start()
        modbus_up = True
    except PermissionError:
        modbus_up = False  # binding 502 may need privileges; still test the web checks

    report = audit.audit("127.0.0.1", [502, web_port], 1.0, active=False)
    found = {f["id"] for f in report["findings"]}
    httpd.shutdown()
    mb.close()

    expect_web = {"web-plain-http", "web-open-info", "web-rs485-open"}
    missing = expect_web - found
    assert not missing, f"web checks missed: {missing} (found {found})"
    assert "web-config-passwords" not in found, "false positive: blanked passwords flagged as leaked"
    assert "web-write-noauth" not in found, "false positive: refused writes flagged as open"
    if modbus_up:
        assert "modbus-open" in found, f"modbus check missed modbus-open (found {found})"

    # The ranking must put high severity first.
    sev = [audit.SEVERITY_ORDER[f["severity"]] for f in report["findings"]]
    assert sev == sorted(sev), "findings are not ranked by severity"

    print(f"selftest OK: {len(report['findings'])} findings, modbus {'tested' if modbus_up else 'skipped (needs port 502)'}")
    print("  " + ", ".join(sorted(found)))


if __name__ == "__main__":
    main()
