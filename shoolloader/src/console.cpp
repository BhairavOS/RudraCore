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

Efi::Char16 pendingKey() {
    Efi::InputKey key{};
    if (Efi::failed(system->conIn->readKeyStroke(system->conIn, &key)))
        return 0;
    return key.unicodeChar ? key.unicodeChar : u'?'; // (arrow keys and such count too)
}

bool waitForKey(uint64_t seconds) {
    for (uint64_t i = 0; i < seconds * 20; i++) {
        if (pendingKey())
            return true;
        bootServices->stall(50 * 1000);
    }
    return false;
}

namespace {

struct Allocation {
    uint64_t address, pages;
};
Allocation tracked[96];
uint32_t trackedCount = 0;
bool armed = false;

} // namespace

Context fallbackPoint;

// setjmp/longjmp for the Microsoft x64 ABI (the loader uses no SSE, so no
// xmm registers need saving). rcx is the Context.
asm(R"(
    .globl saveContext
saveContext:
    movq %rbx, 0(%rcx)
    movq %rbp, 8(%rcx)
    movq %rdi, 16(%rcx)
    movq %rsi, 24(%rcx)
    movq %r12, 32(%rcx)
    movq %r13, 40(%rcx)
    movq %r14, 48(%rcx)
    movq %r15, 56(%rcx)
    leaq 8(%rsp), %rdx
    movq %rdx, 64(%rcx)
    movq (%rsp), %rdx
    movq %rdx, 72(%rcx)
    xorl %eax, %eax
    ret

    .globl restoreContext
restoreContext:
    movq 0(%rcx), %rbx
    movq 8(%rcx), %rbp
    movq 16(%rcx), %rdi
    movq 24(%rcx), %rsi
    movq 32(%rcx), %r12
    movq 40(%rcx), %r13
    movq 48(%rcx), %r14
    movq 56(%rcx), %r15
    movq 64(%rcx), %rsp
    movl $1, %eax
    jmpq *72(%rcx)
)");

void armFallback() {
    trackedCount = 0;
    armed = true;
}

void disarmFallback() { armed = false; }

void fail(char const* what, Efi::Status status) {
    print("\nShoolLoader: can't boot: ");
    print(what);
    if (status) {
        print(" (UEFI status ");
        printNumber(status & ~Efi::ERROR_BIT);
        print(")");
    }
    if (armed) {
        // Give back what this attempt took, then try the fallback entry.
        armed = false;
        for (uint32_t i = 0; i < trackedCount; i++)
            bootServices->freePages(tracked[i].address, tracked[i].pages);
        trackedCount = 0;
        print("\n");
        bootServices->stall(1500 * 1000);
        restoreContext(&fallbackPoint);
    }
    print("\n\nPress a key, or wait 30 seconds, to go back to the firmware:\n"
          "it tries the next boot option (a recovery medium, another system).\n");
    for (int i = 0; i < 64 and pendingKey(); i++) {
    } // (keys pressed before the error, such as a held D)
    waitForKey(30);
    // Recovery: hand control back instead of halting. Nothing has been
    // started yet (this is only ever called before ExitBootServices).
    bootServices->exit(imageHandle, Efi::LOAD_ERROR, 0, nullptr);
    for (;;)
        asm volatile("cli; hlt"); // (the firmware refused to take it back)
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
    if (armed and trackedCount < sizeof(tracked) / sizeof(tracked[0]))
        tracked[trackedCount++] = {address, count}; // shortcut: past 96 allocations a failed attempt leaks the rest
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
