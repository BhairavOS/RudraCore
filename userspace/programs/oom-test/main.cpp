// oom-test: checks that a program can't crash the kernel by using up all
// physical memory. Only run in SECURITY_TEST=1 builds (it briefly takes all
// free RAM, which is slow on a big machine).
//
// It takes every page it can get, then asks for things that need kernel
// memory (a channel, shared memory, a new task). Each must fail with
// OUT_OF_MEMORY: the kernel keeps a reserve programs can't touch, so its own
// allocations still succeed and it doesn't panic. Exiting gives it all back.
// Handle 1: a channel to init, which sees it close once the memory is free.

#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

bool expectOutOfMemory(char const* what, long result) {
    Text<> line;
    line.add("oom check: ").add(what).add(" returned ").add(result);
    bool ok = result == Sys::OUT_OF_MEMORY;
    line.add(ok ? " (refused, good)" : " (NOT refused!)");
    Sys::log(line.cstr());
    return ok;
}

} // namespace

int main() {
    // Take memory in shrinking chunks until not even one page is left.
    unsigned long taken = 0;
    for (unsigned long chunk = 64ul << 20; chunk >= 4096; chunk /= 2) {
        while (Sys::memoryAlloc(chunk))
            taken += chunk;
    }
    Text<> line;
    line.add("oom check: took ").add(taken >> 20).add(" MiB before running out");
    Sys::log(line.cstr());

    // Shared memory needs a little less than private memory: take that too.
    while (Sys::memoryCreate(4096) > 0)
        ;

    Sys::Handle a, b;
    bool ok = expectOutOfMemory("channel_create", Sys::channelCreate(a, b));
    ok = expectOutOfMemory("memory_create", Sys::memoryCreate(4096)) and ok;
    ok = expectOutOfMemory("spawn", Sys::spawn("fpu-test")) and ok;

    Sys::log(ok ? "oom check: passed" : "oom check: FAILED");
    return ok ? 0 : 1;
}
