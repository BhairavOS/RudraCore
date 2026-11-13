#pragma once

// Kapalaframe: the file system protocol, between programs and the fs
// server. Most programs use Kapala::Files (kapala/files.h) instead.
//
// Connecting: whoever holds the fs server's "connect" channel sends it a
// Connect message with one end of a new channel attached; the server then
// serves that channel. KamalShell connects every app it starts (the app's
// handle 2).
//
// The Connect message says who the client is and what it may reach, and
// only holders of the connect channel (init and KamalShell) can send one:
// a client can't change its own scope or name. Scopes:
//   System   the whole disk (system services, the App Store, Terminal)
//   User     the user's files: /apps, /var are read-only, /appdata is
//            hidden (other apps' private storage)
//   Private  only its own folder, /appdata/<app>, which it sees as "/";
//            plus, if Connect names a folder, that one user folder as
//            "/shared" (never a protected directory, never "/")
// /apps (installed programs), /var (system state, permissions) and
// /appdata (apps' private storage) are the protected system directories.
// A refused request gets ACCESS_DENIED (or NOT_FOUND for hidden paths).
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

enum class Scope : unsigned char {
    System = 1,
    User = 2,
    Private = 3,
};

constexpr unsigned long MAX_APP_NAME = 24; // lowercase letters, digits and '-'

constexpr unsigned long MAX_FOLDER = 64; // including the final 0

struct Connect {
    unsigned long magic; // CONNECT_MAGIC
    Scope scope;
    char app[MAX_APP_NAME]; // who the client is (its private folder's name)
    char folder[MAX_FOLDER]; // Private: a user folder it may use as /shared ("" = none)
};
static_assert(sizeof(Connect) <= 128);
constexpr unsigned long CONNECT_MAGIC = 0x464C4F58; // "XOLF": v2, with a scope

inline Connect makeConnect(Scope scope, char const* app, char const* folder = "") {
    Connect c{};
    c.magic = CONNECT_MAGIC;
    c.scope = scope;
    for (unsigned long i = 0; app[i] and i + 1 < sizeof(c.app); i++)
        c.app[i] = app[i];
    for (unsigned long i = 0; folder[i] and i + 1 < sizeof(c.folder); i++)
        c.folder[i] = folder[i];
    return c;
}

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
