// IRIV IO Controller LAN gateway.
// Core 0: Ethernet, I/O, Modbus TCP server, MQTT. Core 1: RS485 Modbus RTU master polling.
// Settings come from /ioc_config.json in LittleFS, exported by the dashboard.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Updater.h>
#include <WebServer.h>
#include <pico/mutex.h>
#include <pico/unique_id.h>
#include <hardware/structs/sysinfo.h>

#include <IRIVIOCHardware.h>
#include <IRIVIOCIo.h>
#include <IRIVModbus.h>
#include <WilcoxonMqtt.h>

#include "config.h"
#include "register_map.h"
#include "web_files.h"

using irivmb::Area;

static GatewayConfig cfg;  // global: about 25 KB, too big for a stack
static IRIVIOCIo io;
static irivioc::Ethernet ethernet(PIN_W5500_CS, SPI, PIN_W5500_INT);
static irivioc::Rtc rtc; // PCF85063A on Wire (GP16/GP17)
static char hostname[32];
static volatile bool ready = false; // core 1 waits for setup() to finish
// For SELFTEST: what happened at boot.
static bool ethernetStarted = false, fsMounted = false;
static String settingsNote;          // why the settings file was not (fully) loaded
static uint32_t identifyUntil = 0;   // IDENTIFY: beep and fast blink until this millis()
static uint32_t outputTestStart = 0; // TESTOUTPUTS: running light on DO0-DO3 from this millis(); 0 = off

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
static WiFiClientSecure mqttTlsNet; // used when mqtt.tls is set
static std::unique_ptr<BearSSL::X509List> mqttCa; // must outlive mqttTlsNet's connections
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
    if (int32_t(identifyUntil - now) > 0) io.buzzer(now / 250 % 2 == 0, 2700);
    else io.buzzer(toneOn, cfg.buzzer.frequency);

    if (outputTestStart) {
        // DO0, DO1, DO2, DO3, DO2, DO1 at 250 ms each, twice; then back to the commanded states.
        static const uint8_t steps[] = {0, 1, 2, 3, 2, 1};
        const uint32_t step = (now - outputTestStart) / 250;
        if (step < 2 * sizeof(steps)) {
            for (uint8_t i = 0; i < 4; ++i) io.setOutput(i, i == steps[step % sizeof(steps)]);
        } else {
            outputTestStart = 0;
            for (uint8_t i = 0; i < 4; ++i) io.setOutput(i, doBits[i]);
        }
    }

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
        if (mqttCa) {
            // Certificate dates are checked against the RTC; without a valid time every check fails.
            uint32_t t;
            if (rtc.read(t) != irivioc::Rtc::State::Ok) { log("MQTT TLS: RTC time not valid, set it from the installer"); return; }
            mqttTlsNet.setX509Time(t);
        }
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
// In:  HELLO | STATUS | SELFTEST | IDENTIFY | CONFIG <bytes> (then exactly that many bytes of JSON) |
//      TIME <unix UTC> | REBOOT | BOOTLOADER
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

// RTC for the installer: "ok" with the time, "lost" (stopped while unpowered: CR2032 flat or
// missing, time not valid) or "missing" (chip not answering).
static void addRtc(JsonDocument& d) {
    uint32_t time;
    const irivioc::Rtc::State state = rtc.read(time);
    JsonObject o = d.createNestedObject("rtc");
    o["state"] = state == irivioc::Rtc::State::Ok ? "ok" : state == irivioc::Rtc::State::Lost ? "lost" : "missing";
    if (state == irivioc::Rtc::State::Ok) o["time"] = time;
}

static void setClock(uint32_t unixTime) {
    if (!rtc.write(unixTime)) { replyError("cannot set the clock: time outside 2000-2099 or RTC not answering"); return; }
    StaticJsonDocument<128> d;
    d["type"] = "time";
    addRtc(d);
    replyJson(d);
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
    addRtc(d);
    replyJson(d);
}

