#include <kapala/gfx.h>
#include <kapala/heap.h>
#include <kapala/sys.h>

extern "C" unsigned char const _kapalaFontStart[];
extern "C" unsigned char const _kapalaFontEnd[];

namespace Kapala::Gfx {

namespace {

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline float fmin(float a, float b) { return a < b ? a : b; }
inline float fmax(float a, float b) { return a > b ? a : b; }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float fabs(float v) { return v < 0 ? -v : v; }
inline float sqrt(float v) { return __builtin_sqrtf(v); }

// Signed distance from (px, py) to a rounded rectangle centered at (cx, cy)
// with half-size (hw, hh) and corner radius r: < 0 inside, > 0 outside.
float roundedDistance(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = fabs(px - cx) - (hw - r);
    float qy = fabs(py - cy) - (hh - r);
    float ox = fmax(qx, 0), oy = fmax(qy, 0);
    return sqrt(ox * ox + oy * oy) + fmin(fmax(qx, qy), 0) - r;
}

} // namespace

// MARK: Geometry

Rect Rect::intersect(Rect o) const {
    int x0 = imax(x, o.x), y0 = imax(y, o.y);
    int x1 = imin(right(), o.right()), y1 = imin(bottom(), o.bottom());
    if (x1 <= x0 or y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

Rect Rect::unite(Rect o) const {
    if (empty())
        return o;
    if (o.empty())
        return *this;
    int x0 = imin(x, o.x), y0 = imin(y, o.y);
    int x1 = imax(right(), o.right()), y1 = imax(bottom(), o.bottom());
    return {x0, y0, x1 - x0, y1 - y0};
}

Color Color::mix(Color o, float t) const {
    t = clamp01(t);
    auto m = [t](unsigned char a, unsigned char b) {
        return static_cast<unsigned char>(a + (int(b) - int(a)) * t + 0.5f);
    };
    return {m(r, o.r), m(g, o.g), m(b, o.b), m(a, o.a)};
}

// MARK: Bitmaps

Bitmap Bitmap::allocate(int width, int height, PixelFormat format) {
    Bitmap b;
    b.pixels = static_cast<unsigned*>(Heap::alloc(static_cast<unsigned long>(width) * height * 4));
    b.width = b.pixels ? width : 0;
    b.height = b.pixels ? height : 0;
    b.stride = width;
    b.format = format;
    return b;
}

void Bitmap::copyFrom(Bitmap const& source, Rect area) {
    area = area.intersect(bounds()).intersect(source.bounds());
    for (int y = area.y; y < area.bottom(); y++) {
        unsigned* dest = row(y) + area.x;
        unsigned const* src = source.row(y) + area.x;
        unsigned long bytes = static_cast<unsigned long>(area.w) * 4;
        asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(bytes) : : "memory");
    }
}

// MARK: Text

// MARK: Fonts

namespace {

struct FileHeader {
    unsigned magic, faceCount;
};

struct FaceHeader {
    char name[16];
    unsigned short size;
    short ascent, descent;
    unsigned short lineHeight;
    unsigned glyphCount, glyphOffset;
} __attribute__((packed));

struct GlyphEntry {
    unsigned codepoint;
    short left, top; // from the pen on the baseline
    unsigned short width, height;
    unsigned advance; // 1/64 px
    unsigned bitmap;  // offset in the file
};

static_assert(sizeof(FaceHeader) == 32 and sizeof(GlyphEntry) == 20);

constexpr int MAX_FACES = 16;

struct Fonts {
    unsigned char const* file = nullptr;
    Font faces[MAX_FACES];
    int count = 0;
    Font bitmap; // the fallback
    bool loaded = false;

    void load() {
        if (loaded)
            return;
        loaded = true;
        file = static_cast<unsigned char const*>(Sys::openFile("fonts"));
        auto const* header = reinterpret_cast<FileHeader const*>(file);
        if (not file or header->magic != 0x314E4642)
            return;
        auto const* faceHeaders = reinterpret_cast<FaceHeader const*>(file + sizeof(FileHeader));
        for (unsigned i = 0; i < header->faceCount and count < MAX_FACES; i++) {
            FaceHeader const& h = faceHeaders[i];
            Font& f = faces[count++];
            f._face = &h;
            f.size = h.size;
            f.ascent = h.ascent;
            f.height = h.lineHeight;
            auto const* glyphs = reinterpret_cast<GlyphEntry const*>(file + h.glyphOffset);
            // Width of "0", and whether every ASCII character is as wide.
            int zero = 0;
            bool same = true;
            unsigned first = 0;
            for (unsigned g = 0; g < h.glyphCount and glyphs[g].codepoint < 0x7F; g++) {
                if (glyphs[g].codepoint == '0')
                    zero = static_cast<int>((glyphs[g].advance + 32) / 64);
                if (not first)
                    first = glyphs[g].advance;
                same = same and glyphs[g].advance == first;
            }
            f.width = zero ? zero : h.size / 2;
            f._monospace = same;
        }
    }

    Font const* find(char const* name, int size) {
        load();
        Font const* best = nullptr;
        for (int i = 0; i < count; i++) {
            auto const* h = static_cast<FaceHeader const*>(faces[i]._face);
            bool sameName = true;
            for (int k = 0; k < 16 and (name[k] or h->name[k]); k++) {
                if (name[k] != h->name[k]) {
                    sameName = false;
                    break;
                }
            }
            if (not sameName)
                continue;
            int d = h->size > size ? h->size - size : size - h->size;
            int bestD = best ? (best->size > size ? best->size - size : size - best->size) : 1000;
            if (d < bestD)
                best = &faces[i];
        }
        return best;
    }
};

Fonts fonts;

GlyphEntry const* findGlyph(Font const& font, unsigned codepoint) {
    auto const* h = static_cast<FaceHeader const*>(font._face);
    auto const* glyphs = reinterpret_cast<GlyphEntry const*>(fonts.file + h->glyphOffset);
    unsigned lo = 0, hi = h->glyphCount;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (glyphs[mid].codepoint < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < h->glyphCount and glyphs[lo].codepoint == codepoint)
        return &glyphs[lo];
    return nullptr;
}

// The glyph to draw for a character: itself, or a stand-in.
GlyphEntry const* glyphFor(Font const& font, unsigned codepoint) {
    if (codepoint == '\t')
        codepoint = ' ';
    GlyphEntry const* g = findGlyph(font, codepoint);
    if (not g)
        g = findGlyph(font, 0xFFFD);
    if (not g)
        g = findGlyph(font, '?');
    return g;
}

} // namespace

unsigned decodeUtf8(char const*& text) {
    auto b = static_cast<unsigned char>(*text++);
    if (b < 0x80)
        return b;
    int extra = (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : 0;
    if (extra == 0)
        return 0xFFFD;
    unsigned c = b & (0x3F >> extra);
    for (int i = 0; i < extra; i++) {
        auto next = static_cast<unsigned char>(*text);
        if ((next & 0xC0) != 0x80)
            return 0xFFFD;
        c = c << 6 | (next & 0x3F);
        text++;
    }
    return c;
}

Font const& Font::system() {
    static Font const* font = fonts.find("sans", 13);
    return font ? *font : fonts.bitmap;
}

Font const& Font::bold() {
    static Font const* font = fonts.find("sans-bold", 13);
    return font ? *font : fonts.bitmap;
}

Font const& Font::mono() {
    static Font const* font = fonts.find("mono", 13);
    return font ? *font : fonts.bitmap;
}

Font const& Font::get(char const* face, int size) {
    Font const* font = fonts.find(face, size);
    return font ? *font : fonts.bitmap;
}

unsigned char const* Font::glyph(char c) const {
    // PSF1: 4-byte header, then 16 bytes per glyph (8x16).
    auto index = static_cast<unsigned char>(c);
    unsigned long offset = 4 + static_cast<unsigned long>(index) * 16;
    if (offset + 16 > static_cast<unsigned long>(_kapalaFontEnd - _kapalaFontStart))
        return nullptr;
    return _kapalaFontStart + offset;
}

int Font::measure(char const* text, int scale) const {
    unsigned long n = 0;
    while (text[n])
        n++;
    return measure(text, n, scale);
}

int Font::measure(char const* text, unsigned long bytes, int scale) const {
    char const* end = text + bytes;
    if (not _face) {
        int n = 0;
        while (text < end) {
            decodeUtf8(text);
            n++;
        }
        return n * width * scale;
    }
    unsigned long pen = 0;
    while (text < end) {
        unsigned c = decodeUtf8(text);
        GlyphEntry const* g = glyphFor(*this, c);
        if (g)
            pen += _monospace ? static_cast<unsigned long>(width) * 64 : g->advance;
    }
    return static_cast<int>((pen + 32) / 64);
}

// MARK: Canvas

void Canvas::blend(unsigned& px, Color c, float coverage) {
    float alpha = coverage * (c.a / 255.0f);
    if (alpha >= 0.999f) {
        px = _target.pack(c);
        return;
    }
    if (alpha <= 0.001f)
        return;
    px = _target.pack(_target.unpack(px).mix(c, alpha));
}

void Canvas::fill(Rect r, Color c) {
    r = r.intersect(clip).intersect(_target.bounds());
    if (r.empty())
        return;
    unsigned packed = _target.pack(c);
    for (int y = r.y; y < r.bottom(); y++) {
        unsigned* row = _target.row(y);
        for (int x = r.x; x < r.right(); x++) {
            if (c.a == 255)
                row[x] = packed;
            else
                blend(row[x], c, 1);
        }
    }
}

void Canvas::verticalGradient(Rect full, Color top, Color bottom) {
    Rect r = full.intersect(clip).intersect(_target.bounds());
    for (int y = r.y; y < r.bottom(); y++) {
        float t = full.h > 1 ? float(y - full.y) / float(full.h - 1) : 0;
        Color c = top.mix(bottom, t);
        unsigned packed = _target.pack(c);
        unsigned* row = _target.row(y);
        for (int x = r.x; x < r.right(); x++) {
            if (c.a == 255)
                row[x] = packed;
            else
                blend(row[x], c, 1);
        }
    }
}

void Canvas::horizontalGradient(Rect full, Color left, Color right) {
    Rect r = full.intersect(clip).intersect(_target.bounds());
    for (int y = r.y; y < r.bottom(); y++) {
        unsigned* row = _target.row(y);
        for (int x = r.x; x < r.right(); x++) {
            float t = full.w > 1 ? float(x - full.x) / float(full.w - 1) : 0;
            blend(row[x], left.mix(right, t), 1);
        }
    }
}

void Canvas::fillRounded(Rect r, float radius, Color c) {
    rounded(r, radius, c, c);
}

void Canvas::fillRoundedGradient(Rect r, float radius, Color left, Color right) {
    rounded(r, radius, left, right);
}

void Canvas::rounded(Rect full, float radius, Color left, Color right) {
    float hw = full.w / 2.0f, hh = full.h / 2.0f;
    radius = fmin(radius, fmin(hw, hh));
    float cx = full.x + hw, cy = full.y + hh;
    int corner = static_cast<int>(radius + 1);
    bool solid = left.r == right.r and left.g == right.g and left.b == right.b and left.a == right.a;

    Rect r = full.intersect(clip).intersect(_target.bounds());
    for (int y = r.y; y < r.bottom(); y++) {
        unsigned* row = _target.row(y);
        bool inCornerRows = y < full.y + corner or y >= full.bottom() - corner;
        for (int x = r.x; x < r.right(); x++) {
            Color c = solid ? left : left.mix(right, full.w > 1 ? float(x - full.x) / float(full.w - 1) : 0);
            bool inCornerColumns = x < full.x + corner or x >= full.right() - corner;
            if (not(inCornerRows and inCornerColumns)) {
                blend(row[x], c, 1); // straight edges and the middle: fully covered
                continue;
            }
            float d = roundedDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
            blend(row[x], c, clamp01(0.5f - d));
        }
    }
}

void Canvas::strokeRounded(Rect full, float radius, float width, Color c) {
    float hw = full.w / 2.0f, hh = full.h / 2.0f;
    radius = fmin(radius, fmin(hw, hh));
    float cx = full.x + hw, cy = full.y + hh;

    Rect r = full.intersect(clip).intersect(_target.bounds());
    for (int y = r.y; y < r.bottom(); y++) {
        unsigned* row = _target.row(y);
        for (int x = r.x; x < r.right(); x++) {
            // Skip the inside quickly: only pixels near the edge matter.
            if (x > full.x + radius + width and x < full.right() - radius - width - 1 and
                y > full.y + width + 1 and y < full.bottom() - width - 2)
                continue;
            if (y > full.y + radius + width and y < full.bottom() - radius - width - 1 and
                x > full.x + width + 1 and x < full.right() - width - 2)
                continue;
            float d = roundedDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
            float outer = clamp01(0.5f - d);
            float inner = clamp01(0.5f - (d + width));
            blend(row[x], c, outer - inner);
        }
    }
}

void Canvas::shadow(Rect r, float radius, int spread, Color c) {
    // One pass: darkness fades with the distance from the (shifted-down)
    // shape. The inside is skipped, since the shape itself covers it.
    Rect shape = r.offset(0, spread / 3);
    float hw = shape.w / 2.0f, hh = shape.h / 2.0f;
    radius = fmin(radius, fmin(hw, hh));
    float cx = shape.x + hw, cy = shape.y + hh;
    Rect inner = r.inset(static_cast<int>(radius));

    Rect area = Rect{shape.x - spread, shape.y - spread, shape.w + 2 * spread, shape.h + 2 * spread};
    area = area.intersect(clip).intersect(_target.bounds());
    for (int y = area.y; y < area.bottom(); y++) {
        unsigned* row = _target.row(y);
        bool innerRow = y >= inner.y and y < inner.bottom();
        for (int x = area.x; x < area.right(); x++) {
            if (innerRow and x >= inner.x and x < inner.right()) {
                x = inner.right() - 1; // jump over the covered middle
                continue;
            }
            float d = roundedDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
            if (d >= spread)
                continue;
            float t = d <= 0 ? 1.0f : 1.0f - d / spread;
            blend(row[x], c, t * t);
        }
    }
}

void Canvas::lotus(float cx, float baseY, float size, unsigned char alpha) {
    struct Petal {
        float sin, cos, length, width;
        Color color;
    };
    // Back to front: outer pair (60 degrees), middle pair (30), center.
    Petal const petals[] = {
        {0.866f, 0.5f, 0.70f, 0.30f, Color::hex(0xC2185B)},
        {-0.866f, 0.5f, 0.70f, 0.30f, Color::hex(0xC2185B)},
        {0.5f, 0.866f, 0.90f, 0.33f, Theme::LOTUS},
        {-0.5f, 0.866f, 0.90f, 0.33f, Theme::LOTUS},
        {0.0f, 1.0f, 1.00f, 0.36f, Theme::LOTUS_LIGHT},
    };
    Color const tip = Color::hex(0xFFE0EA);

    Rect box{static_cast<int>(cx - size * 1.9f), static_cast<int>(baseY - size * 2.0f),
             static_cast<int>(size * 3.8f) + 2, static_cast<int>(size * 2.0f) + 2};
    box = box.intersect(clip).intersect(_target.bounds());

    for (auto const& p : petals) {
        float a = p.length * size, b = p.width * size;
        float pcx = cx + p.sin * a, pcy = baseY - p.cos * a; // petal center
        for (int y = box.y; y < box.bottom(); y++) {
            unsigned* row = _target.row(y);
            for (int x = box.x; x < box.right(); x++) {
                float dx = x + 0.5f - pcx, dy = y + 0.5f - pcy;
                float u = dx * p.sin - dy * p.cos; // along the petal, toward the tip
                float v = dx * p.cos + dy * p.sin; // across
                float w = u > 0 ? b * (1.0f - 0.6f * u / a) : b; // narrower toward the tip
                if (w <= 0.5f)
                    continue;
                float e = sqrt((u / a) * (u / a) + (v / w) * (v / w));
                float coverage = clamp01((1.0f - e) * fmin(a, w) + 0.5f);
                if (coverage <= 0)
                    continue;
                Color c = p.color.mix(tip, clamp01((u + a) / (2 * a)) * 0.75f).withAlpha(alpha);
                blend(row[x], c, coverage * 0.92f);
            }
        }
    }
}

int Canvas::paragraph(Rect box, char const* text, Color c, int lineHeight) {
    Font const& font = Font::system();
    int y = box.y;
    while (*text) {
        while (*text == ' ')
            text++;
        // Take as many whole words as fit.
        unsigned long n = 0, lastBreak = 0;
        while (text[n] and text[n] != '\n') {
            unsigned long next = n + 1;
            while (text[next] and (text[next] & 0xC0) == 0x80)
                next++; // whole UTF-8 characters
            if (font.measure(text, next, 1) > box.w)
                break;
            if (text[n] == ' ')
                lastBreak = n;
            n = next;
        }
        if (n == 0 and text[0] and text[0] != '\n')
            n = 1;
        if (text[n] and text[n] != '\n' and text[n] != ' ' and lastBreak > 0)
            n = lastBreak;
        this->text({box.x, y}, text, n, c, font);
        y += lineHeight;
        text += n;
        if (*text == '\n')
            text++;
    }
    return y - box.y;
}

void Canvas::blit(Bitmap const& source, Point at, Rect mask, float maskRadius) {
    Rect r = Rect{at.x, at.y, source.width, source.height}.intersect(clip).intersect(_target.bounds());
    bool masked = not mask.empty();
    if (masked)
        r = r.intersect(mask);

    float hw = mask.w / 2.0f, hh = mask.h / 2.0f;
    float radius = fmin(maskRadius, fmin(hw, hh));
    float cx = mask.x + hw, cy = mask.y + hh;
    int corner = static_cast<int>(radius + 1);

    for (int y = r.y; y < r.bottom(); y++) {
        unsigned* dest = _target.row(y);
        unsigned const* src = source.row(y - at.y) - at.x;
        bool cornerRow = masked and (y < mask.y + corner or y >= mask.bottom() - corner);
        for (int x = r.x; x < r.right(); x++) {
            if (cornerRow and (x < mask.x + corner or x >= mask.right() - corner)) {
                float d = roundedDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
                blend(dest[x], source.unpack(src[x]), clamp01(0.5f - d));
            } else {
                dest[x] = src[x];
            }
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float radius, Color c) {
    Rect full{static_cast<int>(cx - radius - 1), static_cast<int>(cy - radius - 1),
              static_cast<int>(radius * 2 + 3), static_cast<int>(radius * 2 + 3)};
    Rect r = full.intersect(clip).intersect(_target.bounds());
    for (int y = r.y; y < r.bottom(); y++) {
        unsigned* row = _target.row(y);
        for (int x = r.x; x < r.right(); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrt(dx * dx + dy * dy) - radius;
            blend(row[x], c, clamp01(0.5f - d));
        }
    }
}

int Canvas::text(Point at, char const* text, Color c, int scale) {
    if (scale >= 2) {
        Font const& big = Font::get("sans-bold", scale >= 3 ? 28 : 22);
        if (big.smooth())
            return this->text(at, text, c, big);
    } else {
        Font const& font = Font::system();
        if (font.smooth())
            return this->text(at, text, c, font);
    }

    // The bitmap fallback, at integer scales.
    Font const& font = Font::system();
    Rect r = clip.intersect(_target.bounds());
    int x = at.x;
    while (*text) {
        unsigned code = decodeUtf8(text);
        char ch = code < 128 ? static_cast<char>(code) : '?';
        Rect cell{x, at.y, font.width * scale, font.height * scale};
        if (not cell.intersect(r).empty()) {
            unsigned char const* g = font.glyph(ch);
            for (int gy = 0; g and gy < font.height; gy++) {
                for (int gx = 0; gx < font.width; gx++) {
                    if (g[gy] & (0x80 >> gx))
                        fill(Rect{x + gx * scale, at.y + gy * scale, scale, scale}, c);
                }
            }
        }
        x += font.width * scale;
    }
    return x;
}

int Canvas::text(Point at, char const* text, Color c, Font const& font) {
    unsigned long n = 0;
    while (text[n])
        n++;
    return this->text(at, text, n, c, font);
}

int Canvas::text(Point at, char const* text, unsigned long bytes, Color c, Font const& font) {
    if (not font.smooth()) {
        char line[512];
        unsigned long n = bytes < sizeof(line) - 1 ? bytes : sizeof(line) - 1;
        for (unsigned long i = 0; i < n; i++)
            line[i] = text[i];
        line[n] = '\0';
        return this->text(at, line, c, 1);
    }
    Rect r = clip.intersect(_target.bounds());
    // Text this far outside can be skipped quickly.
    if (at.y >= r.bottom() or at.y + font.height <= r.y)
        return at.x + font.measure(text, bytes, 1);

    char const* end = text + bytes;
    unsigned long pen = static_cast<unsigned long>(at.x) * 64;
    int baseline = at.y + font.ascent;
    while (text < end) {
        unsigned code = decodeUtf8(text);
        GlyphEntry const* g = glyphFor(font, code);
        if (not g)
            continue;
        int x0 = static_cast<int>((pen + 32) / 64) + g->left;
        int y0 = baseline + g->top;
        Rect box{x0, y0, g->width, g->height};
        Rect visible = box.intersect(r);
        if (not visible.empty()) {
            unsigned char const* coverage = fonts.file + g->bitmap;
            unsigned packed = _target.pack(c);
            for (int y = visible.y; y < visible.bottom(); y++) {
                unsigned* row = _target.row(y);
                unsigned char const* cov = coverage + (y - y0) * g->width;
                for (int x = visible.x; x < visible.right(); x++) {
                    unsigned char a = cov[x - x0];
                    if (a == 0)
                        continue;
                    if (a == 255 and c.a == 255)
                        row[x] = packed;
                    else
                        blend(row[x], c, a / 255.0f);
                }
            }
        }
        pen += font.monospace() ? static_cast<unsigned long>(font.width) * 64 : g->advance;
    }
    return static_cast<int>((pen + 32) / 64);
}

void Canvas::image(Point at, unsigned char const* rgba, int width, int height) {
    Rect r = Rect{at.x, at.y, width, height}.intersect(clip).intersect(_target.bounds());
    for (int y = r.y; y < r.bottom(); y++) {
        unsigned* row = _target.row(y);
        unsigned char const* src = rgba + (static_cast<long>(y - at.y) * width + (r.x - at.x)) * 4;
        for (int x = r.x; x < r.right(); x++, src += 4)
            blend(row[x], Color{src[0], src[1], src[2], 255}, src[3] / 255.0f);
    }
}

} // namespace Kapala::Gfx
