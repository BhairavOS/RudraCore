# RudraCore

**Efficient microkernel for Bhairav OS**, written from scratch in C++23 for x86_64.

Bhairav OS is a completely new, independent operating system not based on
Linux🐧, Unix, or Windows🪟. It is designed from the ground up with a unique
kernel and architecture to provide a fresh approach to modern computing. This
innovative OS focuses on creating a stable, efficient, and secure environment
for both developers and end users.

| Component | What it is |
|---|---|
| **ShoolLoader🔱** | Trident of System Awakening: the UEFI bootloader |
| **RudraCore** | Efficient microkernel for Bhairav OS (this repository) |
| **Kapalaframe** | Framework for efficient application development |
| **KamalShell🪷** | Beautiful and Responsive Graphical Shell |

## Build and run (Windows)

You only need [Docker Desktop](https://www.docker.com/products/docker-desktop/).
The compiler, assembler, ISO tools and QEMU all run inside a container.

```powershell
.\bhairav.ps1 toolchain   # first time, and after Dockerfile changes: build the toolchain image
.\bhairav.ps1 build       # builds build\bhairavos.iso (and disk.img, the first time)
.\bhairav.ps1 run-uefi    # boots it through ShoolLoader🔱 (UEFI)
.\bhairav.ps1 run         # boots it through Limine (legacy BIOS)
.\bhairav.ps1 shot        # boots headless, saves build\screen.png and build\serial.log
```

For a QEMU window on Windows: `winget install SoftwareFreedomConservancy.QEMU`.
After the boot screen, KamalShell🪷 starts with the About window open.
Click the lotus in the corner to open the launcher (About, Notes, System
Monitor), drag windows by their title bar, click one to bring it to the
front, and close it with its x. Click inside the QEMU window to give it the
mouse; Ctrl+Alt+G releases it.

QEMU also gets a disk: `disk.img`, a 64 MiB FAT32 volume created by the
first build. Notes keeps its text there in `/notes.txt` (Ctrl+S saves, and
closing the window saves too). The disk survives rebuilds and `clean`;
delete `disk.img` to start over. Since it's plain FAT32, other systems can
read it too, e.g. `mdir -i disk.img ::/` with mtools in `.\bhairav.ps1 shell`.

On Linux, install the packages listed in the `Dockerfile` and run `make`,
`make run`, `make run-uefi` or `make shot` directly.

Testing options (inside `.\bhairav.ps1 shell`, or on Linux):

```sh
make shot PANIC_TEST=1                 # fault on purpose to check the panic screen
make shot FIRMWARE=uefi WAIT=9         # boot through UEFI instead of BIOS
MEMORY=4G make shot                    # boot with more RAM
KEYS="h i ret" make shot               # type on the virtual keyboard after boot
MONITOR="mouse_move 41 13; mouse_button 1; mouse_button 0" make shot   # use the mouse
```

## How it boots

`build/bhairavos.iso` boots on both kinds of firmware, from a CD or a USB stick:

- **UEFI → ShoolLoader🔱.** The ISO's UEFI boot image (`build/efiboot.img`,
  a FAT volume) holds `EFI/BOOT/BOOTX64.EFI` (ShoolLoader), the kernel, the
  programs and `boot/shool.cfg`. ShoolLoader reads the config, loads
  everything, picks a screen mode, shows the trident, builds the first page
  tables, exits the firmware and jumps into RudraCore with the **Shool boot
  protocol** (`abi/shool/boot.h`).
