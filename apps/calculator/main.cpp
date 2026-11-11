// Calculator: an example third-party app for Bhairav OS.
//
// It's built like any app (against Kapalaframe, see apps/README.md), but
// shipped as a package and installed from the App Store rather than being
// part of the system.
//
// Click the keys or type: digits, + - * / . % Enter (=), Backspace,
// Escape (clear).

#include <kapala/gfx.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 300, HEIGHT = 420;
constexpr int DISPLAY_H = 112, PAD = 12, GAP = 8;

struct Key {
    char const* label;
    char action; // the character it stands for
    int span;    // columns
    int kind;    // 0 digit, 1 function, 2 operator
};

constexpr Key KEYS[] = {
    {"C", 'c', 1, 1}, {"±", 'n', 1, 1}, {"%", '%', 1, 1}, {"÷", '/', 1, 2},
    {"7", '7', 1, 0}, {"8", '8', 1, 0},      {"9", '9', 1, 0}, {"×", '*', 1, 2},
    {"4", '4', 1, 0}, {"5", '5', 1, 0},      {"6", '6', 1, 0}, {"−", '-', 1, 2},
    {"1", '1', 1, 0}, {"2", '2', 1, 0},      {"3", '3', 1, 0}, {"+", '+', 1, 2},
    {"0", '0', 2, 0}, {".", '.', 1, 0},      {"=", '=', 1, 2},
};
constexpr int KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);

// Formats a number with up to 12 significant digits, trimming zeros.
void format(double v, char* out, int capacity) {
    Text<48> t;
    if (v != v or v > 1e15 or v < -1e15) {
        t.add("Error");
    } else {
        if (v < 0) {
            t.add("-");
            v = -v;
        }
        // Round to 10 decimal places, then split.
        auto whole = static_cast<unsigned long>(v);
        double fraction = v - static_cast<double>(whole);
        auto decimals = static_cast<unsigned long>(fraction * 1e10 + 0.5);
        if (decimals >= 10000000000ul) {
            whole++;
            decimals -= 10000000000ul;
        }
        t.add(whole);
        if (decimals) {
            char digits[11];
            for (int i = 9; i >= 0; i--) {
                digits[i] = static_cast<char>('0' + decimals % 10);
                decimals /= 10;
            }
            int n = 10;
            while (n > 0 and digits[n - 1] == '0')
                n--;
            digits[n] = '\0';
            // Keep at most 12 significant digits in all.
            int room = 12 - static_cast<int>(t.length);
            if (room < 1)
                room = 1;
            if (n > room)
                digits[room] = '\0';
            t.add(".").add(digits);
        }
    }
    int i = 0;
    for (; t.data[i] and i + 1 < capacity; i++)
        out[i] = t.data[i];
    out[i] = '\0';
}

class Calculator {
public:
    bool open() {
        if (not _window.open("Calculator", WIDTH, HEIGHT))
            return false;
        redraw();
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            if (not _window.waitEvent(e))
                continue;
            if (e.kind == WindowEvent::Close)
                return 0;
            if (e.kind == WindowEvent::MouseDown) {
                for (int k = 0; k < KEY_COUNT; k++) {
                    if (keyRect(k).contains(e.at)) {
                        _pressed = k;
                        press(KEYS[k].action);
                    }
                }
            } else if (e.kind == WindowEvent::MouseUp) {
                _pressed = -1;
                redraw();
            } else if (e.kind == WindowEvent::Key and e.pressed and e.character) {
                char c = e.character;
                if (c == '\n')
                    c = '=';
                else if (c == 0x1b)
                    c = 'c';
                else if (c == '\b')
                    c = 'b';
                press(c);
            }
        }
    }

