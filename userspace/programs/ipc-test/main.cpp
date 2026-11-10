// ipc-test: the other side of init's IPC check.
//
//   handle 1: a channel to init
//   handle 2: shared memory init wrote a greeting into
//
// Answers every Number{n} with Number{n + 1}. When init sends DONE, it
// writes a reply into the shared memory, confirms, and exits.

#include <kapala/channel.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

struct Number {
    unsigned long n;
};

constexpr unsigned long DONE = ~0ul;

void copy(char* dest, char const* src) {
    while ((*dest++ = *src++))
        ;
}

} // namespace

int main() {
    Channel init(1);
    auto* shared = static_cast<char*>(Sys::memoryMap(2));
    if (not shared) {
        Sys::log("can't map the shared memory");
        return 1;
    }

    Text<> greeting;
    greeting.add("shared memory from init says: \"").add(shared).add("\"");
    Sys::log(greeting.cstr());

    unsigned long answered = 0;
    for (;;) {
        Number ping;
        if (init.recv(ping) != Sys::OK)
            return 1;

        if (ping.n == DONE) {
            Text<> reply;
            reply.add("namaste from ipc-test, ").add(answered).add(" pings answered");
            copy(shared, reply.cstr());
            init.send(Number{DONE});
            return 0;
        }

        init.send(Number{ping.n + 1});
        answered++;
    }
}
