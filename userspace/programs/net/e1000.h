#pragma once

// The Intel e1000 network card (82540EM and relatives): sends and receives
// Ethernet frames through two rings of descriptors in DMA memory.

#include <kapala/sys.h>

namespace Net {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
using u64 = unsigned long;

class E1000 {
public:
    static constexpr u32 MAX_FRAME = 1514; // without the CRC, which the card adds

    bool start(Kapala::Sys::Handle device);

    u8 const* mac() const { return _mac; }
    bool linkUp();

    // Queues a frame; false if the transmit ring is full.
    bool send(u8 const* frame, u32 length);

    // Calls deliver(frame, length) for each frame received since the last call.
    template <typename F>
    void receive(F deliver) {
        for (u32 n = 0; n < RX_COUNT; n++) {
            RxDescriptor& d = _rx[_rxNext];
            if (not (d.status & 1)) // DD: the card hasn't filled it yet
                break;
            bool whole = (d.status & 2) and d.errors == 0; // EOP, no errors
            if (whole and d.length <= MAX_FRAME + 4)
                deliver(_rxBuffers + _rxNext * BUFFER_SIZE, d.length);
            d.status = 0;
            reg(RDT) = _rxNext; // hand it back to the card
            _rxNext = (_rxNext + 1) % RX_COUNT;
        }
    }

    // Reads and clears the interrupt cause (so the interrupt line drops).
    u32 acknowledgeInterrupt() { return reg(ICR); }

private:
    static constexpr u32 RX_COUNT = 64, TX_COUNT = 32, BUFFER_SIZE = 2048;
    static constexpr u32 CTRL = 0x0000, STATUS = 0x0008, EERD = 0x0014, ICR = 0x00C0, IMS = 0x00D0,
                         IMC = 0x00D8, RCTL = 0x0100, TCTL = 0x0400, TIPG = 0x0410, RDBAL = 0x2800,
                         RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818, TDBAL = 0x3800,
                         TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818, MTA = 0x5200,
                         RAL = 0x5400, RAH = 0x5404;

    struct RxDescriptor {
        u64 address;
        u16 length;
        u16 checksum;
        u8 status;
        u8 errors;
        u16 special;
    };

    struct TxDescriptor {
        u64 address;
        u16 length;
        u8 cso;
        u8 command;
        u8 status;
        u8 css;
        u16 special;
    };

    u32 volatile& reg(u32 offset) { return *reinterpret_cast<u32 volatile*>(_regs + offset); }
    bool readMac();

    u8 volatile* _regs = nullptr;
    RxDescriptor* _rx = nullptr;
    TxDescriptor* _tx = nullptr;
    u8* _rxBuffers = nullptr;
    u8* _txBuffers = nullptr;
    u64 _rxBuffersPhysical = 0, _txBuffersPhysical = 0;
    u32 _rxNext = 0, _txNext = 0;
    u8 _mac[6] = {};
};

} // namespace Net
