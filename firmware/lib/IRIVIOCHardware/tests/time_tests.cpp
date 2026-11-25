// Native tests for the RTC date math. Run: python tools/run_native_tests.py
#include "../src/IRIVIOCTime.h"

#include <stdio.h>

using namespace irivioc;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static DateTime date(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    DateTime t; t.year = y; t.month = mo; t.day = d; t.hour = h; t.minute = mi; t.second = s;
    return t;
}

int main() {
    CHECK(toUnix(date(2000, 1, 1, 0, 0, 0)) == RtcFirstUnix);
    CHECK(toUnix(date(2024, 2, 29, 12, 34, 56)) == 1709210096UL);
    CHECK(toUnix(date(2099, 12, 31, 23, 59, 59)) == RtcEndUnix - 1);
    // Every day of the RTC's range survives a round trip.
    for (uint32_t t = RtcFirstUnix; t < RtcEndUnix; t += 86400 + 3661) {
        const DateTime d = fromUnix(t);
        if (!validDate(d) || toUnix(d) != t) { CHECK(false); printf("  at %lu\n", (unsigned long)t); break; }
    }
    CHECK(weekday(RtcFirstUnix) == 6);   // 2000-01-01 was a Saturday
    CHECK(weekday(1709210096UL) == 4);   // 2024-02-29 was a Thursday
    CHECK(!validDate(date(2023, 2, 29, 0, 0, 0)) && validDate(date(2000, 2, 29, 0, 0, 0)));
    CHECK(!validDate(date(2024, 13, 1, 0, 0, 0)) && !validDate(date(2024, 4, 31, 0, 0, 0)) && !validDate(date(2024, 1, 0, 0, 0, 0)));
    CHECK(toBcd(59) == 0x59 && fromBcd(0x59) == 59 && fromBcd(toBcd(7)) == 7);
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
