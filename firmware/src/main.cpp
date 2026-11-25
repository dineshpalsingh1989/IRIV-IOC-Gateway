// IRIV IO Controller LAN gateway.
// Core 0: Ethernet, I/O, Modbus TCP server, MQTT. Core 1: RS485 Modbus RTU master polling.
// Settings come from /ioc_config.json in LittleFS, exported by the dashboard.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>
#include <SPI.h>
#include <WiFi.h>
#include <pico/mutex.h>
#include <pico/unique_id.h>
#include <hardware/structs/sysinfo.h>

#include <IRIVIOCHardware.h>
#include <IRIVIOCIo.h>
#include <IRIVModbus.h>
#include <WilcoxonMqtt.h>

#include "config.h"
#include "register_map.h"

using irivmb::Area;

static GatewayConfig cfg;  // global: about 25 KB, too big for a stack
static IRIVIOCIo io;
static irivioc::Ethernet ethernet(PIN_W5500_CS, SPI, PIN_W5500_INT);
static char hostname[32];
static volatile bool ready = false; // core 1 waits for setup() to finish

// ---- Modbus TCP map storage ----
static uint8_t diBits[11], doBits[4], buzzerBit[1], counterEnable[5], counterReset[5];
static uint16_t anMillivolts[2], anMicroamps[2], anScaled[4], counterRegs[10];
static uint16_t modelRegs[2] = {0x494F, 0x4300}; // "IO" "C", as Cytron's firmware
static uint16_t versionRegs[3] = {GATEWAY_VERSION_MAJOR, GATEWAY_VERSION_MINOR, GATEWAY_VERSION_PATCH};
static uint16_t pointRegs[regmap::RS485_REGS];
static irivmb::Map modbusMap;
static IRIVModbusTCPServer modbus(modbusMap);

// ---- RS485 points, written by core 1 ----
struct PointValue { float value = NAN; uint32_t errors = 0; uint8_t failedInRow = 0; };
static PointValue pointValues[GatewayConfig::MaxPoints];
auto_init_mutex(pointLock);

// ---- Live state on core 0 ----
static float analogValue[2] = {NAN, NAN};
static iriv::AnalogFault analogFault[2];
static bool buzzerSounding = false;
static iriv::BuzzerState buzzerState;

// ---- MQTT ----
static WiFiClient mqttNet;
static WilcoxonMqttClient mqtt(mqttNet);
static char published[GatewayConfig::MaxTopics][16]; // last payload sent per topic

static void log(const char* text) {
    Serial.printf("[%10lu] %s\n", millis(), text);
}

// ---------------------------------------------------------------------------------------------
// Modbus writes from SCADA/PLC: outputs, buzzer, counters.

static void applyCounterCoils() {
    for (uint8_t k = 0; k < 5; ++k) {
        const uint8_t input = IRIVIOCIo::CounterInputs[k];
        io.setCounter(input, counterEnable[k]);
        if (counterReset[k]) { io.resetCounter(input); counterReset[k] = 0; } // clears itself
    }
}

static void afterModbusWrite(Area area, uint16_t, uint16_t, void*) {
    if (area != Area::Coils) return;
    for (uint8_t i = 0; i < 4; ++i) io.setOutput(i, doBits[i]);
    applyCounterCoils();
}

static void buildModbusMap() {
    modbusMap.addBits(Area::DiscreteInputs, regmap::DI, 11, diBits);
    modbusMap.addBits(Area::Coils, regmap::DO, 4, doBits);
    modbusMap.addBits(Area::Coils, regmap::COUNT_EN, 5, counterEnable);
    modbusMap.addBits(Area::Coils, regmap::COUNT_RST, 5, counterReset);
    modbusMap.addBits(Area::Coils, regmap::BUZZER, 1, buzzerBit);
    modbusMap.addRegisters(Area::InputRegisters, regmap::ANV, 2, anMillivolts);
    modbusMap.addRegisters(Area::InputRegisters, regmap::ANA, 2, anMicroamps);
    modbusMap.addRegisters(Area::InputRegisters, regmap::AN_SCALED, 4, anScaled);
    modbusMap.addRegisters(Area::InputRegisters, regmap::COUNT, 10, counterRegs);
    modbusMap.addRegisters(Area::InputRegisters, regmap::MODEL, 2, modelRegs);
    modbusMap.addRegisters(Area::InputRegisters, regmap::VERSION, 3, versionRegs);
    modbusMap.addRegisters(Area::InputRegisters, regmap::RS485, regmap::RS485_REGS, pointRegs);
    irivmb::Hooks hooks;
    hooks.afterWrite = afterModbusWrite;
    modbus.hooks(hooks);
    for (uint16_t i = 0; i < regmap::RS485_REGS; i += 2) irivmb::putFloat(pointRegs + i, NAN, cfg.modbusOrder);
}

