# Bhairav OS security assessment

Status: first audit, 2026-11-12, of commit `38a627f`, plus the fixes in
§6 (memory reserve) and §7 (kernel hardening round).
Scope: everything I read line by line is marked **reviewed**. Anything
else is marked **not reviewed** and makes no claim. Nothing here is a
certification or a formal proof.

Reviewed: the RudraCore kernel (`kernel/src/core/*`, `mem/*`,
`arch/x86_64/{idt,vmm,cpu}`), the ABI (`abi/rudracore/syscalls.h`), how init
hands out handles, how KamalShell launches apps, the format and install path
of apt/BPK packages, the TLS certificate handling, and the bootloader's CPU
setup. Not reviewed: the `fs` server's path handling, the TCP/IP stack's
packet parsers, the browser's HTML/URL parsers, the image decoders, the
crypto primitives beyond their boot-time test vectors, and the ShoolLoader
file loader.

---

## 1. Current architecture: what is actually enforced

| Property | Status | Evidence |
|---|---|---|
| Separate address space per program | **Enforced** | `Vmm::createUserSpace` gives each task its own lower half and shares only the kernel half. |
| User/kernel split | **Enforced** | Kernel pages have no U bit. `protect-test` checks this at every boot (a read faults and a kernel pointer gets `BAD_ADDRESS`). |
| Syscall pointer validation | **Enforced** | `isUserRange` checks the range is below `USER_END`, has no overflow, is mapped, and is writable when written. Every pointer argument goes through it. |
| W^X for user programs | **Enforced** | The ELF loader rejects W+X segments. NX is set on every non-EXEC leaf. EFER.NXE is set (ShoolLoader `handover.cpp`, Limine). |
| W^X for the kernel image | **Enforced** | `.text` is R-X, `.rodata` R--, `.data` RW-. The direct map is RW and NX. |
| W^X for every alias | **Enforced (§7)** | The direct-map alias of the kernel's code and constants is now read-only too. |
| NX / CR0.WP | **Enforced (§7)** | The kernel sets EFER.NXE and CR0.WP itself, and refuses to boot on a CPU without NX. |
| SMEP / SMAP | **Enforced when the CPU has them (§7)** | SMAP allows user memory only inside two copy helpers. AC is cleared on every kernel entry. |
| Kernel stack guard pages | **Enforced (§7)** | Each kernel stack sits in its own slot with an unmapped page below. The boot task's static stack is the one exception. |
| User stack guard | **Enforced (§7)** | Programs must load below 4 GiB. The stack sits below 128 TiB with nothing mapped under it but shared memory far below. |
| Handles | **Capabilities (§8)** | Each handle has rights (read, write, transfer, duplicate, revoke, MMIO, DMA) and a generation counter, and can be revoked. Rights only shrink. |
| Who reaches hardware | **Enforced by delegation and rights (§8)** | Only init gets IRQ, I/O-port and PCI handles. It strips them to what each driver uses before moving them, so drivers can't pass devices on. |
| DMA isolation | **Enforced with VT-d (§10, §12)** | Each device reaches only the buffers its driver registered, and can't send interrupt messages (interrupt remapping). Without VT-d hardware, unconfined (logged). |
| Kernel memory accounting | **Added in §6 and §7** | A 4 MiB kernel reserve, plus a per-program quota of half of RAM. |
| Boot integrity | **Signed on UEFI, hashed on BIOS (§11)** | ShoolLoader boots only files in an Ed25519-signed list and refuses older boot images. Limine checks a BLAKE2B hash per file and an enrolled config hash. |
| TLS server authentication | **Enforced (§10)** | Chain to a trusted root, hostname, dates, and the CertificateVerify signature. Fails closed. |
| Package integrity | **Enforced (§10, §11)** | Ed25519-signed packages and repository indexes, trusted keys in the boot image, no rollback. Checked again at every launch. |
| App isolation from data | **Enforced (§9)** | Installed apps see only their own folder and no network unless the user grants more. Built-ins get fixed profiles. |
| RNG | **Reasonable** | ChaCha20 DRBG keyed from RDRAND (when present), TSC jitter and IRQ timing. It rekeys before and after each output. There is no reseed-health check, and TSC-only seeding (no RDRAND) is weak early in boot. |

Concurrency model: a single CPU. The kernel runs with interrupts disabled for
every syscall and IRQ. This rules out syscall TOCTOU races on user memory
today, including an ELF image in shared memory that a peer could rewrite.
**SMP would invalidate this. Every user-memory read would then need
copy-in semantics.**

## 2. Threat model

**Assets:** kernel integrity; the user's files on the FAT32 disk; network
traffic and credentials; installed apps; the boot chain; the audit log
(serial).

**Threat actors:**
1. A malicious or compromised **third-party app**: installed through apt or
   BPK, runs as an ordinary task.
2. A **network attacker** on the path: café Wi-Fi, a hostile router, DNS
   spoofing.
3. **Malicious input** to a trusted program: web pages, images, packets,
   disk images, ELF files.
4. A **compromised driver**: e1000 or rtl8139 parsing hostile frames.
5. **Physical / boot-media attacker:** someone who can swap the ISO or the
   EFI partition.

