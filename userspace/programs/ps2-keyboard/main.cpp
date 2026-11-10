// ps2-keyboard: the PS/2 keyboard driver, running as an ordinary program.
//
// It can only touch what init gave it:
//   handle 1: the PS/2 controller's I/O ports (0x60-0x64)
//   handle 2: the keyboard interrupt (IRQ 1)
//   handle 3: a channel to send key events on
// If it crashes, the kernel and everything else keep running.

#include <kapala/channel.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>

using namespace Kapala;

namespace {

constexpr Sys::Handle PORTS = 1;
constexpr Sys::Handle IRQ = 2;
constexpr Sys::Handle EVENTS = 3;

constexpr unsigned short DATA = 0x60;
constexpr unsigned short STATUS = 0x64;
constexpr unsigned short COMMAND = 0x64;

constexpr unsigned char STATUS_OUTPUT_FULL = 1 << 0;
constexpr unsigned char STATUS_INPUT_FULL = 1 << 1;
constexpr unsigned char STATUS_FROM_MOUSE = 1 << 5;

constexpr unsigned char CMD_READ_CONFIG = 0x20;
constexpr unsigned char CMD_WRITE_CONFIG = 0x60;
constexpr unsigned char CMD_ENABLE_KEYBOARD = 0xAE;

constexpr unsigned char CONFIG_KEYBOARD_IRQ = 1 << 0;
constexpr unsigned char CONFIG_KEYBOARD_CLOCK_OFF = 1 << 4;
constexpr unsigned char CONFIG_TRANSLATE = 1 << 6; // deliver scancode set 1

constexpr unsigned char LEFT_SHIFT = 0x2A;
constexpr unsigned char RIGHT_SHIFT = 0x36;
constexpr unsigned char EXTENDED_PREFIX = 0xE0;

// Scancode set 1 to characters, US layout.
constexpr char NORMAL[] =
    "\0\x1b" "1234567890-=" "\b\t" "qwertyuiop[]" "\n\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
constexpr char SHIFTED[] =
    "\0\x1b" "!@#$%^&*()_+" "\b\t" "QWERTYUIOP{}" "\n\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";
constexpr unsigned long TABLE_SIZE = sizeof(NORMAL) - 1;

unsigned char status() { return static_cast<unsigned char>(Sys::portIn(PORTS, STATUS)); }

bool waitUntil(unsigned char mask, bool set) {
    for (int tries = 0; tries < 100000; tries++) {
        if (((status() & mask) != 0) == set)
            return true;
    }
    return false;
}

void command(unsigned char cmd) {
    waitUntil(STATUS_INPUT_FULL, false);
    Sys::portOut(PORTS, COMMAND, cmd);
}

void writeData(unsigned char value) {
    waitUntil(STATUS_INPUT_FULL, false);
    Sys::portOut(PORTS, DATA, value);
}

unsigned char readData() {
    waitUntil(STATUS_OUTPUT_FULL, true);
    return static_cast<unsigned char>(Sys::portIn(PORTS, DATA));
}

// Firmware (especially UEFI) may leave keyboard interrupts off: turn them on.
void setupController() {
    while (status() & STATUS_OUTPUT_FULL)
        Sys::portIn(PORTS, DATA); // drop stale bytes

    command(CMD_READ_CONFIG);
    unsigned char config = readData();
    config |= CONFIG_KEYBOARD_IRQ | CONFIG_TRANSLATE;
    config &= static_cast<unsigned char>(~CONFIG_KEYBOARD_CLOCK_OFF);
    command(CMD_WRITE_CONFIG);
    writeData(config);
    command(CMD_ENABLE_KEYBOARD);
}

} // namespace

int main() {
    Channel events(EVENTS);
    setupController();
    Sys::log("ready: keyboard interrupts on, sending key events to init");

    bool shift = false;
    bool extended = false;

    for (;;) {
        if (Sys::irqWait(IRQ) != Sys::OK)
            return 1;

        while (status() & STATUS_OUTPUT_FULL) {
            bool fromMouse = status() & STATUS_FROM_MOUSE;
            auto byte = static_cast<unsigned char>(Sys::portIn(PORTS, DATA));
            if (fromMouse)
                continue;

            if (byte == EXTENDED_PREFIX) {
                extended = true;
                continue;
            }

            bool pressed = not(byte & 0x80);
            unsigned char code = byte & 0x7F;

            if (extended) { // arrows, right ctrl...: not mapped yet
                extended = false;
                continue;
            }

            if (code == LEFT_SHIFT or code == RIGHT_SHIFT)
                shift = pressed;

            char character = 0;
            if (code < TABLE_SIZE)
                character = shift ? SHIFTED[code] : NORMAL[code];

            if (events.send(Protocol::KeyEvent{code, pressed, character, 0}) == Sys::PEER_CLOSED)
                return 0; // nobody is listening anymore
        }
    }
}
