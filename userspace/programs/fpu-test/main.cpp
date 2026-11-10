// fpu-test: checks that each program's floating-point registers are its own.
//
// init starts two of these at once. Each sums seed * i in a double for a
// long tight loop (no system calls, so the running sum stays in an SSE
// register), and the timer switches between them many times meanwhile. If
// the kernel didn't save and restore SSE state on every switch, one would
// see the other's values and the result would be wrong. Every partial sum
// is a whole number below 2^53, so the double result is exact.

#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

int main() {
    unsigned long seed = Sys::taskId();
    constexpr unsigned long N = 20 * 1000 * 1000;

    unsigned long start = Sys::timeMs();
    double sum = 0;
    double step = double(seed);
    for (unsigned long i = 0; i < N; i++)
        sum += step * double(i);
    unsigned long elapsed = Sys::timeMs() - start;

    unsigned long expected = seed * (N * (N - 1) / 2);
    bool ok = static_cast<unsigned long>(sum) == expected;

    Text<> line;
    line.add(ok ? "fpu check ok: " : "fpu check FAILED: ").add(N).add(" float adds in ").add(elapsed)
        .add(" ms (seed ").add(seed).add(")");
    Sys::log(line.cstr());
    return ok ? 0 : 1;
}
