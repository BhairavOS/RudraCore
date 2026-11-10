#include "display.h"

#include "gfx/console.h"
#include "gfx/splash.h"
#include "log.h"

namespace Rudracore::Display {

namespace {

constexpr Gfx::Color PANIC_BACKGROUND{0x3a, 0x0a, 0x14};
constexpr Gfx::Color PANIC_TITLE{0xff, 0x6b, 0x6b};
constexpr Gfx::Color PANIC_TEXT{0xff, 0xe8, 0xe8};

constexpr Gfx::Color STATUS_BACKGROUND{0x0a, 0x06, 0x18};

bool ready = false;
bool panicking = false;
bool userOwnsScreen = false;
Gfx::Surface screen;
Gfx::Font font;
Gfx::Console console;

void mirrorToConsole(char c) { console.write(c); }

// A small fixed-size text builder for the status bar.
struct Line {
    char text[128] = {};
    usize length = 0;

    void add(char const* s) {
        while (*s and length + 1 < sizeof(text))
            text[length++] = *s++;
    }

    void add(u64 value) {
        char digits[21];
        usize n = 0;
        do {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (n and length + 1 < sizeof(text))
            text[length++] = digits[--n];
    }
};

} // namespace

void init(Boot::Info const& info) {
    if (not info.hasFramebuffer)
        return;

    screen = Gfx::Surface::from(info.framebuffer);
    font = Gfx::Font::embedded();
    Gfx::Rect area = Gfx::drawSplash(screen, font, "RudraCore - Efficient microkernel for Bhairav OS");
    if (not font.valid())
        return;

    console.init(screen, font, area.x, area.y, area.w, area.h,
                 Gfx::Palette::PANEL, Gfx::Palette::TEXT);
    setLogMirror(mirrorToConsole);
    ready = true;
}

void drawStatus(u64 uptimeMs, u64 tasks, u64 freeBytes, u64 heapBytes) {
    Arch::InterruptGuard guard;
    if (not ready or panicking or userOwnsScreen)
        return;

    Line line;
    line.add("uptime ");
    line.add(uptimeMs / 1000);
    line.add(".");
    line.add(uptimeMs / 100 % 10);
    line.add(" s    tasks ");
    line.add(tasks);
    line.add("    free memory ");
    line.add(freeBytes / (1024 * 1024));
    line.add(" MiB    kernel heap ");
    line.add(heapBytes);
    line.add(" bytes");

    i64 y = screen.height - Gfx::STATUS_BAR_HEIGHT;
    screen.fillRect(0, y, screen.width, Gfx::STATUS_BAR_HEIGHT, STATUS_BACKGROUND);
    font.drawText(screen, screen.width / 10, y + (Gfx::STATUS_BAR_HEIGHT - font.height) / 2,
                  line.text, Gfx::Palette::TEXT_DIM);
}

void handOverToUserspace() {
    Arch::InterruptGuard guard;
    if (userOwnsScreen)
        return;
    userOwnsScreen = true;
    setLogMirror(nullptr);
    log("[core] the screen now belongs to userspace; kernel logs continue on serial");
}

void showPanicScreen(char const* title) {
    if (not ready)
        return;
    panicking = true;
    setLogMirror(mirrorToConsole); // the panic report goes on screen even if userspace had it

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
