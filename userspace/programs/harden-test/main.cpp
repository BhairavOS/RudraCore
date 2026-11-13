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
long spawnCrafted(Sys::Handle memory, unsigned char* file, Tweak tweak, Sys::Handle const* give = nullptr,
                  unsigned long giveCount = 0) {
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
    for (unsigned long i = 0; i < giveCount; i++)
        request.handles[i] = give[i];
    request.handleCount = giveCount;
    return Sys::spawnImage(request);
}

// Revoking memory must take it away from a program that has it mapped. A
// child maps our memory (its handle 1), says so on its handle 2, then reads
// the memory forever. After the revoke its next read must fault, ending it,
// which closes its channel.
void checkRevokeUnmaps(Sys::Handle elfMemory, unsigned char* file) {
    static constexpr unsigned char READER[] = {
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1          (handle 1)
        0xB8, 0x15, 0x00, 0x00, 0x00, // mov eax, 21         (memory_map)
        0xCD, 0x80,                   // int 0x80
        0x48, 0x89, 0xC3,             // mov rbx, rax
        0xBF, 0x02, 0x00, 0x00, 0x00, // mov edi, 2          (handle 2)
        0x48, 0x89, 0xDE,             // mov rsi, rbx        (1 byte of the mapping)
        0xBA, 0x01, 0x00, 0x00, 0x00, // mov edx, 1
        0x45, 0x31, 0xD2,             // xor r10d, r10d      (no handle)
        0xB8, 0x0C, 0x00, 0x00, 0x00, // mov eax, 12         (channel_send)
        0xCD, 0x80,                   // int 0x80
        0x8A, 0x03,                   // spin: mov al, [rbx]
        0xEB, 0xFC,                   //       jmp spin
    };
    auto shared = static_cast<Sys::Handle>(Sys::memoryCreate(4096));
    long lent = Sys::duplicate(shared, Sys::RIGHT_READ | Sys::RIGHT_TRANSFER);
    Sys::Handle mine, theirs;
    Sys::channelCreate(mine, theirs);
    Sys::Handle give[] = {static_cast<Sys::Handle>(lent), theirs};
    long child = spawnCrafted(elfMemory, file, [](Header&, Segment* s) {
        auto* code = reinterpret_cast<unsigned char*>(s) - sizeof(Header) + CODE;
        for (unsigned i = 0; i < sizeof(READER); i++)
            code[i] = READER[i];
        s[0].filesz = s[0].memsz = CODE + sizeof(READER);
    }, give, 2);
    expect("starting the reader", child, 0);
    if (child >= 0) {
        char mapped;
        Sys::recv(mine, &mapped, 1); // it has the memory mapped and is reading it
        Sys::revoke(shared);
        Sys::Handle ends[] = {mine};
        long ready = Sys::wait(ends, 1, 2000);
        char gone;
        expect("revoking takes memory away from another program",
               ready == 0 ? Sys::recv(mine, &gone, 1, nullptr, Sys::RECV_NONBLOCK) : ready, Sys::PEER_CLOSED);
    }
    Sys::close(mine);
    Sys::close(shared);
}

// A program started by anything but init is an app, and apps can't start
// programs. The child tries spawn("fpu-test") and sends us the result.
void checkAppCantSpawn(Sys::Handle elfMemory, unsigned char* file) {
    static constexpr unsigned char SPAWNER[] = {
        0x48, 0x8D, 0x3D, 0x32, 0x00, 0x00, 0x00, // lea rdi, [rip + 50]   (the name, below)
        0xBE, 0x08, 0x00, 0x00, 0x00,             // mov esi, 8
        0x31, 0xD2,                               // xor edx, edx          (no handles)
        0x45, 0x31, 0xD2,                         // xor r10d, r10d
        0xB8, 0x06, 0x00, 0x00, 0x00,             // mov eax, 6            (spawn)
        0xCD, 0x80,                               // int 0x80
        0x50,                                     // push rax              (the result)
        0xBF, 0x01, 0x00, 0x00, 0x00,             // mov edi, 1            (handle 1)
        0x48, 0x89, 0xE6,                         // mov rsi, rsp
        0xBA, 0x08, 0x00, 0x00, 0x00,             // mov edx, 8
        0x45, 0x31, 0xD2,                         // xor r10d, r10d
        0xB8, 0x0C, 0x00, 0x00, 0x00,             // mov eax, 12           (channel_send)
        0xCD, 0x80,                               // int 0x80
        0xB8, 0x01, 0x00, 0x00, 0x00,             // mov eax, 1            (exit)
        0x31, 0xFF,                               // xor edi, edi
        0xCD, 0x80,                               // int 0x80
        'f', 'p', 'u', '-', 't', 'e', 's', 't',
    };
    Sys::Handle mine, theirs;
    Sys::channelCreate(mine, theirs);
    Sys::Handle give[] = {theirs};
    long child = spawnCrafted(elfMemory, file, [](Header&, Segment* s) {
        auto* code = reinterpret_cast<unsigned char*>(s) - sizeof(Header) + CODE;
        for (unsigned i = 0; i < sizeof(SPAWNER); i++)
            code[i] = SPAWNER[i];
        s[0].filesz = s[0].memsz = CODE + sizeof(SPAWNER);
    }, give, 1);
    long result = 0;
    if (child >= 0 and Sys::recv(mine, &result, sizeof(result)) == sizeof(result))
        expect("an app starting a program", result, Sys::ACCESS_DENIED);
    else
        expect("starting the spawner app", child, 0);
    Sys::close(mine);
}

