#pragma once

#include "util.h"

namespace Browser {

// "http://host:port/path?query" (the #fragment is dropped).
struct Url {
    static constexpr u64 MAX_HOST = 128, MAX_PATH = 1024;

    char scheme[8] = "http"; // "http", "https" or "about"
    char host[MAX_HOST] = {};
    unsigned short port = 80;
    char path[MAX_PATH] = "/"; // with the query

    // An address as typed: "example.com", "http://x/y", "about:home".
    bool parse(char const* text);

    // A link found on the page at `base`. False for links that aren't
    // web pages (mailto:, javascript:...) or only point within the page.
    bool resolve(Url const& base, char const* href);

    bool isHttp() const { return equals(scheme, "http"); }
    bool isHttps() const { return equals(scheme, "https"); }
    bool isAbout() const { return equals(scheme, "about"); }

    void format(char* out, u64 capacity) const;
};

// Percent-encodes `text` for a query string (spaces become +).
void encodeQuery(char const* text, char* out, u64 capacity);

} // namespace Browser
