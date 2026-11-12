#include "core/syscall.h"

#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/protect.h"
#include "core/channel.h"
#include "core/device.h"
#include "core/programs.h"
#include "core/random.h"
#include "core/sched.h"
#include "core/shared_memory.h"
#include "core/time.h"
#include "display.h"
#include "log.h"
#include "mem/memory.h"
#include "mem/pmm.h"
#include "rudracore/syscalls.h"

extern "C" void* memcpy(void* dest, void const* src, usize n);
extern "C" void* memset(void* dest, int value, usize n);

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

// Longest wait a program may ask for (about 34 years): keeps now + timeout
// from wrapping around.
constexpr u64 MAX_TIMEOUT_MS = 1ull << 40;

// The only ways the kernel touches user memory. The range must already be
// checked (Call::userMemory). With SMAP, user pages are reachable only here.
void fromUser(void* to, u64 from, u64 length) {
    Arch::Protect::UserAccess open;
    memcpy(to, reinterpret_cast<void const*>(from), length);
}

void toUser(u64 to, void const* from, u64 length) {
    Arch::Protect::UserAccess open;
    memcpy(reinterpret_cast<void*>(to), from, length);
}

// Text from a program that goes into the kernel log: control characters
// (newlines, terminal escapes) would let it forge log lines.
void makePrintable(char* text, u64 length) {
    for (u64 i = 0; i < length; i++) {
        if (static_cast<u8>(text[i]) < 0x20 or text[i] == 0x7F)
            text[i] = '?';
    }
}

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

    // A live handle (open, not revoked) that has all of `rights`;
    // otherwise sets `error`.
    Core::Capability* capability(u64 handle, u32 rights, i64& error) const {
        Core::Capability* cap = task->handle(handle);
        if (not cap) {
            error = Abi::BAD_HANDLE;
            return nullptr;
        }
        if (cap->revoked()) {
            error = Abi::REVOKED;
            return nullptr;
        }
        if ((cap->rights & rights) != rights) {
            error = Abi::ACCESS_DENIED;
            return nullptr;
        }
        return cap;
    }

    // The object behind a live handle of the right type with all of
    // `rights`; otherwise sets `error`.
    template <typename T>
    T* lookup(u64 handle, ObjectType type, u32 rights, i64& error) const {
        Core::Capability* cap = capability(handle, 0, error);
        if (not cap)
            return nullptr;
        if (cap->object->type != type) {
            error = Abi::WRONG_TYPE;
            return nullptr;
        }
        if ((cap->rights & rights) != rights) {
            error = Abi::ACCESS_DENIED;
            return nullptr;
        }
        return static_cast<T*>(cap->object);
    }

    // Checks handles about to be moved to a new program: live, no
    // duplicates, none equal to `except`, and with RIGHT_TRANSFER if asked.
    i64 checkMovable(Abi::Handle const* handles, u64 count, bool needTransfer, Abi::Handle except) const {
        for (u64 i = 0; i < count; i++) {
            i64 error = 0;
            if (handles[i] == except)
                return Abi::BAD_HANDLE;
            if (not capability(handles[i], needTransfer ? Abi::RIGHT_TRANSFER : 0, error))
                return error;
            for (u64 j = 0; j < i; j++) {
                if (handles[j] == handles[i])
                    return Abi::INVALID_ARGUMENT;
            }
        }
        return Abi::OK;
    }
};

// A running task waits on nothing: clears it as the waiter of any channel
// it holds (a registration left behind when a wait ended early).
void forgetWaits(Task* task) {
    for (auto& slot : task->handles) {
        Object* object = slot.cap.object;
        if (object and object->type == ObjectType::Channel and static_cast<Core::Endpoint*>(object)->waiter == task)
            static_cast<Core::Endpoint*>(object)->waiter = nullptr;
    }
}

// Rights a new object of each kind starts with, in the handle that made it.
constexpr u32 CHANNEL_RIGHTS = Abi::RIGHT_READ | Abi::RIGHT_WRITE | Abi::RIGHT_TRANSFER | Abi::RIGHT_DUPLICATE |
                               Abi::RIGHT_REVOKE;
