#pragma once

#include "arch/x86_64/pci.h"
#include "boot.h"

// Intel VT-d DMA remapping: a device can only read or write the memory its
// driver registered for it (dma_address), not all of RAM. Every device
// starts with nothing; a driver that gets a buffer's address also gets that
// buffer, and only that buffer, mapped for its device. Without VT-d
// hardware (no ACPI DMAR table) DMA stays unconfined, and the log says so.
namespace Rudracore::Arch::Iommu {

// Finds the remapping units and turns translation on, all devices blocked.
void init(Boot::Info const& info);

bool enabled();

// Lets the device at `device` read and write [physical, physical + size)
// at the same addresses. False if out of memory.
bool allow(Pci::Address device, u64 physical, u64 size);

// Takes [physical, physical + size) back from every device (before the
// memory is freed and reused).
void revoke(u64 physical, u64 size);

// Logs DMA the hardware blocked since the last call.
void reportFaults();

} // namespace Rudracore::Arch::Iommu
