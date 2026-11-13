// fs-test: checks the disk and file system at every boot, then cleans up.
//
//   handle 1: a connection to the fs server (System scope)
//   handles 2-5: User, Private, Private with a folder, and a refused one
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

// The scopes: handle 2 is a User connection and handle 3 a Private one,
// both as the app "fs-test" (handle 1, `files`, is System).
void checkScopes() {
    Files user, mine;
    if (not user.connect(2) or not mine.connect(3)) {
        check(false, "connect with the User and Private scopes");
        return;
    }
    char back[32] = {};
    FileInfo info{};

    // Private: its own folder is "/", and ".." can't leave it.
    check(mine.write("/note.txt", "own", 3) == 3, "write to private storage");
    check(files.read("/appdata/fs-test/note.txt", back, sizeof(back)) == 3, "private storage is /appdata/<app>");
    check(mine.read("/sub/../note.txt", back, sizeof(back)) == 3, "'..' inside its folder");
    check(mine.read("/../../appdata/fs-test/note.txt", back, sizeof(back)) == Sys::INVALID_ARGUMENT,
          "'..' can't climb out of its folder");
    check(mine.stat("/apps", info) == Sys::NOT_FOUND, "a private app doesn't see /apps");
    check(mine.remove("/") == Sys::INVALID_ARGUMENT, "a private app can't remove its own folder");

    // User: system directories are read-only, other apps' storage hidden.
    check(user.write("/apps/planted.txt", "x", 1) == Sys::ACCESS_DENIED, "write into /apps");
    check(user.makeDir("/var/planted") == Sys::ACCESS_DENIED, "make a folder in /var");
    check(user.write("/VAR/planted.txt", "x", 1) == Sys::ACCESS_DENIED, "upper case doesn't get around it");
    check(user.write("/tmp/../var/planted.txt", "x", 1) == Sys::ACCESS_DENIED, "'..' doesn't get around it");
    check(user.remove("/apps") == Sys::ACCESS_DENIED, "remove /apps");
    check(user.read("/appdata/fs-test/note.txt", back, sizeof(back)) == Sys::NOT_FOUND, "apps' storage is hidden");
    FileEntry top[32];
    long n = user.list("/", top, 32);
    bool shown = false;
    for (long i = 0; i < n; i++)
        shown |= equal(top[i].name, "appdata", 8);
    check(n >= 0 and not shown, "apps' storage isn't listed");
    check(user.write("/fs-test-user.txt", "u", 1) == 1, "the user's own files are writable");
    files.remove("/fs-test-user.txt");

    // One user folder (handle 4: /fs-test-shared, seen as /shared), and a
    // folder that may never be given (handle 5: /var, refused at connect).
    Files folder, forbidden;
    check(folder.connect(4), "connect with a folder");
    check(folder.write("/shared/given.txt", "ok", 2) == 2, "write into the given folder");
    check(files.read("/fs-test-shared/given.txt", back, sizeof(back)) == 2, "it lands in that folder");
    check(folder.read("/shared/../shared/given.txt", back, sizeof(back)) == 2, "'..' back into it");
    check(folder.write("/shared/../../var/x", "x", 1) == Sys::INVALID_ARGUMENT, "'..' can't climb out");
    check(folder.remove("/shared") == Sys::INVALID_ARGUMENT, "the given folder itself stays");
    check(folder.write("/own.txt", "x", 1) == 1, "its own storage still works beside it");
    check(not forbidden.connect(5), "a system folder can't be given");
    files.remove("/fs-test-shared/given.txt");
    files.remove("/fs-test-shared");
    files.remove("/appdata/fs-test/own.txt");

    files.remove("/appdata/fs-test/note.txt");
    files.remove("/appdata/fs-test");
}

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

    checkScopes();

    if (ok) {
        Text<> text;
        text.add("fs check: create, read, append, long names, directories, remove and scopes ok; 150 KiB written in ")
            .add(writeMs).add(" ms, read in ").add(readMs).add(" ms");
        Sys::log(text.cstr());
    }
    return 0;
}
