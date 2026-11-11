#pragma once

// Kapalaframe: pictures. Decodes PNG, JPEG (baseline) and GIF (the first
// frame) into RGBA pixels, and scales them.
//
//   Image image;
//   if (Image::decode(bytes, length, image)) {
//       canvas.image({x, y}, image.pixels, image.width, image.height);
//   }
//
// Progressive JPEGs, WebP and SVG aren't supported yet (decode fails).

namespace Kapala {

class Image {
public:
    int width = 0, height = 0;
    unsigned char* pixels = nullptr; // R, G, B, A per pixel, row by row

    Image() = default;
    Image(Image const&) = delete;
    Image& operator=(Image const&) = delete;
    Image(Image&& other) { *this = static_cast<Image&&>(other); }
    Image& operator=(Image&& other);
    ~Image() { delete[] pixels; }

    bool allocate(int w, int h);

    // Decodes any supported format. Refuses images over `maxPixels`.
    static bool decode(unsigned char const* data, unsigned long length, Image& out,
                       unsigned long maxPixels = 16 * 1024 * 1024);

    // A copy at another size (area averaging when shrinking, so it stays smooth).
    bool scaled(int w, int h, Image& out) const;
};

// The format decoders (used by Image::decode).
bool decodePng(unsigned char const* data, unsigned long length, Image& out, unsigned long maxPixels);
bool decodeJpeg(unsigned char const* data, unsigned long length, Image& out, unsigned long maxPixels);
bool decodeGif(unsigned char const* data, unsigned long length, Image& out, unsigned long maxPixels);

// zlib / DEFLATE decompression (RFC 1950/1951). Returns bytes written, or
// -1 if the data is damaged or doesn't fit.
long inflate(unsigned char const* data, unsigned long length, unsigned char* out, unsigned long capacity,
             bool zlibHeader = true);

} // namespace Kapala