constexpr u32 MEMORY_RIGHTS = CHANNEL_RIGHTS;
// Boot files are one object shared by every program that opens them: no
// writing, and nobody may revoke everyone else's copy.
constexpr u32 FILE_RIGHTS = Abi::RIGHT_READ | Abi::RIGHT_TRANSFER | Abi::RIGHT_DUPLICATE;
// A device's registers stay with the driver that mapped them.
constexpr u32 REGISTER_RIGHTS = Abi::RIGHT_READ | Abi::RIGHT_WRITE;

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

    char text[MAX_LOG_LENGTH];
    fromUser(text, address, length);
    // Drop a trailing newline; log() adds its own.
    if (length > 0 and text[length - 1] == '\n')
        length--;
    makePrintable(text, length);

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

    fromUser(name, nameAddress, nameLength);
    name[nameLength] = '\0';

    // No such program: fail before taking any handle, so the caller still
    // has them (e.g. to start an installed app instead).
    if (not Programs::exists(name, nameLength))
        return c.result(Abi::NOT_FOUND);

    // Check every handle before moving any, so a bad one changes nothing.
    // Boot-image programs are part of the system: RIGHT_TRANSFER isn't needed.
    Abi::Handle handles[MAX_SPAWN_HANDLES];
    fromUser(handles, handlesAddress, handleCount * sizeof(Abi::Handle));
    if (i64 bad = c.checkMovable(handles, handleCount, false, Abi::NO_HANDLE))
        return c.result(bad);

    Core::Capability objects[MAX_SPAWN_HANDLES];
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

Arch::Frame* sysSpawnImage(Call const& c) {
    Abi::SpawnImage request;
    if (not c.userMemory(c.arg0(), sizeof(request)))
        return c.result(Abi::BAD_ADDRESS);
    fromUser(&request, c.arg0(), sizeof(request));
    request.name[sizeof(request.name) - 1] = '\0';
    u64 nameLength = 0;
    while (request.name[nameLength])
        nameLength++;
    makePrintable(request.name, nameLength);
    if (request.handleCount > MAX_SPAWN_HANDLES or request.name[0] == '\0')
        return c.result(Abi::INVALID_ARGUMENT);

    i64 error = 0;
    auto* memory = c.lookup<Core::SharedMemory>(request.memory, ObjectType::Memory, Abi::RIGHT_READ, error);
    if (not memory)
        return c.result(error);
    if (not memory->owned or request.size == 0 or request.size > memory->size)
        return c.result(Abi::INVALID_ARGUMENT);

    // Check every handle before moving any, so a bad one changes nothing.
    // An installed app is untrusted code: each handle needs RIGHT_TRANSFER.
    if (i64 bad = c.checkMovable(request.handles, request.handleCount, true, request.memory))
        return c.result(bad);
    Core::Capability objects[MAX_SPAWN_HANDLES];
    for (u64 i = 0; i < request.handleCount; i++)
        objects[i] = c.task->takeHandle(request.handles[i]);

    Abi::Error code = Abi::OK;
    char const* why = nullptr;
    Task* child = Programs::startImage(request.name, Mem::toVirtual<u8>(memory->physical), request.size, objects,
                                       request.handleCount, code, why);
    if (not child) {
        log("[sched] ", c.task->name, " can't start the app ", static_cast<char const*>(request.name), ": ", why);
        return c.result(code);
    }
    log("[sched] ", c.task->name, " started the app ", child->name, " as task ", child->id, " with ",
        request.handleCount, " handle(s)");
    return c.result(static_cast<i64>(child->id));
}

