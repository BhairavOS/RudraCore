#include "gfx/splash.h"

namespace Rudracore::Gfx {

namespace {

// sin/cos scaled by 1024 (no floating point in the kernel).
struct Angle {
    i64 sin, cos;
};

constexpr Angle DEG0{0, 1024};
constexpr Angle DEG30{512, 887};
constexpr Angle DEG60{887, 512};

constexpr Angle mirror(Angle a) { return {-a.sin, a.cos}; }

// One petal: an ellipse of half-length `len` and half-width `wid`, starting at
// the base and leaning by `angle` from vertical. Shaded from `color` at the
// base to a pale tip.
void drawPetal(Surface& s, i64 bx, i64 by, Angle angle, i64 len, i64 wid, Color color) {
    // Axis points from the base toward the tip: (sin, -cos).
    i64 cx = bx + angle.sin * len / 1024;
    i64 cy = by - angle.cos * len / 1024;
    i64 len2 = len * len, wid2 = wid * wid;

    for (i64 y = cy - len; y <= cy + len; y++) {
        for (i64 x = cx - len; x <= cx + len; x++) {
            i64 dx = x - cx, dy = y - cy;
            i64 u = (dx * angle.sin - dy * angle.cos) / 1024; // along the petal
            i64 v = (dx * angle.cos + dy * angle.sin) / 1024; // across the petal

            // Narrow the far half so the tip comes to a soft point.
            i64 w2 = u > 0 ? wid2 * (len - u) / len : wid2;
            if (w2 <= 0 or u * u * w2 + v * v * len2 > len2 * w2)
                continue;

            u8 t = static_cast<u8>(clamp<i64>((u + len) * 200 / (2 * len), 0, 200));
            s.blendPixel(x, y, color.blend(Palette::PETAL_TIP, t), 225);
        }
    }
}

void drawCenteredText(Surface& s, Font const& font, i64 cx, i64 y, char const* text, Color c, i64 scale) {
    font.drawText(s, cx - font.textWidth(text, scale) / 2, y, text, c, scale);
}

} // namespace

void drawLotus(Surface& s, i64 baseX, i64 baseY, i64 size) {
    // Back to front: outer, middle, then center petal.
    drawPetal(s, baseX, baseY, DEG60, size * 7 / 10, size * 3 / 10, Palette::LOTUS_DEEP);
    drawPetal(s, baseX, baseY, mirror(DEG60), size * 7 / 10, size * 3 / 10, Palette::LOTUS_DEEP);
    drawPetal(s, baseX, baseY, DEG30, size * 9 / 10, size * 33 / 100, Palette::LOTUS);
    drawPetal(s, baseX, baseY, mirror(DEG30), size * 9 / 10, size * 33 / 100, Palette::LOTUS);
    drawPetal(s, baseX, baseY, DEG0, size, size * 36 / 100, Palette::LOTUS_LIGHT);

    // Water lines under the flower.
    i64 lineW = size * 2;
    s.blendRect(baseX - lineW / 2, baseY + size / 8, lineW, 3, Palette::SAFFRON, 200);
    s.blendRect(baseX - lineW / 3, baseY + size / 8 + 9, lineW * 2 / 3, 3, Palette::SAFFRON, 140);
}

Rect drawSplash(Surface& s, Font const& font, char const* subtitle) {
    s.verticalGradient(Palette::NIGHT, Palette::MAROON);

    i64 cx = s.width / 2;
    i64 lotusSize = clamp<i64>(s.height / 7, 40, 160);
    i64 lotusBase = s.height / 20 + lotusSize * 2;
    drawLotus(s, cx, lotusBase, lotusSize);

    i64 y = lotusBase + lotusSize / 8 + 30;

    if (font.valid()) {
        i64 titleScale = s.width >= 1024 ? 4 : 3;
        drawCenteredText(s, font, cx, y, "BhairavOS", Palette::SAFFRON, titleScale);
        y += font.height * titleScale + 8;
        drawCenteredText(s, font, cx, y, subtitle, Palette::TEXT_DIM, 1);
        y += font.height + 24;
    }

    // Log console panel with a saffron rule on top.
    i64 margin = s.width / 10;
    Rect panel{margin, y, s.width - 2 * margin, s.height - y - s.height / 20};
    s.fillRect(panel.x, panel.y - 2, panel.w, 2, Palette::SAFFRON);
    s.fillRect(panel.x, panel.y, panel.w, panel.h, Palette::PANEL);

    i64 pad = 12;
    return {panel.x + pad, panel.y + pad, panel.w - 2 * pad, panel.h - 2 * pad};
}

} // namespace Rudracore::Gfx
