// protect-test: checks at every boot that user programs are isolated.
//
// 1. The kernel must reject a pointer into kernel memory passed to a
//    system call (it returns BAD_ADDRESS instead of reading it).
// 2. Reading kernel memory directly must fault. The kernel then ends this
//    task and keeps running.

#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

int main() {
    auto const* kernelText = reinterpret_cast<char const*>(0xffffffff80001000);

    long result = Sys::log(kernelText, 16);
    Text<> line;
    line.add("syscall with a kernel pointer returned ").add(result);
    line.add(result == Sys::BAD_ADDRESS ? " (rejected, good)" : " (NOT rejected!)");
    Sys::log(line.cstr());

    Sys::log("now reading kernel memory directly: this task should be stopped");
    volatile char c = *kernelText;
    (void)c;

    Sys::log("ERROR: kernel memory was readable from userspace");
    return 1;
}
