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
After the boot screen, KamalShell🪷 shows a clean desktop: a thin bar at
the top and a dock at the bottom. Click an app in the dock to open it (or
to bring it back if it's open or minimized). In a window's top-left
corner, the three controls close (rose), minimize (gold) and maximize
(jade); double-clicking the title bar also maximizes. Drag windows by
their title bar and resize them by the bottom-right corner. The lotus at
the top left opens a small menu. Click inside the QEMU window to give it
the mouse; Ctrl+Alt+G releases it.

QEMU gives Bhairav OS a network card (an Intel e1000) on its built-in
network, which reaches the internet through your PC, so the Browser and
the Terminal's `ping`, `host` and `get` work out of the box.

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
make shot SECURITY_TEST=1 WAIT=16      # also run oom-test: a program using up all memory must not crash the kernel
make shot FIRMWARE=uefi WAIT=9         # boot through UEFI instead of BIOS
STEP=0.1 MONITOR="..." make shot       # faster mouse commands (e.g. double clicks)
NIC=rtl8139 make shot                  # a Realtek network card instead of the Intel one
sh scripts/image-test.sh               # check the picture decoders against ImageMagick
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
Likewise the network card and its interrupt go to `net`.
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

## Network

Network cards have their own driver programs: **`e1000`** (Intel e1000
family: QEMU, VirtualBox, VMware and many PCs) and **`rtl8139`** (Realtek
RTL8139). init starts the one that matches the card. They share their main
loop (`kapala/netdriver.h`) and hand frames to the network server through
two rings in shared memory (`kapala/protocol/netdev.h`), so frames aren't
copied through messages.

**`net`** (`userspace/programs/net/`) runs the TCP/IP stack, written from
scratch: Ethernet, ARP, IPv4, ICMP (ping), UDP, a DHCP client (it gets its
address at boot), a DNS resolver with a cache, and TCP connections with
retransmission.

KamalShell connects every app to `net` as its handle 3. Apps use
`Kapala::Net`:

```cpp
Net net;
net.connect(3);
unsigned address;
net.resolve("example.com", address);
long socket = net.tcpConnect(address, 80);
net.send(socket, request, length);
long n = net.receive(socket, buffer, sizeof(buffer)); // 0: the other side closed
net.close(socket);
```

The `net-test` program checks DHCP, ping, DNS and an HTTP download at
every boot.

### HTTPS

`Kapala::Tls` (`kapala/tls.h`) speaks **TLS 1.3** over a TCP socket:
X25519 key exchange and ChaCha20-Poly1305 or AES-128-GCM encryption. The
cryptography (`kapala/crypto.h`: SHA-256, HMAC, HKDF, X25519,
ChaCha20-Poly1305, AES-GCM) is written from scratch, and `tls-test`
checks it against the published test vectors at every boot, then makes
real HTTPS connections. Keys come from the kernel's random number
generator (`SYS_RANDOM`): ChaCha20, stirred with RDRAND when the CPU has
it and with cycle-counter timing at every interrupt.

**Not yet:** the server's certificate isn't checked, so a connection is
private from eavesdroppers but can't prove it reaches the real site (an
attacker in the middle could pretend to be it). The browser says so.
Servers that only speak TLS 1.2 don't work yet.

## Bhairav Browser

