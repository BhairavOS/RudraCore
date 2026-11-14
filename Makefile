# RudraCore / Bhairav OS build. Runs inside the bhairav-toolchain Docker image
# (see Dockerfile and bhairav.ps1), or on any Linux with the same tools.

LIMINE_DIR   ?= /opt/limine
LIMINE_PROTO ?= /opt/limine-protocol/include
FONT         ?= /opt/bhairav/font.psf
OVMF         ?= /usr/share/ovmf/OVMF.fd

BUILD   := build
KERNEL  := $(BUILD)/rudracore.elf
ISO     := $(BUILD)/bhairavos.iso
ISOROOT := $(BUILD)/iso_root

CXX := clang++
LD  := ld.lld

# Flags shared by the kernel and user programs.
COMMON_FLAGS := \
	--target=x86_64-unknown-none-elf -std=c++23 -O2 -g \
	-Wall -Wextra -Werror \
	-ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-stack-protector -fno-stack-check -fno-PIC \
	-fno-omit-frame-pointer -Iabi -MMD -MP

# The kernel never uses floating point or SSE: it doesn't save its own
# FPU state, only user programs' (on task switches).
CXXFLAGS := $(COMMON_FLAGS) -mcmodel=kernel -mno-red-zone \
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-Ikernel/src -I$(BUILD) -isystem $(LIMINE_PROTO)

# User programs get SSE2 and floating point.
USER_CXXFLAGS := $(COMMON_FLAGS) -msse2 -fno-math-errno -Iuserspace/kapalaframe/include -I$(BUILD)

ASFLAGS := --target=x86_64-unknown-none-elf

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	-T kernel/linker.ld

USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	-T userspace/link.ld

# Kernel
CXX_SRCS := $(shell find kernel/src -name '*.cpp')
ASM_SRCS := $(shell find kernel/src -name '*.S')
OBJS := $(CXX_SRCS:%.cpp=$(BUILD)/%.o) $(ASM_SRCS:%.S=$(BUILD)/%.S.o)

# Userspace: the Kapalaframe runtime, linked into every program in
# userspace/programs/<name>/, which becomes build/user/<name>.elf.
KAPALA_SRCS := $(shell find userspace/kapalaframe/src -name '*.cpp' -o -name '*.S')
KAPALA_OBJS := $(patsubst %,$(BUILD)/%.o,$(KAPALA_SRCS))
USER_PROGRAMS := $(notdir $(wildcard userspace/programs/*))
USER_ELFS := $(USER_PROGRAMS:%=$(BUILD)/user/%.elf)
USER_OBJS := $(KAPALA_OBJS) $(patsubst %,$(BUILD)/%.o,$(shell find userspace/programs -name '*.cpp' -o -name '*.S'))

# Objects of one user program: $(call programObjs,init)
programObjs = $(patsubst %,$(BUILD)/%.o,$(shell find userspace/programs/$(1) -name '*.cpp' -o -name '*.S'))

# Lets prerequisites use $$* (the pattern stem), for the program rule below.
.SECONDEXPANSION:

# Keep every object file: make would otherwise delete ones it considers
# intermediate, and rebuild them every time.
.SECONDARY:

# ShoolLoader🔱, the UEFI bootloader: a PE32+ UEFI application, so it's
# built for the Windows x86_64 target (UEFI uses its calling convention and
# file format) and linked with lld-link.
SHOOL_SRCS := $(wildcard shoolloader/src/*.cpp)
# Verified boot uses Kapalaframe's SHA-256 and Ed25519 (shoolloader/src/verify.cpp).
SHOOL_OBJS := $(SHOOL_SRCS:%.cpp=$(BUILD)/%.obj) $(BUILD)/shoolloader/crypto.obj
SHOOL_EFI := $(BUILD)/shoolloader.efi
SHOOL_CXXFLAGS := \
	--target=x86_64-unknown-windows -std=c++23 -O2 \
	-Wall -Wextra -Werror \
	-ffreestanding -fshort-wchar -fno-exceptions -fno-rtti -fno-stack-protector \
	-mno-red-zone -mno-stack-arg-probe -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-Iabi -Iuserspace/kapalaframe/include -I$(BUILD) -MMD -MP

# What goes on the boot media: stripped copies (the unstripped ELFs in
# build/ keep their debug info for backtraces).
BOOT_FILES := $(BUILD)/boot-files
# Third-party apps (the SDK examples in apps/<name>/): built like programs,
# packed into .bpk packages, and bundled into the catalog the App Store
# offers. They aren't part of the system: they're installed on the disk.
SDK_APPS := $(patsubst apps/%/manifest.txt,%,$(wildcard apps/*/manifest.txt))
PACKAGES := $(SDK_APPS:%=$(BUILD)/packages/%.bpk)
APP_OBJS := $(patsubst %,$(BUILD)/%.o,$(shell find apps -name '*.cpp' 2>/dev/null))
appObjs = $(patsubst %,$(BUILD)/%.o,$(shell find apps/$(1) -name '*.cpp'))
BPK := $(BUILD)/tools/bpk

