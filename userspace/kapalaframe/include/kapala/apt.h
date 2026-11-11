#pragma once

// Kapalaframe: apt, the package manager. Finds apps in repositories,
// installs, upgrades and removes them (on top of kapala/apps.h). The
// Terminal's `apt` command and the App Store both use it.
//
// Repositories:
//   - "builtin": the catalog in the boot image (always there);
//   - web addresses listed in /etc/apt/sources.txt (one per line), each a
//     folder on a web server holding a "Packages" index and the packages.
//
// A Packages index is blocks of "key = value" lines (a manifest plus
// `file`, the package's name in the folder), separated by blank lines:
//
//   id = clock
//   title = Clock
//   version = 1.0
//   author = Bhairav OS examples
//   description = An analog clock.
//   file = clock.bpk
//
// `apt update` downloads every index into /var/apt/lists/; everything else
// reads those copies, so only update and install need the network.

#include <kapala/apps.h>
#include <kapala/net.h>

namespace Kapala::Apt {

constexpr char const* SOURCES = "/etc/apt/sources.txt";
constexpr char const* LISTS = "/var/apt/lists";
constexpr char const* BUILTIN = "builtin";
constexpr unsigned long MAX_SOURCE = 160;

// A package a repository offers.
struct Available {
    AppInfo info;
    char source[MAX_SOURCE]; // BUILTIN, or the repository's address
    char file[64];           // its .bpk in that repository
};

// The repositories (BUILTIN first). Returns how many.
long sources(Files& files, char (*out)[MAX_SOURCE], long capacity);
long addSource(Files& files, char const* address);
long removeSource(Files& files, char const* address);

// Downloads every repository's index. `report` is told about each one.
struct UpdateReport {
    virtual void source(char const* address, long packages, char const* error) = 0;
};
long update(Files& files, Net& net, UpdateReport& report);

// Every package the repositories offer (from the last update).
long available(Files& files, Available* out, long capacity);

// The newest version of an app in the repositories, or false.
bool find(Files& files, char const* id, Available& out);

// Downloads (or takes from the catalog) and installs.
long install(Files& files, Net& net, Available const& package, AppInfo& installed, char const*& error);

// <0, 0 or >0, comparing "1.10" > "1.9" as numbers.
int compareVersions(char const* a, char const* b);

} // namespace Kapala::Apt
