#pragma once

// Kapalaframe: the KamalShell🪷 window protocol.
//
// Every app gets a channel to the shell as its handle 1 when the shell
// starts it. The app draws into shared memory it creates itself; the shell
// maps the same memory and composes it on screen.
//
//   app                                 shell
//   CreateWindow(w, h, title) + memory ->
//                                    <- Opened
//   (draws into the memory)
//   Present(rect)                   ->   (copies that part to the screen)
//                                    <- MouseMove / MouseDown / MouseUp / Key / Focus
//                                    <- Close (the user clicked x)
//   (exits: the channel closes and the window goes away)

namespace Kapala::Protocol::Shell {

enum class Type : unsigned char {
    // App -> shell
    CreateWindow = 1, // w, h, title; attached: shared memory of w * h * 4 bytes (the pixels)
    Present = 2,      // x, y, w, h: this part of the window changed
    SetTitle = 3,     // title

    // Shell -> app
    Opened = 10,
    MouseMove = 11, // x, y in window coordinates; buttons held
    MouseDown = 12, // x, y; buttons held after the press
    MouseUp = 13,   // x, y; buttons held after the release
    Key = 14,       // character, scancode, pressed
    Close = 15,     // the user asked to close the window
    Focus = 16,     // pressed: whether the window now has the keyboard
};

struct Message {
    Type type;
    bool pressed;
    char character;
    unsigned char scancode;
    int x, y, w, h;
    unsigned buttons;
    char title[48];
};

static_assert(sizeof(Message) <= 128, "must fit in a channel message");

} // namespace Kapala::Protocol::Shell
