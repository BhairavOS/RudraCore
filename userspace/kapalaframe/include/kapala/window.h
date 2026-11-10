#pragma once

// Kapalaframe: a window on KamalShell🪷.
//
//   Window window;
//   if (not window.open("Notes", 480, 360)) return 1;
//   Canvas canvas(window.surface());
//   ... draw ...
//   window.present();
//
//   for (;;) {
//       WindowEvent event;
//       if (not window.waitEvent(event, 500)) { /* 500 ms passed */ continue; }
//       if (event.kind == WindowEvent::Close) return 0;
//       ...
//   }

#include <kapala/channel.h>
#include <kapala/gfx.h>

namespace Kapala {

struct WindowEvent {
    enum Kind {
        MouseMove,
        MouseDown,
        MouseUp,
        Key,
        Focus,
        Close, // the user closed the window (or the shell is gone)
    };

    Kind kind;
    Gfx::Point at;    // mouse position in window coordinates
    unsigned buttons; // Protocol::MouseButton flags held now
    char character;   // Key: the typed character, or 0
    unsigned char scancode;
    bool pressed; // Key: pressed or released; Focus: gained or lost
};

class Window {
public:
    // Opens a window through the shell connection every app gets as
    // handle 1. Waits until the shell shows it.
    bool open(char const* title, int width, int height, Sys::Handle shell = 1);

    // The window's pixels: draw here, then present().
    Gfx::Bitmap& surface() { return _surface; }
    int width() const { return _surface.width; }
    int height() const { return _surface.height; }

    void present(Gfx::Rect area);
    void present() { present(_surface.bounds()); }

    void setTitle(char const* title);

    bool focused() const { return _focused; }

    // Waits up to `timeoutMs` for an event (Sys::WAIT_FOREVER: no limit).
    // Returns false if the time passed with no event.
    bool waitEvent(WindowEvent& event, unsigned long timeoutMs = Sys::WAIT_FOREVER);

private:
    Channel _shell;
    Gfx::Bitmap _surface;
    bool _focused = false;
};

} // namespace Kapala
