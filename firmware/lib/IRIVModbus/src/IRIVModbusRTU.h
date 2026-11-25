#pragma once
// Modbus RTU master on a HardwareSerial port. The IRIV IOC's isolated RS485 driver switches
// direction itself, so no DE/RE pin is needed. Calls block until a reply or the timeout:
// run them on core 1 so the network on core 0 is not held up.
#include <Arduino.h>
#include "IRIVModbusCore.h"

class IRIVModbusRTUMaster {
public:
    explicit IRIVModbusRTUMaster(HardwareSerial& port) : port_(port) {}
    // config is SERIAL_8N1, SERIAL_8E1, SERIAL_8N2 ... Set the port's pins before begin().
    void begin(uint32_t baud, uint16_t config = SERIAL_8N1, uint32_t timeoutMs = 500);
    void setTimeout(uint32_t ms) { timeoutMs_ = ms; }

    // FC 3 (holding) or 4 (input): `count` registers into out.
    irivmb::RtuResult readRegisters(uint8_t unit, uint8_t function, uint16_t address, uint16_t count, uint16_t* out);
    // FC 1 (coils) or 2 (discrete inputs): `count` bits into out, one per element.
    irivmb::RtuResult readBits(uint8_t unit, uint8_t function, uint16_t address, uint16_t count, uint16_t* out);
    irivmb::RtuResult writeCoil(uint8_t unit, uint16_t address, bool on);
    irivmb::RtuResult writeRegister(uint8_t unit, uint16_t address, uint16_t value);
    uint8_t lastException() const { return exception_; }

private:
    irivmb::RtuResult transact(uint8_t unit, uint8_t function, uint16_t address, uint16_t value,
                               uint16_t count, uint16_t* out);
    HardwareSerial& port_;
    uint32_t timeoutMs_ = 500, silenceUs_ = 1750;
    uint8_t exception_ = 0;
    uint8_t buffer_[irivmb::MaxRtuAdu];
};
