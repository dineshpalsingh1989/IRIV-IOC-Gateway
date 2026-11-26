"""Hold test: count AN0 messages for N seconds; report gaps (a dropped TLS session shows as a gap of 10 s or more)."""
import asyncio, sys, time
from amqtt.client import MQTTClient
from amqtt.mqtt.constants import QOS_0

async def main(seconds):
    c = MQTTClient(client_id="iriv-hold")
    await c.connect("mqtt://127.0.0.1:1883/")
    await c.subscribe([("iriv-test/ioc/an0/state", QOS_0)])
    ts, end = [], time.time() + seconds
    while time.time() < end:
        try: m = await c.deliver_message(timeout_duration=end - time.time())
        except Exception: break
        if m: ts.append(time.time())
    await c.disconnect()
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    print(f"{len(ts)} messages in {seconds} s, expected about {seconds // 2}; largest gap {max(gaps):.2f} s")

asyncio.run(main(int(sys.argv[1])))
