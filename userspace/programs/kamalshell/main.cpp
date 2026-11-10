// KamalShell🪷 - Beautiful and Responsive Graphical Shell
//
//   handle 1: the screen (shared memory)
//   handle 2: input events from the PS/2 driver
//
// The compositor and desktop of Bhairav OS. Apps are separate programs: the
// shell starts each one with a channel (its handle 1), the app creates its
// window's pixels in shared memory, and the shell composes every window
// onto the screen (see kapala/protocol/shell.h).
//
// Drawing: everything that changes adds its rectangle to the damage; once
// per batch of events, that area is recomposed (desktop, windows bottom to
// top, taskbar, launcher) into a back buffer and copied to the screen, with
// the mouse cursor drawn on top.

#include <kapala/channel.h>
#include <kapala/gfx.h>
#include <kapala/protocol/input.h>
#include <kapala/protocol/shell.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;
using namespace Kapala::Gfx;
using Protocol::InputEvent;
using Protocol::InputType;
using ShellMessage = Protocol::Shell::Message;
using ShellType = Protocol::Shell::Type;

namespace {

constexpr Sys::Handle SCREEN = 1;
constexpr Sys::Handle INPUT = 2;

constexpr int MAX_WINDOWS = 16;
constexpr int TITLE_H = 34;
constexpr int TASKBAR_H = 48;
constexpr int RADIUS = 12;
constexpr int SHADOW = 18;
constexpr int SHADOW_MARGIN = SHADOW + 6;

struct AppEntry {
    char const* program;
    char const* label;
    char const* detail;
};

constexpr AppEntry APPS[] = {
    {"about", "About Bhairav OS", "the system and its parts"},
    {"notes", "Notes", "write something down"},
    {"monitor", "System Monitor", "memory, tasks and time"},
};
constexpr int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

// The mouse cursor: X = outline, . = fill.
constexpr char const* CURSOR[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X..........X", "X......XXXXX", "X...X..X    ", "X..XX..X    ",
    "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      XXXX  ",
};
constexpr int CURSOR_W = 12;
constexpr int CURSOR_H = sizeof(CURSOR) / sizeof(CURSOR[0]);

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void copyString(char* dest, unsigned size, char const* src) {
    unsigned i = 0;
    for (; src[i] and i + 1 < size; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

void addTwoDigits(Text<32>& t, unsigned long v) {
    if (v < 10)
        t.add("0");
    t.add(v);
}

struct Window {
    bool used = false;    // a client is connected
    bool visible = false; // it has created its window
    Channel channel;
    Bitmap surface;
    char title[48] = {};
    Rect frame;

    Rect content() const { return {frame.x, frame.y + TITLE_H, frame.w, frame.h - TITLE_H}; }
    Rect titleBar() const { return {frame.x, frame.y, frame.w, TITLE_H}; }
    Rect closeButton() const { return {frame.right() - 32, frame.y + 6, 22, 22}; }
    Rect area() const { // everything it draws, shadow included
        return {frame.x - SHADOW_MARGIN, frame.y - SHADOW_MARGIN, frame.w + 2 * SHADOW_MARGIN,
                frame.h + 2 * SHADOW_MARGIN};
    }
};

class Shell {
public:
    bool start() {
        Sys::SystemInfo info;
        Sys::systemInfo(info);
        auto* pixels = static_cast<unsigned*>(Sys::memoryMap(SCREEN));
        if (not pixels or info.screenWidth == 0)
            return false;

        _format = {info.redShift, info.greenShift, info.blueShift};
        _screen = {pixels, int(info.screenWidth), int(info.screenHeight), int(info.screenPitch / 4), _format};
        _back = Bitmap::allocate(_screen.width, _screen.height, _format);
        if (not _back.pixels)
            return false;

        _input = Channel(INPUT);
        _mouse = _drawnMouse = {_screen.width / 2, _screen.height / 2};
        damage(_screen.bounds());
        flush();
        launch("about"); // a first window to say hello
        return true;
    }

    [[noreturn]] void run() {
        for (;;) {
            Sys::Handle handles[1 + MAX_WINDOWS];
            int owners[1 + MAX_WINDOWS];
            unsigned long count = 0;
            if (_inputOpen) {
                handles[count] = _input.handle();
                owners[count++] = -1;
            }
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (_windows[i].used) {
                    handles[count] = _windows[i].channel.handle();
                    owners[count++] = i;
                }
            }

            // Wake up for the next message, or at the next second for the clock.
            unsigned long untilNextSecond = 1000 - Sys::timeMs() % 1000;
            long ready = count ? Sys::wait(handles, count, untilNextSecond) : Sys::TIMED_OUT;
            if (count == 0)
                Sys::sleep(untilNextSecond);

            if (ready >= 0) {
                if (owners[ready] < 0)
                    readInput();
                else
                    serviceClient(owners[ready]);
            }

            tickClock();
            flush();
        }
    }

private:
    // MARK: Apps and windows

    void launch(char const* program) {
        int slot = -1;
        for (int i = 0; i < MAX_WINDOWS and slot < 0; i++) {
            if (not _windows[i].used)
                slot = i;
        }
        if (slot < 0) {
            Sys::log("too many windows");
            return;
        }

        Channel mine, theirs;
        Channel::createPair(mine, theirs);
        Sys::Handle give[] = {theirs.release()};
        if (Sys::spawn(program, give, 1) < 0) {
            Text<> message;
            message.add("can't start ").add(program);
            Sys::log(message.cstr());
            return;
        }
        _windows[slot] = {};
        _windows[slot].used = true;
        _windows[slot].channel = static_cast<Channel&&>(mine);
    }

    void send(int i, ShellMessage const& message) { _windows[i].channel.send(message); }

    void sendSimple(int i, ShellType type, bool pressed = false) {
        ShellMessage m{};
        m.type = type;
        m.pressed = pressed;
        send(i, m);
    }

    void serviceClient(int i) {
        Window& w = _windows[i];
        for (int n = 0; n < 32; n++) {
            ShellMessage m;
            Sys::Handle attached = Sys::NO_HANDLE;
            long result = Sys::recv(w.channel.handle(), &m, sizeof(m), &attached, Sys::RECV_NONBLOCK);
            if (result == Sys::WOULD_BLOCK)
                return;
            if (result < 0) { // the app exited
                closeWindow(i);
                return;
            }
            if (result != sizeof(m)) {
                if (attached)
                    Sys::close(attached);
                continue;
            }

            switch (m.type) {
            case ShellType::CreateWindow: createWindow(i, m, attached); break;
            case ShellType::Present:
                if (w.visible) {
                    Rect local = Rect{m.x, m.y, m.w, m.h}.intersect(w.surface.bounds());
                    damage(local.offset(w.content().x, w.content().y).intersect(w.content()));
                }
                break;
            case ShellType::SetTitle:
                copyString(w.title, sizeof(w.title), m.title);
                damage(w.titleBar());
                damage(taskbarRect());
                break;
            default:
                break;
            }
            if (attached and m.type != ShellType::CreateWindow)
                Sys::close(attached);
        }
    }

    void createWindow(int i, ShellMessage const& m, Sys::Handle memory) {
        Window& w = _windows[i];
        int maxW = _screen.width - 40, maxH = _screen.height - TASKBAR_H - TITLE_H - 30;
        bool sizeOk = m.w > 0 and m.h > 0 and m.w <= maxW and m.h <= maxH;
        void* pixels = sizeOk and memory and not w.visible ? Sys::memoryMap(memory) : nullptr;
        if (memory)
            Sys::close(memory); // the mapping keeps the memory alive
        if (not pixels) {
            sendSimple(i, ShellType::Close);
            return;
        }

        // Shared memory is page-rounded, so it's at least w * h * 4 bytes.
        w.surface = {static_cast<unsigned*>(pixels), m.w, m.h, m.w, _format};
        copyString(w.title, sizeof(w.title), m.title);

        int k = _placements++ % 8;
        int fw = m.w, fh = m.h + TITLE_H;
        int x = clampInt((_screen.width - fw) / 2 - 120 + k * 40, 12, _screen.width - fw - 12);
        int y = clampInt((_screen.height - TASKBAR_H - fh) / 2 - 60 + k * 32, 12, _screen.height - TASKBAR_H - fh - 12);
        w.frame = {x, y, fw, fh};
        w.visible = true;

        sendSimple(i, ShellType::Opened);
        raise(i);
    }

    void closeWindow(int i) {
        Window& w = _windows[i];
        if (w.visible) {
            damage(w.area());
            Sys::memoryUnmap(w.surface.pixels);
            removeFromOrder(i);
        }
        if (_dragging == i)
            _dragging = -1;
        if (_captured == i)
            _captured = -1;
        _windows[i] = {}; // closes the channel
        damage(taskbarRect());
        updateFocus();
    }

    // MARK: Stacking and focus

    int top() const { return _orderCount ? _order[_orderCount - 1] : -1; }

    void removeFromOrder(int i) {
        int j = 0;
        for (int k = 0; k < _orderCount; k++) {
            if (_order[k] != i)
                _order[j++] = _order[k];
        }
        _orderCount = j;
    }

    void raise(int i) {
        if (top() == i)
            return;
        removeFromOrder(i);
        _order[_orderCount++] = i;
        damage(_windows[i].area());
        damage(taskbarRect());
        updateFocus();
    }

    // Tells apps when they gain or lose the keyboard (the top window has it).
    void updateFocus() {
        int now = top();
        if (now == _focused)
            return;
        if (_focused >= 0 and _windows[_focused].visible) {
            sendSimple(_focused, ShellType::Focus, false);
            damage(_windows[_focused].titleBar());
        }
        _focused = now;
        if (_focused >= 0) {
            sendSimple(_focused, ShellType::Focus, true);
            damage(_windows[_focused].titleBar());
        }
    }

    int windowAt(Point p) const {
        for (int k = _orderCount - 1; k >= 0; k--) {
            if (_windows[_order[k]].frame.contains(p))
                return _order[k];
        }
        return -1;
    }

    // MARK: Input

    void readInput() {
        for (int n = 0; n < 64; n++) {
            InputEvent e;
            long result = _input.recv(e, nullptr, Sys::RECV_NONBLOCK);
            if (result == Sys::WOULD_BLOCK)
                return;
            if (result != Sys::OK) {
                _inputOpen = false;
                Sys::log("input closed");
                return;
            }
            if (e.type == InputType::Key)
                onKey(e);
            else if (e.type == InputType::Mouse)
                onMouse(e);
        }
    }

    void onKey(InputEvent const& e) {
        if (e.pressed and e.character == 0x1b and _launcherOpen) { // Escape
            setLauncher(false);
            return;
        }
        int f = top();
        if (f < 0)
            return;
        ShellMessage m{};
        m.type = ShellType::Key;
        m.character = e.character;
        m.scancode = e.scancode;
        m.pressed = e.pressed;
        send(f, m);
    }

    void onMouse(InputEvent const& e) {
        _mouse.x = clampInt(_mouse.x + e.dx, 0, _screen.width - 1);
        _mouse.y = clampInt(_mouse.y + e.dy, 0, _screen.height - 1);

        bool wasDown = _buttons & Protocol::MOUSE_LEFT;
        _buttons = e.buttons;
        bool isDown = _buttons & Protocol::MOUSE_LEFT;

        if (_dragging >= 0) {
            Window& w = _windows[_dragging];
            damage(w.area());
            w.frame.x = clampInt(_mouse.x - _dragOffset.x, 80 - w.frame.w, _screen.width - 80);
            w.frame.y = clampInt(_mouse.y - _dragOffset.y, 0, _screen.height - TASKBAR_H - TITLE_H);
            damage(w.area());
        } else {
            forwardMouse(ShellType::MouseMove);
        }

        updateHover();

        if (isDown and not wasDown)
            onPress();
        else if (not isDown and wasDown)
            onRelease();
    }

    // Sends a mouse event to the window that has the mouse: the one being
    // clicked in, or else the one under the pointer (if over its content).
    void forwardMouse(ShellType type) {
        int target = _captured;
        if (target < 0) {
            int under = windowAt(_mouse);
            if (under >= 0 and _windows[under].content().contains(_mouse))
                target = under;
        }
        if (target < 0)
            return;
        Rect content = _windows[target].content();
        ShellMessage m{};
        m.type = type;
        m.x = _mouse.x - content.x;
        m.y = _mouse.y - content.y;
        m.buttons = _buttons;
        send(target, m);
    }

    void onPress() {
        if (_launcherOpen) {
            if (launcherRect().contains(_mouse)) {
                if (_hoverItem >= 0) {
                    launch(APPS[_hoverItem].program);
                    setLauncher(false);
                }
                return;
            }
            if (not startButton().contains(_mouse)) {
                setLauncher(false);
                return;
            }
        }

        if (taskbarRect().contains(_mouse)) {
            if (startButton().contains(_mouse)) {
                setLauncher(not _launcherOpen);
                return;
            }
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (_windows[i].visible and taskButton(i).contains(_mouse)) {
                    raise(i);
                    return;
                }
            }
            return;
        }

        int i = windowAt(_mouse);
        if (i < 0)
            return;
        raise(i);
        Window& w = _windows[i];
        if (w.closeButton().contains(_mouse)) {
            sendSimple(i, ShellType::Close);
        } else if (w.titleBar().contains(_mouse)) {
            _dragging = i;
            _dragOffset = {_mouse.x - w.frame.x, _mouse.y - w.frame.y};
        } else {
            _captured = i;
            forwardMouse(ShellType::MouseDown);
        }
    }

    void onRelease() {
        _dragging = -1;
        if (_captured >= 0) {
            forwardMouse(ShellType::MouseUp);
            _captured = -1;
        }
    }

    void updateHover() {
        bool hoverStart = startButton().contains(_mouse);
        if (hoverStart != _hoverStart) {
            _hoverStart = hoverStart;
            damage(startButton());
        }
        int item = -1;
        if (_launcherOpen) {
            for (int k = 0; k < APP_COUNT; k++) {
                if (launcherItem(k).contains(_mouse))
                    item = k;
            }
        }
        if (item != _hoverItem) {
            if (_hoverItem >= 0)
                damage(launcherItem(_hoverItem));
            if (item >= 0)
                damage(launcherItem(item));
            _hoverItem = item;
        }
    }

    void setLauncher(bool open) {
        if (open == _launcherOpen)
            return;
        _launcherOpen = open;
        _hoverItem = -1;
        Rect r = launcherRect();
        damage({r.x - SHADOW_MARGIN, r.y - SHADOW_MARGIN, r.w + 2 * SHADOW_MARGIN, r.h + 2 * SHADOW_MARGIN});
        damage(startButton());
    }

    // MARK: Layout

    Rect taskbarRect() const { return {0, _screen.height - TASKBAR_H, _screen.width, TASKBAR_H}; }
    Rect startButton() const { return {8, _screen.height - TASKBAR_H + 5, 44, 38}; }
    Rect clockRect() const { return {_screen.width - 130, _screen.height - TASKBAR_H + 5, 122, 38}; }

    Rect taskButton(int i) const {
        int n = 0;
        for (int k = 0; k < i; k++) {
            if (_windows[k].visible)
                n++;
        }
        return {62 + n * 178, _screen.height - TASKBAR_H + 6, 170, 36};
    }

    Rect launcherRect() const {
        int h = 92 + APP_COUNT * 52;
        return {8, _screen.height - TASKBAR_H - 10 - h, 300, h};
    }

    Rect launcherItem(int k) const {
        Rect l = launcherRect();
        return {l.x + 10, l.y + 82 + k * 52, l.w - 20, 46};
    }

    // MARK: Drawing

    void damage(Rect r) { _damage = _damage.unite(r.intersect(_screen.bounds())); }

    void tickClock() {
        unsigned long second = Sys::timeMs() / 1000;
        if (second != _clockSecond) {
            _clockSecond = second;
            damage(clockRect());
        }
    }

    // Recomposes and shows everything damaged, and moves the cursor.
    void flush() {
        Rect oldCursor = cursorRect(_drawnMouse);
        bool cursorMoved = _mouse.x != _drawnMouse.x or _mouse.y != _drawnMouse.y;
        _drawnMouse = _mouse;

        if (not _damage.empty()) {
            compose(_damage);
            present(_damage);
            _damage = {};
        }
        if (cursorMoved) {
            present(oldCursor);
            present(cursorRect(_drawnMouse));
        }
    }

    void compose(Rect area) {
        Canvas c(_back);
        c.clip = area;
        drawDesktop(c);
        for (int k = 0; k < _orderCount; k++)
            drawWindow(c, _windows[_order[k]], _order[k] == top());
        drawTaskbar(c);
        if (_launcherOpen)
            drawLauncher(c);
    }

    void drawDesktop(Canvas& c) {
        c.verticalGradient(_back.bounds(), Theme::NIGHT, Theme::MAROON);
        int cx = _back.width / 2, cy = (_back.height - TASKBAR_H) / 2;
        c.lotus(float(cx), float(cy + 10), 70, 70);
        char const* name = "Bhairav OS";
        c.text({cx - Font::system().measure(name, 2) / 2, cy + 32}, name, Theme::TEXT_DIM.withAlpha(150), 2);
        char const* shell = "KamalShell";
        c.text({cx - Font::system().measure(shell) / 2, cy + 70}, shell, Theme::TEXT_DIM.withAlpha(110));
    }

    void drawWindow(Canvas& c, Window const& w, bool focused) {
        if (w.area().intersect(c.clip).empty())
            return;
        c.shadow(w.frame, RADIUS, SHADOW, Color::hex(0x000000, focused ? 170 : 110));
        c.fillRounded(w.frame, RADIUS, Theme::SURFACE);

        // Title bar: round top corners, straight bottom edge (clipped).
        Rect saved = c.clip;
        c.clip = c.clip.intersect(w.titleBar());
        Rect bar{w.frame.x, w.frame.y, w.frame.w, TITLE_H + 2 * RADIUS};
        if (focused)
            c.fillRoundedGradient(bar, RADIUS, Theme::SAFFRON, Theme::LOTUS);
        else
            c.fillRoundedGradient(bar, RADIUS, Theme::SURFACE_ALT, Theme::BORDER);
        c.clip = saved;

        c.text({w.frame.x + 16, w.frame.y + 9}, w.title, focused ? Theme::WHITE : Theme::TEXT_DIM);
        Rect x = w.closeButton();
        c.fillCircle(x.x + x.w / 2.0f, x.y + x.h / 2.0f, 9, Theme::WHITE.withAlpha(focused ? 220 : 90));
        c.text({x.x + 7, x.y + 3}, "x", Theme::NIGHT);

        // The app's pixels, with the bottom corners rounded off.
        c.blit(w.surface, {w.content().x, w.content().y}, w.frame, RADIUS);
        c.strokeRounded(w.frame, RADIUS, 1, Theme::BORDER.withAlpha(focused ? 120 : 200));
    }

    void drawTaskbar(Canvas& c) {
        Rect bar = taskbarRect();
        if (bar.intersect(c.clip).empty())
            return;
        c.fill(bar, Color::hex(0x0B0718, 235));
        c.fill({0, bar.y, bar.w, 1}, Theme::BORDER);

        Rect start = startButton();
        if (_launcherOpen or _hoverStart)
            c.fillRounded(start, 10, Theme::SURFACE_ALT);
        c.lotus(start.x + start.w / 2.0f, start.bottom() - 9.0f, 12);

        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window const& w = _windows[i];
            if (not w.visible)
                continue;
            Rect b = taskButton(i);
            bool focused = i == top();
            c.fillRounded(b, 9, focused ? Theme::SAFFRON.withAlpha(70) : Theme::SURFACE_ALT.withAlpha(200));
            if (focused)
                c.fill({b.x + 14, b.bottom() - 3, b.w - 28, 2}, Theme::SAFFRON);
            // Fit the title in the button, ending in "..." if it's too long.
            constexpr unsigned FIT = 18;
            char label[FIT + 1];
            copyString(label, sizeof(label), w.title);
            if (Sys::length(w.title) > FIT)
                label[FIT - 3] = label[FIT - 2] = label[FIT - 1] = '.';
            c.text({b.x + 12, b.y + 10}, label, focused ? Theme::WHITE : Theme::TEXT_DIM);
        }

        Sys::SystemInfo info;
        Sys::systemInfo(info);
        Text<32> clock;
        addTwoDigits(clock, info.timeOfDay / 3600);
        clock.add(":");
        addTwoDigits(clock, info.timeOfDay / 60 % 60);
        clock.add(":");
        addTwoDigits(clock, info.timeOfDay % 60);
        clock.add(" UTC");
        Rect r = clockRect();
        c.text({r.right() - Font::system().measure(clock.cstr()) - 6, r.y + 11}, clock.cstr(), Theme::TEXT);
    }

