// KamalShell🪷 - Beautiful and Responsive Graphical Shell
//
//   handle 1: the screen (shared memory)
//   handle 2: input events from the PS/2 driver
//   handle 3: the file system's connect channel (protocol/fs.h)
//   handle 4: the network's connect channel (protocol/net.h)
//
// The compositor and desktop of Bhairav OS. Apps are separate programs: the
// shell starts each one with a channel (its handle 1) and a connection to
// the file system (its handle 2) and the network (its handle 3), the app
// creates its
// window's pixels in shared memory, and the shell composes every window
// onto the screen (see kapala/protocol/shell.h).
//
// Drawing: everything that changes adds its rectangle to the damage; once
// per batch of events, that area is recomposed (desktop, windows bottom to
// top, taskbar, launcher) into a back buffer and copied to the screen, with
// the mouse cursor drawn on top.

#include <kapala/channel.h>
#include <kapala/gfx.h>
#include <kapala/net.h>
#include <kapala/protocol/fs.h>
#include <kapala/protocol/input.h>
#include <kapala/protocol/net.h>
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
constexpr Sys::Handle FILES = 3;
constexpr Sys::Handle NETWORK = 4;
constexpr Protocol::Fs::Connect CONNECT{Protocol::Fs::CONNECT_MAGIC};
constexpr Protocol::Net::Connect CONNECT_NET{Protocol::Net::CONNECT_MAGIC};

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
    {"browser", "Bhairav Browser", "explore the web"},
    {"files", "Files", "what's on the disk"},
    {"terminal", "Terminal", "type commands"},
    {"notes", "Notes", "write something down"},
    {"monitor", "System Monitor", "memory, tasks and time"},
    {"about", "About Bhairav OS", "the system and its parts"},
};

