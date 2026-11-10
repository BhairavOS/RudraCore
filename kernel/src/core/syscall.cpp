#include "core/syscall.h"

#include "core/sched.h"
#include "core/time.h"
#include "log.h"
#include "rudracore/syscalls.h"

namespace Rudracore::Syscall {

namespace {

constexpr u64 MAX_LOG_LENGTH = 1024;

Arch::Frame* finish(Arch::Frame* frame, i64 result) {
    frame->rax = static_cast<u64>(result);
    return frame;
}

Arch::Frame* sysLog(Arch::Frame* frame, Core::Task* task) {
    u64 address = frame->rdi;
    u64 length = frame->rsi;

    if (length > MAX_LOG_LENGTH)
        return finish(frame, Abi::INVALID_ARGUMENT);

    // Never trust a pointer from userspace: it must be user memory that is
    // actually mapped in the caller's own address space.
    if (not Vmm::isUserRange(*task->space, address, length))
        return finish(frame, Abi::BAD_ADDRESS);

    // Drop a trailing newline; log() adds its own.
    auto const* text = reinterpret_cast<char const*>(address);
    if (length > 0 and text[length - 1] == '\n')
        length--;

    log("[", task->name, "] ", Str{text, length});
    return finish(frame, Abi::OK);
}

} // namespace

Arch::Frame* dispatch(Arch::Frame* frame) {
    Core::Task* task = Sched::current();
    if (not task->isUser())
        return finish(frame, Abi::UNKNOWN_SYSCALL); // kernel tasks use Sched directly

    switch (frame->rax) {
    case Abi::SYS_LOG:
        return sysLog(frame, task);

    case Abi::SYS_EXIT:
        return Sched::exitCurrent(frame, static_cast<i64>(frame->rdi));

    case Abi::SYS_YIELD:
        finish(frame, Abi::OK);
        return Sched::switchFrom(frame);

    case Abi::SYS_SLEEP:
        finish(frame, Abi::OK);
        return Sched::sleepCurrent(frame, frame->rdi);

    case Abi::SYS_TASK_ID:
        return finish(frame, static_cast<i64>(task->id));

    case Abi::SYS_TIME:
        return finish(frame, static_cast<i64>(Time::now()));

    default:
        return finish(frame, Abi::UNKNOWN_SYSCALL);
    }
}

} // namespace Rudracore::Syscall
