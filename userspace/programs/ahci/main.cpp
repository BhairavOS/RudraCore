// ahci: the SATA disk driver.
//
// Handles from init:
//   1: the AHCI controller (PCI device)
//   2: a channel to its client, the file system server (protocol/block.h)
//
// Uses the first SATA disk found, through command slot 0, and waits for
// each command by polling (spinning briefly, then yielding). The disk reads
// and writes the client's shared buffer directly (DMA).

#include "config.h"

#include <kapala/channel.h>
#include <kapala/protocol/block.h>
#include <kapala/sys.h>
#include <kapala/text.h>

using namespace Kapala;
namespace Block = Kapala::Protocol::Block;

namespace {

constexpr Sys::Handle DEVICE = 1;
constexpr Sys::Handle CLIENT = 2;

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
using u64 = unsigned long;

// MARK: Registers (AHCI 1.3.1)

// Controller
constexpr u32 CAP = 0x00, GHC = 0x04, PI = 0x0C, CAP2 = 0x24, BOHC = 0x28;
constexpr u32 GHC_AE = 1u << 31;
constexpr u32 CAP_S64A = 1u << 31;

// Per port, at 0x100 + port * 0x80
constexpr u32 PxCLB = 0x00, PxCLBU = 0x04, PxFB = 0x08, PxFBU = 0x0C, PxIS = 0x10, PxIE = 0x14,
              PxCMD = 0x18, PxTFD = 0x20, PxSIG = 0x24, PxSSTS = 0x28, PxSCTL = 0x2C, PxSERR = 0x30, PxCI = 0x38;
constexpr u32 CMD_ST = 1u << 0, CMD_FRE = 1u << 4, CMD_FR = 1u << 14, CMD_CR = 1u << 15;
constexpr u32 TFD_ERR = 1u << 0, TFD_DRQ = 1u << 3, TFD_BSY = 1u << 7;
constexpr u32 IS_TFES = 1u << 30;
constexpr u32 SIG_SATA = 0x00000101;

// ATA commands
constexpr u8 ATA_READ_DMA_EXT = 0x25;
constexpr u8 ATA_WRITE_DMA_EXT = 0x35;
constexpr u8 ATA_FLUSH_CACHE_EXT = 0xEA;
constexpr u8 ATA_IDENTIFY = 0xEC;

constexpr u8 FIS_REG_H2D = 0x27;

constexpr unsigned long TIMEOUT_MS = 5000;

// One page of DMA memory per port: the command list (32 headers, only
// slot 0 used), the received-FIS area and slot 0's command table.
constexpr u64 CLB_OFFSET = 0x000;   // 1 KiB, 1 KiB aligned
constexpr u64 FB_OFFSET = 0x400;    // 256 bytes, 256 aligned
constexpr u64 TABLE_OFFSET = 0x800; // 128 aligned
constexpr u64 IDENTIFY_OFFSET = 0xC00; // 512 bytes for IDENTIFY's answer

struct CommandHeader {
    u16 flags; // bits 0-4: command FIS length in dwords; bit 6: write
    u16 prdtLength;
    u32 bytesTransferred;
    u32 tableLow, tableHigh;
    u32 reserved[4];
};

struct PrdtEntry {
    u32 addressLow, addressHigh;
    u32 reserved;
    u32 byteCount; // bits 0-21: bytes - 1
};

struct CommandTable {
    u8 fis[64];
    u8 atapi[16];
    u8 reserved[48];
    PrdtEntry prdt[1];
};

static_assert(sizeof(CommandHeader) == 32);
static_assert(sizeof(CommandTable) == 0x90);

void log(char const* message) { Sys::log(message); }

class Disk {
public:
    bool start() {
        Sys::DeviceInfo info;
        if (Sys::deviceInfo(DEVICE, info) != Sys::OK)
            return fail("no controller handle");

        long bar = Sys::deviceBar(DEVICE, 5); // ABAR
        _hba = bar > 0 ? static_cast<u8 volatile*>(Sys::memoryMap(bar)) : nullptr;
        if (not _hba)
            return fail("can't map the controller's registers");

        takeOwnership();
        reg(GHC) = reg(GHC) | GHC_AE;
        _wide = reg(CAP) & CAP_S64A;

        long dma = Sys::memoryCreate(4096);
        _dma = dma > 0 ? static_cast<u8*>(Sys::memoryMap(dma)) : nullptr;
        long physical = dma > 0 ? Sys::dmaAddress(DEVICE, dma) : -1;
        if (not _dma or physical < 0)
            return fail("can't get DMA memory");
        _dmaPhysical = static_cast<u64>(physical);

        if (not findDisk())
            return fail("no SATA disk attached");
        if (not startPort())
            return fail("the disk's port won't start");
        if (not identify())
            return fail("the disk didn't answer IDENTIFY");

        Text<> text;
        text.add("SATA disk on port ").add(_port).add(", ").add(_sectors / 2048).add(" MiB");
        log(text.cstr());
        return true;
    }

