#include "core/programs.h"

#include "core/elf.h"

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

void dropHandles(Core::Object* const* handles, usize count) {
    for (usize i = 0; i < count; i++) {
        if (handles[i])
            handles[i]->unref();
    }
}

} // namespace

void init(Boot::Info const& info) {
    bootInfo = &info;
}

Core::Task* start(char const* name, usize nameLength, Core::Object* const* handles,
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

    Elf::Program program;
    if (not Elf::load(module->data, module->size, program, error)) {
        code = Abi::INVALID_ARGUMENT;
        dropHandles(handles, handleCount);
        return nullptr;
    }

    Core::Task* task = Sched::spawnUser(module->name, program.space, program.entry, program.stackTop,
                                        handles, handleCount);
    if (not task) {
        code = Abi::OUT_OF_MEMORY;
        error = "out of memory";
    }
    return task;
}

} // namespace Rudracore::Programs
