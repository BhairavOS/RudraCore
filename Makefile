# RudraCore / BhairavOS build. Runs inside the bhairav-toolchain Docker image
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

# Userspace: the KapalaFrame runtime, linked into every program in
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

QEMU_FLAGS := -M q35 -m 256M -cdrom $(ISO) -boot d

.PHONY: all kernel user iso run run-uefi shot clean FORCE

all: iso

kernel: $(KERNEL)
user: $(USER_ELFS)
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

$(ISO): $(KERNEL) $(USER_ELFS) boot/limine.conf
	rm -rf $(ISOROOT)
	mkdir -p $(ISOROOT)/boot/limine $(ISOROOT)/EFI/BOOT
	cp $(KERNEL) $(USER_ELFS) $(ISOROOT)/boot/
	cp boot/limine.conf $(ISOROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	   $(LIMINE_DIR)/limine-uefi-cd.bin $(ISOROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISOROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -quiet \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
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

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d)
