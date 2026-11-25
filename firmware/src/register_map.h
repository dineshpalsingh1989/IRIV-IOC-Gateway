#pragma once
// Modbus TCP address map. Keep in sync with `MB` in ../../app.js (the dashboard).
// Library-compatible addresses match Cytron's iriv_ioc_modbus.py v2.0.0, so existing SCADA tags work.
#include <stdint.h>

namespace regmap {
// Cytron-compatible
const uint16_t DI = 0x0000;        // 1x discrete inputs DI0..DI10
const uint16_t DO = 0x0100;        // 0x coils DO0..DO3
const uint16_t ANV = 0x0200;       // 3x AN0/AN1 terminal voltage, mV
const uint16_t ANA = 0x0210;       // 3x AN0/AN1 terminal current, uA
const uint16_t COUNT_EN = 0x0300;  // 0x counter enable DI1/3/5/7/9
const uint16_t COUNT_RST = 0x0310; // 0x counter reset (clears itself)
const uint16_t COUNT = 0x0400;     // 3x 32-bit counts, high word first
const uint16_t MODEL = 0x0F00;     // 3x "IO", "C"
const uint16_t VERSION = 0x0F10;   // 3x firmware major, minor, patch
// Gateway extensions
const uint16_t AN_SCALED = 0x0220; // 3x AN0/AN1 scaled value, float32 (2 registers each)
const uint16_t BUZZER = 0x0500;    // 0x buzzer remote command
const uint16_t RS485 = 0x1000;     // 3x RS485 points, float32 (2 registers each)
const uint16_t RS485_REGS = 128;   // room for 64 points
}

#define GATEWAY_VERSION_MAJOR 0
#define GATEWAY_VERSION_MINOR 2
#define GATEWAY_VERSION_PATCH 0
#define GATEWAY_VERSION_TEXT "0.2.0" // keep equal to the three numbers above (read by tools/build_site.py)
