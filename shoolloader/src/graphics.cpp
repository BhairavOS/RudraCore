#include "loader.h"

namespace Loader {

namespace {

constexpr uint32_t DEFAULT_WIDTH = 1280;
constexpr uint32_t DEFAULT_HEIGHT = 800;

uint8_t lowestBit(uint32_t mask) {
    uint8_t shift = 0;
    while (mask and not(mask & 1)) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

bool usable(Efi::GraphicsModeInfo const& m) {
    return m.pixelFormat == Efi::PIXEL_RGB_RESERVED_8BIT or m.pixelFormat == Efi::PIXEL_BGR_RESERVED_8BIT or
           m.pixelFormat == Efi::PIXEL_BIT_MASK;
}

// The configured resolution if the screen has it, else 1280x800, else the
// largest mode up to 1920x1080, else whatever is active.
uint32_t chooseMode(Efi::GraphicsOutput* gop, Config const& config) {
    uint32_t wantW = config.width ? config.width : DEFAULT_WIDTH;
    uint32_t wantH = config.height ? config.height : DEFAULT_HEIGHT;

    uint32_t best = gop->mode->mode;
    uint64_t bestArea = 0;
    for (uint32_t i = 0; i < gop->mode->maxMode; i++) {
        uint64_t infoSize = 0;
        Efi::GraphicsModeInfo* info = nullptr;
        if (Efi::failed(gop->queryMode(gop, i, &infoSize, &info)) or not usable(*info))
            continue;

        uint32_t w = info->horizontalResolution, h = info->verticalResolution;
        if (w == wantW and h == wantH)
            return i;
        uint64_t area = uint64_t(w) * h;
        if (w <= 1920 and h <= 1080 and area > bestArea) {
            best = i;
            bestArea = area;
        }
    }
    return best;
}

struct Painter {
    Shool::Framebuffer const& fb;

    uint32_t pack(uint8_t r, uint8_t g, uint8_t b) const {
        return (uint32_t(r) << fb.redShift) | (uint32_t(g) << fb.greenShift) | (uint32_t(b) << fb.blueShift);
    }

    void fill(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color) const {
        for (int64_t py = y; py < y + h; py++) {
            if (py < 0 or uint64_t(py) >= fb.height)
                continue;
            auto* row = reinterpret_cast<uint32_t*>(fb.physical + uint64_t(py) * fb.pitch);
            for (int64_t px = x; px < x + w; px++) {
                if (px >= 0 and uint64_t(px) < fb.width)
                    row[px] = color;
            }
        }
    }

    // A pointed tip: a triangle of `height` rows, `width` wide at its base.
    void tip(int64_t cx, int64_t baseY, int64_t width, int64_t height, uint32_t color) const {
        for (int64_t row = 0; row < height; row++) {
            int64_t half = width / 2 * (row + 1) / height;
            fill(cx - half, baseY - height + row, half * 2 + 1, 1, color);
        }
    }
};

} // namespace

void setupGraphics(Config const& config, Shool::Framebuffer& out) {
    out = {};
    Efi::GraphicsOutput* gop = nullptr;
    if (Efi::failed(bootServices->locateProtocol(&Efi::GRAPHICS_OUTPUT_PROTOCOL, nullptr,
                                                 reinterpret_cast<void**>(&gop)))) {
        print("  no graphics output, continuing without a screen\n");
        return;
    }

    uint32_t mode = chooseMode(gop, config);
    if (mode != gop->mode->mode and Efi::failed(gop->setMode(gop, mode)))
        print("  warning: can't switch screen mode, keeping the current one\n");

    auto const& info = *gop->mode->info;
    if (not usable(info))
        return;

    out.physical = gop->mode->frameBufferBase;
    out.width = info.horizontalResolution;
    out.height = info.verticalResolution;
    out.pitch = uint64_t(info.pixelsPerScanLine) * 4;
    out.bpp = 32;

    switch (info.pixelFormat) {
    case Efi::PIXEL_RGB_RESERVED_8BIT:
        out.redShift = 0, out.greenShift = 8, out.blueShift = 16;
        break;
    case Efi::PIXEL_BGR_RESERVED_8BIT:
        out.redShift = 16, out.greenShift = 8, out.blueShift = 0;
        break;
    default:
        out.redShift = lowestBit(info.pixelInformation.red);
        out.greenShift = lowestBit(info.pixelInformation.green);
        out.blueShift = lowestBit(info.pixelInformation.blue);
        break;
    }
}

void drawTrident(Shool::Framebuffer const& fb) {
    if (not fb.physical)
        return;

    Painter p{fb};
    uint32_t night = p.pack(0x14, 0x0b, 0x2e); // same color as the top of RudraCore's boot screen
    uint32_t saffron = p.pack(0xff, 0x99, 0x33);
    uint32_t gold = p.pack(0xff, 0xc8, 0x57);
    p.fill(0, 0, int64_t(fb.width), int64_t(fb.height), night);

    int64_t size = int64_t(fb.height) / 3;     // total height of the trident
    int64_t cx = int64_t(fb.width) / 2;
    int64_t top = (int64_t(fb.height) - size) / 2;
    int64_t w = size / 22 + 2;                  // stroke width

    int64_t barY = top + size * 36 / 100;      // crossbar
    int64_t sideX = size * 22 / 100;            // side prongs' distance from the center
    int64_t tipH = size / 9;

    // Center prong and its spear tip.
    p.fill(cx - w / 2, top + tipH, w, barY - top - tipH, saffron);
    p.tip(cx, top + tipH, w * 3, tipH, gold);

    // Side prongs, rising from the ends of the crossbar.
    int64_t const sides[] = {-1, 1};
    for (int64_t side : sides) {
        int64_t x = cx + side * sideX;
        int64_t prongTop = top + size * 14 / 100;
        p.fill(x - w / 2, prongTop + tipH, w, barY - prongTop - tipH + w, saffron);
        p.tip(x, prongTop + tipH, w * 3, tipH, gold);
    }

    // Crossbar, a small knot below it, and the shaft.
    p.fill(cx - sideX - w / 2, barY, sideX * 2 + w, w, saffron);
    p.fill(cx - w * 2, barY + size / 12, w * 4, w * 2, gold);
    p.fill(cx - w / 2, barY + w, w, top + size - barY - w, saffron);
}

} // namespace Loader
