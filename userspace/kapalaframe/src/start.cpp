#include <kapala/sys.h>

extern "C" int main();

// Global constructors, collected by userspace/link.ld.
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

// C++ registers destructors of global objects here. Programs end through
// SYS_EXIT and the kernel frees everything a program owns (memory, handles),
// so global destructors don't need to run.
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

extern "C" {
void* __dso_handle = nullptr;
}

extern "C" [[noreturn]] void kapalaStart() {
    for (auto* ctor = __init_array_start; ctor < __init_array_end; ctor++)
        (*ctor)();
    Kapala::Sys::exit(main());
}
