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
.\bhairav.ps1 build       # builds build\bhairavos.iso
.\bhairav.ps1 run-uefi    # boots it through ShoolLoader🔱 (UEFI)
.\bhairav.ps1 run         # boots it through Limine (legacy BIOS)
.\bhairav.ps1 shot        # boots headless, saves build\screen.png and build\serial.log
```

For a QEMU window on Windows: `winget install SoftwareFreedomConservancy.QEMU`.
After the boot screen, the Kapalaframe showcase takes over: move the mouse,
click "Say namaste", and type into the text field (Enter logs the line).
Click inside the QEMU window to give it the mouse; Ctrl+Alt+G releases it.

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
- **Device handles** (an IRQ line, a range of I/O ports) let a driver use
  exactly the hardware it was given, nothing else. An IRQ can be bound to a
  channel, so a driver waits for several interrupts and messages in one place.

At boot the kernel starts one program, **init**, and gives it handles to the
hardware. init starts every other program with only the handles it needs:
the `ps2` driver gets the controller's ports and the keyboard and mouse IRQs
plus a channel for input events; the `showcase` app gets the screen and the
other end of that channel. When a program maps the screen, the kernel stops
drawing on it (logs continue on serial; a panic still takes the screen back).
System calls are listed in `abi/rudracore/syscalls.h`.

Programs can use floating point and SSE: the kernel saves each task's FPU
registers on every switch (the kernel itself is built without SSE).

## Kapalaframe

The library every program links against (`userspace/kapalaframe/`):

| Header | What it gives |
|---|---|
| `kapala/sys.h` | System call wrappers |
| `kapala/channel.h` | `Channel`: typed messages over channels |
| `kapala/heap.h` | The heap behind `new` and `delete` |
| `kapala/gfx.h` | `Bitmap`, `Color`, `Rect` and `Canvas`: clipped, anti-aliased rounded rectangles, circles, gradients, shadows, text and images, plus the Bhairav OS theme colors |
| `kapala/text.h` | `Text`: build strings without the heap |
| `kapala/protocol/input.h` | Keyboard and mouse events |

## Layout

| Path | What it is |
|---|---|
| `kernel/src/main.cpp` | Kernel entry point (`kmain`) |
| `kernel/src/boot.*` | Boot protocol glue: the only code that knows about the bootloader |
| `kernel/src/arch/x86_64/` | CPU-specific code: entry, GDT, IDT, PIC, timer, paging |
| `kernel/src/mem/` | Physical memory manager, kernel heap, virtual memory interface |
| `kernel/src/core/` | Scheduler, system calls, handles, channels, shared memory, device objects, ELF loader |
| `kernel/src/panic.*` | Panic screen and backtraces |
| `kernel/src/gfx/` | Framebuffer drawing, font, log console, boot screen |
| `abi/rudracore/` | The system call interface shared by kernel and userspace |
| `abi/shool/` | The Shool boot protocol shared by ShoolLoader🔱 and the kernel |
| `shoolloader/src/` | ShoolLoader🔱, the UEFI bootloader (own UEFI definitions in `efi.h`) |
| `userspace/kapalaframe/` | Kapalaframe: the runtime and libraries every program uses |
| `userspace/programs/<name>/` | Programs, built to `<name>.elf` and shipped in the boot image |
| `assets/` | The Bhairav logo, converted at build time for the boot screen |
| `boot/shool.cfg` | ShoolLoader🔱 configuration: kernel, programs, screen mode |
| `boot/limine.conf` | Limine configuration, for legacy BIOS boot |
| `legacy/` | Earlier sources, kept for reference, not built |

Programs today: `init` (root of userspace), `ps2` (keyboard and mouse
driver), `showcase` (the first Kapalaframe app), and `ipc-test`,
`protect-test` and `fpu-test` (checks run at every boot).

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
   IRQs as messages, PS/2 keyboard + mouse driver, Kapalaframe graphics,
   and the showcase app
7. **KamalShell🪷**: the compositor and graphical shell

## License

GPL-3.0. See `LICENSE`. The boot console font is Terminus (SIL Open Font
License), embedded from the toolchain image.
