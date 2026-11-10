// init: the first Bhairav OS program, and the root of userspace.
//
// The kernel starts only init, and gives it the hardware as handles:
//   1: the framebuffer (shared memory)
//   2: the PS/2 controller's I/O ports
//   3: the keyboard interrupt
//   4: the mouse interrupt
// init decides who gets what: it starts each driver and program with just
// the handles that program needs. In the next step it starts KamalShell🪷.

#include <kapala/channel.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

// MARK: IPC check

struct Number {
    unsigned long n;
};

constexpr unsigned long DONE = ~0ul;
constexpr unsigned long ROUND_TRIPS = 1000;

void copy(char* dest, char const* src) {
    while ((*dest++ = *src++))
        ;
}

// Starts ipc-test with one end of a channel and a page of shared memory,
// times message round trips, then reads its reply from the shared memory.
void checkIpc() {
    Channel mine, theirs;
    Channel::createPair(mine, theirs);

    long memory = Sys::memoryCreate(4096);
    auto* shared = memory > 0 ? static_cast<char*>(Sys::memoryMap(memory)) : nullptr;
    if (not shared) {
        Sys::log("ipc check: can't create shared memory");
        return;
    }
    copy(shared, "namaste from init");

    Sys::Handle give[] = {theirs.release(), static_cast<Sys::Handle>(memory)};
    if (Sys::spawn("ipc-test", give, 2) < 0) {
        Sys::log("ipc check: can't start ipc-test");
        return;
    }

    unsigned long start = Sys::timeMs();
    for (unsigned long i = 0; i < ROUND_TRIPS; i++) {
        Number reply;
        if (mine.send(Number{i}) != Sys::OK or mine.recv(reply) != Sys::OK or reply.n != i + 1) {
            Sys::log("ipc check: FAILED, wrong or missing reply");
            return;
        }
    }
    unsigned long elapsed = Sys::timeMs() - start;

    Number done;
    mine.send(Number{DONE});
    mine.recv(done);

    Text<> timing;
    timing.add("ipc check: ").add(ROUND_TRIPS).add(" message round trips in ").add(elapsed).add(" ms");
    Sys::log(timing.cstr());

    Text<> reply;
    reply.add("ipc check: shared memory now says: \"").add(shared).add("\"");
    Sys::log(reply.cstr());
}

// Without a screen: echo typed lines to the log.
[[noreturn]] void echoKeyboard(Channel& input) {
    Sys::log("no screen: type something and press Enter");
    Text<128> line;
    for (;;) {
        Protocol::InputEvent event;
        if (input.recv(event) != Sys::OK)
            Sys::exit(1);
        if (event.type != Protocol::InputType::Key or not event.pressed or event.character == 0)
            continue;
        if (event.character == '\n') {
            Text<> echo;
            echo.add("you typed: ").add(line.cstr());
            Sys::log(echo.cstr());
            line = {};
        } else {
            char text[2] = {event.character, 0};
            line.add(text);
        }
    }
}

} // namespace

int main() {
    Text<> hello;
    hello.add("Bhairav OS userspace is up. I am init, task ").add(Sys::taskId());
    Sys::log(hello.cstr());

    Sys::spawn("protect-test");
    Sys::spawn("fpu-test"); // two at once: their floating-point
    Sys::spawn("fpu-test"); // registers must not mix
    checkIpc();

    // Input: the PS/2 driver gets the controller and both IRQs, and sends
    // events to whoever holds the other end of `input`.
    Channel input, driverEnd;
    Channel::createPair(input, driverEnd);
    Sys::Handle ps2[] = {Sys::INIT_HANDLE_PS2_PORTS, Sys::INIT_HANDLE_KEYBOARD_IRQ,
                         Sys::INIT_HANDLE_MOUSE_IRQ, driverEnd.release()};
    if (Sys::spawn("ps2", ps2, 4) < 0)
        Sys::log("can't start the PS/2 driver");

    Sys::SystemInfo info;
    Sys::systemInfo(info);
    if (info.screenWidth == 0)
        echoKeyboard(input);

    // Leave the boot screen up for a moment, then hand the screen and the
    // input to the showcase app.
    Sys::sleep(1500);
    Sys::Handle app[] = {Sys::INIT_HANDLE_FRAMEBUFFER, input.release()};
    if (Sys::spawn("showcase", app, 2) < 0)
        Sys::log("can't start the showcase");

    for (;;)
        Sys::sleep(60 * 1000);
}
