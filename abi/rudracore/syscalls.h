#pragma once

// The RudraCore system call interface, shared by the kernel and userspace
// (Kapalaframe).
//
// Calling convention: `int $0x80` with the call number in rax and arguments
// in rdi, rsi, rdx, r10, r8. The result comes back in rax: >= 0 on success,
// a negative Error on failure. All other registers are preserved.
//
// Handles: programs reach kernel objects (channels, shared memory, devices)
// only through handles, small per-program numbers like file descriptors.
// A handle can be passed to another program in a channel message or when
// spawning it; it then moves to that program. Handle 0 is never valid.

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
                     //   handle, leaves them with the caller.)
    SYS_SYSTEM_INFO = 7, // (SystemInfo* out)
    SYS_RANDOM = 8,      // (void* out, usize length <= 256): fills it with random bytes, good for keys
    SYS_SPAWN_IMAGE = 15, // (SpawnImage const*) -> task id: starts a program from an ELF file in
                          //   shared memory (an installed app). The handles move to it, as with spawn.
    SYS_FILE_OPEN = 9,   // (char const* name, usize length) -> Handle to read-only shared memory holding
                         //   a data file from the boot image (e.g. "fonts"); its size is up to the file

    // Handles and channels
    SYS_HANDLE_CLOSE = 10,   // (Handle)
    SYS_CHANNEL_CREATE = 11, // (Handle out[2]): two connected endpoints
    SYS_CHANNEL_SEND = 12,   // (Handle endpoint, void const* data, usize length, Handle attach)
                             //   Never blocks. `attach` (or NO_HANDLE) moves to the receiver.
    SYS_CHANNEL_RECV = 13,   // (Handle endpoint, void* buffer, usize capacity, Handle* attached, u64 flags)
                             //   -> message length. Waits for a message unless RECV_NONBLOCK.
    SYS_WAIT = 14,           // (Handle const* endpoints, usize count, u64 timeoutMs)
                             //   Waits until one of up to MAX_WAIT endpoints has a message (or its
                             //   peer closed), or until the timeout (WAIT_FOREVER: no limit; 0: just
                             //   check). Returns the index of a ready endpoint, or TIMED_OUT.

    // Memory
    SYS_MEMORY_CREATE = 20, // (usize size) -> Handle to new zeroed shared memory
    SYS_MEMORY_MAP = 21,    // (Handle memory) -> address where it's mapped (read + write)
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
};

} // namespace Rudracore::Abi
