// bpk: makes Bhairav OS app packages and catalogs (on the build machine).
//
//   bpk pack out.bpk manifest.txt app.elf [icon.png]
//   bpk tbs in.bpk out.tbs        the bytes to sign: the package, counting
//                                 one more file (the signature to come)
//   bpk attach in.tbs sig out.bpk appends the 64-byte Ed25519 signature as
//                                 the last file, "signature"
//   bpk catalog out.bin a.bpk b.bpk ...
//
// Signing (see the Makefile): openssl pkeyutl -sign -rawin signs the .tbs.
//
// Formats: see userspace/kapalaframe/include/kapala/apps.h.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool readFile(char const* path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path, "rb");
    if (not f) {
        fprintf(stderr, "bpk: can't read %s\n", path);
        return false;
    }
    unsigned char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
        out.insert(out.end(), buffer, buffer + n);
    fclose(f);
    return true;
}

void put32(std::vector<unsigned char>& out, size_t v) {
    for (int i = 0; i < 4; i++)
        out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

bool writeFile(char const* path, std::vector<unsigned char> const& data) {
    FILE* f = fopen(path, "wb");
    if (not f or fwrite(data.data(), 1, data.size(), f) != data.size()) {
        fprintf(stderr, "bpk: can't write %s\n", path);
        return false;
    }
    fclose(f);
    return true;
}

std::string baseName(std::string const& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 5 and strcmp(argv[1], "pack") == 0) {
        std::vector<unsigned char> out = {'B', 'P', 'K', '1'};
        int files = argc - 3;
        put32(out, static_cast<size_t>(files));
        // The files keep fixed names inside the package.
        char const* names[] = {"manifest.txt", "app.elf", "icon.png"};
        for (int i = 0; i < files and i < 3; i++) {
            std::vector<unsigned char> data;
            if (not readFile(argv[3 + i], data))
                return 1;
            char name[32] = {};
            strncpy(name, names[i], sizeof(name) - 1);
            out.insert(out.end(), name, name + 32);
            put32(out, data.size());
            out.insert(out.end(), data.begin(), data.end());
        }
        return writeFile(argv[2], out) ? 0 : 1;
    }
    if (argc == 4 and strcmp(argv[1], "tbs") == 0) {
        std::vector<unsigned char> data;
        if (not readFile(argv[2], data) or data.size() < 8)
            return 1;
        unsigned count = data[4] | data[5] << 8 | data[6] << 16 | static_cast<unsigned>(data[7]) << 24;
        count++;
        for (int i = 0; i < 4; i++)
            data[4 + i] = static_cast<unsigned char>(count >> (8 * i));
        return writeFile(argv[3], data) ? 0 : 1;
    }
    if (argc == 5 and strcmp(argv[1], "attach") == 0) {
        std::vector<unsigned char> data, signature;
        if (not readFile(argv[2], data) or not readFile(argv[3], signature))
            return 1;
        if (signature.size() != 64) {
            fprintf(stderr, "bpk: %s isn't a 64-byte Ed25519 signature\n", argv[3]);
            return 1;
        }
        char name[32] = "signature";
        data.insert(data.end(), name, name + 32);
        put32(data, signature.size());
        data.insert(data.end(), signature.begin(), signature.end());
        return writeFile(argv[4], data) ? 0 : 1;
    }
    if (argc >= 3 and strcmp(argv[1], "catalog") == 0) {
        std::vector<unsigned char> out = {'B', 'C', 'T', '1'};
        put32(out, static_cast<size_t>(argc - 3));
        put32(out, 0); // the total, filled in below
        for (int i = 3; i < argc; i++) {
            std::vector<unsigned char> data;
            if (not readFile(argv[i], data))
                return 1;
            put32(out, data.size());
            out.insert(out.end(), data.begin(), data.end());
            fprintf(stderr, "  %s (%zu KiB)\n", baseName(argv[i]).c_str(), data.size() / 1024);
        }
        size_t total = out.size();
        for (int i = 0; i < 4; i++)
            out[8 + i] = static_cast<unsigned char>(total >> (8 * i));
        return writeFile(argv[2], out) ? 0 : 1;
    }
    fprintf(stderr, "usage: bpk pack out.bpk manifest.txt app.elf [icon.png]\n"
                    "       bpk tbs in.bpk out.tbs\n"
                    "       bpk attach in.tbs signature out.bpk\n"
                    "       bpk catalog out.bin a.bpk b.bpk ...\n");
    return 2;
}
