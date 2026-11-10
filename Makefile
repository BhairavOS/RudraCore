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

# Flags shared by the kernel and user programs. No SSE/x87 anywhere yet: the
# kernel doesn't save floating-point registers on task switches.
COMMON_FLAGS := \
	--target=x86_64-unknown-none-elf -std=c++23 -O2 -g \
	-Wall -Wextra -Werror \
	-ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-stack-protector -fno-stack-check -fno-PIC \
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-fno-omit-frame-pointer -Iabi -MMD -MP

CXXFLAGS := $(COMMON_FLAGS) -mcmodel=kernel -mno-red-zone \
	-Ikernel/src -I$(BUILD) -isystem $(LIMINE_PROTO)

USER_CXXFLAGS := $(COMMON_FLAGS) -Iuserspace/kapalaframe/include

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
USER_OBJS := $(KAPALA_OBJS) $(patsubst %,$(BUILD)/%.o,$(shell find userspace/programs -name '*.cpp'))

# Objects of one user program: $(call programObjs,init)
programObjs = $(patsubst %,$(BUILD)/%.o,$(shell find userspace/programs/$(1) -name '*.cpp'))

# Lets prerequisites use $$* (the pattern stem), for the program rule below.
.SECONDEXPANSION:

# Keep every object file: make would otherwise delete ones it considers
# intermediate, and rebuild them every time.
.SECONDARY:

# ShoolLoader🔱, the UEFI bootloader: a PE32+ UEFI application, so it's
# built for the Windows x86_64 target (UEFI uses its calling convention and
# file format) and linked with lld-link.
SHOOL_SRCS := $(wildcard shoolloader/src/*.cpp)
SHOOL_OBJS := $(SHOOL_SRCS:%.cpp=$(BUILD)/%.obj)
SHOOL_EFI := $(BUILD)/shoolloader.efi
SHOOL_CXXFLAGS := \
	--target=x86_64-unknown-windows -std=c++23 -O2 \
	-Wall -Wextra -Werror \
	-ffreestanding -fshort-wchar -fno-exceptions -fno-rtti -fno-stack-protector \
	-mno-red-zone -mno-stack-arg-probe -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-Iabi -MMD -MP

# What goes on the boot media: stripped copies (the unstripped ELFs in
# build/ keep their debug info for backtraces).
BOOT_FILES := $(BUILD)/boot-files
STRIPPED := $(BOOT_FILES)/rudracore.elf $(USER_PROGRAMS:%=$(BOOT_FILES)/%.elf)

# The UEFI boot image: a FAT file system with ShoolLoader as the default
# UEFI boot program, plus the kernel, programs and shool.cfg.
EFI_IMG := $(BUILD)/efiboot.img

QEMU_FLAGS := -M q35 -m 256M -cdrom $(ISO) -boot d

.PHONY: all kernel user shool iso run run-uefi shot clean FORCE

all: iso

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
$(BUILD)/config.h: FORCE
	@mkdir -p $(@D)
	@echo '#define RUDRACORE_PANIC_TEST $(if $(PANIC_TEST),1,0)' > $@.tmp
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
$(BUILD)/userspace/%.cpp.o: userspace/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(USER_CXXFLAGS) -c $< -o $@

$(BUILD)/userspace/%.S.o: userspace/%.S
	@mkdir -p $(@D)
	$(CXX) $(ASFLAGS) -c $< -o $@

$(BUILD)/user/%.elf: $(KAPALA_OBJS) userspace/link.ld $$(call programObjs,$$*)
	@mkdir -p $(@D)
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) -o $@

# ShoolLoader objects
$(BUILD)/shoolloader/%.obj: shoolloader/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(SHOOL_CXXFLAGS) -c $< -o $@

$(SHOOL_EFI): $(SHOOL_OBJS)
	lld-link /nologo /subsystem:efi_application /entry:efiMain /nodefaultlib /out:$@ $^

$(BOOT_FILES)/rudracore.elf: $(KERNEL)
	@mkdir -p $(@D)
	llvm-strip -o $@ $<

$(BOOT_FILES)/%.elf: $(BUILD)/user/%.elf
	@mkdir -p $(@D)
	llvm-strip -o $@ $<

$(EFI_IMG): $(SHOOL_EFI) $(STRIPPED) boot/shool.cfg
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=8 status=none
	mformat -i $@ -T 16384 -h 64 -s 32 -v BHAIRAV ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot
	mcopy -i $@ $(SHOOL_EFI) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(STRIPPED) boot/shool.cfg ::/boot/

# One ISO for both firmware types:
#   UEFI -> ShoolLoader🔱 (from the FAT boot image)
#   BIOS -> Limine (ShoolLoader is UEFI-only), reading the ISO's /boot
$(ISO): $(EFI_IMG) $(STRIPPED) boot/limine.conf
	rm -rf $(ISOROOT)
	mkdir -p $(ISOROOT)/boot/limine
	cp $(STRIPPED) $(EFI_IMG) $(ISOROOT)/boot/
	cp boot/limine.conf $(ISOROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(ISOROOT)/boot/limine/
	xorriso -as mkisofs -R -r -J -quiet \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/efiboot.img -efi-boot-part --efi-boot-image \
		--protective-msdos-label $(ISOROOT) -o $@
	$(LIMINE_DIR)/limine bios-install $@ 2>/dev/null

# Serial console in the terminal (no window). Quit with Ctrl+A then X.
run: $(ISO)
	qemu-system-x86_64 $(QEMU_FLAGS) -nographic

run-uefi: $(ISO)
	qemu-system-x86_64 $(QEMU_FLAGS) -bios $(OVMF) -nographic

# Boots headless, saves the screen to build/screen.png and the serial log to
# build/serial.log. Set FIRMWARE=uefi to boot through UEFI instead of BIOS.
shot: $(ISO)
	sh scripts/screenshot.sh $(ISO) $(BUILD) $(if $(filter uefi,$(FIRMWARE)),$(OVMF))

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d) $(SHOOL_OBJS:.obj=.d)
