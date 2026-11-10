#pragma once

#include "gfx/font.h"

namespace Rudracore::Gfx {

// A scrolling text console inside a rectangle of the framebuffer.
struct Console {
    Surface* surface = nullptr;
    Font const* font = nullptr;
    i64 x = 0, y = 0, w = 0, h = 0;
    Color background{};
    Color foreground{};

    i64 col = 0;
    i64 row = 0;

    void init(Surface& s, Font const& f, i64 x, i64 y, i64 w, i64 h, Color bg, Color fg);
    void write(char c);

private:
    i64 cols() const { return w / font->width; }
    i64 rows() const { return h / font->height; }
    void newline();
};

} // namespace Rudracore::Gfx
