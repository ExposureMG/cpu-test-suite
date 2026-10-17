#!/bin/sh
# Copy a built XTS ELF to a mounted USB stick as xenon.elf (XeLL autoloads it from the first FAT partition).
#   tools/usb_install.sh build/xts-smoke.elf32 /run/media/$USER/STICK
# The stick also receives the results: <stick>/xts/<console_tag>/<run_id>/ (see README.md).
set -eu
elf=${1:?usage: usb_install.sh ELF32 MOUNTPOINT}
usb=${2:?usage: usb_install.sh ELF32 MOUNTPOINT}
[ -f "$elf" ] || { echo "no such file: $elf" >&2; exit 1; }
[ -d "$usb" ] || { echo "not a directory: $usb" >&2; exit 1; }
mountpoint -q "$usb" 2>/dev/null || echo "warning: $usb does not look like a mount point" >&2
cp "$elf" "$usb/xenon.elf"
mkdir -p "$usb/xts"
sync
echo "installed $(basename "$elf") as $usb/xenon.elf ($(wc -c < "$elf") bytes)"
[ -f "$usb/xts/label.txt" ] && echo "console label: $(cat "$usb/xts/label.txt")" || true
