// acpi: the ACPI server. Runs the firmware's AML (with uACPI, vendored in
// third_party/uacpi) in userspace, where it can reach only what its handles
// allow: physical memory that isn't RAM, the I/O ports the kernel and the
// drivers don't own, and PCI configuration space.
//
// Handles from init:
//   1: firmware (non-RAM physical memory, PCI configuration)
//   2: firmware I/O ports
//
// Today it loads the namespace and reports what the AML says: \_S5 (the
// soft-off sleep type, evaluated rather than pattern-matched), the PCI root
// bridge's _PRT (interrupt routing), and the battery, AC adapter, power
// button and thermal zone devices. Next: APIC routing, events (SCI),
// battery and thermal readings, sleep.

#include <kapala/heap.h>
#include <kapala/sys.h>
#include <kapala/text.h>

#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

using namespace Kapala;

namespace {

constexpr Sys::Handle FIRMWARE = 1;
constexpr Sys::Handle PORTS = 2;
constexpr unsigned long PAGE = 4096;

// MARK: Mappings (cached: a program may hold only 32, and uACPI maps often)

struct Mapping {
    unsigned long physical = 0, size = 0; // page aligned
    unsigned char* address = nullptr;
    long handle = 0;
    int uses = 0;
};
Mapping mappings[24];

void* mapPhysical(unsigned long physical, unsigned long length) {
    for (Mapping& m : mappings) {
        if (m.address and physical >= m.physical and physical + length <= m.physical + m.size) {
            m.uses++;
            return m.address + (physical - m.physical);
        }
    }
    Mapping* slot = nullptr;
    for (Mapping& m : mappings) {
        if (not m.address) {
            slot = &m;
            break;
        }
    }
    if (not slot) { // all taken: let go of one nobody uses
        for (Mapping& m : mappings) {
            if (m.uses == 0) {
                Sys::memoryUnmap(m.address);
                Sys::close(static_cast<Sys::Handle>(m.handle));
                m = Mapping{};
                slot = &m;
                break;
            }
        }
    }
    if (not slot)
        return UACPI_MAP_FAILED;
    unsigned long start = physical & ~(PAGE - 1), end = (physical + length + PAGE - 1) & ~(PAGE - 1);
    long handle = Sys::firmwareMemory(FIRMWARE, start, end - start);
    if (handle < 0)
        return UACPI_MAP_FAILED;
    auto* address = static_cast<unsigned char*>(Sys::memoryMap(static_cast<Sys::Handle>(handle)));
    if (not address) {
        Sys::close(static_cast<Sys::Handle>(handle));
        return UACPI_MAP_FAILED;
    }
    *slot = {start, end - start, address, handle, 1};
    return address + (physical - start);
}

void unmapPhysical(void* address) {
    auto* p = static_cast<unsigned char*>(address);
    for (Mapping& m : mappings) {
        if (m.address and p >= m.address and p < m.address + m.size and m.uses > 0) {
            m.uses--; // (kept mapped for the next time)
            return;
        }
    }
}

// MARK: Reporting

struct Counter {
    unsigned long n = 0;
};

uacpi_iteration_decision count(void* user, uacpi_namespace_node*, uacpi_u32) {
    static_cast<Counter*>(user)->n++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

uacpi_iteration_decision firstNode(void* user, uacpi_namespace_node* node, uacpi_u32) {
    *static_cast<uacpi_namespace_node**>(user) = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

unsigned long devices(char const* hid) {
    Counter c;
    uacpi_find_devices(hid, count, &c);
    return c.n;
}

void report() {
    Counter objects;
    uacpi_namespace_for_each_child_simple(uacpi_namespace_root(), count, &objects);

    // \_S5: Package { SLP_TYPa, SLP_TYPb, ... }
    Text<200> s5;
    uacpi_object* package = nullptr;
    uacpi_object_array items{};
    uacpi_u64 a = 0, b = 0;
    if (uacpi_eval_simple_package(nullptr, "\\_S5", &package) == UACPI_STATUS_OK and
        uacpi_object_get_package(package, &items) == UACPI_STATUS_OK and items.count >= 2 and
        uacpi_object_get_integer(items.objects[0], &a) == UACPI_STATUS_OK and
        uacpi_object_get_integer(items.objects[1], &b) == UACPI_STATUS_OK)
        s5.add("\\_S5 evaluated: SLP_TYPa ").add(a).add(", SLP_TYPb ").add(b);
    else
        s5.add("\\_S5: none (this machine can't be soft-powered-off through ACPI)");
    uacpi_object_unref(package);

    // _PRT of the PCI root bridge (PCI Express or plain PCI)
    Text<200> prt;
    uacpi_namespace_node* root = nullptr;
    uacpi_find_devices("PNP0A08", firstNode, &root);
    if (not root)
        uacpi_find_devices("PNP0A03", firstNode, &root);
    uacpi_pci_routing_table* table = nullptr;
    if (root and uacpi_get_pci_routing_table(root, &table) == UACPI_STATUS_OK) {
        unsigned long links = 0;
        for (uacpi_size i = 0; i < table->num_entries; i++)
            links += table->entries[i].source != nullptr;
        prt.add("_PRT of the PCI root: ").add(static_cast<unsigned long>(table->num_entries)).add(" entries (")
            .add(links).add(" through link devices, ").add(static_cast<unsigned long>(table->num_entries) - links)
            .add(" fixed)");
        if (table->num_entries) {
            auto const& e = table->entries[0];
            prt.add("; e.g. device ").add(static_cast<unsigned long>(e.address >> 16)).add(" pin ")
                .add(static_cast<unsigned long>(e.pin));
            if (e.source)
                prt.add(" -> a link device");
            else
                prt.add(" -> GSI ").add(static_cast<unsigned long>(e.index));
        }
        uacpi_free_pci_routing_table(table);
    } else {
        prt.add("_PRT: no PCI root bridge routing found");
    }

    Counter thermal;
    uacpi_namespace_for_each_child(uacpi_namespace_root(), count, nullptr, UACPI_OBJECT_THERMAL_ZONE_BIT,
                                   UACPI_MAX_DEPTH_ANY, &thermal);

    Text<200> t;
    t.add("acpi: AML namespace loaded: ").add(objects.n).add(" objects");
    Sys::log(t.cstr());
    Sys::log(s5.cstr());
    Sys::log(prt.cstr());
    Text<200> d;
    d.add("devices: ").add(devices("PNP0C0A")).add(" battery, ").add(devices("ACPI0003")).add(" AC adapter, ")
        .add(devices("PNP0C0C")).add(" power button, ").add(thermal.n).add(" thermal zone(s)");
    Sys::log(d.cstr());
}

} // namespace

// MARK: uACPI's host interface

extern "C" {

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out) {
    Sys::SystemInfo info{};
    Sys::systemInfo(info);
    if (not info.acpiRsdp)
        return UACPI_STATUS_NOT_FOUND;
    *out = info.acpiRsdp;
    return UACPI_STATUS_OK;
}

void* uacpi_kernel_map(uacpi_phys_addr address, uacpi_size length) { return mapPhysical(address, length); }
void uacpi_kernel_unmap(void* address, uacpi_size) { unmapPhysical(address); }

void uacpi_kernel_log(uacpi_log_level level, uacpi_char const* text) {
    if (level > UACPI_LOG_INFO)
        return; // (trace and debug are too chatty for the serial log)
    Text<300> t;
    t.add(level == UACPI_LOG_ERROR ? "uacpi error: " : level == UACPI_LOG_WARN ? "uacpi warning: " : "uacpi: ");
    for (unsigned long i = 0; text[i] and text[i] != '\n' and i < 250; i++) {
        char c[2] = {text[i], 0};
        t.add(c);
    }
    Sys::log(t.cstr());
}

// PCI configuration (segment 0) through the kernel.
struct PciDevice {
    unsigned long address; // bus << 16 | device << 11 | function << 8
};

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle* out) {
    if (a.segment != 0)
        return UACPI_STATUS_UNIMPLEMENTED;
    auto* d = static_cast<PciDevice*>(Heap::alloc(sizeof(PciDevice)));
    if (not d)
        return UACPI_STATUS_OUT_OF_MEMORY;
    d->address = static_cast<unsigned long>(a.bus) << 16 | static_cast<unsigned long>(a.device) << 11 |
                 static_cast<unsigned long>(a.function) << 8;
    *out = d;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle d) { Heap::free(d); }

static uacpi_status pciRead(uacpi_handle d, uacpi_size offset, unsigned width, unsigned long& out) {
    long r = Sys::firmwarePciRead(FIRMWARE, static_cast<PciDevice*>(d)->address | offset, width);
    if (r < 0 or offset > 255)
        return UACPI_STATUS_INVALID_ARGUMENT;
    out = static_cast<unsigned long>(r);
    return UACPI_STATUS_OK;
}
static uacpi_status pciWrite(uacpi_handle d, uacpi_size offset, unsigned width, unsigned long value) {
    if (offset > 255 or Sys::firmwarePciWrite(FIRMWARE, static_cast<PciDevice*>(d)->address | offset, width, value) < 0)
        return UACPI_STATUS_INVALID_ARGUMENT;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read8(uacpi_handle d, uacpi_size o, uacpi_u8* v) {
    unsigned long x;
    uacpi_status s = pciRead(d, o, 1, x);
    *v = static_cast<uacpi_u8>(x);
    return s;
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle d, uacpi_size o, uacpi_u16* v) {
    unsigned long x;
    uacpi_status s = pciRead(d, o, 2, x);
    *v = static_cast<uacpi_u16>(x);
    return s;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle d, uacpi_size o, uacpi_u32* v) {
    unsigned long x;
    uacpi_status s = pciRead(d, o, 4, x);
    *v = static_cast<uacpi_u32>(x);
    return s;
}
uacpi_status uacpi_kernel_pci_write8(uacpi_handle d, uacpi_size o, uacpi_u8 v) { return pciWrite(d, o, 1, v); }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle d, uacpi_size o, uacpi_u16 v) { return pciWrite(d, o, 2, v); }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle d, uacpi_size o, uacpi_u32 v) { return pciWrite(d, o, 4, v); }

