#include "gfx/console.h"

namespace Rudracore::Gfx {

void Console::init(Surface& s, Font const& f, i64 cx, i64 cy, i64 cw, i64 ch, Color bg, Color fg) {
    surface = &s;
    font = &f;
    x = cx;
    y = cy;
    w = cw;
    h = ch;
    background = bg;
    foreground = fg;
    col = 0;
    row = 0;
    surface->fillRect(x, y, w, h, background);
}

void Console::newline() {
    col = 0;
    if (row + 1 < rows()) {
        row++;
        return;
    }

    // Scroll the text area up by one line and clear the last line.
    i64 lineH = font->height;
    surface->scrollUp(x, y, w, rows() * lineH, lineH);
    surface->fillRect(x, y + row * lineH, w, lineH, background);
}

void Console::write(char c) {
    if (not surface or not font or not font->valid() or cols() == 0 or rows() == 0)
        return;

    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        col = 0;
        return;
    }

    if (col >= cols())
        newline();

    font->drawChar(*surface, x + col * font->width, y + row * font->height, c, foreground);
    col++;
}

} // namespace Rudracore::Gfx
