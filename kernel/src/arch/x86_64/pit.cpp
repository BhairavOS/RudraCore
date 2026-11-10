#include "arch/x86_64/pit.h"

#include "arch/x86_64/io.h"

namespace Rudracore::Arch::Pit {

namespace {

constexpr u32 BASE_FREQUENCY = 1193182;
constexpr u16 CHANNEL0 = 0x40;
constexpr u16 COMMAND = 0x43;
constexpr u8 CHANNEL0_RATE_GENERATOR = 0x34; // channel 0, lo/hi byte, mode 2

} // namespace

void init(u32 hz) {
    u32 divisor = BASE_FREQUENCY / hz;
    outb(COMMAND, CHANNEL0_RATE_GENERATOR);
    outb(CHANNEL0, static_cast<u8>(divisor));
    outb(CHANNEL0, static_cast<u8>(divisor >> 8));
}

} // namespace Rudracore::Arch::Pit
