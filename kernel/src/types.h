#pragma once

#include <stddef.h>
#include <stdint.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using usize = size_t;
using isize = ptrdiff_t;

template <typename T>
constexpr T min(T a, T b) { return a < b ? a : b; }

template <typename T>
constexpr T max(T a, T b) { return a > b ? a : b; }

template <typename T>
constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