Arch::Frame* sysSystemInfo(Call const& c, Boot::Info const& boot) {
    u64 address = c.arg0();
    if (not c.userMemory(address, sizeof(Abi::SystemInfo), true))
        return c.result(Abi::BAD_ADDRESS);

    auto stats = Pmm::stats();
    Abi::SystemInfo info;
    memset(&info, 0, sizeof(info)); // padding too: no stale kernel stack bytes reach the program
    info.uptimeMs = Time::now();
    info.totalMemory = stats.managedPages * Mem::PAGE_SIZE;
    info.freeMemory = stats.freePages * Mem::PAGE_SIZE;
    info.taskCount = Sched::taskCount();
    info.timeOfDay = Time::timeOfDay();
    info.unixTime = Time::unixTime();
    if (boot.hasFramebuffer) {
        auto const& fb = boot.framebuffer;
        info.screenWidth = fb.width;
        info.screenHeight = fb.height;
        info.screenPitch = fb.pitch;
        info.redShift = fb.redShift;
        info.greenShift = fb.greenShift;
        info.blueShift = fb.blueShift;
    }
    toUser(address, &info, sizeof(info));
    return c.result(Abi::OK);
}

Arch::Frame* sysRandom(Call const& c) {
    u64 address = c.arg0(), length = c.arg1();
    if (length > 256)
        return c.result(Abi::INVALID_ARGUMENT);
    if (not c.userMemory(address, length, true))
        return c.result(Abi::BAD_ADDRESS);
    u8 bytes[256];
    Random::fill(bytes, length);
    toUser(address, bytes, length);
    memset(bytes, 0, sizeof(bytes)); // no copy stays behind on the kernel stack
    asm volatile("" : : "r"(bytes) : "memory");
    return c.result(Abi::OK);
}

// A data file from the boot image, shared read-only by everyone who opens it.
Arch::Frame* sysFileOpen(Call const& c) {
    char name[32];
    u64 length = c.arg1();
    if (length == 0 or length >= sizeof(name))
        return c.result(Abi::INVALID_ARGUMENT);
    if (not c.userMemory(c.arg0(), length))
        return c.result(Abi::BAD_ADDRESS);
    fromUser(name, c.arg0(), length);
    name[length] = '\0';
    Core::SharedMemory* memory = Programs::file(name);
    if (not memory)
        return c.result(Abi::NOT_FOUND);
    memory->ref();
    u64 handle = c.task->addHandle({memory, FILE_RIGHTS, memory->epoch});
    if (not handle) {
        memory->unref();
        return c.result(Abi::NO_SPACE);
    }
    return c.result(static_cast<i64>(handle));
}

// MARK: Handles and channels

Arch::Frame* sysHandleClose(Call const& c) {
    Object* object = c.task->takeHandle(c.arg0()).object;
    if (not object)
        return c.result(Abi::BAD_HANDLE);
    // The endpoint may live on (other handles): it must not keep naming this task.
    if (object->type == ObjectType::Channel and static_cast<Core::Endpoint*>(object)->waiter == c.task)
        static_cast<Core::Endpoint*>(object)->waiter = nullptr;
    object->unref();
    return c.result(Abi::OK);
}

Arch::Frame* sysHandleDuplicate(Call const& c) {
    i64 error = 0;
    Core::Capability* cap = c.capability(c.arg0(), Abi::RIGHT_DUPLICATE, error);
    if (not cap)
        return c.result(error);
    Core::Capability copy{cap->object, cap->rights & static_cast<u32>(c.arg1()), cap->epoch};
    copy.object->ref();
    u64 handle = c.task->addHandle(copy);
    if (not handle) {
        copy.object->unref();
        return c.result(Abi::NO_SPACE);
    }
    return c.result(static_cast<i64>(handle));
}

Arch::Frame* sysHandleRestrict(Call const& c) {
    i64 error = 0;
    Core::Capability* cap = c.capability(c.arg0(), 0, error);
    if (not cap)
        return c.result(error);
    cap->rights &= static_cast<u32>(c.arg1());
    return c.result(Abi::OK);
}

Arch::Frame* sysHandleRights(Call const& c) {
    i64 error = 0;
    Core::Capability* cap = c.capability(c.arg0(), 0, error);
    if (not cap)
        return c.result(error);
    return c.result(cap->rights);
}

