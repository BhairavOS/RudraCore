#pragma once

// Kapalaframe: messages sent by input drivers.

namespace Kapala::Protocol {

// Sent by the keyboard driver for every key press and release.
struct KeyEvent {
    unsigned char scancode; // PC scancode set 1, without the release bit
    bool pressed;
    char character;         // the typed character, or 0 for keys like Shift
    unsigned char reserved;
};

} // namespace Kapala::Protocol
