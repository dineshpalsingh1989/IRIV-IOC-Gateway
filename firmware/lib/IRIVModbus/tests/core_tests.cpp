// Native tests for the portable Modbus core. Run: python tests/run_native.py
#include "../src/IRIVModbusCore.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace irivmb;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static size_t pdu(Map& map, const Hooks& hooks, const uint8_t* q, size_t n, uint8_t* r) { return serve(map, hooks, q, n, r); }

// Cytron-style sparse map used by most tests.
static uint8_t di[11], doBits[4], buzzer[1];
static uint16_t anv[2] = {5000, 7360}, counts[2], holding[3];
static Map build() {
    Map m;
    CHECK(m.addBits(Area::DiscreteInputs, 0x0000, 11, di));
    CHECK(m.addBits(Area::Coils, 0x0100, 4, doBits));
    CHECK(m.addBits(Area::Coils, 0x0500, 1, buzzer));
    CHECK(m.addRegisters(Area::InputRegisters, 0x0200, 2, anv));
    CHECK(m.addRegisters(Area::InputRegisters, 0x0400, 2, counts));
    CHECK(m.addRegisters(Area::HoldingRegisters, 10, 3, holding));
    return m;
}

static int writes = 0;
static void countWrite(Area, uint16_t, uint16_t, void*) { ++writes; }

static void testMap() {
    Map m;
    uint16_t r[4];
    uint8_t b[4];
    CHECK(m.addRegisters(Area::InputRegisters, 100, 4, r));
    CHECK(!m.addRegisters(Area::InputRegisters, 102, 4, r));  // overlaps
    CHECK(m.addRegisters(Area::HoldingRegisters, 102, 4, r)); // other table is fine
    CHECK(!m.addRegisters(Area::Coils, 0, 4, r));             // wrong kind
    CHECK(!m.addBits(Area::Coils, 0xFFFE, 4, b));             // past 65535
    CHECK(m.find(Area::InputRegisters, 101, 3) != nullptr);
    CHECK(m.find(Area::InputRegisters, 101, 4) == nullptr);   // runs off the block
}

static void testReads() {
    Map m = build();
    Hooks h;
    uint8_t r[MaxPdu];
    di[0] = 1; di[3] = 1; di[10] = 1;
    const uint8_t readDi[] = {2, 0x00, 0x00, 0x00, 11};
    CHECK(pdu(m, h, readDi, 5, r) == 4);
    CHECK(r[0] == 2 && r[1] == 2 && r[2] == 0x09 && r[3] == 0x04); // bits 0,3 then bit 10

    const uint8_t readAn[] = {4, 0x02, 0x00, 0x00, 2};
    CHECK(pdu(m, h, readAn, 5, r) == 6);
    CHECK(r[1] == 4 && r[2] == 0x13 && r[3] == 0x88 && r[4] == 0x1C && r[5] == 0xC0); // 5000, 7360

    const uint8_t gap[] = {4, 0x02, 0x00, 0x00, 3}; // 0x0202 is not mapped
    CHECK(pdu(m, h, gap, 5, r) == 2 && r[0] == 0x84 && r[1] == IllegalDataAddress);

    const uint8_t tooMany[] = {3, 0, 10, 0, 126};
    CHECK(pdu(m, h, tooMany, 5, r) == 2 && r[0] == 0x83 && r[1] == IllegalDataValue);

    const uint8_t unknown[] = {43, 14, 1, 0};
    CHECK(pdu(m, h, unknown, 4, r) == 2 && r[0] == 0xAB && r[1] == IllegalFunction);
}

static void testWrites() {
    Map m = build();
    Hooks h;
    h.afterWrite = countWrite;
    uint8_t r[MaxPdu];
    writes = 0;

    const uint8_t coilOn[] = {5, 0x01, 0x02, 0xFF, 0x00}; // DO2 on
    CHECK(pdu(m, h, coilOn, 5, r) == 5 && !memcmp(r, coilOn, 5));
    CHECK(doBits[2] == 1 && writes == 1);

    const uint8_t badCoil[] = {5, 0x01, 0x02, 0x12, 0x34};
    CHECK(pdu(m, h, badCoil, 5, r) == 2 && r[1] == IllegalDataValue);

    const uint8_t multi[] = {15, 0x01, 0x00, 0x00, 4, 1, 0x09}; // DO0..3 = 1,0,0,1
    CHECK(pdu(m, h, multi, 7, r) == 5);
    CHECK(doBits[0] == 1 && doBits[1] == 0 && doBits[2] == 0 && doBits[3] == 1);

    const uint8_t badCount[] = {15, 0x01, 0x00, 0x00, 4, 2, 0x09, 0x00}; // byte count wrong
    CHECK(pdu(m, h, badCount, 8, r) == 2 && r[1] == IllegalDataValue);

    const uint8_t regs[] = {16, 0, 10, 0, 2, 4, 0x12, 0x34, 0xAB, 0xCD};
    CHECK(pdu(m, h, regs, 10, r) == 5 && holding[0] == 0x1234 && holding[1] == 0xABCD);

    const uint8_t readOnly[] = {6, 0x02, 0x00, 0x00, 0x01}; // input registers cannot be written
    CHECK(pdu(m, h, readOnly, 5, r) == 2 && r[1] == IllegalDataAddress);
    CHECK(writes == 3);
}