// Cuts off every other handle to the object: their epoch no longer matches.
// Then removes what they already got out of it: other programs' mappings of
// revoked memory, and tasks blocked on it are woken (to see REVOKED).
Arch::Frame* sysHandleRevoke(Call const& c) {
    i64 error = 0;
    Core::Capability* cap = c.capability(c.arg0(), Abi::RIGHT_REVOKE, error);
    if (not cap)
        return c.result(error);
    Object* object = cap->object;
    object->epoch++;
    cap->epoch = object->epoch;

    switch (object->type) {
    case ObjectType::Channel:
        static_cast<Core::Endpoint*>(object)->wakeWaiter();
        break;
    case ObjectType::Irq:
        if (Task* waiter = static_cast<Core::Irq*>(object)->waiter) {
            static_cast<Core::Irq*>(object)->waiter = nullptr;
            Sched::wake(waiter);
        }
        break;
    case ObjectType::Memory:
        for (Task* t = Sched::allTasks(); t; t = t->allNext) {
            if (t == c.task or not t->space)
                continue;
            for (auto& mapping : t->mappings) {
                if (mapping.object == object) {
                    t->space->unmap(mapping.address, mapping.size);
                    object->unref();
                    mapping = {};
                }
            }
        }
        break;
    case ObjectType::IoPorts:
    case ObjectType::PciDevice:
        break;
    }
    return c.result(Abi::OK);
}

Arch::Frame* sysChannelCreate(Call const& c) {
    u64 address = c.arg0();
    if (not c.userMemory(address, 2 * sizeof(Abi::Handle), true))
        return c.result(Abi::BAD_ADDRESS);
    // Two endpoints, each a little over one page of kernel heap.
    if (not Pmm::userCanUse(2 * (sizeof(Core::Endpoint) / Mem::PAGE_SIZE + 1)))
        return c.result(Abi::OUT_OF_MEMORY);
    if (not c.task->quota->chargeObjects(2))
        return c.result(Abi::OUT_OF_MEMORY);

    Core::Endpoint *a, *b;
    Core::Endpoint::createPair(a, b);
    a->chargeTo(c.task->quota, 0);
    b->chargeTo(c.task->quota, 0);
    u64 ha = c.task->addHandle({a, CHANNEL_RIGHTS, 0});
    u64 hb = ha ? c.task->addHandle({b, CHANNEL_RIGHTS, 0}) : 0;
    if (not hb) {
        if (ha)
            c.task->takeHandle(ha);
        a->unref();
        b->unref();
        return c.result(Abi::NO_SPACE);
    }

    Abi::Handle out[2] = {ha, hb};
    toUser(address, out, sizeof(out));
    return c.result(Abi::OK);
}

Arch::Frame* sysChannelSend(Call const& c) {
    i64 error = 0;
    auto* endpoint = c.lookup<Core::Endpoint>(c.arg0(), ObjectType::Channel, Abi::RIGHT_WRITE, error);
    if (not endpoint)
        return c.result(error);

    u64 address = c.arg1();
    u64 length = c.arg2();
    u64 attach = c.arg3();

    if (length > Abi::MAX_MESSAGE_SIZE)
        return c.result(Abi::INVALID_ARGUMENT);
    if (not c.userMemory(address, length))
        return c.result(Abi::BAD_ADDRESS);
    if (attach == c.arg0())
        return c.result(Abi::BAD_HANDLE);
    // The attached handle travels with its rights, so it must be live and
    // allowed to move.
    if (attach != Abi::NO_HANDLE and not c.capability(attach, Abi::RIGHT_TRANSFER, error))
        return c.result(error);

    Core::Endpoint* peer = endpoint->peer;
    if (not peer)
        return c.result(Abi::PEER_CLOSED);
    if (peer->full())
        return c.result(Abi::QUEUE_FULL);

    // Objects in a queue hold references, so a cycle of them (an endpoint in
    // its own queue, two endpoints in each other's) could never be freed.
    // Refuse every way to start one: sending the receiving endpoint into its
    // own queue, an endpoint that already holds channels or IRQs in its
    // queue, or an IRQ bound to a channel.
    if (Object* object = attach != Abi::NO_HANDLE ? c.task->handle(attach)->object : nullptr) {
        if (object == peer)
            return c.result(Abi::INVALID_ARGUMENT);
        if (object->type == ObjectType::Channel and static_cast<Core::Endpoint*>(object)->holdsLinks())
            return c.result(Abi::INVALID_ARGUMENT);
        if (object->type == ObjectType::Irq and static_cast<Core::Irq*>(object)->bound)
            return c.result(Abi::INVALID_ARGUMENT);
    }

    Core::Message message;
    fromUser(message.data, address, length);
    message.size = static_cast<u32>(length);
    message.attached = attach != Abi::NO_HANDLE ? c.task->takeHandle(attach) : Core::Capability{};
    peer->push(message);
    return c.result(Abi::OK);
}

