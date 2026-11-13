// App Store: installs, opens and removes third-party apps.
//
//   handle 1: the shell
//   handle 2: the file system
//   handle 3: the network
//
// Shows every app apt can install (kapala/apt.h: the catalog that comes
// with Bhairav OS, plus the repositories added with `apt add-repo`) and the
// apps installed on the disk. Each installed app's Permissions panel shows
// what it may reach (network, the user's files) and changes it. Refresh reads the repositories again (like
// `apt update`). At the bottom, a .bpk package can be installed from a web
// address (http:// or https://) or from a file on the disk.

#include <kapala/apps.h>
#include <kapala/apt.h>
#include <kapala/files.h>
#include <kapala/http.h>
#include <kapala/image.h>
#include <kapala/net.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 720, HEIGHT = 540;
constexpr int HEADER_H = 64, ROW_H = 76, FOOTER_H = 64, STATUS_H = 26;
constexpr int MAX_ROWS = 48, ICON = 48;

unsigned long length(char const* s) {
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

void copy(char* dest, char const* src, unsigned long capacity) {
    unsigned long i = 0;
    for (; src[i] and i + 1 < capacity; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

bool same(char const* a, char const* b) {
    while (*a and *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// One app in the list: offered by a repository, installed, or both.
struct Row {
    AppInfo info;
    bool offered = false; // a repository has it
    Apt::Available package;
    bool installed = false;
    char installedVersion[16] = {};
    Image icon;
};

class Store {
public:
    bool open() {
        if (not _window.open("App Store", WIDTH, HEIGHT, 1, true))
            return false;
        _filesOk = _files.connect(2);
        _netOk = _net.connect(3);
        _catalog = static_cast<unsigned char const*>(Sys::openFile("catalog"));
        reload();
        setStatus("Pick an app, or install a .bpk from an address or a file below.");
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            if (not _window.waitEvent(e, 500)) {
                if (_editing) {
                    _caret = not _caret;
                    redraw();
                }
                continue;
            }
            switch (e.kind) {
            case WindowEvent::Close: return 0;
            case WindowEvent::Resize: redraw(); break;
            case WindowEvent::MouseDown: onClick(e.at); break;
            case WindowEvent::MouseMove: onHover(e.at); break;
            case WindowEvent::Wheel: scrollBy(e.wheel * 40); break;
            case WindowEvent::Key:
                if (e.pressed)
                    onKey(e);
                break;
            case WindowEvent::Focus: redraw(); break;
            default: break;
            }
        }
    }

private:
    // MARK: Apps

    void reload() {
        _review = -1;
        for (int i = 0; i < _count; i++)
            _rows[i] = Row{};
        _count = 0;

        // What the repositories offer (the newest version of each app).
        static Apt::Available offered[MAX_ROWS];
        long n = _filesOk ? Apt::available(_files, offered, MAX_ROWS) : 0;
        for (long k = 0; k < n; k++) {
            Row* row = nullptr;
            for (int i = 0; i < _count; i++) {
                if (same(_rows[i].info.id, offered[k].info.id))
                    row = &_rows[i];
            }
            if (row and Apt::compareVersions(offered[k].info.version, row->package.info.version) <= 0)
                continue;
            if (not row) {
                if (_count >= MAX_ROWS)
                    break;
                row = &_rows[_count++];
            }
            row->info = offered[k].info;
            row->package = offered[k];
            row->offered = true;
        }
        // Icons of apps in the built-in catalog come with it.
        forEachInCatalog(_catalog, [&](unsigned char const* data, unsigned long size) {
            Package p;
            char const* why;
            if (not p.parse(data, size, why))
                return;
            for (int i = 0; i < _count; i++) {
                if (same(_rows[i].info.id, p.info().id) and not _rows[i].icon.pixels) {
                    unsigned long iconSize = 0;
                    unsigned char const* png = p.file("icon.png", iconSize);
                    Image full;
                    if (png and Image::decode(png, iconSize, full, 1024 * 1024))
                        full.scaled(ICON, ICON, _rows[i].icon);
                }
            }
        });

        // What's installed (adding apps that aren't in the catalog).
        if (_filesOk) {
            static AppInfo installed[MAX_ROWS];
            long installedCount = listApps(_files, installed, MAX_ROWS);
            for (long j = 0; j < installedCount; j++) {
                Row* row = nullptr;
                for (int i = 0; i < _count; i++) {
                    if (same(_rows[i].info.id, installed[j].id))
                        row = &_rows[i];
                }
                if (not row) {
                    if (_count >= MAX_ROWS)
                        break;
                    row = &_rows[_count++];
                    row->info = installed[j];
                }
                if (not row->icon.pixels) { // the installed app's own icon
                    Text<96> path;
                    path.add("/apps/").add(installed[j].id).add("/icon.png");
                    unsigned long size = 0;
                    unsigned char* png = readWholeFile(_files, path.cstr(), size, 2 * 1024 * 1024);
                    Image full;
                    if (png and Image::decode(png, size, full, 1024 * 1024))
                        full.scaled(ICON, ICON, row->icon);
                    delete[] png;
                }
                row->installed = true;
                copy(row->installedVersion, installed[j].version, sizeof(row->installedVersion));
            }
        }
        redraw();
    }

    void install(Row& row) {
        if (not _filesOk)
            return setStatus("There's no disk to install to.");
        char const* why = "";
        AppInfo info;
        setStatus(same(row.package.source, Apt::BUILTIN) ? "Installing..." : "Downloading and installing...");
        long r = Apt::install(_files, _net, row.package, info, why);
        if (r < 0) {
            Text<200> t;
            t.add("Couldn't install ").add(row.info.title).add(": ").add(why);
            return setStatus(t.cstr());
        }
        _window.appsChanged();
        Text<200> t;
        t.add(info.title).add(" is installed. ");
        describeGrants(readPermissions(_files, info.id), t);
        setStatus(t.cstr());
        reload();
    }

    // MARK: Permissions

    // "It can use: the internet, your files." or "It can use only its own folder."
    template <unsigned long N>
    static void describeGrants(unsigned granted, Text<N>& t) {
        constexpr unsigned ANY = PERMISSION_NETWORK | PERMISSION_LOCAL_NETWORK | PERMISSION_FILES | PERMISSION_FOLDER |
                                 PERMISSION_HOSTS;
        if (not(granted & ANY)) {
            t.add("It can use only its own folder.");
            return;
        }
        t.add("It can use: ");
        bool first = true;
        auto item = [&](char const* what) {
            t.add(first ? "" : ", ").add(what);
            first = false;
        };
        if (granted & PERMISSION_NETWORK)
            item("the internet");
        else if (granted & PERMISSION_LOCAL_NETWORK)
            item("the local network");
        else if (granted & PERMISSION_HOSTS)
            item("its listed hosts");
        if (granted & PERMISSION_FILES)
            item("your files");
        else if (granted & PERMISSION_FOLDER)
            item("one of your folders");
        t.add(" (see Permissions).");
    }

    void openPermissions(int index) {
        _review = index;
        _grants = readGrants(_files, _rows[index].info.id);
        // Hosts it lists in its manifest can be granted later, too.
        if (not _grants.hosts[0])
            copy(_grants.hosts, _rows[index].info.hosts, sizeof(_grants.hosts));
        if (not _grants.folder[0])
            copy(_grants.folder, "/Documents", sizeof(_grants.folder));
        _editingFolder = false;
        redraw();
    }

    void savePermissions(unsigned granted) {
        Row const& row = _rows[_review];
        Grants next = _grants;
        next.permissions = granted;
        if (writeGrants(_files, row.info.id, next) < 0)
            return setStatus("Couldn't save the permissions.");
        _grants = next;
        Text<200> t;
        t.add("Saved for ").add(row.info.title).add(", from its next start. ");
        describeGrants(granted, t);
        setStatus(t.cstr());
    }

    // Off -> its listed hosts (if it has any) -> local network -> internet -> off.
    void cycleNetwork() {
        unsigned g = _grants.permissions;
        unsigned rest = g & ~(PERMISSION_NETWORK | PERMISSION_LOCAL_NETWORK | PERMISSION_HOSTS);
        if (g & PERMISSION_NETWORK)
            savePermissions(rest);
        else if (g & PERMISSION_LOCAL_NETWORK)
            savePermissions(rest | PERMISSION_NETWORK);
        else if ((g & PERMISSION_HOSTS) or not _grants.hosts[0])
            savePermissions(rest | PERMISSION_LOCAL_NETWORK);
        else
            savePermissions(rest | PERMISSION_HOSTS);
    }

    // Off -> one folder -> all your files -> off.
    void cycleFiles() {
        unsigned g = _grants.permissions;
        unsigned rest = g & ~(PERMISSION_FILES | PERMISSION_FOLDER);
        if (g & PERMISSION_FILES)
            savePermissions(rest);
        else if (g & PERMISSION_FOLDER)
            savePermissions(rest | PERMISSION_FILES);
        else
            savePermissions(rest | PERMISSION_FOLDER);
    }

    // The folder typed in the panel: saved when Enter is pressed.
    void saveFolder() {
        _editingFolder = false;
        if (_grants.folder[0] != '/')
            return setStatus("A folder starts with /, like /Documents.");
        savePermissions(_grants.permissions);
    }

    // Reads every repository's list again (apt update).
    void refresh() {
        struct Report : Apt::UpdateReport {
            int failed = 0;
            void source(char const*, long packages, char const*) override { failed += packages < 0; }
        } report;
        setStatus("Reading the repositories...");
        long total = Apt::update(_files, _net, report);
        Text<160> t;
        t.add(total).add(" apps in the repositories");
        if (report.failed)
            t.add(" (").add(report.failed).add(report.failed == 1 ? " repository didn't answer)" : " repositories didn't answer)");
        t.add(".");
        setStatus(t.cstr());
        reload();
    }

    void remove(Row& row) {
        Text<200> t;
        if (uninstallApp(_files, row.info.id) < 0) {
            t.add("Couldn't remove ").add(row.info.title).add(".");
        } else {
            t.add(row.info.title).add(" was removed.");
            _window.appsChanged();
        }
        setStatus(t.cstr());
        reload();
    }

    // From the field at the bottom: a web address or a path on the disk.
    void installFrom(char const* source) {
        while (*source == ' ')
            source++;
        if (not *source)
            return;
        unsigned char* data = nullptr;
        unsigned long size = 0;
        char const* why = "";
        bool web = source[0] == 'h' and source[1] == 't' and source[2] == 't' and source[3] == 'p';
        if (web) {
            if (not _netOk)
                return setStatus("There's no network.");
            setStatus("Downloading...");
            if (httpGet(_net, source, data, size, why) < 0) {
                Text<200> t;
                t.add("Download failed: ").add(why);
                return setStatus(t.cstr());
            }
        } else {
            data = readWholeFile(_files, source, size);
            if (not data)
                return setStatus("That file couldn't be read (give its full path, like /calculator.bpk).");
        }
        AppInfo info;
        long r = installPackage(_files, data, size, info, why);
        delete[] data;
        Text<200> t;
        if (r < 0) {
            t.add("Couldn't install it: ").add(why);
        } else {
            t.add(info.title).add(" ").add(info.version).add(" is installed. ");
            describeGrants(readPermissions(_files, info.id), t);
            _window.appsChanged();
            _input[0] = '\0';
            _editing = false;
        }
        setStatus(t.cstr());
        reload();
    }

    // MARK: Input

    Rect listArea() const {
        return {0, HEADER_H, _window.width(), _window.height() - HEADER_H - FOOTER_H - STATUS_H};
    }

    Rect rowRect(int i) const {
        Rect l = listArea();
        return {16, l.y + 8 + i * ROW_H - _scroll, l.w - 32, ROW_H - 8};
    }

    // The buttons on a row: 0 = the main one (Install / Open / Update),
    // 1 = Remove, 2 = Permissions (installed apps only).
    Rect rowButton(int i, int which) const {
        Rect r = rowRect(i);
        int w = which == 2 ? 112 : 92;
        int x = r.right() - 14 - 92 - (which >= 1 ? 92 + 8 : 0) - (which == 2 ? w + 8 : 0);
        return {x, r.y + (r.h - 30) / 2, w, 30};
    }

    // The Permissions panel, over the list.
    Rect panelRect() const {
        Rect l = listArea();
        int h = l.h - 32 < 350 ? l.h - 32 : 350;
        return {l.x + 40, l.y + 16, l.w - 80, h};
    }

    // Its rows: 0 = network, 1 = your files, 2 = which folder, 3 = hardware.
    int panelRowY(int which) const { return panelRect().y + 78 + which * 56; }

    Rect panelToggle(int which) const {
        Rect p = panelRect();
        int w = which == 2 ? 260 : 140; // the folder box is wider
        return {p.right() - 20 - w, panelRowY(which), w, 30};
    }

    Rect panelClose() const {
        Rect p = panelRect();
        return {p.right() - 20 - 94, p.bottom() - 46, 94, 32};
    }

    Rect refreshButton() const { return {_window.width() - 16 - 104, 16, 104, 32}; }

    Rect inputRect() const {
        int w = _window.width();
        return {16, _window.height() - FOOTER_H + 16, w - 16 - 120, 32};
    }

    Rect installButton() const {
        Rect in = inputRect();
        return {in.right() + 10, in.y, 94, 32};
    }

    bool updatable(Row const& row) const {
        return row.installed and row.offered and Apt::compareVersions(row.package.info.version, row.installedVersion) > 0;
    }

    void onClick(Point at) {
        if (_review >= 0) {
            if (_editingFolder and not panelToggle(2).contains(at))
                saveFolder();
            if (panelToggle(0).contains(at))
                cycleNetwork();
            else if (panelToggle(1).contains(at))
                cycleFiles();
            else if (panelToggle(2).contains(at))
                _editingFolder = true;
            else if (panelClose().contains(at) or not panelRect().contains(at))
                _review = -1;
            redraw();
            return;
        }
        if (inputRect().contains(at)) {
            _editing = true;
            _caret = true;
            redraw();
            return;
        }
        _editing = false;
        if (installButton().contains(at)) {
            installFrom(_input);
            return;
        }
        if (refreshButton().contains(at)) {
            refresh();
            return;
        }
        if (not listArea().contains(at)) {
            redraw();
            return;
        }
        for (int i = 0; i < _count; i++) {
            Row& row = _rows[i];
            if (rowButton(i, 0).contains(at)) {
                if (updatable(row) or (not row.installed and row.offered))
                    install(row);
                else
                    _window.launch(row.info.id);
                return;
            }
            if (row.installed and rowButton(i, 1).contains(at)) {
                remove(row);
                return;
            }
            if (row.installed and rowButton(i, 2).contains(at)) {
                openPermissions(i);
                return;
            }
        }
        redraw();
    }

    void onHover(Point at) {
        int hover = -1;
        for (int i = 0; i < _count and hover < 0 and _review < 0; i++) {
            if (rowButton(i, 0).contains(at))
                hover = i * 3;
            else if (_rows[i].installed and rowButton(i, 1).contains(at))
                hover = i * 3 + 1;
            else if (_rows[i].installed and rowButton(i, 2).contains(at))
                hover = i * 3 + 2;
        }
        for (int k = 0; k < 3 and _review >= 0; k++) {
            if (panelToggle(k).contains(at))
                hover = 2000 + k;
        }
        if (_review >= 0 and panelClose().contains(at))
            hover = 2010;
        if (installButton().contains(at))
            hover = 1000;
        if (refreshButton().contains(at))
            hover = 1001;
        if (hover != _hover) {
            _hover = hover;
            redraw();
        }
    }

    void onKey(WindowEvent const& e) {
        if (_editingFolder) {
            unsigned long n = length(_grants.folder);
            if (e.character == '\n')
                saveFolder();
            else if (e.character == 0x1b)
                openPermissions(_review); // back to what's saved
            else if (e.character == '\b' and n)
                _grants.folder[n - 1] = '\0';
            else if (e.character >= ' ' and e.character <= '~' and n + 1 < sizeof(_grants.folder)) {
                _grants.folder[n] = e.character;
                _grants.folder[n + 1] = '\0';
            }
            redraw();
            return;
        }
        if (not _editing) {
            if (e.scancode == Protocol::KEY_DOWN)
                scrollBy(40);
            else if (e.scancode == Protocol::KEY_UP)
                scrollBy(-40);
            return;
        }
        unsigned long n = length(_input);
        if (e.character == '\n') {
            char source[sizeof(_input)];
            copy(source, _input, sizeof(source));
            installFrom(source);
            return;
        }
        if (e.character == '\b') {
            if (n)
                _input[n - 1] = '\0';
        } else if (e.character == 0x1b) {
            _editing = false;
        } else if (e.character >= ' ' and e.character <= '~' and n + 1 < sizeof(_input)) {
            _input[n] = e.character;
            _input[n + 1] = '\0';
        }
        _caret = true;
        redraw();
    }

    void scrollBy(int d) {
        int max = _count * ROW_H + 16 - listArea().h;
        _scroll += d;
        if (_scroll > max)
            _scroll = max;
        if (_scroll < 0)
            _scroll = 0;
        redraw();
    }

    // MARK: Drawing

    void setStatus(char const* text) {
        copy(_status, text, sizeof(_status));
        redraw();
    }

    void button(Canvas& c, Rect r, char const* label, bool primary, bool hover) {
        Color fill = primary ? (hover ? Theme::LOTUS_LIGHT : Theme::SAFFRON) : (hover ? Theme::HOVER : Theme::SURFACE_ALT);
        c.fillRounded(r, r.h / 2.0f, fill);
        Font const& f = Font::bold();
        c.text({r.x + (r.w - f.measure(label)) / 2, r.y + (r.h - f.height) / 2}, label,
               primary ? Color::hex(0x2A1606) : Theme::TEXT, f);
    }

    void redraw() {
        Canvas c(_window.surface());
        int w = _window.width(), h = _window.height();
        c.fill(_window.surface().bounds(), Theme::SURFACE);
        Font const& font = Font::system();
        Font const& bold = Font::bold();

        // Header
        c.text({20, 14}, "App Store", Theme::TEXT, Font::get("sans-bold", 22));
        c.text({20, 42}, "The same apps as \"apt\" in the Terminal. Add repositories with apt add-repo.",
               Theme::TEXT_DIM);
        button(c, refreshButton(), "Refresh", false, _hover == 1001);
        c.fill({0, HEADER_H - 1, w, 1}, Color::hex(0x000000, 70));

        // The list
        Rect list = listArea();
        c.clip = list;
        if (_count == 0)
            c.text({20, list.y + 20}, "No apps yet. Build the examples with `make packages`.", Theme::TEXT_DIM);
        for (int i = 0; i < _count; i++) {
            Rect r = rowRect(i);
            if (r.bottom() < list.y or r.y > list.bottom())
                continue;
            Row const& row = _rows[i];
            c.fillRounded(r, 12, Theme::SURFACE_ALT);
            Rect icon{r.x + 12, r.y + (r.h - ICON) / 2, ICON, ICON};
            if (row.icon.pixels)
                c.image({icon.x, icon.y}, row.icon.pixels, ICON, ICON);
            else
                c.fillRounded(icon, 11, Color::hex(0x6A5ACD));
            int tx = icon.right() + 14;
            Text<120> title;
            title.add(row.info.title).add("  ");
            int end = c.text({tx, r.y + 10}, title.cstr(), Theme::TEXT, bold);
            Text<120> meta;
            meta.add(row.info.version[0] ? row.info.version : "?");
            if (row.info.author[0])
                meta.add(" · by ").add(row.info.author);
            if (row.installed)
                meta.add(" · installed");
            if (row.offered and not same(row.package.source, Apt::BUILTIN))
                meta.add(" · repo");
            c.text({end, r.y + 10}, meta.cstr(), Theme::TEXT_DIM);
            Rect desc{tx, r.y + 32, rowButton(i, row.installed ? 2 : 0).x - tx - 10, r.h - 34};
            Rect saved = c.clip;
            c.clip = c.clip.intersect(desc);
            c.paragraph(desc, row.info.description, Theme::TEXT_DIM, 17);
            c.clip = saved;

            char const* main = updatable(row) ? "Update" : row.installed ? "Open" : "Install";
            button(c, rowButton(i, 0), main, not row.installed or updatable(row), _hover == i * 3);
            if (row.installed) {
                button(c, rowButton(i, 1), "Remove", false, _hover == i * 3 + 1);
                button(c, rowButton(i, 2), "Permissions", false, _hover == i * 3 + 2);
            }
        }
        c.clip = _window.surface().bounds();
        if (_review >= 0)
            drawPermissions(c, list);

        // Footer: install from an address or a file.
        Rect footer{0, h - FOOTER_H, w, FOOTER_H};
        c.fill(footer, Theme::SURFACE);
        c.fill({0, footer.y, w, 1}, Color::hex(0x000000, 70));
        Rect in = inputRect();
        c.fillRounded(in, 9, Theme::SURFACE_ALT);
        if (_editing)
            c.strokeRounded(in, 9, 1.5f, Theme::SAFFRON.withAlpha(200));
        int ty = in.y + (in.h - font.height) / 2;
        if (_input[0] or _editing) {
            int end = c.text({in.x + 12, ty}, _input, Theme::TEXT);
            if (_editing and _caret)
                c.fill({end + 1, ty - 1, 2, font.height + 2}, Theme::SAFFRON);
        } else {
            c.text({in.x + 12, ty}, "Install a .bpk from https://... or /path/on/disk.bpk", Theme::TEXT_DIM);
        }
        button(c, installButton(), "Install", true, _hover == 1000);

        // The status line, above the footer.
        Rect bar{0, h - FOOTER_H - STATUS_H, w, STATUS_H};
        c.fill(bar, Theme::SURFACE);
        c.fill({0, bar.y, w, 1}, Color::hex(0x000000, 50));
        c.text({16, bar.y + (bar.h - font.height) / 2}, _status, Theme::TEXT_DIM);
        _window.present();
    }

    // One line of the Permissions panel: a name, what it means, and its state.
    void permissionRow(Canvas& c, int which, char const* name, char const* meaning, char const* state, bool changeable) {
        Rect p = panelRect();
        int y = panelRowY(which);
        c.text({p.x + 20, y}, name, Theme::TEXT, Font::bold());
        Rect text{p.x + 20, y + 20, panelToggle(which).x - p.x - 34, 34};
        Rect saved = c.clip;
        c.clip = c.clip.intersect(text);
        c.paragraph(text, meaning, Theme::TEXT_DIM, 16);
        c.clip = saved;
        if (changeable) {
            button(c, panelToggle(which), state, false, _hover == 2000 + which);
        } else {
            Font const& f = Font::system();
            Rect t = panelToggle(which);
            c.text({t.x + (t.w - f.measure(state)) / 2, t.y + (t.h - f.height) / 2}, state, Theme::TEXT_DIM, f);
        }
    }

    // The folder row: click the box to type another folder, Enter saves.
    void drawFolderRow(Canvas& c) {
        Rect p = panelRect();
        int y = panelRowY(2);
        bool used = _grants.permissions & PERMISSION_FOLDER and not(_grants.permissions & PERMISSION_FILES);
        c.text({p.x + 20, y}, "Which folder", used ? Theme::TEXT : Theme::TEXT_DIM, Font::bold());
        c.text({p.x + 20, y + 20}, _editingFolder ? "Type a folder, then press Enter." : "Click the box to change it.",
               Theme::TEXT_DIM);
        Rect box = panelToggle(2);
        c.fillRounded(box, 8, Theme::SURFACE_ALT);
        if (_editingFolder or _hover == 2002)
            c.strokeRounded(box, 8, 1.5f, Theme::SAFFRON.withAlpha(200));
        Font const& f = Font::system();
        int end = c.text({box.x + 10, box.y + (box.h - f.height) / 2}, _grants.folder, used ? Theme::TEXT : Theme::TEXT_DIM);
        if (_editingFolder)
            c.fill({end + 1, box.y + 7, 2, box.h - 14}, Theme::SAFFRON);
    }

    void drawPermissions(Canvas& c, Rect list) {
        Row const& row = _rows[_review];
        c.fill(list, Color::hex(0x000000, 110));
        Rect p = panelRect();
        c.fillRounded(p, 14, Theme::SURFACE);
        c.strokeRounded(p, 14, 1.0f, Theme::SAFFRON.withAlpha(140));
        Text<120> title;
        title.add("Permissions: ").add(row.info.title);
        c.text({p.x + 20, p.y + 16}, title.cstr(), Theme::TEXT, Font::get("sans-bold", 18));
        Text<160> asked;
        asked.add("Asked for: ");
        unsigned want = row.info.permissions;
        if (not want)
            asked.add("nothing. ");
        else
            asked.add(want & PERMISSION_NETWORK ? "internet " : "")
                .add(want & PERMISSION_LOCAL_NETWORK ? "local network " : "")
                .add(want & PERMISSION_HOSTS ? "its hosts " : "")
                .add(want & PERMISSION_FILES ? "your files " : "")
                .add(want & PERMISSION_FOLDER ? "a folder " : "")
                .add(". ");
        asked.add("Always: its own folder /appdata/").add(row.info.id);
        c.text({p.x + 20, p.y + 44}, asked.cstr(), Theme::TEXT_DIM);

        unsigned g = _grants.permissions;
        char const* network = g & PERMISSION_NETWORK         ? "Internet"
                              : g & PERMISSION_LOCAL_NETWORK ? "Local network"
                              : g & PERMISSION_HOSTS         ? "Its hosts"
                                                             : "Off";
        Text<140> netMeaning;
        if (_grants.hosts[0])
            netMeaning.add("Its hosts: ").add(_grants.hosts).add(". Or local network, or anywhere.");
        else
            netMeaning.add("Local network: computers at home or work. Internet: anywhere.");
        permissionRow(c, 0, "Network", netMeaning.cstr(), network, true);
        permissionRow(c, 1, "Your files", "One folder (seen by it as /shared), or all of them.",
                      g & PERMISSION_FILES ? "All" : g & PERMISSION_FOLDER ? "One folder" : "Off", true);
        drawFolderRow(c);
        permissionRow(c, 3, "Hardware", "Apps never use devices directly: only Bhairav's own drivers can.", "Never", false);

        c.text({p.x + 20, p.bottom() - 38}, "Changes apply the next time the app starts.", Theme::TEXT_DIM);
        button(c, panelClose(), "Done", true, _hover == 2010);
    }

    Window _window;
    Files _files;
    Net _net;
    bool _filesOk = false, _netOk = false;
    unsigned char const* _catalog = nullptr;

    Row _rows[MAX_ROWS];
    int _count = 0, _scroll = 0, _hover = -1;
    int _review = -1;           // the row whose Permissions panel is open
    Grants _grants;             // its permissions
    bool _editingFolder = false; // typing in the panel's folder box
    bool _editing = false, _caret = false;
    char _input[200] = {};
    char _status[200] = {};
};

} // namespace

int main() {
    auto* store = new Store();
    if (not store->open())
        return 1;
    return store->run();
}
