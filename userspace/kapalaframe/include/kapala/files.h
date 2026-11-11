#pragma once

// Kapalaframe: files on the disk, through the fs server.
//
//   Files files;
//   if (files.connect(2)) {           // apps get the connection as handle 2
//       files.write("/hello.txt", "namaste", 7);
//       char text[64];
//       long n = files.read("/hello.txt", text, sizeof(text));
//   }
//
// Every call returns a byte or entry count (>= 0), or a negative Sys::Error.

#include <kapala/channel.h>
#include <kapala/protocol/fs.h>

namespace Kapala {

using FileEntry = Protocol::Fs::Entry;

struct FileInfo {
    unsigned long size;
    bool directory;
};

class Files {
public:
    bool connect(Sys::Handle channel);
    bool connected() const { return _buffer != nullptr; }

    // Reads up to `capacity` bytes from `offset`.
    long read(char const* path, void* data, unsigned long capacity, unsigned long offset = 0);

    // Replaces the file's contents (creating it if needed).
    long write(char const* path, void const* data, unsigned long length);

    // Adds to the end of the file (creating it if needed).
    long append(char const* path, void const* data, unsigned long length);

    // Up to `capacity` entries of a directory, from entry `first` on.
    long list(char const* path, FileEntry* entries, unsigned long capacity, unsigned long first = 0);

    long stat(char const* path, FileInfo& info);
    long remove(char const* path);
    long makeDir(char const* path);

private:
    long call(Protocol::Fs::Op op, char const* path, unsigned long offset, unsigned long length,
              unsigned char flags, Protocol::Fs::Reply& reply);

    Channel _channel;
    unsigned char* _buffer = nullptr;
};

} // namespace Kapala
