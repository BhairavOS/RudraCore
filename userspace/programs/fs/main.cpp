// fs: the file system server. Serves files from a FAT32 disk, so the same
// disk can be read and written by Windows, Linux and macOS too.
//
// Handles from init:
//   1: a channel to the disk driver (protocol/block.h)
//   2: the connect channel: each message is a Connect with a new client's
//      channel attached (protocol/fs.h)
//
// Supports long file names, subdirectories, and a disk that is either a
// whole FAT32 volume, has one in its first MBR partition, or has a GPT with
// a Bhairav data partition (the EFI System Partition is never served). Metadata goes
// through a small sector cache; every change is written back (and the disk
// flushed) before the reply.

#include <kapala/channel.h>
#include <kapala/protocol/block.h>
#include <kapala/protocol/fs.h>
#include <kapala/sys.h>
#include <kapala/text.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memset(void* dest, int value, unsigned long n);
int memcmp(void const* a, void const* b, unsigned long n);
}

using namespace Kapala;
namespace Block = Kapala::Protocol::Block;
namespace Fs = Kapala::Protocol::Fs;

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
using u64 = unsigned long;

constexpr Sys::Handle DISK = 1;
constexpr Sys::Handle CONNECT = 2;

constexpr u32 SECTOR = 512;
constexpr int MAX_CLIENTS = 16;

void log(char const* message) { Sys::log(message); }

u16 get16(u8 const* p) { return static_cast<u16>(p[0] | p[1] << 8); }
u32 get32(u8 const* p) { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }
void put16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
void put32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++)
        p[i] = static_cast<u8>(v >> (8 * i));
}

char upper(char c) { return c >= 'a' and c <= 'z' ? static_cast<char>(c - 32) : c; }
char lower(char c) { return c >= 'A' and c <= 'Z' ? static_cast<char>(c + 32) : c; }

unsigned long length(char const* s) {
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

bool sameName(char const* a, char const* b) {
    while (*a and *b) {
        if (upper(*a++) != upper(*b++))
            return false;
    }
    return *a == *b;
}

// MARK: Disk and sector cache

class Disk {
public:
    bool attach() {
        _channel = Channel(DISK);
        long memory = Sys::memoryCreate(Block::BUFFER_SIZE);
        _buffer = memory > 0 ? static_cast<u8*>(Sys::memoryMap(memory)) : nullptr;
        if (not _buffer)
            return false;
        Block::Request request{Block::Op::Attach, 0, 0};
        Block::Reply reply{};
        if (_channel.send(request, memory) != Sys::OK or _channel.recv(reply) != Sys::OK or reply.status != Sys::OK)
            return false;
        _sectors = reply.sectors;
        return true;
    }

    u64 sectors() const { return _sectors; }

    bool read(u64 lba, u8* out) { return transfer(Block::Op::Read, lba, out); }
    bool write(u64 lba, u8 const* in) { return transfer(Block::Op::Write, lba, const_cast<u8*>(in)); }

    // Reads `count` consecutive sectors (up to Block::MAX_SECTORS) in one request.
    bool readRun(u64 lba, u8* out, u32 count) {
        Block::Request request{Block::Op::Read, count, lba};
        Block::Reply reply{};
        if (_channel.send(request) != Sys::OK or _channel.recv(reply) != Sys::OK or reply.status != Sys::OK)
            return false;
        memcpy(out, _buffer, u64{count} * SECTOR);
        return true;
    }

    // Writes `count` consecutive sectors (up to Block::MAX_SECTORS) in one request.
    bool writeRun(u64 lba, u8 const* const* sectors, u32 count) {
        for (u32 i = 0; i < count; i++)
            memcpy(_buffer + i * SECTOR, sectors[i], SECTOR);
        Block::Request request{Block::Op::Write, count, lba};
        Block::Reply reply{};
        return _channel.send(request) == Sys::OK and _channel.recv(reply) == Sys::OK and reply.status == Sys::OK;
    }

    bool flush() {
        Block::Request request{Block::Op::Flush, 0, 0};
        Block::Reply reply{};
        return _channel.send(request) == Sys::OK and _channel.recv(reply) == Sys::OK and reply.status == Sys::OK;
    }

private:
    bool transfer(Block::Op op, u64 lba, u8* data) {
        if (op == Block::Op::Write)
            memcpy(_buffer, data, SECTOR);
        Block::Request request{op, 1, lba};
        Block::Reply reply{};
        if (_channel.send(request) != Sys::OK or _channel.recv(reply) != Sys::OK or reply.status != Sys::OK)
            return false;
        if (op == Block::Op::Read)
            memcpy(data, _buffer, SECTOR);
        return true;
    }

    Channel _channel;
    u8* _buffer = nullptr;
    u64 _sectors = 0;
};

Disk disk;

// Recently used sectors. Changed ones are written back by flush(), which
// every request that changes something ends with.
class Cache {
public:
    // The sector's bytes, valid until the next get(). With `load` false the
    // caller will overwrite all 512 bytes, so the disk isn't read.
    u8* get(u64 lba, bool load = true) {
        _clock++;
        Slot* victim = &_slots[0];
        for (auto& slot : _slots) {
            if (slot.valid and slot.lba == lba) {
                slot.used = _clock;
                return slot.data;
            }
            if (not slot.valid or (victim->valid and slot.used < victim->used))
                victim = &slot;
        }
        if (victim->valid and victim->dirty)
            writeBack(); // all of them: neighbours go out together
        victim->valid = false;
        victim->dirty = false;
        if (load) {
            if (not disk.read(lba, victim->data)) {
                _failed = true;
                memset(victim->data, 0, SECTOR);
                return victim->data;
            }
        } else {
            memset(victim->data, 0, SECTOR);
        }
        victim->valid = true;
        victim->lba = lba;
        victim->used = _clock;
        return victim->data;
    }

    // The sector returned by the last get(lba) was changed.
    void changed(u64 lba) {
        for (auto& slot : _slots) {
            if (slot.valid and slot.lba == lba)
                slot.dirty = true;
        }
    }

    // Writes every changed sector. Returns false if the disk failed since
    // the last flush.
    bool flush() {
        writeBack();
        if (not disk.flush())
            _failed = true;
        bool ok = not _failed;
        _failed = false;
        return ok;
    }

    bool failed() const { return _failed; }

private:
    struct Slot {
        u64 lba = 0;
        u64 used = 0;
        bool valid = false;
        bool dirty = false;
        u8 data[SECTOR];
    };

    // Writes every changed sector, consecutive ones in a single request.
    void writeBack() {
        Slot* dirty[SLOTS];
        u32 count = 0;
        for (auto& slot : _slots) {
            if (slot.valid and slot.dirty)
                dirty[count++] = &slot;
        }
        for (u32 i = 1; i < count; i++) { // sort by sector
            Slot* s = dirty[i];
            u32 j = i;
            for (; j > 0 and dirty[j - 1]->lba > s->lba; j--)
                dirty[j] = dirty[j - 1];
            dirty[j] = s;
        }
        for (u32 i = 0; i < count;) {
            u8 const* run[SLOTS];
            u32 n = 0;
            while (i + n < count and n < Block::MAX_SECTORS and dirty[i + n]->lba == dirty[i]->lba + n) {
                run[n] = dirty[i + n]->data;
                n++;
            }
            if (not disk.writeRun(dirty[i]->lba, run, n))
                _failed = true;
            for (u32 k = 0; k < n; k++)
                dirty[i + k]->dirty = false;
            i += n;
        }
    }

    static constexpr u32 SLOTS = 128;

    Slot _slots[SLOTS];
    u64 _clock = 0;
    bool _failed = false;
};

Cache cache;

// MARK: Time

// FAT stores local date and time; Bhairav OS treats it as UTC.
void fatTime(u16& date, u16& time) {
    Sys::SystemInfo info{};
    Sys::systemInfo(info);
    u64 t = info.unixTime;
    u64 days = t / 86400, rest = t % 86400;

    // Civil date from days since 1970 (Howard Hinnant's algorithm).
    long z = static_cast<long>(days) + 719468;
    long era = z / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long day = doy - (153 * mp + 2) / 5 + 1;
    long month = mp < 10 ? mp + 3 : mp - 9;
    long year = yoe + era * 400 + (month <= 2);
    if (year < 1980)
        year = 1980, month = 1, day = 1;

    date = static_cast<u16>((year - 1980) << 9 | month << 5 | day);
    time = static_cast<u16>((rest / 3600) << 11 | ((rest / 60) % 60) << 5 | (rest % 60) / 2);
}

u64 unixFromFat(u16 date, u16 time) {
    long y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 or m > 12 or d < 1)
        return 0;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + doe - 719468;
    return static_cast<u64>(days) * 86400 + u64(time >> 11) * 3600 + u64((time >> 5) & 63) * 60 + u64(time & 31) * 2;
}

