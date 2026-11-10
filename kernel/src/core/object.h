#pragma once

#include "types.h"

namespace Rudracore::Core {

enum class ObjectType : u8 {
    Channel,
    Memory,
    Irq,
    IoPorts,
};

// Base of everything a handle can refer to. Reference counted: each handle,
// mapping, or in-flight message holding the object owns one reference.
// Only touched with interrupts disabled (syscalls and interrupt handlers).
struct Object {
    ObjectType type;
    u32 refs = 1;

    explicit Object(ObjectType t) : type(t) {}
    virtual ~Object() = default;

    Object(Object const&) = delete;
    Object& operator=(Object const&) = delete;

    void ref() { refs++; }

    void unref() {
        if (--refs == 0)
            delete this;
    }
};

} // namespace Rudracore::Core
