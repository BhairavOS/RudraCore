#include "arch/x86_64/io.h"
#include "boot.h"
#include "gfx/console.h"
#include "gfx/splash.h"
#include "log.h"
#include "serial.h"

using namespace Rudracore;

// Global constructors, collected by the linker script.
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

namespace {

constexpr char const* VERSION = "0.1.0";

Gfx::Surface screen;
Gfx::Font font;
Gfx::Console console;

void mirrorToConsole(char c) { console.write(c); }

void runGlobalConstructors() {
    for (auto* ctor = __init_array_start; ctor < __init_array_end; ctor++)
        (*ctor)();
}

} // namespace

extern "C" [[noreturn]] void kmain() {
    Serial::init();
    runGlobalConstructors();

    Boot::Info info = Boot::collect();
    if (not info.protocolSupported) {
        log("rudracore: panic: the bootloader does not support Limine base revision 3");
        Arch::halt();
    }

    if (info.hasFramebuffer) {
        screen = Gfx::Surface::from(info.framebuffer);
        font = Gfx::Font::embedded();
        Gfx::Rect area = Gfx::drawSplash(screen, font, "RudraCore microkernel");
        if (font.valid()) {
            console.init(screen, font, area.x, area.y, area.w, area.h,
                         Gfx::Palette::PANEL, Gfx::Palette::TEXT);
            setLogMirror(mirrorToConsole);
        }
    }

    log("RudraCore ", VERSION, " - the BhairavOS kernel");
    log("");
    log("[boot] loader      ", info.loaderName, " ", info.loaderVersion);
    if (info.hasFramebuffer) {
        auto const& fb = info.framebuffer;
        log("[boot] display     ", fb.width, "x", fb.height, " ", fb.bpp, "bpp");
    } else {
        log("[boot] display     none (serial only)");
    }
    log("[boot] font        ", font.valid() ? "ok" : "missing", " (", font.width, "x", font.height, ")");
    log("[mem]  usable      ", Bytes{info.usableMemory});
    log("[mem]  reclaimable ", Bytes{info.reclaimableMemory});
    log("[mem]  map entries ", info.memmapEntries);
    log("[mem]  hhdm offset ", Hex{info.hhdmOffset});
    log("");
    log("[core] step 1 complete. next: GDT, IDT, physical memory manager.");
    log("[core] halting.");

    Arch::halt();
}
