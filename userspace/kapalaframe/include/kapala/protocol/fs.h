#pragma once

// Kapalaframe: the file system protocol, between programs and the fs
// server. Most programs use Kapala::Files (kapala/files.h) instead.
//
// Connecting: whoever holds the fs server's "connect" channel sends it a
// Connect message with one end of a new channel attached; the server then
// serves that channel. KamalShell connects every app it starts (the app's
// handle 2).
//
//   client                                           fs
//   Attach + shared memory (BUFFER_SIZE bytes)    ->
//                                                 <- Reply
//   Read{path, offset, length}                    -> (copies file data into the buffer)
//                                                 <- Reply{length read, file size}
//   (fills the buffer) Write{path, offset, length} -> creates the file if needed;
//                                                     TRUNCATE empties it first
//                                                 <- Reply{length written, new size}
//   List{path, offset: first entry}               -> (Entry array into the buffer)
//                                                 <- Reply{entries returned, total entries}
//   Stat{path}                                    <- Reply{size, directory}
//   Remove{path}  MakeDir{path}                   <- Reply
//
// Paths are absolute ("/notes/today.txt") and case-insensitive.
// Writes reach the disk before the reply.

namespace Kapala::Protocol::Fs {

constexpr unsigned long BUFFER_SIZE = 64 * 1024;
constexpr unsigned long MAX_PATH = 96; // including the final 0
constexpr unsigned long MAX_NAME = 48; // listed names are cut to fit

enum class Op : unsigned char {
    Attach = 1,
    Read = 2,
    Write = 3,
    List = 4,
    Stat = 5,
    Remove = 6,
    MakeDir = 7,
};

enum Flags : unsigned char {
    TRUNCATE = 1 << 0, // Write: empty the file first
};

struct Connect {
    unsigned long magic; // CONNECT_MAGIC
};
constexpr unsigned long CONNECT_MAGIC = 0x464C4F57; // "WOLF"

struct Request {
    Op op;
    unsigned char flags;
    unsigned long offset;
    unsigned long length;
    char path[MAX_PATH];
};

struct Reply {
    long status; // Sys::OK or a negative Sys::Error
    unsigned long length;
    unsigned long size;
    bool directory;
};

struct Entry {
    char name[MAX_NAME];
    unsigned long size;
    unsigned long modified; // seconds since 1970 (UTC)
    bool directory;
};

static_assert(sizeof(Request) <= 128 and sizeof(Reply) <= 128);

} // namespace Kapala::Protocol::Fs
