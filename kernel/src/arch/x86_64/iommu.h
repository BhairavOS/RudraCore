#pragma once

#include "arch/x86_64/pci.h"
#include "boot.h"

// Intel VT-d DMA remapping: a device can only read or write the memory its
// driver registered for it (dma_address), not all of RAM. Every device
// starts with nothing; a driver that gets a buffer's address also gets that
// buffer, and only that buffer, mapped for its device. Without VT-d
// hardware (no ACPI DMAR table) DMA stays unconfined, and the log says so.
//
// Interrupt remapping, where every unit supports it: devices may not send
// interrupt messages (MSI) at all, since every interrupt here comes through
// the legacy PIC. That stops a device from raising arbitrary vectors, NMIs
// or SMIs by writing to 0xFEExxxxx.
namespace Rudracore::Arch::Iommu {

// Finds the remapping units and turns translation on, all devices blocked.
void init(Boot::Info const& info);

bool enabled();

// Whether interrupt remapping is on (then I/O APIC pins need entries).
bool remapsInterrupts();

// Lets the I/O APIC (checked by its requester id from DMAR) raise `vector`
// on the CPU with APIC id `destination` through remapping entry `index`.
// False if remapping is off, or there's no queued invalidation or no I/O
// APIC device scope to check against.
bool remapIoapicPin(u32 index, u8 vector, bool level, u8 destination);

// Lets the device at `device` read and write [physical, physical + size)
// at the same addresses. False if out of memory.
bool allow(Pci::Address device, u64 physical, u64 size);

// Takes [physical, physical + size) back from every device (before the
// memory is freed and reused).
void revoke(u64 physical, u64 size);

// Logs DMA the hardware blocked since the last call.
void reportFaults();

} // namespace Rudracore::Arch::Iommu