- **Legacy BIOS → Limine.** ShoolLoader is UEFI-only, so BIOS machines boot
  through [Limine](https://github.com/limine-bootloader/limine)
  (BSD-2-Clause), configured by `boot/limine.conf`.

The kernel tells the two apart by the magic number ShoolLoader passes in
`rdi`; all bootloader-specific code is in `kernel/src/boot.cpp`.

## How it works

RudraCore is a microkernel: the kernel only does what can't be done
anywhere else (scheduling, memory protection, message passing, delivering
interrupts). Drivers and services are ordinary user programs, each in its
own address space, so a crashing driver can't take the system down.

Programs reach kernel objects only through **handles**, private per-program
numbers. Holding a handle is the permission to use the object:

- **Channels** carry messages (up to 128 bytes, plus one handle) between programs.
- **Shared memory** can be mapped by every program holding a handle to it.
- **Device handles** (an IRQ line, a range of I/O ports, a PCI device) let a
  driver use exactly the hardware it was given, nothing else. An IRQ can be
  bound to a channel, so a driver waits for several interrupts and messages
  in one place. A PCI device handle maps the device's registers and gives
  the physical address of the driver's shared memory, so the device can
  read and write it directly (DMA).

At boot the kernel starts one program, **init**, and gives it handles to the
hardware. init starts every other program with only the handles it needs:
the `ps2` driver gets the controller's ports and the keyboard and mouse IRQs
plus a channel for input events; **KamalShell** gets the screen and the
other end of that channel. The kernel finds the disk controller on the PCI
bus; init gives it to the `ahci` driver, and connects `fs` to that driver.
When a program maps the screen, the kernel stops
drawing on it (logs continue on serial; a panic still takes the screen back).
A program can wait on many channels at once, with a timeout (`SYS_WAIT`).
System calls are listed in `abi/rudracore/syscalls.h`.

## KamalShell🪷

The compositor and desktop (`userspace/programs/kamalshell/`). Apps are
separate programs: the shell starts each one with a channel to itself, the
app draws its window into shared memory, and the shell composes all windows
(rounded corners, shadows, focus, dragging) with the taskbar, launcher and
clock. Only changed areas are redrawn. The protocol is in
`kapala/protocol/shell.h`; apps use `Kapala::Window`:

```cpp
Window window;
window.open("Hello", 400, 300);
Canvas canvas(window.surface());
canvas.fill(window.surface().bounds(), Theme::SURFACE);
canvas.text({20, 20}, "namaste", Theme::SAFFRON, 2);
window.present();

for (;;) {
    WindowEvent e;
    window.waitEvent(e);
    if (e.kind == WindowEvent::Close)
        return 0;
}
```

To add an app: create `userspace/programs/<name>/main.cpp`, add
`module = /boot/<name>.elf` to `boot/shool.cfg` (and `boot/limine.conf`),
and add it to `APPS` in KamalShell's `main.cpp`.

Programs can use floating point and SSE: the kernel saves each task's FPU
registers on every switch (the kernel itself is built without SSE).

## Storage

Two programs, talking over channels:

- **`ahci`** (`userspace/programs/ahci/`) drives the SATA controller found
  in most PCs and in QEMU's q35 machine. It uses the first SATA disk and
  reads and writes sectors straight into its client's shared buffer (up to
  64 KiB per request). Protocol: `kapala/protocol/block.h`.
- **`fs`** (`userspace/programs/fs/`) serves files from a **FAT32** volume
  (the whole disk, or its first MBR partition), with long file names and
  directories, so the same disk works on Windows, Linux and macOS. Changes
  reach the disk before each reply. Protocol: `kapala/protocol/fs.h`.

KamalShell connects every app it starts to `fs`, as the app's handle 2.
Apps use `Kapala::Files`:

```cpp
Files files;
files.connect(2);
files.write("/hello.txt", "namaste", 7);
char text[64];
long n = files.read("/hello.txt", text, sizeof(text));
files.makeDir("/notes");
FileEntry entries[16];
long count = files.list("/", entries, 16);
```

Paths are absolute and case-insensitive. The `fs-test` program checks all
of this at every boot and removes what it made.

## Kapalaframe

The library every program links against (`userspace/kapalaframe/`):

