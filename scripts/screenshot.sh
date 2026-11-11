#!/bin/sh
# Boots the ISO in a headless QEMU, then saves a screenshot and the serial log.
# Usage: screenshot.sh <iso> <out-dir> <disk-image> [uefi-firmware]
#
# Environment:
#   WAIT=5        seconds to wait after boot
#   MEMORY=256M   RAM size
#   KEYS="h i ret"  QEMU key names to type after WAIT (tests the keyboard driver)
#   MONITOR="mouse_move 40 10; mouse_button 1; mouse_button 0"
#                 QEMU monitor commands to run after KEYS (tests the mouse)
#   STEP=0.3      seconds between MONITOR commands (lower it for double clicks)
set -eu

ISO="$1"
OUT="$2"
DISK="$3"
FIRMWARE="${4:-}"
WAIT="${WAIT:-5}"
MEMORY="${MEMORY:-256M}"
KEYS="${KEYS:-}"
MONITOR="${MONITOR:-}"
STEP="${STEP:-0.3}"

rm -f "$OUT/screen.png" "$OUT/serial.log"

set -- -M q35 -m "$MEMORY" -cdrom "$ISO" -boot d \
    -drive "file=$DISK,format=raw,if=none,id=disk" -device ide-hd,drive=disk,bus=ide.0 \
    -netdev user,id=net -device e1000,netdev=net \
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
    # (if-statements, not `[ ] &&`: under `set -e` a false test would end
    # this block early and QEMU would never get "quit")
    echo "$MONITOR" | tr ';' '\n' | while read -r command; do
        if [ -n "$command" ]; then
            echo "$command"
            sleep "$STEP"
        fi
    done
    if [ -n "$KEYS$MONITOR" ]; then
        sleep 1
    fi
    echo "screendump $OUT/screen.png -f png"
    sleep 1
    echo "quit"
} | qemu-system-x86_64 "$@" > /dev/null

echo "--- serial log ($OUT/serial.log) ---"
cat "$OUT/serial.log"
echo "--- screenshot: $OUT/screen.png ---"