// I/O ports through the firmware port handle (the kernel refuses the ones
// it and the drivers own).
struct IoRange {
    unsigned long base, length;
};

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length, uacpi_handle* out) {
    if (base > 0xFFFF or length > 0x10000 - base)
        return UACPI_STATUS_INVALID_ARGUMENT;
    auto* r = static_cast<IoRange*>(Heap::alloc(sizeof(IoRange)));
    if (not r)
        return UACPI_STATUS_OUT_OF_MEMORY;
    *r = {base, length};
    *out = r;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle r) { Heap::free(r); }

static uacpi_status ioRead(uacpi_handle h, uacpi_size offset, unsigned width, unsigned long& out) {
    auto* r = static_cast<IoRange*>(h);
    if (offset + width > r->length)
        return UACPI_STATUS_INVALID_ARGUMENT;
    long v = Sys::portInWide(PORTS, static_cast<unsigned>(r->base + offset), width);
    if (v < 0)
        return UACPI_STATUS_INVALID_ARGUMENT; // a port this server may not touch
    out = static_cast<unsigned long>(v);
    return UACPI_STATUS_OK;
}
static uacpi_status ioWrite(uacpi_handle h, uacpi_size offset, unsigned width, unsigned long value) {
    auto* r = static_cast<IoRange*>(h);
    if (offset + width > r->length or Sys::portOutWide(PORTS, static_cast<unsigned>(r->base + offset), width, value) < 0)
        return UACPI_STATUS_INVALID_ARGUMENT;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size o, uacpi_u8* v) {
    unsigned long x = 0;
    uacpi_status s = ioRead(h, o, 1, x);
    *v = static_cast<uacpi_u8>(x);
    return s;
}
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size o, uacpi_u16* v) {
    unsigned long x = 0;
    uacpi_status s = ioRead(h, o, 2, x);
    *v = static_cast<uacpi_u16>(x);
    return s;
}
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size o, uacpi_u32* v) {
    unsigned long x = 0;
    uacpi_status s = ioRead(h, o, 4, x);
    *v = static_cast<uacpi_u32>(x);
    return s;
}
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size o, uacpi_u8 v) { return ioWrite(h, o, 1, v); }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v) { return ioWrite(h, o, 2, v); }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v) { return ioWrite(h, o, 4, v); }

