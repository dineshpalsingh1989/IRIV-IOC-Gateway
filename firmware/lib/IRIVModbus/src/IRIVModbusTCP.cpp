#include "IRIVModbusTCP.h"

void IRIVModbusTCPServer::begin(uint16_t port, uint8_t unitId) {
    unit_ = unitId;
    server_.begin(port);
    server_.setNoDelay(true); // replies leave immediately
    running_ = true;
}

void IRIVModbusTCPServer::close(Slot& slot) {
    if (slot.open) slot.client.stop();
    slot.open = false;
    slot.received = 0;
}

uint8_t IRIVModbusTCPServer::clientCount() {
    uint8_t n = 0;
    for (Slot& s : slots_) n += s.open;
    return n;
}

void IRIVModbusTCPServer::poll() {
    if (!running_) return;
    const uint32_t now = millis();
    WiFiClient incoming = server_.accept();
    if (incoming) {
        Slot* free = nullptr;
        for (Slot& s : slots_) if (!s.open) { free = &s; break; }
        if (free) {
            free->client = incoming;
            free->client.setNoDelay(true);
            free->open = true;
            free->received = 0;
            free->lastActivity = now;
        } else {
            ++stats_.rejected;
            incoming.stop();
        }
    }
    for (Slot& s : slots_) if (s.open) service(s, now);
}

void IRIVModbusTCPServer::service(Slot& s, uint32_t now) {
    if (!s.client.connected() && !s.client.available()) { close(s); return; }
    if ((idleTimeoutMs && now - s.lastActivity >= idleTimeoutMs) ||
        (s.received && now - s.frameStart >= frameTimeoutMs)) {
        ++stats_.timeouts;
        close(s);
        return;
    }
    // One request per call keeps the other clients and the main loop responsive.
    while (s.client.available()) {
        const int byte = s.client.read();
        if (byte < 0) break;
        if (!s.received) s.frameStart = now;
        s.lastActivity = now;
        s.rx[s.received++] = uint8_t(byte);
        if (s.received < 6) continue;
        const size_t total = irivmb::tcpFrameSize(s.rx);
        if (!total) { ++stats_.invalid; close(s); return; }
        if (s.received < total) continue;
        const int size = irivmb::serveTcp(map_, hooks_, unit_, s.rx, total, tx_);
        s.received = 0;
        if (size < 0) { ++stats_.invalid; close(s); return; }
        if (size == 0) { ++stats_.ignored; return; }
        ++stats_.requests;
        if (tx_[7] & 0x80) ++stats_.exceptions;
        if (s.client.write(tx_, size_t(size)) != size_t(size)) close(s);
        return;
    }
}
