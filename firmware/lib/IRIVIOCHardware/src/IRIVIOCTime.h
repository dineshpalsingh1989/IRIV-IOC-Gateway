#pragma once
// Calendar <-> Unix time and BCD helpers for the PCF85063A RTC. Plain C++, tested on a PC.
#include <stdint.h>

namespace irivioc {
struct DateTime {
    uint16_t year = 2000;
    uint8_t month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

// The RTC stores a two-digit year, so the clock covers 2000-01-01 to 2099-12-31.
const uint32_t RtcFirstUnix = 946684800UL;   // 2000-01-01 00:00:00 UTC
const uint32_t RtcEndUnix = 4102444800UL;    // 2100-01-01 00:00:00 UTC (not included)

inline uint8_t fromBcd(uint8_t v) { return uint8_t((v >> 4) * 10 + (v & 0x0F)); }
inline uint8_t toBcd(uint8_t v) { return uint8_t(((v / 10) << 4) | (v % 10)); }

// Days since 1970-01-01 (H. Hinnant's days_from_civil).
inline int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = uint32_t(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int32_t(doe) - 719468;
}

inline bool validDate(const DateTime& t) {
    static const uint8_t days[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0);
    return t.year >= 2000 && t.year <= 2099 && t.month >= 1 && t.month <= 12 && t.day >= 1 &&
           t.day <= days[t.month - 1] - (t.month == 2 && !leap) && t.hour < 24 && t.minute < 60 && t.second < 60;
}

inline uint32_t toUnix(const DateTime& t) {
    return uint32_t(daysFromCivil(t.year, t.month, t.day)) * 86400UL + t.hour * 3600UL + t.minute * 60UL + t.second;
}

inline DateTime fromUnix(uint32_t unix) {
    // H. Hinnant's civil_from_days, for days >= 0.
    const uint32_t z = unix / 86400 + 719468, secs = unix % 86400;
    const uint32_t era = z / 146097, doe = z - era * 146097;
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    DateTime t;
    t.day = uint8_t(doy - (153 * mp + 2) / 5 + 1);
    t.month = uint8_t(mp < 10 ? mp + 3 : mp - 9);
    t.year = uint16_t(yoe + era * 400 + (t.month <= 2));
    t.hour = uint8_t(secs / 3600);
    t.minute = uint8_t(secs / 60 % 60);
    t.second = uint8_t(secs % 60);
    return t;
}

inline uint8_t weekday(uint32_t unix) { return uint8_t((unix / 86400 + 4) % 7); } // 0 = Sunday; 1970-01-01 was a Thursday
}