void* uacpi_kernel_alloc(uacpi_size size) { return Heap::alloc(size ? size : 1); }
void uacpi_kernel_free(void* p) {
    if (p)
        Heap::free(p);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return Sys::timeMs() * 1'000'000ull; }

void uacpi_kernel_stall(uacpi_u8 usec) {
    // shortcut: the clock counts milliseconds, so short stalls spin a rough count.
    for (unsigned long i = 0; i < usec * 200ul; i++)
        asm volatile("pause");
}

void uacpi_kernel_sleep(uacpi_u64 msec) { Sys::sleep(msec); }

// One thread: semaphores are counters, spinlocks and interrupts need nothing.
struct Semaphore {
    unsigned long units;
};

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial) {
    auto* s = static_cast<Semaphore*>(Heap::alloc(sizeof(Semaphore)));
    if (s)
        s->units = initial;
    return s;
}
void uacpi_kernel_free_semaphore(uacpi_handle s) { Heap::free(s); }
uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle h, uacpi_u16) {
    auto* s = static_cast<Semaphore*>(h);
    if (s->units == 0)
        return UACPI_STATUS_TIMEOUT; // nobody else could ever signal it
    s->units--;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_signal_semaphore(uacpi_handle h) { static_cast<Semaphore*>(h)->units++; }

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return reinterpret_cast<uacpi_thread_id>(1); }
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return 0; }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state) {}

