// Notes: a small text editor. Type, Backspace, Enter for new lines.
// Edits one text file: the one it was launched with (from Files or the
// Terminal), or /notes.txt. Opened at start, saved with Ctrl+S, and saved
// when the window closes.
//
//   handle 1: the shell
//   handle 2: the file system

#include <kapala/files.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 560;
constexpr int HEIGHT = 380;
constexpr int PADDING = 18;
constexpr int LINE_HEIGHT = 20;
constexpr int FOOTER_H = 26;
constexpr unsigned long CAPACITY = 16000;
constexpr char const* DEFAULT_PATH = "/notes.txt";
constexpr unsigned char SCANCODE_CTRL = 0x1D;
constexpr unsigned char SCANCODE_S = 0x1F;

class Notes {
public:
    bool open() {
        if (not _window.open("Notes", WIDTH, HEIGHT, 1, true))
            return false;
        char const* path = _window.argument()[0] ? _window.argument() : DEFAULT_PATH;
        _path.add(path);
        char const* name = path;
        for (char const* p = path; *p; p++) {
            if (*p == '/' and p[1])
                name = p + 1;
        }
        _name.add(name);
        load();
        updateTitle();
        redraw();
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            if (not _window.waitEvent(e, 500)) {
                _caretOn = not _caretOn; // blink
                redraw();
                continue;
            }
            switch (e.kind) {
            case WindowEvent::Close:
                if (_dirty)
                    save();
                return 0;
            case WindowEvent::Key:
                if (e.scancode == SCANCODE_CTRL)
                    _ctrl = e.pressed;
                else if (e.pressed and _ctrl and e.scancode == SCANCODE_S)
                    save();
                else if (e.pressed and e.character and not _ctrl)
                    type(e.character);
                break;
            case WindowEvent::Resize:
                redraw();
                break;
            case WindowEvent::Focus:
                _ctrl = false; // a release while unfocused never reaches us
                _caretOn = e.pressed;
                redraw();
                break;
            default: break;
            }
        }
    }

private:
    // Reads the file, if there is one.
    void load() {
        if (not _files.connect(2)) {
            _status = "No disk: notes won't be saved";
            return;
        }
        long n = _files.read(_path.cstr(), _text, CAPACITY - 1);
        if (n == Sys::NOT_FOUND) {
            setStatus("New file: Ctrl+S saves it");
            return;
        }
        if (n < 0) {
            setStatus("Couldn't open the file");
            return;
        }
        // Keep printable characters and newlines (drop the \r of Windows line ends).
        _length = 0;
        for (long i = 0; i < n; i++) {
            char ch = _text[i];
            if (ch == '\n' or ch == '\t' or (ch >= ' ' and ch <= '~'))
                _text[_length++] = ch == '\t' ? ' ' : ch;
        }
        _text[_length] = '\0';
        setStatus("Opened: Ctrl+S saves");
    }

    void save() {
        if (not _files.connected()) {
            _status = "No disk: can't save";
        } else if (_files.write(_path.cstr(), _text, _length) < 0) {
            _status = "Saving failed";
        } else {
            _dirty = false;
            setStatus("Saved");
        }
        updateTitle();
        redraw();
    }

    void type(char ch) {
        _dirty = true;
        if (ch == '\b') {
            if (_length)
                _text[--_length] = '\0';
        } else if (ch == '\t') {
            for (int i = 0; i < 4; i++)
                insert(' ');
        } else if (ch == '\n' or (ch >= ' ' and ch <= '~')) {
            insert(ch);
        }
        _caretOn = true;
        redraw();
        updateTitle();
    }

    void insert(char ch) {
        if (_length + 1 < CAPACITY) {
            _text[_length++] = ch;
            _text[_length] = '\0';
        }
    }

    void updateTitle() {
        Text<48> title;
        title.add(_dirty ? "* " : "").add(_name.cstr()).add(" - ").add(_length).add(_length == 1 ? " character" : " characters");
        _window.setTitle(title.cstr());
    }

    // Lays out the text character by character (wrapping at the edge), so
    // the caret lands exactly after the last character.
    void redraw() {
        Canvas c(_window.surface());
        c.fill(_window.surface().bounds(), Theme::SURFACE);

        Font const& font = Font::mono();
        int width = _window.width(), height = _window.height();
        int columns = (width - 2 * PADDING) / font.width;
        int x = 0, y = 0;

        // Keep the last line visible: scroll up by the lines that don't fit.
        int visible = (height - 2 * PADDING - FOOTER_H) / LINE_HEIGHT;
        int lines = currentLine() + 1;
        int scroll = lines > visible ? lines - visible : 0;

        if (_length == 0)
            c.text({PADDING, PADDING}, "Start typing...", Theme::TEXT_DIM);

        char glyph[2] = {0, 0};
        for (unsigned long i = 0; i < _length; i++) {
            if (_text[i] == '\n' or x == columns) {
                x = 0;
                y++;
                if (_text[i] == '\n')
                    continue;
            }
            glyph[0] = _text[i];
            if (y >= scroll)
                c.text({PADDING + x * font.width, PADDING + (y - scroll) * LINE_HEIGHT}, glyph, Theme::TEXT, font);
            x++;
        }

        if (_caretOn and _window.focused())
            c.fill({PADDING + x * font.width, PADDING + (y - scroll) * LINE_HEIGHT - 1, 2, 18}, Theme::SAFFRON);

        // Footer: whether the text is saved.
        Rect footer{0, height - FOOTER_H, width, FOOTER_H};
        c.fill(footer, Theme::SURFACE_ALT);
        c.text({PADDING, footer.y + (FOOTER_H - font.height) / 2}, _status, Theme::TEXT_DIM);
        _window.present();
    }

    int currentLine() const {
        int columns = (_window.width() - 2 * PADDING) / Font::mono().width;
        int x = 0, y = 0;
        for (unsigned long i = 0; i < _length; i++) {
            if (_text[i] == '\n' or x == columns) {
                x = 0;
                y++;
                if (_text[i] == '\n')
                    continue;
            }
            x++;
        }
        return y;
    }

    Window _window;
    // "Saved: /docs/a.txt"
    void setStatus(char const* what) {
        _statusText = {};
        _statusText.add(what).add("  -  ").add(_path.cstr());
        _status = _statusText.cstr();
    }

    Files _files;
    Text<96> _path, _name;
    Text<160> _statusText;
    char const* _status = "";
    bool _dirty = false;
    bool _ctrl = false;
    char _text[CAPACITY] = {};
    unsigned long _length = 0;
    bool _caretOn = true;
};

Notes notes;

} // namespace

int main() {
    if (not notes.open())
        return 1;
    return notes.run();
}
