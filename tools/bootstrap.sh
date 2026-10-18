#!/bin/sh
# Verify the toolchain and inputs XTS needs. Changes nothing outside this repository.
#   make bootstrap        (or: XTS_DEVKITXENON=/usr/local/xenon LIBXENON_SRC=~/Projects/libxenon/libxenon tools/bootstrap.sh)
# Like the Makefile, it ignores a DEVKITXENON from the environment (a stale /sdk/xenon is common).
set -u
DEVKITXENON=${XTS_DEVKITXENON:-/usr/local/xenon}
LIBXENON_SRC=${LIBXENON_SRC:-$HOME/Projects/libxenon/libxenon}
fail=0
ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }
echo "XTS bootstrap: DEVKITXENON=$DEVKITXENON LIBXENON_SRC=$LIBXENON_SRC"
gcc_v=$("$DEVKITXENON/bin/xenon-gcc" -dumpversion 2>/dev/null) && ok "xenon-gcc $gcc_v" || bad "xenon-gcc not found in $DEVKITXENON/bin"
case "${gcc_v:-0}" in 16*) ;; *) bad "xenon-gcc 16.x expected (vmuleuh/vmulesh fix)";; esac
tmp=$(mktemp -d)
printf ' vor128 100,101,127\n vmaddfp128 1,2,3,1\n lvx128 64,3,4\n' > "$tmp/t.s"
"$DEVKITXENON/bin/xenon-as" -mvmx128 "$tmp/t.s" -o "$tmp/t.o" 2>/dev/null && ok "xenon-as assembles VMX128 (-mvmx128)" || bad "xenon-as lacks VMX128"
rm -rf "$tmp"
for f in usr/lib/libxenon.a usr/include/xb360/xb360.h app.lds; do
  [ -f "$DEVKITXENON/$f" ] && ok "$f" || bad "$DEVKITXENON/$f missing"
done
[ -f "$LIBXENON_SRC/drivers/ppc/except.S" ] && ok "libxenon except.S (patched copy generated into \$(BUILD)/gen)" || bad "$LIBXENON_SRC/drivers/ppc/except.S missing (set LIBXENON_SRC)"
python3 -c 'import sys; assert sys.version_info >= (3, 8)' 2>/dev/null && ok "python3 $(python3 -c 'import sys; print(sys.version.split()[0])')" || bad "python3 >= 3.8 needed"
gcc --version >/dev/null 2>&1 && ok "host gcc $(gcc -dumpversion)" || bad "host gcc missing"
exit $fail
