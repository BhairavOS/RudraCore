#include <kapala/image.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memset(void* dest, int value, unsigned long n);
}

namespace Kapala {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
using u64 = unsigned long;

// MARK: Image

Image& Image::operator=(Image&& other) {
    if (this != &other) {
        delete[] pixels;
        width = other.width;
        height = other.height;
        pixels = other.pixels;
        other.pixels = nullptr;
        other.width = other.height = 0;
    }
    return *this;
}

bool Image::allocate(int w, int h) {
    delete[] pixels;
    pixels = nullptr;
    width = height = 0;
    if (w <= 0 or h <= 0)
        return false;
    pixels = new u8[static_cast<u64>(w) * static_cast<u64>(h) * 4];
    if (not pixels)
        return false;
    width = w;
    height = h;
    return true;
}

bool Image::decode(u8 const* data, u64 length, Image& out, u64 maxPixels) {
    if (length >= 8 and data[0] == 0x89 and data[1] == 'P' and data[2] == 'N' and data[3] == 'G')
        return decodePng(data, length, out, maxPixels);
    if (length >= 3 and data[0] == 0xFF and data[1] == 0xD8 and data[2] == 0xFF)
        return decodeJpeg(data, length, out, maxPixels);
    if (length >= 6 and data[0] == 'G' and data[1] == 'I' and data[2] == 'F')
        return decodeGif(data, length, out, maxPixels);
    return false;
}

bool Image::scaled(int w, int h, Image& out) const {
    if (not pixels or not out.allocate(w, h))
        return false;
    for (int y = 0; y < h; y++) {
        // The source rows (and columns) this pixel covers.
        int y0 = static_cast<int>(static_cast<long>(y) * height / h);
        int y1 = static_cast<int>(static_cast<long>(y + 1) * height / h);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = static_cast<int>(static_cast<long>(x) * width / w);
            int x1 = static_cast<int>(static_cast<long>(x + 1) * width / w);
            if (x1 <= x0)
                x1 = x0 + 1;
            u32 r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1 and sy < height; sy++) {
                u8 const* p = pixels + (static_cast<u64>(sy) * width + x0) * 4;
                for (int sx = x0; sx < x1 and sx < width; sx++, p += 4) {
                    // Weighted by alpha, so transparent pixels don't darken edges.
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    n++;
                }
            }
            u8* o = out.pixels + (static_cast<u64>(y) * w + x) * 4;
            o[0] = static_cast<u8>(a ? r / a : 0);
            o[1] = static_cast<u8>(a ? g / a : 0);
            o[2] = static_cast<u8>(a ? b / a : 0);
            o[3] = static_cast<u8>(n ? a / n : 0);
        }
    }
    return true;
}

// MARK: Inflate (after Mark Adler's puff, zlib license)

namespace {

struct BitReader {
    u8 const* data;
    u64 length, at = 0;
    u32 buffer = 0;
    int count = 0;
    bool error = false;

    int bits(int need) {
        long value = buffer;
        while (count < need) {
            if (at >= length) {
                error = true;
                return 0;
            }
            value |= static_cast<long>(data[at++]) << count;
            count += 8;
        }
        buffer = static_cast<u32>(value >> need);
        count -= need;
        return static_cast<int>(value & ((1L << need) - 1));
    }
};

struct Huffman {
    u16 count[16];
    u16 symbol[320];
};

int decodeSymbol(BitReader& in, Huffman const& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= in.bits(1);
        int count = h.count[len];
        if (code - count < first)
            return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (in.error)
            return -1;
    }
    return -1;
}

// Builds a canonical code; false if the lengths over-subscribe it.
bool build(Huffman& h, u16 const* lengths, int n) {
    for (int len = 0; len < 16; len++)
        h.count[len] = 0;
    for (int s = 0; s < n; s++)
        h.count[lengths[s]]++;
    if (h.count[0] == n)
        return true; // no codes: fine as long as it isn't used
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0)
            return false;
    }
    u16 offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; len++)
        offsets[len + 1] = static_cast<u16>(offsets[len] + h.count[len]);
    for (int s = 0; s < n; s++) {
        if (lengths[s])
            h.symbol[offsets[lengths[s]]++] = static_cast<u16>(s);
    }
    return true;
}

