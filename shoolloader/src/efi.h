#pragma once

// Minimal UEFI definitions for ShoolLoader🔱, written from the UEFI
// specification (2.10). Only what the loader uses is declared; tables keep
// every member in spec order, since firmware finds functions by position.
//
// Built with the Windows x86_64 target, so the default calling convention
// is the Microsoft one that UEFI uses.

#include <stdint.h>

namespace Efi {

using Status = uint64_t;
using Handle = void*;
using Event = void*;
using PhysicalAddress = uint64_t;
using VirtualAddress = uint64_t;
using Char16 = char16_t;

constexpr Status SUCCESS = 0;
constexpr Status ERROR_BIT = 1ull << 63;
constexpr Status LOAD_ERROR = ERROR_BIT | 1;
constexpr Status INVALID_PARAMETER = ERROR_BIT | 2;
constexpr Status UNSUPPORTED = ERROR_BIT | 3;
constexpr Status BUFFER_TOO_SMALL = ERROR_BIT | 5;
constexpr Status NOT_FOUND = ERROR_BIT | 14;

inline bool failed(Status s) { return s & ERROR_BIT; }

struct Guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

inline bool operator==(Guid const& a, Guid const& b) {
    if (a.data1 != b.data1 or a.data2 != b.data2 or a.data3 != b.data3)
        return false;
    for (int i = 0; i < 8; i++) {
        if (a.data4[i] != b.data4[i])
            return false;
    }
    return true;
}

struct TableHeader {
    uint64_t signature;
    uint32_t revision;
    uint32_t headerSize;
    uint32_t crc32;
    uint32_t reserved;
};

// MARK: Memory

enum MemoryType : uint32_t {
    RESERVED_MEMORY = 0,
    LOADER_CODE = 1,
    LOADER_DATA = 2,
    BOOT_SERVICES_CODE = 3,
    BOOT_SERVICES_DATA = 4,
    RUNTIME_SERVICES_CODE = 5,
    RUNTIME_SERVICES_DATA = 6,
    CONVENTIONAL_MEMORY = 7,
    UNUSABLE_MEMORY = 8,
    ACPI_RECLAIM_MEMORY = 9,
    ACPI_MEMORY_NVS = 10,
    MEMORY_MAPPED_IO = 11,
    MEMORY_MAPPED_IO_PORT_SPACE = 12,
    PAL_CODE = 13,
    PERSISTENT_MEMORY = 14,

    // Types from 0x80000000 are free for OS loaders: ShoolLoader marks what
    // it allocates so the kernel knows what to keep.
    SHOOL_KERNEL = 0x80000001,
    SHOOL_RECLAIMABLE = 0x80000002,
};

enum AllocateType : uint32_t {
    ALLOCATE_ANY_PAGES = 0,
    ALLOCATE_MAX_ADDRESS = 1,
    ALLOCATE_ADDRESS = 2,
};

struct MemoryDescriptor {
    uint32_t type;
    uint32_t padding;
    PhysicalAddress physicalStart;
    VirtualAddress virtualStart;
    uint64_t numberOfPages;
    uint64_t attribute;
};

// MARK: Console

struct SimpleTextOutput {
    Status (*reset)(SimpleTextOutput*, bool);
    Status (*outputString)(SimpleTextOutput*, Char16 const*);
    void* testString;
    void* queryMode;
    void* setMode;
    Status (*setAttribute)(SimpleTextOutput*, uint64_t);
    Status (*clearScreen)(SimpleTextOutput*);
    void* setCursorPosition;
    void* enableCursor;
    void* mode;
};

struct InputKey {
    uint16_t scanCode;
    Char16 unicodeChar;
};

struct SimpleTextInput {
    Status (*reset)(SimpleTextInput*, bool extendedVerification);
    Status (*readKeyStroke)(SimpleTextInput*, InputKey* key); // NOT_READY when no key is waiting
    Event waitForKey;
};

constexpr Status NOT_READY = ERROR_BIT | 6;

// MARK: Boot services

struct BootServices {
    TableHeader header;

    void* raiseTpl;
    void* restoreTpl;

    Status (*allocatePages)(AllocateType, MemoryType, uint64_t pages, PhysicalAddress*);
    Status (*freePages)(PhysicalAddress, uint64_t pages);
    Status (*getMemoryMap)(uint64_t* mapSize, MemoryDescriptor* map, uint64_t* mapKey,
                           uint64_t* descriptorSize, uint32_t* descriptorVersion);
    Status (*allocatePool)(MemoryType, uint64_t size, void** buffer);
    Status (*freePool)(void* buffer);

    void* createEvent;
    void* setTimer;
    void* waitForEvent;
    void* signalEvent;
    void* closeEvent;
    void* checkEvent;

    void* installProtocolInterface;
    void* reinstallProtocolInterface;
    void* uninstallProtocolInterface;
    Status (*handleProtocol)(Handle, Guid const*, void** interface);
    void* reserved;
    void* registerProtocolNotify;
    void* locateHandle;
    void* locateDevicePath;
    void* installConfigurationTable;

    void* loadImage;
    void* startImage;
    Status (*exit)(Handle image, Status exitStatus, uint64_t exitDataSize, Char16* exitData);
    void* unloadImage;
    Status (*exitBootServices)(Handle image, uint64_t mapKey);

    void* getNextMonotonicCount;
    Status (*stall)(uint64_t microseconds);
    Status (*setWatchdogTimer)(uint64_t timeout, uint64_t code, uint64_t dataSize, Char16* data);

