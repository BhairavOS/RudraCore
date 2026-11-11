#pragma once

// Kapalaframe: downloading a file over HTTP or HTTPS.
//
//   unsigned char* data; unsigned long length; char const* why;
//   if (httpGet(net, "https://example.com/app.bpk", data, length, why) == Sys::OK) {
//       ... use it ...
//       delete[] data;
//   }
//
// Follows redirects, decodes chunked responses. (Bhairav Browser has its
// own, page-oriented client.)

#include <kapala/net.h>

namespace Kapala {

long httpGet(Net& net, char const* address, unsigned char*& data, unsigned long& length, char const*& error,
             unsigned long limit = 16 * 1024 * 1024);

} // namespace Kapala
