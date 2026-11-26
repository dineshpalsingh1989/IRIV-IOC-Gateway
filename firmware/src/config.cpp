#include "config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include "register_map.h"

namespace {
// The dashboard stores form fields as text ("1883"); accept numbers or numeric text.
float number(JsonVariantConst v, float fallback) {
    if (v.is<float>() || v.is<long>()) return v.as<float>();
    const char* text = v.as<const char*>();
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const float value = strtof(text, &end);
    return end && !*end ? value : fallback;
}

void copyText(char* out, size_t size, JsonVariantConst v) {
    const char* text = v.as<const char*>();
    strlcpy(out, text ? text : "", size);
}


uint8_t functionCode(const char* fn) {
    if (!fn) return 3;
    if (!strcmp(fn, "coil")) return 1;
    if (!strcmp(fn, "discrete")) return 2;
    if (!strcmp(fn, "input")) return 4;
    return 3;
}
} // namespace

uint16_t serialConfig(const char* parity, int stopBits) {
    const bool two = stopBits == 2;
    if (parity && !strcmp(parity, "Even")) return two ? SERIAL_8E2 : SERIAL_8E1;
    if (parity && !strcmp(parity, "Odd")) return two ? SERIAL_8O2 : SERIAL_8O1;
    return two ? SERIAL_8N2 : SERIAL_8N1;
}

