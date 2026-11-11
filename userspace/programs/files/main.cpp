// Files: the file manager.
//
//   handle 1: the shell
//   handle 2: the file system
//
// Shows a folder of the disk: folders first, then files, with sizes and
// dates. Double-click (or Enter) opens a folder, or a file in Notes. The
// toolbar goes up a level and makes new folders and files; Delete removes
// the selected item (press it twice).

#include <kapala/files.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 660, HEIGHT = 460;
constexpr int TOOLBAR_H = 48, HEADER_H = 26, ROW_H = 30, STATUS_H = 26;
constexpr int MAX_ENTRIES = 512;
constexpr unsigned long DOUBLE_CLICK_MS = 450;

constexpr Color PANE = Color::hex(0x18161E);
constexpr Color ROW_ALT = Color::hex(0x1C1A23);

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

char lower(char c) { return c >= 'A' and c <= 'Z' ? static_cast<char>(c + 32) : c; }

int compareNames(char const* a, char const* b) {
    for (; *a and *b; a++, b++) {
        if (lower(*a) != lower(*b))
            return lower(*a) < lower(*b) ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

void formatSize(unsigned long bytes, Text<32>& out) {
    if (bytes < 1024)
        out.add(bytes).add(" B");
    else if (bytes < 1024 * 1024)
        out.add(bytes / 1024).add(".").add(bytes * 10 / 1024 % 10).add(" KB");
    else
        out.add(bytes / (1024 * 1024)).add(".").add(bytes * 10 / (1024 * 1024) % 10).add(" MB");
}

void two(Text<32>& t, unsigned long v) {
    if (v < 10)
        t.add("0");
    t.add(v);
}

// "2026-11-11 15:20"
void formatDate(unsigned long unixTime, Text<32>& out) {
    if (not unixTime) {
        out.add("-");
        return;
    }
    long days = static_cast<long>(unixTime / 86400), rest = static_cast<long>(unixTime % 86400);
    long z = days + 719468, era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    long year = yoe + era * 400 + (month <= 2);
    out.add(year).add("-");
    two(out, static_cast<unsigned long>(month));
    out.add("-");
    two(out, static_cast<unsigned long>(day));
    out.add(" ");
    two(out, static_cast<unsigned long>(rest / 3600));
    out.add(":");
    two(out, static_cast<unsigned long>(rest / 60 % 60));
}

class FilesApp {
public:
    bool open() {
        if (not _window.open("Files", WIDTH, HEIGHT, 1, true))
            return false;
        _filesOk = _files.connect(2);
        copy(_path, _window.argument()[0] ? _window.argument() : "/", sizeof(_path));
        reload();
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            if (not _window.waitEvent(e, 2000)) {
                continue;
            }
            switch (e.kind) {
            case WindowEvent::Close: return 0;
            case WindowEvent::Resize: redraw(); break;
            case WindowEvent::MouseDown: onClick(e.at); break;
            case WindowEvent::Wheel: scrollBy(e.wheel * 3); break;
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
    // MARK: Folder contents

    void reload(char const* select = nullptr) {
        _count = 0;
        _selected = -1;
        _scroll = 0;
        _confirmDelete = false;
        if (not _filesOk) {
            setStatus("No disk: the file system isn't available");
            return;
        }
        for (;;) {
            long n = _files.list(_path, _entries + _count, static_cast<unsigned long>(MAX_ENTRIES - _count),
                                 static_cast<unsigned long>(_count));
            if (n < 0) {
                Text<120> t;
                t.add("Can't open ").add(_path).add(" (error ").add(n).add(")");
                setStatus(t.cstr());
                break;
            }
            _count += static_cast<int>(n);
            if (n == 0 or _count >= MAX_ENTRIES)
                break;
        }
        sort();
        if (select) {
            for (int i = 0; i < _count; i++) {
                if (compareNames(_entries[i].name, select) == 0)
                    _selected = i;
            }
        }
        Text<64> title;
        title.add("Files - ").add(_path);
        _window.setTitle(title.cstr());
        summary();
        redraw();
    }

    // Folders first, then by name.
    void sort() {
        for (int i = 1; i < _count; i++) {
            FileEntry e = _entries[i];
            int j = i;
            while (j > 0 and before(e, _entries[j - 1])) {
                _entries[j] = _entries[j - 1];
                j--;
            }
            _entries[j] = e;
        }
    }

    static bool before(FileEntry const& a, FileEntry const& b) {
        if (a.directory != b.directory)
            return a.directory;
        return compareNames(a.name, b.name) < 0;
    }

    void summary() {
        int folders = 0;
        unsigned long bytes = 0;
        for (int i = 0; i < _count; i++) {
            if (_entries[i].directory)
                folders++;
            else
                bytes += _entries[i].size;
        }
        Text<32> size;
        formatSize(bytes, size);
        Text<120> t;
        t.add(folders).add(folders == 1 ? " folder, " : " folders, ").add(_count - folders)
            .add(_count - folders == 1 ? " file (" : " files (").add(size.cstr()).add(")");
        setStatus(t.cstr());
    }

    void childPath(char const* name, char* out, unsigned long capacity) {
        copy(out, _path, capacity);
        unsigned long n = length(out);
        if ((n == 0 or out[n - 1] != '/') and n + 1 < capacity) {
            out[n] = '/';
            out[++n] = '\0';
        }
        copy(out + n, name, capacity - n);
    }

    void openEntry(int i) {
        if (i < 0 or i >= _count)
            return;
        char path[96];
        childPath(_entries[i].name, path, sizeof(path));
        if (_entries[i].directory) {
            copy(_path, path, sizeof(_path));
            reload();
        } else {
            _window.launch("notes", path);
            Text<120> t;
            t.add("Opening ").add(_entries[i].name).add(" in Notes");
            setStatus(t.cstr());
        }
    }

    void goUp() {
        unsigned long n = length(_path);
        if (n <= 1)
            return;
        char name[48];
        unsigned long cut = 0;
        for (unsigned long i = 0; i < n; i++) {
            if (_path[i] == '/')
                cut = i;
        }
        copy(name, _path + cut + 1, sizeof(name));
        _path[cut == 0 ? 1 : cut] = '\0';
        reload(name);
    }

    // A name not used yet in this folder: "New folder", "New folder 2"...
    void freshName(char const* base, char const* extension, char* out, unsigned long capacity) {
        for (int k = 1; k < 100; k++) {
            Text<48> name;
            name.add(base);
            if (k > 1)
                name.add(" ").add(k);
            name.add(extension);
            bool taken = false;
            for (int i = 0; i < _count and not taken; i++)
                taken = compareNames(_entries[i].name, name.cstr()) == 0;
            if (not taken) {
                copy(out, name.cstr(), capacity);
                return;
            }
        }
        copy(out, base, capacity);
    }

    void newFolder() {
        char name[48], path[96];
        freshName("New folder", "", name, sizeof(name));
        childPath(name, path, sizeof(path));
        long r = _files.makeDir(path);
        reload(name);
        if (r < 0)
            setStatus("Couldn't make the folder");
    }

    void newFile() {
        char name[48], path[96];
        freshName("untitled", ".txt", name, sizeof(name));
        childPath(name, path, sizeof(path));
        long r = _files.write(path, "", 0);
        reload(name);
        if (r < 0)
            setStatus("Couldn't make the file");
        else
            _window.launch("notes", path);
    }

    void deleteSelected() {
        if (_selected < 0)
            return;
        FileEntry const& e = _entries[_selected];
        if (not _confirmDelete) {
            _confirmDelete = true;
            Text<120> t;
            t.add("Delete ").add(e.name).add("? Press Delete again to confirm.");
            setStatus(t.cstr());
            return;
        }
        char path[96];
        childPath(e.name, path, sizeof(path));
        long r = _files.remove(path);
        if (r == Sys::NOT_EMPTY) {
            _confirmDelete = false;
            setStatus("That folder isn't empty: delete what's inside first");
            return;
        }
        reload();
        if (r < 0)
            setStatus("Couldn't delete it");
    }

    // MARK: Input

    Rect upButton() const { return {10, 8, 34, 32}; }
    Rect newFolderButton() const { return {_window.width() - 296, 8, 120, 32}; }
    Rect newFileButton() const { return {_window.width() - 168, 8, 98, 32}; }
    Rect deleteButton() const { return {_window.width() - 62, 8, 52, 32}; }
    Rect listArea() const {
        return {0, TOOLBAR_H + HEADER_H, _window.width(), _window.height() - TOOLBAR_H - HEADER_H - STATUS_H};
    }
    int visibleRows() const { return listArea().h / ROW_H; }

    void onClick(Point at) {
        if (upButton().contains(at))
            return goUp();
        if (newFolderButton().contains(at))
            return newFolder();
        if (newFileButton().contains(at))
            return newFile();
        if (deleteButton().contains(at))
            return deleteSelected();
        Rect list = listArea();
        if (not list.contains(at))
            return;
        int row = (at.y - list.y) / ROW_H + _scroll;
        unsigned long now = Sys::timeMs();
        if (row >= _count) {
            select(-1);
            return;
        }
        bool twice = row == _selected and now - _lastClick < DOUBLE_CLICK_MS;
        _lastClick = now;
        select(row);
        if (twice)
            openEntry(row);
    }

    void onKey(WindowEvent const& e) {
        switch (e.scancode) {
        case Protocol::KEY_DOWN: select(_selected + 1 < _count ? _selected + 1 : _selected); return;
        case Protocol::KEY_UP: select(_selected > 0 ? _selected - 1 : 0); return;
        case Protocol::KEY_HOME: select(0); return;
        case Protocol::KEY_END: select(_count - 1); return;
        case Protocol::KEY_DELETE: deleteSelected(); return;
        default: break;
        }
        if (e.character == '\n')
            openEntry(_selected);
        else if (e.character == '\b')
            goUp();
    }

    void select(int i) {
        if (i >= _count)
            i = _count - 1;
        if (i != _selected) {
            _confirmDelete = false;
            if (_status[0] == 'D' and _status[1] == 'e')
                summary();
        }
        _selected = i;
        if (i >= 0) { // keep it visible
            if (i < _scroll)
                _scroll = i;
            if (i >= _scroll + visibleRows())
                _scroll = i - visibleRows() + 1;
        }
        redraw();
    }

    void scrollBy(int rows) {
        int max = _count - visibleRows();
        _scroll += rows;
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

    void button(Canvas& c, Rect r, char const* label, bool enabled = true) {
        c.fillRounded(r, 8, Theme::SURFACE_ALT);
        Font const& f = Font::system();
        c.text({r.x + (r.w - f.measure(label)) / 2, r.y + (r.h - f.height) / 2}, label,
               enabled ? Theme::TEXT : Theme::BORDER);
    }

    void icon(Canvas& c, int x, int y, bool folder) {
        if (folder) {
            c.fillRounded({x, y + 2, 9, 5}, 2, Color::hex(0x4A90E8));
            c.fillRounded({x, y + 5, 18, 13}, 3, Color::hex(0x4A90E8));
            c.fillRounded({x, y + 8, 18, 10}, 3, Color::hex(0x7DB4F5));
        } else {
            c.fillRounded({x + 2, y, 14, 18}, 2, Color::hex(0xE9E7EF));
            for (int l = 0; l < 3; l++)
                c.fill({x + 5, y + 5 + l * 4, 8, 1}, Color::hex(0x9C99A8));
        }
    }

    void redraw() {
        Canvas c(_window.surface());
        Font const& f = Font::system();
        int w = _window.width(), h = _window.height();
        c.fill(_window.surface().bounds(), PANE);

        // Toolbar
        c.fill({0, 0, w, TOOLBAR_H}, Theme::SURFACE);
        button(c, upButton(), "^", length(_path) > 1);
        Rect pathBox{52, 8, newFolderButton().x - 62, 32};
        c.fillRounded(pathBox, 8, Theme::SURFACE_ALT);
        c.text({pathBox.x + 12, pathBox.y + 8}, _path, Theme::TEXT);
        button(c, newFolderButton(), "+ New folder");
        button(c, newFileButton(), "+ New file");
        button(c, deleteButton(), "Del", _selected >= 0);

        // Column headings
        int sizeX = w - 260, dateX = w - 160;
        Rect header{0, TOOLBAR_H, w, HEADER_H};
        c.fill(header, Theme::SURFACE_ALT);
        c.text({46, header.y + 5}, "Name", Theme::TEXT_DIM);
        c.text({sizeX, header.y + 5}, "Size", Theme::TEXT_DIM);
        c.text({dateX, header.y + 5}, "Modified", Theme::TEXT_DIM);

        // Rows
        Rect list = listArea();
        c.clip = list;
        for (int r = 0; r <= visibleRows(); r++) {
            int i = r + _scroll;
            int y = list.y + r * ROW_H;
            if (i >= _count)
                break;
            FileEntry const& e = _entries[i];
            Rect row{0, y, w, ROW_H};
            if (i == _selected)
                c.fillRounded({6, y + 1, w - 12, ROW_H - 2}, 6, _window.focused() ? Theme::SAFFRON.withAlpha(85) : Theme::HOVER);
            else if (i % 2)
                c.fill(row, ROW_ALT);
            icon(c, 16, y + 6, e.directory);
            char name[40];
            copy(name, e.name, sizeof(name));
            int maxChars = (sizeX - 60) / f.width;
            if (maxChars > 3 and static_cast<int>(length(name)) > maxChars) {
                name[maxChars - 3] = name[maxChars - 2] = name[maxChars - 1] = '.';
                name[maxChars] = '\0';
            }
            c.text({46, y + 7}, name, Theme::TEXT);
            Text<32> size, date;
            if (e.directory)
                size.add("folder");
            else
                formatSize(e.size, size);
            formatDate(e.modified, date);
            c.text({sizeX, y + 7}, size.cstr(), Theme::TEXT_DIM);
            c.text({dateX, y + 7}, date.cstr(), Theme::TEXT_DIM);
        }
        if (_count == 0 and _filesOk)
            c.text({46, list.y + 14}, "This folder is empty.", Theme::TEXT_DIM);
        c.clip = _window.surface().bounds();

        // Status bar
        Rect status{0, h - STATUS_H, w, STATUS_H};
        c.fill(status, Theme::SURFACE);
        c.fill({0, status.y, w, 1}, Theme::BORDER);
        c.text({12, status.y + 5}, _status, _confirmDelete ? Theme::GOLD : Theme::TEXT_DIM);
        _window.present();
    }

    Window _window;
    Files _files;
    bool _filesOk = false;
    char _path[96] = "/";
    FileEntry _entries[MAX_ENTRIES];
    int _count = 0, _selected = -1, _scroll = 0;
    unsigned long _lastClick = 0;
    bool _confirmDelete = false;
    char _status[160] = {};
};

} // namespace

int main() {
    auto* app = new FilesApp();
    if (not app->open())
        return 1;
    return app->run();
}