// MARK: FAT32 volume

constexpr u32 END_OF_CHAIN = 0x0FFFFFFF;
constexpr u32 ATTR_HIDDEN = 0x02, ATTR_VOLUME = 0x08,
              ATTR_DIRECTORY = 0x10, ATTR_ARCHIVE = 0x20, ATTR_LONG_NAME = 0x0F;
constexpr u8 FREE_ENTRY = 0xE5;
constexpr u8 CASE_LOWER_BASE = 0x08, CASE_LOWER_EXT = 0x10;

bool endOfChain(u32 cluster) { return cluster < 2 or cluster >= 0x0FFFFFF8; }

// A directory entry's place on the disk.
struct Position {
    u32 cluster = 0;
    u32 index = 0; // entry number within the cluster
};

// A file or directory as found in its parent.
struct Node {
    char name[256];
    u8 entry[32];       // the short (8.3) entry
    Position at;        // where that short entry is
    Position first;     // the first of its entries (long name parts come first)
    u32 slots = 0;      // entries used, long name parts included
    u32 parent = 0;     // the directory holding it (cluster)
    bool isRoot = false;

    bool directory() const { return isRoot or (entry[11] & ATTR_DIRECTORY); }
    u32 cluster() const { return u32{get16(entry + 20)} << 16 | get16(entry + 26); }
    u32 size() const { return get32(entry + 28); }
};

// MARK: GPT

u64 get64(u8 const* p) { return u64{get32(p)} | u64{get32(p + 4)} << 32; }

