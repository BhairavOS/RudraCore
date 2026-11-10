#include "core/time.h"

#include "arch/x86_64/pic.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/rtc.h"

namespace Rudracore::Time {

namespace {

u64 ticks = 0;
u64 bootTimeOfDay = 0;

} // namespace

void init() {
    bootTimeOfDay = Arch::Rtc::secondsOfDay();
    Arch::Pit::init(TICKS_PER_SECOND);
    Arch::Pic::unmask(0);
}

u64 timeOfDay() {
    return (bootTimeOfDay + now() / 1000) % (24 * 60 * 60);
}

void tick() {
    __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
}

u64 now() {
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED) * 1000 / TICKS_PER_SECOND;
}

} // namespace Rudracore::Time
