// Notes: a small text editor. Type, Backspace, Enter for new lines.

#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 520;
constexpr int HEIGHT = 360;
constexpr int PADDING = 18;
constexpr int LINE_HEIGHT = 20;
constexpr unsigned long CAPACITY = 4000;

class Notes {
public:
    bool open() {
        if (not _window.open("Notes", WIDTH, HEIGHT))
            return false;
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
            case WindowEvent::Close: return 0;
            case WindowEvent::Key:
                if (e.pressed and e.character)
                    type(e.character);
                break;
            case WindowEvent::Focus:
                _caretOn = e.pressed;
                redraw();
                break;
            default: break;
            }
        }
    }

private:
    void type(char ch) {
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
        title.add("Notes - ").add(_length).add(_length == 1 ? " character" : " characters");
        _window.setTitle(title.cstr());
    }

    // Lays out the text character by character (wrapping at the edge), so
    // the caret lands exactly after the last character.
    void redraw() {
        Canvas c(_window.surface());
        c.fill(_window.surface().bounds(), Theme::SURFACE);

        Font const& font = Font::system();
        int columns = (WIDTH - 2 * PADDING) / font.width;
        int x = 0, y = 0;

        // Keep the last line visible: scroll up by the lines that don't fit.
        int visible = (HEIGHT - 2 * PADDING) / LINE_HEIGHT;
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
                c.text({PADDING + x * font.width, PADDING + (y - scroll) * LINE_HEIGHT}, glyph, Theme::TEXT);
            x++;
        }

        if (_caretOn and _window.focused())
            c.fill({PADDING + x * font.width, PADDING + (y - scroll) * LINE_HEIGHT - 1, 2, 18}, Theme::SAFFRON);
        _window.present();
    }

    int currentLine() const {
        int columns = (WIDTH - 2 * PADDING) / Font::system().width;
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
