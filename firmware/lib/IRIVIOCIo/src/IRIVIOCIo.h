#pragma once
// IRIVIOCIo: hardware I/O for the Cytron IRIV IO Controller (Arduino-Pico, RP2350A).
// 11 isolated digital inputs (pulse counters on DI1/3/5/7/9), 4 solid-state outputs,
// 2 analog inputs, the passive buzzer, USER button and USR LED. Pins from the datasheet.
// Call update() often from one core; the counters count in interrupts.
#include <Arduino.h>
#include "IRIVIOCLogic.h"

#define IRIVIOCIO_VERSION "0.1.0-alpha.1"

class IRIVIOCIo {
public:
    static const uint8_t InputCount = 11, OutputCount = 4, AnalogCount = 2;
    static const uint8_t CounterInputs[5]; // DI1, DI3, DI5, DI7, DI9 as on Cytron's firmware

    // ADC reference in volts. The IOC's 3.3 V rail measures about 3.32 V; calibrate per unit if needed.
    float adcReference = 3.32f;
    uint16_t debounceMs = 10;

    void begin();
    void update(uint32_t nowMs); // debounces inputs and samples the analog inputs

    bool input(uint8_t i) const { return i < InputCount && inputState_[i]; }
    const bool* inputs() const { return inputState_; }

    void setOutput(uint8_t i, bool on);
    bool output(uint8_t i) const { return i < OutputCount && outputState_[i]; }

    uint16_t adc(uint8_t i) const { return i < AnalogCount ? adc_[i] : 0; }
    float terminalVolts(uint8_t i) const { return iriv::terminalVolts(adc(i), adcReference); }

    // Pulse counting on DI1/3/5/7/9 (falling edge, up to the datasheet's 5 kHz).
    // A counting input reads inactive, as on Cytron's firmware.
    bool setCounter(uint8_t input, bool enable);
    bool counterEnabled(uint8_t input) const { return input < InputCount && counting_[input]; }
    uint32_t count(uint8_t input) const;
    void resetCounter(uint8_t input);

    void buzzer(bool on, uint16_t frequency);
    bool button() const { return digitalRead(PIN_BUTTON) == LOW; } // active low
    void led(bool on) { digitalWrite(PIN_LED, on ? HIGH : LOW); }

private:
    static bool isCounterInput(uint8_t input);
    static void onPulse(void* counter);
    bool inputState_[InputCount] = {}, rawState_[InputCount] = {}, counting_[InputCount] = {};
    uint32_t changedAt_[InputCount] = {};
    volatile uint32_t counts_[InputCount] = {};
    bool outputState_[OutputCount] = {};
    uint16_t adc_[AnalogCount] = {};
    uint16_t toneHz_ = 0;
};
