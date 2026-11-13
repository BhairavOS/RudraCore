#pragma once

#include "arch/x86_64/frame.h"
#include "core/object.h"
#include "core/quota.h"
#include "mem/vmm.h"

namespace Rudracore::Core {

enum class TaskState : u8 {
    Ready,
    Running,
    Sleeping,
    Blocked, // waiting on an object (channel, irq); the object holds the task
    Dead,
};

// Shared memory mapped into a task's address space.
struct Mapping {
    Object* object = nullptr; // holds a reference
    u64 address = 0;
    u64 size = 0;
};

// Who a task is, which bounds what it may hold and do:
//   Root    init (started by the kernel)
//   System  what init starts: drivers, servers, KamalShell, boot checks
//   App     what anything else starts: built-in and installed apps
// Only Root and System tasks may start programs, and an App never holds
// hardware (devices, IRQs, I/O ports, device memory).
enum class Privilege : u8 {
    App,
    System,
    Root,
};

struct Task {
    static constexpr usize MAX_HANDLES = 64;
    static constexpr usize MAX_MAPPINGS = 32;
    static constexpr u64 MAP_BASE = 0x100000000; // shared memory goes from 4 GiB up

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
    Quota* quota = nullptr;      // user tasks: the memory it may hold (holds a reference)
    Privilege privilege = Privilege::App;
    u64 wakeAt = 0;
    u64 waitDeadline = 0; // while in a timed SYS_WAIT: when it gives up (0 = none)
    i64 exitCode = 0;

    Task* next = nullptr;    // link in the ready, sleep or zombie list
    Task* allNext = nullptr; // link in the list of every task (Sched::allTasks)

    // Floating point / SSE registers while the task isn't running (FXSAVE
    // format, must be 16-byte aligned).
    alignas(16) u8 fpuState[512];

    // A handle's value is (generation << 8) | slot; the generation counts
    // how often the slot was freed, so a stale value never matches.
    struct HandleSlot {
        Capability cap;
        u64 generation = 0;
    };
    HandleSlot handles[MAX_HANDLES] = {}; // slot 0 is never used
    Mapping mappings[MAX_MAPPINGS] = {};
    u64 nextMapAddress = MAP_BASE;

    bool isUser() const { return space != nullptr; }

    // Takes over the capability's reference. Returns the handle, or 0 if the
    // table is full.
    u64 addHandle(Capability cap);

    // The capability behind a handle (no reference added; revoked ones
    // included), or nullptr if the handle isn't open.
    Capability* handle(u64 h);

    // Removes the handle and returns its capability (and reference) to the
    // caller; an empty one if the handle isn't open.
    Capability takeHandle(u64 h);

    // Drops every handle and mapping, and the address space.
    void releaseResources();
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
// Takes ownership of `space` and of the references in `handles`, which
// become the task's handles 1, 2, 3... (nullptr entries leave a gap).
Core::Task* spawnUser(char const* name, Vmm::Space* space, u64 entry, u64 stackTop,
                      Core::Capability const* handles = nullptr, usize handleCount = 0);

// Every task that exists (linked by Task::allNext), dead ones not yet freed included.
Core::Task* allTasks();

Core::Task* current();

u64 taskCount();

// For kernel tasks.
void yield();
void sleep(u64 milliseconds);
[[noreturn]] void exit(i64 code);

// Run by the boot task once setup is done: enables interrupts and waits.
[[noreturn]] void idle();

// Makes a Blocked task runnable again, or ends a timed wait (a Sleeping
// task) early. Interrupts must be disabled.
void wake(Core::Task* task);

// For interrupt handlers (interrupts disabled). Each returns the frame of
// the task to resume, which may be a different task.
Arch::Frame* onTimerTick(Arch::Frame* frame);
Arch::Frame* switchFrom(Arch::Frame* frame);
Arch::Frame* sleepCurrent(Arch::Frame* frame, u64 milliseconds);
Arch::Frame* exitCurrent(Arch::Frame* frame, i64 code);

// Blocks the current task until something calls wake() on it. The caller
// must have registered the task with the object it waits on first.
Arch::Frame* blockCurrent(Arch::Frame* frame);

// Same, but also wakes up by itself at `deadline` (milliseconds since boot).
Arch::Frame* blockCurrentUntil(Arch::Frame* frame, u64 deadline);

} // namespace Rudracore::Sched
