#include "loader.h"

namespace Loader {

namespace {

constexpr char const* CONFIG_PATH = "/boot/shool.cfg";

bool isSpace(char c) { return c == ' ' or c == '\t' or c == '\r'; }

void copyString(char* dest, uint64_t size, char const* src, uint64_t length) {
    uint64_t i = 0;
    for (; i < length and i + 1 < size; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

bool equals(char const* a, uint64_t aLength, char const* b) {
    uint64_t i = 0;
    for (; i < aLength; i++) {
        if (a[i] != b[i] or b[i] == '\0')
            return false;
    }
    return b[i] == '\0';
}

constexpr uint64_t MAX_CONFIG_SIZE = 16 * 1024;

// A number of 1-5 digits, or -1.
int64_t parseNumber(char const*& p, char const* end) {
    int64_t value = 0;
    int digits = 0;
    while (p < end and *p >= '0' and *p <= '9' and digits < 6) {
        value = value * 10 + (*p++ - '0');
        digits++;
    }
    return digits == 0 or digits > 5 ? -1 : value;
}

// An absolute path of printable characters with no "." or ".." parts.
bool validPath(char const* value, uint64_t length) {
    if (length == 0 or length >= MAX_PATH or value[0] != '/')
        return false;
    uint64_t partStart = 1;
    for (uint64_t i = 1; i <= length; i++) {
        if (i == length or value[i] == '/') {
            uint64_t n = i - partStart;
            if (n == 0 or (n == 1 and value[partStart] == '.') or
                (n == 2 and value[partStart] == '.' and value[partStart + 1] == '.'))
                return false;
            partStart = i + 1;
        } else if (value[i] == '\\' or value[i] < 0x21 or value[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

// lowercase letters, digits and '-', 1-23 of them
bool validName(char const* value, uint64_t length) {
    if (length == 0 or length >= MAX_NAME)
        return false;
    for (uint64_t i = 0; i < length; i++) {
        char c = value[i];
        if (not((c >= 'a' and c <= 'z') or (c >= '0' and c <= '9') or c == '-'))
            return false;
    }
    return true;
}

bool sameString(char const* a, char const* b) {
    while (*a and *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// The whole configuration is checked: anything it doesn't understand stops
// the boot (with the line number), rather than being skipped or cut short.
struct Parser {
    Config& config;
    uint64_t line = 0;
    bool resolutionSet = false, timeoutSet = false;
    // Per entry: what was set explicitly (`like` doesn't count).
    bool kernelSet[Config::MAX_ENTRIES] = {}, modeSet[Config::MAX_ENTRIES] = {},
         diagnosticSet[Config::MAX_ENTRIES] = {}, titleSet[Config::MAX_ENTRIES] = {};
    // Names resolved at the end (an entry may name a later one).
    char fallbackName[Config::MAX_ENTRIES][MAX_NAME] = {};
    uint64_t fallbackLine[Config::MAX_ENTRIES] = {};
    char defaultName[MAX_NAME] = {};
    uint64_t defaultLine = 0;

    [[noreturn]] void error(char const* what) {
        print("  /boot/shool.cfg line ");
        printNumber(line);
        print(": ");
        print(what);
        print("\n");
        fail("the boot configuration has an error");
    }

    int find(char const* name) const {
        for (uint32_t i = 0; i < config.entryCount; i++) {
            if (sameString(config.entries[i].name, name))
                return static_cast<int>(i);
        }
        return -1;
    }

    Entry& newEntry(char const* name, uint64_t length) {
        if (config.entryCount == Config::MAX_ENTRIES)
            error("more than 8 entries");
        Entry& e = config.entries[config.entryCount++];
        e = Entry{};
        copyString(e.name, MAX_NAME, name, length);
        copyString(e.title, sizeof(e.title), name, length);
        e.fallback = -1;
        return e;
    }

    // The entry being described (the old single-entry form makes "default").
    Entry& current() {
        if (config.entryCount == 0)
            newEntry("default", 7);
        return config.entries[config.entryCount - 1];
    }

    void apply(char const* key, uint64_t keyLength, char const* value, uint64_t valueLength) {
        uint32_t index = config.entryCount ? config.entryCount - 1 : 0;
        if (equals(key, keyLength, "entry")) {
            if (not validName(value, valueLength))
                error("an entry's name is lowercase letters, digits and '-' (at most 23)");
            char name[MAX_NAME];
            copyString(name, MAX_NAME, value, valueLength);
            if (find(name) >= 0)
                error("two entries with the same name");
            newEntry(value, valueLength);
        } else if (equals(key, keyLength, "title")) {
            Entry& e = current();
            if (titleSet[index] or valueLength == 0 or valueLength >= sizeof(e.title))
                error("one 'title =' per entry, 1-47 characters");
            copyString(e.title, sizeof(e.title), value, valueLength);
            titleSet[index] = true;
        } else if (equals(key, keyLength, "like")) {
            Entry& e = current();
            char name[MAX_NAME];
            copyString(name, MAX_NAME, value, valueLength);
            int from = validName(value, valueLength) ? find(name) : -1;
            if (from < 0 or static_cast<uint32_t>(from) == config.entryCount - 1)
                error("'like =' names an earlier entry");
            if (e.kernel[0] or e.moduleCount)
                error("'like =' comes before the entry's kernel and modules");
            Entry const& source = config.entries[from];
            for (uint64_t i = 0; i < sizeof(e.kernel); i++)
                e.kernel[i] = source.kernel[i];
            for (uint32_t m = 0; m < source.moduleCount; m++)
                copyString(e.modules[m], MAX_PATH, source.modules[m], MAX_PATH);
            e.moduleCount = source.moduleCount;
        } else if (equals(key, keyLength, "kernel")) {
            Entry& e = current();
            if (kernelSet[index])
                error("a second 'kernel =' in this entry");
            if (not validPath(value, valueLength))
                error("'kernel =' needs an absolute path (no . or .., under 96 characters)");
            copyString(e.kernel, MAX_PATH, value, valueLength);
            kernelSet[index] = true;
        } else if (equals(key, keyLength, "module")) {
            Entry& e = current();
            if (e.moduleCount == Shool::BootInfo::MAX_MODULES)
                error("more than 32 modules");
            if (not validPath(value, valueLength))
                error("'module =' needs an absolute path (no . or .., under 96 characters)");
            for (uint32_t i = 0; i < e.moduleCount; i++) {
                if (equals(value, valueLength, e.modules[i]))
                    error("the same module twice");
            }
            copyString(e.modules[e.moduleCount++], MAX_PATH, value, valueLength);
        } else if (equals(key, keyLength, "mode")) {
            Entry& e = current();
            if (modeSet[index])
                error("a second 'mode =' in this entry");
            if (equals(value, valueLength, "normal"))
                e.mode = Shool::MODE_NORMAL;
            else if (equals(value, valueLength, "safe"))
                e.mode = Shool::MODE_SAFE;
            else if (equals(value, valueLength, "recovery"))
                e.mode = Shool::MODE_RECOVERY;
            else
                error("'mode =' needs normal, safe or recovery");
            modeSet[index] = true;
        } else if (equals(key, keyLength, "fallback")) {
            current();
            if (fallbackName[index][0] or not validName(value, valueLength))
                error("one 'fallback =' per entry, naming an entry");
            copyString(fallbackName[index], MAX_NAME, value, valueLength);
            fallbackLine[index] = line;
        } else if (equals(key, keyLength, "diagnostic")) {
            Entry& e = current();
            if (diagnosticSet[index])
                error("a second 'diagnostic =' in this entry");
            if (equals(value, valueLength, "yes"))
                e.diagnostic = true;
            else if (not equals(value, valueLength, "no"))
                error("'diagnostic =' needs yes or no");
            diagnosticSet[index] = true;
        } else if (config.entryCount and (equals(key, keyLength, "timeout") or equals(key, keyLength, "default") or
                                          equals(key, keyLength, "resolution"))) {
            error("timeout, default and resolution go before the first entry");
        } else if (equals(key, keyLength, "timeout")) {
            char const* p = value;
            int64_t seconds = parseNumber(p, value + valueLength);
            if (timeoutSet or p != value + valueLength or seconds < 0 or seconds > 30)
                error("one 'timeout =', 0 to 30 seconds");
            config.timeout = static_cast<uint32_t>(seconds);
            timeoutSet = true;
        } else if (equals(key, keyLength, "default")) {
            if (defaultName[0] or not validName(value, valueLength))
                error("one 'default =', naming an entry");
            copyString(defaultName, MAX_NAME, value, valueLength);
            defaultLine = line;
        } else if (equals(key, keyLength, "resolution")) {
            if (resolutionSet)
                error("a second 'resolution ='");
            char const* p = value;
            char const* end = value + valueLength;
            int64_t width = parseNumber(p, end);
            int64_t height = -1;
            if (p < end and (*p == 'x' or *p == 'X')) {
                p++;
                height = parseNumber(p, end);
            }
            if (p != end or width < 320 or height < 200 or width > 16384 or height > 16384)
                error("'resolution =' needs WIDTHxHEIGHT, from 320x200 to 16384x16384");
            config.width = static_cast<uint32_t>(width);
            config.height = static_cast<uint32_t>(height);
            resolutionSet = true;
        } else {
            error("unknown setting (timeout, default, resolution, entry, title, like, kernel, module, mode, "
                  "diagnostic, fallback)");
        }
    }

    // After the last line: every entry complete, every name known.
    void finish() {
        if (config.entryCount == 0)
            fail("/boot/shool.cfg describes nothing to start");
        for (uint32_t i = 0; i < config.entryCount; i++) {
            Entry& e = config.entries[i];
            if (not e.kernel[0]) {
                print("  entry ");
                print(e.name);
                print(":\n");
                fail("/boot/shool.cfg: an entry has no kernel");
            }
            if (fallbackName[i][0]) {
                line = fallbackLine[i];
                e.fallback = find(fallbackName[i]);
                if (e.fallback < 0 or static_cast<uint32_t>(e.fallback) == i)
                    error("'fallback =' names another entry");
            }
        }
        if (defaultName[0]) {
            line = defaultLine;
            int d = find(defaultName);
            if (d < 0)
                error("'default =' names no entry");
            config.defaultEntry = static_cast<uint32_t>(d);
        }
    }
};

} // namespace

Efi::File* openBootVolume() {
    Efi::LoadedImage* image = nullptr;
    Efi::Status status = bootServices->handleProtocol(imageHandle, &Efi::LOADED_IMAGE_PROTOCOL,
                                                      reinterpret_cast<void**>(&image));
    if (Efi::failed(status))
        fail("can't find the boot device", status);

    Efi::SimpleFileSystem* fs = nullptr;
    status = bootServices->handleProtocol(image->deviceHandle, &Efi::SIMPLE_FILE_SYSTEM_PROTOCOL,
                                          reinterpret_cast<void**>(&fs));
    if (Efi::failed(status))
        fail("the boot device has no readable file system", status);

    Efi::File* root = nullptr;
    status = fs->openVolume(fs, &root);
    if (Efi::failed(status))
        fail("can't open the boot volume", status);
    return root;
}

bool readFile(Efi::File* root, char const* path, Efi::MemoryType type, FileData& out) {
    // UEFI paths are UTF-16 with backslashes.
    Efi::Char16 path16[MAX_PATH];
    uint64_t i = 0;
    for (; path[i] and i + 1 < MAX_PATH; i++)
        path16[i] = path[i] == '/' ? u'\\' : static_cast<Efi::Char16>(path[i]);
    path16[i] = 0;

    Efi::File* file = nullptr;
    if (Efi::failed(root->open(root, &file, path16, Efi::FILE_MODE_READ, 0)))
        return false;

    // Ask for the info size first, then the info (which holds the file size).
    uint64_t infoSize = 0;
    file->getInfo(file, &Efi::FILE_INFO_ID, &infoSize, nullptr);
    uint8_t infoBuffer[512];
    if (infoSize == 0 or infoSize > sizeof(infoBuffer) or
        Efi::failed(file->getInfo(file, &Efi::FILE_INFO_ID, &infoSize, infoBuffer))) {
        file->close(file);
        return false;
    }
    uint64_t size = reinterpret_cast<Efi::FileInfo*>(infoBuffer)->fileSize;

    uint64_t pages = alignUp(size == 0 ? 1 : size, PAGE_SIZE) / PAGE_SIZE;
    auto* data = reinterpret_cast<uint8_t*>(allocatePages(pages, type));

    uint64_t done = 0;
    while (done < size) {
        uint64_t chunk = size - done;
        if (Efi::failed(file->read(file, &chunk, data + done)) or chunk == 0) {
            file->close(file);
            return false;
        }
        done += chunk;
    }

    file->close(file);
    out = {data, size};
    return true;
}

Config const& readConfig(Efi::File* root) {
    static Config config; // (about 25 KiB: not on the stack)
    config = Config{};

    FileData file;
    if (not readFile(root, CONFIG_PATH, Efi::LOADER_DATA, file)) {
        print("  no /boot/shool.cfg, using defaults\n");
        Entry& e = config.entries[config.entryCount++];
        copyString(e.name, MAX_NAME, "default", 7);
        copyString(e.title, sizeof(e.title), "default", 7);
        copyString(e.kernel, MAX_PATH, "/boot/rudracore.elf", 19);
        e.fallback = -1;
        return config;
    }
    verifyFile(CONFIG_PATH, file);

    Parser parser{config};
    if (file.size > MAX_CONFIG_SIZE)
        fail("/boot/shool.cfg is too big (over 16 KiB)");
    char const* p = reinterpret_cast<char const*>(file.data);
    char const* end = p + file.size;
    while (p < end) {
        parser.line++;
        char const* lineEnd = p;
        while (lineEnd < end and *lineEnd != '\n')
            lineEnd++;

        // Only text: printable ASCII and UTF-8 (in comments), tabs, CR.
        for (char const* c = p; c < lineEnd; c++) {
            auto byte = static_cast<unsigned char>(*c);
            if ((byte < 0x20 and byte != '\t' and byte != '\r') or byte == 0x7F)
                parser.error("a control character");
        }

        // "key = value", ignoring blanks and lines starting with '#'
        char const* s = p;
        while (s < lineEnd and isSpace(*s))
            s++;
        if (s < lineEnd and *s != '#') {
            char const* eq = s;
            while (eq < lineEnd and *eq != '=')
                eq++;
            if (eq == lineEnd)
                parser.error("not a 'key = value' line");
            char const* keyEnd = eq;
            while (keyEnd > s and isSpace(keyEnd[-1]))
                keyEnd--;
            char const* value = eq + 1;
            while (value < lineEnd and isSpace(*value))
                value++;
            char const* valueEnd = lineEnd;
            while (valueEnd > value and isSpace(valueEnd[-1]))
                valueEnd--;
            parser.apply(s, static_cast<uint64_t>(keyEnd - s), value, static_cast<uint64_t>(valueEnd - value));
        }
        p = lineEnd + 1;
    }
    parser.finish();
    return config;
}

void baseName(char* dest, uint64_t size, char const* path) {
    char const* start = path;
    for (char const* p = path; *p; p++) {
        if (*p == '/' or *p == '\\')
            start = p + 1;
    }
    uint64_t i = 0;
    for (; start[i] and start[i] != '.' and i + 1 < size; i++)
        dest[i] = start[i];
    dest[i] = '\0';
}

} // namespace Loader
