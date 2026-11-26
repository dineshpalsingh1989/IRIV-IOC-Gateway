"""MQTT + output test of the IRIV IOC against the local broker. Switches DO0-DO3 and the buzzer briefly."""
import asyncio, socket, struct, sys, time
from amqtt.client import MQTTClient
from amqtt.mqtt.constants import QOS_0

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.206"
T = "iriv-test/ioc"
latest, seen, results = {}, {}, []

def check(name, ok, detail=""):
    results.append(ok); print(f"[{'PASS' if ok else 'FAIL'}] {name}  {detail}", flush=True)

class MB:
    def __init__(self): self.s = socket.create_connection((HOST, 502), timeout=3); self.tid = 0
    def req(self, fc, payload):
        self.tid += 1; pdu = bytes([fc]) + payload
        self.s.sendall(struct.pack(">HHHB", self.tid, 0, len(pdu) + 1, 255) + pdu)
        h = self.s.recv(7); n = struct.unpack(">HHHB", h)[2] - 1; b = b""
        while len(b) < n: b += self.s.recv(n - len(b))
        return b
    def bits(self, fc, addr, n):
        b = self.req(fc, struct.pack(">HH", addr, n)); return [b[2 + i // 8] >> (i % 8) & 1 for i in range(n)]
    def regs(self, addr, n):
        b = self.req(4, struct.pack(">HH", addr, n)); return list(struct.unpack(f">{n}H", b[2:2 + 2 * n]))
    def coil(self, addr, on): return self.req(5, struct.pack(">HH", addr, 0xFF00 if on else 0))

async def pump(c):
    while True:
        m = await c.deliver_message()
        t = m.publish_packet.variable_header.topic_name; v = m.publish_packet.payload.data.decode()
        latest[t] = v; seen.setdefault(t, []).append((time.time(), v))

async def wait_for(topic, value, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        if latest.get(topic) == value: return True
        await asyncio.sleep(0.05)
    return False

async def main():
    c = MQTTClient(client_id="iriv-ioc-tester")
    await c.connect("mqtt://127.0.0.1:1883/")
    await c.subscribe([(f"{T}/#", QOS_0)])
    task = asyncio.create_task(pump(c))
    print("Waiting up to 30 s for the IOC to connect and publish...", flush=True)
    want = [f"{T}/di{i}/state" for i in range(11)] + [f"{T}/do{i}/state" for i in range(4)] + \
           [f"{T}/an0/state", f"{T}/an1/state", f"{T}/buzzer/state"]
    end = time.time() + 30
    while time.time() < end and not all(w in latest for w in want): await asyncio.sleep(0.2)
    missing = [w for w in want if w not in latest]
    check("IOC connected and published all 18 topics", not missing, f"missing {missing}" if missing else "")
    if missing and len(missing) == len(want): return
    check("DI0-DI10 over MQTT", True, "".join(latest.get(f"{T}/di{i}/state", "?") for i in range(11)))
    check("AN0/AN1 over MQTT", True, f"{latest.get(f'{T}/an0/state')} / {latest.get(f'{T}/an1/state')}")

    # Interval: AN publishes every 2 s
    await asyncio.sleep(7)
    ts = [t for t, _ in seen.get(f"{T}/an0/state", [])][-3:]
    gaps = [round(b - a, 2) for a, b in zip(ts, ts[1:])]
    check("AN interval about 2 s", gaps and all(1.5 < g < 2.6 for g in gaps), f"gaps {gaps}")

    mb = MB()
    for i in range(4):
        await c.publish(f"{T}/do{i}/set", b"1")
        ok = await wait_for(f"{T}/do{i}/state", "1"); coil = mb.bits(1, 0x0100, 4)[i]
        check(f"DO{i} ON via MQTT -> state + Modbus coil", ok and coil == 1, f"mqtt={latest.get(f'{T}/do{i}/state')} coil={coil}")
        await asyncio.sleep(0.5)
        await c.publish(f"{T}/do{i}/set", b"OFF")
        ok = await wait_for(f"{T}/do{i}/state", "0"); coil = mb.bits(1, 0x0100, 4)[i]
        check(f"DO{i} OFF via MQTT (payload OFF)", ok and coil == 0, f"coil={coil}")
    for i in range(4):
        mb.coil(0x0100 + i, True)
        ok = await wait_for(f"{T}/do{i}/state", "1")
        check(f"DO{i} ON via Modbus FC05 -> MQTT state", ok)
        await asyncio.sleep(0.3)
        mb.coil(0x0100 + i, False)
        check(f"DO{i} OFF via Modbus FC05 -> MQTT state", await wait_for(f"{T}/do{i}/state", "0"))
    # FC15: all four on, then off
    r = mb.req(15, struct.pack(">HHB", 0x0100, 4, 1) + b"\x0f")
    on = all([await wait_for(f"{T}/do{i}/state", "1") for i in range(4)])
    r2 = mb.req(15, struct.pack(">HHB", 0x0100, 4, 1) + b"\x00")
    off = all([await wait_for(f"{T}/do{i}/state", "0") for i in range(4)])
    check("FC15 all DO on then off", r[0] == 15 and r2[0] == 15 and on and off)
    await c.publish(f"{T}/do0/set", b"garbage")
    await asyncio.sleep(1)
    check("Bad payload ignored", latest.get(f"{T}/do0/state") == "0" and mb.bits(1, 0x0100, 1)[0] == 0)

    # Buzzer (short)
    await c.publish(f"{T}/buzzer/set", b"1")
    ok = await wait_for(f"{T}/buzzer/state", "1"); bit = mb.bits(1, 0x0500, 1)[0]
    check("Buzzer ON via MQTT", ok and bit == 1, f"coil={bit}")
    await asyncio.sleep(1)
    await c.publish(f"{T}/buzzer/set", b"0")
    check("Buzzer OFF via MQTT", await wait_for(f"{T}/buzzer/state", "0"))

    # Counter on DI1 via Modbus: MQTT switches from state to count
    mb.coil(0x0300, True)
    ok = await wait_for(f"{T}/di1/state", "0", 5) and mb.bits(1, 0x0300, 1)[0] == 1
    check("Counter DI1 enable (FC05)", ok, f"count={mb.regs(0x0400, 2)}")
    mb.coil(0x0310, True); await asyncio.sleep(0.3)
    check("Counter reset clears itself", mb.bits(1, 0x0310, 1)[0] == 0 and mb.regs(0x0400, 2) == [0, 0])
    mb.coil(0x0300, False)
    check("Counter DI1 disable", mb.bits(1, 0x0300, 1)[0] == 0)

    # Writes to read-only tables must fail
    b = mb.req(6, struct.pack(">HH", 0x0200, 1)); check("FC06 to input register -> exception", b[0] & 0x80 == 0x80, b.hex())
    b = mb.req(5, struct.pack(">HH", 0x0100, 0x1234)); check("FC05 bad value -> exception 3", b[:2] == b"\x85\x03", b.hex())

    # Reconnect: drop the IOC's session by restarting? Not done here (needs broker restart).
    task.cancel(); await c.disconnect()
    print(f"\n{sum(results)}/{len(results)} passed")

asyncio.run(main())
