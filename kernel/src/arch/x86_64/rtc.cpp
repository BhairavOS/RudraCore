#include "arch/x86_64/rtc.h"

#include "arch/x86_64/io.h"

namespace Rudracore::Arch::Rtc {

namespace {

constexpr u16 CMOS_SELECT = 0x70;
constexpr u16 CMOS_DATA = 0x71;

constexpr u8 REG_SECONDS = 0x00;
constexpr u8 REG_MINUTES = 0x02;
constexpr u8 REG_HOURS = 0x04;
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

} // namespace

u64 secondsOfDay() {
    // The clock updates once a second; read twice and retry until both
    // reads agree, so no value is torn by an update.
    u8 s, m, h;
    for (;;) {
        while (read(REG_STATUS_A) & STATUS_A_UPDATING)
            ;
        s = read(REG_SECONDS);
        m = read(REG_MINUTES);
        h = read(REG_HOURS);
        while (read(REG_STATUS_A) & STATUS_A_UPDATING)
            ;
        if (s == read(REG_SECONDS) and m == read(REG_MINUTES) and h == read(REG_HOURS))
            break;
    }

    u8 status = read(REG_STATUS_B);
    bool pm = h & HOUR_PM;
    h &= static_cast<u8>(~HOUR_PM);
    if (not(status & STATUS_B_BINARY)) {
        s = fromBcd(s);
        m = fromBcd(m);
        h = fromBcd(h);
    }
    if (not(status & STATUS_B_24_HOUR)) {
        h %= 12;
        if (pm)
            h += 12;
    }
    return u64(h) * 3600 + u64(m) * 60 + s;
}

} // namespace Rudracore::Arch::Rtc