// ---------------------------------------------------------------------------------------------
// I/O sampling and rules (every loop).

static void updateIo(uint32_t now) {
    io.update(now);
    for (uint8_t i = 0; i < 11; ++i) diBits[i] = io.input(i);
    for (uint8_t i = 0; i < 2; ++i) {
        const float volts = io.terminalVolts(i);
        anMillivolts[i] = iriv::clampU16(volts * 1000.0f);
        anMicroamps[i] = iriv::clampU16(volts / iriv::CurrentShuntOhm * 1e6f);
        const float signal = iriv::analogSignal(cfg.analogMode[i], io.adc(i), io.adcReference);
        analogFault[i] = iriv::analogFault(cfg.analogMode[i], signal);
        analogValue[i] = analogFault[i] == iriv::AnalogFault::None
            ? iriv::scaleSignal(cfg.analogMode[i], signal, cfg.analogLow[i], cfg.analogHigh[i]) : NAN;
        irivmb::putFloat(anScaled + 2 * i, analogValue[i], cfg.modbusOrder);
    }
    for (uint8_t k = 0; k < 5; ++k) iriv::counterWords(io.count(IRIVIOCIo::CounterInputs[k]), counterRegs + 2 * k);

    bool toneOn = false;
    const bool wanted = iriv::buzzerWanted(cfg.buzzer, io.inputs(), analogValue, analogFault, buzzerBit[0]);
    buzzerSounding = buzzerState.update(wanted, now, cfg.buzzer, toneOn);
    io.buzzer(toneOn, cfg.buzzer.frequency);

    mutex_enter_blocking(&pointLock);
    for (uint8_t p = 0; p < cfg.pointCount; ++p)
        irivmb::putFloat(pointRegs + (cfg.points[p].tcp - regmap::RS485), pointValues[p].value, cfg.modbusOrder);
    mutex_exit(&pointLock);
}

// ---------------------------------------------------------------------------------------------
// MQTT: states publish on change, measurements on the interval.

static void formatFloat(char* out, size_t size, float v) {
    if (isnan(v)) strlcpy(out, "NaN", size);
    else snprintf(out, size, "%.4g", double(v));
}

// Payload for a source id; `measurement` true means publish on the interval, false on change.
static bool payloadFor(const char* source, char* out, size_t size, bool& measurement) {
    measurement = false;
    if (!strncmp(source, "DI", 2)) {
        const int i = atoi(source + 2);
        if (i < 0 || i > 10) return false;
        if (io.counterEnabled(i)) { measurement = true; snprintf(out, size, "%lu", (unsigned long)io.count(i)); }
        else strlcpy(out, io.input(i) ? "1" : "0", size);
        return true;
    }
    if (!strncmp(source, "DO", 2)) {
        const int i = atoi(source + 2);
        if (i < 0 || i > 3) return false;
        strlcpy(out, io.output(i) ? "1" : "0", size);
        return true;
    }
    if (!strcmp(source, "AN0") || !strcmp(source, "AN1")) {
        measurement = true;
        formatFloat(out, size, analogValue[source[2] - '0']);
        return true;
    }
    if (!strcmp(source, "BZ")) { strlcpy(out, buzzerSounding ? "1" : "0", size); return true; }
    for (uint8_t p = 0; p < cfg.pointCount; ++p) {
        if (strcmp(source, cfg.points[p].id)) continue;
        measurement = true;
        mutex_enter_blocking(&pointLock);
        const float v = pointValues[p].value;
        mutex_exit(&pointLock);
        formatFloat(out, size, v);
        return true;
    }
    return false;
}

static int parseSwitch(const uint8_t* payload, unsigned int length) {
    char text[8] = "";
    if (length >= sizeof(text)) return -1;
    for (unsigned int i = 0; i < length; ++i) text[i] = char(tolower(payload[i]));
    if (!strcmp(text, "1") || !strcmp(text, "on") || !strcmp(text, "true")) return 1;
    if (!strcmp(text, "0") || !strcmp(text, "off") || !strcmp(text, "false")) return 0;
    return -1;
}

