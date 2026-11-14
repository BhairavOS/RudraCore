#pragma once

// The RudraCore system call interface, shared by the kernel and userspace
// (Kapalaframe).
//
// Calling convention: `int $0x80` with the call number in rax and arguments
// in rdi, rsi, rdx, r10, r8. The result comes back in rax: >= 0 on success,
// a negative Error on failure. All other registers are preserved.
//
// Handles: programs reach kernel objects (channels, shared memory, devices)
// only through handles, per-program numbers like file descriptors. A handle
// can be passed to another program in a channel message or when spawning
// it; it then moves to that program. Handle 0 is never valid.
//
// A handle is (generation << 8) | slot. The generation changes each time a
// slot is freed, so a closed handle's number never names a later object.
// A program's first handles (from spawn) are still 1, 2, 3...
//
// Each handle carries rights (Rights below), checked on every call. Rights
// can only shrink: handle_restrict drops some, handle_duplicate makes a
// second handle with the same or fewer, and a moved handle keeps exactly
// its rights. A handle with REVOKE can cut off every other handle to its
// object (handle_revoke). Revoked handles fail with REVOKED.

namespace Rudracore::Abi {

constexpr unsigned char SYSCALL_VECTOR = 0x80;

using Handle = unsigned long;
constexpr Handle NO_HANDLE = 0;

enum Syscall : unsigned long {
    // Tasks
    SYS_LOG = 0,     // (char const* text, usize length): write to the kernel log
    SYS_EXIT = 1,    // (long code): end the calling task
    SYS_YIELD = 2,   // (): let other tasks run
    SYS_SLEEP = 3,   // (u64 milliseconds)
    SYS_TASK_ID = 4, // () -> id of the calling task
    SYS_TIME = 5,    // () -> milliseconds since boot
    SYS_SPAWN = 6,   // (char const* name, usize length, Handle const* handles, usize count) -> task id
                     //   Starts a program from the boot image. The handles move to the new
                     //   program, which finds them as handles 1, 2, 3... (NOT_FOUND, or a bad
                     //   handle, leaves them with the caller.) Boot-image programs are part of
                     //   the system, so this moves handles without RIGHT_TRANSFER too: that's
                     //   how init gives a driver a device it can't pass on.
    SYS_SYSTEM_INFO = 7, // (SystemInfo* out)
    SYS_RANDOM = 8,      // (void* out, usize length <= 256): fills it with random bytes, good for keys
    SYS_SPAWN_IMAGE = 15, // (SpawnImage const*) -> task id: starts a program from an ELF file in
                          //   shared memory (an installed app). The handles move to it, as with
                          //   spawn, but each needs RIGHT_TRANSFER.
    SYS_FILE_OPEN = 9,   // (char const* name, usize length) -> Handle to read-only shared memory holding
                         //   a data file from the boot image (e.g. "fonts"); its size is up to the file

    // Handles and channels
    SYS_HANDLE_CLOSE = 10,   // (Handle): also works on a revoked handle
    SYS_CHANNEL_CREATE = 11, // (Handle out[2]): two connected endpoints
    SYS_CHANNEL_SEND = 12,   // (Handle endpoint, void const* data, usize length, Handle attach)
                             //   Never blocks. Needs RIGHT_WRITE. `attach` (or NO_HANDLE) moves to
                             //   the receiver with its rights; it needs RIGHT_TRANSFER.
    SYS_CHANNEL_RECV = 13,   // (Handle endpoint, void* buffer, usize capacity, Handle* attached, u64 flags)
                             //   -> message length. Waits for a message unless RECV_NONBLOCK. Needs
                             //   RIGHT_READ (so does wait). An attached handle revoked on the way
                             //   arrives as NO_HANDLE.
    SYS_WAIT = 14,           // (Handle const* endpoints, usize count, u64 timeoutMs)
                             //   Waits until one of up to MAX_WAIT endpoints has a message (or its
                             //   peer closed), or until the timeout (WAIT_FOREVER: no limit; 0: just
                             //   check). Returns the index of a ready endpoint, or TIMED_OUT.
    SYS_HANDLE_DUPLICATE = 16, // (Handle, u64 rights) -> a new handle to the same object with
                               //   `rights` & the handle's own. Needs RIGHT_DUPLICATE.
    SYS_HANDLE_RESTRICT = 17,  // (Handle, u64 rights): keep only `rights` & the current ones
    SYS_HANDLE_REVOKE = 18,    // (Handle): every other handle to this object, in any program,
                               //   stops working (REVOKED); shared memory is unmapped from every
                               //   other program; blocked waiters wake up. Needs RIGHT_REVOKE.
    SYS_HANDLE_RIGHTS = 19,    // (Handle) -> the handle's rights

    // Memory
    SYS_MEMORY_CREATE = 20, // (usize size) -> Handle to new zeroed shared memory
    SYS_MEMORY_MAP = 21,    // (Handle memory) -> address where it's mapped. Needs RIGHT_READ; writable
                            //   only with RIGHT_WRITE.
    SYS_MEMORY_ALLOC = 22,  // (usize size) -> address of new zeroed private memory (read + write)
    SYS_MEMORY_UNMAP = 23,  // (address returned by SYS_MEMORY_MAP): remove that mapping

