// Native tests for the IRIV IOC I/O logic. Run: python tests/run_native.py
#include "../src/IRIVIOCLogic.h"

#include <math.h>
#include <stdio.h>

using namespace iriv;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool near(float a, float b, float tol = 0.01f) { return fabsf(a - b) <= tol; }

static void testAnalog() {
    // Full-scale ADC reading at 3.3 V reference = 10.56 V at the terminal (datasheet full scale).
    CHECK(near(terminalVolts(4095, 3.3f), 10.56f));
    CHECK(near(analogSignal(AnalogMode::MilliAmp4To20, 4095, 3.3f), 42.58f, 0.05f)); // datasheet 42.58 mA
    CHECK(near(scaleSignal(AnalogMode::MilliAmp4To20, 12.0f, 0, 10), 5.0f));
    CHECK(near(scaleSignal(AnalogMode::Volt0To10, 7.36f, 0, 10), 7.36f));
    CHECK(near(scaleSignal(AnalogMode::MilliAmp0To20, 10.0f, -50, 150), 50.0f));
    CHECK(analogFault(AnalogMode::MilliAmp4To20, 2.0f) == AnalogFault::WireBreak);
    CHECK(analogFault(AnalogMode::MilliAmp4To20, 4.0f) == AnalogFault::None);
    CHECK(analogFault(AnalogMode::MilliAmp4To20, 21.5f) == AnalogFault::OverRange);
    CHECK(analogFault(AnalogMode::Volt0To10, 10.4f) == AnalogFault::None);
    CHECK(analogFault(AnalogMode::Volt0To10, 10.56f) == AnalogFault::OverRange);
    CHECK(parseAnalogMode("4-20mA") == AnalogMode::MilliAmp4To20 && parseAnalogMode("bogus") == AnalogMode::Volt0To10);
    CHECK(clampU16(70000.0f) == 65535 && clampU16(-3.0f) == 0 && clampU16(42.6f) == 43);
}

static void testCounter() {
    uint16_t r[2];
    counterWords(70000, r);
    CHECK(r[0] == 1 && r[1] == 4464); // Cytron v2.0.0's `>> 8` would give 273 for the high word
}

static void testBuzzer() {
    CHECK(patternOn(BuzzerPattern::Beep, 100) && !patternOn(BuzzerPattern::Beep, 300));
    CHECK(patternOn(BuzzerPattern::Continuous, 99999));
    CHECK(!patternOn(BuzzerPattern::Once, 500));

    BuzzerRule rule;
    parseTrigger("DI3", rule);
    CHECK(rule.trigger == BuzzerTrigger::Input && rule.source == 3);
    parseTrigger("AN1", rule);
    CHECK(rule.trigger == BuzzerTrigger::Analog && rule.source == 1);
    parseTrigger("DI3x", rule);
    CHECK(rule.trigger == BuzzerTrigger::Remote);
    parseTrigger("DI11", rule);
    CHECK(rule.trigger == BuzzerTrigger::Remote);

    bool inputs[11] = {};
    float values[2] = {0, 50};
    AnalogFault faults[2] = {AnalogFault::WireBreak, AnalogFault::None};
    BuzzerRule di;
    parseTrigger("DI4", di);
    CHECK(!buzzerWanted(di, inputs, values, faults, false));
    inputs[4] = true;
    CHECK(buzzerWanted(di, inputs, values, faults, false));
    BuzzerRule remoteOnly;
    CHECK(buzzerWanted(remoteOnly, inputs, values, faults, true) && !buzzerWanted(remoteOnly, inputs, values, faults, false));

    BuzzerRule an;
    parseTrigger("AN1", an);
    an.threshold = 40;
    CHECK(buzzerWanted(an, inputs, values, faults, false));
    an.above = false;
    CHECK(!buzzerWanted(an, inputs, values, faults, false));
    parseTrigger("AN0", an);
    CHECK(buzzerWanted(an, inputs, values, faults, false)); // wire break always alarms

    BuzzerState state;
    BuzzerRule quiet;
    quiet.pattern = BuzzerPattern::Continuous;
    quiet.silenceMs = 1000;
    bool tone = false;
    CHECK(state.update(true, 5000, quiet, tone) && tone);
    CHECK(!state.update(true, 6000, quiet, tone) && !tone);  // auto-silenced
    CHECK(!state.update(true, 9000, quiet, tone) && !tone);  // stays quiet while still wanted
    CHECK(!state.update(false, 9100, quiet, tone));
    CHECK(state.update(true, 9200, quiet, tone) && tone);    // a new alarm sounds again
}

int main() {
    testAnalog();
    testCounter();
    testBuzzer();
    printf(failures ? "%d check(s) failed\n" : "IRIVIOCIo logic: all checks passed\n", failures);
    return failures ? 1 : 0;
}
