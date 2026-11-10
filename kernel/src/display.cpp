#include "display.h"

#include "gfx/console.h"
#include "gfx/splash.h"
#include "log.h"

namespace Rudracore::Display {

namespace {

constexpr Gfx::Color PANIC_BACKGROUND{0x3a, 0x0a, 0x14};
constexpr Gfx::Color PANIC_TITLE{0xff, 0x6b, 0x6b};
constexpr Gfx::Color PANIC_TEXT{0xff, 0xe8, 0xe8};

bool ready = false;
Gfx::Surface screen;
Gfx::Font font;
Gfx::Console console;

void mirrorToConsole(char c) { console.write(c); }

} // namespace

void init(Boot::Info const& info) {
    if (not info.hasFramebuffer)
        return;

    screen = Gfx::Surface::from(info.framebuffer);
    font = Gfx::Font::embedded();
    Gfx::Rect area = Gfx::drawSplash(screen, font, "RudraCore microkernel");
    if (not font.valid())
        return;

    console.init(screen, font, area.x, area.y, area.w, area.h,
                 Gfx::Palette::PANEL, Gfx::Palette::TEXT);
    setLogMirror(mirrorToConsole);
    ready = true;
}

void showPanicScreen(char const* title) {
    if (not ready)
        return;

    screen.fillRect(0, 0, screen.width, screen.height, PANIC_BACKGROUND);

    i64 margin = screen.width / 16;
    i64 y = screen.height / 16;
    font.drawText(screen, margin, y, "RudraCore panic", PANIC_TITLE, 3);
    y += font.height * 3 + 6;
    font.drawText(screen, margin, y, title, PANIC_TEXT, 1);
    y += font.height + 12;
    screen.fillRect(margin, y, screen.width - 2 * margin, 2, PANIC_TITLE);
    y += 16;

    console.init(screen, font, margin, y, screen.width - 2 * margin,
                 screen.height - y - margin / 2, PANIC_BACKGROUND, PANIC_TEXT);
}

} // namespace Rudracore::Display
