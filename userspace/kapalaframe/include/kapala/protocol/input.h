#pragma once

// Kapalaframe: messages sent by input drivers.

namespace Kapala::Protocol {

enum class InputType : unsigned char {
    Key = 1,
    Mouse = 2,
};

enum MouseButton : unsigned char {
    MOUSE_LEFT = 1 << 0,
    MOUSE_RIGHT = 1 << 1,
    MOUSE_MIDDLE = 1 << 2,
};

// Keys that type no character, by scancode. Keys with the 0xE0 prefix in
// scancode set 1 (arrows and the block above them) get 0x80 added.
enum Key : unsigned char {
    KEY_CTRL = 0x1D, // left and right
    KEY_SHIFT_LEFT = 0x2A,
    KEY_SHIFT_RIGHT = 0x36,
    KEY_ALT = 0x38,  // left and right
    KEY_F5 = 0x3F,
    KEY_HOME = 0xC7,
    KEY_UP = 0xC8,
    KEY_PAGE_UP = 0xC9,
    KEY_LEFT = 0xCB,
    KEY_RIGHT = 0xCD,
    KEY_END = 0xCF,
    KEY_DOWN = 0xD0,
    KEY_PAGE_DOWN = 0xD1,
    KEY_DELETE = 0xD3,
};

// One message per key press/release or mouse movement/click.
struct InputEvent {
    InputType type;

    // Key
    bool pressed;
    char character;         // the typed character, or 0 for keys like Shift
    unsigned char scancode; // PC scancode set 1 without the release bit (see Key)

    // Mouse: movement since the last event (y grows downward, like the
    // screen), the buttons held now, and wheel steps (positive: toward
    // the user, i.e. scroll down).
    unsigned char buttons;
    signed char wheel;
    short dx;
    short dy;
};

} // namespace Kapala::Protocol
