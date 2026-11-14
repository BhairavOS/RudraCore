#pragma once

#include "boot.h"
#include "mem/vmm.h"

// ACPI tables: found from the RSDP the bootloader passes, every one
// checksum-checked before use (a corrupted table is skipped and logged).
// Parsed: MADT (CPUs, I/O APICs, interrupt overrides), FADT (power
// management blocks, reset register, flags), HPET, MCFG (PCI Express
// configuration space), SRAT (NUMA domains). Other tables (DMAR) are found
// with find(). Power: reset through the FADT, and soft-off (S5) with the
// sleep type read from the DSDT's \_S5 object.
//
// Not done: an AML interpreter. So battery and thermal readings (_BST,
// _TMP) aren't available; only whether the tables declare such devices.
namespace Rudracore::Arch::Acpi {

struct Madt {
    bool present;
    u64 localApic;  // physical address of the local APIC
    bool pcCompatible; // dual 8259 PICs too (PCAT_COMPAT)
    u32 cpus;       // enabled processors (local APIC and x2APIC entries)
    u32 ioApics;
    u64 ioApic;     // the first one's address
    u32 ioApicGsiBase;
    u32 overrides;  // interrupt source overrides (ISA IRQ -> GSI)
    u8 isaIrq[16];  // where each ISA IRQ arrives (GSI), after overrides
    u16 isaFlags[16]; // their MPS INTI flags (polarity bits 0-1, trigger bits 2-3; 0: ISA default)
};

struct Fadt {
    bool present;
    bool hardwareReduced; // no fixed hardware (PM1 blocks): no S5 the old way
    u16 sciInterrupt;
    u32 smiCommand;
    u8 acpiEnable;
    u32 pm1aControl, pm1bControl; // I/O ports, 0 if absent
    u32 pmTimer;                  // I/O port of the ACPI timer, 0 if absent
    bool pmTimer32;
    bool resetSupported;
    u8 resetSpace;   // 0 memory, 1 I/O
    u64 resetAddress;
    u8 resetValue;
    u64 dsdt;        // physical address
    bool has8042, vgaAbsent, msiUnsupported, rtcAbsent; // IA-PC boot architecture flags
    u8 century;      // CMOS register of the century, 0 if none
};

struct Hpet {
    bool present;
    u64 address;
    u32 periodFs;    // counter tick in femtoseconds (from the HPET itself)
    u32 timers;
    bool counter64;
};

struct Mcfg {
    static constexpr u32 MAX = 4;
    u32 count;
    struct Range {
        u64 base;
        u16 segment;
        u8 startBus, endBus;
    } ranges[MAX];
};

struct Srat {
    bool present;
    u32 domains;      // distinct proximity domains
    u32 cpus;         // enabled processor affinity entries
    u32 memoryRanges; // enabled memory affinity entries
    u64 memoryBytes;
};

struct Power {
    bool s5;            // \_S5 found in the DSDT
    u8 sleepTypeA, sleepTypeB;
    u32 batteries;      // battery devices (PNP0C0A) declared in the DSDT/SSDTs
    u32 thermalZones;   // ThermalZone objects declared
};

struct Info {
    bool present;      // a valid RSDP and root table
    u8 revision;       // 0: ACPI 1.0 (RSDT), 2+: XSDT
    u32 tables;
    u32 badTables;     // skipped: checksum or length wrong
    Madt madt;
    Fadt fadt;
    Hpet hpet;
    Mcfg mcfg;
    Srat srat;
    Power power;
};

// Reads every table. Call once, after the kernel's page tables are up.
void init(Boot::Info const& boot);

Info const& info();

// A checksum-checked table with this signature ("DMAR"), or nullptr.
u8 const* find(char const* signature);

// Maps [physical, physical + length) into the direct map if it isn't yet
// (tables and registers can lie outside the RAM it covers).
void* map(u64 physical, u64 length, Vmm::Cache cache);

// Turns the machine off (S5). Returns only if it couldn't.
bool powerOff();

// Restarts the machine: the FADT reset register, then the 8042, then a
// triple fault. Doesn't return.
[[noreturn]] void reboot();

// Logs what was found (everything, when `detailed`).
void report(bool detailed);

} // namespace Rudracore::Arch::Acpi