static void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    const int value = parseSwitch(payload, length);
    if (value < 0) return; // ignore anything that is not an on/off command
    for (uint8_t t = 0; t < cfg.topicCount; ++t) {
        const TopicConfig& tc = cfg.topics[t];
        if (!tc.subscribe[0] || strcmp(topic, tc.subscribe)) continue;
        if (!strncmp(tc.source, "DO", 2)) {
            const int i = atoi(tc.source + 2);
            if (i >= 0 && i < 4) { doBits[i] = value; io.setOutput(i, value); }
        } else if (!strcmp(tc.source, "BZ")) {
            buzzerBit[0] = value;
        }
    }
}

static void serviceMqtt(uint32_t now) {
    static uint32_t nextConnect = 0, nextPeriodic = 0;
    if (!cfg.mqttEnabled || !cfg.mqttHost[0] || !ethernet.connected()) return;
    if (!mqtt.connected()) {
        if (int32_t(now - nextConnect) < 0) return;
        nextConnect = now + 10000;
        const char* id = cfg.mqttClient[0] ? cfg.mqttClient : hostname;
        const bool ok = cfg.mqttUser[0] ? mqtt.connect(id, cfg.mqttUser, cfg.mqttPass) : mqtt.connect(id);
        if (!ok) { Serial.printf("MQTT connect failed, state %d\n", mqtt.state()); return; }
        log("MQTT connected");
        for (uint8_t t = 0; t < cfg.topicCount; ++t)
            if (cfg.topics[t].subscribe[0]) mqtt.subscribe(cfg.topics[t].subscribe, cfg.mqttSubscribeQos);
        memset(published, 0, sizeof(published)); // republish everything after a reconnect
    }
    mqtt.loop();
    const bool periodic = int32_t(now - nextPeriodic) >= 0;
    if (periodic) nextPeriodic = now + cfg.mqttIntervalMs;
    for (uint8_t t = 0; t < cfg.topicCount; ++t) {
        char text[16];
        bool measurement;
        if (!payloadFor(cfg.topics[t].source, text, sizeof(text), measurement)) continue;
        const bool due = measurement ? (periodic || !published[t][0]) : strcmp(text, published[t]) != 0;
        if (!due) continue;
        if (mqtt.publish(cfg.topics[t].publish, text, cfg.mqttRetain)) strlcpy(published[t], text, sizeof(published[t]));
    }
}

// ---------------------------------------------------------------------------------------------
// USB setup protocol, used by the browser installer over Web Serial.
// In:  HELLO | STATUS | CONFIG <bytes> (then exactly that many bytes of JSON) | REBOOT | BOOTLOADER
// Out: one line per reply, "@IOC " + JSON, so replies stand apart from log messages.

static const char* ConfigPath = "/ioc_config.json";
static const char* UploadPath = "/ioc_config.tmp";

static void replyJson(JsonDocument& doc) {
    Serial.print("@IOC ");
    serializeJson(doc, Serial);
    Serial.println();
}

static void replyError(const char* message) {
    StaticJsonDocument<256> d;
    d["type"] = "error";
    d["message"] = message;
    replyJson(d);
}

// The chip this firmware is running on, read from the silicon (SYSINFO), so the installer can
// check it against the firmware build. The IRIV IOC uses the RP2350A (QFN60 package).
static void addChipInfo(JsonDocument& d) {
#if defined(PICO_RP2350)
    d["chip"] = (sysinfo_hw->package_sel & SYSINFO_PACKAGE_SEL_BITS) ? "RP2350A" : "RP2350B"; // 1 = QFN60
    d["chipRevision"] = sysinfo_hw->chip_id >> 28;
#else
    d["chip"] = "RP2040";
#endif
    d["builtFor"] = "RP2350A";
    d["board"] = "cytron_iriv_io_controller";
    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
    d["uid"] = id;
}

static void sendHello() {
    StaticJsonDocument<512> d;
    d["type"] = "hello";
    d["product"] = "iriv-ioc-gateway";
    d["version"] = GATEWAY_VERSION_TEXT;
    addChipInfo(d);
    d["host"] = hostname;
    d["ip"] = ethernet.localIP().toString();
    d["config"] = LittleFS.exists(ConfigPath);
    replyJson(d);
}

