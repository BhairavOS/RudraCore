// ACPI (spec 6.5): RSDP -> XSDT/RSDT -> tables, each checksum-checked.

#include "arch/x86_64/acpi.h"

#include "arch/x86_64/io.h"
#include "log.h"
#include "mem/memory.h"

namespace Rudracore::Arch::Acpi {

namespace {

constexpr u32 MAX_TABLES = 64;
constexpr u32 MAX_TABLE_LENGTH = 16 * 1024 * 1024; // (a DSDT is rarely over 1 MiB)

Info state;
struct Table {
    char signature[4];
    u64 physical;
    u32 length;
};
Table tables[MAX_TABLES];
u32 tableCount = 0;

// Little-endian reads that don't care about alignment (tables are packed).
u16 get16(u8 const* p) { return static_cast<u16>(p[0] | p[1] << 8); }
u32 get32(u8 const* p) { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }
u64 get64(u8 const* p) { return u64{get32(p)} | u64{get32(p + 4)} << 32; }

bool sameSignature(char const* a, char const* b) { return a[0] == b[0] and a[1] == b[1] and a[2] == b[2] and a[3] == b[3]; }

bool checksumOk(u8 const* p, u64 length) {
    u8 sum = 0;
    for (u64 i = 0; i < length; i++)
        sum = static_cast<u8>(sum + p[i]);
    return sum == 0;
}

// A whole table, mapped and checked; nullptr if it isn't sound.
u8 const* loadTable(u64 physical, u32& length) {
    if (not physical)
        return nullptr;
    auto const* header = static_cast<u8 const*>(map(physical, 36, Vmm::Cache::WriteBack));
    length = get32(header + 4);
    if (length < 36 or length > MAX_TABLE_LENGTH)
        return nullptr;
    auto const* table = static_cast<u8 const*>(map(physical, length, Vmm::Cache::WriteBack));
    return checksumOk(table, length) ? table : nullptr;
}

// MARK: Tables

void parseMadt(u8 const* t, u32 length) {
    Madt& m = state.madt;
    m.present = true;
    m.localApic = get32(t + 36);
    m.pcCompatible = get32(t + 40) & 1;
    for (u8 irq = 0; irq < 16; irq++)
        m.isaIrq[irq] = irq; // identity unless overridden
    for (u32 at = 44; at + 2 <= length;) {
        u8 type = t[at], size = t[at + 1];
        if (size < 2 or at + size > length)
            break;
        u8 const* e = t + at;
        if (type == 0 and size >= 8 and (get32(e + 4) & 1))
            m.cpus++;
        else if (type == 9 and size >= 16 and (get32(e + 8) & 1))
            m.cpus++;
        else if (type == 1 and size >= 12) {
            if (m.ioApics++ == 0) {
                m.ioApic = get32(e + 4);
                m.ioApicGsiBase = get32(e + 8);
            }
        } else if (type == 2 and size >= 10) {
            m.overrides++;
            if (e[2] == 0 and e[3] < 16 and get32(e + 4) < 256) {
                m.isaIrq[e[3]] = static_cast<u8>(get32(e + 4));
                m.isaFlags[e[3]] = get16(e + 8);
            }
        } else if (type == 5 and size >= 12) {
            m.localApic = get64(e + 4);
        }
        at += size;
    }
}

void parseFadt(u8 const* t, u32 length) {
    Fadt& f = state.fadt;
    if (length < 116)
        return;
    f.present = true;
    f.sciInterrupt = get16(t + 46);
    f.smiCommand = get32(t + 48);
    f.acpiEnable = t[52];
    f.pm1aControl = get32(t + 64);
    f.pm1bControl = get32(t + 68);
    f.pmTimer = t[91] >= 4 ? get32(t + 76) : 0;
    f.century = t[108];
    u16 boot = get16(t + 109);
    f.has8042 = boot & (1 << 1);
    f.vgaAbsent = boot & (1 << 2);
    f.msiUnsupported = boot & (1 << 3);
    f.rtcAbsent = boot & (1 << 5);
    u32 flags = get32(t + 112);
    f.pmTimer32 = flags & (1 << 8);
    f.hardwareReduced = flags & (1 << 20);
    f.dsdt = get32(t + 40);
    if (length >= 129) {
        f.resetSupported = flags & (1 << 10);
        f.resetSpace = t[116];
        f.resetAddress = get64(t + 120);
        f.resetValue = t[128];
    }
    if (length >= 148 and get64(t + 140))
        f.dsdt = get64(t + 140); // X_DSDT wins
    // The extended (GAS) control blocks win too, when they're I/O ports.
    if (length >= 184 and t[172] == 1 and get64(t + 176) and get64(t + 176) < 0x10000)
        f.pm1aControl = static_cast<u32>(get64(t + 176));
    if (length >= 196 and t[184] == 1 and get64(t + 188) and get64(t + 188) < 0x10000)
        f.pm1bControl = static_cast<u32>(get64(t + 188));
}

void parseHpet(u8 const* t, u32 length) {
    Hpet& h = state.hpet;
    if (length < 56 or t[40] != 0) // the registers must be memory-mapped
        return;
    h.address = get64(t + 44);
    if (not h.address)
        return;
    // The capabilities register says how fast it counts (at most 100 ns a tick).
    auto const* regs = static_cast<u64 const volatile*>(map(h.address, 1024, Vmm::Cache::Uncached));
    u64 capabilities = regs[0];
    h.periodFs = static_cast<u32>(capabilities >> 32);
    h.timers = static_cast<u32>((capabilities >> 8) & 31) + 1;
    h.counter64 = capabilities & (1 << 13);
    h.present = h.periodFs > 0 and h.periodFs <= 100'000'000;
}

void parseMcfg(u8 const* t, u32 length) {
    Mcfg& m = state.mcfg;
    for (u32 at = 44; at + 16 <= length and m.count < Mcfg::MAX; at += 16) {
        u8 const* e = t + at;
        if (e[11] < e[10]) // end bus before start bus
            continue;
        m.ranges[m.count++] = {get64(e), get16(e + 8), e[10], e[11]};
    }
}

void parseSrat(u8 const* t, u32 length) {
    Srat& s = state.srat;
    s.present = true;
    u32 seen[16];
    u32 seenCount = 0;
    auto domain = [&](u32 d) {
        for (u32 i = 0; i < seenCount; i++) {
            if (seen[i] == d)
                return;
        }
        if (seenCount < 16)
            seen[seenCount++] = d;
    };
    for (u32 at = 48; at + 2 <= length;) {
        u8 type = t[at], size = t[at + 1];
        if (size < 2 or at + size > length)
            break;
        u8 const* e = t + at;
        if (type == 0 and size >= 16 and (get32(e + 4) & 1)) {
            s.cpus++;
            domain(e[2] | u32{e[9]} << 8 | u32{e[10]} << 16 | u32{e[11]} << 24);
        } else if (type == 2 and size >= 24 and (get32(e + 16) & 1)) {
            s.cpus++;
            domain(get32(e + 4));
        } else if (type == 1 and size >= 40 and (get32(e + 28) & 1)) {
            s.memoryRanges++;
            s.memoryBytes += get64(e + 16);
            domain(get32(e + 2));
        }
        at += size;
    }
    s.domains = seenCount;
}

// MARK: AML (without an interpreter)

// A PkgLength (ACPI 20.2.4): how many bytes it takes.
u32 pkgLengthBytes(u8 lead) { return 1 + (lead >> 6); }

// One small integer as AML encodes a constant: ZeroOp, OneOp, BytePrefix n.
bool smallInteger(u8 const*& p, u8 const* end, u8& out) {
    if (p >= end)
        return false;
    if (*p == 0x00 or *p == 0x01) {
        out = *p++;
        return true;
    }
    if (*p == 0x0A and p + 1 < end) {
        out = p[1];
        p += 2;
        return true;
    }
    return false;
}

// Name(_S5, Package () { SLP_TYPa, SLP_TYPb, ... }) in a DSDT: the values
// to write for soft-off. The usual way without an interpreter.
void findS5(u8 const* aml, u32 length) {
    for (u32 i = 1; i + 8 < length; i++) {
        if (aml[i] != '_' or aml[i + 1] != 'S' or aml[i + 2] != '5' or aml[i + 3] != '_')
            continue;
        if (not(aml[i - 1] == 0x08 or (i >= 2 and aml[i - 1] == '\\' and aml[i - 2] == 0x08)))
            continue; // not a Name()
        u8 const* p = aml + i + 4;
        u8 const* end = aml + length;
        if (*p != 0x12) // PackageOp
            continue;
        p++;
        p += pkgLengthBytes(*p);
        p++; // NumElements
        u8 a, b;
        if (p < end and smallInteger(p, end, a) and smallInteger(p, end, b)) {
            state.power.s5 = true;
            state.power.sleepTypeA = a & 7;
            state.power.sleepTypeB = b & 7;
            return;
        }
    }
}

// Battery devices and thermal zones the AML declares (not their readings).
void countDevices(u8 const* aml, u32 length) {
    static constexpr u8 BATTERY[] = {0x0C, 0x41, 0xD0, 0x0C, 0x0A}; // DWordConst EisaId("PNP0C0A")
    for (u32 i = 0; i + 5 <= length; i++) {
        bool battery = true;
        for (u32 k = 0; k < 5 and battery; k++)
            battery = aml[i + k] == BATTERY[k];
        state.power.batteries += battery;
        state.power.thermalZones += aml[i] == 0x5B and aml[i + 1] == 0x85; // ThermalZoneOp
    }
}

// MARK: Generic addresses

void writeRegister(u8 space, u64 address, u8 value) {
    if (space == 1 and address < 0x10000)
        outb(static_cast<u16>(address), value);
    else if (space == 0 and address)
        *static_cast<u8 volatile*>(map(address, 1, Vmm::Cache::Uncached)) = value;
}

} // namespace

void* map(u64 physical, u64 length, Vmm::Cache cache) {
    auto& kernel = Vmm::kernelSpace();
    for (u64 page = Mem::alignDown(physical, Mem::PAGE_SIZE); page < physical + length; page += Mem::PAGE_SIZE) {
        u64 found;
        if (not kernel.translate(Mem::hhdmOffset + page, found))
            kernel.map(Mem::hhdmOffset + page, page, Mem::PAGE_SIZE, Vmm::READ | Vmm::WRITE, cache);
    }
    return Mem::toVirtual(physical);
}

void init(Boot::Info const& boot) {
    state = {};
    tableCount = 0;
    if (not boot.acpiRsdp)
        return;
    auto const* rsdp = static_cast<u8 const*>(map(boot.acpiRsdp, 36, Vmm::Cache::WriteBack));
    if (rsdp[0] != 'R' or rsdp[1] != 'S' or rsdp[2] != 'D' or rsdp[3] != ' ' or rsdp[4] != 'P' or rsdp[5] != 'T' or
        rsdp[6] != 'R' or rsdp[7] != ' ' or not checksumOk(rsdp, 20)) {
        log("[acpi] the RSDP is damaged: no ACPI");
        return;
    }
    state.revision = rsdp[15];
    // ACPI 2.0+: the extended checksum covers all 36 bytes, and the XSDT wins.
    bool xsdt = state.revision >= 2 and get32(rsdp + 20) >= 36 and checksumOk(rsdp, 36) and get64(rsdp + 24);
    u32 rootLength = 0;
    u8 const* root = loadTable(xsdt ? get64(rsdp + 24) : get32(rsdp + 16), rootLength);
    if (not root and xsdt) { // a damaged XSDT: the RSDT may still be good
        xsdt = false;
        root = loadTable(get32(rsdp + 16), rootLength);
    }
    if (not root) {
        log("[acpi] the root table (XSDT/RSDT) is damaged: no ACPI");
        return;
    }
    state.present = true;
    u32 entry = xsdt ? 8 : 4;
    for (u32 at = 36; at + entry <= rootLength and tableCount < MAX_TABLES; at += entry) {
        u64 physical = xsdt ? get64(root + at) : get32(root + at);
        u32 length = 0;
        u8 const* t = loadTable(physical, length);
        if (not t) {
            state.badTables++;
            continue;
        }
        Table& table = tables[tableCount++];
        for (int i = 0; i < 4; i++)
            table.signature[i] = static_cast<char>(t[i]);
        table.physical = physical;
        table.length = length;
    }
    state.tables = tableCount;

    if (u8 const* t = find("APIC"))
        parseMadt(t, get32(t + 4));
    if (u8 const* t = find("FACP"))
        parseFadt(t, get32(t + 4));
    if (u8 const* t = find("HPET"))
        parseHpet(t, get32(t + 4));
    if (u8 const* t = find("MCFG"))
        parseMcfg(t, get32(t + 4));
    if (u8 const* t = find("SRAT"))
        parseSrat(t, get32(t + 4));
    // The DSDT isn't in the root table: the FADT points at it.
    u32 dsdtLength = 0;
    if (u8 const* dsdt = loadTable(state.fadt.dsdt, dsdtLength); dsdt and sameSignature(reinterpret_cast<char const*>(dsdt), "DSDT")) {
        findS5(dsdt + 36, dsdtLength - 36);
        countDevices(dsdt + 36, dsdtLength - 36);
    } else if (state.fadt.dsdt) {
        state.badTables++;
    }
    for (u32 i = 0; i < tableCount; i++) {
        if (sameSignature(tables[i].signature, "SSDT")) {
            auto const* ssdt = static_cast<u8 const*>(Mem::toVirtual(tables[i].physical));
            countDevices(ssdt + 36, tables[i].length - 36);
        }
    }
}

Info const& info() { return state; }

u8 const* find(char const* signature) {
    for (u32 i = 0; i < tableCount; i++) {
        if (sameSignature(tables[i].signature, signature))
            return static_cast<u8 const*>(Mem::toVirtual(tables[i].physical));
    }
    return nullptr;
}

bool powerOff() {
    Fadt const& f = state.fadt;
    if (not f.present or f.hardwareReduced or not state.power.s5 or not f.pm1aControl) {
        log("[acpi] can't power off: ", not state.power.s5 ? "no \\_S5 in the DSDT" : "no PM1 control block");
        return false;
    }
    constexpr u16 SCI_EN = 1, SLP_EN = 1 << 13;
    // ACPI mode (SCI_EN): the firmware may still own the hardware.
    if (not(inw(static_cast<u16>(f.pm1aControl)) & SCI_EN) and f.smiCommand and f.acpiEnable) {
        outb(static_cast<u16>(f.smiCommand), f.acpiEnable);
        for (int i = 0; i < 3'000'000 and not(inw(static_cast<u16>(f.pm1aControl)) & SCI_EN); i++)
            asm volatile("pause");
    }
    log("[acpi] powering off (S5)");
    outw(static_cast<u16>(f.pm1aControl), static_cast<u16>(state.power.sleepTypeA << 10 | SLP_EN));
    if (f.pm1bControl)
        outw(static_cast<u16>(f.pm1bControl), static_cast<u16>(state.power.sleepTypeB << 10 | SLP_EN));
    for (int i = 0; i < 10'000'000; i++)
        asm volatile("pause");
    log("[acpi] the machine didn't power off");
    return false;
}

void reboot() {
    log("[acpi] restarting");
    Fadt const& f = state.fadt;
    if (f.resetSupported)
        writeRegister(f.resetSpace, f.resetAddress, f.resetValue);
    for (int i = 0; i < 1'000'000; i++)
        asm volatile("pause");
    if (not f.present or f.has8042 or state.revision == 0) {
        for (int i = 0; i < 100000 and (inb(0x64) & 2); i++) // input buffer empty
            asm volatile("pause");
        outb(0x64, 0xFE); // the 8042 pulses the reset line
        for (int i = 0; i < 1'000'000; i++)
            asm volatile("pause");
    }
    // Last resort: a triple fault (no IDT, then an exception).
    struct [[gnu::packed]] {
        u16 limit;
        u64 base;
    } empty{0, 0};
    asm volatile("lidt %0; int3" : : "m"(empty));
    halt();
}

void report(bool detailed) {
    if (not state.present) {
        log("[acpi] no ACPI tables");
        return;
    }
    Madt const& m = state.madt;
    Fadt const& f = state.fadt;
    log("[acpi] ", state.revision >= 2 ? "XSDT" : "RSDT", ", ", state.tables, " tables",
        state.badTables ? " (some damaged: skipped)" : ", checksums ok", "; ", m.cpus, " CPU(s), ", m.ioApics,
        " I/O APIC(s), HPET ", state.hpet.present ? "yes" : "no", ", MCFG ", state.mcfg.count ? "yes" : "no",
        ", SRAT ", state.srat.present ? "yes" : "no", ", S5 ", state.power.s5 ? "yes" : "no",
        f.resetSupported ? ", reset register" : "");
    log("[acpi] battery devices declared: ", state.power.batteries, ", thermal zones: ", state.power.thermalZones,
        " (by byte pattern; the acpi server evaluates the AML)");
    if (not detailed)
        return;
    char signature[5] = {};
    for (u32 i = 0; i < tableCount; i++) {
        for (int k = 0; k < 4; k++)
            signature[k] = tables[i].signature[k];
        log("[diag]   ACPI ", signature, " at ", Hex{tables[i].physical}, ", ", tables[i].length, " bytes");
    }
    if (m.present)
        log("[diag]   MADT: local APIC ", Hex{m.localApic}, ", first I/O APIC ", Hex{m.ioApic}, " (GSI ", m.ioApicGsiBase,
            "), ", m.overrides, " override(s), IRQ0 -> GSI ", m.isaIrq[0], m.pcCompatible ? ", 8259s present" : "");
    if (f.present)
        log("[diag]   FADT: SCI ", f.sciInterrupt, ", PM1a ", Hex{f.pm1aControl, 4}, ", PM timer ", Hex{f.pmTimer, 4},
            f.pmTimer32 ? " (32-bit)" : " (24-bit)", ", reset ", f.resetSupported ? "" : "not ",
            "supported, 8042 ", f.has8042 ? "yes" : "no", f.hardwareReduced ? ", hardware-reduced" : "");
    if (state.hpet.present)
        log("[diag]   HPET at ", Hex{state.hpet.address}, ": ", state.hpet.timers, " timers, ",
            1'000'000'000'000'000ull / state.hpet.periodFs, " Hz, ", state.hpet.counter64 ? "64" : "32", "-bit");
    for (u32 i = 0; i < state.mcfg.count; i++)
        log("[diag]   MCFG: segment ", state.mcfg.ranges[i].segment, " buses ", state.mcfg.ranges[i].startBus, "-",
            state.mcfg.ranges[i].endBus, " at ", Hex{state.mcfg.ranges[i].base});
    if (state.srat.present)
        log("[diag]   SRAT: ", state.srat.domains, " NUMA domain(s), ", state.srat.cpus, " CPU(s), ",
            state.srat.memoryRanges, " memory range(s), ", Bytes{state.srat.memoryBytes});
    if (state.power.s5)
        log("[diag]   \\_S5: SLP_TYPa ", state.power.sleepTypeA, ", SLP_TYPb ", state.power.sleepTypeB);
}

} // namespace Rudracore::Arch::Acpi
