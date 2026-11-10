#pragma once

#include "types.h"

namespace Rudracore {

// Prints an integer in hexadecimal, e.g. log("addr=", Hex{ptr}).
struct Hex {
    u64 value;
};

// Prints a byte count in a human readable unit, e.g. "256 MiB".
struct Bytes {
    u64 value;
};

using LogMirror = void (*)(char);

// Everything logged goes to serial, and also to the mirror once one is set
// (the framebuffer console sets itself as the mirror once it is ready).
void setLogMirror(LogMirror mirror);

void put(char c);
void put(char const* str);
void put(Hex hex);
void put(Bytes bytes);
void putUnsigned(u64 value);
void putSigned(i64 value);

template <typename T>
    requires(__is_integral(T) and not __is_same(T, char) and not __is_same(T, bool))
void put(T value) {
    if constexpr (__is_signed(T))
        putSigned(value);
    else
        putUnsigned(value);
}

inline void put(bool value) { put(value ? "true" : "false"); }

template <typename... Ts>
void log(Ts... args) {
    (put(args), ...);
    put('\n');
}

} // namespace Rudracore