uacpi_handle uacpi_kernel_create_spinlock(void) { return Heap::alloc(1); }
void uacpi_kernel_free_spinlock(uacpi_handle s) { Heap::free(s); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle) { return 0; }
void uacpi_kernel_unlock_spinlock(uacpi_handle, uacpi_cpu_flags) {}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* request) {
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        Sys::log("acpi: the firmware's AML reported a fatal error");
    return UACPI_STATUS_OK;
}

// shortcut: the SCI isn't wired up yet (events come with the next step);
// the handler is accepted and never called.
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32, uacpi_interrupt_handler, uacpi_handle,
                                                    uacpi_handle* out) {
    *out = reinterpret_cast<uacpi_handle>(1);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler, uacpi_handle) {
    return UACPI_STATUS_OK;
}

// Work runs at once: there's no second thread to hand it to.
uacpi_handle uacpi_kernel_create_work_item(void) { return Heap::alloc(1); }
void uacpi_kernel_free_work_item(uacpi_handle w) { Heap::free(w); }
void uacpi_kernel_schedule_work(uacpi_work_type, uacpi_handle, uacpi_work_handler handler, uacpi_handle context) {
    handler(context);
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) { return UACPI_STATUS_OK; }

} // extern "C"

int main() {
    // The fence around firmware code: a port the kernel owns (the PIC) and a
    // page of ordinary RAM must both be refused.
    long pic = Sys::portInWide(PORTS, 0x21, 1);
    long ram = Sys::firmwareMemory(FIRMWARE, 0x200000, PAGE);
    if (pic >= 0 or ram >= 0) {
        Sys::log("acpi: FAILED: the firmware handles reach the kernel's ports or RAM");
        return 1;
    }
    Sys::log("acpi: firmware access limits hold (kernel ports and RAM refused)");

    uacpi_status s = uacpi_initialize(0);
    if (s == UACPI_STATUS_OK)
        s = uacpi_namespace_load();
    if (s == UACPI_STATUS_OK)
        s = uacpi_namespace_initialize();
    if (s != UACPI_STATUS_OK) {
        Text<200> t;
        t.add("acpi: couldn't load the firmware's AML: ").add(uacpi_status_to_string(s));
        Sys::log(t.cstr());
        return 1;
    }
    report();
    for (;;)
        Sys::sleep(60 * 1000);
}
