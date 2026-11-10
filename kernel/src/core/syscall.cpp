#include "core/syscall.h"

#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "core/channel.h"
#include "core/device.h"
#include "core/programs.h"
#include "core/sched.h"
#include "core/shared_memory.h"
#include "core/time.h"
#include "log.h"
#include "mem/memory.h"
#include "mem/pmm.h"
#include "rudracore/syscalls.h"

extern "C" void* memcpy(void* dest, void const* src, usize n);

namespace Rudracore::Syscall {

using Core::Object;
using Core::ObjectType;
using Core::Task;

namespace {

constexpr u64 MAX_LOG_LENGTH = 1024;
constexpr u64 MAX_SPAWN_HANDLES = 8;
constexpr u64 INT80_LENGTH = 2; // `int $0x80` is 2 bytes: CD 80

// Highest address shared memory may be mapped at (below the user stack).
constexpr u64 MAP_LIMIT = 0x00007f0000000000;

// Everything a system call needs: the caller and its saved registers.
struct Call {
    Arch::Frame* frame;
    Task* task;

    u64 arg0() const { return frame->rdi; }
    u64 arg1() const { return frame->rsi; }
    u64 arg2() const { return frame->rdx; }
    u64 arg3() const { return frame->r10; }
    u64 arg4() const { return frame->r8; }

    Arch::Frame* result(i64 value) const {
        frame->rax = static_cast<u64>(value);
        return frame;
    }

    // Puts the task to sleep until it's woken, then runs the same system
    // call again from the start: rax and the arguments are still in the
    // frame, so rewinding rip over `int $0x80` is all it takes. When it
    // runs again the call finds what it was waiting for (or an error).
    Arch::Frame* blockAndRetry() const {
        frame->rip -= INT80_LENGTH;
        return Sched::blockCurrent(frame);
    }

    bool userMemory(u64 address, u64 length, bool writable = false) const {
        return Vmm::isUserRange(*task->space, address, length, writable);
    }

