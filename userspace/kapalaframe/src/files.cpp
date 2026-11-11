#include <kapala/files.h>

extern "C" void* memcpy(void* dest, void const* src, unsigned long n);

namespace Kapala {

namespace Fs = Protocol::Fs;

bool Files::connect(Sys::Handle channel) {
    _channel = Channel(channel);
    long memory = Sys::memoryCreate(Fs::BUFFER_SIZE);
    if (memory < 0)
        return false;
    auto* buffer = static_cast<unsigned char*>(Sys::memoryMap(memory));
    if (not buffer) {
        Sys::close(memory);
        return false;
    }

    Fs::Request request{};
    request.op = Fs::Op::Attach;
    Fs::Reply reply{};
    // The handle moves to the server; the mapping keeps the memory here.
    if (_channel.send(request, memory) != Sys::OK or _channel.recv(reply) != Sys::OK or reply.status != Sys::OK) {
        Sys::memoryUnmap(buffer);
        return false;
    }
    _buffer = buffer;
    return true;
}

long Files::call(Fs::Op op, char const* path, unsigned long offset, unsigned long length,
                 unsigned char flags, Fs::Reply& reply) {
    if (not _buffer)
        return Sys::NOT_FOUND;

    Fs::Request request{};
    request.op = op;
    request.flags = flags;
    request.offset = offset;
    request.length = length;
    unsigned long n = 0;
    while (path[n]) {
        if (n + 1 >= Fs::MAX_PATH)
            return Sys::INVALID_ARGUMENT;
        request.path[n] = path[n];
        n++;
    }
    request.path[n] = '\0';

    long result = _channel.send(request);
    if (result == Sys::OK)
        result = _channel.recv(reply);
    return result == Sys::OK ? reply.status : result;
}

long Files::read(char const* path, void* data, unsigned long capacity, unsigned long offset) {
    auto* out = static_cast<unsigned char*>(data);
    unsigned long total = 0;
    while (total < capacity) {
        unsigned long chunk = capacity - total < Fs::BUFFER_SIZE ? capacity - total : Fs::BUFFER_SIZE;
        Fs::Reply reply{};
        long result = call(Fs::Op::Read, path, offset + total, chunk, 0, reply);
        if (result < 0)
            return result;
        memcpy(out + total, _buffer, reply.length);
        total += reply.length;
        if (reply.length < chunk)
            break; // the end of the file
    }
    return static_cast<long>(total);
}

long Files::write(char const* path, void const* data, unsigned long length) {
    if (not _buffer)
        return Sys::NOT_FOUND;
    auto const* in = static_cast<unsigned char const*>(data);
    unsigned long total = 0;
    do {
        unsigned long chunk = length - total < Fs::BUFFER_SIZE ? length - total : Fs::BUFFER_SIZE;
        memcpy(_buffer, in + total, chunk);
        Fs::Reply reply{};
        long result = call(Fs::Op::Write, path, total, chunk, total == 0 ? Fs::TRUNCATE : 0, reply);
        if (result < 0)
            return result;
        total += chunk;
    } while (total < length);
    return static_cast<long>(total);
}

long Files::append(char const* path, void const* data, unsigned long length) {
    if (not _buffer)
        return Sys::NOT_FOUND;
    FileInfo info{};
    long result = stat(path, info);
    if (result == Sys::NOT_FOUND)
        info.size = 0;
    else if (result < 0)
        return result;

    auto const* in = static_cast<unsigned char const*>(data);
    unsigned long total = 0;
    while (total < length) {
        unsigned long chunk = length - total < Fs::BUFFER_SIZE ? length - total : Fs::BUFFER_SIZE;
        memcpy(_buffer, in + total, chunk);
        Fs::Reply reply{};
        result = call(Fs::Op::Write, path, info.size + total, chunk, 0, reply);
        if (result < 0)
            return result;
        total += chunk;
    }
    return static_cast<long>(total);
}

long Files::list(char const* path, FileEntry* entries, unsigned long capacity, unsigned long first) {
    unsigned long max = Fs::BUFFER_SIZE / sizeof(FileEntry);
    Fs::Reply reply{};
    long result = call(Fs::Op::List, path, first, capacity < max ? capacity : max, 0, reply);
    if (result < 0)
        return result;
    memcpy(entries, _buffer, reply.length * sizeof(FileEntry));
    return static_cast<long>(reply.length);
}

long Files::stat(char const* path, FileInfo& info) {
    Fs::Reply reply{};
    long result = call(Fs::Op::Stat, path, 0, 0, 0, reply);
    if (result < 0)
        return result;
    info = {reply.size, reply.directory};
    return Sys::OK;
}

long Files::remove(char const* path) {
    Fs::Reply reply{};
    return call(Fs::Op::Remove, path, 0, 0, 0, reply);
}

long Files::makeDir(char const* path) {
    Fs::Reply reply{};
    return call(Fs::Op::MakeDir, path, 0, 0, 0, reply);
}

} // namespace Kapala