u32 crc32(u8 const* p, u64 n, u32 crc = 0) { // (chainable: pass the previous result)
    crc = ~crc;
    for (u64 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// The Bhairav data partition's type, B4A1F0E2-6D3C-4B8A-9E7F-2C5D1A0B3E64,
// as GPT stores it (the first three fields little-endian).
constexpr u8 DATA_PARTITION[16] = {0xE2, 0xF0, 0xA1, 0xB4, 0x3C, 0x6D, 0x8A, 0x4B,
                                   0x9E, 0x7F, 0x2C, 0x5D, 0x1A, 0x0B, 0x3E, 0x64};

// Reads the GPT header at `lba` (1, or the backup at the last sector) and
// its entry array, both CRC-checked and bounds-checked, and finds the first
// Bhairav data partition. True with its first sector and length.
bool gptPartition(u64 lba, u64& first, u64& count) {
    u8 h[SECTOR];
    if (not disk.read(lba, h) or memcmp(h, "EFI PART", 8) != 0)
        return false;
    u32 size = get32(h + 12);
    if (size < 92 or size > SECTOR)
        return false;
    u32 stored = get32(h + 16);
    put32(h + 16, 0);
    if (crc32(h, size) != stored or get64(h + 24) != lba)
        return false;
    u64 firstUsable = get64(h + 40), lastUsable = get64(h + 48), entries = get64(h + 72);
    u32 n = get32(h + 80), entrySize = get32(h + 84), entriesCrc = get32(h + 88);
    if (entrySize < 128 or SECTOR % entrySize or n == 0 or n > 1024 or firstUsable > lastUsable or
        lastUsable >= disk.sectors())
        return false;
    u64 bytes = u64{n} * entrySize, sectors = (bytes + SECTOR - 1) / SECTOR;
    if (entries < 2 or entries + sectors > disk.sectors())
        return false;
    u32 crc = 0;
    bool found = false;
    for (u64 i = 0; i < sectors; i++) {
        u8 s[SECTOR];
        if (not disk.read(entries + i, s))
            return false;
        u64 here = bytes - i * SECTOR < SECTOR ? bytes - i * SECTOR : SECTOR;
        crc = crc32(s, here, crc);
        for (u64 at = 0; at + entrySize <= here and not found; at += entrySize) {
            if (memcmp(s + at, DATA_PARTITION, 16) != 0)
                continue;
            u64 start = get64(s + at + 32), last = get64(s + at + 40);
            if (start < firstUsable or last > lastUsable or last < start)
                return false;
            first = start;
            count = last - start + 1;
            found = true;
        }
    }
    return found and crc == entriesCrc;
}

class Volume {
public:
    bool mount() {
        u8 boot[SECTOR];
        if (not disk.read(0, boot))
            return false;
        u64 end = disk.sectors(); // where the volume must stop
        if (not isFat32(boot)) {
            if (get16(boot + 510) != 0xAA55)
                return false;
            if (boot[446 + 4] == 0xEE) {
                // A protective MBR: the disk has a GPT. Use its Bhairav data
                // partition (never the EFI System Partition), through the
                // primary header or, if that's damaged, the backup.
                u64 count = 0;
                if (not gptPartition(1, _base, count) and not gptPartition(disk.sectors() - 1, _base, count)) {
                    log("the disk's GPT is damaged or has no Bhairav data partition");
                    return false;
                }
                end = _base + count;
            } else {
                // Maybe partitioned: try the first MBR partition.
                _base = get32(boot + 446 + 8);
            }
            if (not _base or not disk.read(_base, boot) or not isFat32(boot))
                return false;
        }

        _sectorsPerCluster = boot[13];
        u32 reserved = get16(boot + 14);
        u32 fats = boot[16];
        u32 total = get16(boot + 19) ? get16(boot + 19) : get32(boot + 32);
        if (_base + total > end) {
            log("the FAT32 volume claims more sectors than its partition or disk has");
            return false;
        }
        _fatSize = get32(boot + 36);
        _root = get32(boot + 44);
        _fsInfo = get16(boot + 48);
        _fatStart = _base + reserved;
        _fats = fats;
        _dataStart = _base + reserved + u64{fats} * _fatSize;
        _clusters = (total - (reserved + fats * _fatSize)) / _sectorsPerCluster;
        _clusterBytes = _sectorsPerCluster * SECTOR;
        _entriesPerCluster = _clusterBytes / 32;

        if (_fsInfo and _fsInfo < reserved) {
            u8 const* info = cache.get(_base + _fsInfo);
            if (get32(info) == 0x41615252 and get32(info + 484) == 0x61417272) {
                u32 hint = get32(info + 492);
                if (hint >= 2 and hint < _clusters + 2)
                    _nextFree = hint;
            }
        }

        Text<> text;
        text.add("FAT32 volume, ").add(u64{_clusters} * _clusterBytes / (1024 * 1024)).add(" MiB, ")
            .add(_clusterBytes).add("-byte clusters");
        log(text.cstr());
        return true;
    }

    u32 clusterBytes() const { return _clusterBytes; }
    u32 sectorsPerCluster() const { return _sectorsPerCluster; }
    u32 entriesPerCluster() const { return _entriesPerCluster; }
    u32 root() const { return _root; }

    u64 sectorOf(u32 cluster) const { return _dataStart + u64{cluster - 2} * _sectorsPerCluster; }

    u32 next(u32 cluster) {
        if (endOfChain(cluster) or cluster >= _clusters + 2)
            return END_OF_CHAIN;
        u64 offset = u64{cluster} * 4;
        u8 const* sector = cache.get(_fatStart + offset / SECTOR);
        return get32(sector + offset % SECTOR) & 0x0FFFFFFF;
    }

    void setNext(u32 cluster, u32 value) {
        u64 offset = u64{cluster} * 4;
        for (u32 f = 0; f < _fats; f++) {
            u64 lba = _fatStart + u64{f} * _fatSize + offset / SECTOR;
            u8* sector = cache.get(lba);
            u8* p = sector + offset % SECTOR;
            put32(p, (get32(p) & 0xF0000000) | (value & 0x0FFFFFFF));
            cache.changed(lba);
        }
        invalidateFsInfo();
    }

    // A free cluster, marked as the end of a chain and linked after
    // `previous` (if any). Zeroed if `zero`. 0 if the disk is full.
    u32 allocate(u32 previous, bool zero) {
        for (u32 i = 0; i < _clusters; i++) {
            u32 c = 2 + (_nextFree - 2 + i) % _clusters;
            if (next(c) != 0)
                continue;
            setNext(c, END_OF_CHAIN);
            if (previous)
                setNext(previous, c);
            _nextFree = c + 1 < _clusters + 2 ? c + 1 : 2;
            if (zero) {
                for (u32 s = 0; s < _sectorsPerCluster; s++) {
                    // (a freed cluster's old contents may still be cached)
                    memset(cache.get(sectorOf(c) + s, false), 0, SECTOR);
                    cache.changed(sectorOf(c) + s);
                }
            }
            return c;
        }
        return 0;
    }

    void freeChain(u32 cluster) {
        while (not endOfChain(cluster) and cluster < _clusters + 2) {
            u32 following = next(cluster);
            setNext(cluster, 0);
            cluster = following;
        }
    }

    // The cluster `n` steps along the chain from `first`. With `grow`,
    // missing clusters are added (and `first` set if the chain was empty).
    // 0 if the chain is too short or the disk is full.
    u32 seek(u32& first, u64 n, bool grow) {
        if (endOfChain(first)) {
            if (not grow or not (first = allocate(0, false)))
                return 0;
        }
        u32 c = first;
        for (u64 i = 0; i < n; i++) {
            u32 following = next(c);
            if (endOfChain(following)) {
                if (not grow or not (following = allocate(c, false)))
                    return 0;
            }
            c = following;
        }
        return c;
    }

    // MARK: Directory entries

    // The 32 bytes of the entry at `p` (valid until the next cache use),
    // and the sector holding it.
    u8* entryAt(Position p, u64& lba) {
        u64 offset = u64{p.index} * 32;
        lba = sectorOf(p.cluster) + offset / SECTOR;
        return cache.get(lba) + offset % SECTOR;
    }

    // The entry after `p`; false at the end of the directory's chain.
    bool advance(Position& p, bool grow = false) {
        if (++p.index < _entriesPerCluster)
            return true;
        u32 following = next(p.cluster);
        if (endOfChain(following)) {
            if (not grow or not (following = allocate(p.cluster, true)))
                return false;
        }
        p = {following, 0};
        return true;
    }

    // Calls visit(Node&) for each entry of a directory (with long names put
    // together) until it returns true. Returns whether one did.
    template <typename F>
    bool walk(u32 directory, F visit) {
        Node node;
        u8 checksum = 0;
        int longParts = 0, expected = 0;
        Position longStart;
        u16 longName[260];

        Position p{directory, 0};
        do {
            u64 lba;
            u8 raw[32];
            memcpy(raw, entryAt(p, lba), 32);
            if (raw[0] == 0)
                return false; // nothing after this
            if (raw[0] == FREE_ENTRY) {
                expected = 0;
                continue;
            }
            if (raw[11] == ATTR_LONG_NAME) {
                int order = raw[0] & 0x1F;
                if (raw[0] & 0x40) {
                    expected = longParts = order;
                    checksum = raw[13];
                    longStart = p;
                    for (auto& ch : longName)
                        ch = 0;
                } else if (order != expected or raw[13] != checksum) {
                    expected = 0;
                    continue;
                }
                if (order < 1 or order > 20) {
                    expected = 0;
                    continue;
                }
                u16* part = longName + (order - 1) * 13;
                static constexpr int OFFSETS[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int i = 0; i < 13; i++)
                    part[i] = get16(raw + OFFSETS[i]);
                expected = order - 1;
                continue;
            }

            bool hasLong = longParts and expected == 0 and checksumOf(raw) == checksum;
            if (raw[11] & ATTR_VOLUME) {
                longParts = 0;
                continue;
            }

            memcpy(node.entry, raw, 32);
            node.at = p;
            node.parent = directory;
            node.isRoot = false;
            if (hasLong) {
                int n = 0;
                for (int i = 0; i < longParts * 13 and longName[i] and longName[i] != 0xFFFF and n < 255; i++)
                    node.name[n++] = longName[i] < 128 ? static_cast<char>(longName[i]) : '?';
                node.name[n] = '\0';
                node.first = longStart;
                node.slots = static_cast<u32>(longParts) + 1;
            } else {
                shortName(raw, node.name);
                node.first = p;
                node.slots = 1;
            }
            longParts = 0;
            expected = 0;
            if (visit(node))
                return true;
        } while (advance(p));
        return false;
    }

    // Finds a path. `parentOut` gets the cluster of the directory that
    // holds (or would hold) the last component, and `leaf` that name.
    long resolve(char const* path, Node& node, u32* parentOut = nullptr, char const** leaf = nullptr) {
        if (path[0] != '/')
            return Sys::INVALID_ARGUMENT;

        node = {};
        node.isRoot = true;
        u32 directory = _root;
        char const* p = path;
        for (;;) {
            while (*p == '/')
                p++;
            if (not *p)
                return Sys::OK;
            char part[256];
            unsigned long n = 0;
            char const* start = p;
            while (*p and *p != '/') {
                if (n >= 255)
                    return Sys::INVALID_ARGUMENT;
                part[n++] = *p++;
            }
            part[n] = '\0';
            if (not node.directory())
                return Sys::NOT_DIRECTORY;

            if (parentOut)
                *parentOut = directory;
            if (leaf)
                *leaf = start;
            bool found = walk(directory, [&](Node& candidate) {
                if (not sameName(candidate.name, part))
                    return false;
                node = candidate;
                return true;
            });
            if (not found)
                return Sys::NOT_FOUND; // (locateNew checks whether only the last part is missing)
            if (node.directory()) {
                directory = node.cluster() ? node.cluster() : _root; // ".." of a top-level directory is 0
                if (sameName(node.name, ".") or sameName(node.name, ".."))
                    node.isRoot = directory == _root;
            }
        }
    }

    // Like resolve, but for creating: finds the parent directory and checks
    // the name. Returns OK if the path doesn't exist yet, ALREADY_EXISTS
    // with `existing` filled in if it does.
    long locateNew(char const* path, u32& parent, char* name, Node& existing) {
        char const* leaf = nullptr;
        long result = resolve(path, existing, &parent, &leaf);
        if (result == Sys::OK)
            return existing.isRoot ? Sys::INVALID_ARGUMENT : Sys::ALREADY_EXISTS;
        if (result != Sys::NOT_FOUND or not leaf)
            return result;

        unsigned long n = 0;
        while (leaf[n] and leaf[n] != '/') {
            name[n] = leaf[n];
            n++;
        }
        name[n] = '\0';
        for (unsigned long i = n; leaf[i]; i++) {
            if (leaf[i] != '/')
                return Sys::NOT_FOUND; // a directory in the middle is missing
        }
        return validName(name) ? Sys::OK : Sys::INVALID_ARGUMENT;
    }

    // Adds an entry (with long name parts when needed) to a directory.
    long create(u32 directory, char const* name, u8 attributes, u32 firstCluster, Node& node) {
        u8 shortEntry[11];
        u8 caseFlags = 0;
        bool needsLong = not fitsShort(name, shortEntry, caseFlags);
        if (needsLong and not uniqueAlias(directory, name, shortEntry))
            return Sys::NO_SPACE;

        unsigned long len = length(name);
        u32 longParts = needsLong ? static_cast<u32>((len + 12) / 13) : 0;
        u32 slots = longParts + 1;

        Position start;
        if (not findFreeRun(directory, slots, start))
            return Sys::NO_SPACE;

        u8 checksum = checksumOf(shortEntry);
        Position p = start;
        for (u32 part = longParts; part >= 1; part--) {
            u64 lba;
            u8* e = entryAt(p, lba);
            memset(e, 0, 32);
            e[0] = static_cast<u8>(part | (part == longParts ? 0x40 : 0));
            e[11] = ATTR_LONG_NAME;
            e[13] = checksum;
            static constexpr int OFFSETS[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int i = 0; i < 13; i++) {
                unsigned long at = (part - 1) * 13 + static_cast<unsigned long>(i);
                u16 ch = at < len ? static_cast<u8>(name[at]) : at == len ? 0 : 0xFFFF;
                put16(e + OFFSETS[i], ch);
            }
            cache.changed(lba);
            advance(p);
        }

        u64 lba;
        u8* e = entryAt(p, lba);
        memset(e, 0, 32);
        memcpy(e, shortEntry, 11);
        e[11] = attributes;
        e[12] = caseFlags;
        u16 date, time;
        fatTime(date, time);
        put16(e + 14, time);
        put16(e + 16, date);
        put16(e + 18, date);
        put16(e + 22, time);
        put16(e + 24, date);
        put16(e + 20, static_cast<u16>(firstCluster >> 16));
        put16(e + 26, static_cast<u16>(firstCluster));
        memcpy(node.entry, e, 32);
        cache.changed(lba);

        unsigned long n = 0;
        for (; name[n] and n < 255; n++)
            node.name[n] = name[n];
        node.name[n] = '\0';
        node.at = p;
        node.first = start;
        node.slots = slots;
        node.parent = directory;
        node.isRoot = false;
        return Sys::OK;
    }

    // Writes a node's short entry back (after changing its cluster, size or time).
    void update(Node& node, bool touch) {
        if (touch) {
            u16 date, time;
            fatTime(date, time);
            put16(node.entry + 22, time);
            put16(node.entry + 24, date);
            put16(node.entry + 18, date);
            node.entry[11] |= node.directory() ? 0 : ATTR_ARCHIVE;
        }
        u64 lba;
        u8* e = entryAt(node.at, lba);
        memcpy(e, node.entry, 32);
        cache.changed(lba);
    }

    void erase(Node const& node) {
        Position p = node.first;
        for (u32 i = 0; i < node.slots; i++) {
            u64 lba;
            u8* e = entryAt(p, lba);
            e[0] = FREE_ENTRY;
            cache.changed(lba);
            if (i + 1 < node.slots and not advance(p))
                break;
        }
    }

    bool empty(u32 directory) {
        return not walk(directory, [](Node& n) { return not sameName(n.name, ".") and not sameName(n.name, ".."); });
    }

private:
    static bool isFat32(u8 const* boot) {
        return get16(boot + 510) == 0xAA55 and get16(boot + 11) == SECTOR and boot[13] and
               get16(boot + 22) == 0 and get32(boot + 36) != 0 and memcmp(boot + 82, "FAT32   ", 8) == 0;
    }

    // The FS information sector's free-cluster count would go stale: mark
    // it unknown, as the specification allows.
    void invalidateFsInfo() {
        if (_fsInfoInvalidated or not _fsInfo)
            return;
        _fsInfoInvalidated = true;
        u64 lba = _base + _fsInfo;
        u8* info = cache.get(lba);
        if (get32(info) != 0x41615252)
            return;
        put32(info + 488, 0xFFFFFFFF);
        put32(info + 492, 0xFFFFFFFF);
        cache.changed(lba);
    }

    static u8 checksumOf(u8 const* shortEntry) {
        u8 sum = 0;
        for (int i = 0; i < 11; i++)
            sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + shortEntry[i]);
        return sum;
    }

    static void shortName(u8 const* e, char* out) {
        int n = 0;
        for (int i = 0; i < 8 and e[i] != ' '; i++) {
            char c = static_cast<char>(i == 0 and e[i] == 0x05 ? 0xE5 : e[i]);
            out[n++] = (e[12] & CASE_LOWER_BASE) ? lower(c) : c;
        }
        if (e[8] != ' ') {
            out[n++] = '.';
            for (int i = 8; i < 11 and e[i] != ' '; i++)
                out[n++] = (e[12] & CASE_LOWER_EXT) ? lower(static_cast<char>(e[i])) : static_cast<char>(e[i]);
        }
        out[n] = '\0';
    }

    static bool validName(char const* name) {
        unsigned long n = length(name);
        if (n == 0 or n > 255 or sameName(name, ".") or sameName(name, ".."))
            return false;
        for (unsigned long i = 0; i < n; i++) {
            char c = name[i];
            if (c < ' ' or c == '"' or c == '*' or c == ':' or c == '<' or c == '>' or c == '?' or
                c == '\\' or c == '|' or static_cast<unsigned char>(c) > 126)
                return false;
        }
        return name[n - 1] != ' ' and name[n - 1] != '.';
    }

    static bool shortChar(char c) {
        if ((c >= 'A' and c <= 'Z') or (c >= 'a' and c <= 'z') or (c >= '0' and c <= '9'))
            return true;
        for (char const* s = "!#$%&'()-@^_`{}~"; *s; s++) {
            if (c == *s)
                return true;
        }
        return false;
    }

    // Whether `name` can be stored as just an 8.3 entry (with the
    // lower-case flags Windows uses), and that entry's name.
    static bool fitsShort(char const* name, u8* out, u8& caseFlags) {
        memset(out, ' ', 11);
        caseFlags = 0;
        char const* dot = nullptr;
        for (char const* p = name; *p; p++) {
            if (*p == '.') {
                if (dot)
                    return false;
                dot = p;
            }
        }
        unsigned long baseLen = dot ? static_cast<unsigned long>(dot - name) : length(name);
        unsigned long extLen = dot ? length(dot + 1) : 0;
        if (baseLen == 0 or baseLen > 8 or extLen > 3 or (dot and extLen == 0))
            return false;

        auto part = [&](char const* s, unsigned long n, u8* dest, u8 lowerFlag) {
            bool hasUpper = false, hasLower = false;
            for (unsigned long i = 0; i < n; i++) {
                if (not shortChar(s[i]))
                    return false;
                hasUpper |= s[i] >= 'A' and s[i] <= 'Z';
                hasLower |= s[i] >= 'a' and s[i] <= 'z';
                dest[i] = static_cast<u8>(upper(s[i]));
            }
            if (hasUpper and hasLower)
                return false; // mixed case needs a long name
            if (hasLower)
                caseFlags |= lowerFlag;
            return true;
        };
        if (not part(name, baseLen, out, CASE_LOWER_BASE))
            return false;
        if (dot and not part(dot + 1, extLen, out + 8, CASE_LOWER_EXT))
            return false;
        return out[0] != 0xE5;
    }

    // A short alias like "MYLONG~1.TXT" for a long name, not used yet in
    // the directory.
    bool uniqueAlias(u32 directory, char const* name, u8* out) {
        memset(out, ' ', 11);
        char const* dot = nullptr;
        for (char const* p = name; *p; p++) {
            if (*p == '.')
                dot = p;
        }
        if (dot == name)
            dot = nullptr; // ".profile" has no extension
        int b = 0;
        for (char const* p = name; *p and p != dot and b < 6; p++) {
            if (shortChar(*p))
                out[b++] = static_cast<u8>(upper(*p));
        }
        if (b == 0)
            out[b++] = '_';
        if (dot) {
            int e = 0;
            for (char const* p = dot + 1; *p and e < 3; p++) {
                if (shortChar(*p))
                    out[8 + e++] = static_cast<u8>(upper(*p));
            }
        }

        for (u32 k = 1; k < 1000000; k++) {
            char digits[8];
            int d = 0;
            for (u32 v = k; v; v /= 10)
                digits[d++] = static_cast<char>('0' + v % 10);
            int keep = b < 7 - d ? b : 7 - d;
            for (int i = keep; i < 8; i++)
                out[i] = ' ';
            out[keep] = '~';
            for (int i = 0; i < d; i++)
                out[keep + 1 + i] = static_cast<u8>(digits[d - 1 - i]);

            bool taken = walk(directory, [&](Node& n) { return memcmp(n.entry, out, 11) == 0; });
            if (not taken)
                return true;
        }
        return false;
    }

    // `count` free entries in a row, growing the directory if needed.
    bool findFreeRun(u32 directory, u32 count, Position& start) {
        Position p{directory, 0};
        u32 run = 0;
        for (;;) {
            u64 lba;
            u8 first = entryAt(p, lba)[0];
            if (first == 0 or first == FREE_ENTRY) {
                if (run++ == 0)
                    start = p;
                if (run == count)
                    return true;
            } else {
                run = 0;
            }
            if (not advance(p, true))
                return false;
        }
    }

    u64 _base = 0;
    u32 _sectorsPerCluster = 1;
    u32 _fatSize = 0;
    u32 _fats = 2;
    u32 _root = 2;
    u32 _fsInfo = 0;
    u64 _fatStart = 0;
    u64 _dataStart = 0;
    u32 _clusters = 0;
    u32 _clusterBytes = SECTOR;
    u32 _entriesPerCluster = 16;
    u32 _nextFree = 2;
    bool _fsInfoInvalidated = false;
};

Volume volume;

// MARK: Operations

// Whole sectors are read straight from the disk, as many consecutive ones
// per request as the cluster chain allows. (The cache holds no unwritten
// changes between requests, so the disk is up to date.)
long readFile(Node const& node, u64 offset, u8* out, u64 length, u64& done) {
    done = 0;
    u64 size = node.size();
    if (offset >= size)
        return Sys::OK;
    if (length > size - offset)
        length = size - offset;

    u32 first = node.cluster();
    u32 bytes = volume.clusterBytes();
    u32 perCluster = volume.sectorsPerCluster();
    u64 clusterIndex = offset / bytes; // which cluster of the file `cluster` is
    u32 cluster = volume.seek(first, clusterIndex, false);
    u64 position = offset;
    while (done < length) {
        while (cluster and clusterIndex < position / bytes) {
            cluster = volume.next(cluster);
            clusterIndex++;
            if (endOfChain(cluster))
                cluster = 0;
        }
        if (not cluster)
            return Sys::IO_ERROR; // the chain is shorter than the size says

        u64 within = position % bytes;
        u64 lba = volume.sectorOf(cluster) + within / SECTOR;
        u64 at = within % SECTOR;

        if (at == 0 and length - done >= SECTOR) {
            u64 want = (length - done) / SECTOR;
            if (want > Block::MAX_SECTORS)
                want = Block::MAX_SECTORS;
            u64 n = perCluster - within / SECTOR;
            u32 last = cluster;
            u64 lastIndex = clusterIndex;
            while (n < want) {
                u32 following = volume.next(last);
                if (following != last + 1)
                    break;
                last = following;
                lastIndex++;
                n += perCluster;
            }
            if (n > want)
                n = want;
            if (not disk.readRun(lba, out + done, static_cast<u32>(n)))
                return Sys::IO_ERROR;
            done += n * SECTOR;
            position += n * SECTOR;
            u64 target = position / bytes;
            if (target <= lastIndex) { // still inside the run: its clusters are consecutive
                cluster += static_cast<u32>(target - clusterIndex);
                clusterIndex = target;
            } else {
                cluster = last; // the loop above steps on from here
                clusterIndex = lastIndex;
            }
            continue;
        }

        u64 n = SECTOR - at < length - done ? SECTOR - at : length - done;
        memcpy(out + done, cache.get(lba) + at, n);
        done += n;
        position += n;
    }
    return cache.failed() ? Sys::IO_ERROR : Sys::OK;
}

long writeFile(Node& node, u64 offset, u8 const* in, u64 length, u64& done) {
    done = 0;
    u64 size = node.size();
    if (offset > size)
        return Sys::INVALID_ARGUMENT; // no holes
    if (length == 0)
        return Sys::OK;
    if (offset + length > 0xFFFFFFFFul)
        return Sys::NO_SPACE; // FAT32's 4 GiB limit

    u32 first = node.cluster();
    u32 bytes = volume.clusterBytes();
    u32 cluster = volume.seek(first, offset / bytes, true);
    long result = Sys::OK;
    u64 position = offset;
    while (done < length) {
        if (not cluster) {
            result = Sys::NO_SPACE;
            break;
        }
        u64 within = position % bytes;
        u64 lba = volume.sectorOf(cluster) + within / SECTOR;
        u64 at = within % SECTOR;
        u64 n = SECTOR - at < length - done ? SECTOR - at : length - done;
        // A sector written in full, or past the old end, needn't be read first.
        bool whole = at == 0 and (n == SECTOR or position >= size);
        u8* sector = cache.get(lba, not whole);
        memcpy(sector + at, in + done, n);
        cache.changed(lba);
        done += n;
        position += n;
        if (position % bytes == 0 and done < length) {
            u32 following = volume.next(cluster);
            cluster = endOfChain(following) ? volume.allocate(cluster, false) : following;
        }
    }

    put16(node.entry + 20, static_cast<u16>(first >> 16));
    put16(node.entry + 26, static_cast<u16>(first));
    if (offset + done > size)
        put32(node.entry + 28, static_cast<u32>(offset + done));
    volume.update(node, true);
    return result;
}

long truncate(Node& node) {
    volume.freeChain(node.cluster());
    put16(node.entry + 20, 0);
    put16(node.entry + 26, 0);
    put32(node.entry + 28, 0);
    volume.update(node, true);
    return Sys::OK;
}

void copyName(char* dest, char const* src) {
    unsigned long n = 0;
    for (; src[n] and n + 1 < Fs::MAX_NAME; n++)
        dest[n] = src[n];
    dest[n] = '\0';
}

long makeDirectory(char const* path) {
    Node node;
    u32 parent;
    char name[256];
    long status = volume.locateNew(path, parent, name, node);
    if (status != Sys::OK)
        return status;
    u32 cluster = volume.allocate(0, true);
    if (not cluster)
        return Sys::NO_SPACE;
    status = volume.create(parent, name, ATTR_DIRECTORY, cluster, node);
    if (status != Sys::OK) {
        volume.freeChain(cluster);
    } else {
        // "." and ".." (0 when the parent is the root directory).
        u64 lba = volume.sectorOf(cluster);
        u8* sector = cache.get(lba);
        for (int i = 0; i < 2; i++) {
            u8* e = sector + i * 32;
            memcpy(e, node.entry, 32);
            memset(e, ' ', 11);
            e[0] = '.';
            if (i == 1)
                e[1] = '.';
            e[11] = ATTR_DIRECTORY;
            e[12] = 0;
            u32 target = i == 0 ? cluster : (parent == volume.root() ? 0 : parent);
            put16(e + 20, static_cast<u16>(target >> 16));
            put16(e + 26, static_cast<u16>(target));
            put32(e + 28, 0);
        }
        cache.changed(lba);
    }
    if (not cache.flush() and status == Sys::OK)
        status = Sys::IO_ERROR;
    return status;
}

// MARK: Access policy

// The protected system directories (top level of the disk).
constexpr char const* APPS_DIR = "apps";       // installed programs
constexpr char const* SYSTEM_DIR = "var";      // system state: apt, permissions
constexpr char const* PRIVATE_DIR = "appdata"; // each app's private storage

struct Client {
    Channel channel;
    u8* buffer = nullptr;
    bool used = false;
    Fs::Scope scope = Fs::Scope::Private;
    char root[16 + Fs::MAX_APP_NAME] = {}; // Private: "/appdata/<app>"
    char folder[Fs::MAX_FOLDER] = {};      // Private: the user folder it sees as /shared, or ""
};

// Rewrites `path` as a plain absolute path: no empty, "." or ".." parts.
// False if ".." would climb above "/" or the result is too long. After
// this, the path names exactly the directories the resolver will walk (it
// would otherwise follow the ".." entries on the disk).
bool normalize(char const* path, char* out) {
    if (path[0] != '/')
        return false;
    unsigned long n = 0;
    char const* p = path;
    for (;;) {
        while (*p == '/')
            p++;
        if (not *p)
            break;
        char const* start = p;
        while (*p and *p != '/')
            p++;
        unsigned long len = static_cast<unsigned long>(p - start);
        if (len == 1 and start[0] == '.')
            continue;
        if (len == 2 and start[0] == '.' and start[1] == '.') {
            if (n == 0)
                return false;
            while (n > 0 and out[n - 1] != '/')
                n--;
            n--; // the '/' too
            continue;
        }
        if (n + 1 + len >= Fs::MAX_PATH)
            return false;
        out[n++] = '/';
        memcpy(out + n, start, len);
        n += len;
    }
    if (n == 0)
        out[n++] = '/';
    out[n] = '\0';
    return true;
}

// True if the (normalized) path's first part is `dir`.
bool under(char const* path, char const* dir) {
    char top[256];
    unsigned long n = 0;
    for (char const* p = path + 1; *p and *p != '/' and n < 255; p++)
        top[n++] = *p;
    top[n] = '\0';
    return sameName(top, dir);
}

// Applies the client's scope to a request's path, rewriting it to the path
// on the disk. Returns OK, or why it's refused.
long applyScope(Client const& client, Fs::Request& request) {
    bool writes = request.op == Fs::Op::Write or request.op == Fs::Op::Remove or request.op == Fs::Op::MakeDir;
    char clean[Fs::MAX_PATH];
    if (not normalize(request.path, clean))
        return Sys::INVALID_ARGUMENT;

    switch (client.scope) {
    case Fs::Scope::System:
        break;
    case Fs::Scope::User:
        if (under(clean, PRIVATE_DIR))
            return Sys::NOT_FOUND; // other apps' storage isn't there for it
        if (writes and (under(clean, APPS_DIR) or under(clean, SYSTEM_DIR)))
            return Sys::ACCESS_DENIED;
        break;
    case Fs::Scope::Private: {
        if (writes and clean[1] == '\0')
            return Sys::INVALID_ARGUMENT; // its own root stays
        // "/shared/..." is the user folder it was given, if any.
        char const* base = client.root;
        char const* rest = clean;
        if (client.folder[0] and under(clean, "shared")) {
            if (writes and clean[7] == '\0')
                return Sys::INVALID_ARGUMENT; // the folder itself stays
            base = client.folder;
            rest = clean + 7; // what follows "/shared" ("" or "/...")
        }
        unsigned long baseLength = 0;
        while (base[baseLength])
            baseLength++;
        unsigned long restLength = 0;
        while (rest[restLength])
            restLength++;
        if (rest[0] == '/' and rest[1] == '\0')
            restLength = 0; // "/" is the base itself
        if (baseLength + restLength >= Fs::MAX_PATH)
            return Sys::INVALID_ARGUMENT;
        memcpy(request.path, base, baseLength);
        memcpy(request.path + baseLength, rest, restLength);
        request.path[baseLength + restLength] = '\0';
        return Sys::OK;
    }
    }
    memcpy(request.path, clean, Fs::MAX_PATH);
    return Sys::OK;
}

Fs::Reply handle(Client& client, Fs::Request& request, Sys::Handle attached) {
    Fs::Reply reply{};
    request.path[Fs::MAX_PATH - 1] = '\0';

    if (request.op == Fs::Op::Attach) {
        u8* buffer = attached ? static_cast<u8*>(Sys::memoryMap(attached)) : nullptr;
        if (attached)
            Sys::close(attached); // the mapping keeps the memory
        if (not buffer) {
            reply.status = Sys::INVALID_ARGUMENT;
            return reply;
        }
        if (client.buffer)
            Sys::memoryUnmap(client.buffer);
        client.buffer = buffer;
        reply.status = Sys::OK;
        return reply;
    }
    if (attached)
        Sys::close(attached);
    if (not client.buffer or request.length > Fs::BUFFER_SIZE) {
        reply.status = Sys::INVALID_ARGUMENT;
        return reply;
    }
    if (long refused = applyScope(client, request)) {
        reply.status = refused;
        return reply;
    }
    // Listing the disk's top level for a User client: other apps' storage isn't shown.
    bool hidePrivate = client.scope == Fs::Scope::User and request.path[1] == '\0';

    Node node;
    switch (request.op) {
    case Fs::Op::Read: {
        reply.status = volume.resolve(request.path, node);
        if (reply.status == Sys::OK and node.directory())
            reply.status = Sys::IS_DIRECTORY;
        if (reply.status == Sys::OK)
            reply.status = readFile(node, request.offset, client.buffer, request.length, reply.length);
        reply.size = node.size();
        break;
    }

    case Fs::Op::Write: {
        u32 parent;
        char name[256];
        long found = volume.locateNew(request.path, parent, name, node);
        if (found == Sys::OK)
            found = volume.create(parent, name, ATTR_ARCHIVE, 0, node);
        else if (found == Sys::ALREADY_EXISTS)
            found = node.directory() ? Sys::IS_DIRECTORY : Sys::OK;
        if (found == Sys::OK and (request.flags & Fs::TRUNCATE))
            found = truncate(node);
        if (found == Sys::OK)
            found = writeFile(node, request.offset, client.buffer, request.length, reply.length);
        reply.status = found;
        reply.size = node.size();
        if (not cache.flush() and reply.status == Sys::OK)
            reply.status = Sys::IO_ERROR;
        break;
    }

    case Fs::Op::List: {
        reply.status = volume.resolve(request.path, node);
        if (reply.status == Sys::OK and not node.directory())
            reply.status = Sys::NOT_DIRECTORY;
        if (reply.status != Sys::OK)
            break;
        u32 directory = node.isRoot ? volume.root() : node.cluster();
        if (not directory)
            directory = volume.root();
        u64 capacity = request.length < Fs::BUFFER_SIZE / sizeof(Fs::Entry) ? request.length
                                                                             : Fs::BUFFER_SIZE / sizeof(Fs::Entry);
        auto* out = reinterpret_cast<Fs::Entry*>(client.buffer);
        u64 index = 0;
        volume.walk(directory, [&](Node& entry) {
            if (sameName(entry.name, ".") or sameName(entry.name, "..") or (entry.entry[11] & ATTR_HIDDEN))
                return false;
            if (hidePrivate and sameName(entry.name, PRIVATE_DIR))
                return false;
            if (index >= request.offset and reply.length < capacity) {
                Fs::Entry& e = out[reply.length++];
                copyName(e.name, entry.name);
                e.size = entry.directory() ? 0 : entry.size();
                e.directory = entry.directory();
                e.modified = unixFromFat(get16(entry.entry + 24), get16(entry.entry + 22));
            }
            index++;
            return false;
        });
        reply.size = index;
        break;
    }

    case Fs::Op::Stat:
        reply.status = volume.resolve(request.path, node);
        if (reply.status == Sys::OK) {
            reply.directory = node.directory();
            reply.size = node.directory() ? 0 : node.size();
        }
        break;

    case Fs::Op::Remove:
        reply.status = volume.resolve(request.path, node);
        if (reply.status == Sys::OK and node.isRoot)
            reply.status = Sys::INVALID_ARGUMENT;
        if (reply.status == Sys::OK and node.directory() and node.cluster() and not volume.empty(node.cluster()))
            reply.status = Sys::NOT_EMPTY;
        if (reply.status == Sys::OK) {
            volume.freeChain(node.cluster());
            volume.erase(node);
            if (not cache.flush())
                reply.status = Sys::IO_ERROR;
        }
        break;

    case Fs::Op::MakeDir:
        reply.status = makeDirectory(request.path);
        break;

    default: reply.status = Sys::INVALID_ARGUMENT; break;
    }
    return reply;
}

Client clients[MAX_CLIENTS];
bool ready = false; // the disk is mounted

// A user folder given to a Private client: normalized into `out`, never
// "/" (that's every file: the User scope) or a protected directory.
bool validFolder(Fs::Scope scope, char const* folder, char* out) {
    char clean[Fs::MAX_PATH];
    if (scope != Fs::Scope::Private or not normalize(folder, clean) or clean[1] == '\0')
        return false;
    if (under(clean, APPS_DIR) or under(clean, SYSTEM_DIR) or under(clean, PRIVATE_DIR))
        return false;
    unsigned long n = 0;
    while (clean[n])
        n++;
    if (n >= Fs::MAX_FOLDER)
        return false;
    memcpy(out, clean, n + 1);
    return true;
}

// Makes a directory and any missing parents (the path is normalized).
void makePath(char const* path) {
    char partial[Fs::MAX_PATH];
    unsigned long n = 0;
    for (unsigned long i = 0; path[i]; i++) {
        if (path[i] == '/' and i > 0) {
            partial[n] = '\0';
            makeDirectory(partial);
        }
        partial[n++] = path[i];
    }
    partial[n] = '\0';
    makeDirectory(partial);
}

// An app's name as its folder: lowercase letters, digits and '-'.
bool validAppName(char const* name) {
    if (not name[0])
        return false;
    for (char const* p = name; *p; p++) {
        if (not((*p >= 'a' and *p <= 'z') or (*p >= '0' and *p <= '9') or *p == '-'))
            return false;
    }
    return true;
}

// Takes a new client from the connect channel. Returns false once nobody
// holds the other end anymore.
bool acceptClient(Channel& connect) {
    Fs::Connect message{};
    Sys::Handle attached = Sys::NO_HANDLE;
    long result = connect.recv(message, &attached, Sys::RECV_NONBLOCK);
    if (result == Sys::PEER_CLOSED)
        return false;
    if (result == Sys::BUFFER_TOO_SMALL) {
        // It would stay queued forever: stop listening rather than spin.
        log("bad message on the connect channel, no longer accepting clients");
        return false;
    }
    message.app[Fs::MAX_APP_NAME - 1] = '\0';
    bool validScope = message.scope == Fs::Scope::System or message.scope == Fs::Scope::User or
                      message.scope == Fs::Scope::Private;
    message.folder[Fs::MAX_FOLDER - 1] = '\0';
    char folder[Fs::MAX_FOLDER] = {};
    if (message.folder[0] and not validFolder(message.scope, message.folder, folder))
        result = Sys::INVALID_ARGUMENT; // refuse the connection
    if (result != Sys::OK or message.magic != Fs::CONNECT_MAGIC or not attached or not validScope or
        not validAppName(message.app)) {
        if (attached)
            Sys::close(attached);
        return true;
    }
    for (auto& client : clients) {
        if (not client.used) {
            client = {};
            client.channel = Channel(attached);
            client.used = true;
            client.scope = message.scope;
            if (message.scope == Fs::Scope::Private and ready) {
                // Its private folder, made on first use.
                Text<sizeof(client.root)> root;
                root.add("/").add(PRIVATE_DIR).add("/").add(message.app);
                memcpy(client.root, root.cstr(), sizeof(client.root));
                makeDirectory("/appdata");
                makeDirectory(client.root);
                if (folder[0]) {
                    memcpy(client.folder, folder, sizeof(folder));
                    makePath(folder);
                }
            }
            return true;
        }
    }
    log("too many clients");
    Sys::close(attached);
    return true;
}

void serve(Client& client) {
    Fs::Request request;
    Sys::Handle attached = Sys::NO_HANDLE;
    long result = client.channel.recv(request, &attached, Sys::RECV_NONBLOCK);
    if (result == Sys::WOULD_BLOCK)
        return;
    if (result < 0 and result != Sys::INVALID_ARGUMENT) { // the client is gone
        if (client.buffer)
            Sys::memoryUnmap(client.buffer);
        client = {};
        return;
    }
    Fs::Reply reply{};
    if (result == Sys::OK)
        reply = handle(client, request, attached);
    else
        reply.status = Sys::INVALID_ARGUMENT;
    client.channel.send(reply);
}

} // namespace