static void sendStatus() {
    DynamicJsonDocument d(6144);
    d["type"] = "status";
    d["version"] = GATEWAY_VERSION_TEXT;
    d["host"] = hostname;
    d["ip"] = ethernet.localIP().toString();
    d["link"] = ethernet.linkUp();
    d["online"] = ethernet.connected();
    d["dhcp"] = !cfg.network.useStatic;
    d["uptime"] = millis() / 1000;
    JsonObject mb = d.createNestedObject("modbus");
    mb["enabled"] = cfg.modbusEnabled;
    mb["port"] = cfg.modbusPort;
    mb["clients"] = modbus.clientCount();
    mb["requests"] = modbus.stats().requests;
    JsonObject mq = d.createNestedObject("mqtt");
    mq["enabled"] = cfg.mqttEnabled;
    mq["host"] = cfg.mqttHost;
    mq["connected"] = mqtt.connected();
    mq["state"] = mqtt.state();
    JsonArray points = d.createNestedArray("rs485");
    mutex_enter_blocking(&pointLock);
    for (uint8_t p = 0; p < cfg.pointCount; ++p) {
        JsonObject o = points.createNestedObject();
        o["id"] = cfg.points[p].id;
        if (isnan(pointValues[p].value)) o["value"] = nullptr; // JSON has no NaN
        else o["value"] = pointValues[p].value;
        o["errors"] = pointValues[p].errors;
    }
    mutex_exit(&pointLock);
    replyJson(d);
}

// Streams the upload into a temporary file, checks it with the real loader, then swaps it in
// and restarts. The working settings stay untouched until the new file has passed the check.
static void receiveConfig(long size) {
    if (size <= 0 || size > 65536) { replyError("size must be 1 to 65536 bytes"); return; }
    File file = LittleFS.open(UploadPath, "w");
    if (!file) { replyError("cannot write to flash"); return; }
    uint8_t buffer[256];
    long received = 0;
    uint32_t lastByte = millis();
    while (received < size && millis() - lastByte < 5000) {
        rp2040.wdt_reset();
        const int available = Serial.available();
        if (available <= 0) { delay(1); continue; }
        const size_t want = size_t(min(long(sizeof(buffer)), min(long(available), size - received)));
        const size_t got = Serial.readBytes(reinterpret_cast<char*>(buffer), want);
        if (file.write(buffer, got) != got) break;
        received += long(got);
        lastByte = millis();
    }
    file.close();
    if (received != size) { LittleFS.remove(UploadPath); replyError("upload incomplete or flash full"); return; }

    std::unique_ptr<GatewayConfig> check(new GatewayConfig()); // ~25 KB: heap, not stack
    String warning;
    if (!loadConfig(UploadPath, *check, warning)) {
        LittleFS.remove(UploadPath);
        replyError(warning.c_str());
        return;
    }
    if (!LittleFS.rename(UploadPath, ConfigPath)) { replyError("cannot save settings"); return; } // replaces the old file
    StaticJsonDocument<384> d;
    d["type"] = "saved";
    d["bytes"] = size;
    if (warning.length()) d["warning"] = warning;
    replyJson(d);
    Serial.flush();
    delay(300);
    rp2040.reboot(); // start again with the new settings
}

static void serviceUsbCommands() {
    static char line[40];
    static size_t used = 0;
    while (Serial.available()) {
        const int c = Serial.read();
        if (c < 0 || c == '\r') continue;
        if (c != '\n') {
            if (used < sizeof(line) - 1) line[used++] = char(c);
            continue;
        }
        line[used] = 0;
        used = 0;
        if (!strcmp(line, "HELLO")) sendHello();
        else if (!strcmp(line, "STATUS")) sendStatus();
        else if (!strncmp(line, "CONFIG ", 7)) receiveConfig(atol(line + 7));
        else if (!strcmp(line, "REBOOT")) { Serial.flush(); delay(100); rp2040.reboot(); }
        else if (!strcmp(line, "BOOTLOADER")) { Serial.flush(); delay(100); rp2040.rebootToBootloader(); }
        else if (line[0]) replyError("unknown command");
    }
}

// ---------------------------------------------------------------------------------------------
// Core 0

