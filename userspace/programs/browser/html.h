#pragma once

// Turns HTML (or plain text) into positioned runs of text, ready to draw.
// Understands headings, paragraphs, lists, links, emphasis, code,
// preformatted text, block quotes, rules, simple tables, images (as their
// alt text) and the usual entities. Scripts and styles are skipped.

#include "util.h"

#include <kapala/gfx.h>

namespace Browser {

enum class RunKind : u8 { Text, Rule, Bullet };

struct Run {
    int x, y, w, h;
    u32 start, length; // text in Document::text
    RunKind kind;
    u8 scale;
    bool bold, underline;
    Kapala::Gfx::Color color;
    int link; // index into Document::links, or -1
};

struct Document {
    Vector<Run> runs;
    Vector<char> text;
    Vector<u32> links; // offsets of 0-terminated hrefs in `text`
    char title[128] = {};
    int height = 0;

    void clear() {
        runs.clear();
        text.clear();
        links.clear();
        title[0] = '\0';
        height = 0;
    }

    char const* href(int link) const { return text.data() + links[static_cast<u64>(link)]; }
};

// Lays out `source` for a page `width` pixels wide.
void layout(char const* source, u64 length, bool plainText, int width, Document& out);

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
