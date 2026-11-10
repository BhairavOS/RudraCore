#pragma once

#include "gfx/surface.h"

namespace Rudracore::Gfx {

// A bitmap console font in PC Screen Font format (PSF1 or PSF2), embedded
// into the kernel image at build time (see font.S).
struct Font {
    u8 const* glyphs = nullptr;
    u32 glyphCount = 0;
    u32 bytesPerGlyph = 0;
    u32 width = 0;
    u32 height = 0;

    static Font embedded();

    bool valid() const { return glyphs != nullptr; }

    void drawChar(Surface& s, i64 x, i64 y, char c, Color color, i64 scale = 1) const;

    // Returns the x position after the last character.
    i64 drawText(Surface& s, i64 x, i64 y, char const* text, Color color, i64 scale = 1) const;

    i64 textWidth(char const* text, i64 scale = 1) const;
};

} // namespace Rudracore::Gfx
