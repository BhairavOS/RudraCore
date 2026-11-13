#include <kapala/apps.h>
#include <kapala/text.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
int memcmp(void const* a, void const* b, unsigned long n);
}

namespace Kapala {

namespace {

using u8 = unsigned char;
using u64 = unsigned long;

u64 length(char const* s) {
    u64 n = 0;
    while (s[n])
        n++;
    return n;
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

unsigned read32(u8 const* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<unsigned>(p[3]) << 24; }

bool validId(char const* id) {
    u64 n = length(id);
    if (n == 0 or n >= sizeof(AppInfo::id) or id[0] == '-')
        return false;
    for (u64 i = 0; i < n; i++) {
        char c = id[i];
        if (not ((c >= 'a' and c <= 'z') or (c >= '0' and c <= '9') or c == '-'))
            return false;
    }
    return true;
}

void appPath(char const* id, char const* file, Text<96>& out) {
    out = {};
    out.add("/apps/").add(id);
    if (file)
        out.add("/").add(file);
}

void permissionsPath(char const* id, Text<96>& out) {
    out = {};
    out.add(PERMISSIONS_DIR).add("/").add(id);
}

// Removes a directory and everything in it.
long removeTree(Files& files, char const* path, int depth = 0) {
    FileInfo info{};
    long r = files.stat(path, info);
    if (r < 0)
        return r;
    if (info.directory and depth < 8) {
        static FileEntry entries[8][32]; // one batch per level
        for (;;) {
            long n = files.list(path, entries[depth], 32);
            if (n <= 0)
                break;
            long removed = 0;
            for (long i = 0; i < n; i++) {
                Text<96> child;
                child.add(path).add("/").add(entries[depth][i].name);
                removed += removeTree(files, child.cstr(), depth + 1) >= 0;
            }
            if (n < 32 or removed == 0) // (stuck: don't list the same entries forever)
                break;
        }
    }
    return files.remove(path);
}

} // namespace

bool isBuiltInProgram(char const* id) {
    static constexpr char const* BUILT_IN[] = {
        "init", "ps2", "ahci", "fs", "e1000", "rtl8139", "net", "kamalshell", "browser", "files", "terminal",
        "notes", "monitor", "about", "store", "ipc-test", "protect-test", "oom-test", "harden-test", "fpu-test", "fs-test", "net-test",
        "tls-test", "fonts", "catalog"};
    for (char const* name : BUILT_IN) {
        if (same(name, id))
            return true;
    }
    return false;
}

bool Package::parse(u8 const* data, u64 len, char const*& error) {
    _data = data;
    _length = len;
    if (len < 8 or memcmp(data, "BPK1", 4) != 0) {
        error = "not a Bhairav OS package (.bpk)";
        return false;
    }
    _count = read32(data + 4);
    // Every file must lie inside the package.
    u64 at = 8;
    for (unsigned i = 0; i < _count; i++) {
        if (at + 36 > len) {
            error = "the package is cut short";
            return false;
        }
        u64 size = read32(data + at + 32);
        at += 36;
        if (size > len - at) {
            error = "the package is cut short";
            return false;
        }
        at += size;
    }
    u64 size = 0;
    u8 const* manifest = file("manifest.txt", size);
    if (not manifest or not parseManifest(reinterpret_cast<char const*>(manifest), size, _info)) {
        error = "the package has no valid manifest.txt";
        return false;
    }
    if (not validId(_info.id)) {
        error = "the app's id must be lowercase letters, digits and '-'";
        return false;
    }
    u8 const* elf = file("app.elf", size);
    if (not elf or size < 64 or memcmp(elf, "\x7f" "ELF", 4) != 0 or elf[4] != 2) {
        error = "the package has no 64-bit program (app.elf)";
        return false;
    }
    return true;
}

u8 const* Package::file(char const* name, u64& size) const {
    u64 at = 8;
    for (unsigned i = 0; i < _count and at + 36 <= _length; i++) {
        char entry[33] = {};
        memcpy(entry, _data + at, 32);
        u64 n = read32(_data + at + 32);
        at += 36;
        if (n > _length - at)
            return nullptr;
        if (same(entry, name)) {
            size = n;
            return _data + at;
        }
        at += n;
    }
    return nullptr;
}

bool parseManifest(char const* text, u64 len, AppInfo& out) {
    out = AppInfo{};
    u64 i = 0;
    while (i < len) {
        u64 start = i;
        while (i < len and text[i] != '\n')
            i++;
        u64 end = i;
        i++;
        // "key = value", spaces around both trimmed
        u64 eq = start;
        while (eq < end and text[eq] != '=')
            eq++;
        if (eq >= end)
            continue;
        u64 ks = start, ke = eq, vs = eq + 1, ve = end;
        while (ks < ke and (text[ks] == ' ' or text[ks] == '\t'))
            ks++;
        while (ke > ks and (text[ke - 1] == ' ' or text[ke - 1] == '\t'))
            ke--;
        while (vs < ve and (text[vs] == ' ' or text[vs] == '\t'))
            vs++;
        while (ve > vs and (text[ve - 1] == ' ' or text[ve - 1] == '\t' or text[ve - 1] == '\r'))
            ve--;
        char key[16] = {};
        copy(key, text + ks, ke - ks, sizeof(key));
        char const* v = text + vs;
        u64 n = ve - vs;
        if (same(key, "id"))
            copy(out.id, v, n, sizeof(out.id));
        else if (same(key, "title"))
            copy(out.title, v, n, sizeof(out.title));
        else if (same(key, "version"))
            copy(out.version, v, n, sizeof(out.version));
        else if (same(key, "author"))
            copy(out.author, v, n, sizeof(out.author));
        else if (same(key, "description"))
            copy(out.description, v, n, sizeof(out.description));
        else if (same(key, "permissions"))
            out.permissions = parsePermissions(v, n);
    }
    if (not out.title[0])
        copy(out.title, out.id, length(out.id), sizeof(out.title));
    return out.id[0] != '\0';
}

long installPackage(Files& files, u8 const* data, u64 len, AppInfo& out, char const*& error) {
    Package package;
    if (not package.parse(data, len, error))
        return Sys::INVALID_ARGUMENT;
    out = package.info();
    if (isBuiltInProgram(out.id)) {
        error = "that name belongs to a built-in program";
        return Sys::ALREADY_EXISTS;
    }

    files.makeDir("/apps");
    Text<96> path;
    appPath(out.id, "manifest.txt", path);
    FileInfo existing{};
    bool update = files.stat(path.cstr(), existing) == Sys::OK;
    appPath(out.id, nullptr, path);
    long r = files.makeDir(path.cstr());
    if (r < 0 and r != Sys::ALREADY_EXISTS) {
        error = "couldn't make the app's folder";
        return r;
    }
    static constexpr char const* FILES[] = {"manifest.txt", "app.elf", "icon.png"};
    for (char const* name : FILES) {
        u64 size = 0;
        u8 const* content = package.file(name, size);
        appPath(out.id, name, path);
        if (not content) {
            files.remove(path.cstr()); // (an update may have dropped its icon)
            continue;
        }
        r = files.write(path.cstr(), content, size);
        if (r < 0) {
            error = "couldn't write to the disk (is it full?)";
            return r;
        }
    }
    // A new install gets what its manifest asks for; an update keeps what
    // was granted and never gains a permission without the user.
    unsigned granted = update ? readPermissions(files, out.id) & out.permissions : out.permissions;
    if (writePermissions(files, out.id, granted) < 0) {
        error = "couldn't save the app's permissions";
        return Sys::IO_ERROR;
    }
    return Sys::OK;
}

unsigned parsePermissions(char const* text, u64 len) {
    unsigned bits = 0;
    u64 i = 0;
    while (i < len) {
        while (i < len and (text[i] == ' ' or text[i] == ',' or text[i] == '\n' or text[i] == '\r' or text[i] == '\t'))
            i++;
        u64 start = i;
        while (i < len and text[i] != ' ' and text[i] != ',' and text[i] != '\n' and text[i] != '\r' and text[i] != '\t')
            i++;
        char word[16] = {};
        copy(word, text + start, i - start, sizeof(word));
        if (same(word, "network"))
            bits |= PERMISSION_NETWORK;
        else if (same(word, "local-network"))
            bits |= PERMISSION_LOCAL_NETWORK;
        else if (same(word, "files"))
            bits |= PERMISSION_FILES;
    }
    return bits;
}

unsigned readPermissions(Files& files, char const* id) {
    if (not validId(id))
        return 0;
    Text<96> path;
    permissionsPath(id, path);
    char text[128];
    long n = files.read(path.cstr(), text, sizeof(text));
    return n > 0 ? parsePermissions(text, static_cast<u64>(n)) : 0;
}

long writePermissions(Files& files, char const* id, unsigned granted) {
    if (not validId(id))
        return Sys::INVALID_ARGUMENT;
    files.makeDir("/var");
    files.makeDir(PERMISSIONS_DIR);
    Text<128> text;
    if (granted & PERMISSION_NETWORK)
        text.add("network\n");
    if (granted & PERMISSION_LOCAL_NETWORK)
        text.add("local-network\n");
    if (granted & PERMISSION_FILES)
        text.add("files\n");
    Text<96> path;
    permissionsPath(id, path);
    long r = files.write(path.cstr(), text.cstr(), length(text.cstr()));
    return r < 0 ? r : Sys::OK;
}

long uninstallApp(Files& files, char const* id) {
    if (not validId(id))
        return Sys::INVALID_ARGUMENT;
    Text<96> path;
    static constexpr char const* FILES[] = {"manifest.txt", "app.elf", "icon.png"};
    for (char const* name : FILES) {
        appPath(id, name, path);
        files.remove(path.cstr());
    }
    permissionsPath(id, path);
    files.remove(path.cstr());
    path = {};
    path.add("/appdata/").add(id);
    removeTree(files, path.cstr()); // its private storage
    appPath(id, nullptr, path);
    return files.remove(path.cstr());
}

long readAppInfo(Files& files, char const* id, AppInfo& out) {
    Text<96> path;
    appPath(id, "manifest.txt", path);
    char text[1024];
    long n = files.read(path.cstr(), text, sizeof(text));
    if (n < 0)
        return n;
    if (not parseManifest(text, static_cast<u64>(n), out) or not same(out.id, id))
        return Sys::INVALID_ARGUMENT;
    return Sys::OK;
}

long listApps(Files& files, AppInfo* out, long capacity) {
    static FileEntry entries[64];
    long n = files.list("/apps", entries, 64);
    if (n < 0)
        return n == Sys::NOT_FOUND ? 0 : n;
    long count = 0;
    for (long i = 0; i < n and count < capacity; i++) {
        if (entries[i].directory and validId(entries[i].name) and readAppInfo(files, entries[i].name, out[count]) == Sys::OK)
            count++;
    }
    return count;
}

u8* readWholeFile(Files& files, char const* path, u64& len, u64 limit) {
    FileInfo info{};
    if (files.stat(path, info) < 0 or info.directory or info.size == 0 or info.size > limit)
        return nullptr;
    auto* data = new u8[info.size];
    if (not data)
        return nullptr;
    long n = files.read(path, data, info.size);
    if (n != static_cast<long>(info.size)) {
        delete[] data;
        return nullptr;
    }
    len = info.size;
    return data;
}

} // namespace Kapala
