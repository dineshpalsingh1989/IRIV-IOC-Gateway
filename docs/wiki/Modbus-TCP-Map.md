# Modbus TCP map

Port 502 by default. Addresses are 0-based protocol addresses. Rows marked **Cytron** are identical to Cytron's own firmware (`iriv_ioc_modbus.py` v2.0.0), so existing SCADA tags keep working.

| Address | Table | Content | |
| --- | --- | --- | --- |
| 0x0000–0x000A | Discrete inputs (FC02) | DI0–DI10 (a counting input reads 0) | Cytron |
| 0x0100–0x0103 | Coils (FC01/05/15) | DO0–DO3 | Cytron |
| 0x0200–0x0201 | Input registers (FC04) | AN0/AN1 terminal voltage, mV | Cytron |
| 0x0210–0x0211 | Input registers | AN0/AN1 terminal current, µA | Cytron |
| 0x0300–0x0304 | Coils | Counter enable, DI1/3/5/7/9 | Cytron |
| 0x0310–0x0314 | Coils | Counter reset (write 1; clears itself) | Cytron |
| 0x0400–0x0409 | Input registers | 32-bit counts, high word first | Cytron (fixed) |
| 0x0F00–0x0F01, 0x0F10–0x0F12 | Input registers | Model "IOC", firmware version (major, minor, patch) | Cytron |
| 0x0220–0x0223 | Input registers | AN0/AN1 scaled value, float32, NaN on a fault | Gateway |
| 0x0500 | Coils | Buzzer remote command | Gateway |
| 0x1000–0x107F | Input registers | RS485 points, float32 (scaled), NaN after 3 failed polls | Gateway |

- **Unit ID:** the configured unit (default 255) and 255 are answered; 0 answers every unit.
- **Float32 word order** follows the Modbus setting (ABCD by default).
- **Clients:** up to 4 at once.
- **Errors:** an unmapped address answers exception 2, a bad count exception 3, an unsupported function exception 1, a write to a read-only table an exception.
- **Fixed from Cytron's firmware:** the counter high word. Cytron v2.0.0 sends `count >> 8`, wrong above 65,535 pulses; this firmware sends `count >> 16`.

The dashboard's **Connections → Modbus TCP map** table lists every register with its PLC reference (for example `30545–30546`) for your actual setup, including each RS485 point.
