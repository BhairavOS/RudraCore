#pragma once

#include "mem/vmm.h"

namespace Rudracore::Elf {

// Where a loaded program's stack ends (it grows down from here).
constexpr u64 USER_STACK_TOP = 0x00007ffffffff000;
constexpr u64 USER_STACK_SIZE = 64 * 1024;

// A program's segments must end below this: above it are shared memory
// mappings (from 4 GiB, Task::MAP_BASE), and far above those the stack, so
// the stack has terabytes of unmapped guard below it.
constexpr u64 USER_IMAGE_END = 0x100000000;

struct Program {
    Vmm::Space* space;
    u64 entry;
    u64 stackTop;
};

// The error load() gives when memory runs out (compare the pointer).
inline constexpr char const OUT_OF_MEMORY[] = "out of memory";

// Loads a static x86_64 ELF executable into a new user address space, with
// a stack. On failure returns false and sets `error`; nothing is leaked.
bool load(u8 const* data, usize size, Program& out, char const*& error);

} // namespace Rudracore::Elf
