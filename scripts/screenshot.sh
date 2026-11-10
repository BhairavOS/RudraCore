#!/bin/sh
# Boots the ISO in a headless QEMU, then saves a screenshot and the serial log.
# Usage: screenshot.sh <iso> <out-dir> [uefi-firmware]
set -eu

ISO="$1"
OUT="$2"
FIRMWARE="${3:-}"
WAIT="${WAIT:-5}"

rm -f "$OUT/screen.png" "$OUT/serial.log"

set -- -M q35 -m 256M -cdrom "$ISO" -boot d \
    -display none -serial "file:$OUT/serial.log" -monitor stdio
if [ -n "$FIRMWARE" ]; then
    set -- "$@" -bios "$FIRMWARE"
fi

{
    sleep "$WAIT"
    echo "screendump $OUT/screen.png -f png"
    sleep 1
    echo "quit"
} | qemu-system-x86_64 "$@" > /dev/null

echo "--- serial log ($OUT/serial.log) ---"
cat "$OUT/serial.log"
echo "--- screenshot: $OUT/screen.png ---"
