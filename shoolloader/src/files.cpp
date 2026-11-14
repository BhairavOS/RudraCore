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
    if (length == 0 or length >= Config::MAX_PATH or value[0] != '/')
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

// The whole configuration is checked: anything it doesn't understand stops
// the boot (with the line number), rather than being skipped or cut short.
struct Parser {
    Config& config;
    uint64_t line = 0;
    bool kernelSet = false, resolutionSet = false, diagnosticSet = false;

    [[noreturn]] void error(char const* what) {
        print("  /boot/shool.cfg line ");
        printNumber(line);
        print(": ");
        print(what);
        print("\n");
        fail("the boot configuration has an error");
    }

    void apply(char const* key, uint64_t keyLength, char const* value, uint64_t valueLength) {
        if (equals(key, keyLength, "kernel")) {
            if (kernelSet)
                error("a second 'kernel ='");
            if (not validPath(value, valueLength))
                error("'kernel =' needs an absolute path (no . or .., under 96 characters)");
            copyString(config.kernel, Config::MAX_PATH, value, valueLength);
            kernelSet = true;
        } else if (equals(key, keyLength, "module")) {
            if (config.moduleCount == Shool::BootInfo::MAX_MODULES)
                error("more than 32 modules");
            if (not validPath(value, valueLength))
                error("'module =' needs an absolute path (no . or .., under 96 characters)");
            for (uint32_t i = 0; i < config.moduleCount; i++) {
                if (equals(value, valueLength, config.modules[i]))
                    error("the same module twice");
            }
            copyString(config.modules[config.moduleCount++], Config::MAX_PATH, value, valueLength);
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
        } else if (equals(key, keyLength, "diagnostic")) {
            if (diagnosticSet)
                error("a second 'diagnostic ='");
            if (equals(value, valueLength, "yes"))
                config.diagnostic = true;
            else if (not equals(value, valueLength, "no"))
                error("'diagnostic =' needs yes or no");
            diagnosticSet = true;
        } else {
            error("unknown setting (kernel, module, resolution, diagnostic)");
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
    Efi::Char16 path16[Config::MAX_PATH];
    uint64_t i = 0;
    for (; path[i] and i + 1 < Config::MAX_PATH; i++)
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

Config readConfig(Efi::File* root) {
    Config config{};
    copyString(config.kernel, Config::MAX_PATH, "/boot/rudracore.elf", 19);

    FileData file;
    if (not readFile(root, CONFIG_PATH, Efi::LOADER_DATA, file)) {
        print("  no /boot/shool.cfg, using defaults\n");
        return config;
    }
    verifyFile(CONFIG_PATH, file);
    config.kernel[0] = '\0'; // (a configuration must name its kernel)

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
    if (not parser.kernelSet)
        fail("/boot/shool.cfg has no 'kernel =' line");
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
