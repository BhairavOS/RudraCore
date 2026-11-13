// KamalShell🪷 - Beautiful and Responsive Graphical Shell
//
//   handle 1: the screen (shared memory)
//   handle 2: input events from the PS/2 driver
//   handle 3: the file system's connect channel (protocol/fs.h)
//   handle 4: the network's connect channel (protocol/net.h)
//
// The compositor and desktop of Bhairav OS. Apps are separate programs: the
// shell starts each one with a channel (its handle 1) and connections to the
// file system (handle 2) and the network (handle 3); the app draws its
// window's pixels in shared memory, and the shell composes every window
// onto the screen (see kapala/protocol/shell.h).
//
// Apps are either built in (in the boot image) or installed on the disk in
// /apps/<id>/ (kapala/apps.h); both kinds appear in the dock and the app
// grid and start the same way.
//
// The desktop: a thin bar at the top (the lotus menu, the active app, the
// network and the clock), a floating dock at the bottom (apps, a dot under
// the running ones, and the app grid), and windows in between, with their controls
// at the top left: close, minimize, maximize.
//
// Drawing: everything that changes adds its rectangle to the damage; once
// per batch of events, that area is recomposed (wallpaper, windows bottom
// to top, bars, dock, menus) into a back buffer and copied to the screen,
// with the mouse cursor drawn on top.

#include <kapala/apps.h>
#include <kapala/channel.h>
#include <kapala/files.h>
#include <kapala/gfx.h>
#include <kapala/image.h>
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
using Protocol::Fs::Scope;
using Protocol::Net::Access;

// What an app's file and network connections reach (protocol/fs.h, net.h).
// KamalShell sets this when it connects the app; the app can't change it.
struct Profile {
    Scope files;
    Access network;
};

using Protocol::Fs::makeConnect;
using Protocol::Net::makeConnect;

constexpr int MAX_WINDOWS = 16;
constexpr int TOPBAR_H = 28;
constexpr int TITLE_H = 32;
constexpr int RADIUS = 11;
constexpr int SHADOW = 26;
constexpr int SHADOW_MARGIN = SHADOW + 8;

constexpr int DOCK_ICON = 46;
constexpr int DOCK_GAP = 12;
constexpr int DOCK_PAD = 10;
constexpr int DOCK_SEPARATOR = 18;                   // extra room around the separators
constexpr int DOCK_H = DOCK_ICON + 2 * DOCK_PAD + 6; // room for the running dots
constexpr int DOCK_BOTTOM = 8;                       // space under the dock
constexpr int DOCK_RESERVE = DOCK_H + DOCK_BOTTOM + 6; // kept free of maximized windows
constexpr int MAX_DOCK_INSTALLED = 6;                  // more are in the app grid

constexpr int GRID_ICON = 64, GRID_CELL_W = 124, GRID_CELL_H = 112, GRID_COLUMNS = 6;
constexpr int MAX_INSTALLED = 24;

constexpr int MIN_W = 260, MIN_H = 160;
constexpr unsigned long DOUBLE_CLICK_MS = 450;

// Window controls: close, minimize, maximize.
constexpr Color CONTROL[3] = {Color::hex(0xF0566F), Color::hex(0xF4B740), Color::hex(0x44C28C)};
constexpr Color CONTROL_IDLE = Color::hex(0x4A4656);

struct AppEntry {
    char const* program;
    char const* name;
    Profile profile;
};

// Built-in apps get exactly what they use. The App Store and Terminal
// install apps (they write /apps and /var); Files and Notes work on the
// user's files; the rest only have their own private folder.
constexpr AppEntry APPS[] = {
    {"browser", "Browser", {Scope::Private, Access::Internet}},
    {"files", "Files", {Scope::User, Access::None}},
    {"terminal", "Terminal", {Scope::System, Access::Internet}},
    {"notes", "Notes", {Scope::User, Access::None}},
    {"monitor", "Monitor", {Scope::Private, Access::None}},
    {"store", "App Store", {Scope::System, Access::Internet}},
    {"about", "About Bhairav OS", {Scope::Private, Access::None}},
};
constexpr int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

// The lotus menu.
enum MenuItem { MENU_ABOUT, MENU_APPS, MENU_STORE, MENU_SEPARATOR, MENU_SHOW_DESKTOP, MENU_COUNT };
constexpr char const* MENU_LABELS[MENU_COUNT] = {"About Bhairav OS", "All Apps", "App Store", "", "Show Desktop"};

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