// Desktop icons (the same apps), with a short label each.
constexpr char const* ICON_LABELS[] = {"Browser", "Files", "Terminal", "Notes", "Monitor", "About"};
constexpr int ICON_SIZE = 56;
constexpr int ICON_CELL = 92;
constexpr int MIN_W = 260, MIN_H = 160;
constexpr unsigned long DOUBLE_CLICK_MS = 450;
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
    bool resizable = false;
    bool maximized = false;
    Rect restore;               // the frame before maximizing
    char argument[96] = {};     // sent to the app before Opened

    Rect content() const { return {frame.x, frame.y + TITLE_H, frame.w, frame.h - TITLE_H}; }
    Rect titleBar() const { return {frame.x, frame.y, frame.w, TITLE_H}; }
    Rect closeButton() const { return {frame.right() - 32, frame.y + 6, 22, 22}; }
    Rect maximizeButton() const { return {frame.right() - 60, frame.y + 6, 22, 22}; }
    Rect resizeGrip() const { return {frame.right() - 18, frame.bottom() - 18, 18, 18}; }
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

        // Our own network connection, for the status in the taskbar.
        Channel mine, theirs;
        Channel::createPair(mine, theirs);
        if (Sys::send(NETWORK, &CONNECT_NET, sizeof(CONNECT_NET), theirs.handle()) == Sys::OK) {
            theirs.release();
            _netOk = _net.connect(mine.release());
        }
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

    void launch(char const* program, char const* argument = "") {
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

        // The app's own file system connection.
        Channel files, filesServer;
        Channel::createPair(files, filesServer);
        if (Sys::send(FILES, &CONNECT, sizeof(CONNECT), filesServer.handle()) == Sys::OK)
            filesServer.release(); // moved to fs

        // And its network connection.
        Channel net, netServer;
        Channel::createPair(net, netServer);
        if (Sys::send(NETWORK, &CONNECT_NET, sizeof(CONNECT_NET), netServer.handle()) == Sys::OK)
            netServer.release(); // moved to net

        Sys::Handle give[] = {theirs.release(), files.release(), net.release()};
        if (Sys::spawn(program, give, 3) < 0) {
            for (Sys::Handle h : give)
                Sys::close(h); // (not moved: spawn changes nothing when it fails)
            Text<> message;
            message.add("can't start ").add(program);
            Sys::log(message.cstr());
            return;
        }
        _windows[slot] = {};
        _windows[slot].used = true;
        _windows[slot].channel = static_cast<Channel&&>(mine);
        copyString(_windows[slot].argument, sizeof(_windows[slot].argument), argument);
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
            case ShellType::Resize:
                resizeWindow(i, m, attached);
                attached = Sys::NO_HANDLE;
                break;
            case ShellType::Launch: {
                char program[Protocol::Shell::LAUNCH_ARGUMENT];
                copyString(program, sizeof(program), m.title);
                char argument[96];
                copyString(argument, sizeof(argument), m.title + Protocol::Shell::LAUNCH_ARGUMENT);
                launch(program, argument);
                break;
            }
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
        w.resizable = m.pressed;

        if (w.argument[0]) {
            ShellMessage a{};
            a.type = ShellType::Argument;
            copyString(a.title, sizeof(a.title), w.argument);
            send(i, a);
        }
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
        if (_resizing == i) {
            _resizing = -1;
            damage(outlineArea(_resizeRect));
        }
        if (_captured == i)
            _captured = -1;
        _windows[i] = {}; // closes the channel
        damage(taskbarRect());
        updateFocus();
    }

    // The app answered a ResizeRequest with pixels of the new size.
    void resizeWindow(int i, ShellMessage const& m, Sys::Handle memory) {
        Window& w = _windows[i];
        int maxW = _screen.width, maxH = _screen.height - TASKBAR_H - TITLE_H;
        bool sizeOk = w.visible and w.resizable and m.w >= 1 and m.h >= 1 and m.w <= maxW and m.h <= maxH;
        void* pixels = sizeOk and memory ? Sys::memoryMap(memory) : nullptr;
        if (memory)
            Sys::close(memory);
        if (not pixels)
            return;
        damage(w.area());
        Sys::memoryUnmap(w.surface.pixels);
        w.surface = {static_cast<unsigned*>(pixels), m.w, m.h, m.w, _format};
        w.frame.w = m.w;
        w.frame.h = m.h + TITLE_H;
        damage(w.area());
        damage(taskbarRect());
    }

    // Asks the app for a new content size (it answers with Resize).
    void requestSize(int i, int contentW, int contentH) {
        ShellMessage m{};
        m.type = ShellType::ResizeRequest;
        m.w = clampInt(contentW, MIN_W, _screen.width);
        m.h = clampInt(contentH, MIN_H, _screen.height - TASKBAR_H - TITLE_H);
        send(i, m);
    }

    void toggleMaximize(int i) {
        Window& w = _windows[i];
        if (not w.resizable)
            return;
        damage(w.area());
        if (w.maximized) {
            w.maximized = false;
            w.frame.x = w.restore.x;
            w.frame.y = w.restore.y;
            requestSize(i, w.restore.w, w.restore.h - TITLE_H);
        } else {
            w.maximized = true;
            w.restore = w.frame;
            w.frame.x = 0;
            w.frame.y = 0;
            requestSize(i, _screen.width, _screen.height - TASKBAR_H - TITLE_H);
        }
        damage(w.area());
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

        if (e.wheel)
            forwardMouse(ShellType::Wheel, e.wheel);

        if (_resizing >= 0) {
            Window& w = _windows[_resizing];
            damage(outlineArea(_resizeRect));
            _resizeRect = {w.frame.x, w.frame.y, clampInt(_mouse.x - w.frame.x + _dragOffset.x, MIN_W, _screen.width - w.frame.x),
                           clampInt(_mouse.y - w.frame.y + _dragOffset.y, MIN_H + TITLE_H, _screen.height - TASKBAR_H - w.frame.y)};
            damage(outlineArea(_resizeRect));
        } else if (_dragging >= 0) {
            Window& w = _windows[_dragging];
            damage(w.area());
            if (w.maximized) { // dragging a maximized window un-maximizes it, under the mouse
                w.maximized = false;
                int newW = w.restore.w;
                _dragOffset.x = clampInt(_dragOffset.x * newW / (w.frame.w ? w.frame.w : 1), 20, newW - 80);
                requestSize(_dragging, w.restore.w, w.restore.h - TITLE_H);
            }
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
    void forwardMouse(ShellType type, int wheel = 0) {
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
        m.h = wheel;
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
        unsigned long now = Sys::timeMs();
        if (i < 0) {
            // The desktop: icons open with a double click.
            int icon = iconAt(_mouse);
            bool twice = icon >= 0 and icon == _lastClickIcon and now - _lastClickTime < DOUBLE_CLICK_MS;
            if (icon != _selectedIcon) {
                if (_selectedIcon >= 0)
                    damage(iconRect(_selectedIcon));
                if (icon >= 0)
                    damage(iconRect(icon));
                _selectedIcon = icon;
            }
            _lastClickIcon = icon;
            _lastClickTime = now;
            if (twice) {
                launch(APPS[icon].program);
                _lastClickIcon = -1;
            }
            return;
        }
        if (_selectedIcon >= 0) {
            damage(iconRect(_selectedIcon));
            _selectedIcon = -1;
        }
        raise(i);
        Window& w = _windows[i];
        if (w.closeButton().contains(_mouse)) {
            sendSimple(i, ShellType::Close);
        } else if (w.resizable and w.maximizeButton().contains(_mouse)) {
            toggleMaximize(i);
        } else if (w.resizable and not w.maximized and w.resizeGrip().contains(_mouse)) {
            _resizing = i;
            _resizeRect = w.frame;
            _dragOffset = {w.frame.right() - _mouse.x, w.frame.bottom() - _mouse.y};
            damage(outlineArea(_resizeRect));
        } else if (w.titleBar().contains(_mouse)) {
            bool twice = _lastTitleClick == i and now - _lastClickTime < DOUBLE_CLICK_MS;
            _lastTitleClick = i;
            _lastClickTime = now;
            if (twice) {
                toggleMaximize(i);
                _lastTitleClick = -1;
                return;
            }
            _dragging = i;
            _dragOffset = {_mouse.x - w.frame.x, _mouse.y - w.frame.y};
        } else {
            _captured = i;
            forwardMouse(ShellType::MouseDown);
        }
    }

    void onRelease() {
        _dragging = -1;
        if (_resizing >= 0) {
            int i = _resizing;
            _resizing = -1;
            damage(outlineArea(_resizeRect));
            requestSize(i, _resizeRect.w, _resizeRect.h - TITLE_H);
        }
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
    Rect clockRect() const { return {_screen.width - 136, _screen.height - TASKBAR_H + 5, 128, 38}; }

    Rect taskButton(int i) const {
        int n = 0;
        for (int k = 0; k < i; k++) {
            if (_windows[k].visible)
                n++;
        }
        return {62 + n * 178, _screen.height - TASKBAR_H + 6, 170, 36};
    }

    Rect iconRect(int k) const { return {14, 16 + k * ICON_CELL, 84, ICON_CELL - 8}; }

    int iconAt(Point p) const {
        for (int k = 0; k < APP_COUNT; k++) {
            if (iconRect(k).contains(p))
                return k;
        }
        return -1;
    }

    static Rect outlineArea(Rect r) { return {r.x - 3, r.y - 3, r.w + 6, r.h + 6}; }

    Rect statusRect() const { return {_screen.width - 400, _screen.height - TASKBAR_H + 5, 262, 38}; }

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
            if (second % 3 == 0 and _netOk) {
                NetStatus status{};
                bool up = _net.status(status) == Sys::OK and status.up;
                if (up != _netUp or status.address != _netAddress) {
                    _netUp = up;
                    _netAddress = status.address;
                    damage(statusRect());
                }
            }
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
        if (_resizing >= 0) {
            c.fillRounded(_resizeRect, RADIUS, Theme::SAFFRON.withAlpha(28));
            c.strokeRounded(_resizeRect, RADIUS, 2, Theme::SAFFRON);
        }
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

        for (int k = 0; k < APP_COUNT; k++)
            drawIcon(c, k);
    }

    void drawIcon(Canvas& c, int k) {
        Rect cell = iconRect(k);
        if (cell.intersect(c.clip).empty())
            return;
        if (k == _selectedIcon)
            c.fillRounded(cell, 10, Theme::SAFFRON.withAlpha(45));
        Rect tile{cell.x + (cell.w - ICON_SIZE) / 2, cell.y + 4, ICON_SIZE, ICON_SIZE};
        c.shadow(tile, 14, 8, Color::hex(0x000000, 120));
        float x = float(tile.x), y = float(tile.y), s = float(ICON_SIZE);
        Color ink = Theme::WHITE;
        switch (k) {
        case 0: { // Browser: a globe
            c.fillRoundedGradient(tile, 14, Color::hex(0x2F6FEB), Color::hex(0x5B3FD0));
            float cx = x + s / 2, cy = y + s / 2;
            c.fillCircle(cx, cy, 17, ink);
            c.fillCircle(cx, cy, 15, Color::hex(0x3F5EE0));
            c.fill({int(cx) - 15, int(cy) - 1, 30, 2}, ink);
            c.fill({int(cx) - 1, int(cy) - 15, 2, 30}, ink);
            c.fill({int(cx) - 12, int(cy) - 8, 24, 2}, ink.withAlpha(170));
            c.fill({int(cx) - 12, int(cy) + 7, 24, 2}, ink.withAlpha(170));
            break;
        }
        case 1: // Files: a folder
            c.fillRoundedGradient(tile, 14, Color::hex(0xF2A33A), Color::hex(0xE0782B));
            c.fillRounded({tile.x + 12, tile.y + 15, 16, 8}, 3, Color::hex(0xFFE2A8));
            c.fillRounded({tile.x + 12, tile.y + 19, 32, 23}, 4, Color::hex(0xFFE2A8));
            c.fillRounded({tile.x + 12, tile.y + 24, 32, 18}, 4, Color::hex(0xFFD07A));
            break;
        case 2: // Terminal: ">_"
            c.fillRoundedGradient(tile, 14, Color::hex(0x2B2140), Color::hex(0x120C22));
            c.strokeRounded(tile, 14, 1, Theme::BORDER);
            c.text({tile.x + 12, tile.y + 14}, ">", Theme::SAFFRON, 2);
            c.fill({tile.x + 30, tile.y + 40, 14, 3}, Theme::TEXT);
            break;
        case 3: // Notes: a page with lines
            c.fillRoundedGradient(tile, 14, Theme::LOTUS, Color::hex(0xB8336A));
            c.fillRounded({tile.x + 14, tile.y + 10, 28, 36}, 4, Color::hex(0xFFF6EC));
            for (int line = 0; line < 4; line++)
                c.fill({tile.x + 19, tile.y + 18 + line * 7, line == 3 ? 10 : 18, 2}, Color::hex(0xC9A99A));
            break;
        case 4: { // Monitor: a little graph
            c.fillRoundedGradient(tile, 14, Color::hex(0x1E9E7A), Color::hex(0x147A8C));
            int heights[] = {10, 18, 13, 26, 20, 30};
            for (int b = 0; b < 6; b++)
                c.fillRounded({tile.x + 11 + b * 6, tile.y + 44 - heights[b], 4, heights[b]}, 2, ink.withAlpha(220));
            break;
        }
        default: // About: the lotus
            c.fillRoundedGradient(tile, 14, Theme::SAFFRON, Theme::LOTUS);
            c.lotus(x + s / 2, y + s - 16, 14);
            break;
        }
        Font const& font = Font::system();
        char const* label = ICON_LABELS[k];
        int lx = cell.x + (cell.w - font.measure(label)) / 2;
        c.text({lx + 1, tile.bottom() + 7}, label, Color::hex(0x000000, 160));
        c.text({lx, tile.bottom() + 6}, label, Theme::TEXT);
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
        Font const& markFont = Font::system();
        c.text({x.x + (x.w - markFont.measure("\u00d7")) / 2, x.y + (x.h - markFont.height) / 2}, "\u00d7", Theme::NIGHT);
        if (w.resizable) {
            Rect m = w.maximizeButton();
            c.fillCircle(m.x + m.w / 2.0f, m.y + m.h / 2.0f, 9, Theme::WHITE.withAlpha(focused ? 150 : 60));
            Rect box = w.maximized ? Rect{m.x + 7, m.y + 7, 8, 8} : Rect{m.x + 6, m.y + 6, 10, 10};
            c.strokeRounded(box, 1.5f, 1.5f, Theme::NIGHT);
        }

        // The app's pixels, with the bottom corners rounded off.
        c.blit(w.surface, {w.content().x, w.content().y}, w.frame, RADIUS);
        c.strokeRounded(w.frame, RADIUS, 1, Theme::BORDER.withAlpha(focused ? 120 : 200));
        if (w.resizable and not w.maximized) { // the resize grip: three dots
            Rect g = w.resizeGrip();
            for (int d = 0; d < 3; d++)
                c.fillCircle(g.x + 13.0f - d * 4.0f, g.y + 13.0f, 1.3f, Theme::TEXT_DIM.withAlpha(160));
            c.fillCircle(g.x + 13.0f, g.y + 9.0f, 1.3f, Theme::TEXT_DIM.withAlpha(160));
            c.fillCircle(g.x + 9.0f, g.y + 9.0f, 1.3f, Theme::TEXT_DIM.withAlpha(160));
            c.fillCircle(g.x + 13.0f, g.y + 5.0f, 1.3f, Theme::TEXT_DIM.withAlpha(160));
        }
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
        Rect r = clockRect();
        Font const& font = Font::system();
        c.text({r.right() - font.measure(clock.cstr()) - 6, r.y + 2}, clock.cstr(), Theme::TEXT);
        Text<32> date;
        formatDate(info.unixTime, date);
        c.text({r.right() - font.measure(date.cstr()) - 6, r.y + 20}, date.cstr(), Theme::TEXT_DIM);

        // Network: a dot and the address.
        Rect s = statusRect();
        Text<40> net;
        if (_netUp)
            Net::format(_netAddress, net);
        else
            net.add(_netOk ? "no network yet" : "offline");
        int tx = s.right() - font.measure(net.cstr()) - 4;
        c.fillCircle(tx - 12.0f, s.y + 19.0f, 4.5f, _netUp ? Color::hex(0x3DDC84) : Theme::BORDER);
        c.text({tx, s.y + 11}, net.cstr(), _netUp ? Theme::TEXT : Theme::TEXT_DIM);
    }

    // "Wed 11 Nov 2026" (UTC)
    static void formatDate(unsigned long unixTime, Text<32>& out) {
        static constexpr char const* DAYS[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
        static constexpr char const* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        long days = static_cast<long>(unixTime / 86400);
        long z = days + 719468, era = z / 146097, doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        long day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
        long year = yoe + era * 400 + (month <= 2);
        out.add(DAYS[days % 7]).add(" ").add(day).add(" ").add(MONTHS[month - 1]).add(" ").add(year);
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

    int _resizing = -1; // the window whose corner is being dragged
    Rect _resizeRect;   // its outline
    int _selectedIcon = -1, _lastClickIcon = -1, _lastTitleClick = -1;
    unsigned long _lastClickTime = 0;

    Net _net;
    bool _netOk = false, _netUp = false;
    unsigned _netAddress = 0;

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