STRIPPED := $(BOOT_FILES)/rudracore.elf $(USER_PROGRAMS:%=$(BOOT_FILES)/%.elf) $(BOOT_FILES)/fonts.bin \
	$(BOOT_FILES)/catalog.bin $(BOOT_FILES)/trust.bin $(BOOT_FILES)/roots.bin

# Verified boot. ShoolLoader boots only files listed in shool.sum (their
# SHA-256) when shool.sig is that list's Ed25519 signature by BOOT_KEY,
# whose public half is built into the loader. BOOT_VERSION is the boot
# image's security version: raise it when a release fixes a security hole,
# and machines that booted it refuse older images from then on.
BOOT_KEY ?= keys/boot-signing.pem
BOOT_VERSION ?= 1
SIGNED_BOOT := $(BOOT_FILES)/shool.cfg $(BOOT_FILES)/shool.sum $(BOOT_FILES)/shool.sig

# More package/repository keys to trust besides SIGNING_KEY's: public keys
# (PEM) in keys/trusted/. Put the old key's public half there when rotating
# to a new SIGNING_KEY; delete it to revoke a key. At most 15.
TRUSTED_KEYS := $(wildcard keys/trusted/*.pem)

# The certificate authorities HTTPS trusts: the build machine's bundle
# (Mozilla's list, from the ca-certificates package), as roots.bin.
CA_BUNDLE ?= /etc/ssl/certs/ca-certificates.crt

# Packages are signed (Ed25519) with this key, made the first time. It
# lives outside build/ so `make clean` keeps it (packages already installed
# stay updatable) and is never committed (see .gitignore). Bhairav OS only
# installs packages signed by a key in trust.bin, made from it.
SIGNING_KEY ?= keys/package-signing.pem

# The UEFI boot image: a FAT file system with ShoolLoader as the default
# UEFI boot program, plus the kernel, programs and shool.cfg.
EFI_IMG := $(BUILD)/efiboot.img

# The disk: a FAT32 image attached as a SATA disk (q35's AHCI controller).
# Created once and never rebuilt, so files saved in Bhairav OS stay (it
# lives outside build/, which `make clean` deletes). Delete it to start over.
DISK := disk.img
DISK_MB := 64

# The network card QEMU emulates: e1000 (default) or rtl8139.
NIC ?= e1000

QEMU_FLAGS := -M q35 -m 256M -cdrom $(ISO) -boot d \
	-drive file=$(DISK),format=raw,if=none,id=disk -device ide-hd,drive=disk,bus=ide.0 \
	-netdev user,id=net -device $(NIC),netdev=net

.PHONY: all kernel user shool iso disk packages repo serve-repo install-app run run-uefi shot clean FORCE

all: iso $(DISK)

disk: $(DISK)

kernel: $(KERNEL)
user: $(USER_ELFS)
shool: $(SHOOL_EFI)
iso: $(ISO)

$(BUILD)/font.psf: $(FONT)
	@mkdir -p $(@D)
	cp $< $@

# The logo: the black background around the mask becomes transparent (flood
# fill from the corners, so black details inside the mask are kept).
$(BUILD)/logo.rgba: assets/bhairav-logo.jpg
	@mkdir -p $(@D)
	convert $< -alpha set -fuzz 14% -fill none \
		-draw "color 0,0 floodfill" -draw "color 399,0 floodfill" \
		-draw "color 0,399 floodfill" -draw "color 399,399 floodfill" \
		-resize 256x256! rgba:$@

# Build options as a header. Only rewritten when an option changes, so only
# the files that include it get rebuilt.
#   PANIC_TEST=1  makes the kernel fault on purpose to show the panic screen.
#   SECURITY_TEST=1  init first runs oom-test (a program uses up all memory;
#                    the kernel must refuse it, not panic).
#   FAULT_TEST=stack|smep|smap|dma  the kernel breaks that protection on purpose:
#                    the boot must end in a panic naming the fault. Run with
#                    CPU=max so QEMU emulates SMEP and SMAP.
$(BUILD)/config.h: FORCE
	@mkdir -p $(@D)
	@echo '#define RUDRACORE_PANIC_TEST $(if $(PANIC_TEST),1,0)' > $@.tmp
	@echo '#define BHAIRAV_SECURITY_TEST $(if $(SECURITY_TEST),1,0)' >> $@.tmp
	@echo '#define RUDRACORE_FAULT_TEST $(if $(filter stack,$(FAULT_TEST)),1,$(if $(filter smep,$(FAULT_TEST)),2,$(if $(filter smap,$(FAULT_TEST)),3,$(if $(filter dma,$(FAULT_TEST)),4,0))))' >> $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# Kernel objects
$(BUILD)/kernel/%.o: kernel/%.cpp | $(BUILD)/config.h
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kernel/%.S.o: kernel/%.S $(BUILD)/font.psf $(BUILD)/logo.rgba
	@mkdir -p $(@D)
	$(CXX) $(ASFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# User objects
$(BUILD)/userspace/%.cpp.o: userspace/%.cpp | $(BUILD)/config.h
	@mkdir -p $(@D)
	$(CXX) $(USER_CXXFLAGS) -c $< -o $@

$(BUILD)/userspace/%.S.o: userspace/%.S $(BUILD)/font.psf $(BUILD)/logo.rgba
	@mkdir -p $(@D)
	$(CXX) $(ASFLAGS) -c $< -o $@

$(BUILD)/user/%.elf: $(KAPALA_OBJS) userspace/link.ld $$(call programObjs,$$*)
	@mkdir -p $(@D)
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) -o $@

# ShoolLoader objects
$(BUILD)/shoolloader/%.obj: shoolloader/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(SHOOL_CXXFLAGS) -c $< -o $@

$(BUILD)/shoolloader/crypto.obj: userspace/kapalaframe/src/crypto.cpp
	@mkdir -p $(@D)
	$(CXX) $(SHOOL_CXXFLAGS) -c $< -o $@

# The boot key's public half, built into the loader.
$(BUILD)/shoolloader/src/verify.obj: $(BUILD)/boot-key.h

$(BUILD)/boot-key.h: $(BOOT_KEY)
	@mkdir -p $(@D)
	{ echo '// Made by the Makefile from $(BOOT_KEY): boot files must be signed by it.'; \
	  echo 'constexpr uint8_t BOOT_KEY[32] = {'; \
	  openssl pkey -in $< -pubout -outform DER | tail -c 32 | od -An -v -tx1 | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  echo '};'; } > $@.tmp
	mv $@.tmp $@

$(SHOOL_EFI): $(SHOOL_OBJS)
	lld-link /nologo /subsystem:efi_application /entry:efiMain /nodefaultlib /out:$@ $^

$(BOOT_FILES)/rudracore.elf: $(KERNEL)
	@mkdir -p $(@D)
	llvm-strip -o $@ $<

# The fonts (made by scripts/make-fonts.py, see `make fonts`).
# Apps and packages
$(BUILD)/apps/%.cpp.o: apps/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(USER_CXXFLAGS) -c $< -o $@

$(BUILD)/app-elf/%.elf: $(KAPALA_OBJS) userspace/link.ld $$(call appObjs,$$*)
	@mkdir -p $(@D)
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) -o $@

# The packing tool runs on the build machine.
$(BPK): tools/bpk.cpp
	@mkdir -p $(@D)
	clang++ -std=c++20 -O2 $< -o $@

$(SIGNING_KEY):
	@mkdir -p $(@D)
	(umask 077 && openssl genpkey -algorithm ed25519 -out $@)
	@echo "made a new package signing key: $@ (keep it private, don't lose it)"

$(BOOT_KEY):
	@mkdir -p $(@D)
	(umask 077 && openssl genpkey -algorithm ed25519 -out $@)
	@echo "made a new boot signing key: $@ (keep it private; only boot images it signs will boot)"

$(BUILD)/packages/%.bpk: $(BUILD)/app-elf/%.elf apps/%/manifest.txt $(BPK) $(SIGNING_KEY)
	@mkdir -p $(@D)
	llvm-strip -o $(@D)/$*.elf $<
	$(BPK) pack $@.unsigned apps/$*/manifest.txt $(@D)/$*.elf $(wildcard apps/$*/icon.png)
	$(BPK) tbs $@.unsigned $@.tbs
	openssl pkeyutl -sign -rawin -inkey $(SIGNING_KEY) -in $@.tbs -out $@.sig
	$(BPK) attach $@.tbs $@.sig $@
	rm -f $(@D)/$*.elf $@.unsigned $@.tbs $@.sig

# roots.bin: a catalog (the .bpk catalog format) of the DER certificates.
$(BOOT_FILES)/roots.bin: $(CA_BUNDLE) $(BPK)
	@mkdir -p $(@D)
	rm -rf $(BUILD)/roots && mkdir -p $(BUILD)/roots
	awk '/BEGIN CERTIFICATE/ { n++; f = sprintf("$(BUILD)/roots/%03d.pem", n) } f { print > f } /END CERTIFICATE/ { close(f); f = "" }' $<
	for f in $(BUILD)/roots/*.pem; do openssl x509 -in $$f -outform DER -out $${f%.pem}.der; done
	$(BPK) catalog $@ $(BUILD)/roots/*.der 2>/dev/null

# The keys Bhairav OS trusts for packages and repository indexes: "BTK1", a
# 32-bit count, then each 32-byte Ed25519 public key (the last 32 bytes of
# its DER form). SIGNING_KEY's first, then TRUSTED_KEYS. It's a boot file,
# so verified boot protects it.
$(BOOT_FILES)/trust.bin: $(SIGNING_KEY) $(TRUSTED_KEYS)
	@mkdir -p $(@D)
	@test $(words x $(TRUSTED_KEYS)) -le 16 || (echo "trust.bin holds at most 16 keys" && exit 1)
	printf "BTK1\\$$(printf %03o $(words x $(TRUSTED_KEYS)))\\000\\000\\000" > $@
	openssl pkey -in $(SIGNING_KEY) -pubout -outform DER | tail -c 32 >> $@
	for k in $(TRUSTED_KEYS); do openssl pkey -pubin -in $$k -outform DER | tail -c 32 >> $@ || exit 1; done

# The signed list of boot files (see BOOT_KEY above). Remade every time but
# only replaced when it changes, so BOOT_VERSION changes are picked up.
$(BOOT_FILES)/shool.cfg: boot/shool.cfg
	@mkdir -p $(@D)
	cp $< $@

$(BOOT_FILES)/shool.sum: $(STRIPPED) $(BOOT_FILES)/shool.cfg FORCE
	@{ echo "version $(BOOT_VERSION)"; cd $(BOOT_FILES) && sha256sum $(notdir $(STRIPPED)) shool.cfg; } > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BOOT_FILES)/shool.sig: $(BOOT_FILES)/shool.sum $(BOOT_KEY)
	openssl pkeyutl -sign -rawin -inkey $(BOOT_KEY) -in $< -out $@

# Limine (the BIOS path) checks a BLAKE2B hash after every path, and its
# own copy holds the config's hash (enroll-config below), so a changed file
# or config stops the boot. That's integrity, not a signature: whoever can
# rewrite the whole medium can rewrite limine-bios.sys too.
$(BUILD)/limine.conf: boot/limine.conf $(STRIPPED)
	cp $< $@.tmp
	for f in $(notdir $(STRIPPED)); do \
		sed -i "s|boot():/boot/$$f\$$|&#$$(b2sum $(BOOT_FILES)/$$f | cut -d' ' -f1)|" $@.tmp || exit 1; \
	done
	@if grep -q 'boot():/boot/[^#]*$$' $@.tmp; then echo "limine.conf: a boot file without a hash" && exit 1; fi
	mv $@.tmp $@

packages: $(PACKAGES)

$(BOOT_FILES)/catalog.bin: $(PACKAGES) $(BPK)
	@mkdir -p $(@D)
	$(BPK) catalog $@ $(PACKAGES)

# An apt repository: every package here plus its Packages index, in
# build/repo/. `make serve-repo` serves it on port 8080; in QEMU, Bhairav OS
# reaches it with: apt add-repo http://10.0.2.2:8080
REPO := $(BUILD)/repo
SERVE := $(BUILD)/tools/serve

repo: $(REPO)/Packages

# The index starts with a serial (the time it was made: apt refuses one
# older than it has already read), gives each package's SHA-256, and is
# signed: Packages.sig, by SIGNING_KEY.
$(REPO)/Packages: $(PACKAGES) $(SIGNING_KEY)
	@mkdir -p $(REPO)
	cp $(PACKAGES) $(REPO)/
	@printf 'serial = %s\n\n' "$$(date +%s)" > $@.tmp
	@for app in $(SDK_APPS); do \
		sed -e 's/\r$$//' apps/$$app/manifest.txt >> $@.tmp; \
		echo "file = $$app.bpk" >> $@.tmp; \
		echo "sha256 = $$(sha256sum $(REPO)/$$app.bpk | cut -d' ' -f1)" >> $@.tmp; \
		echo >> $@.tmp; \
	done
	openssl pkeyutl -sign -rawin -inkey $(SIGNING_KEY) -in $@.tmp -out $@.sig
	mv $@.tmp $@

$(SERVE): tools/serve.cpp
	@mkdir -p $(@D)
	clang++ -std=c++20 -O2 $< -o $@

serve-repo: $(REPO)/Packages $(SERVE)
	$(SERVE) $(REPO) 8080

# Copies a package onto the disk, to install from the App Store's file
# field: make install-app PKG=build/packages/clock.bpk
install-app: $(DISK)
	@test -n "$(PKG)" || (echo "usage: make install-app PKG=path/to/app.bpk" && exit 1)
	mcopy -o -i $(DISK) $(PKG) ::/

$(BOOT_FILES)/fonts.bin: assets/fonts.bin
	@mkdir -p $(@D)
	cp $< $@

$(BOOT_FILES)/%.elf: $(BUILD)/user/%.elf
	@mkdir -p $(@D)
	llvm-strip -o $@ $<

$(EFI_IMG): $(SHOOL_EFI) $(STRIPPED) $(SIGNED_BOOT)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=16 status=none
	mformat -i $@ -T 32768 -h 64 -s 32 -v BHAIRAV ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot
	mcopy -i $@ $(SHOOL_EFI) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(STRIPPED) $(SIGNED_BOOT) ::/boot/

# A blank FAT32 volume (no partition table) with a welcome file.
$(DISK): assets/welcome.txt
	@if [ -f $@ ]; then touch $@; exit 0; fi; \
	dd if=/dev/zero of=$@.tmp bs=1M count=$(DISK_MB) status=none && \
	mformat -i $@.tmp -F -v BHAIRAV :: && \
	mcopy -i $@.tmp assets/welcome.txt ::/welcome.txt && \
	mv $@.tmp $@

# One ISO for both firmware types:
#   UEFI -> ShoolLoader🔱 (from the FAT boot image)
#   BIOS -> Limine (ShoolLoader is UEFI-only), reading the ISO's /boot
$(ISO): $(EFI_IMG) $(STRIPPED) $(BUILD)/limine.conf
	rm -rf $(ISOROOT)
	mkdir -p $(ISOROOT)/boot/limine
	cp $(STRIPPED) $(EFI_IMG) $(ISOROOT)/boot/
	cp $(BUILD)/limine.conf $(ISOROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(ISOROOT)/boot/limine/
	$(LIMINE_DIR)/limine enroll-config --quiet $(ISOROOT)/boot/limine/limine-bios.sys \
		$$(b2sum $(BUILD)/limine.conf | cut -d' ' -f1)
	xorriso -as mkisofs -R -r -J -quiet \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/efiboot.img -efi-boot-part --efi-boot-image \
		--protective-msdos-label $(ISOROOT) -o $@
	$(LIMINE_DIR)/limine bios-install $@ 2>/dev/null

# Serial console in the terminal (no window). Quit with Ctrl+A then X.
run: $(ISO) $(DISK)
	qemu-system-x86_64 $(QEMU_FLAGS) -nographic

run-uefi: $(ISO) $(DISK)
	qemu-system-x86_64 $(QEMU_FLAGS) -bios $(OVMF) -nographic

# Boots headless, saves the screen to build/screen.png and the serial log to
# build/serial.log. Set FIRMWARE=uefi to boot through UEFI instead of BIOS.
shot: $(ISO) $(DISK)
	NIC=$(NIC) sh scripts/screenshot.sh $(ISO) $(BUILD) $(DISK) $(if $(filter uefi,$(FIRMWARE)),$(OVMF))

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d) $(SHOOL_OBJS:.obj=.d) $(APP_OBJS:.o=.d)
