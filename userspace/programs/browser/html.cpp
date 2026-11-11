#include "html.h"

namespace Browser {

using Kapala::Gfx::Color;

namespace {

constexpr int GLYPH_W = 8, GLYPH_H = 16;

struct Style {
    u8 scale = 1;
    bool bold = false, underline = false, pre = false, hidden = false;
    Color color = Colors::INK;
    int link = -1;
    int indent = 0;
};

// MARK: Line building

class Builder {
public:
    Builder(Document& doc, int width) : _doc(doc), _width(width) {}

    Style style;

    void word(char const* w, u64 n) {
        if (style.hidden or n == 0)
            return;
        int cw = GLYPH_W * style.scale;
        int left = style.indent, right = _width;
        if (_empty or _x < left)
            _x = left; // a line starts at the indent of whatever is open now

        bool spaced = _space and not _empty;
        _space = false;
        int width = static_cast<int>(n) * cw;
        if (spaced) {
            if (_x + cw + width > right)
                newLine();
            else
                _x += cw;
        } else if (_x + width > right and not _empty) {
            newLine();
        }

        // Longer than a whole line: break it up.
        while (_x + width > right and n > 1) {
            int fit = (right - _x) / cw;
            if (fit < 1)
                fit = 1;
            if (static_cast<u64>(fit) >= n)
                break;
            emit(w, static_cast<u64>(fit), false);
            w += fit;
            n -= static_cast<u64>(fit);
            width = static_cast<int>(n) * cw;
            newLine();
        }
        emit(w, n, spaced);
    }

    void space() { _space = true; }

    // <br>: ends the line (an empty one too).
    void lineBreak() {
        if (style.hidden)
            return;
        if (_empty)
            _y += lineHeight(style.scale);
        else
            newLine();
        _space = false;
        _margin = 0;
    }

    // Starts a new block, with `margin` pixels above it (margins collapse).
    void block(int margin) {
        if (not _empty)
            newLine();
        _space = false;
        if (margin > _margin) {
            _y += margin - _margin;
            _margin = margin;
        }
    }

    void rule() {
        block(10);
        Run r{};
        r.kind = RunKind::Rule;
        r.x = style.indent;
        r.y = _y;
        r.w = _width - style.indent;
        r.h = 1;
        r.color = Colors::RULE;
        r.link = -1;
        _doc.runs.push(r);
        _y += 1;
        _margin = 0;
        block(10);
    }

    // A list marker in the margin left of the current indent.
    void marker(char const* text) {
        if (style.hidden)
            return;
        block(4);
        _x = style.indent;
        Run r{};
        r.y = _y;
        r.h = GLYPH_H;
        r.scale = 1;
        r.color = Colors::DIM;
        r.link = -1;
        if (text) {
            r.kind = RunKind::Text;
            r.start = static_cast<u32>(_doc.text.size());
            r.length = static_cast<u32>(length(text));
            _doc.text.append(text, r.length);
            r.w = static_cast<int>(r.length) * GLYPH_W;
            r.x = style.indent - r.w - 8;
        } else {
            r.kind = RunKind::Bullet;
            r.w = 6;
            r.x = style.indent - 14;
        }
        _doc.runs.push(r);
        _empty = true; // the marker alone doesn't start the line's text
        _hasMarker = true;
    }

    void finish() {
        if (not _empty or _hasMarker)
            newLine();
        _doc.height = _y + 24;
    }

private:
    static int lineHeight(int scale) { return GLYPH_H * scale + 5 * scale; }