// A spinning app must not slow a system task: system tasks run first and
// preempt apps at the next 1 ms tick. Before that rule, a sleeper woke up
// to 10 ms late (one app time slice). Runs once the boot's own busy checks
// (fpu-test) are over, so they don't compete with the measurement.
void checkAppCantHogCpu(Sys::Handle elfMemory, unsigned char* file) {
    static constexpr unsigned char SPINNER[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, // mov eax, 5 (time); int 0x80
        0x48, 0x8D, 0x98, 0xDC, 0x05, 0x00, 0x00, // lea rbx, [rax + 1500]
        0xB8, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, // spin: time
        0x48, 0x39, 0xD8,                         // cmp rax, rbx
        0x72, 0xF4,                               // jb spin
        0xB8, 0x01, 0x00, 0x00, 0x00,             // mov eax, 1 (exit)
        0x31, 0xFF, 0xCD, 0x80,                   // xor edi, edi; int 0x80
    };
    while (Sys::timeMs() < 9000)
        Sys::sleep(250);
    long child = spawnCrafted(elfMemory, file, [](Header&, Segment* s) {
        auto* code = reinterpret_cast<unsigned char*>(s) - sizeof(Header) + CODE;
        for (unsigned i = 0; i < sizeof(SPINNER); i++)
            code[i] = SPINNER[i];
        s[0].filesz = s[0].memsz = CODE + sizeof(SPINNER);
    });
    expect("starting the spinning app", child, 0);
    unsigned long worst = 0;
    for (int i = 0; i < 40; i++) {
        unsigned long before = Sys::timeMs();
        Sys::sleep(5);
        unsigned long late = Sys::timeMs() - before - 5;
        worst = late > worst ? late : worst;
    }
    Text<> line;
    line.add("harden check: worst wake-up delay with a spinning app: ").add(worst).add(" ms");
    Sys::log(line.cstr());
    expect("a spinning app delays a system task", worst <= 2 ? 0 : -1, 0);
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

    checkRevokeUnmaps(m, file);
    checkAppCantSpawn(m, file);
    checkAppCantHogCpu(m, file);

    Sys::memoryUnmap(file);
    Sys::close(m);
}

// MARK: Capabilities