`userspace/programs/browser/`: loads pages over HTTP (following
redirects), lays out HTML (headings, paragraphs, lists, links, emphasis,
code, preformatted text, quotes, rules, simple tables, image descriptions,
entities) and draws it on a paper-colored page. Links, back/reload/home,
an address bar that also searches (typing words searches through
[FrogFind](http://frogfind.com), a search engine made for small browsers),
and scrolling with the wheel, keys or the scrollbar.

HTTPS works through `Kapala::Tls` (see above). If a site's TLS can't be
spoken, the browser explains why and offers a simplified copy through
FrogFind's reader (a third-party service that fetches the page for you).

**Pictures** (PNG, JPEG, GIF) load after the page, one at a time, and the
page re-flows around them. **Fonts** are smooth, with headings in
several sizes, and the text keeps its accents (Latin, Greek, Cyrillic).
Scripts and style sheets don't run; pages that mark their content with
`<main>` show just that, and menus and other clutter hidden by CSS stay
hidden.

## Desktop

- **Top bar** (frosted): the lotus menu (About, All Apps, App Store,
  Show Desktop), the name of the app in front, the network status and
  address, and the date and time (UTC).
- **Dock** (floating, frosted): the built-in apps, then installed ones,
  then the **All Apps** grid; the name above each under the pointer, and a
  dot under the running ones.
- **Windows:** the title bar shares the window's color, with the title
  centered and the controls at the top left; soft shadows and a fine light
  edge. Windows can be moved, resized (apps that support it get a
  `WindowEvent::Resize`), minimized to the dock, maximized (to the space
  between the bars) and closed.
- **Palette:** calm warm neutrals with saffron and lotus as accents
  (`Theme` in `kapala/gfx.h`), and a soft wallpaper painted at start.
- **Apps:** Browser, **Files** (folders and files on the disk, sizes and
  dates, new folder or file, delete, open in Notes), **Terminal** (`help`
  lists its commands: files, network, system, opening apps), Notes,
  System Monitor and About.
- Apps can start other apps with an argument (`Window::launch("notes",
  "/a.txt")`), and read theirs with `Window::argument()`.

## Installing apps

Apps that aren't part of the system install onto the disk, in
`/apps/<id>/`, and then appear in the dock and the All Apps grid like the
built-in ones. They come as `.bpk` packages (program, manifest and icon).

- **apt** (in the Terminal), the package manager:

  ```
  apt update                     read the repositories' package lists
  apt search clock               find apps
  apt install clock calculator   install (also a .bpk file path or web address)
  apt remove clock               uninstall
  apt upgrade                    install newer versions
  apt list --installed           (or --upgradable)
  apt show clock
  apt add-repo http://...        add a repository; apt sources lists them
  ```

  Repositories are web folders with a `Packages` index and the `.bpk`
  files, listed in `/etc/apt/sources.txt`; the catalog in the system is
  always one of them. `apt update` keeps their lists in `/var/apt/lists/`.
- **App Store** (in the dock): the same packages as apt, with Install,
  Open, Update and Remove buttons, Refresh (`apt update`), and a field to
  install a package from a web address or a file on the disk.

Installed programs start through `SYS_SPAWN_IMAGE`: KamalShell reads the
program from the disk into shared memory and the kernel loads it from
there (checking it like any program: no writable-and-executable memory,
a memory limit, everything inside user space). Apps get the same handles
as built-in ones (shell, files, network), so they can do what any app can:
install only what you trust.

**Making apps:** see [`apps/README.md`](apps/README.md). `make packages`
builds every app in `apps/` (two examples: Calculator and Clock) into a
package and the built-in catalog; `make repo` makes an apt repository of
them in `build/repo/`, and `make serve-repo` serves it, so Bhairav OS in
QEMU can `apt add-repo http://10.0.2.2:8080` (QEMU's address for the build
machine).

## Text and pictures

**Fonts:** DejaVu Sans, Sans Bold and Sans Mono at several sizes,
rendered with FreeType at build time into `assets/fonts.bin` (about 1,700
characters per face: Latin with accents, Greek, Cyrillic, symbols). The
kernel shares the file read-only with every program (`SYS_FILE_OPEN`), and
`Canvas::text` draws it anti-aliased, as UTF-8. `Font::system()`,
`Font::bold()`, `Font::mono()` and `Font::get("sans-bold", 22)` pick a
face. To change the fonts, edit `scripts/make-fonts.py` and run it with
Pillow (see the script). DejaVu's license is in `assets/DEJAVU-LICENSE`.
Scripts that need shaping (Devanagari, Arabic) aren't supported yet.

**Pictures:** `Kapala::Image` (`kapala/image.h`) decodes PNG (every color
type and depth, interlaced too), baseline JPEG and GIF (the first frame),
with its own inflate (zlib), and scales smoothly. `scripts/image-test.sh`
compares every decoder with ImageMagick, pixel by pixel. Progressive
JPEG, WebP and SVG aren't supported yet.

## Kapalaframe

The library every program links against (`userspace/kapalaframe/`):

| Header | What it gives |
|---|---|
| `kapala/sys.h` | System call wrappers |
| `kapala/channel.h` | `Channel`: typed messages over channels |
| `kapala/heap.h` | The heap behind `new` and `delete` |
| `kapala/gfx.h` | `Bitmap`, `Color`, `Rect` and `Canvas`: clipped, anti-aliased rounded rectangles, circles, gradients, shadows, smooth text (`Font`) and images, plus the Bhairav OS theme colors |
| `kapala/window.h` | `Window`: a window on KamalShell, with events, timeouts, resizing and launching apps |
| `kapala/files.h` | `Files`: read, write, append, list, remove and make directories on the disk |
| `kapala/net.h` | `Net`: DNS, ping and TCP connections |
| `kapala/tls.h`, `crypto.h` | `Tls`: TLS 1.3 connections; the cryptography under it |
| `kapala/image.h` | `Image`: PNG, JPEG and GIF decoding, scaling |
| `kapala/netdriver.h` | The main loop network card drivers share |
| `kapala/apps.h` | Packages, installing and listing apps |
| `kapala/apt.h` | apt: repositories, update, install, upgrade |
| `kapala/http.h` | `httpGet`: download a file over HTTP or HTTPS |
| `kapala/text.h` | `Text`: build strings without the heap |
| `kapala/protocol/input.h` | Keyboard and mouse events |
| `kapala/protocol/shell.h` | The KamalShell window protocol |
| `kapala/protocol/block.h`, `fs.h`, `net.h`, `netdev.h` | The disk driver, file system, network and network card protocols |

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
| `assets/` | The Bhairav logo (converted at build time for the boot screen), the fonts, and the disk's welcome file |
| `boot/shool.cfg` | ShoolLoader🔱 configuration: kernel, programs, screen mode |
| `boot/limine.conf` | Limine configuration, for legacy BIOS boot |
| `apps/` | Third-party example apps and the guide to making apps |
| `tools/` | Build-machine tools (`bpk`: makes packages; `serve`: serves a repository) |
| `legacy/` | Earlier sources, kept for reference, not built |

Programs today: `init` (root of userspace), `ps2` (keyboard and wheel
mouse driver), `ahci` (disk driver), `fs` (file system), `e1000` and
`rtl8139` (network card drivers), `net` (TCP/IP), `kamalshell` (the
desktop), the apps `browser`, `files`, `terminal`, `notes`, `monitor`,
`store` and `about`, and `ipc-test`, `protect-test`, `fpu-test`, `fs-test`, `net-test`
and `tls-test` (checks run at every boot; `oom-test` runs only in
`SECURITY_TEST=1` builds). The boot image also holds
`fonts.bin` and `catalog.bin` (the App Store's packages).

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

9. **Network** ✅: e1000 driver, TCP/IP stack (ARP, IPv4, ICMP, UDP, DHCP,
   DNS, TCP), `Kapala::Net`
10. **Bhairav Browser** ✅: HTTP, HTML layout, links, history, search
11. **Desktop** ✅: icons, resizable and maximizable windows, mouse wheel and
    arrow keys, network status and date in the taskbar, Files and Terminal

12. **Network drivers** ✅: card drivers as their own programs over shared
    rings, a Realtek RTL8139 driver next to the Intel e1000 one
13. **HTTPS** ✅: kernel random numbers, SHA-256/HKDF/X25519/ChaCha20-Poly1305/
    AES-GCM, TLS 1.3 client, HTTPS in the browser and Terminal
14. **Smooth text** ✅: DejaVu fonts shared from the boot image, anti-aliased
    UTF-8 text everywhere
15. **Pictures** ✅: PNG, JPEG and GIF decoders, pictures in web pages

16. **Clean desktop** ✅: top bar, dock, minimize, calmer palette
17. **Third-party apps** ✅: packages, App Store, installing from the disk or
    the web, `SYS_SPAWN_IMAGE`, the app grid, an SDK with two examples
18. **apt** ✅: repositories, `apt update/search/install/remove/upgrade`,
    shared with the App Store; `make repo` / `make serve-repo`

Next: checking HTTPS certificates (so the browser knows a site is real),
TLS 1.2, progressive JPEG and WebP, text shaping for Indic scripts, NVMe
disks and USB input (most new laptops), loading programs from disk, and
multi-core.

## License

GPL-3.0. See `LICENSE`. The boot console font is Terminus (SIL Open Font
License), embedded from the toolchain image. The interface fonts are
DejaVu (Bitstream Vera license, see `assets/DEJAVU-LICENSE`).