static void fillStatus(JsonDocument& d) {
    d["type"] = "status";
    d["version"] = GATEWAY_VERSION_TEXT;
    d["host"] = hostname;
    d["ip"] = ethernet.localIP().toString();
    d["link"] = ethernet.linkUp();
    d["online"] = ethernet.connected();
    d["dhcp"] = !cfg.network.useStatic;
    d["uptime"] = millis() / 1000;
    addRtc(d);
    JsonObject mb = d.createNestedObject("modbus");
    mb["enabled"] = cfg.modbusEnabled;
    mb["port"] = cfg.modbusPort;
    mb["clients"] = modbus.clientCount();
    mb["requests"] = modbus.stats().requests;
    JsonObject mq = d.createNestedObject("mqtt");
    mq["enabled"] = cfg.mqttEnabled;
    mq["host"] = cfg.mqttHost;
    mq["tls"] = cfg.mqttTls;
    mq["brokerChecked"] = bool(mqttCa);
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
    // Live I/O for the web page. Analog values are in the configured unit: V or mA.
    JsonObject live = d.createNestedObject("io");
    JsonArray di = live.createNestedArray("di");
    for (uint8_t i = 0; i < 11; ++i) di.add(io.input(i));
    JsonArray outs = live.createNestedArray("do");
    for (uint8_t i = 0; i < 4; ++i) outs.add(io.output(i));
    JsonArray an = live.createNestedArray("an");
    for (uint8_t i = 0; i < 2; ++i) an.add(iriv::analogSignal(cfg.analogMode[i], io.adc(i), io.adcReference));
    JsonArray counts = live.createNestedArray("count");
    for (uint8_t k = 0; k < 5; ++k) counts.add(io.count(IRIVIOCIo::CounterInputs[k]));
    live["buzzer"] = buzzerSounding;
}

static void sendStatus() {
    DynamicJsonDocument d(6144);
    fillStatus(d);
    replyJson(d);
}

// Raw facts for the installer's controller check; the installer decides pass or fail.
static void fillSelfTest(JsonDocument& d) {
    d["type"] = "selftest";
    d["ethernetChip"] = ethernetStarted; // begin() fails when the W5500 does not answer over SPI
    d["link"] = ethernet.linkUp();
    d["ip"] = ethernet.localIP().toString();
    addRtc(d);
    JsonObject fs = d.createNestedObject("flash");
    fs["mounted"] = fsMounted;
    FSInfo info;
    if (fsMounted && LittleFS.info(info)) { fs["total"] = uint32_t(info.totalBytes); fs["used"] = uint32_t(info.usedBytes); }
    JsonObject st = d.createNestedObject("settings");
    st["saved"] = fsMounted && LittleFS.exists(ConfigPath);
    if (settingsNote.length()) st["note"] = settingsNote;
    d["temperature"] = analogReadTemp();
    JsonArray di = d.createNestedArray("di");
    for (uint8_t i = 0; i < 11; ++i) di.add(io.input(i));
    JsonArray an = d.createNestedArray("an");
    for (uint8_t i = 0; i < 2; ++i) an.add(io.terminalVolts(i));
}

static void sendSelfTest() {
    DynamicJsonDocument d(1024);
    fillSelfTest(d);
    replyJson(d);
}

// Checks UploadPath with the real loader, then swaps it in for the saved settings (USB and web).
static bool installUploadedConfig(String& error, String& warning) {
    std::unique_ptr<GatewayConfig> check(new GatewayConfig()); // ~25 KB: heap, not stack
    if (!loadConfig(UploadPath, *check, warning)) { LittleFS.remove(UploadPath); error = warning; return false; }
    if (!LittleFS.rename(UploadPath, ConfigPath)) { error = "cannot save settings"; return false; } // replaces the old file
    return true;
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

    String error, warning;
    if (!installUploadedConfig(error, warning)) { replyError(error.c_str()); return; }
    StaticJsonDocument<384> d;
    d["type"] = "saved";
    d["bytes"] = size;
    if (warning.length()) d["warning"] = warning;
    replyJson(d);
    Serial.flush();
    delay(300);
    rp2040.reboot(); // start again with the new settings
}

// Firmware update over USB: "UPDATE <bytes> <md5>" then the .bin. Arduino-Pico's Updater stores it in
// LittleFS and checks the MD5; its boot stage copies it into place on the next start.
static void receiveFirmware(long size, const char* md5) {
    if (size <= 0 || size > 1024L * 1024 || strlen(md5) != 32) { replyError("usage: UPDATE <bytes> <md5>"); return; }
    if (!Update.begin(size)) { replyError("not enough free flash for the update"); return; }
    Update.setMD5(md5);
    uint8_t buffer[512];
    long received = 0;
    uint32_t lastByte = millis();
    while (received < size && millis() - lastByte < 5000) {
        rp2040.wdt_reset();
        const int available = Serial.available();
        if (available <= 0) { delay(1); continue; }
        const size_t want = size_t(min(long(sizeof(buffer)), min(long(available), size - received)));
        const size_t got = Serial.readBytes(reinterpret_cast<char*>(buffer), want);
        if (Update.write(buffer, got) != got) break;
        received += long(got);
        lastByte = millis();
    }
    if (received != size || !Update.end()) {
        LittleFS.remove("firmware.bin"); // free the space; the running firmware stays as it was
        replyError(received != size ? "update incomplete" : "update rejected: MD5 mismatch or flash error");
        return;
    }
    StaticJsonDocument<64> d;
    d["type"] = "updated";
    d["bytes"] = size;
    replyJson(d);
    Serial.flush();
    delay(300);
    rp2040.reboot(); // the boot stage installs the new firmware, then starts it
}