    void emit(char const* w, u64 n, bool spaced) {
        // Joins the previous run when the style is the same, so a
        // paragraph is a few long runs rather than one per word.
        if (spaced and _doc.runs.size() > _lineStart) {
            Run& last = _doc.runs.back();
            if (last.kind == RunKind::Text and last.scale == style.scale and last.bold == style.bold and
                last.underline == style.underline and last.link == style.link and
                last.color.r == style.color.r and last.color.g == style.color.g and
                last.color.b == style.color.b and last.start + last.length == _doc.text.size() and
                last.length + n < 400) {
                _doc.text.push(' ');
                _doc.text.append(w, n);
                last.length += static_cast<u32>(n + 1);
                int cw = GLYPH_W * style.scale;
                last.w += cw * static_cast<int>(n + 1);
                _x += cw * static_cast<int>(n);
                _empty = false;
                _margin = 0;
                return;
            }
        }
        Run r{};
        r.kind = RunKind::Text;
        r.x = _x;
        r.y = _y;
        r.scale = style.scale;
        r.w = static_cast<int>(n) * GLYPH_W * style.scale;
        r.h = GLYPH_H * style.scale;
        r.bold = style.bold;
        r.underline = style.underline;
        r.color = style.color;
        r.link = style.link;
        r.start = static_cast<u32>(_doc.text.size());
        r.length = static_cast<u32>(n);
        _doc.text.append(w, n);
        _doc.runs.push(r);
        _x += r.w;
        _empty = false;
        _margin = 0;
    }

    // Finishes the current line: lines up the runs' bottoms (a line can
    // mix text sizes) and moves down.
    void newLine() {
        int height = 0, scale = 1;
        for (u64 i = _lineStart; i < _doc.runs.size(); i++) {
            Run const& r = _doc.runs[i];
            if (r.kind != RunKind::Rule and r.h > height)
                height = r.h;
            if (r.scale > scale)
                scale = r.scale;
        }
        if (height == 0)
            height = GLYPH_H;
        for (u64 i = _lineStart; i < _doc.runs.size(); i++) {
            Run& r = _doc.runs[i];
            if (r.kind != RunKind::Rule)
                r.y = _y + height - r.h;
        }
        _y += height + 5 * scale;
        _x = style.indent;
        _lineStart = _doc.runs.size();
        _empty = true;
        _hasMarker = false;
    }