int main() {
    Channel connect(CONNECT);
    ready = disk.attach() and volume.mount();
    if (not ready)
        log("no usable FAT32 disk: files are unavailable");

    bool listening = true;
    for (;;) {
        Sys::Handle handles[1 + MAX_CLIENTS];
        int owners[1 + MAX_CLIENTS];
        unsigned long count = 0;
        if (listening) {
            handles[count] = connect.handle();
            owners[count++] = -1;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].used) {
                handles[count] = clients[i].channel.handle();
                owners[count++] = i;
            }
        }

        if (count == 0)
            return 0; // nobody can reach this server anymore
        long index = Sys::wait(handles, count);
        if (index < 0)
            continue;
        if (owners[index] < 0) {
            listening = acceptClient(connect);
            continue;
        }

        Client& client = clients[owners[index]];
        if (ready) {
            serve(client);
        } else {
            // Answer every request with NOT_FOUND, so clients don't hang.
            Fs::Request request;
            Sys::Handle attached = Sys::NO_HANDLE;
            long result = client.channel.recv(request, &attached, Sys::RECV_NONBLOCK);
            if (attached)
                Sys::close(attached);
            if (result == Sys::WOULD_BLOCK)
                continue;
            if (result < 0 and result != Sys::INVALID_ARGUMENT) {
                client = {};
                continue;
            }
            Fs::Reply reply{};
            reply.status = Sys::NOT_FOUND;
            client.channel.send(reply);
        }
    }
}
