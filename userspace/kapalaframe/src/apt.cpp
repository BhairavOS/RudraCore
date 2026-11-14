#include <kapala/apt.h>
#include <kapala/crypto.h>
#include <kapala/http.h>
#include <kapala/text.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
int memcmp(void const* a, void const* b, unsigned long n);
}

namespace Kapala::Apt {

namespace {

using u8 = unsigned char;
using u64 = unsigned long;

u64 length(char const* s) {
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

bool startsWith(char const* text, char const* prefix) {
    for (; *prefix; text++, prefix++) {
        if (*text != *prefix)
            return false;
    }
    return true;
}

bool same(char const* a, char const* b) {
    while (*a and *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void copy(char* dest, char const* src, u64 n, u64 capacity) {
    if (n >= capacity)
        n = capacity - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
}

void copyString(char* dest, char const* src, u64 capacity) { copy(dest, src, length(src), capacity); }

constexpr long MAX_SOURCES = 16;

// The address with no trailing slash (so "x/" and "x" are the same source).
void normalize(char const* address, char* out) {
    copyString(out, address, MAX_SOURCE);
    u64 n = length(out);
    while (n > 0 and (out[n - 1] == '/' or out[n - 1] == ' ' or out[n - 1] == '\r'))
        out[--n] = '\0';
}

void listPath(long index, Text<96>& out) {
    out = {};
    out.add(LISTS).add("/source-").add(index).add(".txt");
}

// Calls take(key, keyLength, value, valueLength) for each "key = value" line,
// and block() at each blank line (and the end).
template <typename Take, typename Block>
void readBlocks(char const* text, u64 len, Take take, Block block) {
    u64 i = 0;
    bool any = false;
    while (i <= len) {
        u64 start = i;
        while (i < len and text[i] != '\n')
            i++;
        u64 end = i;
        i++;
        while (end > start and (text[end - 1] == '\r' or text[end - 1] == ' '))
            end--;
        if (end == start) { // blank: the block ends
            if (any)
                block();
            any = false;
            if (i > len)
                break;
            continue;
        }
        u64 eq = start;
        while (eq < end and text[eq] != '=')
            eq++;
        if (eq < end) {
            u64 ke = eq, vs = eq + 1;
            while (ke > start and text[ke - 1] == ' ')
                ke--;
            while (vs < end and text[vs] == ' ')
                vs++;
            take(text + start, ke - start, text + vs, end - vs);
            any = true;
        }
    }
    if (any)
        block();
}

bool keyIs(char const* key, u64 n, char const* name) { return n == length(name) and memcmp(key, name, n) == 0; }

u64 number(char const* s, u64 n) {
    u64 v = 0;
    for (u64 i = 0; i < n and s[i] >= '0' and s[i] <= '9' and v < 1'000'000'000'000'000'000ul; i++)
        v = v * 10 + static_cast<u64>(s[i] - '0');
    return v;
}

void toHex(u8 const digest[32], char out[65]) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = DIGITS[digest[i] >> 4];
        out[2 * i + 1] = DIGITS[digest[i] & 15];
    }
    out[64] = '\0';
}

// Where a repository's highest index serial is kept: SERIALS/<16 hex digits
// of its address's SHA-256> (any address fits a file name that way).
void serialPath(char const* address, Text<96>& out) {
    u8 digest[32];
    Crypto::Sha256::hash(address, length(address), digest);
    char hex[65];
    toHex(digest, hex);
    hex[16] = '\0';
    out = {};
    out.add(SERIALS).add("/").add(hex);
}

u64 readSerial(Files& files, char const* address) {
    Text<96> path;
    serialPath(address, path);
    char text[24];
    long n = files.read(path.cstr(), text, sizeof(text));
    return n > 0 ? number(text, static_cast<u64>(n)) : 0;
}

long writeSerial(Files& files, char const* address, u64 serial) {
    files.makeDir(SERIALS);
    Text<96> path;
    serialPath(address, path);
    Text<24> text;
    text.add(serial);
    return files.write(path.cstr(), text.cstr(), text.length);
}

} // namespace

long sources(Files& files, char (*out)[MAX_SOURCE], long capacity) {
    long count = 0;
    if (capacity > 0)
        copyString(out[count++], BUILTIN, MAX_SOURCE);
    char text[4096];
    long n = files.read(SOURCES, text, sizeof(text) - 1);
    if (n <= 0)
        return count;
    for (long i = 0; i < n and count < capacity;) {
        long start = i;
        while (i < n and text[i] != '\n')
            i++;
        long end = i++;
        while (start < end and (text[start] == ' ' or text[start] == '\t'))
            start++;
        if (end > start and text[start] != '#') {
            char line[MAX_SOURCE];
            copy(line, text + start, static_cast<u64>(end - start), sizeof(line));
            normalize(line, out[count]);
            if (out[count][0])
                count++;
        }
    }
    return count;
}

namespace {

long writeSources(Files& files, char (*list)[MAX_SOURCE], long count) {
    files.makeDir("/etc");
    files.makeDir("/etc/apt");
    Text<4096> text;
    text.add("# apt repositories: one web address per line\n");
    for (long i = 1; i < count; i++) // (not BUILTIN: it's always there)
        text.add(list[i]).add("\n");
    long r = files.write(SOURCES, text.cstr(), text.length);
    return r < 0 ? r : Sys::OK;
}

} // namespace

long addSource(Files& files, char const* address) {
    char normal[MAX_SOURCE];
    normalize(address, normal);
    if (not (startsWith(normal, "http://") or startsWith(normal, "https://")))
        return Sys::INVALID_ARGUMENT;
    static char list[MAX_SOURCES][MAX_SOURCE];
    long count = sources(files, list, MAX_SOURCES);
    for (long i = 0; i < count; i++) {
        if (same(list[i], normal))
            return Sys::ALREADY_EXISTS;
    }
    if (count >= MAX_SOURCES)
        return Sys::NO_SPACE;
    copyString(list[count++], normal, MAX_SOURCE);
    return writeSources(files, list, count);
}

long removeSource(Files& files, char const* address) {
    char normal[MAX_SOURCE];
    normalize(address, normal);
    static char list[MAX_SOURCES][MAX_SOURCE];
    long count = sources(files, list, MAX_SOURCES);
    long kept = 1;
    bool found = false;
    for (long i = 1; i < count; i++) {
        if (same(list[i], normal))
            found = true;
        else
            copyString(list[kept++], list[i], MAX_SOURCE);
    }
    if (not found)
        return Sys::NOT_FOUND;
    return writeSources(files, list, kept);
}

long update(Files& files, Net& net, UpdateReport& report) {
    static char list[MAX_SOURCES][MAX_SOURCE];
    long count = sources(files, list, MAX_SOURCES);
    files.makeDir("/var");
    files.makeDir("/var/apt");
    files.makeDir(LISTS);

    // Forget old lists (a removed repository shouldn't linger).
    for (long i = 1; i < MAX_SOURCES; i++) {
        Text<96> path;
        listPath(i, path);
        files.remove(path.cstr());
    }

    long total = 0;
    for (long i = 0; i < count; i++) {
        if (same(list[i], BUILTIN)) {
            long n = 0;
            forEachInCatalog(static_cast<u8 const*>(Sys::openFile("catalog")), [&](u8 const*, u64) { n++; });
            report.source(BUILTIN, n, nullptr);
            total += n;
            continue;
        }
        Text<MAX_SOURCE + 16> address;
        address.add(list[i]).add("/Packages");
        u8* data = nullptr;
        u64 size = 0;
        char const* why = "";
        if (httpGet(net, address.cstr(), data, size, why, 1024 * 1024) < 0) {
            report.source(list[i], -1, why);
            continue;
        }
        // It must be signed by a trusted key, and not older than the last one.
        address.add(".sig");
        u8* signature = nullptr;
        u64 signatureSize = 0;
        bool signedOk = httpGet(net, address.cstr(), signature, signatureSize, why, 1024) >= 0 and
                        signatureSize == 64 and trustedSignature(signature, data, size);
        delete[] signature;
        long n = 0;
        u64 serial = 0;
        readBlocks(
            reinterpret_cast<char const*>(data), size,
            [&](char const* key, u64 kn, char const* value, u64 vn) {
                if (keyIs(key, kn, "serial"))
                    serial = number(value, vn);
                else if (keyIs(key, kn, "id"))
                    n++;
            },
            [] {});
        char const* refused = nullptr;
        if (not signedOk)
            refused = "its index isn't signed by a trusted publisher (Packages.sig missing or wrong)";
        else if (serial == 0)
            refused = "its index has no serial";
        else if (serial < readSerial(files, list[i]))
            refused = "its index is older than one already read (an old copy replayed?)";
        else if (writeSerial(files, list[i], serial) < 0)
            refused = "couldn't save its index serial";
        if (refused) {
            delete[] data;
            report.source(list[i], -1, refused);
            continue;
        }
        // Keep it with the address it came from on the first line.
        Text<MAX_SOURCE + 16> header;
        header.add("source = ").add(list[i]).add("\n\n");
        Text<96> path;
        listPath(i, path);
        files.write(path.cstr(), header.cstr(), header.length);
        files.append(path.cstr(), data, size);
        delete[] data;
        report.source(list[i], n, nullptr);
        total += n;
    }
    return total;
}

long available(Files& files, Available* out, long capacity) {
    long count = 0;
    // The built-in catalog.
    forEachInCatalog(static_cast<u8 const*>(Sys::openFile("catalog")), [&](u8 const* data, u64 size) {
        Package p;
        char const* why;
        if (count >= capacity or not p.parse(data, size, why))
            return;
        Available& a = out[count++];
        a.info = p.info();
        copyString(a.source, BUILTIN, sizeof(a.source));
        copyString(a.file, p.info().id, sizeof(a.file));
    });
    // The downloaded lists.
    static char text[64 * 1024];
    for (long i = 1; i < MAX_SOURCES and count < capacity; i++) {
        Text<96> path;
        listPath(i, path);
        long n = files.read(path.cstr(), text, sizeof(text) - 1);
        if (n <= 0)
            continue;
        char source[MAX_SOURCE] = {};
        Available current{};
        readBlocks(
            text, static_cast<u64>(n),
            [&](char const* key, u64 kn, char const* value, u64 vn) {
                if (keyIs(key, kn, "source"))
                    copy(source, value, vn, sizeof(source));
                else if (keyIs(key, kn, "id"))
                    copy(current.info.id, value, vn, sizeof(current.info.id));
                else if (keyIs(key, kn, "title"))
                    copy(current.info.title, value, vn, sizeof(current.info.title));
                else if (keyIs(key, kn, "version"))
                    copy(current.info.version, value, vn, sizeof(current.info.version));
                else if (keyIs(key, kn, "author"))
                    copy(current.info.author, value, vn, sizeof(current.info.author));
                else if (keyIs(key, kn, "description"))
                    copy(current.info.description, value, vn, sizeof(current.info.description));
                else if (keyIs(key, kn, "file"))
                    copy(current.file, value, vn, sizeof(current.file));
                else if (keyIs(key, kn, "sha256"))
                    copy(current.sha256, value, vn, sizeof(current.sha256));
            },
            [&] {
                if (current.info.id[0] and current.file[0] and count < capacity) {
                    if (not current.info.title[0])
                        copyString(current.info.title, current.info.id, sizeof(current.info.title));
                    current.source[0] = '\0';
                    copyString(current.source, source, sizeof(current.source));
                    out[count++] = current;
                }
                current = Available{};
            });
    }
    return count;
}

bool find(Files& files, char const* id, Available& out) {
    static Available all[64];
    long n = available(files, all, 64);
    bool found = false;
    for (long i = 0; i < n; i++) {
        if (same(all[i].info.id, id) and (not found or compareVersions(all[i].info.version, out.info.version) > 0)) {
            out = all[i];
            found = true;
        }
    }
    return found;
}

long install(Files& files, Net& net, Available const& package, AppInfo& installed, char const*& error) {
    if (same(package.source, BUILTIN)) {
        long result = Sys::NOT_FOUND;
        error = "it's no longer in the catalog";
        forEachInCatalog(static_cast<u8 const*>(Sys::openFile("catalog")), [&](u8 const* data, u64 size) {
            Package p;
            char const* why;
            if (result == Sys::NOT_FOUND and p.parse(data, size, why) and same(p.info().id, package.info.id))
                result = installPackage(files, data, size, installed, error);
        });
        return result;
    }
    Text<MAX_SOURCE + 80> address;
    address.add(package.source).add("/").add(package.file);
    u8* data = nullptr;
    u64 size = 0;
    long r = httpGet(net, address.cstr(), data, size, error);
    if (r < 0)
        return r;
    // It must be exactly the package the signed index promised: not another
    // app, and not another (older, signed) version of it.
    u8 digest[32];
    char hex[65];
    Crypto::Sha256::hash(data, size, digest);
    toHex(digest, hex);
    Package p;
    if (not same(hex, package.sha256)) {
        error = "the package isn't the one the repository's signed index lists (run apt update)";
        r = Sys::INVALID_ARGUMENT;
    } else if (not p.parse(data, size, error)) {
        r = Sys::INVALID_ARGUMENT;
    } else if (not same(p.info().id, package.info.id) or not same(p.info().version, package.info.version)) {
        error = "the package isn't the app the repository's index promised";
        r = Sys::INVALID_ARGUMENT;
    } else {
        r = installPackage(files, data, size, installed, error);
    }
    delete[] data;
    return r;
}

int compareVersions(char const* a, char const* b) {
    while (*a or *b) {
        unsigned long x = 0, y = 0;
        while (*a >= '0' and *a <= '9')
            x = x * 10 + static_cast<unsigned long>(*a++ - '0');
        while (*b >= '0' and *b <= '9')
            y = y * 10 + static_cast<unsigned long>(*b++ - '0');
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.' or *a == '-')
            a++;
        else if (*a)
            a++; // (other characters: skipped)
        if (*b == '.' or *b == '-')
            b++;
        else if (*b)
            b++;
    }
    return 0;
}

} // namespace Kapala::Apt
