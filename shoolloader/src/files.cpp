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

uint32_t parseNumber(char const*& p, char const* end) {
    uint32_t value = 0;
    while (p < end and *p >= '0' and *p <= '9')
        value = value * 10 + static_cast<uint32_t>(*p++ - '0');
    return value;
}

void applySetting(Config& config, char const* key, uint64_t keyLength, char const* value, uint64_t valueLength) {
    if (equals(key, keyLength, "kernel")) {
        copyString(config.kernel, Config::MAX_PATH, value, valueLength);
    } else if (equals(key, keyLength, "module")) {
        if (config.moduleCount < Shool::BootInfo::MAX_MODULES)
            copyString(config.modules[config.moduleCount++], Config::MAX_PATH, value, valueLength);
        else
            print("  warning: too many modules in shool.cfg, ignoring the rest\n");
    } else if (equals(key, keyLength, "resolution")) {
        char const* p = value;
        char const* end = value + valueLength;
        config.width = parseNumber(p, end);
        if (p < end and (*p == 'x' or *p == 'X')) {
            p++;
            config.height = parseNumber(p, end);
        }
    } else {
        print("  warning: unknown setting in shool.cfg\n");
    }
}

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

    char const* p = reinterpret_cast<char const*>(file.data);
    char const* end = p + file.size;
    while (p < end) {
        char const* lineEnd = p;
        while (lineEnd < end and *lineEnd != '\n')
            lineEnd++;

        // "key = value", ignoring blanks and lines starting with '#'
        char const* s = p;
        while (s < lineEnd and isSpace(*s))
            s++;
        if (s < lineEnd and *s != '#') {
            char const* eq = s;
            while (eq < lineEnd and *eq != '=')
                eq++;
            if (eq < lineEnd) {
                char const* keyEnd = eq;
                while (keyEnd > s and isSpace(keyEnd[-1]))
                    keyEnd--;
                char const* value = eq + 1;
                while (value < lineEnd and isSpace(*value))
                    value++;
                char const* valueEnd = lineEnd;
                while (valueEnd > value and isSpace(valueEnd[-1]))
                    valueEnd--;
                applySetting(config, s, static_cast<uint64_t>(keyEnd - s), value,
                             static_cast<uint64_t>(valueEnd - value));
            }
        }
        p = lineEnd + 1;
    }
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
