#pragma once
// Portable Modbus core: register map with sparse blocks, PDU server (FC 01-06, 15, 16),
// TCP framing, RTU CRC/request/response, and 32-bit value helpers.
// No Arduino, socket or RTOS dependencies, so it builds and is tested on a PC.
#include <stddef.h>
#include <stdint.h>

namespace irivmb {

enum class Area : uint8_t { Coils, DiscreteInputs, InputRegisters, HoldingRegisters };

enum Exception : uint8_t {
    Ok = 0,
    IllegalFunction = 1,
    IllegalDataAddress = 2,
    IllegalDataValue = 3,
    DeviceFailure = 4,
};

// Consecutive addresses in one table. Registers use `regs`; coils and discrete inputs use
// `bits`, one byte per item (0 or 1). Storage is owned by the application.
struct Block {
    Area area;
    uint16_t start, count;
    uint16_t* regs;
    uint8_t* bits;
};

// Called inside serve(). beforeRead may refresh the values or return an exception code.
// afterWrite runs once the written values are stored.
struct Hooks {
    uint8_t (*beforeRead)(Area area, uint16_t address, uint16_t count, void* context) = nullptr;
    void (*afterWrite)(Area area, uint16_t address, uint16_t count, void* context) = nullptr;
    void* context = nullptr;
};

class Map {
public:
    static const uint8_t MaxBlocks = 32;
    // false when full, empty, overlapping another block or out of the 16-bit range.
    bool addRegisters(Area area, uint16_t start, uint16_t count, uint16_t* regs);
    bool addBits(Area area, uint16_t start, uint16_t count, uint8_t* bits);
    // The block holding all of [address, address + count), or nullptr.
    Block* find(Area area, uint16_t address, uint16_t count);
    uint8_t size() const { return count_; }
    void clear() { count_ = 0; }
private:
    bool add(const Block& block);
    Block blocks_[MaxBlocks] = {};
    uint8_t count_ = 0;
};

// Largest PDU (function code + data) and TCP ADU.
const size_t MaxPdu = 253;
const size_t MaxTcpAdu = 260;
const size_t MaxRtuAdu = 256;

// Serves one request PDU. Returns the response PDU size (exceptions included), or 0 when
// the request is too short to have a function code. `response` needs MaxPdu bytes.
size_t serve(Map& map, const Hooks& hooks, const uint8_t* request, size_t size, uint8_t* response);

// ---- Modbus TCP ----
// Total ADU size from the first 6 header bytes, or 0 for an invalid header.
size_t tcpFrameSize(const uint8_t* header);
// Answers a complete ADU. unitFilter 0 answers every unit; otherwise only that unit and 255.
// Returns the response size, 0 to ignore (other unit) or -1 for an invalid frame (close).
int serveTcp(Map& map, const Hooks& hooks, uint8_t unitFilter, const uint8_t* adu, size_t size,
             uint8_t* response);

// ---- Modbus RTU (master side) ----
uint16_t crc16(const uint8_t* data, size_t size);
// Read (FC 1-4) or write-single (FC 5/6) request with CRC: always 8 bytes. 0 on a bad argument.
size_t rtuRequest(uint8_t unit, uint8_t function, uint16_t address, uint16_t value, uint8_t* out);

enum class RtuResult : uint8_t { Ok, Timeout, Crc, Frame, Exception, InvalidArgument, Transport };
const char* rtuResultText(RtuResult result);

// Checks a reply to rtuRequest(). For FC 1/2, `out` receives `count` bits (one per element);
// for FC 3/4 `count` registers. Write echoes (FC 5/6) need no output.
RtuResult rtuParse(const uint8_t* reply, size_t size, uint8_t unit, uint8_t function,
                   uint16_t address, uint16_t count, uint16_t* out, uint8_t& exception);

// ---- 32-bit values ----
// How a 32-bit value is laid out in two registers. A-D are its bytes, most significant first.
enum class WordOrder : uint8_t { ABCD, CDAB, BADC, DCBA };
WordOrder parseWordOrder(const char* text); // unknown text gives ABCD

void putU32(uint16_t* regs, uint32_t value, WordOrder order = WordOrder::ABCD);
uint32_t getU32(const uint16_t* regs, WordOrder order = WordOrder::ABCD);
void putFloat(uint16_t* regs, float value, WordOrder order = WordOrder::ABCD);
float getFloat(const uint16_t* regs, WordOrder order = WordOrder::ABCD);

// Data types a device register can hold. Bool is a coil or discrete input.
enum class DataType : uint8_t { Bool, Int16, UInt16, Int32, UInt32, Float32 };
DataType parseDataType(const char* text); // "int16", "uint32", "float32", "bool"; unknown gives Int16
uint8_t registerCount(DataType type);      // 1 or 2
float decode(const uint16_t* regs, DataType type, WordOrder order);

} // namespace irivmb