// `c` must be freshly constructed (defaults). It is not reset here: a temporary
// GatewayConfig (about 25 KB) would overflow the core's stack.
bool loadConfig(const char* path, GatewayConfig& c, String& error) {
    File file = LittleFS.open(path, "r");
    if (!file) { error = String(path) + " not found, using defaults"; return false; }
    // Keep only what the firmware uses: the export also holds labels, simulated values and the
    // full Modbus table for people, which would not fit with 64 RS485 points.
    StaticJsonDocument<768> filter;
    filter["network"] = true;
    filter["modbus"] = true;
    filter["rs485"] = true; // settings plus points[]
    filter["web"]["password"] = true;
    for (const char* key : {"id", "mode", "scaleLo", "scaleHi", "counter", "trigger", "compare", "threshold", "freq", "pattern", "silence"})
        filter["channels"][0][key] = true;
    for (const char* key : {"enabled", "host", "port", "tls", "ca", "client", "user", "pass", "retain", "qos", "interval"})
        filter["mqtt"][key] = true;
    for (const char* key : {"source", "publish", "subscribe"}) filter["map"]["mqtt"][0][key] = true;
    // Heap, freed after loading. Worst case (64 points, 82 topics of 128 characters) is about 51 KB.
    DynamicJsonDocument doc(65536);
    const DeserializationError parsed = deserializeJson(doc, file, DeserializationOption::Filter(filter));
    file.close();
    if (parsed) { error = String("JSON error: ") + parsed.c_str() + ", using defaults"; return false; }
    if (doc.overflowed()) { error = "ioc_config.json too large, using defaults"; return false; }

    JsonObjectConst net = doc["network"];
    c.network.useStatic = !(net["dhcp"] | true);
    if (c.network.useStatic) {
        const bool ok = irivioc::parseIPv4(net["ip"] | "", c.network.ip) &&
                        irivioc::parseIPv4(net["subnet"] | "", c.network.mask) &&
                        irivioc::parseIPv4(net["gateway"] | "", c.network.gateway, true) &&
                        irivioc::parseIPv4(net["dns"] | "", c.network.dns, true);
        if (!ok) { c.network.useStatic = false; error = "invalid static IP, using DHCP"; }
    }

    JsonObjectConst mb = doc["modbus"];
    c.modbusEnabled = mb["enabled"] | true;
    c.modbusPort = uint16_t(number(mb["port"], 502));
    c.modbusUnit = uint8_t(number(mb["unit"], 255));
    c.modbusOrder = irivmb::parseWordOrder(mb["wordOrder"] | "ABCD");

    JsonObjectConst mq = doc["mqtt"];
    c.mqttEnabled = mq["enabled"] | false;
    copyText(c.mqttHost, sizeof(c.mqttHost), mq["host"]);
    c.mqttPort = uint16_t(number(mq["port"], 1883));
    c.mqttTls = mq["tls"] | false;
    copyText(c.mqttCa, sizeof(c.mqttCa), mq["ca"]);
    // A cut-off certificate fails every check, so the broker is refused rather than trusted.
    if (strlen(mq["ca"] | "") >= sizeof(c.mqttCa)) error = "MQTT CA certificate too long (max 3071 characters)";
    copyText(c.mqttClient, sizeof(c.mqttClient), mq["client"]);
    copyText(c.mqttUser, sizeof(c.mqttUser), mq["user"]);
    copyText(c.mqttPass, sizeof(c.mqttPass), mq["pass"]);
    c.mqttRetain = mq["retain"] | true;
    c.mqttSubscribeQos = number(mq["qos"], 0) >= 1 ? 1 : 0;
    c.mqttIntervalMs = uint32_t(constrain(number(mq["interval"], 5), 0.1f, 3600.0f) * 1000);
    copyText(c.webPassword, sizeof(c.webPassword), doc["web"]["password"]);

    JsonObjectConst rs = doc["rs485"];
    c.rs485Enabled = rs["enabled"] | false;
    c.rs485Baud = uint32_t(number(rs["baud"], 9600));
    c.rs485Serial = serialConfig(rs["parity"] | "None", int(number(rs["stopBits"], 1)));
    c.rs485PollMs = uint32_t(constrain(number(rs["poll"], 1000), 100.0f, 60000.0f));
    c.rs485TimeoutMs = uint32_t(constrain(number(rs["timeout"], 500), 50.0f, 5000.0f));
    c.rs485Order = irivmb::parseWordOrder(rs["wordOrder"] | "ABCD");
    for (JsonObjectConst p : rs["points"].as<JsonArrayConst>()) {
        if (c.pointCount >= GatewayConfig::MaxPoints) { error = "too many RS485 points, extra ones ignored"; break; }
        PointConfig& pc = c.points[c.pointCount];
        copyText(pc.id, sizeof(pc.id), p["id"]);
        pc.slave = uint8_t(number(p["slave"], 1));
        pc.function = functionCode(p["fn"] | "holding");
        pc.reg = uint16_t(number(p["reg"], 0));
        pc.type = pc.function <= 2 ? irivmb::DataType::Bool : irivmb::parseDataType(p["type"] | "int16");
        pc.scale = number(p["scale"], 1);
        pc.tcp = uint16_t(number(p["tcp"], 0));
        // Each point is a float32 in the RS485 block; skip anything that would not fit.
        if (!pc.slave || pc.slave > 247 || pc.tcp < regmap::RS485 || pc.tcp + 2 > regmap::RS485 + regmap::RS485_REGS) {
            error = String("RS485 point ") + pc.id + " skipped: bad slave or Modbus TCP address";
            continue;
        }
        ++c.pointCount;
    }

    for (JsonObjectConst ch : doc["channels"].as<JsonArrayConst>()) {
        const char* id = ch["id"] | "";
        if (!strncmp(id, "AN", 2) && (id[2] == '0' || id[2] == '1') && !id[3]) {
            const int i = id[2] - '0';
            c.analogMode[i] = iriv::parseAnalogMode(ch["mode"] | "0-10V");
            c.analogLow[i] = number(ch["scaleLo"], 0);
            c.analogHigh[i] = number(ch["scaleHi"], 10);
        } else if (!strncmp(id, "DI", 2)) {
            const int i = atoi(id + 2);
            if (i >= 0 && i < 11) c.counter[i] = ch["counter"] | false;
        } else if (!strcmp(id, "BZ")) {
            iriv::parseTrigger(ch["trigger"] | "remote", c.buzzer);
            c.buzzer.above = strcmp(ch["compare"] | "above", "below") != 0;
            c.buzzer.threshold = number(ch["threshold"], 0);
            c.buzzer.frequency = uint16_t(constrain(number(ch["freq"], 2700), 100.0f, 10000.0f));
            c.buzzer.pattern = iriv::parsePattern(ch["pattern"] | "beep");
            c.buzzer.silenceMs = uint32_t(number(ch["silence"], 0) * 1000);
        }
    }

    for (JsonObjectConst t : doc["map"]["mqtt"].as<JsonArrayConst>()) {
        if (c.topicCount >= GatewayConfig::MaxTopics) break;
        TopicConfig& tc = c.topics[c.topicCount];
        copyText(tc.source, sizeof(tc.source), t["source"]);
        copyText(tc.publish, sizeof(tc.publish), t["publish"]);
        copyText(tc.subscribe, sizeof(tc.subscribe), t["subscribe"]);
        if (tc.source[0] && tc.publish[0]) ++c.topicCount;
    }
    return true;
}
