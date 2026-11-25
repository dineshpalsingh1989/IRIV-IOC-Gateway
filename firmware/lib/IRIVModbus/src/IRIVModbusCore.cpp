#include "IRIVModbusCore.h"

#include <string.h>

namespace irivmb {

namespace {
uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
bool isBits(Area area) { return area == Area::Coils || area == Area::DiscreteInputs; }

size_t exception(uint8_t function, uint8_t code, uint8_t* response) {
    response[0] = uint8_t(function | 0x80);
    response[1] = code;
    return 2;
}

uint8_t readArea(Map& map, const Hooks& hooks, Area area, uint16_t address, uint16_t count,
                 uint8_t* out, size_t& written) {
    Block* block = map.find(area, address, count);
    if (!block) return IllegalDataAddress;
    if (hooks.beforeRead) {
        const uint8_t code = hooks.beforeRead(area, address, count, hooks.context);
        if (code) return code;
    }
    const uint16_t offset = uint16_t(address - block->start);
    if (isBits(area)) {
        written = (count + 7u) / 8u;
        memset(out, 0, written);
        for (uint16_t i = 0; i < count; ++i)
            if (block->bits[offset + i]) out[i >> 3] = uint8_t(out[i >> 3] | (1u << (i & 7)));
    } else {
        written = size_t(count) * 2u;
        for (uint16_t i = 0; i < count; ++i) put16(out + 2 * i, block->regs[offset + i]);
    }
    return Ok;
}
} // namespace

// ---- Map ----

bool Map::add(const Block& block) {
    if (count_ >= MaxBlocks || !block.count || uint32_t(block.start) + block.count > 0x10000u) return false;
    for (uint8_t i = 0; i < count_; ++i) {
        const Block& b = blocks_[i];
        if (b.area == block.area && block.start < uint32_t(b.start) + b.count &&
            b.start < uint32_t(block.start) + block.count)
            return false;
    }
    blocks_[count_++] = block;
    return true;
}

bool Map::addRegisters(Area area, uint16_t start, uint16_t count, uint16_t* regs) {
    return !isBits(area) && regs && add(Block{area, start, count, regs, nullptr});
}

bool Map::addBits(Area area, uint16_t start, uint16_t count, uint8_t* bits) {
    return isBits(area) && bits && add(Block{area, start, count, nullptr, bits});
}

Block* Map::find(Area area, uint16_t address, uint16_t count) {
    for (uint8_t i = 0; i < count_; ++i) {
        Block& b = blocks_[i];
        if (b.area == area && address >= b.start && uint32_t(address) + count <= uint32_t(b.start) + b.count)
            return &b;
    }
    return nullptr;
}

// ---- PDU server ----

size_t serve(Map& map, const Hooks& hooks, const uint8_t* q, size_t n, uint8_t* r) {
    if (!n) return 0;
    const uint8_t fn = q[0];
    const uint16_t address = n >= 3 ? be16(q + 1) : 0;
    const uint16_t value = n >= 5 ? be16(q + 3) : 0; // quantity, or the single value for FC 5/6
    size_t written = 0;
    uint8_t code = Ok;

    switch (fn) {
    case 1: case 2: case 3: case 4: {
        const bool bits = fn <= 2;
        const Area area = fn == 1 ? Area::Coils : fn == 2 ? Area::DiscreteInputs
                        : fn == 3 ? Area::HoldingRegisters : Area::InputRegisters;
        if (n != 5) return exception(fn, IllegalDataValue, r);
        if (!value || value > (bits ? 2000 : 125)) return exception(fn, IllegalDataValue, r);
        code = readArea(map, hooks, area, address, value, r + 2, written);
        if (code) return exception(fn, code, r);
        r[0] = fn;
        r[1] = uint8_t(written);
        return written + 2;
    }
    case 5: case 6: {
        if (n != 5) return exception(fn, IllegalDataValue, r);
        const Area area = fn == 5 ? Area::Coils : Area::HoldingRegisters;
        if (fn == 5 && value != 0xFF00 && value != 0x0000) return exception(fn, IllegalDataValue, r);
        Block* block = map.find(area, address, 1);
        if (!block) return exception(fn, IllegalDataAddress, r);
        const uint16_t offset = uint16_t(address - block->start);
        if (fn == 5) block->bits[offset] = value == 0xFF00;
        else block->regs[offset] = value;
        if (hooks.afterWrite) hooks.afterWrite(area, address, 1, hooks.context);
        memcpy(r, q, 5);
        return 5;
    }
    case 15: case 16: {
        if (n < 6) return exception(fn, IllegalDataValue, r);
        const bool bits = fn == 15;
        const uint8_t bytes = q[5];
        const size_t expected = bits ? (value + 7u) / 8u : size_t(value) * 2u;
        if (!value || value > (bits ? 1968 : 123) || bytes != expected || n != 6u + bytes)
            return exception(fn, IllegalDataValue, r);
        const Area area = bits ? Area::Coils : Area::HoldingRegisters;
        Block* block = map.find(area, address, value);
        if (!block) return exception(fn, IllegalDataAddress, r);
        const uint16_t offset = uint16_t(address - block->start);
        for (uint16_t i = 0; i < value; ++i) {
            if (bits) block->bits[offset + i] = (q[6 + (i >> 3)] >> (i & 7)) & 1;
            else block->regs[offset + i] = be16(q + 6 + 2 * i);
        }
        if (hooks.afterWrite) hooks.afterWrite(area, address, value, hooks.context);
        memcpy(r, q, 5);
        return 5;
    }
    default:
        return exception(fn, IllegalFunction, r);
    }
}

// ---- TCP ----

size_t tcpFrameSize(const uint8_t* h) {
    const uint16_t protocol = be16(h + 2), length = be16(h + 4);
    if (protocol != 0 || length < 2 || length > MaxPdu + 1) return 0;
    return 6u + length;
}

int serveTcp(Map& map, const Hooks& hooks, uint8_t unitFilter, const uint8_t* adu, size_t size,
             uint8_t* response) {
    if (size < 8 || tcpFrameSize(adu) != size) return -1;
    const uint8_t unit = adu[6];
    if (unitFilter && unit != unitFilter && unit != 255) return 0;
    const size_t pdu = serve(map, hooks, adu + 7, size - 7, response + 7);
    if (!pdu) return -1;
    memcpy(response, adu, 2); // transaction id
    put16(response + 2, 0);
    put16(response + 4, uint16_t(pdu + 1));
    response[6] = unit;
    return int(pdu + 7);
}

// ---- RTU ----

uint16_t crc16(const uint8_t* data, size_t size) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0xA001) : uint16_t(crc >> 1);
    }
    return crc;
}

