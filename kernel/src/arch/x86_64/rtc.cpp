#include "arch/x86_64/rtc.h"

#include "arch/x86_64/io.h"

namespace Rudracore::Arch::Rtc {

namespace {

constexpr u16 CMOS_SELECT = 0x70;
constexpr u16 CMOS_DATA = 0x71;

constexpr u8 REG_SECONDS = 0x00;
constexpr u8 REG_MINUTES = 0x02;
constexpr u8 REG_HOURS = 0x04;
constexpr u8 REG_DAY = 0x07;
constexpr u8 REG_MONTH = 0x08;
constexpr u8 REG_YEAR = 0x09;
constexpr u8 REG_STATUS_A = 0x0A;
constexpr u8 REG_STATUS_B = 0x0B;

constexpr u8 STATUS_A_UPDATING = 1 << 7;
constexpr u8 STATUS_B_24_HOUR = 1 << 1;
constexpr u8 STATUS_B_BINARY = 1 << 2;
constexpr u8 HOUR_PM = 1 << 7;

u8 read(u8 reg) {
    outb(CMOS_SELECT, reg);
    return inb(CMOS_DATA);
}

u8 fromBcd(u8 v) { return static_cast<u8>((v >> 4) * 10 + (v & 0x0F)); }

// Days from 1970-01-01 to the given date (proleptic Gregorian calendar).
i64 daysFromCivil(i64 y, u32 m, u32 d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = static_cast<u32>(y - era * 400);
    u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<i64>(doe) - 719468;
}

} // namespace

u64 unixTime() {
    // The clock updates once a second; read twice and retry until both
    // reads agree, so no value is torn by an update.
    u8 v[6], again[6];
    constexpr u8 REGS[6] = {REG_SECONDS, REG_MINUTES, REG_HOURS, REG_DAY, REG_MONTH, REG_YEAR};
    for (;;) {
        while (read(REG_STATUS_A) & STATUS_A_UPDATING)
            ;
        for (int i = 0; i < 6; i++)
            v[i] = read(REGS[i]);
        while (read(REG_STATUS_A) & STATUS_A_UPDATING)
            ;
        bool same = true;
        for (int i = 0; i < 6; i++) {
            again[i] = read(REGS[i]);
            same = same and again[i] == v[i];
        }
        if (same)
            break;
    }

    u8 status = read(REG_STATUS_B);
    u8 s = v[0], m = v[1], h = v[2], day = v[3], month = v[4], year = v[5];
    bool pm = h & HOUR_PM;
    h &= static_cast<u8>(~HOUR_PM);
    if (not(status & STATUS_B_BINARY)) {
        s = fromBcd(s);
        m = fromBcd(m);
        h = fromBcd(h);
        day = fromBcd(day);
        month = fromBcd(month);
        year = fromBcd(year);
    }
    if (not(status & STATUS_B_24_HOUR)) {
        h %= 12;
        if (pm)
            h += 12;
    }
    if (month < 1 or month > 12 or day < 1 or day > 31)
        return u64(h) * 3600 + u64(m) * 60 + s; // no usable date: just the time of day

    i64 days = daysFromCivil(2000 + year, month, day);
    return static_cast<u64>(days) * 86400 + u64(h) * 3600 + u64(m) * 60 + s;
}

} // namespace Rudracore::Arch::Rtc
