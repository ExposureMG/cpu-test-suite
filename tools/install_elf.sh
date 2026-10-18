#!/bin/sh
# Copy a built XTS ELF to a mounted USB stick as xenon.elf (XeLL autoloads it from the first FAT partition), then sync and
# unmount the stick. The stick is only XeLL's boot medium: XTS never writes to it (results arrive over the network,
# tools/xts_recv.py). Writes exactly one file, <mountpoint>/xenon.elf, and nothing else.
#   tools/install_elf.sh build/xts-smoke.elf32 /run/media/$USER/STICK
# Unmount: udisksctl unmount -b <device> when available, else umount <mountpoint>.
set -eu
elf=${1:?usage: install_elf.sh ELF32 MOUNTPOINT}
usb=${2:?usage: install_elf.sh ELF32 MOUNTPOINT}
[ -f "$elf" ] || { echo "no such file: $elf" >&2; exit 1; }
[ -d "$usb" ] || { echo "not a directory: $usb" >&2; exit 1; }
usb=$(cd "$usb" && pwd -P)
[ "$usb" != / ] || { echo "refusing to write to /" >&2; exit 1; }
mountpoint -q "$usb" 2>/dev/null || echo "warning: $usb does not look like a mount point" >&2
cp "$elf" "$usb/xenon.elf"
sync
echo "installed $(basename "$elf") as $usb/xenon.elf ($(wc -c < "$elf") bytes)"

dev=
if command -v findmnt >/dev/null 2>&1; then dev=$(findmnt -n -o SOURCE --target "$usb" 2>/dev/null || true); fi
if ! mountpoint -q "$usb" 2>/dev/null; then
  echo "$usb is not a mount point: nothing to unmount"
  exit 0
fi
unmounted=0
if [ -n "$dev" ] && [ -b "$dev" ] && command -v udisksctl >/dev/null 2>&1 && udisksctl unmount -b "$dev" >/dev/null 2>&1; then
  unmounted=1
elif umount "$usb" 2>/dev/null; then
  unmounted=1
fi
if [ "$unmounted" = 1 ]; then
  sync
  echo "unmounted $usb: it is safe to remove the stick"
else
  echo "could NOT unmount $usb: do not remove the stick yet (run: udisksctl unmount -b <device>, or umount $usb)" >&2
  exit 1
fi
