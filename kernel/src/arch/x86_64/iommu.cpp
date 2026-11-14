// Intel VT-d (Intel Virtualization Technology for Directed I/O, spec rev 3+):
// root table -> context table per bus -> per-device second-level page
// tables, invalidated with the register interface.

#include "arch/x86_64/iommu.h"

#include "arch/x86_64/cpu.h"
#include "log.h"
#include "mem/memory.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

extern "C" int memcmp(void const* a, void const* b, usize n);

namespace Rudracore::Arch::Iommu {

namespace {

constexpr usize MAX_UNITS = 4;
constexpr usize MAX_DOMAINS = 32;

// Registers
constexpr u64 CAP = 0x08, ECAP = 0x10, GCMD = 0x18, GSTS = 0x1C, RTADDR = 0x20, CCMD = 0x28, FSTS = 0x34,
              IRTA = 0xB8;
constexpr u32 TRANSLATION_ENABLE = 1u << 31, SET_ROOT_POINTER = 1u << 30;
constexpr u32 INTERRUPT_REMAP_ENABLE = 1u << 25, SET_INTERRUPT_TABLE = 1u << 24, COMPATIBILITY_ALLOWED = 1u << 23;
constexpr u64 ECAP_INTERRUPT_REMAPPING = 1u << 3;
constexpr u8 FIRST_INTERRUPT_FAULT = 0x20; // fault reasons 0x20-0x2F are interrupt requests
constexpr u32 ONE_SHOT_MASK = 0x96FFFFFF; // GSTS bits that may be written back to GCMD

// Second-level page table entries
constexpr u64 READ = 1, WRITE = 2;
constexpr u64 ADDRESS = 0x000FFFFFFFFFF000;

struct Unit {
    u8 volatile* regs;
    u64 iotlb; // offset of the IOTLB invalidate register
    u64 faults; // offset of the first fault recording register
    u64 faultCount;
};

struct Domain {
    Pci::Address device;
    u64 root; // its second-level page table
};

Unit units[MAX_UNITS];
usize unitCount = 0;
Domain domains[MAX_DOMAINS];
usize domainCount = 0;
u64 rootTable = 0; // shared by every unit
int levels = 0;    // 3 (39-bit addresses) or 4 (48-bit)
bool coherent = true; // the hardware snoops our table writes (else: flush them)
bool interruptRemapping = true; // every unit can remap interrupts (cleared when one can't)

u64 read64(Unit const& u, u64 off) { return *reinterpret_cast<u64 volatile*>(u.regs + off); }
u32 read32(Unit const& u, u64 off) { return *reinterpret_cast<u32 volatile*>(u.regs + off); }
void write64(Unit const& u, u64 off, u64 v) { *reinterpret_cast<u64 volatile*>(u.regs + off) = v; }
void write32(Unit const& u, u64 off, u32 v) { *reinterpret_cast<u32 volatile*>(u.regs + off) = v; }

u64* table(u64 physical) { return Mem::toVirtual<u64>(physical); }

void flush(void const* p) {
    if (not coherent)
        asm volatile("clflush (%0)" : : "r"(p) : "memory");
}

// Makes sure [physical, physical + length) is reachable in the direct map
// (ACPI tables, registers may lie outside the RAM it covers).
void* reach(u64 physical, u64 length, Vmm::Cache cache) {
    auto& kernel = Vmm::kernelSpace();
    for (u64 page = Mem::alignDown(physical, Mem::PAGE_SIZE); page < physical + length; page += Mem::PAGE_SIZE) {
        u64 found;
        if (not kernel.translate(Mem::hhdmOffset + page, found))
            kernel.map(Mem::hhdmOffset + page, page, Mem::PAGE_SIZE, Vmm::READ | Vmm::WRITE, cache);
    }
    return Mem::toVirtual(physical);
}

void waitClear64(Unit const& u, u64 off, u64 bit) {
    for (int i = 0; i < 10000000 and (read64(u, off) & bit); i++)
        asm volatile("pause");
}

void invalidateAll() {
    for (usize i = 0; i < unitCount; i++) {
        Unit const& u = units[i];
        write64(u, CCMD, (1ull << 63) | (1ull << 61)); // context cache, global
        waitClear64(u, CCMD, 1ull << 63);
        write64(u, u.iotlb + 8, (1ull << 63) | (1ull << 60) | (3ull << 48)); // IOTLB, global, drain
        waitClear64(u, u.iotlb + 8, 1ull << 63);
    }
}

void setCommand(Unit const& u, u32 bit) {
    u32 status = read32(u, GSTS) & ONE_SHOT_MASK;
    write32(u, GCMD, status | bit);
    for (int i = 0; i < 10000000 and not(read32(u, GSTS) & bit); i++)
        asm volatile("pause");
}

// The ACPI table with this signature, or nullptr.
u8 const* findTable(u64 rsdp, char const* signature) {
    auto const* r = static_cast<u8 const*>(reach(rsdp, 36, Vmm::Cache::WriteBack));
    if (memcmp(r, "RSD PTR ", 8) != 0)
        return nullptr;
    bool xsdt = r[15] >= 2;
    u64 sdt = xsdt ? *reinterpret_cast<u64 const*>(r + 24) : *reinterpret_cast<u32 const*>(r + 16);
    auto const* header = static_cast<u8 const*>(reach(sdt, 36, Vmm::Cache::WriteBack));
    u32 length = *reinterpret_cast<u32 const*>(header + 4);
    header = static_cast<u8 const*>(reach(sdt, length, Vmm::Cache::WriteBack));
    u64 entry = xsdt ? 8 : 4;
    for (u64 at = 36; at + entry <= length; at += entry) {
        u64 address = xsdt ? *reinterpret_cast<u64 const*>(header + at) : *reinterpret_cast<u32 const*>(header + at);
        auto const* t = static_cast<u8 const*>(reach(address, 36, Vmm::Cache::WriteBack));
        if (memcmp(t, signature, 4) == 0)
            return static_cast<u8 const*>(reach(address, *reinterpret_cast<u32 const*>(t + 4), Vmm::Cache::WriteBack));
    }
    return nullptr;
}

// The leaf entry for `address` in a domain's page table, made if `create`.
u64* entryFor(u64 root, u64 address, bool create) {
    u64 physical = root;
    for (int level = levels - 1; level > 0; level--) {
        u64* t = table(physical);
        u64& e = t[(address >> (12 + 9 * level)) & 511];
        if (not(e & READ)) {
            if (not create)
                return nullptr;
            u64 next = Pmm::allocZeroed();
            if (not next)
                return nullptr;
            e = next | READ | WRITE;
            flush(&e);
        }
        physical = e & ADDRESS;
    }
    return &table(physical)[(address >> 12) & 511];
}

Domain* domainFor(Pci::Address a) {
    for (usize i = 0; i < domainCount; i++) {
        Domain& d = domains[i];
        if (d.device.bus == a.bus and d.device.device == a.device and d.device.function == a.function)
            return &d;
    }
    if (domainCount == MAX_DOMAINS)
        return nullptr;
    u64 root = Pmm::allocZeroed();
    if (not root)
        return nullptr;
    // The bus's context table, then this device's entry in it.
    u64* roots = table(rootTable);
    if (not(roots[2 * a.bus] & 1)) {
        u64 context = Pmm::allocZeroed();
        if (not context) {
            Pmm::free(root);
            return nullptr;
        }
        roots[2 * a.bus] = context | 1;
        flush(&roots[2 * a.bus]);
    }
    u64* context = table(roots[2 * a.bus] & ADDRESS);
    usize index = 2 * (a.device * 8u + a.function);
    u64 domainId = domainCount + 1;
    context[index + 1] = (levels == 4 ? 2 : 1) | (domainId << 8); // address width, domain
    context[index] = root | 1;                                    // present, translate untranslated requests
    flush(&context[index]);
    Domain& d = domains[domainCount++];
    d = {a, root};
    return &d;
}

} // namespace

void init(Boot::Info const& info) {
    u8 const* dmar = info.acpiRsdp ? findTable(info.acpiRsdp, "DMAR") : nullptr;
    if (not dmar) {
        log("[iommu] no VT-d (no ACPI DMAR table): drivers' devices can reach all memory");
        return;
    }
    u32 length = *reinterpret_cast<u32 const*>(dmar + 4);
    int sagaw = 0x1F;
    for (u32 at = 48; at + 4 <= length;) {
        u16 type = *reinterpret_cast<u16 const*>(dmar + at);
        u16 size = *reinterpret_cast<u16 const*>(dmar + at + 2);
        if (size < 4)
            break;
        if (type == 0 and unitCount < MAX_UNITS) { // DRHD: one remapping unit
            u64 base = *reinterpret_cast<u64 const*>(dmar + at + 8);
            Unit& u = units[unitCount++];
            u.regs = static_cast<u8 volatile*>(reach(base, 2 * Mem::PAGE_SIZE, Vmm::Cache::Uncached));
            u64 cap = read64(u, CAP), ecap = read64(u, ECAP);
            u.iotlb = ((ecap >> 8) & 0x3FF) * 16;
            u.faults = ((cap >> 24) & 0x3FF) * 16;
            u.faultCount = ((cap >> 40) & 0xFF) + 1;
            sagaw &= static_cast<int>((cap >> 8) & 0x1F);
            coherent = coherent and (ecap & 1);
            interruptRemapping = interruptRemapping and (ecap & ECAP_INTERRUPT_REMAPPING);
        }
        at += size;
    }
    levels = (sagaw & 4) ? 4 : (sagaw & 2) ? 3 : 0;
    rootTable = levels ? Pmm::allocZeroed() : 0;
    if (not unitCount or not rootTable) {
        log("[iommu] VT-d found but unusable (no supported page-table depth): DMA stays unconfined");
        unitCount = 0;
        return;
    }
    for (usize i = 0; i < unitCount; i++) {
        write64(units[i], RTADDR, rootTable);
        setCommand(units[i], SET_ROOT_POINTER);
    }
    invalidateAll();
    for (usize i = 0; i < unitCount; i++)
        setCommand(units[i], TRANSLATION_ENABLE);
    log("[iommu] VT-d on (", unitCount, " unit(s), ", levels == 4 ? 48 : 39,
        "-bit): devices reach only memory their driver registered");

    // Interrupt remapping. A device write to 0xFEExxxxx isn't DMA but an
    // interrupt message, which DMA remapping lets through: a driver's device
    // could raise any vector, an NMI or an SMI. Every interrupt here comes
    // through the legacy PIC (wires, not messages), so no device needs to
    // send one: the table is all not-present, and compatibility-format
    // messages stay blocked (GCMD.CFI clear). Each blocked one is logged.
    // shortcut: no entries yet; when a driver uses MSI, give its device
    // entries with source-id checking (SVT/SID) instead of blocking all.
    u64 interruptTable = interruptRemapping ? Pmm::allocZeroed() : 0; // 256 entries of 16 bytes
    if (not interruptTable) {
        log("[iommu] no interrupt remapping: a device could still send interrupt messages (MSI)");
        return;
    }
    bool on = true;
    for (usize i = 0; i < unitCount; i++) {
        write64(units[i], IRTA, interruptTable | 7); // 2^(7+1) = 256 entries, xAPIC format
        setCommand(units[i], SET_INTERRUPT_TABLE);
        setCommand(units[i], INTERRUPT_REMAP_ENABLE);
        u32 status = read32(units[i], GSTS);
        on = on and (status & INTERRUPT_REMAP_ENABLE) and not(status & COMPATIBILITY_ALLOWED);
    }
    // (The interrupt entry cache needs no flush: remapping was off until
    // now, and the table never changes.)
    if (on)
        log("[iommu] interrupt remapping on: devices can't send interrupt messages (all interrupts use the PIC)");
    else
        log("[iommu] interrupt remapping didn't turn on (or lets compatibility-format messages through)");
}

bool enabled() {
    return unitCount > 0;
}

bool allow(Pci::Address device, u64 physical, u64 size) {
    InterruptGuard guard;
    Domain* d = domainFor(device);
    if (not d)
        return false;
    for (u64 page = Mem::alignDown(physical, Mem::PAGE_SIZE); page < physical + size; page += Mem::PAGE_SIZE) {
        u64* e = entryFor(d->root, page, true);
        if (not e)
            return false;
        *e = page | READ | WRITE;
        flush(e);
    }
    invalidateAll();
    return true;
}

void revoke(u64 physical, u64 size) {
    InterruptGuard guard;
    for (usize i = 0; i < domainCount; i++) {
        for (u64 page = Mem::alignDown(physical, Mem::PAGE_SIZE); page < physical + size; page += Mem::PAGE_SIZE) {
            if (u64* e = entryFor(domains[i].root, page, false)) {
                *e = 0;
                flush(e);
            }
        }
    }
    invalidateAll(); // no device may still hold a cached translation once this returns
}

void reportFaults() {
    InterruptGuard guard;
    for (usize i = 0; i < unitCount; i++) {
        Unit const& u = units[i];
        for (u64 f = 0; f < u.faultCount; f++) {
            u64 at = u.faults + 16 * f;
            u64 high = read64(u, at + 8);
            if (not(high >> 63))
                continue;
            u64 address = read64(u, at) & ADDRESS;
            u16 source = static_cast<u16>(high);
            u8 reason = static_cast<u8>(high >> 32);
            if (reason >= FIRST_INTERRUPT_FAULT)
                log("[iommu] blocked an interrupt message by device ", source >> 8, ":", (source >> 3) & 31, ".",
                    source & 7, " (reason ", Hex{reason}, ")");
            else
                log("[iommu] blocked a DMA ", (high >> 62) & 1 ? "read" : "write", " by device ", source >> 8, ":",
                    (source >> 3) & 31, ".", source & 7, " at ", Hex{address}, " (reason ", reason, ")");
            write64(u, at + 8, 1ull << 63); // clear the record
        }
        write32(u, FSTS, read32(u, FSTS)); // clear overflow / pending
    }
}

} // namespace Rudracore::Arch::Iommu
