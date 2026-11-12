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
Core::Task* start(char const* name, usize nameLength, Core::Capability const* handles,
                  usize handleCount, Abi::Error& code, char const*& error);

// Whether the boot image has a program called `name`.
bool exists(char const* name, usize nameLength);

// Like start, for a program image in memory (an installed app). `name`
// is only its task's name.
Core::Task* startImage(char const* name, u8 const* data, usize size, Core::Capability const* handles,
                       usize handleCount, Abi::Error& code, char const*& error);

// A data file from the boot image as read-only shared memory (made on first
// use and kept), or nullptr.
Core::SharedMemory* file(char const* name);

} // namespace Rudracore::Programs
