#pragma once
// Modbus TCP server for Arduino-Pico over any lwIP interface (W5500 on the IRIV IOC).
// Uses WiFiServer/WiFiClient because Arduino-Pico's lwIP has no BSD socket layer.
// Call poll() often from the core that owns the network (core 0).
#include <Arduino.h>
#include <WiFi.h>
#include "IRIVModbusCore.h"

#ifndef IRIVMODBUS_TCP_CLIENTS
#define IRIVMODBUS_TCP_CLIENTS 4
#endif

class IRIVModbusTCPServer {
public:
    struct Stats {
        uint32_t requests = 0, exceptions = 0, ignored = 0, invalid = 0, timeouts = 0, rejected = 0;
    };
    uint32_t idleTimeoutMs = 60000;  // close a silent client; 0 keeps it
    uint32_t frameTimeoutMs = 5000;  // a started request must arrive in time

    explicit IRIVModbusTCPServer(irivmb::Map& map) : map_(map) {}
    void hooks(const irivmb::Hooks& hooks) { hooks_ = hooks; }
    // unitId 0 answers every unit; otherwise that unit and 255.
    void begin(uint16_t port = 502, uint8_t unitId = 0);
    void poll();
    uint8_t clientCount();
    const Stats& stats() const { return stats_; }

private:
    struct Slot {
        WiFiClient client;
        bool open = false;
        uint8_t rx[irivmb::MaxTcpAdu];
        size_t received = 0;
        uint32_t lastActivity = 0, frameStart = 0;
    };
    void service(Slot& slot, uint32_t now);
    void close(Slot& slot);

    irivmb::Map& map_;
    irivmb::Hooks hooks_;
    WiFiServer server_;
    Slot slots_[IRIVMODBUS_TCP_CLIENTS];
    uint8_t unit_ = 0;
    bool running_ = false;
    Stats stats_;
    uint8_t tx_[irivmb::MaxTcpAdu];
};
