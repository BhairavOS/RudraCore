#include "core/programs.h"

#include "core/elf.h"
#include "mem/memory.h"

extern "C" void* memcpy(void* dest, void const* src, unsigned long n);

namespace Rudracore::Programs {

namespace {

Boot::Info const* bootInfo = nullptr;

bool nameEquals(char const* a, char const* b, usize bLength) {
    usize i = 0;
    for (; i < bLength; i++) {
        if (a[i] != b[i] or a[i] == '\0')
            return false;
    }
    return a[i] == '\0';
}

void dropHandles(Core::Capability const* handles, usize count) {
    for (usize i = 0; i < count; i++) {
        if (handles[i].object)
            handles[i].object->unref();
    }
}

} // namespace

void init(Boot::Info const& info) {
    bootInfo = &info;
}

Core::SharedMemory* file(char const* name) {
    static Core::SharedMemory* cache[Boot::Info::MAX_MODULES] = {};
    for (usize i = 0; bootInfo and i < bootInfo->moduleCount; i++) {
        Boot::Module const& module = bootInfo->modules[i];
        usize n = 0;
        while (name[n])
            n++;
        if (not nameEquals(module.name, name, n))
            continue;
        if (not cache[i]) {
            Core::SharedMemory* memory = Core::SharedMemory::allocate(module.size);
            if (not memory)
                return nullptr;
            memcpy(Mem::toVirtual<u8>(memory->physical), module.data, module.size);
            memory->readOnly = true;
            cache[i] = memory; // the cache keeps this first reference
        }
        return cache[i];
    }
    return nullptr;
}

bool exists(char const* name, usize nameLength) {
    for (usize i = 0; bootInfo and i < bootInfo->moduleCount; i++) {
        if (nameEquals(bootInfo->modules[i].name, name, nameLength))
            return true;
    }
    return false;
}

Core::Task* startImage(char const* name, u8 const* data, usize size, Core::Capability const* handles,
                       usize handleCount, Abi::Error& code, char const*& error) {
    Elf::Program program;
    if (not Elf::load(data, size, program, error)) {
        code = error == Elf::OUT_OF_MEMORY ? Abi::OUT_OF_MEMORY : Abi::INVALID_ARGUMENT;
        dropHandles(handles, handleCount);
        return nullptr;
    }
    Core::Task* task = Sched::spawnUser(name, program.space, program.entry, program.stackTop, handles, handleCount);
    if (not task) {
        code = Abi::OUT_OF_MEMORY;
        error = "out of memory";
    }
    return task;
}

Core::Task* start(char const* name, usize nameLength, Core::Capability const* handles,
                  usize handleCount, Abi::Error& code, char const*& error) {
    Boot::Module const* module = nullptr;
    for (usize i = 0; bootInfo and i < bootInfo->moduleCount; i++) {
        if (nameEquals(bootInfo->modules[i].name, name, nameLength)) {
            module = &bootInfo->modules[i];
            break;
        }
    }
    if (not module) {
        code = Abi::NOT_FOUND;
        error = "no such program";
        dropHandles(handles, handleCount);
        return nullptr;
    }
    return startImage(module->name, module->data, module->size, handles, handleCount, code, error);
}

} // namespace Rudracore::Programs
