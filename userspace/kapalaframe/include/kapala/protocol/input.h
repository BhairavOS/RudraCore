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

// One message per key press/release or mouse movement/click.
struct InputEvent {
    InputType type;

    // Key
    bool pressed;
    char character;         // the typed character, or 0 for keys like Shift
    unsigned char scancode; // PC scancode set 1, without the release bit

    // Mouse: movement since the last event (y grows downward, like the
    // screen) and the buttons held now.
    unsigned char buttons;
    unsigned char reserved;
    short dx;
    short dy;
};

} // namespace Kapala::Protocol
