#pragma once

#include "boot.h"
#include "types.h"

namespace Rudracore::Gfx {

struct Color {
    u8 r, g, b;

    // Mix `this` toward `other`; alpha 0 = this, 255 = other.
    constexpr Color blend(Color other, u8 alpha) const {
        auto mix = [&](u8 a, u8 b) {
            return static_cast<u8>((a * (255 - alpha) + b * alpha) / 255);
        };
        return {mix(r, other.r), mix(g, other.g), mix(b, other.b)};
    }
};

// A 32-bit linear framebuffer. All drawing is clipped to its bounds.
// No floating point anywhere: the kernel is built without SSE/x87.
struct Surface {
    u8* base = nullptr;
    i64 width = 0;
    i64 height = 0;
    i64 pitch = 0;
    u8 rShift = 16, gShift = 8, bShift = 0;

    static Surface from(Boot::Framebuffer const& fb);

    u32 pack(Color c) const;
    Color unpack(u32 px) const;

    void plot(i64 x, i64 y, Color c);
    void blendPixel(i64 x, i64 y, Color c, u8 alpha);
    void fillRect(i64 x, i64 y, i64 w, i64 h, Color c);
    void blendRect(i64 x, i64 y, i64 w, i64 h, Color c, u8 alpha);
    void verticalGradient(Color top, Color bottom);

    // Copies the rows [y + dy, y + h) up to y. Used for console scrolling.
    void scrollUp(i64 x, i64 y, i64 w, i64 h, i64 dy);
};

} // namespace Rudracore::Gfx