    Document& _doc;
    int _width;
    int _x = 0, _y = 0;
    int _margin = 1000; // nothing above the first block
    u64 _lineStart = 0;
    bool _empty = true, _space = false, _hasMarker = false;
};

// MARK: Characters

// Unicode to the font's ASCII (an approximation where needed).
void appendCodepoint(u32 c, Vector<char>& out) {
    char const* s = nullptr;
    switch (c) {
    case 0xA0: s = " "; break;
    case 0xA9: s = "(c)"; break;
    case 0xAE: s = "(R)"; break;
    case 0xB0: s = " deg"; break;
    case 0xB7: case 0x2022: case 0x2023: case 0x25CF: s = "*"; break;
    case 0xAB: s = "<<"; break;
    case 0xBB: s = ">>"; break;
    case 0xD7: s = "x"; break;
    case 0x2013: case 0x2010: case 0x2011: case 0x2212: s = "-"; break;
    case 0x2014: case 0x2015: s = "--"; break;
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: s = "'"; break;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: s = "\""; break;
    case 0x2026: s = "..."; break;
    case 0x2122: s = "TM"; break;
    case 0x20AC: s = "EUR"; break;
    case 0xA3: s = "GBP"; break;
    case 0x20B9: s = "Rs"; break;
    case 0x2190: s = "<-"; break;
    case 0x2192: s = "->"; break;
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: return; // invisible
    default: break;
    }
    if (s) {
        out.append(s, length(s));
    } else if (c >= 0xC0 and c <= 0xFF) {
        // Accented Latin letters: drop the accent.
        static constexpr char LATIN[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        out.push(LATIN[c - 0xC0]);
    } else if (c < 128) {
        out.push(static_cast<char>(c));
    } else {
        out.push('?');
    }
}

// Decodes UTF-8 at `p`; returns bytes used.
u64 decodeUtf8(u8 const* p, u64 n, u32& c) {
    u8 b = p[0];
    u64 len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    if (len > n)
        len = 1;
    if (len == 1) {
        c = b < 0x80 ? b : 0xFFFD;
        return 1;
    }
    c = b & (0x7F >> len);
    for (u64 i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            c = 0xFFFD;
            return 1;
        }
        c = c << 6 | (p[i] & 0x3F);
    }
    return len;
}

struct Entity {
    char const* name;
    u32 code;
};

constexpr Entity ENTITIES[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022},
    {"middot", 0xB7}, {"times", 0xD7}, {"raquo", 0xBB}, {"laquo", 0xAB}, {"trade", 0x2122},
    {"deg", 0xB0}, {"euro", 0x20AC}, {"pound", 0xA3}, {"rarr", 0x2192}, {"larr", 0x2190},
    {"shy", 0xAD}, {"zwj", 0x200D}, {"zwnj", 0x200C},
};

// Decodes text (entities and UTF-8) into ASCII.
void decode(char const* s, u64 n, Vector<char>& out) {
    out.clear();
    for (u64 i = 0; i < n;) {
        char ch = s[i];
        if (ch == '&') {
            u64 j = i + 1;
            u32 code = 0;
            bool ok = false;
            if (j < n and s[j] == '#') {
                j++;
                bool hex = j < n and (s[j] == 'x' or s[j] == 'X');
                if (hex)
                    j++;
                u64 digitsStart = j;
                while (j < n and j - digitsStart < 8) {
                    char d = s[j];
                    int v = d >= '0' and d <= '9' ? d - '0'
                            : hex and lower(d) >= 'a' and lower(d) <= 'f' ? lower(d) - 'a' + 10
                                                                          : -1;
                    if (v < 0)
                        break;
                    code = code * (hex ? 16 : 10) + static_cast<u32>(v);
                    j++;
                }
                ok = j > digitsStart;
            } else {
                for (auto const& e : ENTITIES) {
                    u64 len = length(e.name);
                    if (j + len <= n and memcmp(s + j, e.name, len) == 0) {
                        code = e.code;
                        j += len;
                        ok = true;
                        break;
                    }
                }
            }
            if (ok) {
                if (j < n and s[j] == ';')
                    j++;
                appendCodepoint(code, out);
                i = j;
                continue;
            }
            out.push('&');
            i++;
            continue;
        }
        u32 c;
        i += decodeUtf8(reinterpret_cast<u8 const*>(s + i), n - i, c);
        appendCodepoint(c, out);
    }
}

// MARK: Elements

enum Flags : u32 {
    BLOCK = 1 << 0,
    VOID = 1 << 1,    // never has a closing tag
    SKIP = 1 << 2,    // content isn't shown (script, style...)
    RAW = 1 << 3,     // content is plain text up to the closing tag
    HIDDEN = 1 << 4,  // content isn't shown, but is parsed (head)
};

struct Element {
    char const* name;
    u32 flags;
    int margin;
    int indent;
};

constexpr Element ELEMENTS[] = {
    {"p", BLOCK, 12, 0}, {"div", BLOCK, 0, 0}, {"h1", BLOCK, 18, 0}, {"h2", BLOCK, 16, 0},
    {"h3", BLOCK, 14, 0}, {"h4", BLOCK, 12, 0}, {"h5", BLOCK, 10, 0}, {"h6", BLOCK, 10, 0},
    {"ul", BLOCK, 8, 28}, {"ol", BLOCK, 8, 32}, {"menu", BLOCK, 8, 28}, {"li", BLOCK, 2, 0},
    {"dl", BLOCK, 8, 0}, {"dt", BLOCK, 6, 0}, {"dd", BLOCK, 2, 28}, {"blockquote", BLOCK, 12, 28},
    {"pre", BLOCK, 12, 0}, {"section", BLOCK, 4, 0}, {"article", BLOCK, 4, 0}, {"header", BLOCK, 4, 0},
    {"footer", BLOCK, 8, 0}, {"nav", BLOCK, 4, 0}, {"main", BLOCK, 4, 0}, {"aside", BLOCK, 8, 0},
    {"figure", BLOCK, 8, 16}, {"figcaption", BLOCK, 4, 0}, {"form", BLOCK, 4, 0},
    {"fieldset", BLOCK, 8, 8}, {"legend", BLOCK, 4, 0}, {"address", BLOCK, 8, 0},
    {"details", BLOCK, 4, 0}, {"summary", BLOCK, 4, 0}, {"center", BLOCK, 4, 0},
    {"table", BLOCK, 8, 0}, {"tr", BLOCK, 2, 0}, {"caption", BLOCK, 4, 0}, {"body", BLOCK, 0, 0},
    {"html", BLOCK, 0, 0}, {"hgroup", BLOCK, 4, 0}, {"noscript", BLOCK, 4, 0},
    {"br", VOID, 0, 0}, {"hr", VOID, 0, 0}, {"img", VOID, 0, 0}, {"input", VOID, 0, 0},
    {"meta", VOID, 0, 0}, {"link", VOID, 0, 0}, {"wbr", VOID, 0, 0}, {"area", VOID, 0, 0},
    {"base", VOID, 0, 0}, {"col", VOID, 0, 0}, {"embed", VOID, 0, 0}, {"source", VOID, 0, 0},
    {"track", VOID, 0, 0}, {"param", VOID, 0, 0},
    {"script", SKIP | RAW, 0, 0}, {"style", SKIP | RAW, 0, 0}, {"template", SKIP, 0, 0},
    {"svg", SKIP, 0, 0}, {"iframe", SKIP, 0, 0}, {"object", SKIP, 0, 0}, {"canvas", SKIP, 0, 0},
    {"select", SKIP, 0, 0}, {"video", SKIP, 0, 0}, {"audio", SKIP, 0, 0}, {"map", SKIP, 0, 0},
    {"math", SKIP, 0, 0}, {"title", RAW, 0, 0}, {"textarea", RAW, 0, 0}, {"head", HIDDEN, 0, 0},
};

Element const* findElement(char const* name) {
    for (auto const& e : ELEMENTS) {
        if (equals(e.name, name))
            return &e;
    }
    return nullptr;
}

struct Open {
    char name[12];
    Style saved;
    Element const* element;
    int listCounter;
    bool ordered;
};

class Parser {
public:
    Parser(Document& doc, int width) : _doc(doc), _b(doc, width) {}

