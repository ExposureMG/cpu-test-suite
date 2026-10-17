#!/bin/sh
# Copy a built XTS ELF to a mounted USB stick as xenon.elf (XeLL autoloads it from the first FAT partition).
#   tools/usb_install.sh build/xts-smoke.elf32 /run/media/$USER/STICK
# The stick also receives the results: <stick>/xts/<console_tag>/<run_id>/ (see README.md).
# Writes only xenon.elf and the empty xts/ directory under the given mountpoint. Afterwards it syncs and unmounts the stick
# (udisksctl unmount -b <device> when available, else umount <mountpoint>) and says when it is safe to remove it.
set -eu
elf=${1:?usage: usb_install.sh ELF32 MOUNTPOINT}
usb=${2:?usage: usb_install.sh ELF32 MOUNTPOINT}
[ -f "$elf" ] || { echo "no such file: $elf" >&2; exit 1; }
[ -d "$usb" ] || { echo "not a directory: $usb" >&2; exit 1; }
usb=$(cd "$usb" && pwd -P)
[ "$usb" != / ] || { echo "refusing to write to /" >&2; exit 1; }
mountpoint -q "$usb" 2>/dev/null || echo "warning: $usb does not look like a mount point" >&2
cp "$elf" "$usb/xenon.elf"
mkdir -p "$usb/xts"
sync
echo "installed $(basename "$elf") as $usb/xenon.elf ($(wc -c < "$elf") bytes)"
[ -f "$usb/xts/label.txt" ] && echo "console label: $(cat "$usb/xts/label.txt")" || true

# flush and unmount so the stick can be pulled without corrupting the FAT
sync
dev=
if command -v findmnt >/dev/null 2>&1; then dev=$(findmnt -n -o SOURCE --target "$usb" 2>/dev/null || true); fi
unmounted=0
if mountpoint -q "$usb" 2>/dev/null; then
  if [ -n "$dev" ] && [ -b "$dev" ] && command -v udisksctl >/dev/null 2>&1 && udisksctl unmount -b "$dev" >/dev/null 2>&1; then
    unmounted=1
  elif umount "$usb" 2>/dev/null; then
    unmounted=1
  fi
else
  echo "$usb is not a mount point: nothing to unmount (this is not a removable stick)"
  exit 0
fi
if [ "$unmounted" = 1 ]; then
  sync
  echo "unmounted $usb: it is safe to remove the stick"
else
  echo "could NOT unmount $usb: do not remove the stick yet (run: udisksctl unmount -b <device>, or umount $usb)" >&2
  exit 1
fi
