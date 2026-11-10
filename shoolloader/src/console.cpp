#include "loader.h"

namespace Loader {

Efi::SystemTable* system = nullptr;
Efi::BootServices* bootServices = nullptr;
Efi::Handle imageHandle = nullptr;

void print(char const* text) {
    // Convert to UTF-16 in small chunks, turning "\n" into "\r\n".
    Efi::Char16 buffer[128];
    uint64_t n = 0;
    auto flush = [&] {
        buffer[n] = 0;
        system->conOut->outputString(system->conOut, buffer);
        n = 0;
    };

    for (; *text; text++) {
        if (n + 3 >= sizeof(buffer) / sizeof(buffer[0]))
            flush();
        if (*text == '\n')
            buffer[n++] = u'\r';
        buffer[n++] = static_cast<unsigned char>(*text);
    }
    flush();
}

void printNumber(uint64_t value) {
    char digits[21];
    uint64_t i = sizeof(digits);
    digits[--i] = '\0';
    do {
        digits[--i] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    print(&digits[i]);
}

void printSize(uint64_t bytes) {
    if (bytes >= 1024 * 1024) {
        printNumber(bytes / (1024 * 1024));
        print(" MiB");
    } else {
        printNumber((bytes + 1023) / 1024);
        print(" KiB");
    }
}

void fail(char const* what, Efi::Status status) {
    print("\nShoolLoader: can't boot: ");
    print(what);
    if (status) {
        print(" (UEFI status ");
        printNumber(status & ~Efi::ERROR_BIT);
        print(")");
    }
    print("\n");
    bootServices->stall(30 * 1000 * 1000);
    for (;;)
        asm volatile("cli; hlt");
}

void copyMemory(void* dest, void const* src, uint64_t size) {
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(size) : : "memory");
}

void zeroMemory(void* dest, uint64_t size) {
    asm volatile("rep stosb" : "+D"(dest), "+c"(size) : "a"(0) : "memory");
}

uint64_t allocatePages(uint64_t count, Efi::MemoryType type) {
    Efi::PhysicalAddress address = 0;
    Efi::Status status = bootServices->allocatePages(Efi::ALLOCATE_ANY_PAGES, type, count, &address);
    if (Efi::failed(status))
        fail("out of memory", status);
    zeroMemory(reinterpret_cast<void*>(address), count * PAGE_SIZE);
    return address;
}

} // namespace Loader

// The compiler may call these on its own.
extern "C" void* memcpy(void* dest, void const* src, uint64_t size) {
    Loader::copyMemory(dest, src, size);
    return dest;
}

extern "C" void* memset(void* dest, int value, uint64_t size) {
    void* ret = dest;
    asm volatile("rep stosb" : "+D"(dest), "+c"(size) : "a"(value) : "memory");
    return ret;
}
