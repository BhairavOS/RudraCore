#pragma once

// Kapalaframe: a small fixed-capacity text builder (no heap needed).

namespace Kapala {

template <unsigned long Capacity = 256>
struct Text {
    char data[Capacity] = {};
    unsigned long length = 0;

    Text& add(char const* s) {
        while (*s and length + 1 < Capacity)
            data[length++] = *s++;
        return *this;
    }

    Text& add(unsigned long value) {
        char digits[21];
        unsigned long n = 0;
        do {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (n and length + 1 < Capacity)
            data[length++] = digits[--n];
        return *this;
    }

    Text& add(long value) {
        if (value < 0) {
            add("-");
            return add(static_cast<unsigned long>(-(value + 1)) + 1);
        }
        return add(static_cast<unsigned long>(value));
    }

    Text& add(int value) { return add(static_cast<long>(value)); }
    Text& add(unsigned value) { return add(static_cast<unsigned long>(value)); }

    char const* cstr() const { return data; }
};

} // namespace Kapala
