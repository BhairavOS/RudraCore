// showcase: the first Kapalaframe application.
//
//   handle 1: the screen (shared memory)
//   handle 2: a channel of input events from the PS/2 driver
//
// Shows off what KamalShell🪷 will be built from: anti-aliased shapes,
// gradients, shadows, text, images, a mouse cursor, a clickable button and
// a text field. Drawing goes to a back buffer; only changed areas are
// redrawn and copied to the screen, with the cursor drawn on top.

#include <kapala/channel.h>
#include <kapala/gfx.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;
using namespace Kapala::Gfx;
using Protocol::InputEvent;
using Protocol::InputType;

extern "C" unsigned char const _showcaseLogo[]; // 256x256 RGBA

namespace {

constexpr Sys::Handle SCREEN = 1;
constexpr Sys::Handle INPUT = 2;

constexpr int LOGO_SIZE = 256;

// The mouse cursor: X = outline, . = fill.
constexpr char const* CURSOR[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X..........X",
    "X......XXXXX",
    "X...X..X    ",
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     X..X  ",
    "      XXXX  ",
};
constexpr int CURSOR_W = 12;
constexpr int CURSOR_H = sizeof(CURSOR) / sizeof(CURSOR[0]);

constexpr Color SWATCHES[] = {
    Theme::SAFFRON, Theme::GOLD, Theme::LOTUS, Theme::LOTUS_LIGHT, Color::hex(0x7C5CFF), Color::hex(0x3DD6C3),
};

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

class Showcase {
public:
    bool start() {
        Sys::SystemInfo info;
        Sys::systemInfo(info);
        auto* pixels = static_cast<unsigned*>(Sys::memoryMap(SCREEN));
        if (not pixels or info.screenWidth == 0)
            return false;

        PixelFormat format{info.redShift, info.greenShift, info.blueShift};
        _screen = {pixels, int(info.screenWidth), int(info.screenHeight), int(info.screenPitch / 4), format};
        _back = Bitmap::allocate(_screen.width, _screen.height, format);
        if (not _back.pixels)
            return false;

        layout();
        _mouse = {_screen.width / 2, _screen.height / 2};
        _drawnMouse = _mouse;
        refreshStats();
        redraw(_screen.bounds());
        return true;
    }

    void onInput(InputEvent const& e) {
        if (e.type == InputType::Key)
            onKey(e);
        else if (e.type == InputType::Mouse)
            onMouse(e);
    }

    // Called after a batch of input: moves the cursor once, not per event.
    void flushCursor() {
        if (_mouse.x == _drawnMouse.x and _mouse.y == _drawnMouse.y)
            return;
        Rect old = cursorRect(_drawnMouse);
        _drawnMouse = _mouse;
        present(old);
        present(cursorRect(_mouse));
        redraw(_mouseLine);

        bool hover = _button.contains(_mouse);
        if (hover != _hover) {
            _hover = hover;
            redraw(_button);
        }
    }

    void tick(unsigned long now) {
        if (now - _lastBlink >= 500) {
            _lastBlink = now;
            _caretOn = not _caretOn;
            redraw(_field);
        }
        if (now - _lastStats >= 1000) {
            _lastStats = now;
            refreshStats();
            redraw(_stats);
        }
    }

private:
    // MARK: Layout and drawing

    void layout() {
        int w = 740, h = 470;
        _window = {(_screen.width - w) / 2, (_screen.height - h) / 2 - 10, w, h};
        _title = {_window.x, _window.y, _window.w, 46};
        int x0 = _window.x + 316;
        _stats = {x0, _window.y + 158, 400, 64};
        _button = {x0, _window.y + 236, 190, 44};
        _clicksLabel = {x0 + 206, _window.y + 236, 200, 44};
        _field = {x0, _window.y + 300, 390, 42};
        _swatches = {x0, _window.y + 362, 6 * 36, 30};
        _mouseLine = {_window.x + 28, _window.bottom() - 40, 400, 20};
    }

