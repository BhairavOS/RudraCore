// Memory routines the compiler may call on its own. String instructions keep
// the compiler from turning these bodies back into calls to themselves.

extern "C" {

void* memcpy(void* dest, void const* src, unsigned long n) {
    void* ret = dest;
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memset(void* dest, int value, unsigned long n) {
    void* ret = dest;
    asm volatile("rep stosb" : "+D"(dest), "+c"(n) : "a"(value) : "memory");
    return ret;
}

void* memmove(void* dest, void const* src, unsigned long n) {
    // (volatile: so the compiler can't turn these loops into memmove calls)
    auto volatile* d = static_cast<unsigned char*>(dest);
    auto const volatile* s = static_cast<unsigned char const*>(src);
    if (d < s) {
        for (unsigned long i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (unsigned long i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(void const* a, void const* b, unsigned long n) {
    auto const* x = static_cast<unsigned char const*>(a);
    auto const* y = static_cast<unsigned char const*>(b);
    for (unsigned long i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

} // extern "C"
