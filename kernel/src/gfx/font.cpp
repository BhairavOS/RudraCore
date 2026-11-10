#include "gfx/font.h"

extern "C" u8 const _embeddedFontStart[];
extern "C" u8 const _embeddedFontEnd[];

namespace Rudracore::Gfx {

namespace {

constexpr u16 PSF1_MAGIC = 0x0436;
constexpr u8 PSF1_MODE512 = 0x01;
constexpr u32 PSF2_MAGIC = 0x864ab572;

struct [[gnu::packed]] Psf1Header {
    u16 magic;
    u8 mode;
    u8 charSize;
};

struct [[gnu::packed]] Psf2Header {
    u32 magic;
    u32 version;
    u32 headerSize;
    u32 flags;
    u32 glyphCount;
    u32 bytesPerGlyph;
    u32 height;
    u32 width;
};

} // namespace

Font Font::embedded() {
    u8 const* data = _embeddedFontStart;
    usize size = static_cast<usize>(_embeddedFontEnd - _embeddedFontStart);
    Font font{};

    if (size >= sizeof(Psf2Header)) {
        auto const* h = reinterpret_cast<Psf2Header const*>(data);
        if (h->magic == PSF2_MAGIC) {
            font = {
                .glyphs = data + h->headerSize,
                .glyphCount = h->glyphCount,
                .bytesPerGlyph = h->bytesPerGlyph,
                .width = h->width,
                .height = h->height,
            };
        }
    }

    if (not font.valid() and size >= sizeof(Psf1Header)) {
        auto const* h = reinterpret_cast<Psf1Header const*>(data);
        if (h->magic == PSF1_MAGIC) {
            font = {
                .glyphs = data + sizeof(Psf1Header),
                .glyphCount = (h->mode & PSF1_MODE512) ? 512u : 256u,
                .bytesPerGlyph = h->charSize,
                .width = 8,
                .height = h->charSize,
            };
        }
    }

    // Reject a font whose glyph table would run past the embedded data.
    if (font.valid()) {
        usize needed = static_cast<usize>(font.glyphs - data) +
                       usize(font.glyphCount) * font.bytesPerGlyph;
        if (needed > size)
            font = {};
    }

    return font;
}

void Font::drawChar(Surface& s, i64 x, i64 y, char c, Color color, i64 scale) const {
    u32 index = static_cast<u8>(c);
    if (not valid() or index >= glyphCount)
        return;

    u8 const* glyph = glyphs + index * bytesPerGlyph;
    u32 rowBytes = (width + 7) / 8;

    for (u32 gy = 0; gy < height; gy++) {
        u8 const* row = glyph + gy * rowBytes;
        for (u32 gx = 0; gx < width; gx++) {
            if (row[gx / 8] & (0x80 >> (gx % 8)))
                s.fillRect(x + gx * scale, y + gy * scale, scale, scale, color);
        }
    }
}

i64 Font::drawText(Surface& s, i64 x, i64 y, char const* text, Color color, i64 scale) const {
    for (; *text; text++) {
        drawChar(s, x, y, *text, color, scale);
        x += width * scale;
    }
    return x;
}

i64 Font::textWidth(char const* text, i64 scale) const {
    i64 len = 0;
    while (text[len])
        len++;
    return len * width * scale;
}

} // namespace Rudracore::Gfx
