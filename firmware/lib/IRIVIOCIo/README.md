# IRIVIOCIo

Experimental 0.1.0-alpha.1 I/O library for the Cytron IRIV IO Controller (Arduino-Pico, RP2350A). Pins come from the board variant (datasheet section 5.1). Hardware validation is pending.

| Header | Contents |
| --- | --- |
| `IRIVIOCLogic.h` | Portable, no Arduino: ADC to terminal volts/milliamps (divider 16/5, 248 Ω shunt), linear scaling for 0–10 V, 0–20 mA and 4–20 mA, NAMUR NE43 faults (below 3.6 mA wire break, above 21 mA over range), 32-bit counter words, buzzer rules, patterns and auto-silence. |
| `IRIVIOCIo.h` | `IRIVIOCIo`: 11 debounced isolated inputs, pulse counters on DI1/3/5/7/9 (falling edge, interrupts), 4 solid-state outputs, 12-bit analog inputs, buzzer tone, USER button, USR LED. |

```cpp
#include <IRIVIOCIo.h>
IRIVIOCIo io;
void setup() { io.begin(); io.setCounter(1, true); }
void loop() {
    io.update(millis());
    const float mA = iriv::analogSignal(iriv::AnalogMode::MilliAmp4To20, io.adc(0), io.adcReference);
    io.setOutput(0, io.input(3));
}
```

Notes:

- The analog mode in software must match the AN-IN MODE DIP switch under the controller.
- `adcReference` defaults to 3.32 V, the value Cytron's examples use. Readings are not calibrated; calibrate per unit for accuracy.
- A counting input reads inactive, matching Cytron's firmware. Counting restarts from zero when enabled.
- Outputs are solid-state relays rated 50 V / 500 mA; add a flyback diode across relay coils.

Tests: `python ../../tools/run_native_tests.py` (needs g++, clang++ or zig). MIT licence.