static void serviceUsbCommands() {
    static char line[64];
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
        else if (!strcmp(line, "SELFTEST")) sendSelfTest();
        else if (!strcmp(line, "TESTOUTPUTS")) { outputTestStart = millis() | 1; StaticJsonDocument<64> d; d["type"] = "testoutputs"; d["ms"] = 3000; replyJson(d); }
        else if (!strcmp(line, "IDENTIFY")) { identifyUntil = millis() + 3000; StaticJsonDocument<64> d; d["type"] = "identify"; replyJson(d); }
        else if (!strncmp(line, "CONFIG ", 7)) receiveConfig(atol(line + 7));
        else if (!strncmp(line, "UPDATE ", 7)) {
            char* md5 = nullptr;
            const long size = strtol(line + 7, &md5, 10);
            receiveFirmware(size, md5 && *md5 == ' ' ? md5 + 1 : "");
        }
        else if (!strncmp(line, "TIME ", 5)) setClock(strtoul(line + 5, nullptr, 10));
        else if (!strcmp(line, "REBOOT")) { Serial.flush(); delay(100); rp2040.reboot(); }
        else if (!strcmp(line, "BOOTLOADER")) { Serial.flush(); delay(100); rp2040.rebootToBootloader(); }
        else if (line[0]) replyError("unknown command");
    }
}

// ---------------------------------------------------------------------------------------------
// Web page on port 80: the same dashboard as the website, with live values from this controller.
// Reading is open; changing settings, updating firmware and switching outputs need the web password
// (HTTP digest, user "admin"). Without a password set, the page is read-only.

static WebServer web(80);
static String updateError;   // set while a firmware upload is refused or fails
static bool updateDone = false;

static bool webAllowed() {
    if (!cfg.webPassword[0]) {
        web.send(403, "text/plain", "No web password set: set one under Connections and send the settings over USB");
        return false;
    }
    if (web.authenticate("admin", cfg.webPassword)) return true;
    web.requestAuthentication(DIGEST_AUTH, "IRIV IOC");
    return false;
}

static void webJson(JsonDocument& d) {
    String text;
    serializeJson(d, text);
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", text);
}

// An empty password in an upload keeps the saved one: the page never receives the saved passwords.
static void keepSecret(JsonDocument& doc, const char* section, const char* key, const char* saved) {
    if (!*(doc[section][key] | "") && *saved) doc[section][key] = saved;
}

static void webGetConfig() {
    File file = LittleFS.open(ConfigPath, "r");
    if (!file) { web.send(404, "application/json", "{}"); return; }
    DynamicJsonDocument doc(65536);
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) { web.send(500, "text/plain", "saved settings unreadable"); return; }
    if (doc["mqtt"].is<JsonObject>()) doc["mqtt"]["pass"] = ""; // passwords never leave the controller
    doc.remove("web");
    webJson(doc);
}

static void webPostConfig() {
    if (!webAllowed()) return;
    const String& body = web.arg("plain");
    if (!body.length() || body.length() > 65536) { web.send(400, "text/plain", "settings must be 1 to 65536 bytes of JSON"); return; }
    DynamicJsonDocument doc(65536);
    if (deserializeJson(doc, body)) { web.send(400, "text/plain", "settings are not valid JSON"); return; }
    keepSecret(doc, "mqtt", "pass", cfg.mqttPass);
    keepSecret(doc, "web", "password", cfg.webPassword);
    File file = LittleFS.open(UploadPath, "w");
    if (!file) { web.send(500, "text/plain", "cannot write to flash"); return; }
    serializeJson(doc, file);
    file.close();
    String error, warning;
    if (!installUploadedConfig(error, warning)) { web.send(400, "text/plain", error); return; }
    StaticJsonDocument<256> reply;
    reply["type"] = "saved";
    if (warning.length()) reply["warning"] = warning;
    webJson(reply);
    delay(300);
    rp2040.reboot(); // start again with the new settings
}

