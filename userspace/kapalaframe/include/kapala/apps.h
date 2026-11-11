#pragma once

// Kapalaframe: apps that aren't part of the system (third-party apps).
//
// An app travels as a package, a .bpk file:
//   "BPK1", u32 file count, then per file: char name[32], u32 size, the bytes.
// It holds:
//   manifest.txt   lines "key = value": id, title, version, author, description
//   app.elf        the program (built against Kapalaframe, like any app)
//   icon.png       its icon (optional; square, 64 px or more looks best)
//
// Installing puts those files in /apps/<id>/ on the disk; KamalShell shows
// installed apps in the dock and the app grid and starts them like the
// built-in ones (they get the same handles: shell, files, network).
//
// A catalog bundles several packages (the App Store's shelf):
//   "BCT1", u32 package count, u32 total size of the file, then per
//   package: u32 size, the .bpk bytes.

#include <kapala/files.h>

namespace Kapala {

struct AppInfo {
    char id[24];   // lowercase letters, digits and '-' (also its folder name)
    char title[40];
    char version[16];
    char author[40];
    char description[160];
};

constexpr unsigned long MAX_APP_SIZE = 16 * 1024 * 1024;

// Programs that come with Bhairav OS: an app can't take their names.
bool isBuiltInProgram(char const* id);

// A package in memory.
class Package {
public:
    // False if it isn't a well-formed package (sets error).
    bool parse(unsigned char const* data, unsigned long length, char const*& error);

    // One of its files, or nullptr.
    unsigned char const* file(char const* name, unsigned long& size) const;

    AppInfo const& info() const { return _info; }

private:
    unsigned char const* _data = nullptr;
    unsigned long _length = 0;
    unsigned _count = 0;
    AppInfo _info{};
};

// Reads "key = value" lines.
bool parseManifest(char const* text, unsigned long length, AppInfo& out);

// Installs (or updates) a package on the disk. OK, or an error with `error`
// saying why.
long installPackage(Files& files, unsigned char const* data, unsigned long length, AppInfo& out,
                    char const*& error);

long uninstallApp(Files& files, char const* id);

// The installed apps (up to `capacity`); returns how many.
long listApps(Files& files, AppInfo* out, long capacity);

long readAppInfo(Files& files, char const* id, AppInfo& out);

// Reads a whole file into a new heap buffer (delete[] it). nullptr on failure.
unsigned char* readWholeFile(Files& files, char const* path, unsigned long& length,
                             unsigned long limit = MAX_APP_SIZE);

// Iterates the packages of a catalog: calls visit(data, length) for each.
// (The catalog's own header says how long it is.)
template <typename F>
bool forEachInCatalog(unsigned char const* data, F visit) {
    if (not data or data[0] != 'B' or data[1] != 'C' or data[2] != 'T' or data[3] != '1')
        return false;
    unsigned count = data[4] | data[5] << 8 | data[6] << 16 | static_cast<unsigned>(data[7]) << 24;
    unsigned long length = data[8] | data[9] << 8 | data[10] << 16 | static_cast<unsigned long>(data[11]) << 24;
    unsigned long at = 12;
    for (unsigned i = 0; i < count and at + 4 <= length; i++) {
        unsigned long size = data[at] | data[at + 1] << 8 | data[at + 2] << 16 |
                             static_cast<unsigned long>(data[at + 3]) << 24;
        at += 4;
        if (size > length - at)
            return false;
        visit(data + at, size);
        at += size;
    }
    return true;
}

} // namespace Kapala