**Trust boundaries:** user↔kernel (`int 0x80`) · app↔fs/net servers (IPC) ·
driver↔hardware (DMA) · network↔net/TLS · repository↔apt ·
firmware↔ShoolLoader↔kernel.

| # | Scenario | Path | Impact | Today |
|---|---|---|---|---|
| T1 | App exhausts RAM, kernel panics | `memory_alloc` loop, then any kernel `new` | Whole-system DoS | **Fixed (§6)** |
| T2 | MITM serves a malicious package | `http://` repo, or `https://` with no cert check, and no signature | Code execution as an app, with full disk and network access | **Open, critical** |
| T3 | Hostile app reads or alters every file, including other apps' `app.elf` | full `fs` channel | Data theft, persistence, trojaning other apps | **Open, critical** |
| T4 | Compromised NIC driver DMAs over kernel memory | `SYS_DMA_ADDRESS` + bus master, no IOMMU | Kernel compromise | **Open, high (needs VT-d)** |
| T5 | MITM on HTTPS | no X.509 validation | Credential theft, content injection | **Open, high** |
| T6 | Boot media tampering | no verified boot | Persistent kernel compromise | **Mitigated (§11, §13)**: signed boot files; the loader is signed for UEFI Secure Boot |
| T7 | Memory corruption in a parser (HTML, PNG/JPEG, TCP, FAT) | not reviewed | Compromise of that one task (contained by isolation) | **Unknown: fuzz** |
| T8 | Kernel exploit made easier | no SMEP/SMAP, no stack guards | Escalation from a kernel bug | **Mitigation gap** |

## 3. Security gap analysis

### Confirmed vulnerabilities (reproduced or traced through the code)

| ID | Sev | Finding | Status |
|---|---|---|---|
| V-01 | High | **An unprivileged program can panic the kernel.** `SYS_MEMORY_ALLOC` (256 MiB per call, no limit on calls), `SYS_MEMORY_CREATE`, spawn and channel creation take physical pages with no reserve. Once RAM is gone, the next kernel `new` (for example `new Endpoint` in `channel_create`) calls `panic("heap: out of memory")`. **Reproduced:** serial log `oom check: took 246 MiB … !!! RudraCore panic: heap: out of memory`. | **Fixed, §6** |
| V-02 | Critical | **Packages have no integrity.** BPK (`tools/bpk.cpp`) has no hash or signature, apt accepts `http://` sources, and `https://` doesn't authenticate the server. Anyone on the path can replace an app. | **Fixed, §10** |
| V-03 | Critical | **Apps get the whole disk and network.** `KamalShell::launch` gives every app, built-in or third-party, an unrestricted `fs` and `net` channel. One bad app can overwrite `/apps/<other>/app.elf`. | **Fixed, §9** |
| V-04 | High | **TLS doesn't check the server certificate.** `tls.cpp` parses the Certificate message and drops it. There is no X.509 parsing, chain, hostname or expiry check, and no trust store. | **Fixed, §10** |
| V-05 | Medium | **Endpoint reference cycles leak kernel memory for good.** Send endpoint `y` over its peer `x` and `y` lands in its own queue holding a reference to itself. Close `x` and `y` can never be freed. Longer cycles work too. | **Fixed, §7** |
| V-06 | Medium | **Dead tasks' memory wasn't freed while nothing else was runnable.** Zombies were reaped only at a task switch away from a ready task. | **Fixed, §6** (reaped on each timer tick) |
| V-07 | Low | **Log spoofing.** `SYS_LOG` writes user bytes, including newlines, into the kernel log after a `[name]` prefix. Spawned app names are user-chosen (`SpawnImage.name`), so an app can call itself `init` in the log. | **Fixed, §7** |
| V-08 | Low | **An ELF segment can overlap the shared-memory window (≥ 4 GiB).** A later `memory_map` or `memory_alloc` then overwrites an OWNED PTE and leaks that page. It only hurts the task itself. | **Fixed, §7** |

### Architectural weaknesses / insecure assumptions

- ~~No handle rights~~ and ~~no handle generations~~: done in §8.
- **Any task may `spawn` any boot program.** It's harmless today, because
  drivers need handles only init holds, but it widens the attack surface.
- **A driver is trusted with all of RAM (DMA).** The comment in
  `device.h` says so. VT-d is the only real fix. In software, the most we can
  do is limit `SYS_DMA_ADDRESS` to memory the driver itself created.
- **FAT32 can't hold ownership or permissions.** Isolation has to be enforced
  by the `fs` server: per-app roots, or capabilities to subtrees.
- **No SMEP/SMAP or guard pages.** They don't fix any bug, but they make
  kernel bugs much harder to exploit.
- **Single-CPU assumption.** See §1.

## 4. Cybersecurity platform gap analysis

