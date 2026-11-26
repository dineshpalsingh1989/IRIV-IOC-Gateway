#pragma once
// Gateway settings, loaded from /ioc_config.json (the file the dashboard exports).
#include <Arduino.h>
#include <IRIVIOCHardware.h>
#include <IRIVIOCLogic.h>
#include <IRIVModbusCore.h>

struct PointConfig {
    char id[8] = "";
    uint8_t slave = 1;
    uint8_t function = 3;      // 1 coil, 2 discrete input, 3 holding, 4 input register
    uint16_t reg = 0;
    irivmb::DataType type = irivmb::DataType::Int16;
    float scale = 1;
    uint16_t tcp = 0;          // first Modbus TCP input register (float32)
};

struct TopicConfig {
    char source[8] = "";       // DI0..DI10, DO0..DO3, AN0, AN1, BZ, P1..
    char publish[129] = "";
    char subscribe[129] = "";  // only outputs and the buzzer
};

struct GatewayConfig {
    static const uint8_t MaxPoints = 64, MaxTopics = 82;

    irivioc::Address network;

    bool modbusEnabled = true;
    uint16_t modbusPort = 502;
    uint8_t modbusUnit = 255;  // Cytron's firmware answers unit 255
    irivmb::WordOrder modbusOrder = irivmb::WordOrder::ABCD;

    bool mqttEnabled = false;
    char mqttHost[65] = "";
    uint16_t mqttPort = 1883;
    bool mqttTls = false;
    char mqttCa[3072] = ""; // PEM CA certificate(s) to check the broker; empty = encrypted but not checked
    char mqttClient[65] = "";
    char mqttUser[65] = "";
    char mqttPass[65] = "";
    bool mqttRetain = true;
    uint8_t mqttSubscribeQos = 0;
    uint32_t mqttIntervalMs = 5000;
    char webPassword[65] = ""; // controller web page (user admin); empty = the page is read-only

    bool rs485Enabled = false;
    uint32_t rs485Baud = 9600;
    uint16_t rs485Serial = SERIAL_8N1;
    uint32_t rs485PollMs = 1000, rs485TimeoutMs = 500;
    irivmb::WordOrder rs485Order = irivmb::WordOrder::ABCD;
    PointConfig points[MaxPoints];
    uint8_t pointCount = 0;

    iriv::AnalogMode analogMode[2] = {iriv::AnalogMode::Volt0To10, iriv::AnalogMode::Volt0To10};
    float analogLow[2] = {0, 0}, analogHigh[2] = {10, 10};
    bool counter[11] = {};
    iriv::BuzzerRule buzzer;

    TopicConfig topics[MaxTopics];
    uint8_t topicCount = 0;
};

// Fills `out` (pass a default-constructed config) from the JSON file. Missing values keep their
// defaults. Returns false (and an error text) when the file is missing or unreadable; `out` then
// holds the defaults. A true return may still carry a warning in `error` (e.g. a skipped point).
bool loadConfig(const char* path, GatewayConfig& out, String& error);
// SERIAL_8N1 ... from the dashboard's parity ("None", "Even", "Odd") and stop bits.
uint16_t serialConfig(const char* parity, int stopBits);
