#include "gfx/surface.h"

extern "C" void* memmove(void* dest, void const* src, usize n);

namespace Rudracore::Gfx {

Surface Surface::from(Boot::Framebuffer const& fb) {
    return {
        .base = fb.address,
        .width = static_cast<i64>(fb.width),
        .height = static_cast<i64>(fb.height),
        .pitch = static_cast<i64>(fb.pitch),
        .rShift = fb.redShift,
        .gShift = fb.greenShift,
        .bShift = fb.blueShift,
    };
}

u32 Surface::pack(Color c) const {
    return (u32(c.r) << rShift) | (u32(c.g) << gShift) | (u32(c.b) << bShift);
}

Color Surface::unpack(u32 px) const {
    return {
        static_cast<u8>(px >> rShift),
        static_cast<u8>(px >> gShift),
        static_cast<u8>(px >> bShift),
    };
}

static inline u32* pixelAt(Surface& s, i64 x, i64 y) {
    return reinterpret_cast<u32*>(s.base + y * s.pitch) + x;
}

void Surface::plot(i64 x, i64 y, Color c) {
    if (x < 0 or y < 0 or x >= width or y >= height)
        return;
    *pixelAt(*this, x, y) = pack(c);
}

void Surface::blendPixel(i64 x, i64 y, Color c, u8 alpha) {
    if (x < 0 or y < 0 or x >= width or y >= height)
        return;
    u32* px = pixelAt(*this, x, y);
    *px = pack(unpack(*px).blend(c, alpha));
}

void Surface::fillRect(i64 x, i64 y, i64 w, i64 h, Color c) {
    i64 x0 = max<i64>(x, 0), y0 = max<i64>(y, 0);
    i64 x1 = min(x + w, width), y1 = min(y + h, height);
    u32 packed = pack(c);
    for (i64 py = y0; py < y1; py++) {
        u32* row = pixelAt(*this, 0, py);
        for (i64 px = x0; px < x1; px++)
            row[px] = packed;
    }
}

void Surface::blendRect(i64 x, i64 y, i64 w, i64 h, Color c, u8 alpha) {
    i64 x0 = max<i64>(x, 0), y0 = max<i64>(y, 0);
    i64 x1 = min(x + w, width), y1 = min(y + h, height);
    for (i64 py = y0; py < y1; py++)
        for (i64 px = x0; px < x1; px++)
            blendPixel(px, py, c, alpha);
}

void Surface::verticalGradient(Color top, Color bottom) {
    for (i64 y = 0; y < height; y++) {
        u8 t = static_cast<u8>(height > 1 ? (y * 255) / (height - 1) : 0);
        fillRect(0, y, width, 1, top.blend(bottom, t));
    }
}

void Surface::scrollUp(i64 x, i64 y, i64 w, i64 h, i64 dy) {
    x = max<i64>(x, 0);
    w = min(w, width - x);
    for (i64 row = y; row + dy < y + h and row + dy < height; row++)
        memmove(pixelAt(*this, x, row), pixelAt(*this, x, row + dy), w * 4);
}

} // namespace Rudracore::Gfx