void setup() {
    Serial.begin(115200);
    io.begin();
    io.led(true);

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
    snprintf(hostname, sizeof(hostname), "IRIV-IOC_%s", id + strlen(id) - 6); // same style as Cytron's firmware

    String error;
    if (!LittleFS.begin()) error = "LittleFS mount failed, using defaults"; // never formats
    else loadConfig(ConfigPath, cfg, error);
    if (error.length()) log(error.c_str());

    // Hold USER at power-up for a fixed address, for a laptop plugged in directly.
    if (io.button()) {
        cfg.network.useStatic = true;
        cfg.network.ip = IPAddress(192, 168, 4, 1);
        cfg.network.mask = IPAddress(255, 255, 255, 0);
        cfg.network.gateway = cfg.network.dns = IPAddress(0, 0, 0, 0);
        log("USER held: using 192.168.4.1");
    }

    for (uint8_t i = 0; i < 11; ++i) if (cfg.counter[i] && io.setCounter(i, true)) {
        for (uint8_t k = 0; k < 5; ++k) if (IRIVIOCIo::CounterInputs[k] == i) counterEnable[k] = 1;
    }
    buildModbusMap();

    pinMode(PIN_W5500_RST, OUTPUT);
    digitalWrite(PIN_W5500_RST, LOW);
    delay(10);
    digitalWrite(PIN_W5500_RST, HIGH);
    delay(100);
    SPI.setRX(PIN_W5500_MISO);
    SPI.setTX(PIN_W5500_MOSI);
    SPI.setSCK(PIN_W5500_SCK);
    if (!ethernet.begin(hostname, cfg.network)) log("Ethernet start failed");

    if (cfg.modbusEnabled) modbus.begin(cfg.modbusPort, cfg.modbusUnit);
    mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
    mqtt.setBufferSize(512);
    mqtt.setSocketTimeout(2); // connect() blocks at most this long
    mqtt.setKeepAlive(30);
    mqtt.setCallback(onMqttMessage);

    rp2040.wdt_begin(8000); // restart if the loop stops for 8 s
    ready = true;
    Serial.printf("%s ready: Modbus TCP %s, MQTT %s, RS485 %u points\n", hostname,
                  cfg.modbusEnabled ? "on" : "off", cfg.mqttEnabled ? "on" : "off", cfg.pointCount);
}

void loop() {
    static uint32_t nextBlink = 0, nextIpReport = 0;
    static bool hadIp = false;
    const uint32_t now = millis();
    rp2040.wdt_reset();
    updateIo(now);
    modbus.poll();
    serviceMqtt(now);
    serviceUsbCommands();
    if (int32_t(now - nextBlink) >= 0) { // blinking USR LED = loop alive, as on Cytron's firmware
        nextBlink = now + 500;
        static bool led = false;
        io.led(led = !led);
    }
    const bool hasIp = ethernet.connected();
    if (hasIp != hadIp && int32_t(now - nextIpReport) >= 0) {
        hadIp = hasIp;
        nextIpReport = now + 1000;
        Serial.printf("Ethernet %s %s\n", hasIp ? "up" : "down", ethernet.localIP().toString().c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// Core 1: RS485 polling. RTU calls block for up to the timeout, so they stay off core 0.

static IRIVModbusRTUMaster rtu(Serial2);

void setup1() {
    while (!ready) delay(10);
    if (!cfg.rs485Enabled || !cfg.pointCount) return;
    Serial2.setTX(PIN_SERIAL2_TX);
    Serial2.setRX(PIN_SERIAL2_RX);
    rtu.begin(cfg.rs485Baud, cfg.rs485Serial, cfg.rs485TimeoutMs);
}

void loop1() {
    static uint8_t next = 0;
    static uint32_t due = 0;
    if (!ready || !cfg.rs485Enabled || !cfg.pointCount) { delay(100); return; }
    const uint32_t now = millis();
    if (int32_t(now - due) < 0) { delay(1); return; }
    due = now + cfg.rs485PollMs / cfg.pointCount; // each point is read once per poll interval

    const PointConfig& p = cfg.points[next];
    uint16_t regs[2] = {0, 0};
    const uint8_t count = irivmb::registerCount(p.type);
    const irivmb::RtuResult result = p.function <= 2
        ? rtu.readBits(p.slave, p.function, p.reg, 1, regs)
        : rtu.readRegisters(p.slave, p.function, p.reg, count, regs);

    mutex_enter_blocking(&pointLock);
    PointValue& v = pointValues[next];
    if (result == irivmb::RtuResult::Ok) {
        v.value = irivmb::decode(regs, p.type, cfg.rs485Order) * p.scale;
        v.failedInRow = 0;
    } else {
        ++v.errors;
        // Keep the last value through a couple of glitches, then report NaN so SCADA sees the loss.
        if (++v.failedInRow >= 3) v.value = NAN;
    }
    const bool report = result != irivmb::RtuResult::Ok && v.failedInRow == 3;
    mutex_exit(&pointLock);
    if (report) Serial.printf("RS485 %s slave %u: %s\n", p.id, p.slave, irivmb::rtuResultText(result));
    next = uint8_t((next + 1) % cfg.pointCount);
}
