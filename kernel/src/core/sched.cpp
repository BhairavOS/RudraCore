#include "core/sched.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "core/time.h"
#include "log.h"
#include "mem/memory.h"
#include "mem/pmm.h"
#include "panic.h"

extern "C" u8 kernelStackTop[]; // the boot stack, from start.S

namespace Rudracore::Sched {

using Core::Task;
using Core::TaskState;

namespace {

constexpr u64 STACK_PAGES = 4; // 16 KiB kernel stack per task
constexpr u64 TIME_SLICE_MS = 10;
constexpr u64 RFLAGS_INTERRUPTS_ENABLED = 0x202;

Task* idleTask = nullptr;
Task* currentTask = nullptr;

Task* readyHead = nullptr;
Task* readyTail = nullptr;
Task* sleeping = nullptr;
Task* zombies = nullptr;

u64 nextId = 0;
u64 liveTasks = 0;
u64 sliceStart = 0;

void enqueue(Task* t) {
    t->next = nullptr;
    if (readyTail)
        readyTail->next = t;
    else
        readyHead = t;
    readyTail = t;
}

void enqueueFront(Task* t) {
    t->next = readyHead;
    readyHead = t;
    if (not readyTail)
        readyTail = t;
}

Task* dequeue() {
    Task* t = readyHead;
    if (t) {
        readyHead = t->next;
        if (not readyHead)
            readyTail = nullptr;
        t->next = nullptr;
    }
    return t;
}

void wakeSleepers(u64 now) {
    Task** link = &sleeping;
    while (*link) {
        Task* t = *link;
        if (t->wakeAt <= now) {
            *link = t->next;
            t->state = TaskState::Ready;
            enqueue(t);
        } else {
            link = &t->next;
        }
    }
}

void destroy(Task* t) {
    t->releaseResources();
    if (t->stackPhysical)
        Pmm::free(t->stackPhysical, STACK_PAGES);
    delete t;
}

// Dead tasks can't free their own kernel stack while running on it, so they
// wait in the zombie list until some other task is running.
void reapZombies() {
    Task** link = &zombies;
    while (*link) {
        Task* t = *link;
        if (t == currentTask) {
            link = &t->next;
            continue;
        }
        *link = t->next;
        destroy(t);
    }
}

Task* newTask(char const* name) {
    u64 stack = Pmm::alloc(STACK_PAGES);
    if (not stack)
        return nullptr;

    Task* t = new Task{};
    t->id = nextId++;
    for (usize i = 0; name[i] and i + 1 < sizeof(t->name); i++)
        t->name[i] = name[i];
    Arch::Fpu::initState(t->fpuState);
    t->stackPhysical = stack;
    t->stackTop = reinterpret_cast<u64>(Mem::toVirtual(stack)) + STACK_PAGES * Mem::PAGE_SIZE;
    return t;
}

// The first "interrupt return" into a new task uses this frame.
Arch::Frame* initialFrame(Task* t) {
    auto* frame = reinterpret_cast<Arch::Frame*>(t->stackTop - sizeof(Arch::Frame));
    *frame = {};
    frame->rflags = RFLAGS_INTERRUPTS_ENABLED;
    t->savedFrame = reinterpret_cast<u64>(frame);
    return frame;
}

void start(Task* t) {
    Arch::InterruptGuard guard;
    liveTasks++;
    enqueue(t);
}

// Kernel tasks begin here, so returning from `entry` ends the task cleanly.
void kernelTaskStart(void (*entry)(u64), u64 arg) {
    entry(arg);
    exit(0);
}

void kernelSwitch() {
    asm volatile("int %0" : : "i"(Arch::Idt::YIELD_VECTOR) : "memory");
}

} // namespace

void init() {
    idleTask = new Task{};
    idleTask->id = nextId++;
    char const name[] = "idle";
    for (usize i = 0; i < sizeof(name); i++)
        idleTask->name[i] = name[i];
    idleTask->state = TaskState::Running;
    idleTask->stackTop = reinterpret_cast<u64>(kernelStackTop);
    Arch::Fpu::initState(idleTask->fpuState);
    currentTask = idleTask;
    liveTasks = 1;
}

Task* spawnKernel(char const* name, void (*entry)(u64), u64 arg) {
    Task* t = newTask(name);
    if (not t)
        return nullptr;

    Arch::Frame* frame = initialFrame(t);
    frame->rip = reinterpret_cast<u64>(&kernelTaskStart);
    frame->rdi = reinterpret_cast<u64>(entry);
    frame->rsi = arg;
    frame->cs = Arch::Gdt::KERNEL_CODE;
    frame->ss = Arch::Gdt::KERNEL_DATA;

    // Start below the frame, as if kernelTaskStart had been called: the ABI
    // expects rsp % 16 == 8 on function entry, with a return address there.
    u64 rsp = reinterpret_cast<u64>(frame) - 8;
    *reinterpret_cast<u64*>(rsp) = 0;
    frame->rsp = rsp;

    start(t);
    return t;
}

Task* spawnUser(char const* name, Vmm::Space* space, u64 entry, u64 stackTop,
                Core::Object* const* handles, usize handleCount) {
    Task* t = newTask(name);
    if (not t) {
        Vmm::destroyUserSpace(space);
        for (usize i = 0; i < handleCount; i++) {
            if (handles[i])
                handles[i]->unref();
        }
        return nullptr;
    }

    t->space = space;
    for (usize i = 0; i < handleCount and i + 1 < Task::MAX_HANDLES; i++)
        t->handles[i + 1] = handles[i];

    Arch::Frame* frame = initialFrame(t);
    frame->rip = entry;
    frame->rsp = stackTop;
    frame->cs = Arch::Gdt::USER_CODE;
    frame->ss = Arch::Gdt::USER_DATA;

    start(t);
    return t;
}

Task* current() {
    return currentTask;
}

u64 taskCount() {
    Arch::InterruptGuard guard;
    return liveTasks;
}

void yield() {
    kernelSwitch();
}

void sleep(u64 milliseconds) {
    Arch::InterruptGuard guard;
    currentTask->wakeAt = Time::now() + milliseconds;
    currentTask->state = TaskState::Sleeping;
    kernelSwitch();
}

void exit(i64 code) {
    Arch::disableInterrupts();
    if (currentTask == idleTask)
        panic("sched: the idle task can't exit");
    currentTask->exitCode = code;
    currentTask->state = TaskState::Dead;
    log("[sched] ", currentTask->name, " (task ", currentTask->id, ") finished");
    kernelSwitch();
    panic("sched: a dead task was resumed");
}

void idle() {
    for (;;) {
        Arch::enableInterrupts();
        Arch::waitForInterrupt();
    }
}

Arch::Frame* switchFrom(Arch::Frame* frame) {
    Task* prev = currentTask;
    prev->savedFrame = reinterpret_cast<u64>(frame);
    Arch::Fpu::save(prev->fpuState);

    switch (prev->state) {
    case TaskState::Running:
        prev->state = TaskState::Ready;
        if (prev != idleTask)
            enqueue(prev);
        break;
    case TaskState::Sleeping:
        prev->next = sleeping;
        sleeping = prev;
        break;
    case TaskState::Dead:
        prev->next = zombies;
        zombies = prev;
        liveTasks--;
        break;
    case TaskState::Blocked: // the object it waits on will wake it
    case TaskState::Ready:
        break;
    }

    reapZombies();

    Task* next = dequeue();
    if (not next)
        next = idleTask;

    next->state = TaskState::Running;
    currentTask = next;
    sliceStart = Time::now();

    // Interrupts from user mode must land on this task's kernel stack.
    Arch::Gdt::setKernelStack(next->stackTop);
    Arch::Fpu::restore(next->fpuState);

    Vmm::Space const& space = next->space ? *next->space : Vmm::kernelSpace();
    if (Arch::readCr3() != space.root)
        space.activate();

    return reinterpret_cast<Arch::Frame*>(next->savedFrame);
}

Arch::Frame* onTimerTick(Arch::Frame* frame) {
    Time::tick();
    u64 now = Time::now();
    wakeSleepers(now);

    if (not readyHead)
        return frame;
    if (currentTask == idleTask or now - sliceStart >= TIME_SLICE_MS)
        return switchFrom(frame);
    return frame;
}

// A task woken by a message or an interrupt runs next, ahead of tasks that
// were preempted: it's waiting on something that just happened (a reply, a
// key press), so running it right away keeps IPC fast and input responsive.
void wake(Task* task) {
    if (task->state == TaskState::Sleeping) {
        // A timed wait ending early: take it off the sleep list.
        for (Task** link = &sleeping; *link; link = &(*link)->next) {
            if (*link == task) {
                *link = task->next;
                break;
            }
        }
    } else if (task->state != TaskState::Blocked) {
        return;
    }
    task->state = TaskState::Ready;
    enqueueFront(task);
}

Arch::Frame* blockCurrentUntil(Arch::Frame* frame, u64 deadline) {
    if (currentTask == idleTask)
        panic("sched: the idle task can't block");
    currentTask->wakeAt = deadline;
    currentTask->state = TaskState::Sleeping;
    return switchFrom(frame);
}

Arch::Frame* blockCurrent(Arch::Frame* frame) {
    if (currentTask == idleTask)
        panic("sched: the idle task can't block");
    currentTask->state = TaskState::Blocked;
    return switchFrom(frame);
}

Arch::Frame* sleepCurrent(Arch::Frame* frame, u64 milliseconds) {
    currentTask->wakeAt = Time::now() + milliseconds;
    currentTask->state = TaskState::Sleeping;
    return switchFrom(frame);
}

Arch::Frame* exitCurrent(Arch::Frame* frame, i64 code) {
    if (currentTask == idleTask)
        panic("sched: the idle task can't exit");
    currentTask->exitCode = code;
    currentTask->state = TaskState::Dead;
    log("[sched] ", currentTask->name, " (task ", currentTask->id, ") exited with code ", code);
    return switchFrom(frame);
}

} // namespace Rudracore::Sched