    void html(char const* s, u64 n) {
        u64 i = 0;
        while (i < n) {
            if (s[i] != '<') {
                u64 start = i;
                while (i < n and s[i] != '<')
                    i++;
                text(s + start, i - start);
                continue;
            }
            if (i + 4 <= n and memcmp(s + i, "<!--", 4) == 0) {
                u64 end = find(s, n, i + 4, "-->");
                i = end < n ? end + 3 : n;
                continue;
            }
            if (i + 1 < n and (s[i + 1] == '!' or s[i + 1] == '?')) {
                while (i < n and s[i] != '>')
                    i++;
                i++;
                continue;
            }
            bool closing = i + 1 < n and s[i + 1] == '/';
            u64 j = i + (closing ? 2 : 1);
            if (j >= n or not isLetter(s[j])) { // not a tag: a lone '<'
                text(s + i, 1);
                i++;
                continue;
            }
            char name[12];
            u64 len = 0;
            while (j < n and (isLetter(s[j]) or (s[j] >= '0' and s[j] <= '9') or s[j] == '-')) {
                if (len + 1 < sizeof(name))
                    name[len++] = lower(s[j]);
                j++;
            }
            name[len] = '\0';

            // Attributes, up to '>'.
            _href[0] = _alt[0] = _value[0] = _type[0] = '\0';
            while (j < n and s[j] != '>') {
                while (j < n and (isSpace(s[j]) or s[j] == '/'))
                    j++;
                if (j >= n or s[j] == '>')
                    break;
                char attr[16];
                u64 a = 0;
                while (j < n and not isSpace(s[j]) and s[j] != '=' and s[j] != '>') {
                    if (a + 1 < sizeof(attr))
                        attr[a++] = lower(s[j]);
                    j++;
                }
                attr[a] = '\0';
                while (j < n and isSpace(s[j]))
                    j++;
                if (j < n and s[j] == '=') {
                    j++;
                    while (j < n and isSpace(s[j]))
                        j++;
                    u64 vs, ve;
                    if (j < n and (s[j] == '"' or s[j] == '\'')) {
                        char quote = s[j++];
                        vs = j;
                        while (j < n and s[j] != quote)
                            j++;
                        ve = j;
                        if (j < n)
                            j++;
                    } else {
                        vs = j;
                        while (j < n and not isSpace(s[j]) and s[j] != '>')
                            j++;
                        ve = j;
                    }
                    attribute(attr, s + vs, ve - vs);
                } else if (a == 0) {
                    j++; // stray character
                }
            }
            i = j < n ? j + 1 : n;

            if (closing) {
                close(name);
                continue;
            }
            Element const* e = findElement(name);
            if (e and (e->flags & RAW)) {
                u64 end = findClosing(s, n, i, name);
                if (equals(name, "title")) {
                    decode(s + i, end - i, _scratch);
                    copyTitle();
                } else if (equals(name, "textarea")) {
                    open(name, e);
                    text(s + i, end - i);
                    close(name);
                }
                i = end;
                while (i < n and s[i] != '>')
                    i++;
                i++;
                continue;
            }
            open(name, e);
        }
    }

