#pragma once

#include "gfx/font.h"

namespace Rudracore::Gfx {

struct Rect {
    i64 x, y, w, h;
};

namespace Palette {
constexpr Color NIGHT{0x14, 0x0b, 0x2e};
constexpr Color MAROON{0x3b, 0x0d, 0x2e};
constexpr Color SAFFRON{0xff, 0x99, 0x33};
constexpr Color LOTUS_DEEP{0xc2, 0x18, 0x5b};
constexpr Color LOTUS{0xe7, 0x54, 0x80};
constexpr Color LOTUS_LIGHT{0xff, 0x8f, 0xab};
constexpr Color PETAL_TIP{0xff, 0xe0, 0xea};
constexpr Color PANEL{0x0d, 0x08, 0x1f};
constexpr Color TEXT{0xe8, 0xe0, 0xf0};
constexpr Color TEXT_DIM{0xa8, 0x9c, 0xc0};
} // namespace Palette

constexpr i64 LOGO_SIZE = 256;

// Height of the status bar at the bottom of the boot screen.
constexpr i64 STATUS_BAR_HEIGHT = 24;

// Draws the embedded Bhairav logo with its top-left corner at (x, y).
void drawLogo(Surface& s, i64 x, i64 y);

// Draws a lotus whose petals grow upward from (baseX, baseY).
void drawLotus(Surface& s, i64 baseX, i64 baseY, i64 size);

// Draws the BhairavOS boot screen and returns the area left for the log console.
Rect drawSplash(Surface& s, Font const& font, char const* subtitle);

} // namespace Rudracore::Gfx
