// fs-test: checks the disk and file system at every boot, then cleans up.
//
//   handle 1: a connection to the fs server
//
// Makes a directory, writes files with short and long names, reads them
// back, appends, writes a file bigger than one transfer, lists, removes
// everything, and logs whether it all matched.

#include <kapala/files.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;

namespace {

constexpr char const* DIR = "/fs-test.tmp";
constexpr char const* SHORT_FILE = "/fs-test.tmp/hello.txt";
constexpr char const* LONG_FILE = "/fs-test.tmp/A Long Name for Bhairav OS.text";
constexpr char const* BIG_FILE = "/fs-test.tmp/big.bin";
constexpr unsigned long BIG_SIZE = 150 * 1024; // more than two 64 KiB transfers

Files files;
bool ok = true;

void check(bool condition, char const* what, long result = 0) {
    if (condition)
        return;
    Text<> text;
    text.add("fs check: FAILED: ").add(what);
    if (result < 0)
        text.add(" (error ").add(result).add(")");
    Sys::log(text.cstr());
    ok = false;
}

bool equal(char const* a, char const* b, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

unsigned char pattern(unsigned long i) { return static_cast<unsigned char>((i * 7 + i / 251) & 0xFF); }

} // namespace

int main() {
    if (not files.connect(1)) {
        Sys::log("fs check: skipped, no file system");
        return 0;
    }
    FileInfo info{};
    if (files.stat("/", info) < 0) {
        Sys::log("fs check: skipped, no disk");
        return 0;
    }

    // Leftovers from a run that was cut short.
    files.remove(SHORT_FILE);
    files.remove(LONG_FILE);
    files.remove(BIG_FILE);
    files.remove(DIR);

    long r = files.makeDir(DIR);
    check(r == Sys::OK, "make a directory", r);
    check(files.makeDir(DIR) == Sys::ALREADY_EXISTS, "refuse to make it twice");

    char const hello[] = "namaste from Bhairav OS";
    r = files.write(SHORT_FILE, hello, sizeof(hello) - 1);
    check(r == sizeof(hello) - 1, "write a file", r);
    char back[64] = {};
    r = files.read("/FS-TEST.TMP/HELLO.TXT", back, sizeof(back)); // names ignore case
    check(r == sizeof(hello) - 1 and equal(back, hello, sizeof(hello) - 1), "read it back", r);

    r = files.append(SHORT_FILE, "!", 1);
    check(r == 1, "append", r);
    r = files.stat(SHORT_FILE, info);
    check(r == Sys::OK and info.size == sizeof(hello) and not info.directory, "size after append", r);

    char const poem[] = "Om Namah Shivaya";
    r = files.write(LONG_FILE, poem, sizeof(poem) - 1);
    check(r == sizeof(poem) - 1, "write a long file name", r);
    r = files.read(LONG_FILE, back, sizeof(back));
    check(r == sizeof(poem) - 1 and equal(back, poem, sizeof(poem) - 1), "read a long file name", r);

    auto* big = new unsigned char[BIG_SIZE];
    auto* bigBack = new unsigned char[BIG_SIZE];
    for (unsigned long i = 0; i < BIG_SIZE; i++)
        big[i] = pattern(i);
    unsigned long start = Sys::timeMs();
    r = files.write(BIG_FILE, big, BIG_SIZE);
    unsigned long writeMs = Sys::timeMs() - start;
    check(r == static_cast<long>(BIG_SIZE), "write a big file", r);
    start = Sys::timeMs();
    r = files.read(BIG_FILE, bigBack, BIG_SIZE);
    unsigned long readMs = Sys::timeMs() - start;
    bool same = r == static_cast<long>(BIG_SIZE);
    for (unsigned long i = 0; same and i < BIG_SIZE; i++)
        same = bigBack[i] == big[i];
    check(same, "read the big file back", r);

    // Overwriting with something shorter replaces the whole file.
    r = files.write(BIG_FILE, "short", 5);
    check(r == 5 and files.stat(BIG_FILE, info) == Sys::OK and info.size == 5, "replace a file");
    delete[] big;
    delete[] bigBack;

    FileEntry entries[8];
    r = files.list(DIR, entries, 8);
    bool foundLong = false;
    for (long i = 0; i < r; i++)
        foundLong |= equal(entries[i].name, "A Long Name for Bhairav OS.text", 31);
    check(r == 3 and foundLong, "list the directory", r);

    check(files.remove(DIR) == Sys::NOT_EMPTY, "refuse to remove a full directory");
    check(files.read("/fs-test.tmp/missing", back, 1) == Sys::NOT_FOUND, "report a missing file");
    check(files.write("/no-such-dir/x", "x", 1) == Sys::NOT_FOUND, "refuse a missing directory");

    check(files.remove(SHORT_FILE) == Sys::OK, "remove a file");
    check(files.remove(LONG_FILE) == Sys::OK, "remove a long-named file");
    check(files.remove(BIG_FILE) == Sys::OK, "remove the big file");
    check(files.remove(DIR) == Sys::OK, "remove the directory");
    check(files.stat(DIR, info) == Sys::NOT_FOUND, "it's gone");

    if (ok) {
        Text<> text;
        text.add("fs check: create, read, append, long names, directories and remove ok; 150 KiB written in ")
            .add(writeMs).add(" ms, read in ").add(readMs).add(" ms");
        Sys::log(text.cstr());
    }
    return 0;
}