private:
    // MARK: Arithmetic

    void press(char c) {
        if ((c >= '0' and c <= '9') or c == '.') {
            if (_fresh) {
                _entry[0] = '\0';
                _fresh = false;
            }
            unsigned long n = Sys::length(_entry);
            bool hasDot = false;
            for (unsigned long i = 0; i < n; i++)
                hasDot = hasDot or _entry[i] == '.';
            if ((c == '.' and hasDot) or n >= 15)
                return redraw();
            if (n == 1 and _entry[0] == '0' and c != '.')
                n = 0; // replace a lone zero
            if (n == 0 and c == '.')
                _entry[n++] = '0';
            _entry[n] = c;
            _entry[n + 1] = '\0';
        } else if (c == '+' or c == '-' or c == '*' or c == '/') {
            if (not _fresh or not _op)
                apply();
            _op = c;
            _fresh = true;
        } else if (c == '=') {
            apply();
            _op = 0;
            _fresh = true;
        } else if (c == 'c') {
            _accumulator = 0;
            _op = 0;
            setEntry(0);
            _fresh = true;
        } else if (c == 'n') {
            setEntry(-value());
            _fresh = false;
        } else if (c == '%') {
            setEntry(value() / 100);
        } else if (c == 'b') {
            unsigned long n = Sys::length(_entry);
            if (not _fresh and n > 1)
                _entry[n - 1] = '\0';
            else
                setEntry(0);
        }
        redraw();
    }

    double value() const {
        double v = 0, scale = 0;
        bool negative = false;
        for (char const* p = _entry; *p; p++) {
            if (*p == '-')
                negative = true;
            else if (*p == '.')
                scale = 1;
            else if (*p >= '0' and *p <= '9') {
                if (scale > 0) {
                    scale /= 10;
                    v += (*p - '0') * scale;
                } else {
                    v = v * 10 + (*p - '0');
                }
            }
        }
        return negative ? -v : v;
    }

    void setEntry(double v) { format(v, _entry, sizeof(_entry)); }

    // Combines the accumulator with the entry using the pending operator.
    void apply() {
        double v = value();
        switch (_op) {
        case '+': _accumulator += v; break;
        case '-': _accumulator -= v; break;
        case '*': _accumulator *= v; break;
        case '/': _accumulator = v == 0 ? __builtin_nan("") : _accumulator / v; break;
        default: _accumulator = v; break;
        }
        setEntry(_accumulator);
    }

    // MARK: Drawing

    Rect keyRect(int k) const {
        int col = 0, row = 0;
        for (int i = 0; i < k; i++) {
            col += KEYS[i].span;
            if (col >= 4) {
                col = 0;
                row++;
            }
        }
        int cell = (WIDTH - 2 * PAD - 3 * GAP) / 4;
        int rowH = (HEIGHT - DISPLAY_H - PAD - 4 * GAP) / 5;
        return {PAD + col * (cell + GAP), DISPLAY_H + row * (rowH + GAP),
                cell * KEYS[k].span + GAP * (KEYS[k].span - 1), rowH};
    }

    void redraw() {
        Canvas c(_window.surface());
        c.fill(_window.surface().bounds(), Color::hex(0x17151C));

        // The display: the pending operation small, the entry large.
        Font const& big = Font::get("sans-bold", 28);
        Font const& small = Font::system();
        if (_op) {
            char acc[32];
            format(_accumulator, acc, sizeof(acc));
            Text<48> pending;
            pending.add(acc).add(" ").add(_op == '*' ? "×" : _op == '/' ? "÷" : _op == '-' ? "−" : "+");
            c.text({WIDTH - PAD - 6 - small.measure(pending.cstr()), 24}, pending.cstr(), Theme::TEXT_DIM, small);
        }
        Font const& entryFont = big.measure(_entry) > WIDTH - 2 * PAD - 12 ? Font::get("sans-bold", 18) : big;
        c.text({WIDTH - PAD - 6 - entryFont.measure(_entry), DISPLAY_H - 22 - entryFont.height}, _entry, Theme::WHITE,
               entryFont);

        Font const& label = Font::get("sans-bold", 18);
        for (int k = 0; k < KEY_COUNT; k++) {
            Rect r = keyRect(k);
            Color fill = KEYS[k].kind == 2   ? Theme::SAFFRON
                         : KEYS[k].kind == 1 ? Color::hex(0x4A4656)
                                             : Color::hex(0x2E2B36);
            if (k == _pressed)
                fill = fill.mix(Theme::WHITE, 0.25f);
            if (KEYS[k].kind == 2 and _op == KEYS[k].action and _fresh)
                fill = Color::hex(0xFFE2C2);
            c.fillRounded(r, r.h / 2.0f, fill);
            Color ink = KEYS[k].kind == 2 and _op == KEYS[k].action and _fresh ? Theme::SAFFRON : Theme::WHITE;
            c.text({r.x + (r.w - label.measure(KEYS[k].label)) / 2, r.y + (r.h - label.height) / 2}, KEYS[k].label,
                   ink, label);
        }
        _window.present();
    }

    Window _window;
    char _entry[32] = "0";
    double _accumulator = 0;
    char _op = 0;
    bool _fresh = true; // the next digit starts a new number
    int _pressed = -1;
};

Calculator calculator;

} // namespace

int main() {
    if (not calculator.open())
        return 1;
    return calculator.run();
}
