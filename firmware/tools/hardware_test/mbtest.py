"""Read-only Modbus TCP check of the IRIV IOC gateway. No writes."""
import socket, struct, sys, time, math

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.206"
UNIT = 255
tid = 0
results = []

def check(name, ok, detail=""):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}  {detail}")

def conn():
    s = socket.create_connection((HOST, 502), timeout=3)
    return s

def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise ConnectionError("closed")
        b += c
    return b

def req(s, fc, payload, unit=UNIT):
    global tid
    tid = (tid + 1) & 0xFFFF
    pdu = bytes([fc]) + payload
    s.sendall(struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu)
    h = recv_exact(s, 7)
    rtid, proto, ln, ru = struct.unpack(">HHHB", h)
    body = recv_exact(s, ln - 1)
    assert rtid == tid and proto == 0, "bad MBAP"
    return body

def read(s, fc, addr, count, unit=UNIT):
    b = req(s, fc, struct.pack(">HH", addr, count), unit)
    if b[0] & 0x80: return ("exc", b[1])
    data = b[2:2 + b[1]]
    if fc in (1, 2):
        return [bool(data[i // 8] >> (i % 8) & 1) for i in range(count)]
    return list(struct.unpack(f">{count}H", data))

def f32(hi, lo): return struct.unpack(">f", struct.pack(">HH", hi, lo))[0]

s = conn()
di = read(s, 2, 0x0000, 11); check("DI0-DI10 (FC02)", isinstance(di, list), "".join("1" if x else "0" for x in di))
do = read(s, 1, 0x0100, 4); check("DO0-DO3 (FC01)", isinstance(do, list), str([int(x) for x in do]))
mv = read(s, 4, 0x0200, 2); check("AN mV", isinstance(mv, list), f"AN0 {mv[0]} mV, AN1 {mv[1]} mV")
ua = read(s, 4, 0x0210, 2); check("AN uA", isinstance(ua, list), f"AN0 {ua[0]} uA, AN1 {ua[1]} uA")
sc = read(s, 4, 0x0220, 4); check("AN scaled float", isinstance(sc, list), f"AN0 {f32(sc[0],sc[1]):.3f}, AN1 {f32(sc[2],sc[3]):.3f}")
ce = read(s, 1, 0x0300, 5); check("Counter enable", isinstance(ce, list), str([int(x) for x in ce]))
cn = read(s, 4, 0x0400, 10); check("Counters", isinstance(cn, list), str([cn[i] << 16 | cn[i+1] for i in range(0, 10, 2)]))
md = read(s, 4, 0x0F00, 2); txt = b"".join(struct.pack(">H", x) for x in md).decode(errors="replace").strip("\0 ")
check("Model = IOC", txt == "IOC", repr(txt))
ver = read(s, 4, 0x0F10, 3); check("Version = 0.3.0", ver == [0, 3, 0], str(ver))
bz = read(s, 1, 0x0500, 1); check("Buzzer coil", isinstance(bz, list), str([int(x) for x in bz]))
rs = read(s, 4, 0x1000, 4); check("RS485 block readable", isinstance(rs, list), f"point0 {f32(rs[0],rs[1])}, point1 {f32(rs[2],rs[3])}")

# Exceptions
e = read(s, 4, 0x7000, 1); check("Unmapped address -> exception 2", e == ("exc", 2), str(e))
e = read(s, 3, 0x0200, 0); check("Count 0 -> exception 3", e == ("exc", 3), str(e))
b = req(s, 0x2B, b"\x0e\x01\x00"); check("Unsupported FC 0x2B -> exception 1", b[0] == 0xAB and b[1] == 1, b.hex())
# Unit filter: unit 7 should be ignored (no reply) unless configured
try:
    s.settimeout(1.5); r = read(s, 4, 0x0F10, 3, unit=7); check("Unit 7 ignored", False, f"answered {r}")
except socket.timeout:
    check("Unit 7 ignored (no reply)", True)
s.close()

# Concurrency: 4 clients at once, each reads the version 20 times
socks = [conn() for _ in range(4)]
ok = True
t0 = time.time()
for i in range(20):
    for k in socks:
        ok &= read(k, 4, 0x0F10, 3) == [0, 3, 0]
check("4 clients x 20 requests", ok, f"{(time.time()-t0)*1000/80:.1f} ms per request")
for k in socks: k.close()

# Latency
s = conn(); t = []
for _ in range(50):
    t0 = time.perf_counter(); read(s, 4, 0x0200, 2); t.append((time.perf_counter() - t0) * 1000)
s.close(); t.sort()
check("50 reads latency", True, f"median {t[25]:.1f} ms, max {t[-1]:.1f} ms")
print(f"\n{sum(results)}/{len(results)} passed")
