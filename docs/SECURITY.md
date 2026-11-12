# Bhairav OS security assessment

Status: first audit, 2026-11-12, of commit `38a627f` plus the fix in §6.
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
| CR0.WP | **Enforced** | Set by ShoolLoader. |
| SMEP / SMAP | **Not enabled** | CR4 bits 20/21 are never set. The kernel can run or touch user pages. |
| Kernel stack guard pages | **None** | Kernel stacks are 4 pages in the direct map, with nothing between them. |
| User stack guard | **Partial** | 64 KiB stack. An ELF segment may end right at its bottom. |
| Handles | **Enforced, no rights** | Per-task tables of 64 slots, typed lookup, and **move-only** transfer (there is no duplicate call). No READ/WRITE/MAP rights, no generation counter. |
| Who reaches hardware | **Enforced by delegation** | Only init gets IRQ, I/O-port and PCI handles. It moves them to the drivers (ps2, ahci, e1000/rtl8139). |
| DMA isolation | **None** | No IOMMU. `SYS_DEVICE_BAR` turns on bus mastering. A driver can make its device DMA to any physical address. |
| Kernel memory accounting | **Added in §6** | Before: none. Any program could crash the kernel (V-01). |
| Boot integrity | **None** | ShoolLoader and Limine load every module with no signature or hash. |
| TLS server authentication | **None** | The certificate is skipped. README states this. |
| Package integrity | **None** | BPK has no hash or signature. Repositories may be plain `http://`. |
| App isolation from data | **None** | Every app, third-party ones included, gets a full `fs` channel (the whole disk) and a full `net` channel (`kamalshell/main.cpp` `launch`). |
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
| T6 | Boot media tampering | no verified boot | Persistent kernel compromise | **Open, high** |
| T7 | Memory corruption in a parser (HTML, PNG/JPEG, TCP, FAT) | not reviewed | Compromise of that one task (contained by isolation) | **Unknown: fuzz** |
| T8 | Kernel exploit made easier | no SMEP/SMAP, no stack guards | Escalation from a kernel bug | **Mitigation gap** |

## 3. Security gap analysis

### Confirmed vulnerabilities (reproduced or traced through the code)

| ID | Sev | Finding | Status |
|---|---|---|---|
| V-01 | High | **An unprivileged program can panic the kernel.** `SYS_MEMORY_ALLOC` (256 MiB per call, no limit on calls), `SYS_MEMORY_CREATE`, spawn and channel creation take physical pages with no reserve. Once RAM is gone, the next kernel `new` (for example `new Endpoint` in `channel_create`) calls `panic("heap: out of memory")`. **Reproduced:** serial log `oom check: took 246 MiB … !!! RudraCore panic: heap: out of memory`. | **Fixed, §6** |
| V-02 | Critical | **Packages have no integrity.** BPK (`tools/bpk.cpp`) has no hash or signature, apt accepts `http://` sources, and `https://` doesn't authenticate the server. Anyone on the path can replace an app. | Open |
| V-03 | Critical | **Apps get the whole disk and network.** `KamalShell::launch` gives every app, built-in or third-party, an unrestricted `fs` and `net` channel. One bad app can overwrite `/apps/<other>/app.elf`. | Open |
| V-04 | High | **TLS doesn't check the server certificate.** `tls.cpp` parses the Certificate message and drops it. There is no X.509 parsing, chain, hostname or expiry check, and no trust store. | Open (known) |
| V-05 | Medium | **Endpoint reference cycles leak kernel memory for good.** Send endpoint `y` over its peer `x` and `y` lands in its own queue holding a reference to itself. Close `x` and `y` can never be freed. Longer cycles work too. With V-01 fixed this is bounded memory loss, no longer a crash. | Open |
| V-06 | Medium | **Dead tasks' memory wasn't freed while nothing else was runnable.** Zombies were reaped only at a task switch away from a ready task. | **Fixed, §6** (reaped on each timer tick) |
| V-07 | Low | **Log spoofing.** `SYS_LOG` writes user bytes, including newlines, into the kernel log after a `[name]` prefix. Spawned app names are user-chosen (`SpawnImage.name`), so an app can call itself `init` in the log. | Open |
| V-08 | Low | **An ELF segment can overlap the shared-memory window (≥ 4 GiB).** A later `memory_map` or `memory_alloc` then overwrites an OWNED PTE and leaks that page. It only hurts the task itself. | Open |

### Architectural weaknesses / insecure assumptions

- **No handle rights.** A handle grants everything its object type allows.
  Shared memory is always mapped RW unless the kernel set `readOnly`. There's
  no way to pass read-only access to memory, or send-only access to a channel.
- **No handle generations.** A closed slot is reused by the next `addHandle`,
  so a stale handle in a buggy program silently refers to a new object.
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
| 1 | Per-app storage isolation | kamalshell, fs | `fs` connection bound to a root (`/apps/<id>/data`); only Files and Terminal get `/`. Use a `Connect` field set by KamalShell (apps can't forge it, because the shell is the one connecting) | – | M | fs-test case: an app opening `/apps/other/app.elf` gets refused |
| 2 | Package signatures | tools/bpk, apt, apps.cpp | Ed25519 signature over the BPK. Public key in the boot image. Refuse unsigned packages. Repo index signed (anti-rollback: version must not decrease) | Ed25519 implementation + RFC 8032 test vectors | M | Tampered or unsigned `.bpk` refused; vectors in tls-test |
| 3 | X.509 + chain + hostname + expiry | kapala/tls, new x509.cpp | DER parser, RSA-PKCS1/PSS and ECDSA-P256 verify, trust store file in the boot image, fail closed | Bignum/P-256 code | L | Bad-cert test servers (expired, wrong host, self-signed) must fail |
| 4 | Endpoint cycle prevention | channel, syscall | Refuse to send an endpoint whose queue isn't empty, or an endpoint over its own peer. Or add per-task object quotas | – | S | ipc-test case |
| 5 | SMEP (then SMAP) | arch/cpu init, syscall copy paths | Set CR4.SMEP when CPUID says so. For SMAP, wrap user copies in `stac/clac` helpers | – | S / M | protect-test: kernel exec of a user page must fault (debug build) |
| 6 | Handle rights + generations | object, task, syscalls | `Handle = (gen << 8) \| slot`, rights mask per handle, `handle_restrict(h, rights)` | ABI change: all programs | M | Stale/forged handle tests |
| 7 | Kernel stack guard pages | sched | Map kernel stacks in a vmalloc area with unmapped guards | – | M | Deliberate overflow faults cleanly (PANIC_TEST variant) |
| 8 | Verified boot | shoolloader | Hash every module and verify a signed manifest. Document the Limine/BIOS path as unverified | P2's signature code | M | Altered module refused |
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
