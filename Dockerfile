# Bhairav OS toolchain image: compiler, assembler, ISO tools, QEMU and Limine.
# Build:  docker build -t bhairav-toolchain .
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang lld llvm make nasm xorriso mtools git ca-certificates \
        qemu-system-x86 ovmf console-setup-linux gzip imagemagick \
    && rm -rf /var/lib/apt/lists/*

# Limine: temporary bootloader until ShoolLoader is ready.
RUN git clone --depth=1 --branch=v9.x-binary \
        https://github.com/limine-bootloader/limine.git /opt/limine \
    && make -C /opt/limine

# Limine boot protocol header (limine.h).
RUN git clone --depth=1 \
        https://github.com/limine-bootloader/limine-protocol.git /opt/limine-protocol

# 8x16 console font used by the kernel's framebuffer console.
RUN mkdir -p /opt/bhairav \
    && gunzip -c /usr/share/consolefonts/Lat2-Terminus16.psf.gz > /opt/bhairav/font.psf

WORKDIR /src