size_t rtuRequest(uint8_t unit, uint8_t function, uint16_t address, uint16_t value, uint8_t* out) {
    if (!unit || unit > 247 || function < 1 || function > 6) return 0;
    if (function <= 4 && (!value || value > (function <= 2 ? 2000 : 125))) return 0;
    if (function == 5) value = value ? 0xFF00 : 0x0000;
    out[0] = unit;
    out[1] = function;
    put16(out + 2, address);
    put16(out + 4, value);
    const uint16_t crc = crc16(out, 6);
    out[6] = uint8_t(crc); // CRC goes low byte first
    out[7] = uint8_t(crc >> 8);
    return 8;
}

const char* rtuResultText(RtuResult result) {
    switch (result) {
    case RtuResult::Ok: return "ok";
    case RtuResult::Timeout: return "no reply";
    case RtuResult::Crc: return "CRC error";
    case RtuResult::Frame: return "bad reply";
    case RtuResult::Exception: return "device exception";
    case RtuResult::InvalidArgument: return "invalid request";
    default: return "serial error";
    }
}

RtuResult rtuParse(const uint8_t* reply, size_t size, uint8_t unit, uint8_t function,
                   uint16_t address, uint16_t count, uint16_t* out, uint8_t& exceptionCode) {
    exceptionCode = 0;
    if (!size) return RtuResult::Timeout;
    if (size < 5) return RtuResult::Frame;
    if (crc16(reply, size - 2) != (uint16_t(reply[size - 1]) << 8 | reply[size - 2])) return RtuResult::Crc;
    if (reply[0] != unit) return RtuResult::Frame;
    if (reply[1] == (function | 0x80)) {
        if (size != 5) return RtuResult::Frame;
        exceptionCode = reply[2];
        return RtuResult::Exception;
    }
    if (reply[1] != function) return RtuResult::Frame;
    if (function == 5 || function == 6) {
        // Echo of address and value. FC 5 echoes 0xFF00/0x0000, not the caller's 0/1.
        const uint16_t echoed = be16(reply + 4);
        const bool valueOk = function == 6 ? echoed == count : echoed == (count ? 0xFF00 : 0x0000);
        return size == 8 && be16(reply + 2) == address && valueOk ? RtuResult::Ok : RtuResult::Frame;
    }
    const size_t bytes = function <= 2 ? (count + 7u) / 8u : size_t(count) * 2u;
    if (reply[2] != bytes || size != 5 + bytes || !out) return RtuResult::Frame;
    for (uint16_t i = 0; i < count; ++i)
        out[i] = function <= 2 ? uint16_t((reply[3 + (i >> 3)] >> (i & 7)) & 1) : be16(reply + 3 + 2 * i);
    return RtuResult::Ok;
}

