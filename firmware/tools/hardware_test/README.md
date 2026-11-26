# Hardware tests

Bench tests for a real IRIV IOC running the gateway firmware. They switch DO0–DO3 and sound the buzzer, so disconnect anything wired to the outputs first.

```sh
python -m venv .venv && .venv/Scripts/pip install pyserial amqtt cryptography
python mbtest.py 192.168.0.206          # Modbus TCP, read-only: every table, error replies, 4 clients
python usb.py COM11 HELLO SELFTEST STATUS
python usb.py COM11 TIME                # set the RTC from this computer
python usb.py COM11 CONFIG:settings.json   # send settings; the controller restarts
```

MQTT needs a broker on this computer that the IOC can reach (allow Python through Windows Firewall):

```sh
python mkcert.py                        # test CA + broker certificate for 8883 (edit the IP inside first; it goes in as IP and DNS name)
amqtt -c broker.yaml                    # listeners on 1883 and 8883 (ssl: true, certfile, keyfile)
python mqtttest.py 192.168.0.206        # 29 checks: topics, DO0-3 via MQTT and Modbus, buzzer, counter
python hold.py 300                      # session holds: AN0 every interval, no gaps
```

The settings sent to the IOC must point MQTT at this computer with base topic `iriv-test/ioc`, interval 2 s and retain on. For TLS add `"tls": true`, port 8883 and, to check the broker, `"ca"` with the contents of `ca.pem`.

Results of 26 Nov 2026 (firmware 0.3.0, unit 1A86F8806D886257) are in [docs/notes/2026-11-26-session.md](../../../docs/notes/2026-11-26-session.md).
