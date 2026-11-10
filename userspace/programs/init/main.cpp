// init: the first Bhairav OS program, and the root of userspace.
//
// The kernel starts only init, and gives it the hardware as handles:
//   1: the framebuffer (shared memory)
//   2: the PS/2 controller's I/O ports
//   3: the keyboard interrupt
// init decides who gets what: it starts each driver and service with just
// the handles that program needs. Later it will start KamalShell🪷.

#include <kapala/channel.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

// MARK: Screen

struct Screen {
    unsigned int* pixels = nullptr;
    unsigned long width = 0, height = 0, stride = 0; // stride in pixels
    unsigned char rShift = 16, gShift = 8, bShift = 0;

    unsigned int pack(unsigned r, unsigned g, unsigned b) const {
        return (r << rShift) | (g << gShift) | (b << bShift);
    }

    // Mixes a color into a pixel; alpha 0..255.
    void blend(long x, long y, unsigned r, unsigned g, unsigned b, unsigned alpha) {
        if (x < 0 or y < 0 or static_cast<unsigned long>(x) >= width or static_cast<unsigned long>(y) >= height)
            return;
        unsigned int& px = pixels[y * stride + x];
        unsigned pr = (px >> rShift) & 0xFF, pg = (px >> gShift) & 0xFF, pb = (px >> bShift) & 0xFF;
        px = pack((pr * (255 - alpha) + r * alpha) / 255,
                  (pg * (255 - alpha) + g * alpha) / 255,
                  (pb * (255 - alpha) + b * alpha) / 255);
    }
};

struct Angle {
    long sin, cos; // scaled by 1024
};

// One lotus petal leaning `a` from vertical, shaded from base color to a pale tip.
void petal(Screen& s, long bx, long by, Angle a, long len, long wid, unsigned r, unsigned g, unsigned b) {
    long cx = bx + a.sin * len / 1024, cy = by - a.cos * len / 1024;
    for (long y = cy - len; y <= cy + len; y++) {
        for (long x = cx - len; x <= cx + len; x++) {
            long dx = x - cx, dy = y - cy;
            long u = (dx * a.sin - dy * a.cos) / 1024;
            long v = (dx * a.cos + dy * a.sin) / 1024;
            long w2 = u > 0 ? wid * wid * (len - u) / len : wid * wid;
            if (w2 <= 0 or u * u * w2 + v * v * len * len > len * len * w2)
                continue;
            unsigned t = static_cast<unsigned>((u + len) * 200 / (2 * len));
            s.blend(x, y, (r * (255 - t) + 0xFF * t) / 255, (g * (255 - t) + 0xE0 * t) / 255,
                    (b * (255 - t) + 0xEA * t) / 255, 225);
        }
    }
}

// The KamalShell🪷 emblem, drawn by userspace straight into the framebuffer.
void drawLotus(Screen& s, long bx, long by, long size) {
    petal(s, bx, by, {887, 512}, size * 7 / 10, size * 3 / 10, 0xC2, 0x18, 0x5B);
    petal(s, bx, by, {-887, 512}, size * 7 / 10, size * 3 / 10, 0xC2, 0x18, 0x5B);
    petal(s, bx, by, {512, 887}, size * 9 / 10, size * 33 / 100, 0xE7, 0x54, 0x80);
    petal(s, bx, by, {-512, 887}, size * 9 / 10, size * 33 / 100, 0xE7, 0x54, 0x80);
    petal(s, bx, by, {0, 1024}, size, size * 36 / 100, 0xFF, 0x8F, 0xAB);
}

void drawOnScreen() {
    Sys::SystemInfo info;
    Sys::systemInfo(info);
    if (info.screenWidth == 0) {
        Sys::log("no framebuffer, skipping the screen check");
        return;
    }

    auto* pixels = static_cast<unsigned int*>(Sys::memoryMap(Sys::INIT_HANDLE_FRAMEBUFFER));
    if (not pixels) {
        Sys::log("can't map the framebuffer");
        return;
    }

    Screen screen{pixels, info.screenWidth, info.screenHeight, info.screenPitch / 4,
                  info.redShift, info.greenShift, info.blueShift};
    drawLotus(screen, 110, 150, 56);

    Text<> line;
    line.add("drew the lotus (top left) from userspace: ").add(info.screenWidth).add("x")
        .add(info.screenHeight).add(" framebuffer mapped via handle 1");
    Sys::log(line.cstr());
}

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

// MARK: Keyboard

[[noreturn]] void runKeyboard() {
    Channel keys, driverEnd;
    Channel::createPair(keys, driverEnd);

    Sys::Handle give[] = {Sys::INIT_HANDLE_PS2_PORTS, Sys::INIT_HANDLE_KEYBOARD_IRQ, driverEnd.release()};
    if (Sys::spawn("ps2-keyboard", give, 3) < 0) {
        Sys::log("can't start the keyboard driver");
        Sys::exit(1);
    }

    Sys::log("type something and press Enter");
    Text<128> line;
    for (;;) {
        Protocol::KeyEvent event;
        if (keys.recv(event) != Sys::OK) {
            Sys::log("the keyboard driver stopped");
            Sys::exit(1);
        }
        if (not event.pressed or event.character == 0)
            continue;

        if (event.character == '\n') {
            Text<> echo;
            echo.add("you typed: ").add(line.cstr());
            Sys::log(echo.cstr());
            line = {};
        } else if (event.character == '\b') {
            if (line.length > 0)
                line.data[--line.length] = '\0';
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

    drawOnScreen();
    Sys::spawn("protect-test");
    checkIpc();
    runKeyboard();
}
