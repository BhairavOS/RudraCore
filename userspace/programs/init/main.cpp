// init: the first BhairavOS program, started by RudraCore at boot.
//
// For now it shows that user programs work: it talks to the kernel through
// system calls, sleeps, and reads the time. In later steps init will start
// the system servers (framebuffer, input) and the Kamala shell.

#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

int main() {
    Text<> hello;
    hello.add("hello from userspace! I am task ").add(Sys::taskId()).add(", running in ring 3");
    Sys::log(hello.cstr());

    for (unsigned long i = 1; i <= 3; i++) {
        Sys::sleep(700);
        Text<> line;
        line.add("heartbeat ").add(i).add(" at ").add(Sys::timeMs()).add(" ms");
        Sys::log(line.cstr());
    }

    Sys::log("all good, exiting");
    return 0;
}
