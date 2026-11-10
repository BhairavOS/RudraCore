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

CXXFLAGS := \
	--target=x86_64-unknown-none-elf -std=c++23 -O2 -g \
	-Wall -Wextra -Werror \
	-ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-stack-protector -fno-stack-check -fno-PIC \
	-mcmodel=kernel -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-Ikernel/src -isystem $(LIMINE_PROTO) \
	-MMD -MP

ASFLAGS := --target=x86_64-unknown-none-elf

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	-T kernel/linker.ld

CXX_SRCS := $(shell find kernel/src -name '*.cpp')
ASM_SRCS := $(shell find kernel/src -name '*.S')
OBJS := $(CXX_SRCS:%.cpp=$(BUILD)/%.o) $(ASM_SRCS:%.S=$(BUILD)/%.S.o)

QEMU_FLAGS := -M q35 -m 256M -cdrom $(ISO) -boot d

.PHONY: all kernel iso run run-uefi shot clean

all: iso

kernel: $(KERNEL)
iso: $(ISO)

$(BUILD)/font.psf: $(FONT)
	@mkdir -p $(@D)
	cp $< $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S $(BUILD)/font.psf
	@mkdir -p $(@D)
	$(CXX) $(ASFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(ISO): $(KERNEL) boot/limine.conf
	rm -rf $(ISOROOT)
	mkdir -p $(ISOROOT)/boot/limine $(ISOROOT)/EFI/BOOT
	cp $(KERNEL) $(ISOROOT)/boot/
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

-include $(OBJS:.o=.d)