    // The object behind a handle if it's open and of the right type;
    // otherwise sets `error`.
    template <typename T>
    T* lookup(u64 handle, ObjectType type, i64& error) const {
        Object* object = task->handle(handle);
        if (not object) {
            error = Abi::BAD_HANDLE;
            return nullptr;
        }
        if (object->type != type) {
            error = Abi::WRONG_TYPE;
            return nullptr;
        }
        return static_cast<T*>(object);
    }
};

// MARK: Tasks

Arch::Frame* sysLog(Call const& c) {
    u64 address = c.arg0();
    u64 length = c.arg1();

    if (length > MAX_LOG_LENGTH)
        return c.result(Abi::INVALID_ARGUMENT);

    // Never trust a pointer from userspace: it must be user memory that is
    // actually mapped in the caller's own address space.
    if (not c.userMemory(address, length))
        return c.result(Abi::BAD_ADDRESS);

    // Drop a trailing newline; log() adds its own.
    auto const* text = reinterpret_cast<char const*>(address);
    if (length > 0 and text[length - 1] == '\n')
        length--;

    log("[", c.task->name, "] ", Str{text, length});
    return c.result(Abi::OK);
}

Arch::Frame* sysSpawn(Call const& c) {
    u64 nameAddress = c.arg0();
    u64 nameLength = c.arg1();
    u64 handlesAddress = c.arg2();
    u64 handleCount = c.arg3();

    char name[32];
    if (nameLength == 0 or nameLength >= sizeof(name) or handleCount > MAX_SPAWN_HANDLES)
        return c.result(Abi::INVALID_ARGUMENT);
    if (not c.userMemory(nameAddress, nameLength) or
        not c.userMemory(handlesAddress, handleCount * sizeof(Abi::Handle)))
        return c.result(Abi::BAD_ADDRESS);

    memcpy(name, reinterpret_cast<void const*>(nameAddress), nameLength);
    name[nameLength] = '\0';

    // Check every handle before moving any, so a bad one changes nothing.
    Abi::Handle handles[MAX_SPAWN_HANDLES];
    memcpy(handles, reinterpret_cast<void const*>(handlesAddress), handleCount * sizeof(Abi::Handle));
    for (u64 i = 0; i < handleCount; i++) {
        if (not c.task->handle(handles[i]))
            return c.result(Abi::BAD_HANDLE);
        for (u64 j = 0; j < i; j++) {
            if (handles[j] == handles[i])
                return c.result(Abi::INVALID_ARGUMENT);
        }
    }

    Object* objects[MAX_SPAWN_HANDLES];
    for (u64 i = 0; i < handleCount; i++)
        objects[i] = c.task->takeHandle(handles[i]);

    Abi::Error code = Abi::OK;
    char const* error = nullptr;
    Task* child = Programs::start(name, nameLength, objects, handleCount, code, error);
    if (not child) {
        log("[sched] ", c.task->name, " can't start ", static_cast<char const*>(name), ": ", error);
        return c.result(code);
    }

    log("[sched] ", c.task->name, " started ", child->name, " as task ", child->id,
        " with ", handleCount, " handle(s)");
    return c.result(static_cast<i64>(child->id));
}

Arch::Frame* sysSystemInfo(Call const& c, Boot::Info const& boot) {
    u64 address = c.arg0();
    if (not c.userMemory(address, sizeof(Abi::SystemInfo), true))
        return c.result(Abi::BAD_ADDRESS);

    auto stats = Pmm::stats();
    Abi::SystemInfo info{};
    info.uptimeMs = Time::now();
    info.totalMemory = stats.managedPages * Mem::PAGE_SIZE;
    info.freeMemory = stats.freePages * Mem::PAGE_SIZE;
    info.taskCount = Sched::taskCount();
    if (boot.hasFramebuffer) {
        auto const& fb = boot.framebuffer;
        info.screenWidth = fb.width;
        info.screenHeight = fb.height;
        info.screenPitch = fb.pitch;
        info.redShift = fb.redShift;
        info.greenShift = fb.greenShift;
        info.blueShift = fb.blueShift;
    }
    memcpy(reinterpret_cast<void*>(address), &info, sizeof(info));
    return c.result(Abi::OK);
}

// MARK: Handles and channels

Arch::Frame* sysHandleClose(Call const& c) {
    Object* object = c.task->takeHandle(c.arg0());
    if (not object)
        return c.result(Abi::BAD_HANDLE);
    object->unref();
    return c.result(Abi::OK);
}

Arch::Frame* sysChannelCreate(Call const& c) {
    u64 address = c.arg0();
    if (not c.userMemory(address, 2 * sizeof(Abi::Handle), true))
        return c.result(Abi::BAD_ADDRESS);

    Core::Endpoint *a, *b;
    Core::Endpoint::createPair(a, b);
    u64 ha = c.task->addHandle(a);
    u64 hb = ha ? c.task->addHandle(b) : 0;
    if (not hb) {
        if (ha)
            c.task->takeHandle(ha);
        a->unref();
        b->unref();
        return c.result(Abi::NO_SPACE);
    }

    auto* out = reinterpret_cast<Abi::Handle*>(address);
    out[0] = ha;
    out[1] = hb;
    return c.result(Abi::OK);
}

Arch::Frame* sysChannelSend(Call const& c) {
    i64 error = 0;
    auto* endpoint = c.lookup<Core::Endpoint>(c.arg0(), ObjectType::Channel, error);
    if (not endpoint)
        return c.result(error);

    u64 address = c.arg1();
    u64 length = c.arg2();
    u64 attach = c.arg3();

    if (length > Abi::MAX_MESSAGE_SIZE)
        return c.result(Abi::INVALID_ARGUMENT);
    if (not c.userMemory(address, length))
        return c.result(Abi::BAD_ADDRESS);
    if (attach != Abi::NO_HANDLE and (attach == c.arg0() or not c.task->handle(attach)))
        return c.result(Abi::BAD_HANDLE);

    Core::Endpoint* peer = endpoint->peer;
    if (not peer)
        return c.result(Abi::PEER_CLOSED);
    if (peer->full())
        return c.result(Abi::QUEUE_FULL);

    Core::Message message;
    memcpy(message.data, reinterpret_cast<void const*>(address), length);
    message.size = static_cast<u32>(length);
    message.attached = attach != Abi::NO_HANDLE ? c.task->takeHandle(attach) : nullptr;
    peer->push(message);
    return c.result(Abi::OK);
}

Arch::Frame* sysChannelRecv(Call const& c) {
    i64 error = 0;
    auto* endpoint = c.lookup<Core::Endpoint>(c.arg0(), ObjectType::Channel, error);
    if (not endpoint)
        return c.result(error);

    u64 buffer = c.arg1();
    u64 capacity = c.arg2();
    u64 handleOut = c.arg3();
    u64 flags = c.arg4();

    if (endpoint->empty()) {
        if (not endpoint->peer)
            return c.result(Abi::PEER_CLOSED);
        if (flags & Abi::RECV_NONBLOCK)
            return c.result(Abi::WOULD_BLOCK);
        if (endpoint->waiter and endpoint->waiter != c.task)
            return c.result(Abi::INVALID_ARGUMENT); // one receiver at a time
        endpoint->waiter = c.task;
        return c.blockAndRetry();
    }

    Core::Message& message = endpoint->front();
    if (message.size > capacity)
        return c.result(Abi::BUFFER_TOO_SMALL);
    if (not c.userMemory(buffer, message.size, true))
        return c.result(Abi::BAD_ADDRESS);
    if (handleOut and not c.userMemory(handleOut, sizeof(Abi::Handle), true))
        return c.result(Abi::BAD_ADDRESS);

    // Install an attached handle first: if the table is full, the message
    // stays queued and nothing is lost.
    u64 received = Abi::NO_HANDLE;
    if (message.attached and handleOut) {
        received = c.task->addHandle(message.attached);
        if (not received)
            return c.result(Abi::NO_SPACE);
    } else if (message.attached) {
        message.attached->unref(); // the receiver didn't ask for handles
    }

    memcpy(reinterpret_cast<void*>(buffer), message.data, message.size);
    if (handleOut)
        *reinterpret_cast<Abi::Handle*>(handleOut) = received;

    i64 size = message.size;
    endpoint->pop();
    return c.result(size);
}

// MARK: Memory

Arch::Frame* sysMemoryCreate(Call const& c) {
    auto* memory = Core::SharedMemory::allocate(c.arg0());
    if (not memory)
        return c.result(c.arg0() == 0 or c.arg0() > Core::SharedMemory::MAX_SIZE
                            ? Abi::INVALID_ARGUMENT
                            : Abi::OUT_OF_MEMORY);

    u64 handle = c.task->addHandle(memory);
    if (not handle) {
        memory->unref();
        return c.result(Abi::NO_SPACE);
    }
    return c.result(static_cast<i64>(handle));
}

Arch::Frame* sysMemoryMap(Call const& c) {
    i64 error = 0;
    auto* memory = c.lookup<Core::SharedMemory>(c.arg0(), ObjectType::Memory, error);
    if (not memory)
        return c.result(error);

    Core::Mapping* slot = nullptr;
    for (auto& mapping : c.task->mappings) {
        if (not mapping.object) {
            slot = &mapping;
            break;
        }
    }

    u64 address = c.task->nextMapAddress;
    if (not slot or address + memory->size > MAP_LIMIT)
        return c.result(Abi::NO_SPACE);

    u32 access = Vmm::USER | Vmm::WRITE; // not OWNED: the object owns the pages
    if (not c.task->space->map(address, memory->physical, memory->size, access, memory->cache)) {
        c.task->space->unmap(address, memory->size);
        return c.result(Abi::OUT_OF_MEMORY);
    }

    memory->ref();
    *slot = {memory, address, memory->size};
    c.task->nextMapAddress = address + memory->size + Mem::PAGE_SIZE; // gap between mappings
    return c.result(static_cast<i64>(address));
}

// MARK: Devices

Arch::Frame* sysIrqWait(Call const& c) {
    i64 error = 0;
    auto* irq = c.lookup<Core::Irq>(c.arg0(), ObjectType::Irq, error);
    if (not irq)
        return c.result(error);

    if (irq->pending) {
        irq->pending = false; // the line stays masked until the next wait
        return c.result(Abi::OK);
    }

    // The driver has finished with the last interrupt: let the next one in.
    irq->waiter = c.task;
    Arch::Pic::unmask(irq->line);
    return c.blockAndRetry();
}

Arch::Frame* sysPortIn(Call const& c) {
    i64 error = 0;
    auto* ports = c.lookup<Core::IoPorts>(c.arg0(), ObjectType::IoPorts, error);
    if (not ports)
        return c.result(error);

    u64 port = c.arg1();
    if (port > 0xFFFF or not ports->contains(static_cast<u16>(port)))
        return c.result(Abi::INVALID_ARGUMENT);
    return c.result(Arch::inb(static_cast<u16>(port)));
}

Arch::Frame* sysPortOut(Call const& c) {
    i64 error = 0;
    auto* ports = c.lookup<Core::IoPorts>(c.arg0(), ObjectType::IoPorts, error);
    if (not ports)
        return c.result(error);

    u64 port = c.arg1();
    if (port > 0xFFFF or not ports->contains(static_cast<u16>(port)) or c.arg2() > 0xFF)
        return c.result(Abi::INVALID_ARGUMENT);
    Arch::outb(static_cast<u16>(port), static_cast<u8>(c.arg2()));
    return c.result(Abi::OK);
}

Boot::Info const* bootInfo = nullptr;

} // namespace

void init(Boot::Info const& info) {
    bootInfo = &info;
}

Arch::Frame* dispatch(Arch::Frame* frame) {
    Call c{frame, Sched::current()};
    if (not c.task->isUser())
        return c.result(Abi::UNKNOWN_SYSCALL); // kernel tasks use Sched directly

    switch (frame->rax) {
    case Abi::SYS_LOG: return sysLog(c);
    case Abi::SYS_EXIT: return Sched::exitCurrent(frame, static_cast<i64>(c.arg0()));
    case Abi::SYS_YIELD:
        c.result(Abi::OK);
        return Sched::switchFrom(frame);
    case Abi::SYS_SLEEP:
        c.result(Abi::OK);
        return Sched::sleepCurrent(frame, c.arg0());
    case Abi::SYS_TASK_ID: return c.result(static_cast<i64>(c.task->id));
    case Abi::SYS_TIME: return c.result(static_cast<i64>(Time::now()));
    case Abi::SYS_SPAWN: return sysSpawn(c);
    case Abi::SYS_SYSTEM_INFO: return sysSystemInfo(c, *bootInfo);

    case Abi::SYS_HANDLE_CLOSE: return sysHandleClose(c);
    case Abi::SYS_CHANNEL_CREATE: return sysChannelCreate(c);
    case Abi::SYS_CHANNEL_SEND: return sysChannelSend(c);
    case Abi::SYS_CHANNEL_RECV: return sysChannelRecv(c);

    case Abi::SYS_MEMORY_CREATE: return sysMemoryCreate(c);
    case Abi::SYS_MEMORY_MAP: return sysMemoryMap(c);

    case Abi::SYS_IRQ_WAIT: return sysIrqWait(c);
    case Abi::SYS_PORT_IN: return sysPortIn(c);
    case Abi::SYS_PORT_OUT: return sysPortOut(c);

    default: return c.result(Abi::UNKNOWN_SYSCALL);
    }
}

} // namespace Rudracore::Syscall