    // Devices (drivers only: needs a handle given by init)
    SYS_IRQ_WAIT = 30,  // (Handle irq): waits for the interrupt to fire
    SYS_PORT_IN = 31,   // (Handle ports, u16 port) -> byte read
    SYS_PORT_OUT = 32,  // (Handle ports, u16 port, u8 value)
    SYS_IRQ_BIND = 33,  // (Handle irq, Handle endpoint, u64 cookie)
                        //   From now on each interrupt sends an IrqMessage{cookie} from
                        //   `endpoint` (to its peer), so a driver can wait for several
                        //   IRQs and other messages in one place. The line stays masked
                        //   after each message until SYS_IRQ_ACK.
    SYS_IRQ_ACK = 34,   // (Handle irq): done with the last interrupt, let the next one in
    SYS_DEVICE_INFO = 35, // (Handle device, DeviceInfo* out): what a PCI device is
    SYS_DEVICE_BAR = 36,  // (Handle device, usize index) -> Handle to shared memory: the device's
                          //   registers in memory BAR `index` (map it to use them, uncached).
                          //   Also turns on the device's memory decoding and DMA.
    SYS_DMA_ADDRESS = 37, // (Handle device, Handle memory) -> physical address of shared memory
                          //   (from SYS_MEMORY_CREATE: physically contiguous), so the device can
                          //   read and write it. Keep the memory handle open while the device
                          //   uses it.
};

// A PCI device's identity (SYS_DEVICE_INFO).
struct DeviceInfo {
    unsigned short vendor, device;
    unsigned char classCode, subclass, progIf, irqLine;
    unsigned long barSize[6]; // 0: no memory BAR there
};

struct SpawnImage {
    Handle memory;          // shared memory holding the ELF file
    unsigned long size;     // its length in bytes
    char name[32];          // the task's name
    Handle handles[8];      // given to the program as its handles 1, 2, 3...
    unsigned long handleCount;
};

// What a handle allows. Objects are created with every right that applies.
enum Rights : unsigned long {
    RIGHT_READ = 1 << 0,      // channel: receive, wait; memory: map; IRQ: wait, bind, ack;
                              // ports: in; device: info
    RIGHT_WRITE = 1 << 1,     // channel: send; memory: map writable, DMA target; ports: out
    RIGHT_TRANSFER = 1 << 2,  // move it to another program
    RIGHT_DUPLICATE = 1 << 3, // make another handle to the same object
    RIGHT_REVOKE = 1 << 4,    // cut off every other handle to the object
    RIGHT_MMIO = 1 << 5,      // device: map its registers (device_bar)
    RIGHT_DMA = 1 << 6,       // device: get physical addresses for it (dma_address)
    RIGHTS_ALL = (1 << 7) - 1,
};

struct IrqMessage {
    unsigned long cookie;
};

enum RecvFlags : unsigned long {
    RECV_NONBLOCK = 1 << 0, // return WOULD_BLOCK instead of waiting
};

constexpr unsigned long MAX_MESSAGE_SIZE = 128;
constexpr unsigned long MAX_WAIT = 32;
constexpr unsigned long WAIT_FOREVER = ~0ul;

enum Error : long {
    OK = 0,
    UNKNOWN_SYSCALL = -1,
    BAD_ADDRESS = -2,      // a pointer argument isn't valid user memory
    INVALID_ARGUMENT = -3,
    BAD_HANDLE = -4,       // not an open handle
    WRONG_TYPE = -5,       // the handle refers to another kind of object
    PEER_CLOSED = -6,      // the other end of the channel is gone
    WOULD_BLOCK = -7,
    QUEUE_FULL = -8,
    NO_SPACE = -9,         // handle table or address space full
    NOT_FOUND = -10,
    BUFFER_TOO_SMALL = -11,
    OUT_OF_MEMORY = -12,
    TIMED_OUT = -13,

    // Used by userspace servers (file system)
    ALREADY_EXISTS = -14,
    NOT_EMPTY = -15,    // a directory that still has entries
    IO_ERROR = -16,     // the disk failed
    IS_DIRECTORY = -17, // wanted a file
    NOT_DIRECTORY = -18,

    // Used by the network server
    CONNECTION_REFUSED = -19,
    CONNECTION_RESET = -20,
    NETWORK_DOWN = -21, // no address yet (DHCP hasn't answered) or no network card

    // Capabilities
    ACCESS_DENIED = -22, // the handle lacks a right the call needs
    REVOKED = -23,       // the handle was revoked
};

// Handles init starts with (when the hardware exists).
enum InitHandle : Handle {
    INIT_HANDLE_FRAMEBUFFER = 1, // shared memory: the screen
    INIT_HANDLE_PS2_PORTS = 2,   // I/O ports 0x60-0x64 (PS/2 controller)
    INIT_HANDLE_KEYBOARD_IRQ = 3,
    INIT_HANDLE_MOUSE_IRQ = 4,
    INIT_HANDLE_STORAGE = 5,     // PCI device: the AHCI (SATA) disk controller
    INIT_HANDLE_NETWORK = 6,     // PCI device: the network card (Intel e1000 family)
    INIT_HANDLE_NETWORK_IRQ = 7, // its interrupt line
};

struct SystemInfo {
    unsigned long uptimeMs;
    unsigned long totalMemory;
    unsigned long freeMemory;
    unsigned long taskCount;

    // Framebuffer (all zero if there's none). 32 bits per pixel.
    unsigned long screenWidth;
    unsigned long screenHeight;
    unsigned long screenPitch; // bytes per row
    unsigned char redShift, greenShift, blueShift, reserved;

    unsigned long timeOfDay; // seconds since midnight, from the hardware clock (UTC)
    unsigned long unixTime;  // seconds since 1970-01-01 (UTC)

    unsigned long bootMode; // BOOT_NORMAL, BOOT_SAFE or BOOT_RECOVERY: the boot entry chosen
};

// SystemInfo.bootMode. Safe: the known-good minimum (no network, no
// third-party apps, no self-tests). Recovery: repair (no third-party apps,
// network on, a Terminal opens).
constexpr unsigned long BOOT_NORMAL = 0, BOOT_SAFE = 1, BOOT_RECOVERY = 2;

} // namespace Rudracore::Abi
