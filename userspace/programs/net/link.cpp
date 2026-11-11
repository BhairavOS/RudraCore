#include "link.h"

namespace Net {

namespace Sys = Kapala::Sys;
namespace Dev = Kapala::Protocol::NetDev;

bool Link::attach(Sys::Handle driver) {
    _driver = Kapala::Channel(driver);
    long memory = Sys::memoryCreate(Dev::SHARED_SIZE);
    if (memory < 0)
        return false;
    _shared = static_cast<Dev::Shared*>(Sys::memoryMap(memory));
    if (not _shared)
        return false;
    Dev::Message attach{};
    attach.type = Dev::Type::Attach;
    if (_driver.send(attach, static_cast<Sys::Handle>(memory)) != Sys::OK) {
        _shared = nullptr;
        return false;
    }
    // Its Info says which card it is (or the channel closes: no card).
    Sys::Handle h = _driver.handle();
    if (Sys::wait(&h, 1, 5000) < 0)
        return false;
    bool gotInfo = service();
    bool anyMac = false;
    for (u8 b : _mac)
        anyMac |= b != 0;
    if (not gotInfo or not anyMac) {
        _shared = nullptr;
        return false;
    }
    return true;
}

bool Link::send(u8 const* frame, u32 length) {
    if (not _shared or not Dev::push(_shared->send, frame, length))
        return false;
    _needKick = true;
    return true;
}

void Link::flush() {
    if (not _needKick)
        return;
    _needKick = false;
    Dev::Message kick{};
    kick.type = Dev::Type::Kick;
    _driver.send(kick); // QUEUE_FULL: one is already waiting
}

bool Link::service() {
    for (int n = 0; n < 64; n++) {
        Dev::Message m{};
        long r = _driver.recv(m, nullptr, Sys::RECV_NONBLOCK);
        if (r == Sys::WOULD_BLOCK)
            return true;
        if (r < 0 and r != Sys::INVALID_ARGUMENT)
            return false;
        if (r == Sys::OK and m.type == Dev::Type::Info) {
            _up = m.linkUp;
            for (int i = 0; i < 6; i++)
                _mac[i] = m.mac[i];
            for (int i = 0; i < 23; i++)
                _name[i] = m.name[i];
        }
    }
    return true;
}

} // namespace Net
