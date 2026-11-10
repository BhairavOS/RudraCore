#pragma once

#include "mem/vmm.h"

namespace Rudracore::Elf {

// Where a loaded program's stack ends (it grows down from here).
constexpr u64 USER_STACK_TOP = 0x00007ffffffff000;
constexpr u64 USER_STACK_SIZE = 64 * 1024;

struct Program {
    Vmm::Space* space;
    u64 entry;
    u64 stackTop;
};

// Loads a static x86_64 ELF executable into a new user address space, with
// a stack. On failure returns false and sets `error`; nothing is leaked.
bool load(u8 const* data, usize size, Program& out, char const*& error);

} // namespace Rudracore::Elf
