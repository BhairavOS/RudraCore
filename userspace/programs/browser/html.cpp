#include "html.h"

namespace Browser {

using Kapala::Gfx::Color;
using Kapala::Gfx::Font;

namespace {

struct Style {
    u8 font = BODY; // BODY, MONO, H1, H2 or H3 (bold picks the bold variant)
    bool bold = false, underline = false, pre = false, hidden = false;
    Color color = Colors::INK;
    int link = -1;
    int indent = 0;
};

// MARK: Line building

u8 resolve(Style const& st) {
    if (st.font == MONO)
        return st.bold ? MONO_BOLD : MONO;
    if (st.font == BODY)
        return st.bold ? BODY_BOLD : BODY;
    return st.font;
}

// Bytes in the UTF-8 character starting at p.
u64 charLength(char const* p) {
    auto b = static_cast<u8>(*p);
    return b < 0xC0 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
}

class Builder {
public:
    Builder(Document& doc, int width) : _doc(doc), _width(width) {}

    Style style;

    void word(char const* w, u64 n) {
        if (style.hidden or n == 0)
            return;
        u8 fontId = resolve(style);
        Font const& font = fontFor(fontId);
        int left = style.indent, right = _width;
        if (_empty or _x < left)
            _x = left; // a line starts at the indent of whatever is open now

        bool spaced = _space and not _empty;
        _space = false;
        int space = font.measure(" ");
        int width = font.measure(w, n, 1);
        if (spaced) {
            if (_x + space + width > right)
                newLine();
            else
                _x += space;
        } else if (_x + width > right and not _empty) {
            newLine();
        }

        // Longer than a whole line: break it up (at character boundaries).
        while (_x + width > right and n > 1) {
            u64 fit = 0;
            while (fit < n) {
                u64 next = fit + charLength(w + fit);
                if (_x + font.measure(w, next, 1) > right)
                    break;
                fit = next;
            }
            if (fit == 0)
                fit = charLength(w);
            if (fit >= n)
                break;
            emit(w, fit, false, fontId, font);
            w += fit;
            n -= fit;
            width = font.measure(w, n, 1);
            newLine();
        }
        emit(w, n, spaced, fontId, font);
    }

    void space() { _space = true; }

    // A picture box, flowing with the text (a big one takes a line).
    void picture(int index, int width, int height) {
        if (style.hidden)
            return;
        int left = style.indent, right = _width;
        if (_empty or _x < left)
            _x = left;
        int space = fontFor(BODY).measure(" ");
        bool spaced = _space and not _empty;
        _space = false;
        if (not _empty and _x + (spaced ? space : 0) + width > right)
            newLine();
        else if (spaced)
            _x += space;
        Run r{};
        r.kind = RunKind::Image;
        r.x = _x;
        r.y = _y;
        r.w = width;
        r.h = height;
        r.font = BODY;
        r.link = style.link;
        r.image = index;
        r.color = Colors::RULE;
        _doc.runs.push(r);
        _x += width;
        _empty = false;
        _margin = 0;
    }