    void drawScene(Rect area) {
        Canvas c(_back);
        c.clip = area;

        // Desktop background and footer.
        c.verticalGradient(_back.bounds(), Theme::NIGHT, Theme::MAROON);
        char const* footer = "Bhairav OS  |  RudraCore  |  Kapalaframe  |  KamalShell coming next";
        c.text({(_back.width - Font::system().measure(footer)) / 2, _back.height - 34}, footer, Theme::TEXT_DIM);

        // Window with a soft shadow.
        c.shadow(_window, 16, 20, Color::hex(0x000000, 170));
        c.fillRounded(_window, 16, Theme::SURFACE);
        c.strokeRounded(_window, 16, 1, Theme::BORDER);

        // Title bar: the clip keeps only the top of a gradient rounded rect,
        // so the top corners are round and the bottom edge is straight.
        Rect saved = c.clip;
        c.clip = c.clip.intersect(_title);
        c.fillRoundedGradient(Rect{_title.x, _title.y, _title.w, _title.h + 20}, 16, Theme::SAFFRON, Theme::LOTUS);
        c.clip = saved;
        c.text({_title.x + 20, _title.y + 7}, "Kapalaframe", Theme::WHITE, 2);
        for (int i = 0; i < 3; i++)
            c.fillCircle(_title.right() - 26 - i * 24, _title.y + 23, 7, Theme::WHITE.withAlpha(i == 0 ? 230 : 140));

        // Logo.
        c.image({_window.x + 30, _window.y + 72}, _showcaseLogo, LOGO_SIZE, LOGO_SIZE);

        // Heading.
        int x0 = _window.x + 316;
        c.text({x0, _window.y + 72}, "Kapalaframe", Theme::SAFFRON, 2);
        c.text({x0, _window.y + 110}, "Framework for efficient", Theme::TEXT_DIM);
        c.text({x0, _window.y + 128}, "application development", Theme::TEXT_DIM);

        drawStats(c);
        drawButton(c);
        drawField(c);

        for (int i = 0; i < 6; i++)
            c.fillCircle(_swatches.x + 15 + i * 36, _swatches.y + 15, 13, SWATCHES[i]);

        Text<96> mouseText;
        mouseText.add("mouse ").add(long(_mouse.x)).add(", ").add(long(_mouse.y)).add("   buttons: ");
        mouseText.add(_buttons & Protocol::MOUSE_LEFT ? "L " : "").add(_buttons & Protocol::MOUSE_RIGHT ? "R " : "");
        mouseText.add(_buttons ? "" : "none");
        c.text({_mouseLine.x, _mouseLine.y}, mouseText.cstr(), Theme::TEXT_DIM);
    }

    void drawStats(Canvas& c) {
        Text<96> lines[3];
        lines[0].add("screen  ").add(long(_screen.width)).add(" x ").add(long(_screen.height));
        lines[1].add("memory  ").add(_info.freeMemory / (1024 * 1024)).add(" MiB free of ")
            .add(_info.totalMemory / (1024 * 1024)).add(" MiB");
        lines[2].add("tasks   ").add(_info.taskCount).add("      uptime ").add(_info.uptimeMs / 1000).add(" s");
        for (int i = 0; i < 3; i++)
            c.text({_stats.x, _stats.y + i * 20}, lines[i].cstr(), Theme::TEXT);
    }

    void drawButton(Canvas& c) {
        Color color = _pressed ? Theme::LOTUS : (_hover ? Theme::GOLD : Theme::SAFFRON);
        c.fillRounded(_button, 22, color);
        char const* label = "Say namaste";
        c.text({_button.x + (_button.w - Font::system().measure(label)) / 2, _button.y + 14}, label, Theme::NIGHT);

        Text<64> clicks;
        if (_clicks == 0)
            clicks.add("click the button");
        else
            clicks.add("namaste x ").add(_clicks);
        c.text({_clicksLabel.x, _clicksLabel.y + 14}, clicks.cstr(), Theme::LOTUS_LIGHT);
    }

