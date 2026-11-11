#include "url.h"

namespace Browser {

namespace {

void stripFragment(char* path) {
    for (char* p = path; *p; p++) {
        if (*p == '#') {
            *p = '\0';
            return;
        }
    }
}

// Removes "." and ".." segments from an absolute path (the query is kept as is).
void normalize(char* path) {
    char query[Url::MAX_PATH] = {};
    for (char* p = path; *p; p++) {
        if (*p == '?') {
            copy(query, p, sizeof(query));
            *p = '\0';
            break;
        }
    }

    // Split into segments, dropping "." and letting ".." remove the one before.
    char const* segments[128];
    u64 lengths[128];
    int count = 0;
    bool directory = false; // ends with a slash
    for (char const* p = path; *p;) {
        while (*p == '/')
            p++;
        char const* start = p;
        while (*p and *p != '/')
            p++;
        u64 len = static_cast<u64>(p - start);
        if (len == 0)
            break;
        directory = *p == '/';
        if (len == 1 and start[0] == '.') {
            directory = true;
        } else if (len == 2 and start[0] == '.' and start[1] == '.') {
            if (count)
                count--;
            directory = true;
        } else if (count < 128) {
            segments[count] = start;
            lengths[count++] = len;
        }
    }

    char out[Url::MAX_PATH];
    u64 n = 0;
    for (int i = 0; i < count; i++) {
        if (n + lengths[i] + 2 >= sizeof(out))
            break;
        out[n++] = '/';
        memcpy(out + n, segments[i], lengths[i]);
        n += lengths[i];
    }
    if (n == 0 or directory)
        out[n++] = '/';
    out[n] = '\0';
    copy(path, out, Url::MAX_PATH);
    append(path, query, Url::MAX_PATH);
}

} // namespace

bool Url::parse(char const* text) {
    while (isSpace(*text))
        text++;
    *this = Url{};

    if (startsWith(text, "about:")) {
        copy(scheme, "about", sizeof(scheme));
        copy(path, text + 6, sizeof(path));
        return path[0] != '\0';
    }

    char const* rest = text;
    for (char const* p = text; *p and *p != '/'; p++) {
        if (p[0] == ':' and p[1] == '/' and p[2] == '/') {
            u64 n = static_cast<u64>(p - text);
            if (n == 0 or n >= sizeof(scheme))
                return false;
            for (u64 i = 0; i < n; i++)
                scheme[i] = lower(text[i]);
            scheme[n] = '\0';
            rest = p + 3;
            break;
        }
    }
    if (not isHttp() and not isHttps())
        return false;
    port = isHttps() ? 443 : 80;

    u64 h = 0;
    while (*rest and *rest != '/' and *rest != '?' and *rest != '#' and *rest != ':' and not isSpace(*rest)) {
        if (h + 1 >= sizeof(host))
            return false;
        host[h++] = lower(*rest++);
    }
    host[h] = '\0';
    if (h == 0)
        return false;
    if (*rest == ':') {
        rest++;
        unsigned value = 0;
        while (*rest >= '0' and *rest <= '9')
            value = value * 10 + static_cast<unsigned>(*rest++ - '0');
        if (value == 0 or value > 65535)
            return false;
        port = static_cast<unsigned short>(value);
    }

    if (*rest == '/') {
        copy(path, rest, sizeof(path));
    } else if (*rest == '?') {
        path[0] = '/';
        copy(path + 1, rest, sizeof(path) - 1);
    } else {
        copy(path, "/", sizeof(path));
    }
    // Spaces typed in an address end it.
    for (char* p = path; *p; p++) {
        if (isSpace(*p)) {
            *p = '\0';
            break;
        }
    }
    stripFragment(path);
    normalize(path);
    return true;
}

bool Url::resolve(Url const& base, char const* href) {
    while (isSpace(*href))
        href++;
    if (not *href or *href == '#')
        return false;

    // An absolute link has a scheme ("http:", "mailto:").
    for (char const* p = href; *p; p++) {
        if (*p == ':') {
            if (startsWith(href, "http://") or startsWith(href, "https://"))
                return parse(href);
            return false; // mailto:, javascript:, tel:...
        }
        bool schemeChar = (*p >= 'a' and *p <= 'z') or (*p >= 'A' and *p <= 'Z') or (*p >= '0' and *p <= '9') or
                          *p == '+' or *p == '-' or *p == '.';
        if (not schemeChar)
            break;
    }

    if (href[0] == '/' and href[1] == '/') {
        char full[Url::MAX_HOST + Url::MAX_PATH + 16];
        copy(full, base.scheme, sizeof(full));
        append(full, ":", sizeof(full));
        append(full, href, sizeof(full));
        return parse(full);
    }

    *this = base;
    if (href[0] == '/') {
        copy(path, href, sizeof(path));
    } else if (href[0] == '?') {
        for (char* p = path; *p; p++) {
            if (*p == '?') {
                *p = '\0';
                break;
            }
        }
        append(path, href, sizeof(path));
    } else {
        // Relative to the base path's directory.
        u64 cut = 0;
        for (u64 i = 0; base.path[i] and base.path[i] != '?'; i++) {
            if (base.path[i] == '/')
                cut = i + 1;
        }
        copy(path, base.path, sizeof(path), cut);
        append(path, href, sizeof(path));
    }
    for (char* p = path; *p; p++) {
        if (*p == ' ' or *p == '\n' or *p == '\r' or *p == '\t') { // stray whitespace in links
            u64 n = length(p);
            memcpy(p, p + 1, n);
            p--;
        }
    }
    stripFragment(path);
    normalize(path);
    return true;
}

void Url::format(char* out, u64 capacity) const {
    if (isAbout()) {
        copy(out, "about:", capacity);
        append(out, path, capacity);
        return;
    }
    copy(out, scheme, capacity);
    append(out, "://", capacity);
    append(out, host, capacity);
    if ((isHttp() and port != 80) or (isHttps() and port != 443)) {
        char digits[8];
        int n = 0;
        for (unsigned v = port; v; v /= 10)
            digits[n++] = static_cast<char>('0' + v % 10);
        char p[8] = {':'};
        for (int i = 0; i < n; i++)
            p[1 + i] = digits[n - 1 - i];
        p[1 + n] = '\0';
        append(out, p, capacity);
    }
    append(out, path, capacity);
}

void encodeQuery(char const* text, char* out, u64 capacity) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    u64 n = 0;
    for (; *text and n + 4 < capacity; text++) {
        unsigned char c = static_cast<unsigned char>(*text);
        bool plain = (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or c == '-' or
                     c == '_' or c == '.' or c == '~';
        if (plain) {
            out[n++] = static_cast<char>(c);
        } else if (c == ' ') {
            out[n++] = '+';
        } else {
            out[n++] = '%';
            out[n++] = HEX[c >> 4];
            out[n++] = HEX[c & 15];
        }
    }
    out[n] = '\0';
}

} // namespace Browser
