# RudraCore

The microkernel of **BhairavOS** 🪷, written from scratch in C++23 for x86_64.

## Build and run (Windows)

You only need [Docker Desktop](https://www.docker.com/products/docker-desktop/).
The compiler, assembler, ISO tools and QEMU all run inside a container.

```powershell
.\bhairav.ps1 toolchain   # first time only: build the toolchain image
.\bhairav.ps1 build       # builds build\bhairavos.iso
.\bhairav.ps1 run         # boots it (opens a window if QEMU for Windows is installed)
.\bhairav.ps1 shot        # boots headless, saves build\screen.png and build\serial.log
```

For a QEMU window on Windows: `winget install SoftwareFreedomConservancy.QEMU`.

On Linux, install the packages listed in the `Dockerfile` and run `make`,
`make run`, `make run-uefi` or `make shot` directly.

Testing options (inside `.\bhairav.ps1 shell`, or on Linux):

```sh
make shot PANIC_TEST=1            # fault on purpose to check the panic screen
make shot FIRMWARE=uefi WAIT=8    # boot through UEFI instead of BIOS
MEMORY=4G make shot               # boot with more RAM
```

## Layout

| Path | What it is |
|---|---|
| `kernel/src/main.cpp` | Kernel entry point (`kmain`) |
| `kernel/src/boot.*` | Boot protocol glue: the only code that knows about the bootloader |
| `kernel/src/arch/x86_64/` | CPU-specific code: entry, GDT, IDT, PIC, paging |
| `kernel/src/mem/` | Physical memory manager and the virtual memory interface |
| `kernel/src/panic.*` | Panic screen and backtraces |
| `kernel/src/gfx/` | Framebuffer drawing, font, log console, boot splash |
| `kernel/src/log.*`, `serial.*` | Kernel logging to COM1 and the screen |
| `kernel/linker.ld` | Kernel memory layout (higher half) |
| `boot/limine.conf` | Boot menu configuration |
| `legacy/` | Earlier skift-derived sources, kept for reference, not built |

## Roadmap

BhairavOS is built from scratch, one working step at a time:

1. **Boot** ✅: boots on BIOS and UEFI, draws the splash screen, logs to screen and serial
2. **CPU and memory** ✅: GDT/TSS, IDT with a panic screen for CPU exceptions,
   bitmap physical memory manager, own 4-level page tables (no memory is both
   writable and executable), bootloader memory reclaimed
3. **Tasks**: timer, scheduler, syscalls, first userspace program
4. **ShoolLoader** 🔱: own UEFI bootloader, replacing Limine
5. **KapalaFrame**: the C++ framework for BhairavOS applications
6. **Graphics**: userspace framebuffer, input, compositor
7. **Kamala** 🪷: the graphical shell

Until ShoolLoader is ready, the kernel boots through
[Limine](https://github.com/limine-bootloader/limine) (BSD-2-Clause). All
Limine-specific code is in `kernel/src/boot.cpp`, so switching bootloaders
only touches that file.

## License

GPL-3.0. See `LICENSE`.
