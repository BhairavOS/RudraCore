#pragma once

#include "core/object.h"
#include "core/quota.h"
#include "mem/vmm.h"

namespace Rudracore::Core {

// Physically contiguous memory that tasks can map. Either allocated for the
// object (and freed with it), or a device's memory such as the framebuffer
// (never freed).
struct SharedMemory : public Object {
    static constexpr u64 MAX_SIZE = 64 * 1024 * 1024;

    u64 physical = 0;
    u64 size = 0; // page aligned
    bool owned = false;
    bool isScreen = false; // the boot framebuffer: mapping it hands the screen to userspace
    bool readOnly = false; // mapped without write access (boot image files)
    Vmm::Cache cache = Vmm::Cache::WriteBack;
    Quota* charged = nullptr; // the program whose quota pays for it (holds a reference)

    SharedMemory() : Object(ObjectType::Memory) {}
    ~SharedMemory() override;

    // Zeroed memory of at least `size` bytes, or nullptr.
    static SharedMemory* allocate(u64 size);

    // Wraps existing device memory.
    static SharedMemory* device(u64 physical, u64 size, Vmm::Cache cache);
};

} // namespace Rudracore::Core
