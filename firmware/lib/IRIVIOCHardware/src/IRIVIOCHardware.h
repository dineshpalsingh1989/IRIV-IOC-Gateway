#pragma once
#include <Arduino.h>
#include <W5500lwIP.h>

namespace irivioc {
// Static address set. useStatic false means DHCP. dns 0.0.0.0 means "use the gateway".
struct Address {
    bool useStatic = false;
    IPAddress ip, mask, gateway, dns;
};

// Strict dotted IPv4 ("192.168.1.20"). Empty text gives 0.0.0.0 and is accepted only when allowEmpty.
inline bool parseIPv4(const char* text, IPAddress& out, bool allowEmpty = false) {
    if (!text || !*text) { out = IPAddress(0, 0, 0, 0); return allowEmpty; }
    uint8_t parts[4]; int part = 0; uint16_t value = 0; int digits = 0;
    for (const char* p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            if (++digits > 3 || value > 255) return false;
        } else if ((*p == '.' || !*p) && digits && part < 4) {
            parts[part++] = uint8_t(value); value = 0; digits = 0;
            if (!*p) break;
        } else {
            return false;
        }
    }
    if (part != 4) return false;
    out = IPAddress(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

// Caller supplies board wiring. Does not change pin assignments, reset the chip,
// mount/format storage, or silently switch to a static IP.
class Ethernet {
public:
    Ethernet(int cs, SPIClass& spi, int interruptPin = -1)
        : driver_(cs, spi, interruptPin) {}
    Ethernet(const Ethernet&) = delete;
    Ethernet& operator=(const Ethernet&) = delete;
    bool begin(const char* hostname) {
        driver_.setHostname(hostname);
        return driver_.begin();
    }
    // DHCP or a fixed address. A static address must be set before the interface starts.
    bool begin(const char* hostname, const Address& address) {
        driver_.setHostname(hostname);
        if (address.useStatic) {
            const IPAddress dns = uint32_t(address.dns) ? address.dns : address.gateway;
            // ESP order (ip, gateway, mask, dns); the core also accepts the Arduino order.
            if (!driver_.config(address.ip, address.gateway, address.mask, dns)) return false;
        }
        return driver_.begin();
    }
    bool connected() { return driver_.connected(); }
    bool linkUp() { return driver_.linkStatus() != LinkOFF; }
    IPAddress localIP() { return driver_.localIP(); }
    Wiznet5500lwIP& driver() { return driver_; }
private:
    Wiznet5500lwIP driver_;
};
}