static void testTcp() {
    Map m = build();
    Hooks h;
    uint8_t r[MaxTcpAdu];
    const uint8_t adu[] = {0x00, 0x07, 0x00, 0x00, 0x00, 0x06, 0xFF, 4, 0x02, 0x00, 0x00, 0x01};
    CHECK(tcpFrameSize(adu) == sizeof(adu));
    const int n = serveTcp(m, h, 255, adu, sizeof(adu), r);
    CHECK(n == 11);
    CHECK(r[0] == 0x00 && r[1] == 0x07 && r[5] == 5 && r[6] == 0xFF && r[7] == 4 && r[8] == 2);

    uint8_t other[sizeof(adu)];
    memcpy(other, adu, sizeof(adu));
    other[6] = 7;
    CHECK(serveTcp(m, h, 1, other, sizeof(other), r) == 0); // unit 7 is not ours
    CHECK(serveTcp(m, h, 0, other, sizeof(other), r) > 0);  // 0 answers every unit

    uint8_t badProtocol[sizeof(adu)];
    memcpy(badProtocol, adu, sizeof(adu));
    badProtocol[3] = 1;
    CHECK(tcpFrameSize(badProtocol) == 0);
    CHECK(serveTcp(m, h, 0, badProtocol, sizeof(badProtocol), r) == -1);
}

static void testRtu() {
    // Reference frame from the Modbus spec examples: read 2 holding registers at 0 from unit 1.
    uint8_t q[8];
    CHECK(rtuRequest(1, 3, 0, 2, q) == 8);
    CHECK(q[6] == 0xC4 && q[7] == 0x0B); // CRC 0x0BC4, low byte first
    CHECK(rtuRequest(0, 3, 0, 1, q) == 0); // unit 0 is broadcast, not polled
    CHECK(rtuRequest(1, 3, 0, 126, q) == 0);

    uint8_t reply[] = {1, 3, 4, 0x00, 0x0A, 0xFF, 0xF6, 0, 0};
    uint16_t crc = crc16(reply, 7);
    reply[7] = uint8_t(crc); reply[8] = uint8_t(crc >> 8);
    uint16_t out[2] = {};
    uint8_t ex = 0;
    CHECK(rtuParse(reply, 9, 1, 3, 0, 2, out, ex) == RtuResult::Ok);
    CHECK(out[0] == 10 && int16_t(out[1]) == -10);

    reply[3] ^= 1; // corrupt
    CHECK(rtuParse(reply, 9, 1, 3, 0, 2, out, ex) == RtuResult::Crc);

    uint8_t exc[] = {1, 0x83, 2, 0, 0};
    crc = crc16(exc, 3);
    exc[3] = uint8_t(crc); exc[4] = uint8_t(crc >> 8);
    CHECK(rtuParse(exc, 5, 1, 3, 0, 2, out, ex) == RtuResult::Exception && ex == 2);
    CHECK(rtuParse(exc, 0, 1, 3, 0, 2, out, ex) == RtuResult::Timeout);

    uint8_t bits[] = {7, 1, 1, 0x05, 0, 0}; // coils: 1,0,1
    crc = crc16(bits, 4);
    bits[4] = uint8_t(crc); bits[5] = uint8_t(crc >> 8);
    uint16_t b[3];
    CHECK(rtuParse(bits, 6, 7, 1, 4, 3, b, ex) == RtuResult::Ok && b[0] == 1 && b[1] == 0 && b[2] == 1);

    uint8_t echo[8];
    CHECK(rtuRequest(7, 5, 3, 1, echo) == 8 && echo[4] == 0xFF && echo[5] == 0x00);
    CHECK(rtuParse(echo, 8, 7, 5, 3, 1, nullptr, ex) == RtuResult::Ok);
}

static void testValues() {
    uint16_t r[2];
    // 123.5f is 0x42F70000: A=42 B=F7 C=00 D=00
    const struct { WordOrder order; uint16_t w0, w1; } cases[] = {
        {WordOrder::ABCD, 0x42F7, 0x0000}, {WordOrder::CDAB, 0x0000, 0x42F7},
        {WordOrder::BADC, 0xF742, 0x0000}, {WordOrder::DCBA, 0x0000, 0xF742}};
    for (const auto& c : cases) {
        putFloat(r, 123.5f, c.order);
        CHECK(r[0] == c.w0 && r[1] == c.w1);
        CHECK(getFloat(r, c.order) == 123.5f);
        CHECK(decode(r, DataType::Float32, c.order) == 123.5f);
    }
    const uint16_t neg[2] = {0xFFFF, 0xFFFE};
    CHECK(decode(neg, DataType::Int32, WordOrder::ABCD) == -2.0f);
    CHECK(decode(neg, DataType::Int16, WordOrder::ABCD) == -1.0f);
    CHECK(decode(neg, DataType::UInt16, WordOrder::ABCD) == 65535.0f);
    putFloat(r, NAN);
    CHECK(isnan(getFloat(r)));
    CHECK(parseDataType("float32") == DataType::Float32 && registerCount(DataType::Float32) == 2);
    CHECK(parseWordOrder("CDAB") == WordOrder::CDAB && parseWordOrder("junk") == WordOrder::ABCD);
}

int main() {
    testMap();
    testReads();
    testWrites();
    testTcp();
    testRtu();
    testValues();
    printf(failures ? "%d check(s) failed\n" : "IRIVModbus core: all checks passed\n", failures);
    return failures ? 1 : 0;
}
