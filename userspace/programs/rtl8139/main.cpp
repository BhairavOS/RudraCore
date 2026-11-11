// rtl8139: the driver for Realtek RTL8139 network cards (common in older
// PCs, and available in QEMU as -device rtl8139).
//
//   handle 1: the card (PCI device)
//   handle 2: a channel to the net server (protocol/netdev.h)
//   handle 3: the card's interrupt (optional)
//
// Uses the card's memory-mapped registers (BAR 1). Received frames land
// one after another in a ring buffer; sending uses the card's four
// transmit slots in turn.

#include <kapala/netdriver.h>
#include <kapala/sys.h>

extern "C" void* memcpy(void* dest, void const* src, unsigned long n);

namespace {

namespace Sys = Kapala::Sys;
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
using u64 = unsigned long;

// Registers
constexpr u32 IDR0 = 0x00, TSD0 = 0x10, TSAD0 = 0x20, RBSTART = 0x30, CR = 0x37, CAPR = 0x38, IMR = 0x3C,
              ISR = 0x3E, TCR = 0x40, RCR = 0x44, CONFIG1 = 0x52, MSR = 0x58;
constexpr u8 CR_RESET = 0x10, CR_RX = 0x08, CR_TX = 0x04, CR_BUFFER_EMPTY = 0x01;
constexpr u16 INT_ROK = 1 << 0, INT_RER = 1 << 1, INT_TOK = 1 << 2, INT_TER = 1 << 3, INT_RX_OVERFLOW = 1 << 4,
              INT_LINK = 1 << 5, INT_FIFO_OVERFLOW = 1 << 6;
constexpr u32 TSD_OWN = 1 << 13; // the card is done with the slot

constexpr u32 RX_SIZE = 8192;            // the ring itself
constexpr u32 RX_ALLOC = RX_SIZE + 16 + 1536; // plus room for a frame running past the end (WRAP)
constexpr u32 TX_SLOT = 2048;

class Rtl8139 {
public:
    bool start(Sys::Handle device) {
        long bar = Sys::deviceBar(device, 1);
        _regs = bar > 0 ? static_cast<u8 volatile*>(Sys::memoryMap(bar)) : nullptr;
        if (not _regs)
            return false;

        reg8(CONFIG1) = 0; // power on
        reg8(CR) = CR_RESET;
        for (int i = 0; i < 100 and (reg8(CR) & CR_RESET); i++)
            Sys::sleep(1);

        for (int i = 0; i < 6; i++)
            _mac[i] = reg8(IDR0 + static_cast<u32>(i));

        u64 rxPhysical = 0, txPhysical = 0;
        _rx = dma(device, RX_ALLOC, rxPhysical);
        _tx = dma(device, 4 * TX_SLOT, txPhysical);
        if (not _rx or not _tx or rxPhysical + RX_ALLOC > 0x100000000ul or txPhysical + 4 * TX_SLOT > 0x100000000ul)
            return false; // the card only takes 32-bit addresses

        reg32(RBSTART) = static_cast<u32>(rxPhysical);
        for (u32 i = 0; i < 4; i++)
            reg32(TSAD0 + 4 * i) = static_cast<u32>(txPhysical + i * TX_SLOT);

        reg16(IMR) = INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RX_OVERFLOW | INT_LINK | INT_FIFO_OVERFLOW;
        // Accept broadcast (AB), multicast (AM) and frames to us (APM); WRAP:
        // a frame at the end of the ring continues past it instead of wrapping.
        reg32(RCR) = (1 << 3) | (1 << 2) | (1 << 1) | (1 << 7) | (7u << 8) /* unlimited DMA burst */;
        reg32(TCR) = 6u << 8; // 1024-byte DMA bursts
        reg8(CR) = CR_RX | CR_TX;
        return true;
    }

    u8 const* mac() const { return _mac; }

    bool linkUp() { return not (reg8(MSR) & (1 << 2)); } // LINKB: 1 means link down

    bool send(u8 const* frame, unsigned length) {
        if (length > TX_SLOT)
            return false;
        u32 status = reg32(TSD0 + 4 * _txNext);
        if (_busy[_txNext] and not (status & TSD_OWN))
            return false; // still sending what's there
        memcpy(_tx + _txNext * TX_SLOT, frame, length);
        u32 padded = length < 60 ? 60 : length;
        if (padded > length)
            for (u32 i = length; i < padded; i++)
                _tx[_txNext * TX_SLOT + i] = 0;
        reg32(TSD0 + 4 * _txNext) = padded; // clearing OWN starts the transfer
        _busy[_txNext] = true;
        _txNext = (_txNext + 1) % 4;
        return true;
    }

    template <typename F>
    void receive(F deliver) {
        for (int n = 0; n < 64 and not (reg8(CR) & CR_BUFFER_EMPTY); n++) {
            u8 const* p = _rx + _rxOffset;
            u16 status = static_cast<u16>(p[0] | p[1] << 8);
            u16 length = static_cast<u16>(p[2] | p[3] << 8); // includes the 4-byte CRC
            if (not (status & 1) or length < 18 or length > 1518) {
                restartReceive(); // a damaged header: the ring can't be trusted anymore
                return;
            }
            deliver(p + 4, length - 4u);
            _rxOffset = (_rxOffset + length + 4 + 3) & ~3u;
            _rxOffset %= RX_SIZE;
            reg16(CAPR) = static_cast<u16>(_rxOffset - 16); // (the card wants it 16 behind)
        }
    }

    void acknowledgeInterrupt() { reg16(ISR) = reg16(ISR); } // writing the bits back clears them

private:
    u8 volatile& reg8(u32 r) { return *reinterpret_cast<u8 volatile*>(_regs + r); }
    u16 volatile& reg16(u32 r) { return *reinterpret_cast<u16 volatile*>(_regs + r); }
    u32 volatile& reg32(u32 r) { return *reinterpret_cast<u32 volatile*>(_regs + r); }

    static u8* dma(Sys::Handle device, u64 size, u64& physical) {
        long memory = Sys::memoryCreate(size);
        if (memory < 0)
            return nullptr;
        void* address = Sys::memoryMap(memory);
        long phys = Sys::dmaAddress(device, memory);
        if (not address or phys < 0)
            return nullptr;
        physical = static_cast<u64>(phys);
        return static_cast<u8*>(address);
    }

    void restartReceive() {
        reg8(CR) = CR_TX;
        _rxOffset = 0;
        reg16(CAPR) = static_cast<u16>(0 - 16);
        reg8(CR) = CR_RX | CR_TX;
    }

    u8 volatile* _regs = nullptr;
    u8* _rx = nullptr;
    u8* _tx = nullptr;
    u32 _rxOffset = 0, _txNext = 0;
    bool _busy[4] = {};
    u8 _mac[6] = {};
};

Rtl8139 card;

} // namespace

int main() { return Kapala::runNetDriver(card, "Realtek RTL8139"); }