    // Sets the client's buffer, where all transfers go.
    bool attach(Sys::Handle memory) {
        long physical = Sys::dmaAddress(DEVICE, memory);
        if (physical < 0)
            return false;
        if (not _wide and static_cast<u64>(physical) + Block::BUFFER_SIZE > 0x100000000ul)
            return false;
        if (_buffer)
            Sys::close(_buffer);
        _buffer = memory;
        _bufferPhysical = static_cast<u64>(physical);
        return true;
    }

    long transfer(bool write, u64 lba, u32 count) {
        if (not _buffer or count == 0 or count > Block::MAX_SECTORS)
            return Sys::INVALID_ARGUMENT;
        if (lba >= _sectors or count > _sectors - lba)
            return Sys::INVALID_ARGUMENT;
        return command(write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, count, _bufferPhysical,
                       count * Block::SECTOR_SIZE, write);
    }

    long flush() { return command(ATA_FLUSH_CACHE_EXT, 0, 0, 0, 0, false); }

    u64 sectors() const { return _sectors; }

private:
    bool fail(char const* why) {
        log(why);
        return false;
    }

    u32 volatile& reg(u32 offset) { return *reinterpret_cast<u32 volatile*>(_hba + offset); }
    u32 volatile& port(u32 offset) { return reg(0x100 + _port * 0x80 + offset); }

    // If the firmware still drives the controller, ask it to let go.
    void takeOwnership() {
        if (not (reg(CAP2) & 1))
            return;
        reg(BOHC) = reg(BOHC) | 2; // OS ownership
        waitUntil([&] { return not (reg(BOHC) & 1); }, 100);
    }

    template <typename F>
    bool waitUntil(F done, unsigned long timeoutMs = TIMEOUT_MS) {
        unsigned long deadline = Sys::timeMs() + timeoutMs;
        while (not done()) {
            if (Sys::timeMs() > deadline)
                return false;
            Sys::yield();
        }
        return true;
    }

    // Prepares each port in turn and uses the first with a SATA disk.
    bool findDisk() {
        u32 implemented = reg(PI);
        for (u32 p = 0; p < 32; p++) {
            if (not (implemented & (1u << p)))
                continue;
            _port = p;
            if (not preparePort())
                continue;
            // Some controllers (QEMU's among them) only report the link
            // once FIS receiving is on, so check after preparing.
            if (not waitUntil([&] { return linkUp(); }, 20))
                reset(); // the firmware may have left the link down
            if (linkUp() and port(PxSIG) == SIG_SATA)
                return true;
            port(PxCMD) = port(PxCMD) & ~CMD_FRE; // not ours: stop using our memory
            waitUntil([&] { return not (port(PxCMD) & CMD_FR); }, 500);
        }
        return false;
    }

