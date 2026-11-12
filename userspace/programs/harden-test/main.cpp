// harden-test: checks at every boot that the kernel refuses what it must.
//
// 1. Channel cycles: an endpoint sent into its own queue, or two endpoints
//    sent into each other's queues, would never be freed.
// 2. Bad program files: spawn_image must reject each crafted ELF below and
//    still start a good one.
// 3. No kernel bytes in system_info's padding.
// 4. Control characters in log text are made harmless (look at the log: the
//    line below must stay on one line).

#include <kapala/sys.h>
#include <kapala/text.h>

extern "C" void* memset(void* dest, int value, unsigned long n);

using namespace Kapala;

namespace {

int passed = 0, failed = 0;

void expect(char const* what, long result, long wanted) {
    bool ok = wanted == 0 ? result >= 0 : result == wanted;
    (ok ? passed : failed)++;
    if (not ok) {
        Text<> line;
        line.add("harden check: FAILED: ").add(what).add(" returned ").add(result).add(", wanted ").add(wanted);
        Sys::log(line.cstr());
    }
}

// MARK: Channel cycles

void checkCycles() {
    Sys::Handle a, b;
    Sys::channelCreate(a, b);
    char byte = 1;
    expect("sending an endpoint into its own queue", Sys::send(a, &byte, 1, b), Sys::INVALID_ARGUMENT);
    Sys::close(a);
    Sys::close(b);

    Sys::Handle a1, b1, a2, b2;
    Sys::channelCreate(a1, b1);
    Sys::channelCreate(a2, b2);
    expect("sending an endpoint into another queue", Sys::send(a2, &byte, 1, b1), 0); // b1 now waits in b2
    expect("sending an endpoint that holds one", Sys::send(a1, &byte, 1, b2), Sys::INVALID_ARGUMENT);
    Sys::close(a1);
    Sys::close(a2);
    Sys::close(b2); // frees b1 with it
}

// MARK: Program files

struct Header {
    unsigned char ident[16];
    unsigned short type, machine;
    unsigned int version;
    unsigned long entry, phoff, shoff;
    unsigned int flags;
    unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Segment {
    unsigned int type, flags;
    unsigned long offset, vaddr, paddr, filesz, memsz, align;
};

constexpr unsigned long BASE = 0x400000;
constexpr unsigned long CODE = 0x100; // file offset of the code
constexpr unsigned int LOAD = 1, INTERP = 3, R = 4, W = 2, X = 1;

// exit(0): mov eax, SYS_EXIT; xor edi, edi; int 0x80
constexpr unsigned char EXIT_CODE[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x31, 0xFF, 0xCD, 0x80};

// Writes a one-segment program (code at CODE) into `file`; `tweak` changes it.
template <typename Tweak>
long spawnCrafted(Sys::Handle memory, unsigned char* file, Tweak tweak) {
    memset(file, 0, 4096);
    auto* h = reinterpret_cast<Header*>(file);
    auto* segments = reinterpret_cast<Segment*>(file + sizeof(Header));
    unsigned char const magic[] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
    for (unsigned i = 0; i < sizeof(magic); i++)
        h->ident[i] = magic[i];
    h->type = 2;
    h->machine = 62;
    h->version = 1;
    h->entry = BASE + CODE;
    h->phoff = sizeof(Header);
    h->ehsize = sizeof(Header);
    h->phentsize = sizeof(Segment);
    h->phnum = 1;
    segments[0] = {LOAD, R | X, 0, BASE, BASE, CODE + sizeof(EXIT_CODE), CODE + sizeof(EXIT_CODE), 4096};
    for (unsigned i = 0; i < sizeof(EXIT_CODE); i++)
        file[CODE + i] = EXIT_CODE[i];
    tweak(*h, segments);

    Sys::SpawnImage request{};
    request.memory = memory;
    request.size = 4096;
    char const name[] = "crafted";
    for (unsigned i = 0; i < sizeof(name); i++)
        request.name[i] = name[i];
    return Sys::spawnImage(request);
}

void checkPrograms() {
    long memory = Sys::memoryCreate(4096);
    auto* file = memory > 0 ? static_cast<unsigned char*>(Sys::memoryMap(static_cast<Sys::Handle>(memory))) : nullptr;
    if (not file) {
        expect("shared memory for the test programs", memory, 0);
        return;
    }
    auto m = static_cast<Sys::Handle>(memory);
    using S = Segment*;

    expect("a good program", spawnCrafted(m, file, [](Header&, S) {}), 0);
    expect("a writable and executable segment", spawnCrafted(m, file, [](Header&, S s) { s[0].flags = R | W | X; }),
           Sys::INVALID_ARGUMENT);
    expect("a segment above 4 GiB", spawnCrafted(m, file, [](Header& h, S s) {
               s[0].vaddr = 0x140000000;
               h.entry = s[0].vaddr + CODE;
           }), Sys::INVALID_ARGUMENT);
    expect("an entry point outside executable code", spawnCrafted(m, file, [](Header&, S s) { s[0].flags = R; }),
           Sys::INVALID_ARGUMENT);
    expect("a dynamically linked program", spawnCrafted(m, file, [](Header& h, S s) {
               h.phnum = 2;
               s[1] = {INTERP, R, CODE, 0, 0, 1, 1, 1};
           }), Sys::INVALID_ARGUMENT);
    expect("a segment off its page alignment", spawnCrafted(m, file, [](Header& h, S s) {
               s[0].vaddr = BASE + 0x10;
               h.entry = s[0].vaddr + CODE;
           }), Sys::INVALID_ARGUMENT);
    expect("two segments sharing a page with different rights", spawnCrafted(m, file, [](Header& h, S s) {
               h.phnum = 2;
               s[1] = {LOAD, R | W, CODE, BASE + CODE, BASE + CODE, 8, 64, 4096};
               s[0].filesz = s[0].memsz = CODE;
               h.entry = BASE + 0x40; // inside the executable segment
           }), Sys::INVALID_ARGUMENT);

    Sys::memoryUnmap(file);
    Sys::close(m);
}

// MARK: Leaks and log text

void checkPadding() {
    Sys::SystemInfo info;
    memset(&info, 0xAA, sizeof(info));
    Sys::systemInfo(info);
    auto const* bytes = reinterpret_cast<unsigned char const*>(&info);
    unsigned long from = __builtin_offsetof(Sys::SystemInfo, reserved) + 1;
    unsigned long to = __builtin_offsetof(Sys::SystemInfo, timeOfDay);
    bool clean = true;
    for (unsigned long i = from; i < to; i++)
        clean = clean and bytes[i] == 0;
    expect("system_info padding is zeroed", clean ? 0 : -1, 0);
}

} // namespace

int main() {
    checkCycles();
    checkPrograms();
    checkPadding();
    Sys::log("harden check: this text\n[init] must not start a second log line");

    Text<> line;
    line.add("harden check: ").add(passed).add(" passed, ").add(failed).add(" failed");
    Sys::log(line.cstr());
    return failed ? 1 : 0;
}