| Header | What it gives |
|---|---|
| `kapala/sys.h` | System call wrappers |
| `kapala/channel.h` | `Channel`: typed messages over channels |
| `kapala/heap.h` | The heap behind `new` and `delete` |
| `kapala/gfx.h` | `Bitmap`, `Color`, `Rect` and `Canvas`: clipped, anti-aliased rounded rectangles, circles, gradients, shadows, text and images, plus the Bhairav OS theme colors |
| `kapala/window.h` | `Window`: a window on KamalShell, with events and timeouts |
| `kapala/files.h` | `Files`: read, write, append, list, remove and make directories on the disk |
| `kapala/text.h` | `Text`: build strings without the heap |
| `kapala/protocol/input.h` | Keyboard and mouse events |
| `kapala/protocol/shell.h` | The KamalShell window protocol |
| `kapala/protocol/block.h`, `fs.h` | The disk driver and file system protocols |

## Layout

| Path | What it is |
|---|---|
| `kernel/src/main.cpp` | Kernel entry point (`kmain`) |
| `kernel/src/boot.*` | Boot protocol glue: the only code that knows about the bootloader |
| `kernel/src/arch/x86_64/` | CPU-specific code: entry, GDT, IDT, PIC, timer, clock, paging, PCI |
| `kernel/src/mem/` | Physical memory manager, kernel heap, virtual memory interface |
| `kernel/src/core/` | Scheduler, system calls, handles, channels, shared memory, device objects, ELF loader |
| `kernel/src/panic.*` | Panic screen and backtraces |
| `kernel/src/gfx/` | Framebuffer drawing, font, log console, boot screen |
| `abi/rudracore/` | The system call interface shared by kernel and userspace |
| `abi/shool/` | The Shool boot protocol shared by ShoolLoader🔱 and the kernel |
| `shoolloader/src/` | ShoolLoader🔱, the UEFI bootloader (own UEFI definitions in `efi.h`) |
| `userspace/kapalaframe/` | Kapalaframe: the runtime and libraries every program uses |
| `userspace/programs/<name>/` | Programs, built to `<name>.elf` and shipped in the boot image |
| `assets/` | The Bhairav logo (converted at build time for the boot screen), and the disk's welcome file |
| `boot/shool.cfg` | ShoolLoader🔱 configuration: kernel, programs, screen mode |
| `boot/limine.conf` | Limine configuration, for legacy BIOS boot |
| `legacy/` | Earlier sources, kept for reference, not built |

Programs today: `init` (root of userspace), `ps2` (keyboard and mouse
driver), `ahci` (disk driver), `fs` (file system), `kamalshell` (the
desktop), the apps `about`, `notes` and `monitor`, and `ipc-test`,
`protect-test`, `fpu-test` and `fs-test` (checks run at every boot).

## Roadmap

Bhairav OS is built from scratch, one working step at a time:

1. **Boot** ✅: boots on BIOS and UEFI, boot screen, logs to screen and serial
2. **CPU and memory** ✅: GDT/TSS, IDT with a panic screen, physical memory
   manager, own page tables (no memory both writable and executable)
3. **Tasks and userspace** ✅: timer, kernel heap, preemptive scheduler,
   system calls, ELF loader, isolated user programs
4. **IPC** ✅: handles, channels, shared memory, IRQ and I/O port
   capabilities, spawn; the keyboard driver runs in userspace
5. **ShoolLoader🔱** ✅: own UEFI bootloader with its own boot protocol,
   config file, screen-mode selection and trident boot screen; Limine stays
   only for legacy BIOS
6. **Kapalaframe + drivers** ✅: floating point in programs, program heap,
   IRQs as messages, PS/2 keyboard + mouse driver, Kapalaframe graphics
7. **KamalShell🪷** ✅: compositor, windows (focus, drag, close), taskbar,
   launcher, clock; apps About, Notes and System Monitor; multi-channel
   wait with timeouts
8. **Storage** ✅: PCI devices and DMA for drivers, AHCI (SATA) disk
   driver, FAT32 file system server with long names and directories,
   `Kapala::Files`; Notes saves to disk; the clock knows the date

Next: a terminal and a file manager, NVMe disks (most new laptops),
loading programs from disk, multi-core, USB input, and smoother text.

## License

GPL-3.0. See `LICENSE`. The boot console font is Terminus (SIL Open Font
License), embedded from the toolchain image.
