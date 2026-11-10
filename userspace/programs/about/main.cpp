// About Bhairav OS: what the system is, and its parts.

#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

extern "C" unsigned char const _aboutLogo[]; // 256x256 RGBA

namespace {

constexpr int WIDTH = 720;
constexpr int HEIGHT = 480;
constexpr int COLUMN = 310; // left edge of the text column

constexpr char const* DESCRIPTION =
    "Bhairav OS is a completely new, independent operating system not based on "
    "Linux, Unix, or Windows. It is designed from the ground up with a unique "
    "kernel and architecture to provide a fresh approach to modern computing. "
    "It focuses on creating a stable, efficient, and secure environment for "
    "both developers and end users.";

struct Component {
    char const* name;
    char const* role;
};

constexpr Component COMPONENTS[] = {
    {"ShoolLoader", "Trident of System Awakening"},
    {"RudraCore", "Efficient microkernel for Bhairav OS"},
    {"Kapalaframe", "Framework for efficient application development"},
    {"KamalShell", "Beautiful and Responsive Graphical Shell"},
};

class About {
public:
    bool open() {
        if (not _window.open("About Bhairav OS", WIDTH, HEIGHT))
            return false;
        _button = {COLUMN, 250, 180, 40};
        draw(_window.surface().bounds());
        _window.present();
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            _window.waitEvent(e);
            switch (e.kind) {
            case WindowEvent::Close: return 0;
            case WindowEvent::MouseMove: setHover(_button.contains(e.at)); break;
            case WindowEvent::MouseDown:
                if (_button.contains(e.at))
                    setPressed(true);
                break;
            case WindowEvent::MouseUp:
                if (_pressed and _button.contains(e.at)) {
                    _clicks++;
                    Text<> message;
                    message.add("namaste! (").add(_clicks).add(")");
                    Sys::log(message.cstr());
                }
                setPressed(false);
                break;
            default: break;
            }
        }
    }

private:
    void draw(Rect area) {
        Canvas c(_window.surface());
        c.clip = area;
        c.fill(_window.surface().bounds(), Theme::SURFACE);

        c.image({24, 12}, _aboutLogo, 256, 256);

        c.text({COLUMN, 22}, "Bhairav OS", Theme::SAFFRON, 2);
        c.paragraph({COLUMN, 64, WIDTH - COLUMN - 24, 180}, DESCRIPTION, Theme::TEXT);

        // The four parts, in a 2 x 2 grid under the logo.
        c.fill({24, 306, WIDTH - 48, 1}, Theme::BORDER);
        for (int i = 0; i < 4; i++) {
            int x = 24 + (i % 2) * 346, y = 322 + (i / 2) * 76;
            c.text({x, y}, COMPONENTS[i].name, Theme::SAFFRON);
            c.paragraph({x, y + 20, 330, 40}, COMPONENTS[i].role, Theme::TEXT_DIM, 18);
        }

        Color color = _pressed ? Theme::LOTUS : (_hover ? Theme::GOLD : Theme::SAFFRON);
        c.fillRounded(_button, 20, color);
        char const* label = "Say namaste";
        c.text({_button.x + (_button.w - Font::system().measure(label)) / 2, _button.y + 12}, label, Theme::NIGHT);

        Text<64> clicks;
        if (_clicks)
            clicks.add("namaste x ").add(_clicks);
        c.text({_button.right() + 16, _button.y + 12}, clicks.cstr(), Theme::LOTUS_LIGHT);
    }

    void redraw(Rect area) {
        draw(area);
        _window.present(area);
    }

    Rect buttonArea() const { return {_button.x, _button.y, WIDTH - _button.x, _button.h}; }

    void setHover(bool hover) {
        if (hover != _hover) {
            _hover = hover;
            redraw(buttonArea());
        }
    }

    void setPressed(bool pressed) {
        _pressed = pressed;
        redraw(buttonArea());
    }

    Window _window;
    Rect _button;
    bool _hover = false, _pressed = false;
    unsigned long _clicks = 0;
};

} // namespace

int main() {
    About about;
    if (not about.open())
        return 1;
    return about.run();
}
