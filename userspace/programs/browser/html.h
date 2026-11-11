#pragma once

// Turns HTML (or plain text) into positioned runs of text, ready to draw.
// Understands headings, paragraphs, lists, links, emphasis, code,
// preformatted text, block quotes, rules, simple tables, images (as their
// alt text) and the usual entities. Scripts and styles are skipped.

#include "util.h"

#include <kapala/gfx.h>

namespace Browser {

enum class RunKind : u8 { Text, Rule, Bullet, Image };

// The page's fonts.
enum FontId : u8 { BODY, BODY_BOLD, MONO, MONO_BOLD, H1, H2, H3, FONT_COUNT };
Kapala::Gfx::Font const& fontFor(u8 id);

struct Run {
    int x, y, w, h;
    u32 start, length; // UTF-8 text in Document::text
    RunKind kind;
    u8 font;           // FontId
    bool bold, underline;
    Kapala::Gfx::Color color;
    int link; // index into Document::links, or -1
    int image; // Image runs: index into Document::images
};

// A picture on the page.
struct PageImage {
    u32 src;         // offset of its (unresolved) address in Document::text
    u32 alt;         // offset of its description
    int width, height; // from its attributes (0: not given)
};

// Tells the layout how big a picture is once it has loaded (natural size),
// given its address as written on the page. False while unknown.
using ImageSize = bool (*)(char const* src, int& width, int& height);

struct Document {
    Vector<Run> runs;
    Vector<char> text;
    Vector<u32> links; // offsets of 0-terminated hrefs in `text`
    Vector<PageImage> images;
    char title[128] = {};
    int height = 0;

    void clear() {
        runs.clear();
        text.clear();
        links.clear();
        images.clear();
        title[0] = '\0';
        height = 0;
    }

    char const* href(int link) const { return text.data() + links[static_cast<u64>(link)]; }
    char const* at(u32 offset) const { return text.data() + offset; }
};

// Lays out `source` for a page `width` pixels wide.
void layout(char const* source, u64 length, bool plainText, int width, Document& out, ImageSize sizes = nullptr);

namespace Colors {
constexpr auto PAPER = Kapala::Gfx::Color::hex(0xFBF8F3);
constexpr auto INK = Kapala::Gfx::Color::hex(0x221C2E);
constexpr auto HEADING = Kapala::Gfx::Color::hex(0x4A1238);
constexpr auto LINK = Kapala::Gfx::Color::hex(0x1A55C8);
constexpr auto DIM = Kapala::Gfx::Color::hex(0x6E6680);
constexpr auto CODE = Kapala::Gfx::Color::hex(0x8A2F12);
constexpr auto RULE = Kapala::Gfx::Color::hex(0xD9D2E2);
} // namespace Colors

} // namespace Browser