| Tool | Buildable now on existing infrastructure? | Needs |
|---|---|---|
| Forensics (hashing, file signatures, hex view, entropy) | **Yes.** `crypto.h` has SHA-256 and `fs` reads files. | MD5/SHA-1/SHA-512 for compatibility; a read-only evidence mode in `fs` |
| BinaryLab: ELF inspector, strings, entropy | **Yes**, purely userspace parsing | x86-64 disassembler (large); debugging needs new kernel calls (ptrace-like, register/memory access by capability) |
| Malware static analysis | **Yes** (static part) | Rule engine; isolated execution needs V-03 fixed first (a sandbox without fs/net handles is already possible: spawn with no handles) |
| Recon: TCP connect scan, DNS, reverse DNS | **Partly.** The `net` protocol does TCP connect and DNS. | Scope/rate-limit policy in `net`; UDP and raw ICMP from apps; audit log |
| PacketLab live capture | **No** | A capture tap in the NIC driver: a shared-memory ring to a capture service. Promiscuous mode is **not enabled** (`e1000.cpp` RCTL sets no UPE/MPE). IPv6 isn't in the stack. |
| WebProbe proxy | **Partly** (HTTP client exists) | Listening sockets in `net` (unverified), X.509 parsing (shared with V-04) |
| VulnScan | Depends on Recon plus a vulnerability data source | Data source and update channel (needs V-02 fixed) |
| NetGuard firewall / IDS | **No** | A policy hook in `net` (it's the only path to the NIC, which is a good single choke point) |
| Hardware research (USB, UART, SPI/I2C) | **No** | No USB host controller (xHCI) driver and no serial driver for users |

**Ordering consequence:** V-02 and V-03 must come before any "security
workstation" features. Otherwise the security tools themselves can be
replaced or read by any app.

## 5. Prioritised roadmap

| P | Item | Components | Change | Depends on | Size | Validation |
|---|---|---|---|---|---|---|
| 0 | ✅ Kernel memory reserve | pmm, syscall, elf, shared_memory, sched | §6 | – | S | `SECURITY_TEST=1` oom-test |
| 1 | ✅ Per-app storage isolation | kamalshell, fs, net, store | §9 | – | M | fs-test, net-test |
| 2 | Package signatures | tools/bpk, apt, apps.cpp | Ed25519 signature over the BPK. Public key in the boot image. Refuse unsigned packages. Repo index signed (anti-rollback: version must not decrease) | Ed25519 implementation + RFC 8032 test vectors | M | Tampered or unsigned `.bpk` refused; vectors in tls-test |
| 3 | X.509 + chain + hostname + expiry | kapala/tls, new x509.cpp | DER parser, RSA-PKCS1/PSS and ECDSA-P256 verify, trust store file in the boot image, fail closed | Bignum/P-256 code | L | Bad-cert test servers (expired, wrong host, self-signed) must fail |
| 4 | ✅ Endpoint cycle prevention | channel, syscall | §7 | – | S | harden-test |
| 5 | ✅ SMEP and SMAP | arch/protect, syscall copy paths | §7 | – | S / M | `FAULT_TEST=smep\|smap` |
| 6 | ✅ Handle rights, generations, revocation | object, task, syscalls | §8 | – | M | harden-test |
| 7 | ✅ Kernel stack guard pages | sched | §7 | – | M | `FAULT_TEST=stack` |
| 8 | ✅ Verified boot | shoolloader, Makefile | §11 | P2's signature code | M | Altered module refused |
| 9 | Capture service + firewall hook | e1000/rtl8139, net | Shared-memory ring, policy in `net` | P1 | L | Fuzzed frames; policy tests |
| 10 | VT-d | new kernel ACPI DMAR + IOMMU code | Per-device page tables | ACPI parsing | XL | QEMU `-device intel-iommu`: DMA outside a mapping faults |
| – | Fuzzing harness | hosted builds of ELF loader, BPK, PNG/JPEG, HTML, TCP parsers | libFuzzer + ASan on Linux host | – | M | Corpus runs clean |

## 6. Change made: kernel memory reserve (V-01, V-06)

**Problem.** Programs could take every physical page. The kernel's own
allocations (`operator new` panics on failure) then took the whole system
down. Any program could do it, third-party apps included.

**Fix.**
- `Pmm::userCanUse(pages)` keeps 4 MiB (1024 pages) that only the kernel's
  own allocations may use.
- It is checked before everything a system call allocates:
  `SharedMemory::allocate` (`memory_create`, boot files), `memory_alloc`
  (including page tables), `channel_create` (two heap endpoints), and
  `Elf::load` (segments, page tables, stacks, task state; this covers
  `spawn` and `spawn_image`). These now return `OUT_OF_MEMORY` (spawn used to
  say `INVALID_ARGUMENT` for every load failure).
- Dead tasks are also reaped on each timer tick, so their memory comes back
  even when nothing else is runnable (V-06).

**Test.** `userspace/programs/oom-test`, run first by init in
`make shot SECURITY_TEST=1` builds. It takes all memory, then checks that
`channel_create`, `memory_create` and `spawn` all return `OUT_OF_MEMORY`.
init waits for it to exit before starting anything else.

**Results (QEMU q35, 256 MiB):**
- Before the fix: `took 246 MiB` → `RudraCore panic: heap: out of memory`.
- After the fix: all three refused, `oom check: passed`. Then every boot
  check passes (ipc, protect, fpu, fs, net, tls) and KamalShell starts.
- A normal build boots to the desktop under both BIOS (Limine) and UEFI
  (ShoolLoader).

**Limits.** This is a global reserve, not per-app quotas. One app can still
starve other apps of memory (they get `OUT_OF_MEMORY`), but no longer the
kernel. Per-task accounting is the follow-up. `memory_map` page tables and
`device_bar` objects aren't counted: they're bounded (32 mappings, driver
only) and the reserve absorbs them.

## 7. Kernel hardening round

Each item below was built, then booted in QEMU (q35, 256 MiB). The test
setups were: the default CPU model (no SMEP/SMAP), `-cpu max` (both), BIOS,
and UEFI.

| Item | What changed | How it's checked |
|---|---|---|
| SMEP | Set in CR4 when CPUID leaf 7 reports it (`arch/x86_64/protect.cpp`). | `FAULT_TEST=smep`: the kernel calls a user page and gets `page fault … protection violation, instruction fetch`. |
| SMAP | Set when supported. The kernel touches user memory only in `fromUser`/`toUser` (`core/syscall.cpp`), which open it with STAC/CLAC. `interruptDispatch` runs CLAC first, because ring 3 can set RFLAGS.AC and an interrupt doesn't clear it. | `FAULT_TEST=smap`: a plain kernel read of a user page faults. Every boot check still passes with SMAP on, and the browser opens and renders. |
| Kernel stack guards | Kernel stacks are mapped at `0xffffff0000000000` in 20 KiB slots, with an unmapped page below each 16 KiB stack. Overflow → #PF can't be pushed → #DF on its IST stack → panic that names the guard address. | `FAULT_TEST=stack`: `double fault … kernel stack overflow into its guard page, at 0xffffff000000afd8`. |
| User stack guard | ELF segments must end below 4 GiB (`Elf::USER_IMAGE_END`, static_assert ≤ `Task::MAP_BASE`). The stack below 128 TiB has no mapping beneath it. This also fixes V-08, since images can no longer overlap the shared-memory window. | harden-test: a segment at 5 GiB is refused. |
| NX and W^X | The kernel sets EFER.NXE (and panics without NX) and CR0.WP itself. The direct-map alias of `.text`/`.rodata` is read-only. | Every boot works with the stricter mappings. |
| Memory quotas | `Core::Quota`: half of RAM per program, charged by `memory_alloc` and `memory_create`. Shared memory refunds its creator when freed, even after that program has ended (the quota is reference counted). | oom-test: one program stops at 124 of 248 MiB. With a helper taking the rest, the kernel reserve still refuses channel, memory and spawn requests. |
| Syscall validation | Log text and spawned app names have control characters replaced (V-07). `sleep` and `wait` timeouts are capped so deadlines can't wrap. Channel sends refuse anything that could start a reference cycle (V-05). | harden-test: both cycle shapes are refused. A forged newline in the log stays on one line. |
| ELF validation | All headers are checked before anything is allocated: EI_VERSION, at most 64 program headers, no PT_INTERP/PT_DYNAMIC, segment address and file offset congruent mod 4 KiB, no shared page with different rights, entry point in an executable segment. | harden-test: 6 crafted bad files refused, and a good one still runs. |
| Object lifetime | Audit: references are held by handles, mappings, queued messages and bound IRQs. Cycles were the only leak (fixed above). `Object::ref()` now traps rather than wrap a u32 count. Waiters are cleared on wake, on wait exit and in `releaseResources`. | Code review, plus the cycle tests. |
| Info leaks | `system_info` zeroes padding (4 bytes after `reserved` used to carry stale kernel stack). `random` output is built in a kernel buffer that is wiped afterwards. Every other output struct and new page was already zeroed. | harden-test: the padding is checked for zero. |

**Not covered.** ELF segments still don't count toward the quota (bounded
by the loader's 256 MiB per program and the kernel reserve). The boot
task's static stack has no guard page. SMEP and SMAP are off on CPUs
without them, which includes QEMU's default model. Handle rights and
generation counters were added afterwards (§8).

## 8. Capabilities

Handles are now capabilities. Contract: `abi/rudracore/syscalls.h`.
Kernel: `core/object.h` (`Capability`, object epoch, quota charge),
`core/task.cpp` (handle table), `core/syscall.cpp` (checks and the new calls).

| Item | Design | Checked by harden-test |
|---|---|---|
| Generation counters | Handle = `(generation << 8) \| slot`. The slot's generation is bumped when the slot is freed, so a stale number never matches. First handles are still 1, 2, 3…, so existing programs didn't change. | A reused slot gets a new number. The old number gives `BAD_HANDLE`. |
| Rights | `READ`, `WRITE`, `TRANSFER`, `DUPLICATE`, `REVOKE`, `MMIO`, `DMA`, checked on every call (`ACCESS_DENIED`). New objects get all applicable rights. Boot files never get `WRITE` or `REVOKE` (one object shared by every program). Device registers aren't transferable. | Restricting never adds rights, and duplicating needs `DUPLICATE`. |
| Read-only and send-only | `handle_duplicate(h, rights)` and `handle_restrict(h, rights)` (intersection only). Memory without `WRITE` maps read-only. A channel end with only `WRITE` sends but can't receive, and vice versa. | Send-only can't receive, receive-only can't send. The kernel won't write into a read-only mapping (`random` → `BAD_ADDRESS`). |
| Revocation | Each object has an epoch and each handle records it. `handle_revoke` bumps the epoch, so every other handle (any program, in flight too) gets `REVOKED`. Memory is unmapped from every other program, and waiters on channels and IRQs are woken. | The revoker keeps access, others get `REVOKED`. A handle revoked in flight arrives as nothing. **A child program reading revoked memory faults at once.** |
| Secure transfer | A moved handle keeps exactly its rights. Channel sends and `spawn_image` (untrusted apps) need `TRANSFER`. `spawn` of boot-image programs doesn't, which is how init gives drivers devices they can't pass on. Revoked handles can't move. Cycle rules from §7 still apply. | Moving without `TRANSFER` gets `ACCESS_DENIED`, and a revoked handle can't be moved. |
| Object quotas | 1,024 channels and memory objects per program, charged to the creator, refunded when the object is destroyed (`Object::~Object`). | Parking channel ends in queues hits `OUT_OF_MEMORY`. Closing them gives the quota back. |
| Device permissions | init restricts its hardware handles before spawning drivers: disk and network cards `READ\|MMIO\|DMA`, IRQs `READ`, PS/2 ports `READ\|WRITE`, screen `READ\|WRITE`. `dma_address` needs `DMA` on the device and `WRITE` on the memory. | Every driver works with only these rights (AHCI, e1000, RTL8139, PS/2), in QEMU under BIOS and UEFI. |

**Lifetime fix found while adding revocation.** If a handle was revoked
while its task was blocked in `wait` on several channels, the other
channels could keep naming the task as their waiter. That pointer would
dangle after the task exited. `wait` now clears the task's stale
registrations on that path, and `handle_close` clears the closing task as
waiter.

**Limits.**
- Revocation is all-or-nothing per object: it doesn't track which handle
  was derived from which.
- Revoking a channel end doesn't touch its peer.
- A driver could still `spawn` a boot-image program with its device; boot
  programs are trusted system code.
- DMA itself is still unconfined without an IOMMU (threat T4 in §2).

## 9. App sandbox

| Item | Design | Checked by |
|---|---|---|
| Unforgeable identity | Identity and scope travel in the `Connect` message on the fs/net **connect** channel, which only init and KamalShell hold. An app gets only its session channel, and nothing on it changes the scope. KamalShell takes the identity from what it launches: the built-in's name, or the installed folder's id. Installs can't take a built-in's name (`isBuiltInProgram`). The Connect magic changed, so an old-format Connect is refused. | Code review. Every other check below depends on it. |
| Per-app roots, private storage | fs scopes: **System** (whole disk), **User**, **Private** (`/appdata/<app>` shown as `/`, made on first connect). Each path is first rewritten with no `.`/`..` parts and refused if `..` would climb above `/`. This matters because the resolver follows `..` entries on the disk, so a plain prefix could be escaped. | fs-test: private write lands in `/appdata/fs-test`; `/../../…` refused; `/sub/../x` works; `/apps` invisible. |
| Protected system directories | `/apps`, `/var`, `/appdata`. User scope: the first two read-only, `/appdata` hidden (NOT_FOUND, left out of `/` listings). Only System scope writes them. The check is on the normalised first component with the resolver's own case-insensitive compare, so case and `..` tricks name the same folder. | fs-test: writes, mkdir and remove refused, including `/VAR/…` and `/tmp/../var/…`. Files (User) doesn't list `appdata`. |
| Network policies | net levels: **None** (Status only), **Local** (DNS; ping/TCP only to 10/8, 172.16/12, 192.168/16, 127/8, 169.254/16), **Internet**. Checked before each request. | net-test: None can't resolve or ping. Local pings the gateway but can't open TCP to example.com. |
| Explicit hardware permissions | Kernel privilege levels: Root (init) → System (what init starts) → App (anything else, including every installed app). Apps can't call `spawn`/`spawn_image`. The kernel refuses to give an app hardware (IRQ, ports, PCI device, device memory) by spawn or by message. Drivers hold devices with only the rights from §8. | harden-test: an app's `spawn` → ACCESS_DENIED. The hardware rule is code review (no app holds hardware to try with). |
| Process resource limits | Apps: a quarter of RAM and 256 kernel objects. System services: half and 1,024. Also 63 handles per program, 64 KiB stack, 256 MiB image. | oom-test: system service 124/248 MiB, app helpers 62 and 55 MiB, then the reserve refuses channel, memory and spawn. |
| Permission review | Manifest `permissions = network local-network files`. Install grants what's asked; an update keeps the old grant, intersected with the new request. Grants live in `/var/permissions/<id>`. The App Store **Permissions** panel shows the request and cycles Network (Off / Local / Internet) and Your files (Off / On). Hardware shows "Never", own folder "Always". Uninstall deletes the grants and `/appdata/<id>`. | In QEMU: the panel opened for Clock, toggles saved `network` and `files` to the disk, and Clock started with `/appdata/clock`. |

**Built-in profiles** (`kamalshell/main.cpp` `APPS`):

| App | Files | Network |
|---|---|---|
| Browser | Private | Internet |
| Files | User | None |
| Notes | User | None |
| Terminal | System | Internet |
| App Store | System | Internet |
| Monitor | Private | None |
| About | Private | None |

Terminal and the App Store need System scope because they install apps,
which means writing `/apps` and `/var`.

**Limits.**
- Apps installed before this change have no grants file, so they start
  with their own folder only until the user grants more.
- Grants are per app, not per folder: `files` means all of the user's files.
- Network policy works by address. An app with Local access can still look
  up names, and a local proxy would relay its traffic further.
- The shell and the servers are trusted to apply all this. A compromised
  KamalShell could connect an app with any scope.
- There's no CPU-time limit yet.

## 10. Closing the open items

| Item | Design | Checked by |
|---|---|---|
| CPU time | Two ready queues: system tasks (kernel, init and what it starts) always run before apps, and a ready system task preempts an app at the next 1 ms tick. Apps share what's left round-robin. Each task's CPU time is counted (`Task::cpuMs`). | harden-test: with an app spinning for 1.5 s, a system task's worst wake-up delay was 0 ms. With preemption switched off as a check, it was 5 ms and the test failed. |
| Per-folder file grants | Permission `folder`: a Private app also gets one user folder, which it sees as `/shared`. The folder is chosen in the App Store and checked by fs (never `/`, never a protected directory; `..` normalised away). | fs-test: writes land in the folder; `..` can't leave it; it can't be removed; a `/var` grant is refused at connect. |
| Host allowlists (the proxy route) | Access level `Hosts`: DNS only for the names in the manifest's `hosts` line (`*.` covers subdomains), and ping/TCP only to addresses those lookups returned. A local proxy is just another unlisted address. | net-test: a hosts-only client can't ping the gateway or resolve another name, but can reach example.com. |
| Signed packages | Ed25519 (RFC 8032, after TweetNaCl) over the whole package; the signature is the last file. Build: `bpk tbs` → `openssl pkeyutl -sign -rawin` → `bpk attach`, key in `keys/` (gitignored). Trusted keys: `trust.bin` in the boot image. `installPackage` refuses unsigned, altered and older versions. | tls-test: RFC 8032 vectors (and tampered ones), SHA-512/384 vectors, every catalog package verifies, one changed byte fails. OpenSSL signs and Bhairav verifies, so the two implementations cross-check. |
| HTTPS certificates | `x509.cpp` (DER, chain to `roots.bin` from the build machine's CA bundle, dates, CA flag, SAN hostnames, unknown critical extensions refused) plus `pkey.cpp` (Montgomery arithmetic; RSA PKCS #1 and PSS; ECDSA P-256/P-384). TLS verifies the chain and the CertificateVerify signature over the transcript, and fails closed. | tls-test: example.com and wikipedia.org verify. A wrong host name, an expired chain, no roots and a changed certificate are all refused. Also tried: debian.org, microsoft.com, github.com and kernel.org all verify, exercising ECDSA SHA-256/384, RSA PKCS #1 SHA-256/384 and RSA-PSS. |
| IOMMU | Intel VT-d (`arch/x86_64/iommu.cpp`). The ACPI DMAR table names the units. One root table, a context entry per device, a second-level page table per device. Every device starts blocked. `dma_address` maps exactly that buffer for that device (same address); freeing the buffer unmaps it and flushes the IOTLB before the pages are reused. Blocked DMA is logged. | `IOMMU=1`: every boot check passes through the mappings. `IOMMU=1 FAULT_TEST=dma`: the AHCI and e1000 DMA are blocked and logged, and the disk stops. Without VT-d the log says DMA is unconfined. |

**Limits.**
- CPU priority is two levels, not per-app shares or caps.
- The root store is the build machine's bundle. There's no revocation
  checking (OCSP/CRL), and no IP-address names.
- A lost signing key means packages built with a new key won't update
  installed ones signed by the old key, unless both keys are in
  `trust.bin`.
- VT-d needs the hardware. AMD IOMMU is not supported. A compromised driver
  can still misuse the buffers it registered. Interrupt remapping: see §12.

## 11. Supply chain: boot, repositories, installs

| Item | Design | Checked by |
|---|---|---|
| Signed boot modules, kernel verification | The build writes `shool.sum` (`version N` plus the SHA-256 of the kernel, every module and `shool.cfg`) and signs it with `keys/boot-signing.pem` as `shool.sig`. ShoolLoader has the public key built in (`build/boot-key.h`). It refuses to boot when the signature is wrong or missing, and when any file it loads is unlisted or changed. `shool.cfg` is listed, so it can't point `kernel =` at another signed file. | UEFI tests: an untouched image boots. A changed `init.elf`, a re-hashed but unsigned `shool.sum`, a `shool.cfg` with `kernel = /boot/init.elf`, and a missing `shool.sig` are each refused, with the reason on screen. |
| BIOS path | `build/limine.conf` gets a BLAKE2B hash after every path, and `limine enroll-config` writes the config's hash into the ISO's `limine-bios.sys`. | A changed `init.elf`: Limine panics with "Blake2b hash … does not match". An edited `limine.conf`: "CHECKSUM MISMATCH FOR CONFIG FILE". |
| Boot anti-rollback | `BOOT_VERSION` (Makefile) is the image's security version. ShoolLoader keeps the highest that has booted in the UEFI variable `BhairavBootVersion`, set non-volatile and boot-services only, so the running OS can't change it. A variable with other attributes or another size isn't ours, so it's replaced. An older image is refused. | OVMF with persistent variables: version 2 boots, then version 1 is refused ("rollback refused"). |
| Signed repository metadata | `make repo` writes `Packages` starting with `serial = <time>`, adds `sha256 =` per package, and signs it as `Packages.sig` (package key). `apt update` refuses an index that's unsigned or wrongly signed, has no serial, or has a serial lower than the last one read from that repository (`/var/apt/serials/`). `apt install` from a repository refuses a package whose SHA-256 or version differs from the signed index. | A served repo: the signed index is accepted. The same index with one line changed is refused ("isn't signed by a trusted publisher"). A properly signed index with an older serial is refused ("older than one already read"). |
| Package anti-rollback | §10 (an update never installs an older version), plus the index's version and hash binding above, so a mirror can't swap in an older signed build. | §10 |
| Trusted key management | Separate roles: the boot key only signs boot images and lives in the loader. The package key signs packages and indexes and lives in `trust.bin`, which is itself a signed boot file. `keys/trusted/*.pem` adds more public keys (rotation: keep the old key's public half there; revocation: delete it, rebuild). Up to 15 extra keys. New keys are made with `umask 077`. | Build: `trust.bin` holds the key count and keys. Boot: `trust.bin` is checked like every other boot file. |
| Secure installation | `installPackage` keeps the signed package as `/apps/<id>/package.bpk` (written first) plus its manifest and icon, and deletes any old `app.elf`. KamalShell never runs a bare program file. At every launch it reads the package and checks the signature, that it's this app's (not another app's signed package, which would run with this app's grants), and that it matches the installed manifest. Only then does it run the `app.elf` inside. A failed check logs why and doesn't start the app. | Code review. The same check (`loadInstalled`) is what `apt verify` runs, tested below. |
| Recovery verification | `apt verify` re-checks every installed app and names each failure. `apt install <id>` repairs a failed app from its signed source even at the same version. | On a copy of the user's disk: Clock, installed before this change, shows `FAILED … its package is missing`. After `apt install clock`, `apt verify` shows `ok clock 1.0`. |

**Limits.**
- The root of trust is `BOOTX64.EFI` on the medium. Anyone who can rewrite the whole medium can replace the loader, and the BIOS path's `limine-bios.sys`. Closing that needs UEFI Secure Boot with the loader signed by a key in the firmware's `db` (not set up). The BIOS path has hashes but no signature.
- Boot rollback state is a UEFI variable. A physical attacker with a UEFI shell, or someone resetting NVRAM, can clear it. QEMU's `-bios OVMF.fd` (as `make run-uefi` uses) doesn't persist variables at all.
- Index serials stop replays of older indexes, but not a freeze: an attacker can keep serving the latest signed index forever, hiding new updates. That would need an expiry date in the index.
- Installed apps are checked against `trust.bin`, not against what was installed, so swapping an app's package for an older signed version of the same app (offline) isn't caught. Removing a repository forgets its serial.
- Apps installed before this change don't start until reinstalled (`apt verify` lists them).
- A lost boot key means new images need a new loader. A lost package key is handled as in §10, with `keys/trusted/`.

## 12. VT-d interrupt remapping

**Investigation.** DMA remapping (§10) gives each device its own domain: a page table holding only the buffers its driver registered with `dma_address`, unmapped and flushed before the memory is reused. But a device write to `0xFEExxxxx` isn't DMA. It's an interrupt message (MSI), and DMA remapping doesn't translate it. A compromised driver can point its device's MSI anywhere and raise any vector, an NMI or an SMI. Every interrupt Bhairav OS uses arrives through the legacy 8259 PIC as a wire: the kernel programs no LAPIC, IOAPIC, MSI or MSI-X. So no device has a legitimate reason to send an interrupt message.

| Item | Design | Checked by |
|---|---|---|
| Interrupt remapping | When every DRHD unit reports ECAP.IR, `Iommu::init` installs a 256-entry interrupt remapping table (xAPIC format) with every entry not present, then sets SIRTP and IRE. GCMD.CFI stays clear, so compatibility-format messages are blocked too. GSTS is read back: the log says "on" only if IRES is set and CFIS is clear. Blocked messages appear in the fault log with the device and reason. Without IR support the log says devices can still send interrupt messages. | `IOMMU=1`: every boot check passes with remapping on. `IOMMU=1 FAULT_TEST=msi`: the kernel aims the SATA controller's MSI at CPU 0 as an NMI, and the AHCI driver turns its interrupts on. Result: `blocked an interrupt message by device 0:31.2 (reason 0x22)`, and the system keeps running. With `IOMMU=dma-only`, the same NMI arrives: `ahci (task 10) crashed: non-maskable interrupt`. |

**Limits.**
- QEMU 8.2 doesn't model the compatibility-format block. It reports CFIS clear but passes such messages anyway, so the test uses the remappable format, which reaches our table. Real VT-d hardware blocks both formats. This hasn't been tried on real hardware.
- The interrupt entry cache isn't invalidated (that needs queued invalidation). That's safe while remapping starts off and the table never changes. A driver using MSI would need table entries with source-id checking (SVT/SID) plus queued invalidation.
- An NMI that does arrive (without interrupt remapping) is treated as a fault of whatever task is running, which kills that task.

## 13. ShoolLoader hardening and Secure Boot

| Item | Design | Checked by |
|---|---|---|
| Robust configuration parser | `shool.cfg` is checked completely. It must be 16 KiB at most, plain text (no control characters), and `key = value` lines only. It needs exactly one `kernel`, at most 32 distinct `module`s, and at most one each of `resolution` (320x200 to 16384x16384) and `diagnostic` (yes/no). Paths are absolute, printable and under 96 characters, with no `.` or `..` parts. Any error stops the boot with the line number, instead of being skipped or truncated. | UEFI, each re-signed so it reaches the parser: unknown key, second kernel, bad resolution, overlong path, control character, `..` path and a repeated module are each refused with "shool.cfg line N: …". |
| Memory map validation | The kernel (`Boot::collect`) doesn't trust either loader. It drops empty, wrapping and beyond-52-bit regions and page-aligns usable ones inward. Usable memory overlapping anything else is cut out (split in two when needed). Modules must lie in kernel memory, the direct map must be in the higher half, the kernel where it was linked, and the framebuffer sane (otherwise serial only). A built-in test runs every boot on a deliberately broken map (overlaps, unaligned, empty, wrapping, beyond any address). **Found and fixed:** Limine places modules in bootloader-reclaimable memory, which the kernel frees early, yet boot programs are read from there later. Their pages are now marked kernel memory. | Every boot: "memory map validation ok". On the real QEMU maps: 0 repairs. BIOS boots keep module pages out of reclaimable memory. |
| ELF validation | The loader checks the kernel: 64-bit little-endian x86_64 `ET_EXEC`, ELF version 1, 1 to 64 program headers inside the file, segments inside the file, page-congruent and not wrapping, all in the top 2 GiB with no two sharing a page, entry point in an executable segment, at most 64 MiB. Every `.elf` module gets the same header and segment checks. The kernel still checks programs fully when it starts them (§7). | A kernel with another machine type and a big-endian `notes.elf` (both re-signed) are refused, naming the file. |
| Boot protocol versioning | Shool protocol v2 (`BootInfo.flags`). The kernel embeds a `Shool::KernelHeader` (magic, inverted magic, min and max version). ShoolLoader finds it by scanning the kernel file and refuses, on screen and before handing over, a kernel that doesn't take its version. The kernel also checks `BootInfo.version` and `.size`. | A kernel patched to take only v1 (re-signed): "this loader speaks Shool protocol v2; the kernel takes v1 to v1". |
| Kernel handover validation | Before ExitBootServices, the loader requires NX and walks the new page tables. They must map every kernel page, the entry point, the stack, BootInfo (both ends), every module, the framebuffer, and the loader's own hand-over code (identity) where the protocol says. The kernel then validates BootInfo as above. | Every boot passes the walk. Diagnostic boots print "hand-over checked". |
| Signed module verification | §11. A module named in `shool.cfg` but missing now stops the boot instead of being skipped. | A missing `notes.elf` is refused. |
| Secure Boot integration | The build signs `BOOTX64.EFI` (Authenticode, `sbsign`) with `keys/secureboot.key`, made the first time with a self-signed certificate. `build/secureboot.cer` is what to enroll in a firmware's db. `make secure-boot-vars` makes OVMF variables with only that certificate enrolled and Secure Boot on, and `make run-secureboot` boots with them. The loader reads `SecureBoot`/`SetupMode`, shows the state and passes `FLAG_SECURE_BOOT`. The kernel logs it. | OVMF Secure Boot build with our key: the signed loader boots, and both the loader and the kernel report Secure Boot on. An unsigned loader and one signed by another key are refused by the firmware ("Access Denied"). |
| Failure recovery | Every loader error is shown, then after a key or 30 seconds the loader calls `Exit` back to the firmware, which tries its next boot option instead of halting. All errors happen before ExitBootServices. | Missing module, then Enter: the firmware reports "Load Error" for our entry and goes on to PXE. |
| Diagnostic boot mode | Hold D while booting, or set `diagnostic = yes`. The loader shows the checks it made for each file, the screen mode and the hand-over check, then waits for a key (60 s at most). It skips the splash and sets `FLAG_DIAGNOSTIC`. The kernel then logs the validated memory map and every module. | With `diagnostic = yes`: the loader's lines, the wait, then "[diag]" lines with the whole memory map. |

**Limits.**
- The Secure Boot key is a local self-signed one. Real machines trust Microsoft's keys by default, so booting there with Secure Boot on means enrolling `secureboot.cer` in the firmware (or a shim, not done). `make secure-boot-vars` also puts Microsoft's KEK in KEK (a `virt-fw-vars` default), though db holds only our certificate.
- The BIOS path has no Secure Boot. Limine checks hashes (§11), and the kernel reports "boot files not signature-checked".
- The Limine path's modules are checked against the map but not ELF-checked before the kernel starts them (the kernel's own ELF checks still apply).
- Diagnostic mode via the D key depends on the firmware buffering the key. In QEMU, keys typed on the serial console work, but the monitor's `sendkey` didn't reach OVMF.
- Failure recovery is "try the next boot option". There's no second, known-good boot image on the medium to fall back to.
