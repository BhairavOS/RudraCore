// init: the first Bhairav OS program, and the root of userspace.
//
// The kernel starts only init, and gives it the hardware as handles:
//   1: the framebuffer (shared memory)
//   2: the PS/2 controller's I/O ports
//   3: the keyboard interrupt
//   4: the mouse interrupt
//   5: the disk controller (AHCI), if there is one
//   6: the network card (Intel e1000 or Realtek RTL8139), if there is one
//   7: its interrupt line
// init decides who gets what: it starts each driver and program with just
// the handles that program needs, and finally KamalShell🪷.

#include <kapala/channel.h>
#include <kapala/protocol/fs.h>
#include <kapala/protocol/input.h>
#include <kapala/protocol/net.h>
#include <kapala/sys.h>
#include <kapala/text.h>

#include "config.h" // build options (Makefile)

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

// The network: a driver for the card (chosen by its PCI vendor), and net,
// which runs TCP/IP on top of it. Returns the channel that connects new
// clients to net.
Channel startNetwork() {
    Channel driverSide, netSide;
    Channel::createPair(driverSide, netSide);

    Sys::DeviceInfo card{};
    bool haveCard = Sys::deviceInfo(Sys::INIT_HANDLE_NETWORK, card) == Sys::OK;
    char const* driver = card.vendor == 0x10EC ? "rtl8139" : "e1000";
    if (haveCard) {
        Sys::Handle give[] = {Sys::INIT_HANDLE_NETWORK, driverSide.handle(), Sys::INIT_HANDLE_NETWORK_IRQ};
        long result = Sys::spawn(driver, give, 3);
        if (result == Sys::BAD_HANDLE)
            result = Sys::spawn(driver, give, 2); // a card without a usable interrupt: polled
        if (result >= 0)
            driverSide.release();
    } else {
        Sys::log("no network card: the network is unavailable");
    }
    driverSide = Channel(); // without a driver, net sees the channel closed

    Channel connect, netEnd;
    Channel::createPair(connect, netEnd);
    Sys::Handle net[] = {netSide.handle(), netEnd.handle()};
    if (Sys::spawn("net", net, 2) >= 0) {
        netSide.release();
        netEnd.release();
    }
    return connect;
}

// Starts `program` with one new connection (to fs or net) per message, as
// its handles 1, 2, 3...
template <typename ConnectMessage, unsigned long N>
void spawnConnected(char const* program, Channel& connect, ConnectMessage const (&messages)[N]) {
    Sys::Handle give[N];
    for (unsigned long i = 0; i < N; i++) {
        Channel mine, theirs;
        Channel::createPair(mine, theirs);
        if (connect.send(messages[i], theirs.handle()) == Sys::OK)
            theirs.release();
        give[i] = mine.release();
    }
    if (Sys::spawn(program, give, N) < 0) {
        for (Sys::Handle h : give)
            Sys::close(h);
    }
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

// The kernel gives init every right to the hardware. Each driver gets only
// what it uses: no passing a device on (TRANSFER), no copies (DUPLICATE),
// no revoking. Missing hardware just makes a call fail.
void limitDevices() {
    constexpr unsigned long PCI = Sys::RIGHT_READ | Sys::RIGHT_MMIO | Sys::RIGHT_DMA;
    Sys::restrictRights(Sys::INIT_HANDLE_STORAGE, PCI);
    Sys::restrictRights(Sys::INIT_HANDLE_NETWORK, PCI);
    Sys::restrictRights(Sys::INIT_HANDLE_NETWORK_IRQ, Sys::RIGHT_READ);
    Sys::restrictRights(Sys::INIT_HANDLE_PS2_PORTS, Sys::RIGHT_READ | Sys::RIGHT_WRITE);
    Sys::restrictRights(Sys::INIT_HANDLE_KEYBOARD_IRQ, Sys::RIGHT_READ);
    Sys::restrictRights(Sys::INIT_HANDLE_MOUSE_IRQ, Sys::RIGHT_READ);
    Sys::restrictRights(Sys::INIT_HANDLE_FRAMEBUFFER, Sys::RIGHT_READ | Sys::RIGHT_WRITE);
}

int main() {
    Text<> hello;
    hello.add("Bhairav OS userspace is up. I am init, task ").add(Sys::taskId());
    Sys::log(hello.cstr());
    limitDevices();

#if BHAIRAV_SECURITY_TEST
    // First, alone: it takes all free memory for a moment, so nothing else
    // may be starting. Its channel closes when the kernel has freed it.
    {
        Channel mine, theirs;
        Channel::createPair(mine, theirs);
        Sys::Handle give[] = {theirs.handle()};
        if (Sys::spawn("oom-test", give, 1) >= 0) {
            theirs.release();
            char nothing;
            mine.recv(nothing); // returns PEER_CLOSED
        }
    }
#endif

    Sys::spawn("protect-test");
    Sys::spawn("harden-test");
    Sys::spawn("fpu-test"); // two at once: their floating-point
    Sys::spawn("fpu-test"); // registers must not mix
    checkIpc();

    Channel files = startStorage();
    // fs-test also checks the scopes: one connection of each kind.
    using Protocol::Fs::Scope;
    Protocol::Fs::Connect const fsTest[] = {Protocol::Fs::makeConnect(Scope::System, "fs-test"),
                                            Protocol::Fs::makeConnect(Scope::User, "fs-test"),
                                            Protocol::Fs::makeConnect(Scope::Private, "fs-test")};
    spawnConnected("fs-test", files, fsTest);
    Channel network = startNetwork();
    // net-test also checks the access levels: one connection of each kind.
    using Protocol::Net::Access;
    Protocol::Net::Connect const netTest[] = {Protocol::Net::makeConnect(Access::Internet, "net-test"),
                                              Protocol::Net::makeConnect(Access::None, "net-test"),
                                              Protocol::Net::makeConnect(Access::Local, "net-test")};
    spawnConnected("net-test", network, netTest);
    Protocol::Net::Connect const tlsTest[] = {Protocol::Net::makeConnect(Access::Internet, "tls-test")};
    spawnConnected("tls-test", network, tlsTest);

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
