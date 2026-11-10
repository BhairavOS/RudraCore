#include <kapala/sys.h>

extern "C" int main();

// Global constructors, collected by userspace/link.ld.
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

extern "C" [[noreturn]] void kapalaStart() {
    for (auto* ctor = __init_array_start; ctor < __init_array_end; ctor++)
        (*ctor)();
    Kapala::Sys::exit(main());
}
