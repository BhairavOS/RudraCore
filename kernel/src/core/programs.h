#pragma once

#include "core/shared_memory.h"
#include "boot.h"
#include "core/sched.h"
#include "rudracore/syscalls.h"

// Programs shipped in the boot image (the bootloader's modules).
namespace Rudracore::Programs {

void init(Boot::Info const& info);

// Loads and starts the program called `name`, giving it `handles` (the
// references move to the new task, even on failure). Returns the task, or
// nullptr with `code` and a readable `error` set.
Core::Task* start(char const* name, usize nameLength, Core::Object* const* handles,
                  usize handleCount, Abi::Error& code, char const*& error);

// A data file from the boot image as read-only shared memory (made on first
// use and kept), or nullptr.
Core::SharedMemory* file(char const* name);

} // namespace Rudracore::Programs