Arch::Frame* sysChannelRecv(Call const& c) {
    i64 error = 0;
    auto* endpoint = c.lookup<Core::Endpoint>(c.arg0(), ObjectType::Channel, Abi::RIGHT_READ, error);
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
    // A handle revoked on the way is dropped, like one the receiver didn't
    // ask for.
    u64 received = Abi::NO_HANDLE;
    Core::Capability& attached = message.attached;
    if (attached.object and handleOut and not attached.revoked()) {
        received = c.task->addHandle(attached);
        if (not received)
            return c.result(Abi::NO_SPACE);
    } else if (attached.object) {
        attached.object->unref();
    }

    toUser(buffer, message.data, message.size);
    if (handleOut)
        toUser(handleOut, &received, sizeof(received));

    i64 size = message.size;
    endpoint->pop();
    return c.result(size);
}

// Waits for any of several endpoints. Like recv, a wait that blocks runs
// again from the start when woken; task->waitDeadline remembers the
// deadline across those runs (and is cleared whenever the call returns).
Arch::Frame* sysWait(Call const& c) {
    u64 address = c.arg0();
    u64 count = c.arg1();
    u64 timeout = c.arg2();
    Task* task = c.task;

    auto finish = [&](i64 result, Core::Endpoint* const* endpoints, u64 n) {
        for (u64 i = 0; i < n; i++) {
            if (endpoints[i]->waiter == task)
                endpoints[i]->waiter = nullptr;
        }
        task->waitDeadline = 0;
        return c.result(result);
    };

    if (count == 0 or count > Abi::MAX_WAIT)
        return finish(Abi::INVALID_ARGUMENT, nullptr, 0);
    if (not c.userMemory(address, count * sizeof(Abi::Handle)))
        return finish(Abi::BAD_ADDRESS, nullptr, 0);

    Abi::Handle handles[Abi::MAX_WAIT];
    fromUser(handles, address, count * sizeof(Abi::Handle));
    if (timeout != Abi::WAIT_FOREVER and timeout > MAX_TIMEOUT_MS)
        timeout = MAX_TIMEOUT_MS;

    Core::Endpoint* endpoints[Abi::MAX_WAIT];
    for (u64 i = 0; i < count; i++) {
        i64 error = 0;
        endpoints[i] = c.lookup<Core::Endpoint>(handles[i], ObjectType::Channel, Abi::RIGHT_READ, error);
        if (not endpoints[i]) {
            // A handle revoked while this task waited: endpoints after it
            // may still name the task as their waiter.
            forgetWaits(task);
            return finish(error, endpoints, i);
        }
        if (endpoints[i]->waiter and endpoints[i]->waiter != task)
            return finish(Abi::INVALID_ARGUMENT, endpoints, i); // one waiter per endpoint
    }

    for (u64 i = 0; i < count; i++) {
        if (not endpoints[i]->empty() or not endpoints[i]->peer)
            return finish(static_cast<i64>(i), endpoints, count);
    }

    u64 now = Time::now();
    if (timeout == 0 or (task->waitDeadline and now >= task->waitDeadline))
        return finish(Abi::TIMED_OUT, endpoints, count);

    for (u64 i = 0; i < count; i++)
        endpoints[i]->waiter = task;

    c.frame->rip -= INT80_LENGTH; // run the call again when woken
    if (timeout == Abi::WAIT_FOREVER)
        return Sched::blockCurrent(c.frame);
    if (not task->waitDeadline)
        task->waitDeadline = now + timeout;
    return Sched::blockCurrentUntil(c.frame, task->waitDeadline);
}

