// init: the first Bhairav OS program, and the root of userspace.
//
// The kernel starts only init, and gives it the hardware as handles:
//   1: the framebuffer (shared memory)
//   2: the PS/2 controller's I/O ports
//   3: the keyboard interrupt
//   4: the mouse interrupt
//   5: the disk controller (AHCI), if there is one
//   6: the network card (e1000), if there is one
//   7: its interrupt line
// init decides who gets what: it starts each driver and program with just
// the handles that program needs, and finally KamalShell🪷.

#include <kapala/channel.h>
#include <kapala/protocol/fs.h>
#include <kapala/protocol/input.h>
#include <kapala/protocol/net.h>
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

// Storage: ahci drives the disk, and fs serves files from it. Returns the
// channel that connects new clients to fs.
Channel startStorage() {
    Channel diskClient, diskDriver;
    Channel::createPair(diskClient, diskDriver);
    Sys::Handle ahci[] = {Sys::INIT_HANDLE_STORAGE, diskDriver.handle()};
    if (Sys::spawn("ahci", ahci, 2) >= 0)
        diskDriver.release(); // moved to the driver
    else
        Sys::log("no disk controller: files are unavailable");
    diskDriver = Channel(); // without a driver fs sees the channel closed

    Channel connect, fsEnd;
    Channel::createPair(connect, fsEnd);
    Sys::Handle fs[] = {diskClient.release(), fsEnd.release()};
    if (Sys::spawn("fs", fs, 2) < 0)
        Sys::log("can't start the file system server");
    return connect;
}

// The network: net drives the card and runs TCP/IP. Returns the channel
// that connects new clients to it.
Channel startNetwork() {
    Channel connect, netEnd;
    Channel::createPair(connect, netEnd);
    Sys::Handle net[] = {Sys::INIT_HANDLE_NETWORK, netEnd.handle(), Sys::INIT_HANDLE_NETWORK_IRQ};
    long result = Sys::spawn("net", net, 3);
    if (result == Sys::BAD_HANDLE)
        result = Sys::spawn("net", net, 2); // a card without a usable interrupt: polled
    if (result >= 0)
        netEnd.release();
    else
        Sys::log("no network card: the network is unavailable");
    return connect;
}

// Gives `program` a new connection (to fs or net) as its handle 1.
template <typename ConnectMessage>
void spawnConnected(char const* program, Channel& connect, ConnectMessage message) {
    Channel mine, theirs;
    Channel::createPair(mine, theirs);
    if (connect.send(message, theirs.handle()) == Sys::OK)
        theirs.release();
    Sys::Handle give[] = {mine.handle()};
    if (Sys::spawn(program, give, 1) >= 0)
        mine.release();
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

    Channel files = startStorage();
    spawnConnected("fs-test", files, Protocol::Fs::Connect{Protocol::Fs::CONNECT_MAGIC});
    Channel network = startNetwork();
    spawnConnected("net-test", network, Protocol::Net::Connect{Protocol::Net::CONNECT_MAGIC});

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

    // Leave the boot screen up for a moment, then hand the screen, the
    // input, files and the network to KamalShell, which starts the apps.
    Sys::sleep(1500);
    Sys::Handle shell[] = {Sys::INIT_HANDLE_FRAMEBUFFER, input.release(), files.release(), network.release()};
    if (Sys::spawn("kamalshell", shell, 4) < 0)
        Sys::log("can't start KamalShell");

    for (;;)
        Sys::sleep(60 * 1000);
}
