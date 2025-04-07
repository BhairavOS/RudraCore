#pragma once

#include <rudra/io.h>

#include "object.h"

namespace Rudracore::Core {

struct Vmo : public BaseObject<Vmo, Hj::Type::VMO> {
    using _Mem = Union<Hal::PmmMem, Hal::DmaRange>;
    _Mem _mem;

    static Res<Arc<Vmo>> alloc(usize size, Hj::VmoFlags);

    static Res<Arc<Vmo>> makeDma(Hal::DmaRange prange);

    Vmo(_Mem mem) : _mem(std::move(mem)) {}

    Hal::PmmRange range();
};

} // namespace Rudracore::Core