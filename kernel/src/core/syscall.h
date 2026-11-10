#pragma once

#include "arch/x86_64/frame.h"
#include "boot.h"

namespace Rudracore::Syscall {

void init(Boot::Info const& info);

// Handles `int $0x80`. Returns the frame to resume (see Sched).
Arch::Frame* dispatch(Arch::Frame* frame);

} // namespace Rudracore::Syscall