    void plain(char const* s, u64 n) {
        _b.style.pre = true;
        Element const* pre = findElement("pre");
        open("pre", pre);
        text(s, n);
    }

    void finish() { _b.finish(); }

private:
    static bool isLetter(char c) { return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z'); }

    static u64 find(char const* s, u64 n, u64 from, char const* needle) {
        u64 len = length(needle);
        for (u64 i = from; i + len <= n; i++) {
            if (memcmp(s + i, needle, len) == 0)
                return i;
        }
        return n;
    }

    // Where "</name" starts, case-insensitively.
    static u64 findClosing(char const* s, u64 n, u64 from, char const* name) {
        u64 len = length(name);
        for (u64 i = from; i + len + 2 <= n; i++) {
            if (s[i] == '<' and s[i + 1] == '/') {
                bool same = true;
                for (u64 k = 0; k < len and same; k++)
                    same = lower(s[i + 2 + k]) == name[k];
                if (same)
                    return i;
            }
        }
        return n;
    }

    void attribute(char const* name, char const* value, u64 n) {
        char* dest = equals(name, "href") ? _href : equals(name, "alt") ? _alt : equals(name, "value") ? _value
                     : equals(name, "type") ? _type : nullptr;
        if (not dest)
            return;
        decode(value, n, _scratch);
        u64 len = _scratch.size() < 1023 ? _scratch.size() : 1023;
        memcpy(dest, _scratch.data(), len);
        dest[len] = '\0';
    }

    void copyTitle() {
        u64 n = 0;
        bool space = false;
        for (u64 i = 0; i < _scratch.size() and n + 1 < sizeof(_doc.title); i++) {
            char c = _scratch[i];
            if (isSpace(c)) {
                space = n > 0;
                continue;
            }
            if (space and n + 2 < sizeof(_doc.title))
                _doc.title[n++] = ' ';
            space = false;
            _doc.title[n++] = c;
        }
        _doc.title[n] = '\0';
    }

    void text(char const* s, u64 n) {
        if (_b.style.hidden)
            return;
        decode(s, n, _scratch);
        char const* t = _scratch.data();
        u64 len = _scratch.size();

        if (_b.style.pre) {
            u64 start = 0;
            for (u64 i = 0; i <= len; i++) {
                if (i == len or t[i] == '\n') {
                    // Tabs to spaces, then the whole line as one word.
                    char line[512];
                    u64 m = 0;
                    for (u64 k = start; k < i and m + 8 < sizeof(line); k++) {
                        if (t[k] == '\t') {
                            do
                                line[m++] = ' ';
                            while (m % 4);
                        } else if (t[k] != '\r') {
                            line[m++] = t[k];
                        }
                    }
                    if (m)
                        _b.word(line, m);
                    if (i < len)
                        _b.lineBreak();
                    start = i + 1;
                }
            }
            return;
        }

        u64 i = 0;
        while (i < len) {
            if (isSpace(t[i])) {
                _b.space();
                while (i < len and isSpace(t[i]))
                    i++;
                continue;
            }
            u64 start = i;
            while (i < len and not isSpace(t[i]))
                i++;
            _b.word(t + start, i - start);
        }
    }

    Open* top() { return _depth ? &_stack[_depth - 1] : nullptr; }

    bool isOpen(char const* name, int within = 64) {
        for (int k = _depth - 1; k >= 0 and k >= _depth - within; k--) {
            if (equals(_stack[k].name, name))
                return true;
        }
        return false;
    }

    void open(char const* name, Element const* e) {
        // Body content ends the head, even without a </head>.
        if (e and (e->flags & BLOCK) and isOpen("head"))
            close("head");
        // Elements that end the previous one of their kind implicitly (any
        // block ends an open paragraph).
        if (isOpen("p", 1) and (equals(name, "p") or (e and (e->flags & BLOCK))))
            close("p");
        if (equals(name, "li") and isOpen("li", 3))
            close("li");
        if ((equals(name, "dt") or equals(name, "dd")) and (isOpen("dt", 2) or isOpen("dd", 2)))
            close(isOpen("dt", 2) ? "dt" : "dd");
        if (equals(name, "tr") and isOpen("tr", 4))
            close("tr");
        if ((equals(name, "td") or equals(name, "th")) and (isOpen("td", 2) or isOpen("th", 2)))
            close(isOpen("td", 2) ? "td" : "th");

        u32 flags = e ? e->flags : 0;

        // Void elements: act, nothing to push.
        if (flags & VOID) {
            if (equals(name, "br")) {
                _b.lineBreak();
            } else if (equals(name, "hr")) {
                _b.rule();
            } else if (equals(name, "img")) {
                if (_alt[0])
                    imageLabel(_alt);
            } else if (equals(name, "input")) {
                if (equals(_type, "submit") or equals(_type, "button") or equals(_type, "reset"))
                    imageLabel(_value[0] ? _value : "Submit");
                else if (not equals(_type, "hidden") and not equals(_type, "checkbox") and
                         not equals(_type, "radio"))
                    imageLabel("________");
            }
            return;
        }

        if (_depth >= 64)
            return; // too deep: treat the rest as part of this element
        Open& o = _stack[_depth++];
        copy(o.name, name, sizeof(o.name));
        o.saved = _b.style;
        o.element = e;
        o.ordered = equals(name, "ol");
        o.listCounter = 0;

        Style& st = _b.style;
        if (flags & (SKIP | HIDDEN))
            st.hidden = true;
        if (flags & BLOCK) {
            st.indent += e->indent;
            _b.block(e->margin);
        }

        if (name[0] == 'h' and name[1] >= '1' and name[1] <= '6' and not name[2]) {
            st.bold = true;
            st.color = Colors::HEADING;
            if (name[1] <= '2')
                st.scale = 2;
        } else if (equals(name, "b") or equals(name, "strong") or equals(name, "th") or equals(name, "dt") or
                   equals(name, "summary") or equals(name, "legend")) {
            st.bold = true;
        } else if (equals(name, "u") or equals(name, "ins")) {
            st.underline = true;
        } else if (equals(name, "code") or equals(name, "kbd") or equals(name, "samp") or equals(name, "tt")) {
            st.color = Colors::CODE;
        } else if (equals(name, "pre")) {
            st.pre = true;
            st.color = Colors::CODE;
        } else if (equals(name, "i") or equals(name, "em") or equals(name, "cite") or equals(name, "s") or
                   equals(name, "del") or equals(name, "strike") or equals(name, "small") or
                   equals(name, "figcaption") or equals(name, "blockquote")) {
            if (st.link < 0)
                st.color = Colors::DIM;
        } else if (equals(name, "a")) {
            if (_href[0]) {
                st.link = static_cast<int>(_doc.links.size());
                _doc.links.push(static_cast<u32>(_doc.text.size()));
                _doc.text.append(_href, length(_href) + 1);
                st.color = Colors::LINK;
                st.underline = true;
            }
        } else if (equals(name, "li")) {
            Open* list = nullptr;
            for (int k = _depth - 2; k >= 0; k--) {
                if (equals(_stack[k].name, "ul") or equals(_stack[k].name, "ol") or equals(_stack[k].name, "menu")) {
                    list = &_stack[k];
                    break;
                }
            }
            if (not list)
                st.indent += 28;
            if (list and list->ordered) {
                char number[12];
                int count = ++list->listCounter;
                int d = 0;
                char digits[8];
                do {
                    digits[d++] = static_cast<char>('0' + count % 10);
                    count /= 10;
                } while (count and d < 7);
                for (int k = 0; k < d; k++)
                    number[k] = digits[d - 1 - k];
                number[d] = '.';
                number[d + 1] = '\0';
                _b.marker(number);
            } else {
                _b.marker(nullptr);
            }
        } else if (equals(name, "td") or equals(name, "th")) {
            _b.space();
            _b.space();
        }
    }

    void close(char const* name) {
        int found = -1;
        for (int k = _depth - 1; k >= 0; k--) {
            if (equals(_stack[k].name, name)) {
                found = k;
                break;
            }
        }
        if (found < 0) {
            if (equals(name, "p")) // a stray </p> still ends a paragraph
                _b.block(12);
            return;
        }
        while (_depth > found) {
            Open& o = _stack[--_depth];
            bool block = o.element and (o.element->flags & BLOCK);
            _b.style = o.saved;
            if (block)
                _b.block(o.element->margin);
            else if (equals(o.name, "td") or equals(o.name, "th"))
                _b.space();
        }
    }

    void imageLabel(char const* label) {
        Style saved = _b.style;
        if (_b.style.link < 0)
            _b.style.color = Colors::DIM;
        _b.space();
        char text[1100];
        copy(text, "[", sizeof(text));
        append(text, label, sizeof(text));
        append(text, "]", sizeof(text));
        // Several words: the builder wraps them.
        decode(text, length(text), _scratch);
        u64 i = 0, n = _scratch.size();
        char const* t = _scratch.data();
        while (i < n) {
            if (isSpace(t[i])) {
                _b.space();
                i++;
                continue;
            }
            u64 start = i;
            while (i < n and not isSpace(t[i]))
                i++;
            _b.word(t + start, i - start);
        }
        _b.space();
        _b.style = saved;
    }

    Document& _doc;
    Builder _b;
    Open _stack[64];
    int _depth = 0;
    Vector<char> _scratch;
    char _href[1024], _alt[1024], _value[1024], _type[1024];
};

} // namespace

void layout(char const* source, u64 n, bool plainText, int width, Document& out) {
    out.clear();
    out.text.push('\0'); // offset 0 is never a real run
    auto* parser = new Parser(out, width);
    if (plainText)
        parser->plain(source, n);
    else
        parser->html(source, n);
    parser->finish();
    delete parser;
}

} // namespace Browser