    void* connectController;
    void* disconnectController;

    void* openProtocol;
    void* closeProtocol;
    void* openProtocolInformation;

    void* protocolsPerHandle;
    void* locateHandleBuffer;
    Status (*locateProtocol)(Guid const*, void* registration, void** interface);
    void* installMultipleProtocolInterfaces;
    void* uninstallMultipleProtocolInterfaces;

    void* calculateCrc32;
    void* copyMem;
    void* setMem;
    void* createEventEx;
};

// MARK: Runtime services (only variables are used)

constexpr uint32_t VARIABLE_NON_VOLATILE = 1;
constexpr uint32_t VARIABLE_BOOTSERVICE_ACCESS = 2;
constexpr uint32_t VARIABLE_RUNTIME_ACCESS = 4;

// The vendor of the spec's own variables ("SecureBoot", "SetupMode", ...).
constexpr Guid GLOBAL_VARIABLE = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};

struct RuntimeServices {
    TableHeader header;

    void* getTime;
    void* setTime;
    void* getWakeupTime;
    void* setWakeupTime;
    void* setVirtualAddressMap;
    void* convertPointer;
    Status (*getVariable)(Char16 const* name, Guid const* vendor, uint32_t* attributes, uint64_t* dataSize, void* data);
    void* getNextVariableName;
    Status (*setVariable)(Char16 const* name, Guid const* vendor, uint32_t attributes, uint64_t dataSize,
                          void const* data);
};

// MARK: System table

struct ConfigurationTable {
    Guid vendorGuid;
    void* vendorTable;
};

struct SystemTable {
    TableHeader header;
    Char16* firmwareVendor;
    uint32_t firmwareRevision;
    Handle consoleInHandle;
    SimpleTextInput* conIn;
    Handle consoleOutHandle;
    SimpleTextOutput* conOut;
    Handle standardErrorHandle;
    SimpleTextOutput* stdErr;
    RuntimeServices* runtimeServices;
    BootServices* bootServices;
    uint64_t numberOfTableEntries;
    ConfigurationTable* configurationTable;
};

// MARK: Loaded image

constexpr Guid LOADED_IMAGE_PROTOCOL = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

struct LoadedImage {
    uint32_t revision;
    Handle parentHandle;
    SystemTable* systemTable;
    Handle deviceHandle; // the volume the loader was started from
    void* filePath;
    void* reserved;
    uint32_t loadOptionsSize;
    void* loadOptions;
    void* imageBase;
    uint64_t imageSize;
    MemoryType imageCodeType;
    MemoryType imageDataType;
    void* unload;
};

// MARK: Files

constexpr Guid SIMPLE_FILE_SYSTEM_PROTOCOL = {0x964E5B22, 0x6459, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
constexpr Guid FILE_INFO_ID = {0x09576E92, 0x6D3F, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

constexpr uint64_t FILE_MODE_READ = 1;

struct Time {
    uint16_t year;
    uint8_t month, day, hour, minute, second, pad1;
    uint32_t nanosecond;
    int16_t timeZone;
    uint8_t daylight, pad2;
};

struct FileInfo {
    uint64_t size;
    uint64_t fileSize;
    uint64_t physicalSize;
    Time createTime;
    Time lastAccessTime;
    Time modificationTime;
    uint64_t attribute;
    Char16 fileName[1];
};

struct File {
    uint64_t revision;
    Status (*open)(File*, File** newHandle, Char16 const* fileName, uint64_t openMode, uint64_t attributes);
    Status (*close)(File*);
    void* deleteFile;
    Status (*read)(File*, uint64_t* bufferSize, void* buffer);
    void* write;
    void* getPosition;
    void* setPosition;
    Status (*getInfo)(File*, Guid const* infoType, uint64_t* bufferSize, void* buffer);
    void* setInfo;
    void* flush;
};

struct SimpleFileSystem {
    uint64_t revision;
    Status (*openVolume)(SimpleFileSystem*, File** root);
};

// MARK: Graphics

constexpr Guid GRAPHICS_OUTPUT_PROTOCOL = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};

enum PixelFormat : uint32_t {
    PIXEL_RGB_RESERVED_8BIT = 0, // bytes R, G, B, x
    PIXEL_BGR_RESERVED_8BIT = 1, // bytes B, G, R, x
    PIXEL_BIT_MASK = 2,
    PIXEL_BLT_ONLY = 3, // no framebuffer
};

struct PixelBitmask {
    uint32_t red, green, blue, reserved;
};

struct GraphicsModeInfo {
    uint32_t version;
    uint32_t horizontalResolution;
    uint32_t verticalResolution;
    PixelFormat pixelFormat;
    PixelBitmask pixelInformation;
    uint32_t pixelsPerScanLine;
};

struct GraphicsMode {
    uint32_t maxMode;
    uint32_t mode;
    GraphicsModeInfo* info;
    uint64_t sizeOfInfo;
    PhysicalAddress frameBufferBase;
    uint64_t frameBufferSize;
};

struct GraphicsOutput {
    Status (*queryMode)(GraphicsOutput*, uint32_t mode, uint64_t* sizeOfInfo, GraphicsModeInfo** info);
    Status (*setMode)(GraphicsOutput*, uint32_t mode);
    void* blt;
    GraphicsMode* mode;
};

// MARK: ACPI

constexpr Guid ACPI_20_TABLE = {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
constexpr Guid ACPI_10_TABLE = {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

} // namespace Efi
