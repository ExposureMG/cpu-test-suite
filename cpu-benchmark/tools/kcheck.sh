#!/usr/bin/env bash
# kcheck.sh - check one kernel source file in isolation.
#
#   tools/kcheck.sh src/kernels/int_crc32.c crc32 [crc32b ...]
#
# For every kernel id defined in the file:
#   1. host build with ASan/UBSan, run tools/ktest.c (descriptor sanity,
#      idempotency, result value);
#   2. host build at -O2 (rough host timing);
# Then, for the file:
#   3. cross-compile with both Xenon toolchains (-O2, the benchmark's flags),
#      fail on warnings, list undefined symbols that are not allowed, fail on
#      writable data sections, and write the assembly to build/kcheck/.
#
# Run it from the project root.  Exit status is non-zero on any failure.
set -u
cd "$(dirname "$0")/.."

X3=${X3:-/home/e3xp0/Projects/libxenon/libxenon/toolchain/testxenon3}
O16=${O16:-/home/e3xp0/Projects/libxenon/libxenon/toolchain/test16old}
OPT=${OPT:--O2}
MACHDEP=(-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float)
WARN=(-Wall -Wextra -Werror -Wno-unused-parameter)
OUT=build/kcheck
# mktemp, not $$: sandboxed shells may all run with the same small PID
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kcheck.XXXXXX") || exit 2

if [ $# -lt 2 ]; then
	echo "usage: $0 <kernel.c> <id> [<id> ...]" >&2
	exit 2
fi
src=$1
shift
mkdir -p "$OUT" "$TMP"
trap 'rm -rf "$TMP"' EXIT
base=$(basename "$src" .c)
fail=0

for id in "$@"; do
	if ! gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		"${WARN[@]}" -DBENCH_HOST -DBENCH_VARIANT=kt -DKTEST_ID="$id" -Iinclude \
		tools/ktest.c "$src" -lm -o "$TMP/asan_$id"; then
		echo "FAIL $id: host (asan) build"; fail=1; continue
	fi
	echo "-- host asan/ubsan:"
	"$TMP/asan_$id" || { echo "FAIL $id: host (asan) run"; fail=1; }
	if ! gcc -std=gnu11 -O2 "${WARN[@]}" -DBENCH_HOST -DBENCH_VARIANT=kt \
		-DKTEST_ID="$id" -Iinclude tools/ktest.c "$src" -lm -o "$TMP/o2_$id"; then
		echo "FAIL $id: host (-O2) build"; fail=1; continue
	fi
	echo "-- host -O2:"
	"$TMP/o2_$id" || { echo "FAIL $id: host (-O2) run"; fail=1; }
done

for v in xenon3:"$X3" old16:"$O16"; do
	vid=${v%%:*}
	root=${v#*:}
	cc=$root/bin/xenon-gcc
	obj=$TMP/$base.$vid.o
	if ! "$cc" -std=gnu11 "${MACHDEP[@]}" $OPT "${WARN[@]}" -DBENCH_VARIANT="$vid" \
		-Iinclude -c "$src" -o "$obj"; then
		echo "FAIL $base: $vid cross build"; fail=1; continue
	fi
	"$cc" -std=gnu11 "${MACHDEP[@]}" $OPT -DBENCH_VARIANT="$vid" -Iinclude -S \
		-fverbose-asm "$src" -o "$OUT/$base.$vid.s"
	# Undefined symbols: BLIB routines, sqrt fallbacks and libgcc ABI helpers only.
	bad=$("$root/bin/xenon-nm" -u "$obj" | awk '{print $NF}' |
		grep -Ev "^(${vid}_lib_[A-Za-z0-9_]+|sqrtf?|_(save|rest)(gpr|fpr|vr|vr128)_[0-9]+(_[a-z])?)$" || true)
	if [ -n "$bad" ]; then
		echo "FAIL $base: $vid references disallowed symbols:" $bad; fail=1
	fi
	# No writable data: everything mutable lives in the arena.
	wr=$("$root/bin/xenon-size" -A "$obj" |
		awk '$1 ~ /^\.(s?data|s?bss)/ && $2 > 0 {print $1 "(" $2 ")"}')
	com=$("$root/bin/xenon-nm" "$obj" | awk '$2 == "C" {print $3}')
	if [ -n "$wr$com" ]; then
		echo "FAIL $base: $vid has writable data:" $wr $com; fail=1
	fi
	text=$("$root/bin/xenon-size" -A "$obj" | awk '$1 ~ /^\.text/ {s += $2} END {print s+0}')
	echo "-- $vid: ok, .text ${text} bytes, asm in $OUT/$base.$vid.s"
done

[ $fail -eq 0 ] && echo "PASS $base" || echo "FAILED $base"
exit $fail
