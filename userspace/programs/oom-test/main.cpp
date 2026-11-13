// oom-test: checks that programs can't use up memory the kernel or other
// programs need. Only run in SECURITY_TEST=1 builds (it briefly takes all
// free RAM, which is slow on a big machine).
//
// 1. Quotas: this program (a system service, started by init) can't hold
//    more than half of RAM; the helpers it starts are apps, held to a quarter.
// 2. Reserve: once the helpers have taken the rest, memory runs out, and
//    then everything that needs kernel memory (a channel, shared memory, a
//    new task) must fail with OUT_OF_MEMORY: the kernel keeps a reserve
//    programs can't touch, so it doesn't panic.
//
// Handle 1: a channel to init, which sees it close once the memory is free.
// A helper finds a message waiting on its handle 1 when it starts.

#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

constexpr int MAX_HELPERS = 6;

// Takes memory in shrinking chunks until not even one page is left.
// Returns how much it got.
unsigned long takeEverything() {
    unsigned long taken = 0;
    for (unsigned long chunk = 64ul << 20; chunk >= 4096; chunk /= 2) {
        while (Sys::memoryAlloc(chunk))
            taken += chunk;
    }
    // Shared memory needs a little less than private memory: take that too.
    while (Sys::memoryCreate(4096) > 0)
        taken += 4096;
    return taken;
}

bool expectOutOfMemory(char const* what, long result) {
    Text<> line;
    line.add("oom check: ").add(what).add(" returned ").add(result);
    bool ok = result == Sys::OUT_OF_MEMORY;
    line.add(ok ? " (refused, good)" : " (NOT refused!)");
    Sys::log(line.cstr());
    return ok;
}

[[noreturn]] void helper() {
    unsigned long taken = takeEverything();
    Sys::send(1, &taken, sizeof(taken));
    char quit;
    Sys::recv(1, &quit, 1); // until told to go (or the test ended)
    Sys::exit(0);
}

bool reserveReached() {
    Sys::Handle a, b;
    long r = Sys::channelCreate(a, b);
    if (r == Sys::OUT_OF_MEMORY)
        return true;
    Sys::close(a);
    Sys::close(b);
    return false;
}

} // namespace

int main() {
    unsigned long role;
    if (Sys::recv(1, &role, sizeof(role), nullptr, Sys::RECV_NONBLOCK) == sizeof(role))
        helper();

    Sys::SystemInfo info{};
    Sys::systemInfo(info);
    unsigned long total = info.totalMemory;
    unsigned long taken = takeEverything();
    Text<> line;
    line.add("oom check: this system service got ").add(taken >> 20).add(" MiB of ").add(total >> 20)
        .add(" MiB before its quota ran out");
    Sys::log(line.cstr());
    bool ok = taken <= total / 2;

    // Helpers (apps) take the rest, until the kernel's reserve is all that's left.
    Sys::Handle helpers[MAX_HELPERS];
    int count = 0;
    while (count < MAX_HELPERS and not reserveReached()) {
        Sys::Handle mine, theirs;
        Sys::channelCreate(mine, theirs);
        Sys::send(mine, &role, sizeof(role));
        Sys::Handle give[] = {theirs};
        if (Sys::spawn("oom-test", give, 1) < 0) {
            Sys::close(mine);
            Sys::close(theirs);
            break;
        }
        helpers[count++] = mine;
        unsigned long got = 0;
        Sys::recv(mine, &got, sizeof(got));
        line = {};
        line.add("oom check: app helper ").add(count).add(" got ").add(got >> 20).add(" MiB");
        Sys::log(line.cstr());
        ok = got <= total / 4 and ok;
    }
    Sys::log(ok ? "oom check: quotas hold (system service: half of RAM, app: a quarter)"
                : "oom check: a quota was NOT enforced!");

    Sys::Handle a, b;
    ok = expectOutOfMemory("channel_create", Sys::channelCreate(a, b)) and ok;
    ok = expectOutOfMemory("memory_create", Sys::memoryCreate(4096)) and ok;
    ok = expectOutOfMemory("spawn", Sys::spawn("fpu-test")) and ok;

    // Let the helpers go, and wait until the kernel has freed each.
    for (int i = 0; i < count; i++) {
        char go = 1, gone;
        Sys::send(helpers[i], &go, 1);
        while (Sys::recv(helpers[i], &gone, 1) != Sys::PEER_CLOSED)
            ;
    }

    Sys::log(ok ? "oom check: passed" : "oom check: FAILED");
    return ok ? 0 : 1;
}