// MARK: Memory

Arch::Frame* sysMemoryUnmap(Call const& c) {
    for (auto& mapping : c.task->mappings) {
        if (mapping.object and mapping.address == c.arg0()) {
            c.task->space->unmap(mapping.address, mapping.size);
            mapping.object->unref();
            mapping = {};
            return c.result(Abi::OK);
        }
    }
    return c.result(Abi::NOT_FOUND);
}

Arch::Frame* sysMemoryCreate(Call const& c) {
    if (c.arg0() == 0 or c.arg0() > Core::SharedMemory::MAX_SIZE)
        return c.result(Abi::INVALID_ARGUMENT);
    u64 pages = Mem::alignUp(c.arg0(), Mem::PAGE_SIZE) / Mem::PAGE_SIZE;
    Core::Quota& quota = *c.task->quota;
    if (not quota.chargeObjects(1))
        return c.result(Abi::OUT_OF_MEMORY);
    if (not quota.charge(pages)) {
        quota.refundObjects(1);
        return c.result(Abi::OUT_OF_MEMORY);
    }
    auto* memory = Core::SharedMemory::allocate(c.arg0());
    if (not memory) {
        quota.refund(pages);
        quota.refundObjects(1);
        return c.result(Abi::OUT_OF_MEMORY);
    }
    memory->chargeTo(&quota, pages);

    u64 handle = c.task->addHandle({memory, MEMORY_RIGHTS, 0});
    if (not handle) {
        memory->unref();
        return c.result(Abi::NO_SPACE);
    }
    return c.result(static_cast<i64>(handle));
}

Arch::Frame* sysMemoryMap(Call const& c) {
    i64 error = 0;
    auto* memory = c.lookup<Core::SharedMemory>(c.arg0(), ObjectType::Memory, Abi::RIGHT_READ, error);
    if (not memory)
        return c.result(error);
    bool writable = not memory->readOnly and (c.task->handle(c.arg0())->rights & Abi::RIGHT_WRITE);

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

    u32 access = Vmm::USER | (writable ? Vmm::WRITE : 0); // not OWNED: the object owns the pages
    if (not c.task->space->map(address, memory->physical, memory->size, access, memory->cache)) {
        c.task->space->unmap(address, memory->size);
        return c.result(Abi::OUT_OF_MEMORY);
    }

    memory->ref();
    *slot = {memory, address, memory->size};
    c.task->nextMapAddress = address + memory->size + Mem::PAGE_SIZE; // gap between mappings

    if (memory->isScreen)
        Display::handOverToUserspace();
    return c.result(static_cast<i64>(address));
}

// Private memory: fresh zeroed pages owned by the task's address space
// (freed when the task ends). Pages needn't be physically contiguous.
Arch::Frame* sysMemoryAlloc(Call const& c) {
    constexpr u64 MAX_ALLOC = 256 * 1024 * 1024;
    u64 size = Mem::alignUp(c.arg0(), Mem::PAGE_SIZE);
    if (size == 0 or size > MAX_ALLOC)
        return c.result(Abi::INVALID_ARGUMENT);

    u64 address = c.task->nextMapAddress;
    if (address + size > MAP_LIMIT)
        return c.result(Abi::NO_SPACE);
    u64 pages = size / Mem::PAGE_SIZE;
    if (not Pmm::userCanUse(pages + pages / 512 + 3)) // + page tables
        return c.result(Abi::OUT_OF_MEMORY);
    if (not c.task->quota->charge(pages))
        return c.result(Abi::OUT_OF_MEMORY);

    auto& space = *c.task->space;
    for (u64 offset = 0; offset < size; offset += Mem::PAGE_SIZE) {
        u64 page = Pmm::allocZeroed();
        if (not page or not space.map(address + offset, page, Mem::PAGE_SIZE,
                                      Vmm::USER | Vmm::WRITE | Vmm::OWNED)) {
            // Give back what was mapped so far.
            if (page)
                Pmm::free(page);
            for (u64 undo = 0; undo < offset; undo += Mem::PAGE_SIZE) {
                u64 physical;
                if (space.translate(address + undo, physical))
                    Pmm::free(physical);
            }
            space.unmap(address, offset);
            c.task->quota->refund(pages);
            return c.result(Abi::OUT_OF_MEMORY);
        }
    }

    c.task->nextMapAddress = address + size + Mem::PAGE_SIZE;
    return c.result(static_cast<i64>(address));
}