// ---- 32-bit values ----

WordOrder parseWordOrder(const char* t) {
    if (!t) return WordOrder::ABCD;
    if (!strcmp(t, "CDAB")) return WordOrder::CDAB;
    if (!strcmp(t, "BADC")) return WordOrder::BADC;
    if (!strcmp(t, "DCBA")) return WordOrder::DCBA;
    return WordOrder::ABCD;
}

void putU32(uint16_t* regs, uint32_t value, WordOrder order) {
    uint16_t high = uint16_t(value >> 16), low = uint16_t(value);
    if (order == WordOrder::BADC || order == WordOrder::DCBA) {
        high = uint16_t(high << 8 | high >> 8);
        low = uint16_t(low << 8 | low >> 8);
    }
    const bool swapped = order == WordOrder::CDAB || order == WordOrder::DCBA;
    regs[0] = swapped ? low : high;
    regs[1] = swapped ? high : low;
}

uint32_t getU32(const uint16_t* regs, WordOrder order) {
    const bool swapped = order == WordOrder::CDAB || order == WordOrder::DCBA;
    uint16_t high = swapped ? regs[1] : regs[0], low = swapped ? regs[0] : regs[1];
    if (order == WordOrder::BADC || order == WordOrder::DCBA) {
        high = uint16_t(high << 8 | high >> 8);
        low = uint16_t(low << 8 | low >> 8);
    }
    return uint32_t(high) << 16 | low;
}

void putFloat(uint16_t* regs, float value, WordOrder order) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    putU32(regs, bits, order);
}

float getFloat(const uint16_t* regs, WordOrder order) {
    const uint32_t bits = getU32(regs, order);
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

DataType parseDataType(const char* t) {
    if (!t) return DataType::Int16;
    if (!strcmp(t, "bool")) return DataType::Bool;
    if (!strcmp(t, "uint16")) return DataType::UInt16;
    if (!strcmp(t, "int32")) return DataType::Int32;
    if (!strcmp(t, "uint32")) return DataType::UInt32;
    if (!strcmp(t, "float32")) return DataType::Float32;
    return DataType::Int16;
}

uint8_t registerCount(DataType type) {
    return type == DataType::Int32 || type == DataType::UInt32 || type == DataType::Float32 ? 2 : 1;
}

float decode(const uint16_t* regs, DataType type, WordOrder order) {
    switch (type) {
    case DataType::Bool: return regs[0] ? 1.0f : 0.0f;
    case DataType::UInt16: return float(regs[0]);
    case DataType::Int16: return float(int16_t(regs[0]));
    case DataType::Int32: return float(int32_t(getU32(regs, order)));
    case DataType::UInt32: return float(getU32(regs, order));
    default: return getFloat(regs, order);
    }
}

} // namespace irivmb
