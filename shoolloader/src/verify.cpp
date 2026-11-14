// Verified boot: ShoolLoader loads only files listed in /boot/shool.sum,
// and only if /boot/shool.sig is that list's Ed25519 signature by the boot
// key built into the loader (build/boot-key.h, from keys/boot-signing.pem).
//
// shool.sum is sha256sum's output under a first line "version N":
//   version 1
//   <64 hex digits>  rudracore.elf
//   <64 hex digits>  shool.cfg
//   ...
// N is the boot image's security version. Once a version has booted, an
// older one is refused: the highest seen is kept in a UEFI variable that
// only boot-time code can change (no runtime access, so not the OS).

#include "loader.h"

#include "boot-key.h"

#include <kapala/crypto.h>

namespace Loader {

namespace {

constexpr char const* SUM_PATH = "/boot/shool.sum";
constexpr char const* SIG_PATH = "/boot/shool.sig";

constexpr Efi::Guid VARIABLE_VENDOR = {0x6b1e7a43, 0x52d1, 0x4c3e, {0x9a, 0x1f, 0x7e, 0x2b, 0x61, 0xd0, 0x3c, 0x85}};
constexpr Efi::Char16 const VARIABLE_NAME[] = u"BhairavBootVersion";
constexpr uint32_t VARIABLE_ATTRIBUTES = Efi::VARIABLE_NON_VOLATILE | Efi::VARIABLE_BOOTSERVICE_ACCESS;

FileData sums{};
uint32_t bootVersion = 0;

int hexDigit(char c) {
    if (c >= '0' and c <= '9')
        return c - '0';
    if (c >= 'a' and c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' and c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// "/boot/init.elf" -> "init.elf"
char const* fileName(char const* path) {
    char const* name = path;
    for (char const* p = path; *p; p++) {
        if (*p == '/' or *p == '\\')
            name = p + 1;
    }
    return name;
}

// The SHA-256 the signed list gives for `name`, or false.
bool listedHash(char const* name, uint8_t out[32]) {
    auto const* text = reinterpret_cast<char const*>(sums.data);
    char const* end = text + sums.size;
    for (char const* line = text; line < end;) {
        char const* lineEnd = line;
        while (lineEnd < end and *lineEnd != '\n')
            lineEnd++;
        // "<64 hex>  name" (sha256sum puts " *" instead of "  " in binary mode)
        if (lineEnd - line > 66 and line[64] == ' ' and (line[65] == ' ' or line[65] == '*')) {
            char const* n = line + 66;
            uint64_t i = 0;
            while (n + i < lineEnd and name[i] and n[i] == name[i])
                i++;
            if (n + i == lineEnd and not name[i]) {
                for (int b = 0; b < 32; b++) {
                    int hi = hexDigit(line[2 * b]), lo = hexDigit(line[2 * b + 1]);
                    if (hi < 0 or lo < 0)
                        return false;
                    out[b] = static_cast<uint8_t>(hi << 4 | lo);
                }
                return true;
            }
        }
        line = lineEnd + 1;
    }
    return false;
}

void refuseRollback() {
    Efi::RuntimeServices* rt = system->runtimeServices;
    uint32_t stored = 0, attributes = 0;
    uint64_t size = sizeof(stored);
    Efi::Status status = rt->getVariable(VARIABLE_NAME, &VARIABLE_VENDOR, &attributes, &size, &stored);
    if (status == Efi::NOT_FOUND) {
        stored = 0; // first boot (or the firmware forgot)
    } else if (Efi::failed(status) or size != sizeof(stored) or attributes != VARIABLE_ATTRIBUTES) {
        // Not ours: ours can't be changed after boot, so a variable with
        // other attributes or size was made by someone else (the OS can).
        // It says nothing; replace it.
        rt->setVariable(VARIABLE_NAME, &VARIABLE_VENDOR, 0, 0, nullptr);
        stored = 0;
    }
    if (bootVersion < stored) {
        print("  boot image version ");
        printNumber(bootVersion);
        print(", but version ");
        printNumber(stored);
        print(" has already booted here\n");
        fail("this boot image is older than one that already booted (rollback refused)");
    }
    if (bootVersion > stored and
        Efi::failed(rt->setVariable(VARIABLE_NAME, &VARIABLE_VENDOR, VARIABLE_ATTRIBUTES, sizeof(bootVersion), &bootVersion)))
        print("  warning: couldn't save the boot version, so rollback can't be refused next time\n");
}

} // namespace

void loadBootManifest(Efi::File* root) {
    FileData signature;
    if (not readFile(root, SUM_PATH, Efi::LOADER_DATA, sums) or not readFile(root, SIG_PATH, Efi::LOADER_DATA, signature))
        fail("the boot files aren't signed (no /boot/shool.sum or /boot/shool.sig)");
    if (signature.size != 64 or not Kapala::Crypto::ed25519Verify(signature.data, sums.data, sums.size, BOOT_KEY))
        fail("the signature of the boot file list is wrong: the boot files were changed");

    auto const* text = reinterpret_cast<char const*>(sums.data);
    if (sums.size < 9 or text[0] != 'v' or text[1] != 'e' or text[2] != 'r' or text[3] != 's' or text[4] != 'i' or
        text[5] != 'o' or text[6] != 'n' or text[7] != ' ')
        fail("the boot file list has no version");
    for (uint64_t i = 8; i < sums.size and text[i] >= '0' and text[i] <= '9' and bootVersion < 100000000; i++)
        bootVersion = bootVersion * 10 + static_cast<uint32_t>(text[i] - '0');
    refuseRollback();

    print("  signed boot files, version ");
    printNumber(bootVersion);
    print("\n");
}

void verifyFile(char const* path, FileData const& file) {
    uint8_t listed[32], actual[32];
    if (not listedHash(fileName(path), listed)) {
        print("  ");
        print(path);
        print(":\n");
        fail("this boot file isn't in the signed list");
    }
    Kapala::Crypto::Sha256::hash(file.data, file.size, actual);
    if (not Kapala::Crypto::equal(listed, actual, 32)) {
        print("  ");
        print(path);
        print(":\n");
        fail("this boot file was changed (its hash doesn't match the signed list)");
    }
}

} // namespace Loader