    // A device is attached and talking (DET = 3) and awake (IPM = 1).
    bool linkUp() {
        u32 status = port(PxSSTS);
        return (status & 0xF) == 3 and ((status >> 8) & 0xF) == 1;
    }

    // COMRESET: holds the link in reset for a moment, then lets the device
    // announce itself again (its signature arrives with its first FIS).
    void reset() {
        port(PxSCTL) = (port(PxSCTL) & ~0xFu) | 1;
        Sys::sleep(2); // at least 1 ms
        port(PxSCTL) = port(PxSCTL) & ~0xFu;
        if (waitUntil([&] { return linkUp(); }, 100)) {
            port(PxSERR) = ~0u;
            waitUntil([&] { return port(PxSIG) != 0xFFFFFFFF; }, 500);
        }
    }

    // Points the port at our command list and FIS area, and turns on FIS
    // receiving (the command engine starts later, in startPort).
    bool preparePort() {
        // Stop the command engine and FIS receiving before moving their memory.
        port(PxCMD) = port(PxCMD) & ~(CMD_ST | CMD_FRE);
        if (not waitUntil([&] { return not (port(PxCMD) & (CMD_CR | CMD_FR)); }, 500))
            return false;

        u64 clb = _dmaPhysical + CLB_OFFSET;
        u64 fb = _dmaPhysical + FB_OFFSET;
        port(PxCLB) = static_cast<u32>(clb);
        port(PxCLBU) = static_cast<u32>(clb >> 32);
        port(PxFB) = static_cast<u32>(fb);
        port(PxFBU) = static_cast<u32>(fb >> 32);

        auto* header = reinterpret_cast<CommandHeader*>(_dma + CLB_OFFSET);
        u64 table = _dmaPhysical + TABLE_OFFSET;
        header->tableLow = static_cast<u32>(table);
        header->tableHigh = static_cast<u32>(table >> 32);

        port(PxSERR) = ~0u; // clear old errors
        port(PxIS) = ~0u;
        port(PxIE) = 0;     // polling, no interrupts
        if constexpr (RUDRACORE_FAULT_TEST == 5) {
            // FAULT_TEST=msi: interrupts on, as a compromised driver could,
            // so the controller sends the message the kernel aimed at the CPU.
            port(PxIE) = ~0u;
            reg(GHC) = reg(GHC) | 2; // GHC.IE
        }
        port(PxCMD) = port(PxCMD) | CMD_FRE;
        return true;
    }

    bool startPort() {
        if (not waitUntil([&] { return not (port(PxTFD) & (TFD_BSY | TFD_DRQ)); }))
            return false;
        port(PxCMD) = port(PxCMD) | CMD_ST;
        return true;
    }

    bool identify() {
        if (command(ATA_IDENTIFY, 0, 0, _dmaPhysical + IDENTIFY_OFFSET, 512, false) != Sys::OK)
            return false;
        auto const* words = reinterpret_cast<u16 const*>(_dma + IDENTIFY_OFFSET);
        bool lba48 = words[83] & (1u << 10);
        if (lba48)
            _sectors = u64{words[100]} | u64{words[101]} << 16 | u64{words[102]} << 32 | u64{words[103]} << 48;
        else
            _sectors = u64{words[60]} | u64{words[61]} << 16;
        return _sectors > 0;
    }