// MARK: Devices

Arch::Frame* sysIrqWait(Call const& c) {
    i64 error = 0;
    auto* irq = c.lookup<Core::Irq>(c.arg0(), ObjectType::Irq, Abi::RIGHT_READ, error);
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

Arch::Frame* sysIrqBind(Call const& c) {
    i64 error = 0;
    auto* irq = c.lookup<Core::Irq>(c.arg0(), ObjectType::Irq, Abi::RIGHT_READ, error);
    if (not irq)
        return c.result(error);
    auto* endpoint = c.lookup<Core::Endpoint>(c.arg1(), ObjectType::Channel, Abi::RIGHT_WRITE, error);
    if (not endpoint)
        return c.result(error);

    irq->bind(endpoint, c.arg2());
    return c.result(Abi::OK);
}

Arch::Frame* sysIrqAck(Call const& c) {
    i64 error = 0;
    auto* irq = c.lookup<Core::Irq>(c.arg0(), ObjectType::Irq, Abi::RIGHT_READ, error);
    if (not irq)
        return c.result(error);
    irq->acknowledge();
    return c.result(Abi::OK);
}

Arch::Frame* sysPortIn(Call const& c) {
    i64 error = 0;
    auto* ports = c.lookup<Core::IoPorts>(c.arg0(), ObjectType::IoPorts, Abi::RIGHT_READ, error);
    if (not ports)
        return c.result(error);

    u64 port = c.arg1();
    if (port > 0xFFFF or not ports->contains(static_cast<u16>(port)))
        return c.result(Abi::INVALID_ARGUMENT);
    return c.result(Arch::inb(static_cast<u16>(port)));
}

Arch::Frame* sysPortOut(Call const& c) {
    i64 error = 0;
    auto* ports = c.lookup<Core::IoPorts>(c.arg0(), ObjectType::IoPorts, Abi::RIGHT_WRITE, error);
    if (not ports)
        return c.result(error);

    u64 port = c.arg1();
    if (port > 0xFFFF or not ports->contains(static_cast<u16>(port)) or c.arg2() > 0xFF)
        return c.result(Abi::INVALID_ARGUMENT);
    Arch::outb(static_cast<u16>(port), static_cast<u8>(c.arg2()));
    return c.result(Abi::OK);
}

Arch::Frame* sysDeviceInfo(Call const& c) {
    i64 error = 0;
    auto* device = c.lookup<Core::PciDevice>(c.arg0(), ObjectType::PciDevice, Abi::RIGHT_READ, error);
    if (not device)
        return c.result(error);
    if (not c.userMemory(c.arg1(), sizeof(Abi::DeviceInfo), true))
        return c.result(Abi::BAD_ADDRESS);

    Abi::DeviceInfo info{};
    info.vendor = device->vendor;
    info.device = device->device;
    info.classCode = device->classCode;
    info.subclass = device->subclass;
    info.progIf = device->progIf;
    info.irqLine = device->irqLine;
    for (int i = 0; i < 6; i++)
        info.barSize[i] = device->bars[i].io ? 0 : device->bars[i].size;
    toUser(c.arg1(), &info, sizeof(info));
    return c.result(Abi::OK);
}

Arch::Frame* sysDeviceBar(Call const& c) {
    i64 error = 0;
    auto* device = c.lookup<Core::PciDevice>(c.arg0(), ObjectType::PciDevice, Abi::RIGHT_MMIO, error);
    if (not device)
        return c.result(error);
    u64 index = c.arg1();
    if (index > 5 or device->bars[index].io or not device->bars[index].size or
        device->bars[index].size > Core::SharedMemory::MAX_SIZE)
        return c.result(Abi::INVALID_ARGUMENT);

    auto const& bar = device->bars[index];
    auto* memory = Core::SharedMemory::device(bar.address, bar.size, Vmm::Cache::Uncached);
    u64 handle = c.task->addHandle({memory, REGISTER_RIGHTS, 0});
    if (not handle) {
        memory->unref();
        return c.result(Abi::NO_SPACE);
    }
    device->enable();
    return c.result(static_cast<i64>(handle));
}

Arch::Frame* sysDmaAddress(Call const& c) {
    i64 error = 0;
    if (not c.lookup<Core::PciDevice>(c.arg0(), ObjectType::PciDevice, Abi::RIGHT_DMA, error))
        return c.result(error);
    // The device will write it: the driver must be allowed to write it too.
    auto* memory = c.lookup<Core::SharedMemory>(c.arg1(), ObjectType::Memory, Abi::RIGHT_WRITE, error);
    if (not memory)
        return c.result(error);
    if (not memory->owned)
        return c.result(Abi::INVALID_ARGUMENT); // device memory, not RAM
    return c.result(static_cast<i64>(memory->physical));
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
        return Sched::sleepCurrent(frame, min(c.arg0(), MAX_TIMEOUT_MS));
    case Abi::SYS_TASK_ID: return c.result(static_cast<i64>(c.task->id));
    case Abi::SYS_TIME: return c.result(static_cast<i64>(Time::now()));
    case Abi::SYS_SPAWN: return sysSpawn(c);
    case Abi::SYS_SPAWN_IMAGE: return sysSpawnImage(c);
    case Abi::SYS_SYSTEM_INFO: return sysSystemInfo(c, *bootInfo);
    case Abi::SYS_RANDOM: return sysRandom(c);
    case Abi::SYS_FILE_OPEN: return sysFileOpen(c);

    case Abi::SYS_HANDLE_CLOSE: return sysHandleClose(c);
    case Abi::SYS_CHANNEL_CREATE: return sysChannelCreate(c);
    case Abi::SYS_CHANNEL_SEND: return sysChannelSend(c);
    case Abi::SYS_CHANNEL_RECV: return sysChannelRecv(c);
    case Abi::SYS_WAIT: return sysWait(c);
    case Abi::SYS_HANDLE_DUPLICATE: return sysHandleDuplicate(c);
    case Abi::SYS_HANDLE_RESTRICT: return sysHandleRestrict(c);
    case Abi::SYS_HANDLE_REVOKE: return sysHandleRevoke(c);
    case Abi::SYS_HANDLE_RIGHTS: return sysHandleRights(c);

    case Abi::SYS_MEMORY_CREATE: return sysMemoryCreate(c);
    case Abi::SYS_MEMORY_MAP: return sysMemoryMap(c);
    case Abi::SYS_MEMORY_ALLOC: return sysMemoryAlloc(c);
    case Abi::SYS_MEMORY_UNMAP: return sysMemoryUnmap(c);

    case Abi::SYS_IRQ_WAIT: return sysIrqWait(c);
    case Abi::SYS_PORT_IN: return sysPortIn(c);
    case Abi::SYS_PORT_OUT: return sysPortOut(c);
    case Abi::SYS_IRQ_BIND: return sysIrqBind(c);
    case Abi::SYS_IRQ_ACK: return sysIrqAck(c);
    case Abi::SYS_DEVICE_INFO: return sysDeviceInfo(c);
    case Abi::SYS_DEVICE_BAR: return sysDeviceBar(c);
    case Abi::SYS_DMA_ADDRESS: return sysDmaAddress(c);

    default: return c.result(Abi::UNKNOWN_SYSCALL);
    }
}

} // namespace Rudracore::Syscall