// POST /api/update?size=<bytes>&md5=<hex> with the .bin as a multipart file: same Updater path as USB.
static void webUpdateUpload() {
    HTTPUpload& up = web.upload();
    if (up.status == UPLOAD_FILE_START) {
        updateError = "";
        updateDone = false;
        if (!cfg.webPassword[0] || !web.authenticate("admin", cfg.webPassword)) { updateError = "auth"; return; }
        const long size = web.arg("size").toInt();
        const String md5 = web.arg("md5");
        if (size <= 0 || size > 1024L * 1024 || md5.length() != 32) { updateError = "size and md5 are required"; return; }
        if (!Update.begin(size)) { updateError = "not enough free flash for the update"; return; }
        Update.setMD5(md5.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        rp2040.wdt_reset();
        if (!updateError.length() && Update.write(up.buf, up.currentSize) != up.currentSize) updateError = "flash write failed";
    } else if (up.status == UPLOAD_FILE_END) {
        if (!updateError.length()) {
            if (Update.end()) updateDone = true;
            else updateError = "update rejected: MD5 mismatch or incomplete";
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        updateError = "upload aborted";
    }
}

static void webUpdateDone() {
    if (updateError == "auth") { webAllowed(); return; } // answers 403 or asks for the password
    if (!updateDone) {
        Update.end();
        LittleFS.remove("firmware.bin"); // free the space; the running firmware stays as it was
        web.send(400, "text/plain", updateError.length() ? updateError : String("no firmware file received"));
        return;
    }
    web.send(200, "application/json", "{\"type\":\"updated\"}");
    delay(300);
    rp2040.reboot(); // the boot stage installs the new firmware, then starts it
}

static void startWeb() {
    for (const WebFile& f : webFiles) {
        web.on(f.path, HTTP_GET, [&f] {
            web.sendHeader("Content-Encoding", "gzip");
            web.sendHeader("Cache-Control", "no-cache");
            web.send_P(200, f.mime, reinterpret_cast<const char*>(f.gz), f.size);
        });
    }
    web.on("/api/status", HTTP_GET, [] {
        DynamicJsonDocument d(6144);
        fillStatus(d);
        d["webPassword"] = cfg.webPassword[0] != 0; // whether this page can change anything
        webJson(d);
    });
    web.on("/api/login", HTTP_GET, [] { if (webAllowed()) web.send(200, "application/json", "{\"type\":\"login\"}"); });
    web.on("/api/selftest", HTTP_GET, [] { DynamicJsonDocument d(1024); fillSelfTest(d); webJson(d); });
    web.on("/api/config", HTTP_GET, webGetConfig);
    web.on("/api/config", HTTP_POST, webPostConfig);
    web.on("/api/update", HTTP_POST, webUpdateDone, webUpdateUpload);
    web.on("/api/identify", HTTP_POST, [] {
        if (!webAllowed()) return;
        identifyUntil = millis() + 3000;
        web.send(200, "application/json", "{\"type\":\"identify\"}");
    });
    web.on("/api/testoutputs", HTTP_POST, [] {
        if (!webAllowed()) return;
        outputTestStart = millis() | 1;
        web.send(200, "application/json", "{\"type\":\"testoutputs\"}");
    });
    web.onNotFound([] { web.send(404, "text/plain", "not found"); });
    web.begin();
}

// ---------------------------------------------------------------------------------------------
// Core 0

void setup() {
    Serial.begin(115200);
    io.begin();
    io.led(true);
    Wire.begin();
    uint32_t rtcTime;
    const irivioc::Rtc::State rtcState = rtc.read(rtcTime);
    if (rtcState == irivioc::Rtc::State::Lost) log("RTC time lost (CR2032 flat or missing): set it from the installer");
    else if (rtcState == irivioc::Rtc::State::Missing) log("RTC not answering");

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
    snprintf(hostname, sizeof(hostname), "IRIV-IOC_%s", id + strlen(id) - 6); // same style as Cytron's firmware

    String error;
    fsMounted = LittleFS.begin(); // formats if the area holds no LittleFS of this size (Arduino-Pico default)
    if (!fsMounted) error = "LittleFS mount failed, using defaults";
    else if (LittleFS.exists(ConfigPath)) loadConfig(ConfigPath, cfg, error); // no file = defaults, not an error
    if (error.length()) log(error.c_str());
    settingsNote = error;

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
    ethernetStarted = ethernet.begin(hostname, cfg.network);
    if (!ethernetStarted) log("Ethernet start failed");
    startWeb();

    if (cfg.modbusEnabled) modbus.begin(cfg.modbusPort, cfg.modbusUnit);
    if (cfg.mqttTls) {
        if (cfg.mqttCa[0]) {
            mqttCa.reset(new BearSSL::X509List(cfg.mqttCa));
            if (!mqttCa->getCount()) log("MQTT TLS: CA certificate not readable, the broker will be refused");
            mqttTlsNet.setTrustAnchors(mqttCa.get());
        } else {
            mqttTlsNet.setInsecure(); // no CA given: encrypted, but the broker is not checked
        }
        mqtt.setClient(mqttTlsNet);
    }
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
    web.handleClient();
    if (int32_t(now - nextBlink) >= 0) { // blinking USR LED = loop alive, as on Cytron's firmware
        nextBlink = now + (int32_t(identifyUntil - now) > 0 ? 100 : 500);
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
