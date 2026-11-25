#include "IRIVIOCIo.h"

namespace {
const uint8_t InputPins[IRIVIOCIo::InputCount] = {PIN_DI0, PIN_DI1, PIN_DI2, PIN_DI3, PIN_DI4, PIN_DI5,
                                                  PIN_DI6, PIN_DI7, PIN_DI8, PIN_DI9, PIN_DI10};
const uint8_t OutputPins[IRIVIOCIo::OutputCount] = {PIN_DO0, PIN_DO1, PIN_DO2, PIN_DO3};
const uint8_t AnalogPins[IRIVIOCIo::AnalogCount] = {PIN_AN0, PIN_AN1};
}

const uint8_t IRIVIOCIo::CounterInputs[5] = {1, 3, 5, 7, 9};

void IRIVIOCIo::begin() {
    for (uint8_t i = 0; i < InputCount; ++i) pinMode(InputPins[i], INPUT); // isolated, active high
    for (uint8_t i = 0; i < OutputCount; ++i) {
        pinMode(OutputPins[i], OUTPUT);
        digitalWrite(OutputPins[i], LOW); // outputs start off
    }
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    analogReadResolution(12);
}

void IRIVIOCIo::update(uint32_t now) {
    for (uint8_t i = 0; i < InputCount; ++i) {
        if (counting_[i]) { inputState_[i] = false; continue; }
        const bool raw = digitalRead(InputPins[i]) == HIGH;
        if (raw != rawState_[i]) { rawState_[i] = raw; changedAt_[i] = now; }
        else if (raw != inputState_[i] && now - changedAt_[i] >= debounceMs) inputState_[i] = raw;
    }
    for (uint8_t i = 0; i < AnalogCount; ++i) adc_[i] = uint16_t(analogRead(AnalogPins[i]));
}

void IRIVIOCIo::setOutput(uint8_t i, bool on) {
    if (i >= OutputCount) return;
    outputState_[i] = on;
    digitalWrite(OutputPins[i], on ? HIGH : LOW); // active high (datasheet 5.2.5)
}

bool IRIVIOCIo::isCounterInput(uint8_t input) {
    for (uint8_t c : CounterInputs) if (c == input) return true;
    return false;
}

void IRIVIOCIo::onPulse(void* counter) {
    ++*static_cast<volatile uint32_t*>(counter);
}

bool IRIVIOCIo::setCounter(uint8_t input, bool enable) {
    if (!isCounterInput(input)) return false;
    if (enable == counting_[input]) return true;
    const uint8_t pin = InputPins[input];
    if (enable) {
        counts_[input] = 0; // counting restarts from zero when enabled, as on Cytron's firmware
        attachInterruptParam(digitalPinToInterrupt(pin), onPulse, FALLING, (void*)&counts_[input]);
    } else {
        detachInterrupt(digitalPinToInterrupt(pin));
        rawState_[input] = inputState_[input] = false;
    }
    counting_[input] = enable;
    return true;
}

uint32_t IRIVIOCIo::count(uint8_t input) const {
    return input < InputCount ? counts_[input] : 0; // aligned 32-bit read is atomic on Cortex-M33
}

void IRIVIOCIo::resetCounter(uint8_t input) {
    if (input < InputCount) counts_[input] = 0;
}

void IRIVIOCIo::buzzer(bool on, uint16_t frequency) {
    const uint16_t wanted = on ? frequency : 0;
    if (wanted == toneHz_) return; // tone() restarts the PIO; only call it on a change
    toneHz_ = wanted;
    if (wanted) tone(PIN_BUZZER, wanted);
    else noTone(PIN_BUZZER);
}