void checkCapabilities() {
    char byte = 1, got;

    // Generation counters: a freed slot comes back under a new number.
    Sys::Handle a, b, c, d;
    Sys::channelCreate(a, b);
    Sys::close(a);
    Sys::channelCreate(c, d); // takes a's slot
    expect("a reused slot gets a new handle number", c != a ? 0 : -1, 0);
    expect("using a closed handle", Sys::send(a, &byte, 1), Sys::BAD_HANDLE);
    Sys::close(b);

    // Send-only and receive-only channel ends; rights never grow.
    long sendOnly = Sys::duplicate(c, Sys::RIGHT_WRITE);
    expect("a send-only duplicate", sendOnly, 0);
    expect("a send-only end sends", Sys::send(sendOnly, &byte, 1), 0);
    expect("a send-only end can't receive", Sys::recv(sendOnly, &got, 1, nullptr, Sys::RECV_NONBLOCK),
           Sys::ACCESS_DENIED);
    expect("a send-only end can't be duplicated", Sys::duplicate(sendOnly, Sys::RIGHTS_ALL), Sys::ACCESS_DENIED);
    Sys::restrictRights(sendOnly, Sys::RIGHTS_ALL);
    expect("restricting never adds rights", Sys::rights(sendOnly), Sys::RIGHT_WRITE);
    Sys::restrictRights(d, Sys::RIGHT_READ);
    expect("a receive-only end can't send", Sys::send(d, &byte, 1), Sys::ACCESS_DENIED);
    expect("a receive-only end receives", Sys::recv(d, &got, 1, nullptr, Sys::RECV_NONBLOCK), 0);

    // Moving a handle needs TRANSFER.
    Sys::Handle x, y;
    Sys::channelCreate(x, y);
    long fixed = Sys::duplicate(c, Sys::RIGHT_WRITE);
    expect("moving a handle without TRANSFER", Sys::send(x, &byte, 1, static_cast<Sys::Handle>(fixed)),
           Sys::ACCESS_DENIED);

    // Read-only memory: the kernel won't write into it on our behalf either.
    auto mem = static_cast<Sys::Handle>(Sys::memoryCreate(4096));
    long readOnly = Sys::duplicate(mem, Sys::RIGHT_READ | Sys::RIGHT_TRANSFER);
    void* view = Sys::memoryMap(static_cast<Sys::Handle>(readOnly));
    expect("mapping a read-only duplicate", view ? 0 : -1, 0);
    expect("read-only memory isn't writable", Sys::random(view, 16), Sys::BAD_ADDRESS);
    Sys::memoryUnmap(view);

    // Revocation: every other handle stops working, even one in flight.
    expect("revoking", Sys::revoke(mem), 0);
    expect("a revoked handle stops working", Sys::rights(static_cast<Sys::Handle>(readOnly)), Sys::REVOKED);
    expect("the revoker keeps its handle", Sys::rights(mem), 0);
    expect("a revoked handle can't be moved", Sys::send(x, &byte, 1, static_cast<Sys::Handle>(readOnly)),
           Sys::REVOKED);
    long traveler = Sys::duplicate(c, Sys::RIGHT_WRITE | Sys::RIGHT_TRANSFER);
    Sys::send(x, &byte, 1, static_cast<Sys::Handle>(traveler));
    Sys::revoke(c);
    Sys::Handle arrived = 1234;
    Sys::recv(y, &got, 1, &arrived, Sys::RECV_NONBLOCK);
    expect("a handle revoked on the way arrives as nothing", arrived == Sys::NO_HANDLE ? 0 : -1, 0);
    expect("revoke without the right", Sys::revoke(d), Sys::ACCESS_DENIED);
    long const opened[] = {static_cast<long>(c), static_cast<long>(d), sendOnly, fixed, static_cast<long>(x),
                           static_cast<long>(y), static_cast<long>(mem), readOnly};
    for (long h : opened)
        Sys::close(static_cast<Sys::Handle>(h));

    // Object quota: park channel ends in queues (so few handles stay open)
    // until the kernel stops this program; closing them all gives it back.
    constexpr int MAX_SINKS = 48;
    Sys::Handle sinks[MAX_SINKS];
    int sinkCount = 0;
    long last = 0;
    while (last >= 0 and sinkCount < MAX_SINKS) {
        Sys::Handle in, sink;
        if ((last = Sys::channelCreate(in, sink)) < 0)
            break;
        sinks[sinkCount++] = sink;
        for (int i = 0; i < 32 and last >= 0; i++) {
            Sys::Handle e, f;
            if ((last = Sys::channelCreate(e, f)) >= 0) {
                Sys::send(in, &byte, 1, e);
                Sys::close(f);
            }
        }
        Sys::close(in);
    }
    expect("the object quota stops a program", last, Sys::OUT_OF_MEMORY);
    for (int i = 0; i < sinkCount; i++)
        Sys::close(sinks[i]);
    expect("closing objects gives the quota back", Sys::channelCreate(a, b), 0);
    Sys::close(a);
    Sys::close(b);
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
    checkCapabilities();
    Sys::log("harden check: this text\n[init] must not start a second log line");

    Text<> line;
    line.add("harden check: ").add(passed).add(" passed, ").add(failed).add(" failed");
    Sys::log(line.cstr());
    return failed ? 1 : 0;
}