    void drawField(Canvas& c) {
        c.fillRounded(_field, 10, Theme::SURFACE_ALT);
        c.strokeRounded(_field, 10, 1.5f, _typed.length ? Theme::SAFFRON : Theme::BORDER);
        int textY = _field.y + 13;
        int x = _field.x + 14;
        if (_typed.length == 0)
            c.text({x, textY}, "type something...", Theme::TEXT_DIM);
        else
            x = c.text({x, textY}, _typed.cstr(), Theme::TEXT);
        if (_caretOn)
            c.fill(Rect{x + 1, textY - 1, 2, 18}, Theme::SAFFRON);
    }

    // Copies an area of the back buffer to the screen, then the cursor over it.
    void present(Rect area) {
        area = area.intersect(_screen.bounds());
        if (area.empty())
            return;
        _screen.copyFrom(_back, area);

        Canvas c(_screen);
        c.clip = area;
        for (int y = 0; y < CURSOR_H; y++) {
            for (int x = 0; x < CURSOR_W; x++) {
                char p = CURSOR[y][x];
                if (p != ' ')
                    c.fill(Rect{_drawnMouse.x + x, _drawnMouse.y + y, 1, 1}, p == 'X' ? Theme::BLACK : Theme::WHITE);
            }
        }
    }

    void redraw(Rect area) {
        drawScene(area);
        present(area);
    }

    static Rect cursorRect(Point p) { return {p.x, p.y, CURSOR_W, CURSOR_H}; }

    void refreshStats() { Sys::systemInfo(_info); }

    // MARK: Input

    void onKey(InputEvent const& e) {
        if (not e.pressed or e.character == 0)
            return;

        if (e.character == '\n') {
            if (_typed.length) {
                Text<128> message;
                message.add("you typed: ").add(_typed.cstr());
                Sys::log(message.cstr());
            }
            _typed = {};
        } else if (e.character == '\b') {
            if (_typed.length)
                _typed.data[--_typed.length] = '\0';
        } else if (_typed.length < 40) {
            char text[2] = {e.character, 0};
            _typed.add(text);
        }
        _caretOn = true;
        redraw(_field);
    }

    void onMouse(InputEvent const& e) {
        _mouse.x = clampInt(_mouse.x + e.dx, 0, _screen.width - 1);
        _mouse.y = clampInt(_mouse.y + e.dy, 0, _screen.height - 1);

        bool wasDown = _buttons & Protocol::MOUSE_LEFT;
        _buttons = e.buttons;
        bool isDown = _buttons & Protocol::MOUSE_LEFT;
        bool overButton = _button.contains(_mouse);

        if (isDown and not wasDown and overButton) {
            _pressed = true;
            redraw(_button);
        } else if (not isDown and wasDown and _pressed) {
            _pressed = false;
            if (overButton) {
                _clicks++;
                Text<64> message;
                message.add("namaste! (button clicked ").add(_clicks).add(" times)");
                Sys::log(message.cstr());
            }
            redraw(_button.unite(_clicksLabel));
        }
    }

    Bitmap _screen, _back;
    Rect _window, _title, _stats, _button, _clicksLabel, _field, _swatches, _mouseLine;

    Point _mouse, _drawnMouse;
    unsigned char _buttons = 0;
    bool _hover = false, _pressed = false, _caretOn = true;
    unsigned long _clicks = 0;
    Text<64> _typed;

    Sys::SystemInfo _info{};
    unsigned long _lastBlink = 0, _lastStats = 0;
};

} // namespace

int main() {
    Showcase app;
    if (not app.start()) {
        Sys::log("can't open the screen");
        return 1;
    }
    Sys::log("the screen is mine: Kapalaframe showcase running (try the mouse and keyboard)");

    Channel input(INPUT);
    bool inputOpen = true;
    for (;;) {
        // Handle everything that arrived, then redraw the cursor once.
        for (int i = 0; inputOpen and i < 64; i++) {
            InputEvent event;
            long result = input.recv(event, nullptr, Sys::RECV_NONBLOCK);
            if (result == Sys::WOULD_BLOCK)
                break;
            if (result != Sys::OK) {
                inputOpen = false;
                Sys::log("input closed");
                break;
            }
            app.onInput(event);
        }
        app.flushCursor();
        app.tick(Sys::timeMs());
        Sys::sleep(10);
    }
}
