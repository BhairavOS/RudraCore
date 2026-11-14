#pragma once

// Kapalaframe: the KamalShell🪷 window protocol.
//
// Every app gets a channel to the shell as its handle 1 when the shell
// starts it. The app draws into shared memory it creates itself; the shell
// maps the same memory and composes it on screen.
//
//   app                                 shell
//   CreateWindow(w, h, title, pressed: resizable) + memory ->
//                                    <- Argument (if it was launched with one)
//                                    <- Opened
//   (draws into the memory)
//   Present(rect)                   ->   (copies that part to the screen)
//                                    <- MouseMove / MouseDown / MouseUp / Wheel / Key / Focus
//                                    <- ResizeRequest(w, h): the user resized or maximized it
//   Resize(w, h) + new memory       ->   (the window now uses the new pixels)
//   Launch(program, argument)       ->   (starts another app, built-in or installed)
//   AppsChanged                     ->   (after installing or removing an app)
//   Power(x: 0 off, 1 restart)      ->   (the Terminal's shutdown and reboot)
//                                    <- Close (the user clicked x)
//   (exits: the channel closes and the window goes away)

namespace Kapala::Protocol::Shell {

enum class Type : unsigned char {
    // App -> shell
    CreateWindow = 1, // w, h, title, pressed: resizable; attached: shared memory of w * h * 4 bytes
    Present = 2,      // x, y, w, h: this part of the window changed
    SetTitle = 3,     // title
    Resize = 4,       // w, h; attached: shared memory with the new pixels
    Launch = 5,       // title: program name (up to 23 characters), argument at title + 24
    AppsChanged = 6,  // apps were installed or removed: look again
    Power = 7,        // x: 0 shut down, 1 restart (honoured only from the Terminal)

    // Shell -> app
    Opened = 10,
    MouseMove = 11, // x, y in window coordinates; buttons held
    MouseDown = 12, // x, y; buttons held after the press
    MouseUp = 13,   // x, y; buttons held after the release
    Key = 14,       // character, scancode, pressed
    Close = 15,     // the user asked to close the window
    Focus = 16,     // pressed: whether the window now has the keyboard
    Wheel = 17,     // x, y; h: wheel steps (positive: scroll down)
    ResizeRequest = 18, // w, h: the content size the user wants
    Argument = 19,      // title: what the app was launched with (e.g. a file path)
};

constexpr unsigned LAUNCH_ARGUMENT = 24; // where Launch's argument starts in title

struct Message {
    Type type;
    bool pressed;
    char character;
    unsigned char scancode;
    int x, y, w, h;
    unsigned buttons;
    char title[96];
};

static_assert(sizeof(Message) <= 128, "must fit in a channel message");

} // namespace Kapala::Protocol::Shell
