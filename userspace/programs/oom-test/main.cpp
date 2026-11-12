// oom-test: checks that programs can't use up memory the kernel or other
// programs need. Only run in SECURITY_TEST=1 builds (it briefly takes all
// free RAM, which is slow on a big machine).
//
// 1. Quota: one program can't hold more than half of RAM.
// 2. Reserve: with a helper (a second copy of this program) taking the
//    rest, memory runs out, and then everything that needs kernel memory (a
//    channel, shared memory, a new task) must fail with OUT_OF_MEMORY: the
//    kernel keeps a reserve programs can't touch, so it doesn't panic.
//
// Handle 1: a channel to init, which sees it close once the memory is free.
// The helper finds a message waiting on its handle 1 when it starts.

#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

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
    takeEverything();
    char full = 1;
    Sys::send(1, &full, 1);
    char quit;
    Sys::recv(1, &quit, 1); // until told to go (or the test ended)
    Sys::exit(0);
}

} // namespace

int main() {
    char role;
    if (Sys::recv(1, &role, 1, nullptr, Sys::RECV_NONBLOCK) == 1)
        helper();

    Sys::SystemInfo info{};
    Sys::systemInfo(info);
    unsigned long taken = takeEverything();
    Text<> line;
    line.add("oom check: one program got ").add(taken >> 20).add(" MiB of ").add(info.totalMemory >> 20)
        .add(" MiB before its quota ran out");
    Sys::log(line.cstr());
    bool ok = taken <= info.totalMemory / 2;
    Sys::log(ok ? "oom check: quota holds (at most half of RAM)" : "oom check: quota NOT enforced!");

    // The helper takes what's left. Its handle 1 gets a message first, so it
    // knows its role.
    Sys::Handle mine, theirs;
    Sys::channelCreate(mine, theirs);
    char go = 1;
    Sys::send(mine, &go, 1);
    Sys::Handle give[] = {theirs};
    if (Sys::spawn("oom-test", give, 1) < 0) {
        Sys::log("oom check: FAILED, can't start the helper");
        return 1;
    }
    char full;
    Sys::recv(mine, &full, 1);

    Sys::Handle a, b;
    ok = expectOutOfMemory("channel_create", Sys::channelCreate(a, b)) and ok;
    ok = expectOutOfMemory("memory_create", Sys::memoryCreate(4096)) and ok;
    ok = expectOutOfMemory("spawn", Sys::spawn("fpu-test")) and ok;

    // Let the helper go, and wait until the kernel has freed it.
    Sys::send(mine, &go, 1);
    while (Sys::recv(mine, &full, 1) != Sys::PEER_CLOSED)
        ;

    Sys::log(ok ? "oom check: passed" : "oom check: FAILED");
    return ok ? 0 : 1;
}
