#include "IRIVModbusRTU.h"

using irivmb::RtuResult;

void IRIVModbusRTUMaster::begin(uint32_t baud, uint16_t config, uint32_t timeoutMs) {
    timeoutMs_ = timeoutMs;
    // Frames end after 3.5 character times of silence; the spec fixes 1.75 ms above 19200 baud.
    // 11 bits per character covers 8 data bits with parity or a second stop bit.
    silenceUs_ = baud > 19200 ? 1750 : uint32_t((3500000ULL * 11 + baud - 1) / baud);
    port_.begin(baud, config);
}

RtuResult IRIVModbusRTUMaster::readRegisters(uint8_t unit, uint8_t function, uint16_t address,
                                            uint16_t count, uint16_t* out) {
    if ((function != 3 && function != 4) || !count || count > 125) return RtuResult::InvalidArgument;
    return transact(unit, function, address, count, count, out);
}

RtuResult IRIVModbusRTUMaster::readBits(uint8_t unit, uint8_t function, uint16_t address,
                                       uint16_t count, uint16_t* out) {
    if ((function != 1 && function != 2) || !count || count > 2000) return RtuResult::InvalidArgument;
    return transact(unit, function, address, count, count, out);
}

RtuResult IRIVModbusRTUMaster::writeCoil(uint8_t unit, uint16_t address, bool on) {
    return transact(unit, 5, address, on, on, nullptr);
}

RtuResult IRIVModbusRTUMaster::writeRegister(uint8_t unit, uint16_t address, uint16_t value) {
    return transact(unit, 6, address, value, value, nullptr);
}

RtuResult IRIVModbusRTUMaster::transact(uint8_t unit, uint8_t function, uint16_t address,
                                        uint16_t value, uint16_t count, uint16_t* out) {
    exception_ = 0;
    uint8_t request[8];
    if (!irivmb::rtuRequest(unit, function, address, value, request)) return RtuResult::InvalidArgument;

    // Drop stale bytes, then wait for a quiet bus so the request is not glued to other traffic.
    uint32_t start = millis(), lastByte = micros();
    while (micros() - lastByte < silenceUs_) {
        while (port_.available()) { port_.read(); lastByte = micros(); }
        if (millis() - start >= timeoutMs_) return RtuResult::Timeout;
    }
    if (port_.write(request, sizeof(request)) != sizeof(request)) return RtuResult::Transport;
    port_.flush(); // wait until sent, before timing the reply

    size_t used = 0;
    start = millis();
    lastByte = micros();
    while (millis() - start < timeoutMs_) {
        if (port_.available()) {
            const int byte = port_.read();
            if (byte < 0) continue;
            if (used == sizeof(buffer_)) return RtuResult::Frame;
            buffer_[used++] = uint8_t(byte);
            lastByte = micros();
        } else if (used && micros() - lastByte >= silenceUs_) {
            break; // end of frame
        }
    }
    return irivmb::rtuParse(buffer_, used, unit, function, address, count, out, exception_);
}
