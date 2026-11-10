// ps2: the PS/2 keyboard and mouse driver, running as an ordinary program.
//
// It can only touch what init gave it:
//   handle 1: the PS/2 controller's I/O ports (0x60-0x64)
//   handle 2: the keyboard interrupt (IRQ 1)
//   handle 3: the mouse interrupt (IRQ 12)
//   handle 4: a channel to send input events on
//
// Keyboard and mouse share the controller's data port, so one driver owns
// both: each byte is routed by the "from mouse" status bit. Both IRQs are
// bound to one channel, so a single loop waits for either.

#include <kapala/channel.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>

using namespace Kapala;
using Protocol::InputEvent;
using Protocol::InputType;

namespace {

constexpr Sys::Handle PORTS = 1;
constexpr Sys::Handle KEYBOARD_IRQ = 2;
constexpr Sys::Handle MOUSE_IRQ = 3;
constexpr Sys::Handle EVENTS = 4;

constexpr unsigned short DATA = 0x60;
constexpr unsigned short STATUS = 0x64;
constexpr unsigned short COMMAND = 0x64;

constexpr unsigned char STATUS_OUTPUT_FULL = 1 << 0;
constexpr unsigned char STATUS_INPUT_FULL = 1 << 1;
constexpr unsigned char STATUS_FROM_MOUSE = 1 << 5;

constexpr unsigned char CMD_READ_CONFIG = 0x20;
constexpr unsigned char CMD_WRITE_CONFIG = 0x60;
constexpr unsigned char CMD_ENABLE_MOUSE_PORT = 0xA8;
constexpr unsigned char CMD_ENABLE_KEYBOARD = 0xAE;
constexpr unsigned char CMD_WRITE_TO_MOUSE = 0xD4;

constexpr unsigned char CONFIG_KEYBOARD_IRQ = 1 << 0;
constexpr unsigned char CONFIG_MOUSE_IRQ = 1 << 1;
constexpr unsigned char CONFIG_KEYBOARD_CLOCK_OFF = 1 << 4;
constexpr unsigned char CONFIG_MOUSE_CLOCK_OFF = 1 << 5;
constexpr unsigned char CONFIG_TRANSLATE = 1 << 6; // deliver scancode set 1

constexpr unsigned char MOUSE_SET_DEFAULTS = 0xF6;
constexpr unsigned char MOUSE_ENABLE_REPORTING = 0xF4;
constexpr unsigned char MOUSE_ACK = 0xFA;

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

int readData() {
    if (not waitUntil(STATUS_OUTPUT_FULL, true))
        return -1;
    return static_cast<unsigned char>(Sys::portIn(PORTS, DATA));
}

bool mouseCommand(unsigned char cmd) {
    command(CMD_WRITE_TO_MOUSE);
    writeData(cmd);
    return readData() == MOUSE_ACK;
}

// Firmware (especially UEFI) may leave interrupts or the mouse off.
bool setupController() {
    while (status() & STATUS_OUTPUT_FULL)
        Sys::portIn(PORTS, DATA); // drop stale bytes

    command(CMD_ENABLE_MOUSE_PORT);

    command(CMD_READ_CONFIG);
    int config = readData();
    if (config < 0)
        return false;
    config |= CONFIG_KEYBOARD_IRQ | CONFIG_MOUSE_IRQ | CONFIG_TRANSLATE;
    config &= ~(CONFIG_KEYBOARD_CLOCK_OFF | CONFIG_MOUSE_CLOCK_OFF);
    command(CMD_WRITE_CONFIG);
    writeData(static_cast<unsigned char>(config));
    command(CMD_ENABLE_KEYBOARD);

    return mouseCommand(MOUSE_SET_DEFAULTS) and mouseCommand(MOUSE_ENABLE_REPORTING);
}

struct Keyboard {
    bool shift = false;
    bool extended = false;

    // Returns true and fills `event` when the byte completes a key event.
    bool feed(unsigned char byte, InputEvent& event) {
        if (byte == EXTENDED_PREFIX) {
            extended = true;
            return false;
        }
        bool pressed = not(byte & 0x80);
        unsigned char code = byte & 0x7F;
        if (extended) { // arrows, right ctrl...: not mapped yet
            extended = false;
            return false;
        }
        if (code == LEFT_SHIFT or code == RIGHT_SHIFT)
            shift = pressed;

        event = {};
        event.type = InputType::Key;
        event.pressed = pressed;
        event.scancode = code;
        event.character = code < TABLE_SIZE ? (shift ? SHIFTED[code] : NORMAL[code]) : 0;
        return true;
    }
};

struct Mouse {
    unsigned char packet[3];
    int index = 0;

    // Mouse packets are 3 bytes: flags, dx, dy (9-bit signed, y up).
    bool feed(unsigned char byte, InputEvent& event) {
        if (index == 0 and not(byte & 0x08))
            return false; // out of sync: bit 3 of the first byte is always set
        packet[index++] = byte;
        if (index < 3)
            return false;
        index = 0;

        unsigned char flags = packet[0];
        if (flags & 0xC0)
            return false; // overflow: drop the packet
        int dx = packet[1] - ((flags << 4) & 0x100);
        int dy = packet[2] - ((flags << 3) & 0x100);

        event = {};
        event.type = InputType::Mouse;
        event.buttons = flags & 0x07;
        event.dx = static_cast<short>(dx);
        event.dy = static_cast<short>(-dy);
        return true;
    }
};

} // namespace

int main() {
    Channel events(EVENTS);

    Channel irqs, irqSender;
    Channel::createPair(irqs, irqSender);
    Sys::irqBind(KEYBOARD_IRQ, irqSender.handle(), KEYBOARD_IRQ);
    Sys::irqBind(MOUSE_IRQ, irqSender.handle(), MOUSE_IRQ);

    bool mouseOk = setupController();
    Sys::log(mouseOk ? "ready: keyboard and mouse" : "ready: keyboard (no mouse answered)");

    Keyboard keyboard;
    Mouse mouse;

    for (;;) {
        Sys::IrqMessage irq;
        if (irqs.recv(irq) != Sys::OK)
            return 1;

        while (status() & STATUS_OUTPUT_FULL) {
            bool fromMouse = status() & STATUS_FROM_MOUSE;
            auto byte = static_cast<unsigned char>(Sys::portIn(PORTS, DATA));

            InputEvent event;
            bool complete = fromMouse ? mouse.feed(byte, event) : keyboard.feed(byte, event);
            if (complete and events.send(event) == Sys::PEER_CLOSED)
                return 0; // nobody is listening anymore
        }

        Sys::irqAck(irq.cookie); // the cookie is the irq handle
    }
}
