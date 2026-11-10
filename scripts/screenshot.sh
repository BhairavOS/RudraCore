#!/bin/sh
# Boots the ISO in a headless QEMU, then saves a screenshot and the serial log.
# Usage: screenshot.sh <iso> <out-dir> [uefi-firmware]
#
# Environment:
#   WAIT=5        seconds to wait after boot
#   MEMORY=256M   RAM size
#   KEYS="h i ret"  QEMU key names to type after WAIT (tests the keyboard driver)
set -eu

ISO="$1"
OUT="$2"
FIRMWARE="${3:-}"
WAIT="${WAIT:-5}"
MEMORY="${MEMORY:-256M}"
KEYS="${KEYS:-}"

rm -f "$OUT/screen.png" "$OUT/serial.log"

set -- -M q35 -m "$MEMORY" -cdrom "$ISO" -boot d \
    -display none -serial "file:$OUT/serial.log" -monitor stdio
if [ -n "$FIRMWARE" ]; then
    set -- "$@" -bios "$FIRMWARE"
fi

{
    sleep "$WAIT"
    for key in $KEYS; do
        echo "sendkey $key"
        sleep 0.15
    done
    [ -n "$KEYS" ] && sleep 1
    echo "screendump $OUT/screen.png -f png"
    sleep 1
    echo "quit"
} | qemu-system-x86_64 "$@" > /dev/null

echo "--- serial log ($OUT/serial.log) ---"
cat "$OUT/serial.log"
echo "--- screenshot: $OUT/screen.png ---"
