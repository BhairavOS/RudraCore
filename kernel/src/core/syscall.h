#pragma once

#include "arch/x86_64/frame.h"

namespace Rudracore::Syscall {

// Handles `int $0x80`. Returns the frame to resume (see Sched).
Arch::Frame* dispatch(Arch::Frame* frame);

} // namespace Rudracore::Syscall
