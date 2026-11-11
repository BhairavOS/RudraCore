#pragma once

#include "url.h"
#include "util.h"

#include <kapala/net.h>
#include <kapala/tls.h>

namespace Browser {

struct Response {
    long error = 0;        // a Sys::Error if the request failed, else OK
    int status = 0;        // HTTP status, e.g. 200
    char location[Url::MAX_PATH + 160] = {}; // for redirects
    char contentType[64] = {};
    char security[96] = {}; // e.g. "TLS 1.3, ChaCha20-Poly1305", or why TLS failed
    Vector<char> body;     // the decoded body, with a 0 after it
};

// Called while loading with a short description ("Connecting...").
using Progress = void (*)(char const* message);

// GET over HTTP/1.1, through TLS for https. Follows nothing: redirects
// come back as 3xx.
void fetch(Kapala::Net& net, Url const& url, Response& out, Progress progress);

} // namespace Browser
