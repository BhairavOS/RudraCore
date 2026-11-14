#include <kapala/apps.h>
#include <kapala/apt.h>
#include <kapala/crypto.h>
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
        "tls-test", "fonts", "catalog", "trust", "roots"};
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
            out.permissions |= parsePermissions(v, n);
        else if (same(key, "hosts")) {
            copy(out.hosts, v, n, sizeof(out.hosts));
            if (out.hosts[0])
                out.permissions |= PERMISSION_HOSTS; // listing hosts asks for them
        }
    }
    if (not out.title[0])
        copy(out.title, out.id, length(out.id), sizeof(out.title));
    return out.id[0] != '\0';
}

bool verifyPackage(u8 const* data, u64 len, char const*& error) {
    Package package;
    if (not package.parse(data, len, error))
        return false;
    // The last file must be the signature, covering everything before its entry.
    unsigned count = read32(data + 4);
    u64 at = 8, last = 0;
    for (unsigned i = 0; i < count; i++) {
        last = at;
        at += 36 + read32(data + at + 32);
    }
    if (count < 2 or memcmp(data + last, "signature", 10) != 0 or read32(data + last + 32) != 64 or at != len) {
        error = "the package isn't signed";
        return false;
    }
    if (trustedSignature(data + last + 36, data, last))
        return true;
    error = "the package isn't signed by a trusted publisher (or was changed)";
    return false;
}

bool trustedSignature(u8 const* signature, void const* data, u64 len) {
    static auto const* trust = static_cast<u8 const*>(Sys::openFile("trust"));
    if (not trust or memcmp(trust, "BTK1", 4) != 0)
        return false; // no trusted keys: nothing is trusted
    unsigned keys = read32(trust + 4);
    for (unsigned k = 0; k < keys and k < 16; k++) {
        if (Crypto::ed25519Verify(signature, data, len, trust + 8 + 32 * k))
            return true;
    }
    return false;
}

u8* loadInstalled(Files& files, char const* id, Package& package, u64& len, char const*& error) {
    if (not validId(id)) {
        error = "no such app";
        return nullptr;
    }
    Text<96> path;
    appPath(id, "package.bpk", path);
    u8* data = readWholeFile(files, path.cstr(), len);
    if (not data) {
        error = "its package is missing (installed before packages were kept, or deleted)";
        return nullptr;
    }
    AppInfo installed{};
    if (not verifyPackage(data, len, error) or not package.parse(data, len, error)) {
        // (error says why)
    } else if (not same(package.info().id, id)) {
        error = "its package is another app's";
    } else if (readAppInfo(files, id, installed) != Sys::OK or not same(installed.version, package.info().version)) {
        error = "its manifest doesn't match its package";
    } else {
        return data;
    }
    delete[] data;
    return nullptr;
}

long installPackage(Files& files, u8 const* data, u64 len, AppInfo& out, char const*& error) {
    if (not verifyPackage(data, len, error))
        return Sys::ACCESS_DENIED;
    Package package;
    if (not package.parse(data, len, error))
        return Sys::INVALID_ARGUMENT;
    out = package.info();
    if (isBuiltInProgram(out.id)) {
        error = "that name belongs to a built-in program";
        return Sys::ALREADY_EXISTS;
    }
    // No rollback: an older (signed) version could bring back a fixed hole.
    AppInfo installed{};
    if (readAppInfo(files, out.id, installed) == Sys::OK and Apt::compareVersions(out.version, installed.version) < 0) {
        error = "it's older than the installed version";
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
    // The signed package first: it's what runs. If a write below fails, the
    // manifest no longer matches it and the app refuses to start.
    appPath(out.id, "package.bpk", path);
    r = files.write(path.cstr(), data, len);
    if (r < 0) {
        error = "couldn't write to the disk (is it full?)";
        return r;
    }
    appPath(out.id, "app.elf", path);
    files.remove(path.cstr()); // (older installs ran this, unchecked)
    static constexpr char const* FILES[] = {"manifest.txt", "icon.png"};
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
    // was granted (folder and hosts too) and never gains a permission
    // without the user.
    Grants grants;
    if (update) {
        grants = readGrants(files, out.id);
        grants.permissions &= out.permissions;
    } else {
        grants.permissions = out.permissions;
        copy(grants.hosts, out.hosts, length(out.hosts), sizeof(grants.hosts));
        copy(grants.folder, "/Documents", 10, sizeof(grants.folder));
    }
    if (writeGrants(files, out.id, grants) < 0) {
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
        else if (same(word, "folder"))
            bits |= PERMISSION_FOLDER;
        else if (same(word, "hosts"))
            bits |= PERMISSION_HOSTS;
    }
    return bits;
}

// The file: one permission word per line, then "folder = /path" and
// "hosts = a b" lines.
Grants readGrants(Files& files, char const* id) {
    Grants grants;
    if (not validId(id))
        return grants;
    Text<96> path;
    permissionsPath(id, path);
    char text[320];
    long n = files.read(path.cstr(), text, sizeof(text) - 1);
    if (n <= 0)
        return grants;
    u64 i = 0, len = static_cast<u64>(n);
    while (i < len) {
        u64 start = i;
        while (i < len and text[i] != '\n')
            i++;
        u64 end = i++;
        u64 eq = start;
        while (eq < end and text[eq] != '=')
            eq++;
        if (eq == end) {
            grants.permissions |= parsePermissions(text + start, end - start);
            continue;
        }
        u64 vs = eq + 1;
        while (vs < end and text[vs] == ' ')
            vs++;
        if (end - start > 6 and memcmp(text + start, "folder", 6) == 0)
            copy(grants.folder, text + vs, end - vs, sizeof(grants.folder));
        else if (end - start > 5 and memcmp(text + start, "hosts", 5) == 0)
            copy(grants.hosts, text + vs, end - vs, sizeof(grants.hosts));
    }
    return grants;
}

long writeGrants(Files& files, char const* id, Grants const& grants) {
    if (not validId(id))
        return Sys::INVALID_ARGUMENT;
    files.makeDir("/var");
    files.makeDir(PERMISSIONS_DIR);
    Text<320> text;
    unsigned bits = grants.permissions;
    if (bits & PERMISSION_NETWORK)
        text.add("network\n");
    if (bits & PERMISSION_LOCAL_NETWORK)
        text.add("local-network\n");
    if (bits & PERMISSION_FILES)
        text.add("files\n");
    if (bits & PERMISSION_FOLDER)
        text.add("folder\n");
    if (bits & PERMISSION_HOSTS)
        text.add("hosts\n");
    if (grants.folder[0])
        text.add("folder = ").add(grants.folder).add("\n");
    if (grants.hosts[0])
        text.add("hosts = ").add(grants.hosts).add("\n");
    Text<96> path;
    permissionsPath(id, path);
    long r = files.write(path.cstr(), text.cstr(), length(text.cstr()));
    return r < 0 ? r : Sys::OK;
}

unsigned readPermissions(Files& files, char const* id) { return readGrants(files, id).permissions; }

long writePermissions(Files& files, char const* id, unsigned granted) {
    Grants grants = readGrants(files, id);
    grants.permissions = granted;
    return writeGrants(files, id, grants);
}

long uninstallApp(Files& files, char const* id) {
    if (not validId(id))
        return Sys::INVALID_ARGUMENT;
    Text<96> path;
    static constexpr char const* FILES[] = {"manifest.txt", "package.bpk", "app.elf", "icon.png"};
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
