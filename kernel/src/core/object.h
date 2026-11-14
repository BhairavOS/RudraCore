#pragma once

#include "core/quota.h"
#include "types.h"

namespace Rudracore::Core {

enum class ObjectType : u8 {
    Channel,
    Memory,
    Irq,
    IoPorts,
    PciDevice,
    Firmware, // ACPI: non-RAM physical memory and PCI configuration (the acpi server)
};

// Base of everything a handle can refer to. Reference counted: each handle,
// mapping, or in-flight message holding the object owns one reference.
// Only touched with interrupts disabled (syscalls and interrupt handlers).
struct Object {
    ObjectType type;
    u32 refs = 1;

    // Bumped by handle_revoke: a capability made under an older epoch no
    // longer works.
    u32 epoch = 0;

    // The program whose quota pays for this object, if any (holds a
    // reference), and how many pages of memory were charged with it.
    Quota* quota = nullptr;
    u64 quotaPages = 0;

    explicit Object(ObjectType t) : type(t) {}

    virtual ~Object() {
        if (quota) {
            quota->refund(quotaPages);
            quota->refundObjects(1);
            quota->unref();
        }
    }

    Object(Object const&) = delete;
    Object& operator=(Object const&) = delete;

    void ref() {
        if (refs == ~0u) // a wrap to 0 would free a live object
            __builtin_trap();
        refs++;
    }

    void unref() {
        if (--refs == 0)
            delete this;
    }

    // Charges this object (and `pages` of memory already charged by the
    // caller) to `q`, refunded when the object is destroyed.
    void chargeTo(Quota* q, u64 pages) {
        q->ref();
        quota = q;
        quotaPages = pages;
    }
};

// A handle's worth of access, also how one travels (in a message or to a
// new program): the object (holding a reference), what it allows
// (Abi::Rights), and the object's epoch when it was made.
struct Capability {
    Object* object = nullptr;
    u32 rights = 0;
    u32 epoch = 0;

    bool revoked() const { return object and epoch != object->epoch; }
};

} // namespace Rudracore::Core
