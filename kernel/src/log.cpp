#include "log.h"

#include "serial.h"

namespace Rudracore {

static LogMirror _mirror = nullptr;

void setLogMirror(LogMirror mirror) {
    _mirror = mirror;
}

void put(char c) {
    Serial::write(c);
    if (_mirror)
        _mirror(c);
}

void put(char const* str) {
    if (not str)
        str = "(null)";
    while (*str)
        put(*str++);
}

void put(Str str) {
    for (usize i = 0; i < str.length; i++)
        put(str.data[i]);
}

void putUnsigned(u64 value) {
    char buf[21];
    usize i = sizeof(buf);
    buf[--i] = '\0';
    do {
        buf[--i] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    put(&buf[i]);
}

void putSigned(i64 value) {
    if (value < 0) {
        put('-');
        putUnsigned(static_cast<u64>(-(value + 1)) + 1);
    } else {
        putUnsigned(static_cast<u64>(value));
    }
}

void put(Hex hex) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    put("0x");
    for (int shift = (hex.digits - 1) * 4; shift >= 0; shift -= 4)
        put(DIGITS[(hex.value >> shift) & 0xF]);
}

void put(Bytes bytes) {
    static constexpr char const* UNITS[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    u64 value = bytes.value;
    usize unit = 0;
    while (value >= 1024 and unit + 1 < sizeof(UNITS) / sizeof(UNITS[0])) {
        value /= 1024;
        unit++;
    }
    putUnsigned(value);
    put(' ');
    put(UNITS[unit]);
}

} // namespace Rudracore
