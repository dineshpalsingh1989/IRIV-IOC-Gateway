# MQTT

Set up under **Connections → MQTT**: broker address, port, client ID, base topic, optional user and password, command QoS, analog publish interval and retain.

## Topics and payloads

- Every channel and RS485 point gets a topic under the base topic, for example `factory/line01/ioc01/di0/state`. Outputs and the buzzer also get a command topic, `…/do0/set`.
- You can type your own topic per channel (Channels or the MQTT topic table) or per RS485 point (the point form suggests free ones). A topic already used by another channel or point is refused, and so are `+`, `#` and empty levels.
- On/off states (DI, DO, buzzer) publish **when they change**. Analog values, pulse counts and RS485 points publish **every interval**.
- Payloads: `1` / `0`, numbers, or `NaN` for a sensor fault or a device that stopped answering.
- Commands: outputs and the buzzer accept `1`, `0`, `ON`, `OFF`, `true` or `false`. Anything else is ignored.
- Publishing is QoS 0. The command QoS setting is for subscribing.

## Port 1883 and 8883 (TLS)

| Setting | Result |
| --- | --- |
| Port **1883**, TLS off | Plain MQTT |
| Port **8883**, **Encrypt with TLS** on, no CA certificate | Encrypted, but the controller does **not** check who the broker is |
| Port **8883**, TLS on, **Broker CA certificate** pasted (PEM) | Encrypted, and the broker's certificate is checked against the CA |

Tested on a real IRIV IOC: all three cases passed 29/29 checks, with data both ways (controller to broker: all topics with real values; broker to controller: `do0/set = 1` switched the real output). A certificate from a different CA was refused and nothing was sent. Sessions held without reconnects.

### Rules for checking the broker (CA certificate)

- The controller checks the certificate dates against its **clock (RTC)**. If the clock is lost it does not connect and logs why: set the time by connecting the installer over USB (it sets the clock from your computer).
- The broker's certificate must list the **address you typed as a DNS name**, even when that address is an IP. For a broker at `192.168.0.10` the certificate needs `DNS:192.168.0.10` (and usually `IP:192.168.0.10`). The controller's TLS library does not match IP entries.
- The CA certificate can be up to 3071 characters.

### Not yet tested

Brokers other than the test broker (Mosquitto, EMQX, HiveMQ, cloud brokers) and username/password logins. They use the same MQTT 3.1.1 and TLS, but have not been tried.