bool sameString(char const* a, char const* b) {
    while (*a and *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void addTwoDigits(Text<48>& t, unsigned long v) {
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
    char program[24] = {}; // what was started (built-in name or installed app id)
    Rect frame;
    bool resizable = false;
    bool maximized = false;
    bool minimized = false;
    Rect restore;           // the frame before maximizing
    char argument[96] = {}; // sent to the app before Opened

    Rect content() const { return {frame.x, frame.y + TITLE_H, frame.w, frame.h - TITLE_H}; }
    Rect titleBar() const { return {frame.x, frame.y, frame.w, TITLE_H}; }
    // The controls at the top left: 0 close, 1 minimize, 2 maximize.
    Rect control(int k) const { return {frame.x + 12 + k * 20, frame.y + TITLE_H / 2 - 8, 16, 16}; }
    Rect controls() const { return {frame.x + 8, frame.y + 4, 66, TITLE_H - 8}; }
    Rect resizeGrip() const { return {frame.right() - 16, frame.bottom() - 16, 16, 16}; }
    Rect area() const { // everything it draws, shadow included
        return {frame.x - SHADOW_MARGIN, frame.y - SHADOW_MARGIN, frame.w + 2 * SHADOW_MARGIN,
                frame.h + 2 * SHADOW_MARGIN + 10};
    }
    bool shown() const { return visible and not minimized; }
};

// An app installed on the disk.
struct Installed {
    AppInfo info;
    Image icon;     // as in its package (empty if it has none)
    Image dockIcon; // scaled for the dock
    Image gridIcon; // scaled for the app grid
};

// Something in the dock or the grid: a built-in app, an installed one, or
// (dock only) the button that opens the grid.
struct Slot {
    enum Kind : unsigned char { BuiltIn, InstalledApp, Grid } kind;
    int index;
    int x; // dock: left edge
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
        _wallpaper = Bitmap::allocate(_screen.width, _screen.height, _format);
        if (not _back.pixels or not _wallpaper.pixels)
            return false;
        paintWallpaper();

        _input = Channel(INPUT);
        _mouse = _drawnMouse = {_screen.width / 2, _screen.height / 2};

        // Our own connections: the network (for the status in the top bar)
        // and the files (for installed apps).
        Channel mine, theirs;
        Channel::createPair(mine, theirs);
        auto shellNet = makeConnect(Access::None, "kamalshell"); // the top bar only shows the address
        if (Sys::send(NETWORK, &shellNet, sizeof(shellNet), theirs.handle()) == Sys::OK) {
            theirs.release();
            _netOk = _net.connect(mine.release());
        }
        Channel filesMine, filesTheirs;
        Channel::createPair(filesMine, filesTheirs);
        auto shellFiles = makeConnect(Scope::System, "kamalshell"); // reads /apps and /var/permissions
        if (Sys::send(FILES, &shellFiles, sizeof(shellFiles), filesTheirs.handle()) == Sys::OK) {
            filesTheirs.release();
            _filesOk = _files.connect(filesMine.release());
        }
        rescanApps();
        damage(_screen.bounds());
        flush();
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
    // MARK: Installed apps

    void rescanApps() {
        for (int j = 0; j < _installedCount; j++)
            _installed[j] = Installed{};
        _installedCount = 0;
        if (_filesOk) {
            static AppInfo infos[MAX_INSTALLED];
            long n = listApps(_files, infos, MAX_INSTALLED);
            for (long j = 0; j < n; j++) {
                Installed& app = _installed[_installedCount++];
                app.info = infos[j];
                Text<96> path;
                path.add("/apps/").add(app.info.id).add("/icon.png");
                unsigned long length = 0;
                unsigned char* png = readWholeFile(_files, path.cstr(), length, 2 * 1024 * 1024);
                if (png and Image::decode(png, length, app.icon, 1024 * 1024)) {
                    app.icon.scaled(DOCK_ICON, DOCK_ICON, app.dockIcon);
                    app.icon.scaled(GRID_ICON, GRID_ICON, app.gridIcon);
                }
                delete[] png;
            }
        }
        layoutDock();
        damage(_screen.bounds());
    }

    // Starts an installed app: its program is read from the disk into
    // shared memory, and the kernel loads it from there.
    long startInstalled(char const* id, Sys::Handle const* give, unsigned long count) {
        if (not _filesOk)
            return Sys::NOT_FOUND;
        Text<96> path;
        path.add("/apps/").add(id).add("/app.elf");
        FileInfo info{};
        if (_files.stat(path.cstr(), info) < 0 or info.directory or info.size == 0 or info.size > MAX_APP_SIZE)
            return Sys::NOT_FOUND;
        long memory = Sys::memoryCreate(info.size);
        if (memory < 0)
            return memory;
        void* bytes = Sys::memoryMap(static_cast<Sys::Handle>(memory));
        long result = Sys::OUT_OF_MEMORY;
        if (bytes and _files.read(path.cstr(), bytes, info.size) == static_cast<long>(info.size)) {
            Sys::SpawnImage request{};
            request.memory = static_cast<Sys::Handle>(memory);
            request.size = info.size;
            copyString(request.name, sizeof(request.name), id);
            for (unsigned long i = 0; i < count; i++)
                request.handles[i] = give[i];
            request.handleCount = count;
            result = Sys::spawnImage(request);
        }
        if (bytes)
            Sys::memoryUnmap(bytes);
        Sys::close(static_cast<Sys::Handle>(memory));
        return result;
    }

    int findInstalled(char const* id) const {
        for (int j = 0; j < _installedCount; j++) {
            if (sameString(_installed[j].info.id, id))
                return j;
        }
        return -1;
    }

    int findBuiltIn(char const* program) const {
        for (int k = 0; k < APP_COUNT; k++) {
            if (sameString(APPS[k].program, program))
                return k;
        }
        return -1;
    }

    char const* programOf(Slot const& s) const {
        return s.kind == Slot::BuiltIn ? APPS[s.index].program : _installed[s.index].info.id;
    }

    char const* nameOf(Slot const& s) const {
        if (s.kind == Slot::Grid)
            return "All Apps";
        return s.kind == Slot::BuiltIn ? APPS[s.index].name : _installed[s.index].info.title;
    }

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

        // What it may reach: a built-in's fixed profile, or what the user
        // granted an installed app (nothing but its own folder by default).
        Profile profile{Scope::Private, Access::None};
        Grants grants;
        int builtIn = findBuiltIn(program);
        if (builtIn >= 0) {
            profile = APPS[builtIn].profile;
        } else if (_filesOk) {
            grants = readGrants(_files, program);
            unsigned granted = grants.permissions;
            if (granted & PERMISSION_FILES)
                profile.files = Scope::User;
            else if (not(granted & PERMISSION_FOLDER))
                grants.folder[0] = '\0'; // no folder of the user's
            if (granted & PERMISSION_NETWORK)
                profile.network = Access::Internet;
            else if (granted & PERMISSION_LOCAL_NETWORK)
                profile.network = Access::Local;
            else if ((granted & PERMISSION_HOSTS) and grants.hosts[0])
                profile.network = Access::Hosts;
        }

        // The app's own file system connection.
        Channel files, filesServer;
        Channel::createPair(files, filesServer);
        auto filesHello = makeConnect(profile.files, program, profile.files == Scope::Private ? grants.folder : "");
        if (Sys::send(FILES, &filesHello, sizeof(filesHello), filesServer.handle()) == Sys::OK)
            filesServer.release(); // moved to fs

        // And its network connection.
        Channel net, netServer;
        Channel::createPair(net, netServer);
        auto netHello = makeConnect(profile.network, program, profile.network == Access::Hosts ? grants.hosts : "");
        if (Sys::send(NETWORK, &netHello, sizeof(netHello), netServer.handle()) == Sys::OK)
            netServer.release(); // moved to net

        // Built-in programs first (an installed app can't replace one).
        Sys::Handle give[] = {theirs.handle(), files.handle(), net.handle()};
        long result = Sys::spawn(program, give, 3);
        if (result == Sys::NOT_FOUND)
            result = startInstalled(program, give, 3);
        if (result < 0) {
            Text<> message;
            message.add("can't start ").add(program);
            Sys::log(message.cstr());
            return; // (not moved: the channels close on their own)
        }
        theirs.release();
        files.release();
        net.release();
        _windows[slot] = {};
        _windows[slot].used = true;
        _windows[slot].channel = static_cast<Channel&&>(mine);
        copyString(_windows[slot].argument, sizeof(_windows[slot].argument), argument);
        copyString(_windows[slot].program, sizeof(_windows[slot].program), program);
        damage(dockArea());
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
                if (w.shown()) {
                    Rect local = Rect{m.x, m.y, m.w, m.h}.intersect(w.surface.bounds());
                    damage(local.offset(w.content().x, w.content().y).intersect(w.content()));
                }
                break;
            case ShellType::SetTitle:
                copyString(w.title, sizeof(w.title), m.title);
                damage(w.titleBar());
                damage(topBarRect());
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
            case ShellType::AppsChanged: rescanApps(); break;
            default:
                break;
            }
            if (attached and m.type != ShellType::CreateWindow)
                Sys::close(attached);
        }
    }

    // Where windows may go: between the top bar and the dock.
    Rect workArea() const {
        return {0, TOPBAR_H, _screen.width, _screen.height - TOPBAR_H - DOCK_RESERVE};
    }

    void createWindow(int i, ShellMessage const& m, Sys::Handle memory) {
        Window& w = _windows[i];
        Rect work = workArea();
        bool sizeOk = m.w > 0 and m.h > 0 and m.w <= _screen.width and m.h <= _screen.height;
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

        // Centered in the work area, each new one a little lower and right.
        int k = _placements++ % 6;
        int fw = m.w, fh = m.h + TITLE_H;
        int x = clampInt(work.x + (work.w - fw) / 2 - 75 + k * 30, work.x + 10, work.right() - fw - 10);
        int y = clampInt(work.y + (work.h - fh) / 2 - 50 + k * 26, work.y + 10, work.bottom() - fh - 6);
        x = x < work.x ? work.x : x;
        y = y < work.y ? work.y : y;
        w.frame = {x, y, fw, fh};
        w.visible = true;
        w.resizable = m.pressed;
        // Too big for the space between the bars: a resizable window is
        // asked to fit (a fixed one just sits under the dock).
        if (w.resizable and (fw > work.w - 20 or fh > work.h - 10))
            requestSize(i, fw < work.w - 20 ? fw : work.w - 20, (fh < work.h - 10 ? fh : work.h - 10) - TITLE_H);

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
        if (_hoverControls == i)
            _hoverControls = -1;
        _windows[i] = {}; // closes the channel
        damage(dockArea());
        damage(topBarRect());
        updateFocus();
    }

    // The app answered a ResizeRequest with pixels of the new size.
    void resizeWindow(int i, ShellMessage const& m, Sys::Handle memory) {
        Window& w = _windows[i];
        Rect work = workArea();
        bool sizeOk = w.visible and w.resizable and m.w >= 1 and m.h >= 1 and m.w <= work.w and
                      m.h <= work.h - TITLE_H;
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
    }

    // Asks the app for a new content size (it answers with Resize).
    void requestSize(int i, int contentW, int contentH) {
        Rect work = workArea();
        ShellMessage m{};
        m.type = ShellType::ResizeRequest;
        m.w = clampInt(contentW, MIN_W, work.w);
        m.h = clampInt(contentH, MIN_H, work.h - TITLE_H);
        send(i, m);
    }

    void toggleMaximize(int i) {
        Window& w = _windows[i];
        if (not w.resizable)
            return;
        damage(w.area());
        Rect work = workArea();
        if (w.maximized) {
            w.maximized = false;
            w.frame.x = w.restore.x;
            w.frame.y = w.restore.y;
            requestSize(i, w.restore.w, w.restore.h - TITLE_H);
        } else {
            w.maximized = true;
            w.restore = w.frame;
            w.frame.x = work.x;
            w.frame.y = work.y;
            requestSize(i, work.w, work.h - TITLE_H);
        }
        damage(w.area());
    }

    void minimize(int i) {
        Window& w = _windows[i];
        if (not w.shown())
            return;
        w.minimized = true;
        damage(w.area());
        damage(dockArea());
        if (_hoverControls == i)
            _hoverControls = -1;
        updateFocus();
    }

    void restoreWindow(int i) {
        Window& w = _windows[i];
        if (w.minimized) {
            w.minimized = false;
            damage(w.area());
        }
        raise(i);
        damage(dockArea());
    }

    // MARK: Stacking and focus

    // The front window that's showing (it has the keyboard), or -1.
    int top() const {
        for (int k = _orderCount - 1; k >= 0; k--) {
            if (_windows[_order[k]].shown())
                return _order[k];
        }
        return -1;
    }

    void removeFromOrder(int i) {
        int j = 0;
        for (int k = 0; k < _orderCount; k++) {
            if (_order[k] != i)
                _order[j++] = _order[k];
        }
        _orderCount = j;
    }

    void raise(int i) {
        bool already = _orderCount and _order[_orderCount - 1] == i;
        if (not already) {
            removeFromOrder(i);
            _order[_orderCount++] = i;
            damage(_windows[i].area());
        }
        updateFocus();
    }

    // Tells apps when they gain or lose the keyboard (the front window has it).
    void updateFocus() {
        int now = top();
        if (now == _focused)
            return;
        if (_focused >= 0 and _windows[_focused].visible) {
            sendSimple(_focused, ShellType::Focus, false);
            damage(_windows[_focused].area());
        }
        _focused = now;
        if (_focused >= 0) {
            sendSimple(_focused, ShellType::Focus, true);
            damage(_windows[_focused].area());
        }
        damage(topBarRect());
        damage(dockArea());
    }

    int windowAt(Point p) const {
        for (int k = _orderCount - 1; k >= 0; k--) {
            Window const& w = _windows[_order[k]];
            if (w.shown() and w.frame.contains(p))
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
        if (e.pressed and e.character == 0x1b and (_menuOpen or _gridOpen)) { // Escape
            setMenu(false);
            setGrid(false);
            return;
        }
        if (_gridOpen)
            return;
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

        if (e.wheel and not _gridOpen)
            forwardMouse(ShellType::Wheel, e.wheel);

        Rect work = workArea();
        if (_resizing >= 0) {
            Window& w = _windows[_resizing];
            damage(outlineArea(_resizeRect));
            _resizeRect = {w.frame.x, w.frame.y,
                           clampInt(_mouse.x - w.frame.x + _dragOffset.x, MIN_W, _screen.width - w.frame.x),
                           clampInt(_mouse.y - w.frame.y + _dragOffset.y, MIN_H + TITLE_H, work.bottom() - w.frame.y)};
            damage(outlineArea(_resizeRect));
        } else if (_dragging >= 0) {
            Window& w = _windows[_dragging];
            damage(w.area());
            if (w.maximized) { // dragging a maximized window un-maximizes it, under the mouse
                w.maximized = false;
                int newW = w.restore.w;
                _dragOffset.x = clampInt(_dragOffset.x * newW / (w.frame.w ? w.frame.w : 1), 80, newW - 40);
                requestSize(_dragging, w.restore.w, w.restore.h - TITLE_H);
            }
            w.frame.x = clampInt(_mouse.x - _dragOffset.x, 80 - w.frame.w, _screen.width - 80);
            w.frame.y = clampInt(_mouse.y - _dragOffset.y, TOPBAR_H, _screen.height - TITLE_H - 10);
            damage(w.area());
        } else if (not _gridOpen) {
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
            if (dockRect().contains(_mouse) or topBarRect().contains(_mouse))
                return;
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
        if (_gridOpen) {
            int item = gridItemAt(_mouse);
            setGrid(false);
            if (item >= 0)
                openSlot(gridSlot(item));
            return;
        }
        if (_menuOpen) {
            if (menuRect().contains(_mouse)) {
                if (_hoverMenu >= 0)
                    chooseMenu(_hoverMenu);
                setMenu(false);
                return;
            }
            setMenu(false);
            if (lotusRect().contains(_mouse))
                return; // the lotus closes its own menu
        }

        if (topBarRect().contains(_mouse)) {
            if (lotusRect().contains(_mouse))
                setMenu(true);
            return;
        }

        if (dockRect().contains(_mouse)) {
            int k = dockItemAt(_mouse);
            if (k >= 0)
                openSlot(_dock[k]);
            return;
        }

        int i = windowAt(_mouse);
        if (i < 0)
            return;
        unsigned long now = Sys::timeMs();
        raise(i);
        Window& w = _windows[i];
        if (w.control(0).contains(_mouse)) {
            sendSimple(i, ShellType::Close);
        } else if (w.control(1).contains(_mouse)) {
            minimize(i);
        } else if (w.control(2).contains(_mouse)) {
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

    // A dock or grid item: bring its app's windows forward (restoring a
    // minimized one), or start it if it isn't running.
    void openProgram(char const* program) {
        for (int o = _orderCount - 1; o >= 0; o--) { // the most recently used first
            Window const& w = _windows[_order[o]];
            if (w.visible and sameString(w.program, program)) {
                restoreWindow(_order[o]);
                return;
            }
        }
        launch(program);
    }

    void openSlot(Slot const& s) {
        if (s.kind == Slot::Grid)
            setGrid(not _gridOpen);
        else
            openProgram(programOf(s));
    }

    bool isRunning(char const* program) const {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (_windows[i].used and sameString(_windows[i].program, program))
                return true;
        }
        return false;
    }

    void chooseMenu(int item) {
        switch (item) {
        case MENU_ABOUT: openProgram("about"); break;
        case MENU_APPS: setGrid(true); break;
        case MENU_STORE: openProgram("store"); break;
        case MENU_SHOW_DESKTOP:
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (_windows[i].shown())
                    minimize(i);
            }
            break;
        default: break;
        }
    }

    void updateHover() {
        int dock = not _gridOpen and dockRect().contains(_mouse) ? dockItemAt(_mouse) : -1;
        if (dock != _hoverDock) {
            damage(dockArea());
            _hoverDock = dock;
        }
        int grid = _gridOpen ? gridItemAt(_mouse) : -1;
        if (grid != _hoverGrid) {
            damage(gridRect());
            _hoverGrid = grid;
        }
        int menu = -1;
        if (_menuOpen) {
            for (int k = 0; k < MENU_COUNT; k++) {
                if (k != MENU_SEPARATOR and menuItemRect(k).contains(_mouse))
                    menu = k;
            }
        }
        if (menu != _hoverMenu) {
            damage(menuRect());
            _hoverMenu = menu;
        }
        // The controls show their symbols while the pointer is over them.
        int controls = -1;
        int under = _gridOpen ? -1 : windowAt(_mouse);
        if (under >= 0 and _windows[under].controls().contains(_mouse))
            controls = under;
        if (controls != _hoverControls) {
            if (_hoverControls >= 0)
                damage(_windows[_hoverControls].controls());
            if (controls >= 0)
                damage(_windows[controls].controls());
            _hoverControls = controls;
        }
        bool lotus = lotusRect().contains(_mouse);
        if (lotus != _hoverLotus) {
            _hoverLotus = lotus;
            damage(lotusRect());
        }
    }

    void setMenu(bool open) {
        if (open == _menuOpen)
            return;
        _menuOpen = open;
        _hoverMenu = -1;
        Rect r = menuRect();
        damage({r.x - SHADOW_MARGIN, r.y - SHADOW_MARGIN, r.w + 2 * SHADOW_MARGIN, r.h + 2 * SHADOW_MARGIN});
        damage(lotusRect());
    }

    void setGrid(bool open) {
        if (open == _gridOpen)
            return;
        _gridOpen = open;
        _hoverGrid = -1;
        damage(_screen.bounds()); // the whole screen dims behind it
    }

    // MARK: Layout

    Rect topBarRect() const { return {0, 0, _screen.width, TOPBAR_H}; }
    Rect lotusRect() const { return {8, 2, 98, TOPBAR_H - 4}; }

    // The dock's items: built-in apps, then installed ones (after a
    // separator), then the app grid button (after another).
    void layoutDock() {
        _dockCount = 0;
        int x = DOCK_PAD;
        for (int k = 0; k < APP_COUNT; k++) {
            _dock[_dockCount++] = {Slot::BuiltIn, k, x};
            x += DOCK_ICON + DOCK_GAP;
        }
        int shownInstalled = _installedCount < MAX_DOCK_INSTALLED ? _installedCount : MAX_DOCK_INSTALLED;
        if (shownInstalled)
            x += DOCK_SEPARATOR;
        for (int j = 0; j < shownInstalled; j++) {
            _dock[_dockCount++] = {Slot::InstalledApp, j, x};
            x += DOCK_ICON + DOCK_GAP;
        }
        x += DOCK_SEPARATOR;
        _dock[_dockCount++] = {Slot::Grid, 0, x};
        _dockWidth = x + DOCK_ICON + DOCK_PAD;
    }

    Rect dockRect() const {
        return {(_screen.width - _dockWidth) / 2, _screen.height - DOCK_BOTTOM - DOCK_H, _dockWidth, DOCK_H};
    }

    // The dock, its labels and lifted icons included.
    Rect dockArea() const { return dockRect().inset(-40); }

    Rect dockIcon(int k) const {
        Rect d = dockRect();
        int lift = k == _hoverDock ? 4 : 0;
        return {d.x + _dock[k].x, d.y + DOCK_PAD - lift, DOCK_ICON, DOCK_ICON};
    }

    int dockItemAt(Point p) const {
        Rect d = dockRect();
        for (int k = 0; k < _dockCount; k++) {
            Rect cell{d.x + _dock[k].x - DOCK_GAP / 2, d.y, DOCK_ICON + DOCK_GAP, d.h};
            if (cell.contains(p))
                return k;
        }
        return -1;
    }

    // The app grid: every app, built-in and installed.
    int gridCount() const { return APP_COUNT + _installedCount; }

    Slot gridSlot(int item) const {
        return item < APP_COUNT ? Slot{Slot::BuiltIn, item, 0} : Slot{Slot::InstalledApp, item - APP_COUNT, 0};
    }

    Rect gridRect() const {
        int rows = (gridCount() + GRID_COLUMNS - 1) / GRID_COLUMNS;
        int w = GRID_COLUMNS * GRID_CELL_W + 48, h = rows * GRID_CELL_H + 92;
        return {(_screen.width - w) / 2, TOPBAR_H + (workArea().h - h) / 2, w, h};
    }

    Rect gridCell(int item) const {
        Rect g = gridRect();
        return {g.x + 24 + (item % GRID_COLUMNS) * GRID_CELL_W, g.y + 68 + (item / GRID_COLUMNS) * GRID_CELL_H,
                GRID_CELL_W, GRID_CELL_H};
    }

    int gridItemAt(Point p) const {
        for (int k = 0; k < gridCount(); k++) {
            if (gridCell(k).contains(p))
                return k;
        }
        return -1;
    }

    Rect menuRect() const { return {8, TOPBAR_H + 4, 220, 12 + 4 * 30 + 11}; }

    Rect menuItemRect(int k) const {
        Rect m = menuRect();
        int y = m.y + 6;
        for (int i = 0; i < k; i++)
            y += i == MENU_SEPARATOR ? 11 : 30;
        return {m.x + 6, y, m.w - 12, k == MENU_SEPARATOR ? 11 : 30};
    }

    static Rect outlineArea(Rect r) { return {r.x - 3, r.y - 3, r.w + 6, r.h + 6}; }

    // MARK: Drawing

    void damage(Rect r) { _damage = _damage.unite(r.intersect(_screen.bounds())); }

    void tickClock() {
        unsigned long second = Sys::timeMs() / 1000;
        if (second == _clockSecond)
            return;
        _clockSecond = second;
        Sys::SystemInfo info;
        Sys::systemInfo(info);
        unsigned long minute = info.unixTime / 60;
        if (minute != _clockMinute) { // the bar shows minutes
            _clockMinute = minute;
            damage(topBarRect());
        }
        if (second % 3 == 0 and _netOk) {
            NetStatus status{};
            bool up = _net.status(status) == Sys::OK and status.up;
            if (up != _netUp or status.address != _netAddress) {
                _netUp = up;
                _netAddress = status.address;
                damage(topBarRect());
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
        _back.copyFrom(_wallpaper, area);
        for (int k = 0; k < _orderCount; k++) {
            int i = _order[k];
            if (_windows[i].shown())
                drawWindow(c, _windows[i], i == top(), i == _hoverControls);
        }
        if (_resizing >= 0) {
            c.fillRounded(_resizeRect, RADIUS, Theme::WHITE.withAlpha(14));
            c.strokeRounded(_resizeRect, RADIUS, 1.5f, Theme::WHITE.withAlpha(120));
        }
        drawTopBar(c);
        drawDock(c);
        if (_gridOpen)
            drawGrid(c);
        if (_menuOpen)
            drawMenu(c);
    }

    // A calm wallpaper: a deep gradient with soft glows of saffron and lotus,
    // and a faint lotus in the middle. Painted once.
    void paintWallpaper() {
        int w = _wallpaper.width, h = _wallpaper.height;
        struct Glow {
            float x, y, radius;
            Color color;
            float strength;
        };
        Glow glows[] = {
            {0.08f * w, 1.05f * h, 0.75f * w, Color::hex(0xFF8A3D), 0.42f},
            {0.95f * w, 0.05f * h, 0.65f * w, Color::hex(0xD6457C), 0.30f},
            {0.55f * w, 0.55f * h, 0.55f * w, Color::hex(0x4B3A8F), 0.30f},
        };
        Color top = Color::hex(0x14111F), bottom = Color::hex(0x24132B);
        for (int y = 0; y < h; y++) {
            unsigned* row = _wallpaper.row(y);
            Color base = top.mix(bottom, float(y) / float(h - 1));
            for (int x = 0; x < w; x++) {
                Color c = base;
                for (Glow const& g : glows) {
                    float dx = (x - g.x) / g.radius, dy = (y - g.y) / g.radius;
                    float d = dx * dx + dy * dy;
                    if (d < 1) {
                        float t = (1 - d) * (1 - d) * g.strength; // smooth falloff
                        c = c.mix(g.color, t);
                    }
                }
                row[x] = _wallpaper.pack(c);
            }
        }
        Canvas c(_wallpaper);
        float cx = w / 2.0f, cy = (h - DOCK_RESERVE) / 2.0f + 40;
        c.lotus(cx, cy, 90, 15);
    }

    void drawWindow(Canvas& c, Window const& w, bool focused, bool hoverControls) {
        if (w.area().intersect(c.clip).empty())
            return;
        c.shadow(w.frame, RADIUS, SHADOW, Color::hex(0x000000, focused ? 140 : 80));
        c.fillRounded(w.frame, RADIUS, Theme::SURFACE);

        // The app's pixels, with the bottom corners rounded off.
        c.blit(w.surface, {w.content().x, w.content().y}, w.frame, RADIUS);

        // Title bar: the window's own color, a hairline under it.
        Rect saved = c.clip;
        c.clip = c.clip.intersect(w.titleBar());
        c.fillRounded({w.frame.x, w.frame.y, w.frame.w, TITLE_H + 2 * RADIUS}, RADIUS, Theme::SURFACE);
        c.clip = saved;
        c.fill({w.frame.x, w.frame.y + TITLE_H - 1, w.frame.w, 1}, Color::hex(0x000000, 70));

        // Controls: colored when the window is in front, grey otherwise; their
        // symbols appear under the pointer.
        Font const& font = Font::system();
        for (int k = 0; k < 3; k++) {
            Rect r = w.control(k);
            bool enabled = k != 2 or w.resizable;
            Color color = focused or hoverControls ? CONTROL[k] : CONTROL_IDLE;
            if (not enabled)
                color = CONTROL_IDLE;
            float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
            c.fillCircle(cx, cy, 6.5f, color);
            if (hoverControls and enabled) {
                Color ink = Color::hex(0x000000, 140);
                if (k == 0) {
                    c.text({int(cx) - font.measure("×") / 2, int(cy) - font.height / 2}, "×", ink);
                } else if (k == 1) {
                    c.fill({int(cx) - 3, int(cy), 7, 1}, ink);
                } else {
                    c.fill({int(cx) - 3, int(cy), 7, 1}, ink);
                    c.fill({int(cx), int(cy) - 3, 1, 7}, ink);
                }
            }
        }

        // Title, centered (and cut short to fit between the controls and the edge).
        char title[48];
        copyString(title, sizeof(title), w.title);
        int room = w.frame.w - 2 * 84;
        unsigned long n = Sys::length(title);
        while (n > 3 and font.measure(title) > room) {
            n--;
            title[n - 3] = title[n - 2] = title[n - 1] = '.';
            title[n] = '\0';
        }
        int tw = font.measure(title);
        c.text({w.frame.x + (w.frame.w - tw) / 2, w.frame.y + (TITLE_H - font.height) / 2}, title,
               focused ? Theme::TEXT : Theme::TEXT_DIM);

        // A crisp edge: light outside, so windows stand out from each other.
        c.strokeRounded(w.frame, RADIUS, 1, Theme::WHITE.withAlpha(focused ? 26 : 16));
    }

    // Frosted glass: the wallpaper under it (always visible there),
    // darkened and softened.
    void frosted(Canvas& c, Rect r, float radius, unsigned char alpha) {
        if (radius > 0)
            c.fillRounded(r, radius, Color::hex(0x16131E, alpha));
        else
            c.fill(r, Color::hex(0x16131E, alpha));
    }

    void drawTopBar(Canvas& c) {
        Rect bar = topBarRect();
        if (bar.intersect(c.clip).empty())
            return;
        frosted(c, bar, 0, 150);
        c.fill({0, bar.bottom() - 1, bar.w, 1}, Theme::HAIRLINE);

        Font const& font = Font::system();
        Font const& bold = Font::bold();
        int textY = (TOPBAR_H - font.height) / 2;

        // The lotus menu, then the app in front.
        Rect lotus = lotusRect();
        if (_menuOpen or _hoverLotus)
            c.fillRounded(lotus, 6, Theme::WHITE.withAlpha(_menuOpen ? 34 : 20));
        c.lotus(lotus.x + 15.0f, lotus.y + 17.0f, 7.5f);
        c.text({lotus.x + 28, textY}, "Bhairav", Theme::TEXT, bold);
        int f = top();
        if (f >= 0) {
            Window const& w = _windows[f];
            int k = findBuiltIn(w.program), j = findInstalled(w.program);
            char const* name = k >= 0 ? APPS[k].name : j >= 0 ? _installed[j].info.title : w.title;
            c.text({lotus.right() + 12, textY}, name, Theme::TEXT, font);
        }

        // Right side: network, then date and time.
        Sys::SystemInfo info;
        Sys::systemInfo(info);
        Text<48> clock;
        formatDate(info.unixTime, clock);
        clock.add("   ");
        addTwoDigits(clock, info.timeOfDay / 3600);
        clock.add(":");
        addTwoDigits(clock, info.timeOfDay / 60 % 60);
        int x = bar.right() - 14 - font.measure(clock.cstr());
        c.text({x, textY}, clock.cstr(), Theme::TEXT, font);

        Text<40> net;
        if (_netUp)
            Net::format(_netAddress, net);
        else
            net.add(_netOk ? "Connecting..." : "Offline");
        x -= 22 + font.measure(net.cstr());
        c.text({x, textY}, net.cstr(), Theme::TEXT_DIM, font);
        // A small signal symbol: three bars.
        for (int b = 0; b < 3; b++) {
            int h = 4 + b * 3;
            c.fillRounded({x - 18 + b * 4, TOPBAR_H / 2 + 5 - h, 3, h}, 1,
                          _netUp ? Theme::TEXT : Theme::TEXT_DIM.withAlpha(90));
        }
    }

    // "Wed 11 Nov" (UTC)
    static void formatDate(unsigned long unixTime, Text<48>& out) {
        static constexpr char const* DAYS[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
        static constexpr char const* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        long days = static_cast<long>(unixTime / 86400);
        long z = days + 719468, era = z / 146097, doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        long day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
        out.add(DAYS[days % 7]).add(" ").add(day).add(" ").add(MONTHS[month - 1]);
    }

    void drawDock(Canvas& c) {
        Rect d = dockRect();
        if (dockArea().intersect(c.clip).empty())
            return;
        frosted(c, d, 20, 175); // (no shadow: it would show through the glass)
        c.strokeRounded(d, 20, 1, Theme::HAIRLINE);

        for (int k = 0; k < _dockCount; k++) {
            Slot const& slot = _dock[k];
            Rect icon = dockIcon(k);
            // A thin line before the installed apps and before the grid.
            if (k > 0 and slot.kind != _dock[k - 1].kind)
                c.fill({d.x + slot.x - DOCK_SEPARATOR / 2 - DOCK_GAP / 2, d.y + 14, 1, d.h - 28}, Theme::HAIRLINE);
            drawSlotIcon(c, slot, icon, false);
            if (slot.kind != Slot::Grid and isRunning(programOf(slot)))
                c.fillCircle(icon.x + DOCK_ICON / 2.0f, d.bottom() - 6.0f, 2.2f, Theme::TEXT.withAlpha(220));
        }

        // The name of the app under the pointer, floating above the dock.
        if (_hoverDock >= 0) {
            Font const& font = Font::system();
            char const* name = nameOf(_dock[_hoverDock]);
            Rect icon = dockIcon(_hoverDock);
            int w = font.measure(name) + 20, h = font.height + 10;
            Rect pill{icon.x + icon.w / 2 - w / 2, d.y - h - 8, w, h};
            c.fillRounded(pill, 8, Color::hex(0x1E1C25, 235));
            c.strokeRounded(pill, 8, 1, Theme::HAIRLINE);
            c.text({pill.x + 10, pill.y + 5}, name, Theme::TEXT);
        }
    }

    void drawSlotIcon(Canvas& c, Slot const& slot, Rect tile, bool large) {
        if (slot.kind == Slot::BuiltIn) {
            drawAppIcon(c, slot.index, tile);
            return;
        }
        if (slot.kind == Slot::Grid) { // nine dots on a glass tile
            c.fillRounded(tile, 12, Theme::WHITE.withAlpha(30));
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++)
                    c.fillCircle(tile.x + tile.w / 2.0f + (col - 1) * 10.0f, tile.y + tile.h / 2.0f + (row - 1) * 10.0f,
                                 3.0f, Theme::TEXT);
            return;
        }
        Installed const& app = _installed[slot.index];
        Image const& img = large ? app.gridIcon : app.dockIcon;
        if (img.pixels) {
            c.image({tile.x, tile.y}, img.pixels, img.width, img.height);
            return;
        }
        // No icon: its first letter on a tile.
        c.fillRoundedGradient(tile, tile.w * 12 / 56.0f, Color::hex(0x6A5ACD), Color::hex(0x8E5BC9));
        char first = app.info.title[0];
        char letter[2] = {first >= 'a' and first <= 'z' ? static_cast<char>(first - 32) : first, 0};
        Font const& font = Font::get("sans-bold", large ? 28 : 22);
        c.text({tile.x + (tile.w - font.measure(letter)) / 2, tile.y + (tile.h - font.height) / 2}, letter,
               Theme::WHITE, font);
    }

    // All apps, over a dimmed screen.
    void drawGrid(Canvas& c) {
        c.fill(_screen.bounds(), Color::hex(0x000000, 110));
        Rect g = gridRect();
        c.shadow(g, 18, 26, Color::hex(0x000000, 140));
        c.fillRounded(g, 18, Color::hex(0x1E1C25, 240));
        c.strokeRounded(g, 18, 1, Theme::HAIRLINE);
        c.text({g.x + 28, g.y + 22}, "All Apps", Theme::TEXT, Font::get("sans-bold", 18));
        Font const& font = Font::system();
        for (int k = 0; k < gridCount(); k++) {
            Rect cell = gridCell(k);
            if (k == _hoverGrid)
                c.fillRounded(cell.inset(4), 12, Theme::WHITE.withAlpha(18));
            Rect tile{cell.x + (cell.w - GRID_ICON) / 2, cell.y + 10, GRID_ICON, GRID_ICON};
            drawSlotIcon(c, gridSlot(k), tile, true);
            // The name, cut short with "..." if it doesn't fit.
            char name[40];
            copyString(name, sizeof(name), nameOf(gridSlot(k)));
            unsigned long n = Sys::length(name);
            while (n > 4 and font.measure(name) > cell.w - 12) {
                n--;
                name[n] = '\0';
                name[n - 1] = name[n - 2] = name[n - 3] = '.';
            }
            c.text({cell.x + (cell.w - font.measure(name)) / 2, tile.bottom() + 8}, name, Theme::TEXT);
        }
    }

    // The apps' icons: a rounded tile with a simple symbol.
    void drawAppIcon(Canvas& c, int k, Rect tile) {
        float x = float(tile.x), y = float(tile.y), s = float(tile.w);
        float u = s / 56.0f; // drawn for 56 px, scaled
        auto R = [&](float rx, float ry, float rw, float rh) {
            return Rect{int(x + rx * u), int(y + ry * u), int(rw * u + 0.5f), int(rh * u + 0.5f)};
        };
        float radius = 12 * u;
        Color ink = Theme::WHITE;
        switch (k) {
        case 0: { // Browser: a globe
            c.fillRoundedGradient(tile, radius, Color::hex(0x3A7BF0), Color::hex(0x5B4BD6));
            float cx = x + s / 2, cy = y + s / 2;
            c.fillCircle(cx, cy, 17 * u, ink);
            c.fillCircle(cx, cy, 15 * u, Color::hex(0x4A63E2));
            c.fill(R(13, 27, 30, 2), ink);
            c.fill(R(27, 13, 2, 30), ink);
            c.fill(R(16, 19, 24, 2), ink.withAlpha(170));
            c.fill(R(16, 34, 24, 2), ink.withAlpha(170));
            break;
        }
        case 1: // Files: a folder
            c.fillRoundedGradient(tile, radius, Color::hex(0x5FA8F5), Color::hex(0x3C7FD9));
            c.fillRounded(R(12, 15, 16, 8), 3 * u, Color::hex(0xD8EBFF));
            c.fillRounded(R(12, 19, 32, 23), 4 * u, Color::hex(0xD8EBFF));
            c.fillRounded(R(12, 24, 32, 18), 4 * u, Color::hex(0xFFFFFF));
            break;
        case 2: // Terminal: ">_"
            c.fillRoundedGradient(tile, radius, Color::hex(0x2E2B36), Color::hex(0x17151D));
            c.strokeRounded(tile, radius, 1, Theme::HAIRLINE);
            c.text({tile.x + int(12 * u), tile.y + int(10 * u)}, ">", Theme::SAFFRON, Font::get("sans-bold", 22));
            c.fill(R(28, 36, 14, 3), Theme::TEXT);
            break;
        case 3: // Notes: a page with lines
            c.fillRoundedGradient(tile, radius, Color::hex(0xFFD36B), Color::hex(0xF4A93A));
            c.fillRounded(R(14, 10, 28, 36), 4 * u, Color::hex(0xFFFDF6));
            for (int line = 0; line < 4; line++)
                c.fill(R(19, 18 + line * 7, line == 3 ? 10 : 18, 2), Color::hex(0xD9C9A8));
            break;
        case 4: { // Monitor: a little graph
            c.fillRoundedGradient(tile, radius, Color::hex(0x2BB58C), Color::hex(0x1C8C93));
            int heights[] = {10, 18, 13, 26, 20, 30};
            for (int b = 0; b < 6; b++)
                c.fillRounded(R(11 + b * 6, 44 - heights[b], 4, heights[b]), 2 * u, ink.withAlpha(230));
            break;
        }
        case 5: { // App Store: an arrow down into a tray
            c.fillRoundedGradient(tile, radius, Color::hex(0x9B5DE5), Color::hex(0x6C4AD6));
            c.fillRounded(R(25, 11, 6, 18), 3 * u, ink);
            for (int row = 0; row < 8; row++) // the arrow head
                c.fill(R(20 + row, 24 + row, 16 - 2 * row, 1), ink);
            c.fillRounded(R(13, 38, 30, 4), 2 * u, ink);
            c.fillRounded(R(13, 31, 4, 11), 2 * u, ink);
            c.fillRounded(R(39, 31, 4, 11), 2 * u, ink);
            break;
        }
        default: // About: the lotus
            c.fillRoundedGradient(tile, radius, Theme::SAFFRON, Theme::LOTUS);
            c.lotus(x + s / 2, y + s - 16 * u, 14 * u);
            break;
        }
    }

    void drawMenu(Canvas& c) {
        Rect m = menuRect();
        c.shadow(m, 10, 18, Color::hex(0x000000, 130));
        c.fillRounded(m, 10, Color::hex(0x24212C, 245));
        c.strokeRounded(m, 10, 1, Theme::HAIRLINE);
        Font const& font = Font::system();
        for (int k = 0; k < MENU_COUNT; k++) {
            Rect r = menuItemRect(k);
            if (k == MENU_SEPARATOR) {
                c.fill({r.x + 8, r.y + r.h / 2, r.w - 16, 1}, Theme::HAIRLINE);
                continue;
            }
            if (k == _hoverMenu)
                c.fillRounded(r, 6, Theme::SAFFRON.withAlpha(200));
            c.text({r.x + 12, r.y + (r.h - font.height) / 2}, MENU_LABELS[k],
                   k == _hoverMenu ? Theme::WHITE : Theme::TEXT);
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
    Bitmap _screen, _back, _wallpaper;
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

    Installed _installed[MAX_INSTALLED];
    int _installedCount = 0;
    Slot _dock[APP_COUNT + MAX_DOCK_INSTALLED + 1];
    int _dockCount = 0, _dockWidth = 0;

    bool _menuOpen = false, _gridOpen = false, _hoverLotus = false;
    int _hoverMenu = -1, _hoverDock = -1, _hoverGrid = -1, _hoverControls = -1;

    int _resizing = -1; // the window whose corner is being dragged
    Rect _resizeRect;   // its outline
    int _lastTitleClick = -1;
    unsigned long _lastClickTime = 0;

    Net _net;
    Files _files;
    bool _netOk = false, _netUp = false, _filesOk = false;
    unsigned _netAddress = 0;

    unsigned long _clockSecond = 0, _clockMinute = 0;
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
