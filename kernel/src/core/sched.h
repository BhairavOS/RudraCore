#pragma once

#include "arch/x86_64/frame.h"
#include "mem/vmm.h"

namespace Rudracore::Core {

enum class TaskState : u8 {
    Ready,
    Running,
    Sleeping,
    Dead,
};

struct Task {
    u64 id = 0;
    char name[32] = {};
    TaskState state = TaskState::Ready;

    // While the task isn't running: its saved CPU state (an Arch::Frame on
    // its kernel stack). Resuming the task means returning from an
    // interrupt with this frame.
    u64 savedFrame = 0;

    u64 stackPhysical = 0; // kernel stack pages (0 for the boot task's static stack)
    u64 stackTop = 0;      // also where the CPU enters the kernel from user mode

    Vmm::Space* space = nullptr; // nullptr for kernel tasks
    u64 wakeAt = 0;
    i64 exitCode = 0;

    Task* next = nullptr; // link in the ready, sleep or zombie list

    bool isUser() const { return space != nullptr; }
};

} // namespace Rudracore::Core

// Preemptive round-robin scheduler with a 10 ms time slice.
//
// Task switches happen only on the way out of an interrupt: the interrupt
// handler returns the frame of the task to resume, and the entry code in
// interrupts.S switches to that task's stack before restoring registers.
namespace Rudracore::Sched {

// Turns the boot context (kmain) into the idle task.
void init();

Core::Task* spawnKernel(char const* name, void (*entry)(u64), u64 arg);

// The task starts at `entry` in user mode, with its stack at `stackTop`.
// Takes ownership of `space`.
Core::Task* spawnUser(char const* name, Vmm::Space* space, u64 entry, u64 stackTop);

Core::Task* current();

u64 taskCount();

// For kernel tasks.
void yield();
void sleep(u64 milliseconds);
[[noreturn]] void exit(i64 code);

// Run by the boot task once setup is done: enables interrupts and waits.
[[noreturn]] void idle();

// For interrupt handlers (interrupts disabled). Each returns the frame of
// the task to resume, which may be a different task.
Arch::Frame* onTimerTick(Arch::Frame* frame);
Arch::Frame* switchFrom(Arch::Frame* frame);
Arch::Frame* sleepCurrent(Arch::Frame* frame, u64 milliseconds);
Arch::Frame* exitCurrent(Arch::Frame* frame, i64 code);

} // namespace Rudracore::Sched