    // Runs one ATA command in slot 0 and waits for it.
    long command(u8 ata, u64 lba, u32 count, u64 physical, u32 bytes, bool write) {
        if (not waitUntil([&] { return not (port(PxTFD) & (TFD_BSY | TFD_DRQ)); }))
            return Sys::TIMED_OUT;

        auto* header = reinterpret_cast<CommandHeader*>(_dma + CLB_OFFSET);
        auto* table = reinterpret_cast<CommandTable*>(_dma + TABLE_OFFSET);
        for (auto& b : table->fis)
            b = 0;

        header->flags = static_cast<u16>(5 | (write ? 1u << 6 : 0)); // 5 dwords: a register FIS
        header->prdtLength = bytes ? 1 : 0;
        header->bytesTransferred = 0;
        if (bytes) {
            table->prdt[0].addressLow = static_cast<u32>(physical);
            table->prdt[0].addressHigh = static_cast<u32>(physical >> 32);
            table->prdt[0].reserved = 0;
            table->prdt[0].byteCount = bytes - 1;
        }

        u8* fis = table->fis;
        fis[0] = FIS_REG_H2D;
        fis[1] = 1 << 7; // a command, not a control update
        fis[2] = ata;
        fis[4] = static_cast<u8>(lba);
        fis[5] = static_cast<u8>(lba >> 8);
        fis[6] = static_cast<u8>(lba >> 16);
        fis[7] = 1 << 6; // LBA addressing
        fis[8] = static_cast<u8>(lba >> 24);
        fis[9] = static_cast<u8>(lba >> 32);
        fis[10] = static_cast<u8>(lba >> 40);
        fis[12] = static_cast<u8>(count);
        fis[13] = static_cast<u8>(count >> 8);

        asm volatile("" ::: "memory"); // the table is complete before the disk looks
        port(PxIS) = ~0u;
        port(PxCI) = 1;

        // Disks usually finish within microseconds: spin a little before
        // giving the CPU away (a yield can cost other tasks' whole time slices).
        for (int spin = 0; spin < 20000 and (port(PxCI) & 1) and not (port(PxIS) & IS_TFES); spin++)
            asm volatile("pause");

        bool failed = false;
        bool done = waitUntil([&] {
            if (port(PxIS) & IS_TFES) {
                failed = true;
                return true;
            }
            return not (port(PxCI) & 1);
        });
        if (not done) {
            log("command timed out");
            return Sys::TIMED_OUT;
        }
        if (failed or (port(PxTFD) & TFD_ERR)) {
            Text<> text;
            text.add("command ").add(static_cast<unsigned long>(ata)).add(" failed at sector ").add(lba);
            log(text.cstr());
            recover();
            return Sys::INVALID_ARGUMENT;
        }
        return Sys::OK;
    }

    // After an error the port stops; restart it so the next command works.
    void recover() {
        port(PxCMD) = port(PxCMD) & ~CMD_ST;
        waitUntil([&] { return not (port(PxCMD) & CMD_CR); }, 1000);
        port(PxSERR) = ~0u;
        port(PxIS) = ~0u;
        port(PxCMD) = port(PxCMD) | CMD_ST;
    }

    u8 volatile* _hba = nullptr;
    u8* _dma = nullptr;
    u64 _dmaPhysical = 0;
    bool _wide = false;
    u32 _port = 0;
    u64 _sectors = 0;
    Sys::Handle _buffer = Sys::NO_HANDLE;
    u64 _bufferPhysical = 0;
};

Disk disk;

} // namespace

int main() {
    Channel client(CLIENT);
    bool ready = disk.start();

    for (;;) {
        Block::Request request;
        Sys::Handle attached = Sys::NO_HANDLE;
        long result = client.recv(request, &attached);
        if (result == Sys::PEER_CLOSED)
            return 0;

        Block::Reply reply{Sys::INVALID_ARGUMENT, 0};
        if (result == Sys::OK and ready) {
            switch (request.op) {
            case Block::Op::Attach:
                if (attached and disk.attach(attached)) {
                    attached = Sys::NO_HANDLE; // the disk keeps it
                    reply = {Sys::OK, disk.sectors()};
                }
                break;
            case Block::Op::Read: reply.status = disk.transfer(false, request.lba, request.count); break;
            case Block::Op::Write: reply.status = disk.transfer(true, request.lba, request.count); break;
            case Block::Op::Flush: reply.status = disk.flush(); break;
            }
        } else if (not ready) {
            reply.status = Sys::NOT_FOUND;
        }
        if (attached)
            Sys::close(attached);
        client.send(reply);
    }
}