    // <br>: ends the line (an empty one too).
    void lineBreak() {
        if (style.hidden)
            return;
        if (_empty)
            _y += fontFor(resolve(style)).height + 4;
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
        Font const& font = fontFor(BODY);
        Run r{};
        r.y = _y;
        r.h = font.height;
        r.font = BODY;
        r.color = Colors::DIM;
        r.link = -1;
        if (text) {
            r.kind = RunKind::Text;
            r.start = static_cast<u32>(_doc.text.size());
            r.length = static_cast<u32>(length(text));
            _doc.text.append(text, r.length);
            r.w = font.measure(text);
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
    void emit(char const* w, u64 n, bool spaced, u8 fontId, Font const& font) {
        // Joins the previous run when the style is the same, so a
        // paragraph is a few long runs rather than one per word.
        if (spaced and _doc.runs.size() > _lineStart) {
            Run& last = _doc.runs.back();
            if (last.kind == RunKind::Text and last.font == fontId and last.underline == style.underline and
                last.link == style.link and last.color.r == style.color.r and last.color.g == style.color.g and
                last.color.b == style.color.b and last.start + last.length == _doc.text.size() and
                last.length + n < 400) {
                _doc.text.push(' ');
                _doc.text.append(w, n);
                last.length += static_cast<u32>(n + 1);
                last.w = font.measure(_doc.text.data() + last.start, last.length, 1);
                _x = last.x + last.w;
                _empty = false;
                _margin = 0;
                return;
            }
        }
        Run r{};
        r.kind = RunKind::Text;
        r.x = _x;
        r.y = _y;
        r.font = fontId;
        r.w = font.measure(w, n, 1);
        r.h = font.height;
        r.bold = fontId == BODY_BOLD or fontId == MONO_BOLD;
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

    // Finishes the current line: lines up the runs' baselines (a line can
    // mix text sizes) and moves down.
    void newLine() {
        int ascent = 0, descent = 0;
        for (u64 i = _lineStart; i < _doc.runs.size(); i++) {
            Run const& r = _doc.runs[i];
            if (r.kind == RunKind::Rule)
                continue;
            if (r.kind == RunKind::Image) {
                if (r.h > ascent)
                    ascent = r.h;
                continue;
            }
            Font const& f = fontFor(r.font);
            if (f.ascent > ascent)
                ascent = f.ascent;
            if (f.height - f.ascent > descent)
                descent = f.height - f.ascent;
        }
        if (ascent == 0) {
            ascent = fontFor(BODY).ascent;
            descent = fontFor(BODY).height - ascent;
        }
        for (u64 i = _lineStart; i < _doc.runs.size(); i++) {
            Run& r = _doc.runs[i];
            if (r.kind == RunKind::Image)
                r.y = _y + ascent - r.h;
            else if (r.kind != RunKind::Rule)
                r.y = _y + ascent - fontFor(r.font).ascent;
        }
        _y += ascent + descent + 5;
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

// Appends a character as UTF-8 (the fonts cover Latin, Greek, Cyrillic and
// common symbols; others show as a replacement mark).
void appendCodepoint(u32 c, Vector<char>& out) {
    if (c == 0x200B or c == 0x200C or c == 0x200D or c == 0xFEFF or c == 0xAD)
        return; // invisible
    if (c == '\r')
        return;
    if (c < 0x80) {
        out.push(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push(static_cast<char>(0xC0 | (c >> 6)));
        out.push(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push(static_cast<char>(0xE0 | (c >> 12)));
        out.push(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push(static_cast<char>(0xF0 | (c >> 18)));
        out.push(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push(static_cast<char>(0x80 | (c & 0x3F)));
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
    Parser(Document& doc, int width, ImageSize sizes) : _doc(doc), _b(doc, width), _sizes(sizes), _width_limit(width) {}

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
            _href[0] = _alt[0] = _value[0] = _type[0] = _src[0] = '\0';
            _width = _height = 0;
            _hiddenByAttribute = false;
            _dataSrc = false;
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
                } else {
                    attribute(attr, "", 0); // a boolean attribute
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

    void startHidden() {
        if (_mainOnly)
            _b.style.hidden = true;
    }

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
        // Things pages hide with CSS or attributes stay hidden here.
        if (equals(name, "hidden")) {
            _hiddenByAttribute = true;
            return;
        }
        if (equals(name, "aria-hidden") and n == 4 and memcmp(value, "true", 4) == 0) {
            _hiddenByAttribute = true;
            return;
        }
        if (equals(name, "style")) {
            for (u64 k = 0; k + 12 <= n; k++) {
                if (memcmp(value + k, "display:none", 12) == 0 or (k + 13 <= n and memcmp(value + k, "display: none", 13) == 0))
                    _hiddenByAttribute = true;
            }
            return;
        }
        if (equals(name, "class") or equals(name, "id")) {
            // Menus, dropdowns and similar page furniture (normally laid
            // out or hidden by CSS) recognized by their class names.
            static constexpr char const* CLUTTER[] = {
                "dropdown", "menu", "navbox", "navigation", "sidebar", "editsection", "noprint", "sr-only",
                "visually-hidden", "screen-reader", "cookie", "popup", "modal", "skip-link", "mw-jump",
                "toc", "catlinks", "printfooter", "language", "hatnote-hide"};
            for (u64 k = 0; k < n; k++) {
                for (char const* word : CLUTTER) {
                    u64 len = length(word);
                    if (k + len <= n and startsWith(value + k, word) and (k == 0 or value[k - 1] == ' ' or
                                                                          value[k - 1] == '-' or value[k - 1] == '_'))
                        _hiddenByAttribute = true;
                }
            }
            return;
        }
        if (equals(name, "width") or equals(name, "height")) {
            int v = 0;
            for (u64 k = 0; k < n and value[k] >= '0' and value[k] <= '9'; k++)
                v = v * 10 + (value[k] - '0');
            (equals(name, "width") ? _width : _height) = v;
            return;
        }
        if (equals(name, "data-src")) {
            decode(value, n, _scratch);
            u64 len = _scratch.size() < 1023 ? _scratch.size() : 1023;
            memcpy(_src, _scratch.data(), len);
            _src[len] = '\0';
            _dataSrc = true;
            return;
        }
        if (equals(name, "src") and _dataSrc)
            return;
        char* dest = equals(name, "href") ? _href : equals(name, "alt") ? _alt : equals(name, "value") ? _value
                     : equals(name, "type") ? _type : equals(name, "src") ? _src : nullptr;
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
                image();
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
        if ((flags & (SKIP | HIDDEN)) or _hiddenByAttribute)
            st.hidden = true;
        if (equals(name, "main") and _mainOnly)
            st.hidden = false; // the page's real content
        if (flags & BLOCK) {
            st.indent += e->indent;
            _b.block(e->margin);
        }

        if (name[0] == 'h' and name[1] >= '1' and name[1] <= '6' and not name[2]) {
            st.bold = true;
            st.color = Colors::HEADING;
            st.font = name[1] == '1' ? H1 : name[1] == '2' ? H2 : name[1] == '3' ? H3 : BODY;
        } else if (equals(name, "b") or equals(name, "strong") or equals(name, "th") or equals(name, "dt") or
                   equals(name, "summary") or equals(name, "legend")) {
            st.bold = true;
        } else if (equals(name, "u") or equals(name, "ins")) {
            st.underline = true;
        } else if (equals(name, "code") or equals(name, "kbd") or equals(name, "samp") or equals(name, "tt")) {
            st.color = Colors::CODE;
            st.font = MONO;
        } else if (equals(name, "pre")) {
            st.pre = true;
            st.color = Colors::CODE;
            st.font = MONO;
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

    // An <img>: registered so the browser can load it; shown at its size
    // once known (from its attributes or after loading), else as its alt text.
    void image() {
        char const* src = _src[0] ? _src : nullptr;
        bool usable = src and not startsWith(src, "data:") and not (_width > 0 and _width < 8) and
                      not (_height > 0 and _height < 8);
        if (usable) {
            u64 n = length(src);
            usable = not (n > 4 and equals(src + n - 4, ".svg"));
        }
        if (not usable or _b.style.hidden) {
            if (_alt[0] and not _b.style.hidden)
                imageLabel(_alt);
            return;
        }
        PageImage img{};
        img.src = static_cast<u32>(_doc.text.size());
        _doc.text.append(src, length(src) + 1);
        img.alt = static_cast<u32>(_doc.text.size());
        _doc.text.append(_alt, length(_alt) + 1);
        img.width = _width;
        img.height = _height;
        int index = static_cast<int>(_doc.images.size());
        _doc.images.push(img);

        int naturalW = 0, naturalH = 0;
        bool loaded = _sizes and _sizes(src, naturalW, naturalH) and naturalW > 0 and naturalH > 0;
        int w = _width, h = _height;
        if (loaded) {
            if (w and not h)
                h = naturalH * w / naturalW;
            else if (h and not w)
                w = naturalW * h / naturalH;
            else if (not w and not h) {
                w = naturalW;
                h = naturalH;
            }
        }
        if (w <= 0 or h <= 0) {
            if (_alt[0])
                imageLabel(_alt);
            return;
        }
        int max = _width_limit - _b.style.indent;
        if (w > max and max > 0) { // shrink to fit the page, keeping the shape
            h = h * max / w;
            w = max;
        }
        if (h < 1)
            h = 1;
        _b.picture(index, w, h);
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
    ImageSize _sizes;
    int _width_limit;
    Open _stack[64];
    int _depth = 0;
    Vector<char> _scratch;
    char _href[1024], _alt[1024], _value[1024], _type[1024], _src[1024];
    int _width = 0, _height = 0;
    bool _hiddenByAttribute = false, _dataSrc = false;

public:
    // A page with a <main> element: show only what's inside it (menus and
    // sidebars around it are what CSS would normally move out of the way).
    bool _mainOnly = false;
};

} // namespace

Font const& fontFor(u8 id) {
    switch (id) {
    case BODY_BOLD: return Font::get("sans-bold", 15);
    case MONO: return Font::get("mono", 13);
    case MONO_BOLD: return Font::get("mono-bold", 13);
    case H1: return Font::get("sans-bold", 28);
    case H2: return Font::get("sans-bold", 22);
    case H3: return Font::get("sans-bold", 18);
    default: return Font::get("sans", 15);
    }
}

void layout(char const* source, u64 n, bool plainText, int width, Document& out, ImageSize sizes) {
    out.clear();
    out.text.push('\0'); // offset 0 is never a real run
    auto* parser = new Parser(out, width, sizes);
    if (plainText) {
        parser->plain(source, n);
    } else {
        for (u64 i = 0; i + 6 <= n; i++) {
            if (source[i] == '<' and startsWith(source + i + 1, "main") and
                (source[i + 5] == '>' or isSpace(source[i + 5]))) {
                parser->_mainOnly = true;
                break;
            }
        }
        parser->startHidden();
        parser->html(source, n);
    }
    parser->finish();
    delete parser;
}

} // namespace Browser
