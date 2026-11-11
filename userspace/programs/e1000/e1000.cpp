#include "e1000.h"

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memset(void* dest, int value, unsigned long n);
}

namespace Driver {

namespace Sys = Kapala::Sys;

namespace {

// Shared memory the card can reach, and its physical address.
template <typename T>
T* dmaMemory(Sys::Handle device, u64 size, u64& physical) {
    long memory = Sys::memoryCreate(size);
    if (memory < 0)
        return nullptr;
    void* address = Sys::memoryMap(memory);
    long phys = Sys::dmaAddress(device, memory);
    if (not address or phys < 0)
        return nullptr;
    physical = static_cast<u64>(phys);
    return static_cast<T*>(address); // (the handle stays open: the card uses the memory for good)
}

} // namespace

bool E1000::start(Sys::Handle device) {
    long bar = Sys::deviceBar(device, 0);
    _regs = bar > 0 ? static_cast<u8 volatile*>(Sys::memoryMap(bar)) : nullptr;
    if (not _regs)
        return false;

    // Reset, then bring the link up.
    reg(IMC) = ~0u;
    reg(CTRL) = reg(CTRL) | (1u << 26); // RST
    Sys::sleep(2);
    for (int i = 0; i < 100 and (reg(CTRL) & (1u << 26)); i++)
        Sys::sleep(1);
    reg(IMC) = ~0u;
    u32 ctrl = reg(CTRL);
    ctrl |= (1u << 6) | (1u << 5);                         // SLU (set link up), ASDE
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 30) | (1u << 7)); // LRST, PHY_RST, VME, ILOS
    reg(CTRL) = ctrl;

    if (not readMac())
        return false;
    for (u32 i = 0; i < 128; i++)
        reg(MTA + i * 4) = 0;

    u64 ringsPhysical;
    auto* rings = dmaMemory<u8>(device, 4096, ringsPhysical);
    _rxBuffers = dmaMemory<u8>(device, RX_COUNT * BUFFER_SIZE, _rxBuffersPhysical);
    _txBuffers = dmaMemory<u8>(device, TX_COUNT * BUFFER_SIZE, _txBuffersPhysical);
    if (not rings or not _rxBuffers or not _txBuffers)
        return false;
    _rx = reinterpret_cast<RxDescriptor*>(rings);
    _tx = reinterpret_cast<TxDescriptor*>(rings + 2048);
    u64 rxPhysical = ringsPhysical, txPhysical = ringsPhysical + 2048;

    // Receiving: every descriptor owns one 2 KiB buffer.
    for (u32 i = 0; i < RX_COUNT; i++) {
        _rx[i] = {};
        _rx[i].address = _rxBuffersPhysical + i * BUFFER_SIZE;
    }
    reg(RDBAL) = static_cast<u32>(rxPhysical);
    reg(RDBAH) = static_cast<u32>(rxPhysical >> 32);
    reg(RDLEN) = RX_COUNT * sizeof(RxDescriptor);
    reg(RDH) = 0;
    reg(RDT) = RX_COUNT - 1;
    // EN, BAM (broadcasts), 2 KiB buffers, SECRC (strip the CRC)
    reg(RCTL) = (1u << 1) | (1u << 15) | (1u << 26);

    // Sending: descriptors start out "done", i.e. free.
    for (u32 i = 0; i < TX_COUNT; i++) {
        _tx[i] = {};
        _tx[i].address = _txBuffersPhysical + i * BUFFER_SIZE;
        _tx[i].status = 1;
    }
    reg(TDBAL) = static_cast<u32>(txPhysical);
    reg(TDBAH) = static_cast<u32>(txPhysical >> 32);
    reg(TDLEN) = TX_COUNT * sizeof(TxDescriptor);
    reg(TDH) = 0;
    reg(TDT) = 0;
    // EN, PSP (pad short packets), collision threshold 15, collision distance 64
    reg(TCTL) = (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12);
    reg(TIPG) = 10 | (8u << 10) | (6u << 20);

    // Interrupts: frame received (RXT0), receive ring low (RXDMT0), overrun
    // (RXO), link change (LSC).
    reg(ICR);
    reg(IMS) = (1u << 7) | (1u << 4) | (1u << 6) | (1u << 2);
    return true;
}

bool E1000::readMac() {
    u32 low = reg(RAL), high = reg(RAH);
    if (high & (1u << 31)) { // address valid: the firmware or reset loaded it
        for (int i = 0; i < 4; i++)
            _mac[i] = static_cast<u8>(low >> (i * 8));
        _mac[4] = static_cast<u8>(high);
        _mac[5] = static_cast<u8>(high >> 8);
        return true;
    }
    // Otherwise read the EEPROM: words 0-2.
    for (u32 word = 0; word < 3; word++) {
        reg(EERD) = (word << 8) | 1;
        u32 value = 0;
        int tries = 0;
        while (not ((value = reg(EERD)) & (1u << 4))) {
            if (++tries > 10000)
                return false;
        }
        _mac[word * 2] = static_cast<u8>(value >> 16);
        _mac[word * 2 + 1] = static_cast<u8>(value >> 24);
    }
    reg(RAL) = u32{_mac[0]} | u32{_mac[1]} << 8 | u32{_mac[2]} << 16 | u32{_mac[3]} << 24;
    reg(RAH) = u32{_mac[4]} | u32{_mac[5]} << 8 | (1u << 31);
    return true;
}

bool E1000::linkUp() { return reg(STATUS) & 2; }

bool E1000::send(u8 const* frame, u32 length) {
    if (length > MAX_FRAME)
        return false;
    TxDescriptor& d = _tx[_txNext];
    if (not (d.status & 1))
        return false; // the card is still sending what was here
    memcpy(_txBuffers + _txNext * BUFFER_SIZE, frame, length);
    d.length = static_cast<u16>(length);
    d.command = (1 << 0) | (1 << 1) | (1 << 3); // EOP, IFCS (add the CRC), RS (report status)
    d.status = 0;
    asm volatile("" ::: "memory");
    _txNext = (_txNext + 1) % TX_COUNT;
    reg(TDT) = _txNext;
    return true;
}

} // namespace Driver
