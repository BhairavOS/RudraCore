#pragma once

// Kapalaframe graphics: bitmaps, colors, and a Canvas to draw on them.
//
// Every Canvas operation is clipped to `canvas.clip`, so a program can
// redraw just the part of the screen that changed: set the clip to the
// damaged rectangle and draw the whole scene; only that area is touched.
// Shapes are anti-aliased (edges are computed from a signed distance).

namespace Kapala::Gfx {

// MARK: Geometry

struct Point {
    int x = 0, y = 0;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 or h <= 0; }

    bool contains(Point p) const { return p.x >= x and p.y >= y and p.x < right() and p.y < bottom(); }

    Rect intersect(Rect o) const;
    Rect unite(Rect o) const; // the smallest rect holding both (empty rects are ignored)
    Rect inset(int d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect offset(int dx, int dy) const { return {x + dx, y + dy, w, h}; }
};

// MARK: Color

struct Color {
    unsigned char r = 0, g = 0, b = 0, a = 255;

    static constexpr Color hex(unsigned rgb, unsigned char alpha = 255) {
        return {static_cast<unsigned char>(rgb >> 16), static_cast<unsigned char>(rgb >> 8),
                static_cast<unsigned char>(rgb), alpha};
    }

    constexpr Color withAlpha(unsigned char alpha) const { return {r, g, b, alpha}; }

    // Linear mix toward `o`, t in [0, 1].
    Color mix(Color o, float t) const;
};

// Bhairav OS colors.
namespace Theme {
constexpr Color NIGHT = Color::hex(0x140B2E);
constexpr Color MAROON = Color::hex(0x3B0D2E);
constexpr Color SURFACE = Color::hex(0x1E1638);     // windows
constexpr Color SURFACE_ALT = Color::hex(0x2A2048); // fields, raised areas
constexpr Color BORDER = Color::hex(0x463A6B);
constexpr Color SAFFRON = Color::hex(0xFF9933);
constexpr Color GOLD = Color::hex(0xFFC857);
constexpr Color LOTUS = Color::hex(0xE75480);
constexpr Color LOTUS_LIGHT = Color::hex(0xFF8FAB);
constexpr Color TEXT = Color::hex(0xF0EAF8);
constexpr Color TEXT_DIM = Color::hex(0xA89CC0);
constexpr Color WHITE = Color::hex(0xFFFFFF);
constexpr Color BLACK = Color::hex(0x000000);
} // namespace Theme

// MARK: Bitmaps

// Where each channel sits in a 32-bit pixel (matches the screen's format,
// so bitmaps can be copied to it directly).
struct PixelFormat {
    unsigned char red = 16, green = 8, blue = 0;
};

struct Bitmap {
    unsigned* pixels = nullptr;
    int width = 0, height = 0;
    int stride = 0; // pixels per row
    PixelFormat format;

    Rect bounds() const { return {0, 0, width, height}; }

    unsigned pack(Color c) const {
        return (unsigned(c.r) << format.red) | (unsigned(c.g) << format.green) | (unsigned(c.b) << format.blue);
    }

    Color unpack(unsigned px) const {
        return {static_cast<unsigned char>(px >> format.red), static_cast<unsigned char>(px >> format.green),
                static_cast<unsigned char>(px >> format.blue), 255};
    }

    unsigned* row(int y) const { return pixels + long(y) * stride; }

    // A new bitmap on the heap, same format as `like`.
    static Bitmap allocate(int width, int height, PixelFormat format);

    // Copies `area` of `source` to the same place in this bitmap.
    void copyFrom(Bitmap const& source, Rect area);
};

// MARK: Text

// Fonts: smooth, anti-aliased faces rendered from DejaVu (the "fonts" file
// in the boot image, shared by every program), with the built-in 8x16
// bitmap font (Terminus) as a fallback. Text is UTF-8.
//
//   Font::system()       the interface font (sans, 13 px)
//   Font::bold()         bold interface text
//   Font::mono()         fixed width, for terminals and code (13 px)
//   Font::get("sans-bold", 22)  any face: sans, sans-bold, mono, mono-bold;
//                               the nearest size available
struct Font {
    int width = 8;   // monospace: every character's advance; otherwise a typical one
    int height = 16; // a line's height (ascent + descent)
    int ascent = 12; // from the top of the line to the baseline
    int size = 16;   // in pixels

    static Font const& system();
    static Font const& bold();
    static Font const& mono();
    static Font const& get(char const* face, int size);

    // Width in pixels of UTF-8 text (or of the first `bytes` bytes).
    int measure(char const* text, int scale = 1) const;
    int measure(char const* text, unsigned long bytes, int scale) const;

    bool smooth() const { return _face != nullptr; }
    bool monospace() const { return _monospace; }

    // The bitmap font's 16 rows for a character (fallback only).
    unsigned char const* glyph(char c) const;

    // (internal)
    void const* _face = nullptr;
    bool _monospace = false;
};

// Decodes one UTF-8 character at *text and moves past it.
unsigned decodeUtf8(char const*& text);

// MARK: Canvas

class Canvas {
public:
    explicit Canvas(Bitmap& target) : clip(target.bounds()), _target(target) {}

    Bitmap& target() { return _target; }

    void fill(Rect r, Color c);
    void verticalGradient(Rect r, Color top, Color bottom);
    void horizontalGradient(Rect r, Color left, Color right);

    void fillRounded(Rect r, float radius, Color c);
    void fillRoundedGradient(Rect r, float radius, Color left, Color right); // horizontal
    void strokeRounded(Rect r, float radius, float width, Color c);

    // A soft shadow around a rounded rectangle (slightly below it). Only the
    // part outside the rectangle is drawn: it's meant to sit under an
    // opaque shape.
    void shadow(Rect r, float radius, int spread, Color c);

    void fillCircle(float cx, float cy, float radius, Color c);

    // The Bhairav OS lotus: five petals rising from (cx, baseY).
    void lotus(float cx, float baseY, float size, unsigned char alpha = 255);

    // Draws text with its top-left corner at `at`. Returns the x after it.
    // Scale 1 uses the interface font; 2 and up a large bold one.
    int text(Point at, char const* text, Color c, int scale = 1);
    int text(Point at, char const* text, Color c, Font const& font);
    // Just the first `bytes` bytes of text.
    int text(Point at, char const* text, unsigned long bytes, Color c, Font const& font);

    // Draws text wrapped at word boundaries inside `box`. Returns the
    // height used.
    int paragraph(Rect box, char const* text, Color c, int lineHeight = 20);

    // Draws an RGBA image (bytes R, G, B, A per pixel) blended over the canvas.
    void image(Point at, unsigned char const* rgba, int width, int height);

    // Copies a bitmap (same pixel format) with its top-left corner at `at`.
    // With a `mask`, pixels outside that rounded rectangle are left out
    // (anti-aliased), e.g. to round a window's bottom corners.
    void blit(Bitmap const& source, Point at, Rect mask = {}, float maskRadius = 0);

    Rect clip;

private:
    void blend(unsigned& px, Color c, float coverage);
    void rounded(Rect r, float radius, Color left, Color right);

    Bitmap& _target;
};

} // namespace Kapala::Gfx
