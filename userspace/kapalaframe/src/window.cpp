#include <kapala/protocol/shell.h>
#include <kapala/window.h>

namespace Kapala {

using Protocol::Shell::Message;
using Protocol::Shell::Type;

namespace {

void copyTitle(char* dest, char const* src) {
    unsigned i = 0;
    for (; src[i] and i + 1 < sizeof(Message::title); i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

} // namespace

bool Window::open(char const* title, int width, int height, Sys::Handle shell) {
    _shell = Channel(shell);

    Sys::SystemInfo info;
    Sys::systemInfo(info);

    long memory = Sys::memoryCreate(static_cast<unsigned long>(width) * height * 4);
    if (memory < 0)
        return false;
    auto* pixels = static_cast<unsigned*>(Sys::memoryMap(memory));
    if (not pixels)
        return false;
    _surface = {pixels, width, height, width, {info.redShift, info.greenShift, info.blueShift}};

    Message create{};
    create.type = Type::CreateWindow;
    create.w = width;
    create.h = height;
    copyTitle(create.title, title);
    // The memory handle moves to the shell; our mapping stays.
    if (_shell.send(create, static_cast<Sys::Handle>(memory)) != Sys::OK)
        return false;

    for (;;) {
        Message reply;
        if (_shell.recv(reply) != Sys::OK)
            return false;
        if (reply.type == Type::Opened)
            return true;
        if (reply.type == Type::Close)
            return false;
    }
}

void Window::present(Gfx::Rect area) {
    area = area.intersect(_surface.bounds());
    if (area.empty())
        return;
    Message message{};
    message.type = Type::Present;
    message.x = area.x;
    message.y = area.y;
    message.w = area.w;
    message.h = area.h;
    _shell.send(message);
}

void Window::setTitle(char const* title) {
    Message message{};
    message.type = Type::SetTitle;
    copyTitle(message.title, title);
    _shell.send(message);
}

bool Window::waitEvent(WindowEvent& event, unsigned long timeoutMs) {
    unsigned long deadline = timeoutMs == Sys::WAIT_FOREVER ? 0 : Sys::timeMs() + timeoutMs;

    for (;;) {
        unsigned long remaining = Sys::WAIT_FOREVER;
        if (deadline) {
            unsigned long now = Sys::timeMs();
            remaining = now >= deadline ? 0 : deadline - now;
        }

        Sys::Handle handle = _shell.handle();
        if (Sys::wait(&handle, 1, remaining) == Sys::TIMED_OUT)
            return false;

        Message m;
        long result = _shell.recv(m, nullptr, Sys::RECV_NONBLOCK);
        if (result == Sys::WOULD_BLOCK)
            continue;

        event = {};
        if (result != Sys::OK) { // the shell is gone
            event.kind = WindowEvent::Close;
            return true;
        }

        event.at = {m.x, m.y};
        event.buttons = m.buttons;
        switch (m.type) {
        case Type::MouseMove: event.kind = WindowEvent::MouseMove; return true;
        case Type::MouseDown: event.kind = WindowEvent::MouseDown; return true;
        case Type::MouseUp: event.kind = WindowEvent::MouseUp; return true;
        case Type::Close: event.kind = WindowEvent::Close; return true;
        case Type::Key:
            event.kind = WindowEvent::Key;
            event.character = m.character;
            event.scancode = m.scancode;
            event.pressed = m.pressed;
            return true;
        case Type::Focus:
            _focused = m.pressed;
            event.kind = WindowEvent::Focus;
            event.pressed = m.pressed;
            return true;
        default:
            continue; // not for apps
        }
    }
}

} // namespace Kapala
