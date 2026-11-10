// Minimal memory routines. The compiler may emit calls to these even in a
// freestanding kernel, so they must exist. They use string instructions so
// the compiler can't turn their bodies back into calls to themselves.

#include "types.h"

extern "C" {

void* memcpy(void* dest, void const* src, usize n) {
    void* ret = dest;
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memset(void* dest, int value, usize n) {
    void* ret = dest;
    asm volatile("rep stosb" : "+D"(dest), "+c"(n) : "a"(value) : "memory");
    return ret;
}

void* memmove(void* dest, void const* src, usize n) {
    if (dest <= src or static_cast<u8*>(dest) >= static_cast<u8 const*>(src) + n)
        return memcpy(dest, src, n);

    // Overlapping with dest after src: copy backwards.
    void* ret = dest;
    u8* d = static_cast<u8*>(dest) + n - 1;
    u8 const* s = static_cast<u8 const*>(src) + n - 1;
    asm volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return ret;
}

int memcmp(void const* a, void const* b, usize n) {
    auto const* x = static_cast<u8 const*>(a);
    auto const* y = static_cast<u8 const*>(b);
    for (usize i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

} // extern "C"
