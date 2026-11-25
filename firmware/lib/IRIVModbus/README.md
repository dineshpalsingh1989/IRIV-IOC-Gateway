# IRIVModbus

Experimental 0.1.0-alpha.1 Modbus library for the Cytron IRIV IO Controller (Arduino-Pico, RP2350). No third-party Modbus dependency. Hardware validation is pending.

| Header | Contents |
| --- | --- |
| `IRIVModbusCore.h` | Portable core, no Arduino: register `Map` of sparse blocks per table, PDU server for FC 01, 02, 03, 04, 05, 06, 15 and 16 with the required exceptions, TCP framing, RTU CRC/request/response checks, 32-bit and float helpers in ABCD/CDAB/BADC/DCBA order, device data-type decoding. |
| `IRIVModbusTCP.h` | `IRIVModbusTCPServer`: Modbus TCP over any Arduino-Pico lwIP interface (W5500). Uses `WiFiServer`/`WiFiClient`, because Arduino-Pico's lwIP has no BSD socket layer. 4 clients by default (`IRIVMODBUS_TCP_CLIENTS`), idle and incomplete-frame timeouts, unit filtering, statistics. |
| `IRIVModbusRTU.h` | `IRIVModbusRTUMaster` on a `HardwareSerial`: read coils/discrete inputs/holding/input registers, write single coil/register. Waits for 3.5 character times of bus silence. No DE/RE pin: the IOC's isolated RS485 driver switches direction itself. |

```cpp
#include <IRIVModbus.h>
uint16_t inputs[2];
uint8_t coils[4];
irivmb::Map map;
IRIVModbusTCPServer server(map);

void setup() {
    map.addRegisters(irivmb::Area::InputRegisters, 0x0200, 2, inputs);
    map.addBits(irivmb::Area::Coils, 0x0100, 4, coils);
    server.begin(502, 255);          // after Ethernet is up
}
void loop() { server.poll(); }
```

Notes:

- A request must fall inside one block; gaps answer exception 02 (illegal data address).
- Discrete inputs and input registers are read-only by function code; coils and holding registers are writable. Use `Hooks::afterWrite` to act on writes and `beforeRead` to refresh or refuse reads.
- RTU calls block until the reply or the timeout. On the IOC, run them on core 1 (`setup1()`/`loop1()`).
- Design follows the protocol core of [ForgeModbus](https://github.com/dineshpalsingh1989/ForgeModbus), whose server needs ESP32 sockets, and the RTU timing of Wilcoxon883MRTU.

Tests: `python ../../tools/run_native_tests.py` (needs g++, clang++ or zig). MIT licence.