constexpr u16 LENGTH_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u16 LENGTH_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u16 DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateCodes(BitReader& in, Huffman const& lengths, Huffman const& distances, u8* out, u64 capacity,
                  u64& at) {
    for (;;) {
        int symbol = decodeSymbol(in, lengths);
        if (symbol < 0 or in.error)
            return false;
        if (symbol < 256) {
            if (at >= capacity)
                return false;
            out[at++] = static_cast<u8>(symbol);
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29)
                return false;
            u64 len = LENGTH_BASE[symbol] + static_cast<u64>(in.bits(LENGTH_EXTRA[symbol]));
            int d = decodeSymbol(in, distances);
            if (d < 0 or d >= 30)
                return false;
            u64 dist = DIST_BASE[d] + static_cast<u64>(in.bits(DIST_EXTRA[d]));
            if (dist > at or at + len > capacity or in.error)
                return false;
            for (u64 i = 0; i < len; i++, at++)
                out[at] = out[at - dist];
        }
    }
}

} // namespace

long inflate(u8 const* data, u64 length, u8* out, u64 capacity, bool zlibHeader) {
    if (zlibHeader) {
        if (length < 2 or (data[0] & 0x0F) != 8 or ((data[0] << 8) | data[1]) % 31 != 0 or (data[1] & 0x20))
            return -1;
        data += 2;
        length -= 2;
    }
    BitReader in{data, length};
    u64 at = 0;
    static Huffman fixedLengths, fixedDistances;
    static bool fixedReady = false;
    if (not fixedReady) {
        u16 lengths[288];
        for (int s = 0; s < 288; s++)
            lengths[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
        build(fixedLengths, lengths, 288);
        for (int s = 0; s < 30; s++)
            lengths[s] = 5;
        build(fixedDistances, lengths, 30);
        fixedReady = true;
    }

    int last;
    do {
        last = in.bits(1);
        int type = in.bits(2);
        if (in.error)
            return -1;
        if (type == 0) { // stored
            in.buffer = 0;
            in.count = 0;
            if (in.at + 4 > in.length)
                return -1;
            u64 len = data[in.at] | data[in.at + 1] << 8;
            u64 nlen = data[in.at + 2] | data[in.at + 3] << 8;
            in.at += 4;
            if (len != (~nlen & 0xFFFF) or in.at + len > in.length or at + len > capacity)
                return -1;
            memcpy(out + at, data + in.at, len);
            in.at += len;
            at += len;
        } else if (type == 1) {
            if (not inflateCodes(in, fixedLengths, fixedDistances, out, capacity, at))
                return -1;
        } else if (type == 2) {
            int nlen = in.bits(5) + 257, ndist = in.bits(5) + 1, ncode = in.bits(4) + 4;
            if (nlen > 286 or ndist > 30)
                return -1;
            static constexpr u8 ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            u16 lengths[320] = {};
            for (int i = 0; i < ncode; i++)
                lengths[ORDER[i]] = static_cast<u16>(in.bits(3));
            Huffman codeLengths;
            if (not build(codeLengths, lengths, 19))
                return -1;
            int i = 0;
            while (i < nlen + ndist) {
                int symbol = decodeSymbol(in, codeLengths);
                if (symbol < 0)
                    return -1;
                if (symbol < 16) {
                    lengths[i++] = static_cast<u16>(symbol);
                } else {
                    u16 repeat = 0;
                    int times;
                    if (symbol == 16) {
                        if (i == 0)
                            return -1;
                        repeat = lengths[i - 1];
                        times = 3 + in.bits(2);
                    } else if (symbol == 17) {
                        times = 3 + in.bits(3);
                    } else {
                        times = 11 + in.bits(7);
                    }
                    if (i + times > nlen + ndist)
                        return -1;
                    while (times--)
                        lengths[i++] = repeat;
                }
            }
            if (lengths[256] == 0)
                return -1;
            Huffman litLengths, distances;
            if (not build(litLengths, lengths, nlen) or not build(distances, lengths + nlen, ndist))
                return -1;
            if (not inflateCodes(in, litLengths, distances, out, capacity, at))
                return -1;
        } else {
            return -1;
        }
    } while (not last);
    return static_cast<long>(at);
}

// MARK: PNG

namespace {

u32 be32(u8 const* p) { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }

int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb and pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

// Undoes the per-row filters of one (sub)image in place.
bool unfilter(u8* data, u64 rows, u64 rowBytes, u64 bpp) {
    u8* prior = nullptr;
    for (u64 y = 0; y < rows; y++) {
        u8* row = data + y * (rowBytes + 1);
        u8 type = row[0];
        u8* p = row + 1;
        for (u64 i = 0; i < rowBytes; i++) {
            int a = i >= bpp ? p[i - bpp] : 0;
            int b = prior ? prior[i] : 0;
            int c = prior and i >= bpp ? prior[i - bpp] : 0;
            switch (type) {
            case 0: break;
            case 1: p[i] = static_cast<u8>(p[i] + a); break;
            case 2: p[i] = static_cast<u8>(p[i] + b); break;
            case 3: p[i] = static_cast<u8>(p[i] + (a + b) / 2); break;
            case 4: p[i] = static_cast<u8>(p[i] + paeth(a, b, c)); break;
            default: return false;
            }
        }
        prior = p;
    }
    return true;
}

struct PngInfo {
    u32 width, height;
    int depth, colorType, channels;
    u8 palette[256][4];
    int paletteSize;
    bool hasKey;
    u16 keyGray, keyR, keyG, keyB;
};

// One sample (0-65535 scaled to 0-255 later) of pixel x in an unfiltered row.
u32 sample(u8 const* row, u64 index, int depth) {
    switch (depth) {
    case 16: return u32{row[index * 2]} << 8 | row[index * 2 + 1];
    case 8: return row[index];
    default: {
        u64 bit = index * static_cast<u64>(depth);
        return (row[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & ((1u << depth) - 1);
    }
    }
}

u8 to8(u32 v, int depth) {
    if (depth == 16)
        return static_cast<u8>((v * 255 + 32767) / 65535); // rounded, not truncated
    if (depth == 8)
        return static_cast<u8>(v);
    return static_cast<u8>(v * 255 / ((1u << depth) - 1));
}

void writePixel(PngInfo const& info, u8 const* row, u64 x, u8* out) {
    int d = info.depth;
    switch (info.colorType) {
    case 0: { // gray
        u32 g = sample(row, x, d);
        u8 v = to8(g, d);
        out[0] = out[1] = out[2] = v;
        out[3] = info.hasKey and g == info.keyGray ? 0 : 255;
        break;
    }
    case 2: { // RGB
        u32 r = sample(row, x * 3, d), g = sample(row, x * 3 + 1, d), b = sample(row, x * 3 + 2, d);
        out[0] = to8(r, d);
        out[1] = to8(g, d);
        out[2] = to8(b, d);
        out[3] = info.hasKey and r == info.keyR and g == info.keyG and b == info.keyB ? 0 : 255;
        break;
    }
    case 3: { // palette
        u32 i = sample(row, x, d);
        u8 const* c = info.palette[i < 256 ? i : 0];
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        out[3] = c[3];
        break;
    }
    case 4: // gray + alpha
        out[0] = out[1] = out[2] = to8(sample(row, x * 2, d), d);
        out[3] = to8(sample(row, x * 2 + 1, d), d);
        break;
    default: // RGBA
        for (int k = 0; k < 4; k++)
            out[k] = to8(sample(row, x * 4 + static_cast<u64>(k), d), d);
        break;
    }
}

} // namespace

bool decodePng(u8 const* data, u64 length, Image& out, u64 maxPixels) {
    static constexpr u8 SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; i++)
        if (length < 8 or data[i] != SIGNATURE[i])
            return false;

    PngInfo info{};
    int interlace = 0;
    u64 idatTotal = 0;
    for (int k = 0; k < 256; k++)
        info.palette[k][3] = 255;

    // First pass: header, palette, transparency, and the size of the data.
    for (u64 at = 8; at + 12 <= length;) {
        u32 len = be32(data + at);
        u8 const* type = data + at + 4;
        u8 const* body = data + at + 8;
        if (at + 12 + len > length)
            return false;
        if (type[0] == 'I' and type[1] == 'H' and type[2] == 'D' and type[3] == 'R' and len >= 13) {
            info.width = be32(body);
            info.height = be32(body + 4);
            info.depth = body[8];
            info.colorType = body[9];
            interlace = body[12];
            if (body[10] != 0 or body[11] != 0)
                return false;
        } else if (type[0] == 'P' and type[1] == 'L' and type[2] == 'T' and type[3] == 'E') {
            info.paletteSize = static_cast<int>(len / 3 < 256 ? len / 3 : 256);
            for (int k = 0; k < info.paletteSize; k++)
                for (int c = 0; c < 3; c++)
                    info.palette[k][c] = body[k * 3 + c];
        } else if (type[0] == 't' and type[1] == 'R' and type[2] == 'N' and type[3] == 'S') {
            if (info.colorType == 3) {
                for (u32 k = 0; k < len and k < 256; k++)
                    info.palette[k][3] = body[k];
            } else if (info.colorType == 0 and len >= 2) {
                info.hasKey = true;
                info.keyGray = static_cast<u16>(body[0] << 8 | body[1]);
            } else if (info.colorType == 2 and len >= 6) {
                info.hasKey = true;
                info.keyR = static_cast<u16>(body[0] << 8 | body[1]);
                info.keyG = static_cast<u16>(body[2] << 8 | body[3]);
                info.keyB = static_cast<u16>(body[4] << 8 | body[5]);
            }
        } else if (type[0] == 'I' and type[1] == 'D' and type[2] == 'A' and type[3] == 'T') {
            idatTotal += len;
        } else if (type[0] == 'I' and type[1] == 'E' and type[2] == 'N' and type[3] == 'D') {
            break;
        }
        at += 12 + len;
    }

    int channels = info.colorType == 0 ? 1 : info.colorType == 2 ? 3 : info.colorType == 3 ? 1
                   : info.colorType == 4 ? 2 : info.colorType == 6 ? 4 : 0;
    if (not channels or info.width == 0 or info.height == 0 or idatTotal == 0)
        return false;
    if (static_cast<u64>(info.width) * info.height > maxPixels or info.width > 30000 or info.height > 30000)
        return false;
    int d = info.depth;
    if (d != 1 and d != 2 and d != 4 and d != 8 and d != 16)
        return false;
    info.channels = channels;

    // Gather the compressed data.
    u8* compressed = new u8[idatTotal];
    if (not compressed)
        return false;
    u64 got = 0;
    for (u64 at = 8; at + 12 <= length;) {
        u32 len = be32(data + at);
        u8 const* type = data + at + 4;
        if (at + 12 + len > length)
            break;
        if (type[0] == 'I' and type[1] == 'D' and type[2] == 'A' and type[3] == 'T') {
            memcpy(compressed + got, data + at + 8, len);
            got += len;
        }
        at += 12 + len;
    }

    u64 bitsPerPixel = static_cast<u64>(channels) * static_cast<u64>(d);
    u64 bpp = bitsPerPixel >= 8 ? bitsPerPixel / 8 : 1;
    auto rowBytes = [&](u64 w) { return (w * bitsPerPixel + 7) / 8; };

    // Adam7 passes (or one pass covering everything).
    static constexpr int PASS_X[7] = {0, 4, 0, 2, 0, 1, 0}, PASS_Y[7] = {0, 0, 4, 0, 2, 0, 1};
    static constexpr int STEP_X[7] = {8, 8, 4, 4, 2, 2, 1}, STEP_Y[7] = {8, 8, 8, 4, 4, 2, 2};
    int passes = interlace ? 7 : 1;
    u64 rawSize = 0;
    for (int p = 0; p < passes; p++) {
        u64 pw = interlace ? (info.width + STEP_X[p] - 1 - PASS_X[p]) / STEP_X[p] : info.width;
        u64 ph = interlace ? (info.height + STEP_Y[p] - 1 - PASS_Y[p]) / STEP_Y[p] : info.height;
        if (pw and ph)
            rawSize += ph * (rowBytes(pw) + 1);
    }
    u8* raw = new u8[rawSize];
    bool ok = raw and inflate(compressed, idatTotal, raw, rawSize) == static_cast<long>(rawSize);
    delete[] compressed;
    if (not ok or not out.allocate(static_cast<int>(info.width), static_cast<int>(info.height))) {
        delete[] raw;
        return false;
    }

    u8* pass = raw;
    for (int p = 0; p < passes and ok; p++) {
        u64 pw = interlace ? (info.width + STEP_X[p] - 1 - PASS_X[p]) / STEP_X[p] : info.width;
        u64 ph = interlace ? (info.height + STEP_Y[p] - 1 - PASS_Y[p]) / STEP_Y[p] : info.height;
        if (not pw or not ph)
            continue;
        u64 rb = rowBytes(pw);
        ok = unfilter(pass, ph, rb, bpp);
        for (u64 y = 0; ok and y < ph; y++) {
            u8 const* row = pass + y * (rb + 1) + 1;
            u64 oy = interlace ? PASS_Y[p] + y * STEP_Y[p] : y;
            for (u64 x = 0; x < pw; x++) {
                u64 ox = interlace ? PASS_X[p] + x * STEP_X[p] : x;
                writePixel(info, row, x, out.pixels + (oy * info.width + ox) * 4);
            }
        }
        pass += ph * (rb + 1);
    }
    delete[] raw;
    return ok;
}

// MARK: JPEG (baseline)

namespace {

constexpr u8 ZIGZAG[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                           12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                           35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                           58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct JpegHuffman {
    u8 symbols[256];
    int maxCode[18];
    int valPtr[17];
    u16 minCode[17];
    bool present = false;
};

void buildJpegHuffman(JpegHuffman& h, u8 const counts[16], u8 const* symbols, int total) {
    for (int i = 0; i < total and i < 256; i++)
        h.symbols[i] = symbols[i];
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h.valPtr[len] = k;
        h.minCode[len] = static_cast<u16>(code);
        code += counts[len - 1];
        k += counts[len - 1];
        h.maxCode[len] = counts[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h.maxCode[17] = 0x7FFFFFFF;
    h.present = true;
}

struct JpegBits {
    u8 const* data;
    u64 length, at;
    u32 buffer = 0;
    int count = 0;
    bool marker = false; // hit a marker: only zeros follow

    void fill() {
        while (count <= 24) {
            u32 byte = 0;
            if (not marker and at < length) {
                byte = data[at];
                if (byte == 0xFF) {
                    u8 next = at + 1 < length ? data[at + 1] : 0;
                    if (next == 0x00) {
                        at += 2;
                    } else {
                        marker = true;
                        byte = 0;
                    }
                } else {
                    at++;
                }
            }
            buffer |= byte << (24 - count);
            count += 8;
        }
    }

    int bit() {
        if (count == 0)
            fill();
        int b = static_cast<int>(buffer >> 31);
        buffer <<= 1;
        count--;
        return b;
    }

    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; i++)
            v = v << 1 | bit();
        return v;
    }

    void reset() { // at a restart marker
        buffer = 0;
        count = 0;
        marker = false;
        while (at + 1 < length and not (data[at] == 0xFF and data[at + 1] >= 0xD0 and data[at + 1] <= 0xD7))
            at++;
        if (at + 1 < length)
            at += 2;
    }
};

int decodeJpegSymbol(JpegBits& in, JpegHuffman const& h) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = code << 1 | in.bit();
        if (h.maxCode[len] >= 0 and code <= h.maxCode[len])
            return h.symbols[h.valPtr[len] + code - h.minCode[len]];
    }
    return -1;
}

int extend(int v, int n) { return n == 0 ? 0 : v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

// cos(x) for the IDCT table (programs have no math library): reduce to
// [-pi, pi], then a Taylor series (plenty accurate there).
float cosine(float x) {
    constexpr float PI = 3.14159265358979f;
    while (x > PI)
        x -= 2 * PI;
    while (x < -PI)
        x += 2 * PI;
    float x2 = x * x, term = 1, sum = 1;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / static_cast<float>((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

// Inverse DCT of one block (float, separable), adding 128 and clamping.
void idct(float const in[64], u8 out[64]) {
    static float table[8][8];
    static bool ready = false;
    if (not ready) {
        for (int x = 0; x < 8; x++) {
            for (int u = 0; u < 8; u++) {
                float c = u == 0 ? 0.70710678f : 1.0f;
                table[x][u] = c * cosine(static_cast<float>((2 * x + 1) * u) * 3.14159265358979f / 16) / 2;
            }
        }
        ready = true;
    }
    float tmp[64];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++)
                s += table[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++)
                s += table[y][v] * tmp[v * 8 + x];
            int value = static_cast<int>(s + 128.5f);
            out[y * 8 + x] = static_cast<u8>(value < 0 ? 0 : value > 255 ? 255 : value);
        }
    }
}

struct Component {
    int id, h, v, quant, dcTable, acTable;
    int dcPredictor;
    u8* plane; // decoded samples, blocksWide * 8 by blocksHigh * 8
    int stride;
};

u8 clamp(float v) { return static_cast<u8>(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }

} // namespace

bool decodeJpeg(u8 const* data, u64 length, Image& out, u64 maxPixels) {
    u16 quant[4][64] = {};
    static JpegHuffman dc[4], ac[4];
    for (int i = 0; i < 4; i++)
        dc[i].present = ac[i].present = false;
    Component comps[4] = {};
    int componentCount = 0, width = 0, height = 0, restartInterval = 0;
    bool frame = false, adobeRgb = false;

    u64 at = 2;
    while (at + 4 <= length) {
        if (data[at] != 0xFF) {
            at++;
            continue;
        }
        u8 marker = data[at + 1];
        if (marker == 0xFF) {
            at++;
            continue;
        }
        if (marker == 0xD8 or (marker >= 0xD0 and marker <= 0xD7)) {
            at += 2;
            continue;
        }
        if (marker == 0xD9)
            break;
        u64 len = static_cast<u64>(data[at + 2] << 8 | data[at + 3]);
        u8 const* seg = data + at + 4;
        if (at + 2 + len > length or len < 2)
            return false;
        u64 segLength = len - 2;

        if (marker == 0xDB) { // quantization tables
            for (u64 p = 0; p < segLength;) {
                int precision = seg[p] >> 4, id = seg[p] & 3;
                p++;
                for (int k = 0; k < 64; k++) {
                    quant[id][k] = precision ? static_cast<u16>(seg[p] << 8 | seg[p + 1]) : seg[p];
                    p += precision ? 2 : 1;
                }
            }
        } else if (marker == 0xC4) { // Huffman tables
            for (u64 p = 0; p + 17 <= segLength;) {
                int tableClass = seg[p] >> 4, id = seg[p] & 3;
                u8 const* counts = seg + p + 1;
                int total = 0;
                for (int i = 0; i < 16; i++)
                    total += counts[i];
                if (p + 17 + static_cast<u64>(total) > segLength)
                    return false;
                buildJpegHuffman(tableClass ? ac[id] : dc[id], counts, seg + p + 17, total);
                p += 17 + static_cast<u64>(total);
            }
        } else if (marker == 0xC0 or marker == 0xC1) { // baseline / extended sequential
            if (seg[0] != 8)
                return false;
            height = seg[1] << 8 | seg[2];
            width = seg[3] << 8 | seg[4];
            componentCount = seg[5];
            if (componentCount != 1 and componentCount != 3)
                return false;
            for (int i = 0; i < componentCount; i++) {
                comps[i].id = seg[6 + i * 3];
                comps[i].h = seg[7 + i * 3] >> 4;
                comps[i].v = seg[7 + i * 3] & 15;
                comps[i].quant = seg[8 + i * 3] & 3;
                if (comps[i].h < 1 or comps[i].h > 4 or comps[i].v < 1 or comps[i].v > 4)
                    return false;
            }
            frame = true;
        } else if (marker >= 0xC2 and marker <= 0xCF and marker != 0xC4 and marker != 0xC8 and marker != 0xCC) {
            return false; // progressive, lossless or arithmetic: not supported
        } else if (marker == 0xDD) {
            restartInterval = seg[0] << 8 | seg[1];
        } else if (marker == 0xEE and segLength >= 12 and seg[0] == 'A' and seg[1] == 'd') {
            adobeRgb = seg[11] == 0 and componentCount == 3;
        } else if (marker == 0xDA) { // start of scan: the image data follows
            if (not frame or width == 0 or height == 0)
                return false;
            if (static_cast<u64>(width) * static_cast<u64>(height) > maxPixels)
                return false;
            int scanCount = seg[0];
            if (scanCount != componentCount)
                return false; // (non-interleaved scans: not supported)
            for (int i = 0; i < scanCount; i++) {
                int id = seg[1 + i * 2];
                for (int c = 0; c < componentCount; c++) {
                    if (comps[c].id == id) {
                        comps[c].dcTable = seg[2 + i * 2] >> 4;
                        comps[c].acTable = seg[2 + i * 2] & 3;
                    }
                }
            }
            at += 2 + len;
            break;
        }
        at += 2 + len;
    }
    if (not frame or at >= length)
        return false;

    int hMax = 1, vMax = 1;
    for (int c = 0; c < componentCount; c++) {
        hMax = comps[c].h > hMax ? comps[c].h : hMax;
        vMax = comps[c].v > vMax ? comps[c].v : vMax;
    }
    int mcuW = 8 * hMax, mcuH = 8 * vMax;
    int mcusX = (width + mcuW - 1) / mcuW, mcusY = (height + mcuH - 1) / mcuH;
    for (int c = 0; c < componentCount; c++) {
        if (not dc[comps[c].dcTable].present or not ac[comps[c].acTable].present)
            return false;
        comps[c].stride = mcusX * comps[c].h * 8;
        u64 size = static_cast<u64>(comps[c].stride) * static_cast<u64>(mcusY * comps[c].v * 8);
        comps[c].plane = new u8[size];
        if (not comps[c].plane) {
            for (int k = 0; k < c; k++)
                delete[] comps[k].plane;
            return false;
        }
    }

    JpegBits in{data, length, at};
    float block[64];
    u8 samples[64];
    int mcusLeft = restartInterval;
    bool ok = true;
    for (int my = 0; my < mcusY and ok; my++) {
        for (int mx = 0; mx < mcusX and ok; mx++) {
            if (restartInterval) {
                if (mcusLeft == 0) {
                    in.reset();
                    for (int c = 0; c < componentCount; c++)
                        comps[c].dcPredictor = 0;
                    mcusLeft = restartInterval;
                }
                mcusLeft--;
            }
            for (int c = 0; c < componentCount and ok; c++) {
                Component& comp = comps[c];
                for (int by = 0; by < comp.v and ok; by++) {
                    for (int bx = 0; bx < comp.h; bx++) {
                        for (float& f : block)
                            f = 0;
                        int t = decodeJpegSymbol(in, dc[comp.dcTable]);
                        if (t < 0 or t > 11) {
                            ok = false;
                            break;
                        }
                        comp.dcPredictor += extend(in.bits(t), t);
                        block[0] = static_cast<float>(comp.dcPredictor * quant[comp.quant][0]);
                        for (int k = 1; k < 64;) {
                            int rs = decodeJpegSymbol(in, ac[comp.acTable]);
                            if (rs < 0) {
                                ok = false;
                                break;
                            }
                            int r = rs >> 4, s = rs & 15;
                            if (s == 0) {
                                if (r != 15)
                                    break; // end of block
                                k += 16;
                                continue;
                            }
                            k += r;
                            if (k > 63) {
                                ok = false;
                                break;
                            }
                            block[ZIGZAG[k]] = static_cast<float>(extend(in.bits(s), s) * quant[comp.quant][k]);
                            k++;
                        }
                        if (not ok)
                            break;
                        idct(block, samples);
                        int x0 = (mx * comp.h + bx) * 8, y0 = (my * comp.v + by) * 8;
                        for (int y = 0; y < 8; y++)
                            memcpy(comp.plane + static_cast<u64>(y0 + y) * comp.stride + x0, samples + y * 8, 8);
                    }
                }
            }
        }
    }

    if (ok)
        ok = out.allocate(width, height);
    if (ok) {
        for (int y = 0; y < height; y++) {
            u8* o = out.pixels + static_cast<u64>(y) * width * 4;
            for (int x = 0; x < width; x++, o += 4) {
                // Each component's sample for this pixel (upsampled by repetition).
                float s[3];
                for (int c = 0; c < componentCount; c++) {
                    int sx = x * comps[c].h / hMax, sy = y * comps[c].v / vMax;
                    s[c] = comps[c].plane[static_cast<u64>(sy) * comps[c].stride + sx];
                }
                if (componentCount == 1) {
                    o[0] = o[1] = o[2] = static_cast<u8>(s[0]);
                } else if (adobeRgb) {
                    o[0] = static_cast<u8>(s[0]);
                    o[1] = static_cast<u8>(s[1]);
                    o[2] = static_cast<u8>(s[2]);
                } else {
                    float Y = s[0], cb = s[1] - 128, cr = s[2] - 128;
                    o[0] = clamp(Y + 1.402f * cr);
                    o[1] = clamp(Y - 0.344136f * cb - 0.714136f * cr);
                    o[2] = clamp(Y + 1.772f * cb);
                }
                o[3] = 255;
            }
        }
    }
    for (int c = 0; c < componentCount; c++)
        delete[] comps[c].plane;
    return ok;
}

// MARK: GIF (first frame)

bool decodeGif(u8 const* data, u64 length, Image& out, u64 maxPixels) {
    if (length < 13)
        return false;
    int width = data[6] | data[7] << 8, height = data[8] | data[9] << 8;
    if (width == 0 or height == 0 or static_cast<u64>(width) * static_cast<u64>(height) > maxPixels)
        return false;
    u8 flags = data[10];
    u8 global[256][3] = {};
    u64 at = 13;
    if (flags & 0x80) {
        int size = 2 << (flags & 7);
        if (at + static_cast<u64>(size) * 3 > length)
            return false;
        memcpy(global, data + at, static_cast<u64>(size) * 3);
        at += static_cast<u64>(size) * 3;
    }
    if (not out.allocate(width, height))
        return false;
    memset(out.pixels, 0, static_cast<u64>(width) * height * 4);
    int transparent = -1;

    while (at < length) {
        u8 kind = data[at++];
        if (kind == 0x3B)
            break;
        if (kind == 0x21) { // extension
            if (at >= length)
                return false;
            u8 label = data[at++];
            if (label == 0xF9 and at + 5 <= length and data[at] == 4) {
                if (data[at + 1] & 1)
                    transparent = data[at + 4];
            }
            while (at < length and data[at]) // its sub-blocks
                at += 1 + data[at];
            at++;
            continue;
        }
        if (kind != 0x2C)
            return false;
        // The image descriptor.
        if (at + 9 > length)
            return false;
        int left = data[at] | data[at + 1] << 8, top = data[at + 2] | data[at + 3] << 8;
        int w = data[at + 4] | data[at + 5] << 8, h = data[at + 6] | data[at + 7] << 8;
        u8 imageFlags = data[at + 8];
        at += 9;
        u8 local[256][3];
        u8 (*palette)[3] = global;
        if (imageFlags & 0x80) {
            int size = 2 << (imageFlags & 7);
            if (at + static_cast<u64>(size) * 3 > length)
                return false;
            memcpy(local, data + at, static_cast<u64>(size) * 3);
            at += static_cast<u64>(size) * 3;
            palette = local;
        }
        bool interlaced = imageFlags & 0x40;
        if (at >= length)
            return false;
        int minCode = data[at++];
        if (minCode < 2 or minCode > 8)
            return false;

        // Gather the data sub-blocks.
        u64 total = 0;
        for (u64 p = at; p < length and data[p]; p += 1 + data[p])
            total += data[p];
        u8* lzw = new u8[total + 1];
        if (not lzw)
            return false;
        u64 got = 0;
        while (at < length and data[at]) {
            u64 n = data[at];
            if (at + 1 + n > length)
                break;
            memcpy(lzw + got, data + at + 1, n);
            got += n;
            at += 1 + n;
        }
        at++;

        // LZW decoding.
        u64 pixelsTotal = static_cast<u64>(w) * static_cast<u64>(h);
        u8* indices = new u8[pixelsTotal ? pixelsTotal : 1];
        static u16 prefix[4096];
        static u8 suffix[4096];
        static u8 stack[4097];
        int clear = 1 << minCode, end = clear + 1;
        int codeSize = minCode + 1, next = clear + 2, old = -1;
        u8 first = 0;
        u64 bitPos = 0, produced = 0;
        for (int i = 0; i < clear; i++) {
            prefix[i] = 0xFFFF;
            suffix[i] = static_cast<u8>(i);
        }
        while (produced < pixelsTotal and bitPos + static_cast<u64>(codeSize) <= got * 8) {
            int code = 0;
            for (int b = 0; b < codeSize; b++, bitPos++)
                code |= ((lzw[bitPos / 8] >> (bitPos % 8)) & 1) << b;
            if (code == clear) {
                codeSize = minCode + 1;
                next = clear + 2;
                old = -1;
                continue;
            }
            if (code == end)
                break;
            int sp = 0, current = code;
            if (old < 0) {
                if (code >= clear)
                    break;
                indices[produced++] = static_cast<u8>(code);
                first = static_cast<u8>(code);
                old = code;
                continue;
            }
            if (code >= next) { // the code being defined right now
                stack[sp++] = first;
                current = old;
            }
            while (current >= clear and sp < 4096) {
                stack[sp++] = suffix[current];
                current = prefix[current];
            }
            stack[sp++] = static_cast<u8>(current);
            first = static_cast<u8>(current);
            while (sp > 0 and produced < pixelsTotal)
                indices[produced++] = stack[--sp];
            if (next < 4096) {
                prefix[next] = static_cast<u16>(old);
                suffix[next] = first;
                next++;
                if (next == (1 << codeSize) and codeSize < 12)
                    codeSize++;
            }
            old = code;
        }
        delete[] lzw;

        // Paint the frame (rows reordered if interlaced).
        int row = 0, pass = 0;
        static constexpr int START[4] = {0, 4, 2, 1}, STEP[4] = {8, 8, 4, 2};
        for (int y = 0; y < h; y++) {
            int dy = y;
            if (interlaced) {
                dy = row;
                row += STEP[pass];
                while (row >= h and pass < 3)
                    row = START[++pass];
            }
            for (int x = 0; x < w; x++) {
                u64 i = static_cast<u64>(y) * w + x;
                int ox = left + x, oy = top + dy;
                if (i >= produced or ox >= width or oy >= height)
                    continue;
                u8 index = indices[i];
                if (index == transparent)
                    continue;
                u8* o = out.pixels + (static_cast<u64>(oy) * width + ox) * 4;
                o[0] = palette[index][0];
                o[1] = palette[index][1];
                o[2] = palette[index][2];
                o[3] = 255;
            }
        }
        delete[] indices;
        return true; // the first frame is enough
    }
    return false;
}

} // namespace Kapala