    void drawLauncher(Canvas& c) {
        Rect l = launcherRect();
        c.shadow(l, 14, SHADOW, Color::hex(0x000000, 160));
        c.fillRounded(l, 14, Theme::SURFACE);
        c.strokeRounded(l, 14, 1, Theme::BORDER);

        c.lotus(l.x + 34.0f, l.y + 60.0f, 17);
        c.text({l.x + 66, l.y + 18}, "Bhairav OS", Theme::SAFFRON, 2);
        c.text({l.x + 66, l.y + 52}, "KamalShell", Theme::TEXT_DIM);

        for (int k = 0; k < APP_COUNT; k++) {
            Rect item = launcherItem(k);
            if (k == _hoverItem)
                c.fillRounded(item, 10, Theme::SURFACE_ALT);
            c.fillCircle(item.x + 22.0f, item.y + 23.0f, 6, k == _hoverItem ? Theme::SAFFRON : Theme::LOTUS);
            c.text({item.x + 40, item.y + 6}, APPS[k].label, Theme::TEXT);
            c.text({item.x + 40, item.y + 24}, APPS[k].detail, Theme::TEXT_DIM);
        }
    }

    static Rect cursorRect(Point p) { return {p.x, p.y, CURSOR_W, CURSOR_H}; }

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

    PixelFormat _format;
    Bitmap _screen, _back;
    Rect _damage;

    Window _windows[MAX_WINDOWS];
    int _order[MAX_WINDOWS] = {}; // stacking order, bottom to top
    int _orderCount = 0;
    int _focused = -1;
    int _placements = 0;

    Channel _input;
    bool _inputOpen = true;

    Point _mouse, _drawnMouse;
    unsigned _buttons = 0;
    int _dragging = -1;
    Point _dragOffset;
    int _captured = -1; // the window a mouse press started in

    bool _launcherOpen = false;
    bool _hoverStart = false;
    int _hoverItem = -1;

    unsigned long _clockSecond = 0;
};

Shell shell;

} // namespace

int main() {
    if (not shell.start()) {
        Sys::log("can't open the screen");
        return 1;
    }
    Sys::log("KamalShell is running");
    shell.run();
}
