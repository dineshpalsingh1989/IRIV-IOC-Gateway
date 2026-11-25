#pragma once
// Pure I/O logic for the IRIV IOC: analog conversion and scaling, 4-20 mA fault detection,
// buzzer rules and patterns, pulse-counter words. No Arduino dependency (tested on a PC).
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace iriv {

// ---- Analog inputs (datasheet 5.2.3) ----
// The pin sees AN x 10k/(22k+10k) = AN x 5/16. Current mode adds a shunt: 248 ohm effective.
const float AnalogDivider = 16.0f / 5.0f;
const float CurrentShuntOhm = 248.0f;

enum class AnalogMode : uint8_t { Volt0To10, MilliAmp0To20, MilliAmp4To20 };

inline AnalogMode parseAnalogMode(const char* text) {
    if (text && !strcmp(text, "0-20mA")) return AnalogMode::MilliAmp0To20;
    if (text && !strcmp(text, "4-20mA")) return AnalogMode::MilliAmp4To20;
    return AnalogMode::Volt0To10;
}

inline bool isCurrent(AnalogMode mode) { return mode != AnalogMode::Volt0To10; }

// Terminal voltage (V) from a 12-bit reading. refVolts is the ADC reference (about 3.32 V on the IOC).
inline float terminalVolts(uint16_t adc12, float refVolts) {
    return float(adc12) / 4095.0f * refVolts * AnalogDivider;
}

// Signal in the mode's unit: volts for 0-10 V, milliamps for current modes.
inline float analogSignal(AnalogMode mode, uint16_t adc12, float refVolts) {
    const float volts = terminalVolts(adc12, refVolts);
    return isCurrent(mode) ? volts / CurrentShuntOhm * 1000.0f : volts;
}

inline float signalLow(AnalogMode mode) { return mode == AnalogMode::MilliAmp4To20 ? 4.0f : 0.0f; }
inline float signalHigh(AnalogMode mode) { return mode == AnalogMode::Volt0To10 ? 10.0f : 20.0f; }

// Linear scaling: signalLow -> lo, signalHigh -> hi.
inline float scaleSignal(AnalogMode mode, float signal, float lo, float hi) {
    const float l = signalLow(mode), h = signalHigh(mode);
    return lo + (signal - l) / (h - l) * (hi - lo);
}

enum class AnalogFault : uint8_t { None, WireBreak, OverRange };

// NAMUR NE43 on 4-20 mA loops: below 3.6 mA is a broken wire or failed sensor, above 21 mA a fault.
inline AnalogFault analogFault(AnalogMode mode, float signal) {
    if (mode == AnalogMode::MilliAmp4To20) {
        if (signal < 3.6f) return AnalogFault::WireBreak;
        if (signal > 21.0f) return AnalogFault::OverRange;
        return AnalogFault::None;
    }
    return signal > signalHigh(mode) * 1.05f ? AnalogFault::OverRange : AnalogFault::None;
}

inline const char* analogFaultText(AnalogFault fault) {
    return fault == AnalogFault::WireBreak ? "wire break" : fault == AnalogFault::OverRange ? "over range" : "";
}

// Millivolts / microamps as Cytron's firmware reports them (Modbus 0x0200 / 0x0210).
inline uint16_t clampU16(float value) {
    return value <= 0 ? 0 : value >= 65535.0f ? 65535 : uint16_t(value + 0.5f);
}

// ---- Pulse counters ----
// 32-bit count as two registers, high word first (Cytron map 0x0400). Cytron's CircuitPython
// v2.0.0 shifts by 8 instead of 16, which is wrong above 65535 pulses.
inline void counterWords(uint32_t count, uint16_t* regs) {
    regs[0] = uint16_t(count >> 16);
    regs[1] = uint16_t(count);
}

// ---- Buzzer ----
enum class BuzzerPattern : uint8_t { Continuous, Beep, Pulse, Once };

inline BuzzerPattern parsePattern(const char* text) {
    if (text && !strcmp(text, "continuous")) return BuzzerPattern::Continuous;
    if (text && !strcmp(text, "pulse")) return BuzzerPattern::Pulse;
    if (text && !strcmp(text, "once")) return BuzzerPattern::Once;
    return BuzzerPattern::Beep;
}

// Whether the tone is on `elapsedMs` after the alarm started.
inline bool patternOn(BuzzerPattern pattern, uint32_t elapsedMs) {
    switch (pattern) {
    case BuzzerPattern::Continuous: return true;
    case BuzzerPattern::Once: return elapsedMs < 300;
    case BuzzerPattern::Pulse: return elapsedMs % 1500 < 500;
    default: return elapsedMs % 400 < 200;
    }
}

enum class BuzzerTrigger : uint8_t { Remote, Input, Analog };

struct BuzzerRule {
    BuzzerTrigger trigger = BuzzerTrigger::Remote;
    uint8_t source = 0;       // DI number or AN number
    bool above = true;        // analog: sound above (true) or below the threshold
    float threshold = 0;
    uint16_t frequency = 2700;
    BuzzerPattern pattern = BuzzerPattern::Beep;
    uint32_t silenceMs = 0;   // 0 = sound until the condition clears
};

// Parses the dashboard's trigger text: "remote", "DI3", "AN1".
inline void parseTrigger(const char* text, BuzzerRule& rule) {
    rule.trigger = BuzzerTrigger::Remote;
    if (!text || strlen(text) < 3) return;
    char* end = nullptr;
    const long n = strtol(text + 2, &end, 10);
    if (end == text + 2 || *end) return; // not a plain number after "DI"/"AN"
    if (!strncmp(text, "DI", 2) && n >= 0 && n <= 10) { rule.trigger = BuzzerTrigger::Input; rule.source = uint8_t(n); }
    if (!strncmp(text, "AN", 2) && n >= 0 && n <= 1) { rule.trigger = BuzzerTrigger::Analog; rule.source = uint8_t(n); }
}

// Remote commands always work, whatever the trigger. A faulty analog sensor always alarms.
inline bool buzzerWanted(const BuzzerRule& rule, const bool* inputs, const float* analogValue,
                         const AnalogFault* analogFaults, bool remote) {
    if (remote) return true;
    switch (rule.trigger) {
    case BuzzerTrigger::Input: return inputs[rule.source];
    case BuzzerTrigger::Analog:
        if (analogFaults[rule.source] != AnalogFault::None) return true;
        return rule.above ? analogValue[rule.source] > rule.threshold : analogValue[rule.source] < rule.threshold;
    default: return false;
    }
}

// Tracks one alarm: when it started and whether auto-silence has cut in.
class BuzzerState {
public:
    // Returns whether the alarm is sounding (pattern aside) and sets toneOn for this instant.
    bool update(bool wanted, uint32_t nowMs, const BuzzerRule& rule, bool& toneOn) {
        if (!wanted) { active_ = silenced_ = false; toneOn = false; return false; }
        if (!active_) { active_ = true; startMs_ = nowMs; }
        const uint32_t elapsed = nowMs - startMs_;
        if (rule.silenceMs && elapsed >= rule.silenceMs) silenced_ = true; // quiet until the condition clears
        toneOn = !silenced_ && patternOn(rule.pattern, elapsed);
        return !silenced_;
    }
private:
    bool active_ = false, silenced_ = false;
    uint32_t startMs_ = 0;
};

} // namespace iriv
