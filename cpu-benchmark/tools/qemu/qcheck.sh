#!/usr/bin/env bash
# qcheck.sh - the steps of "make qemu-check" (run by tools/qemu/qemu.mk,
# which passes the paths below in the environment).
#
#   1. instruction scan: no 64-bit, VMX128 or other non-G4 instruction in any
#      object (objdump -M 7400 prints such encodings as ".long"; fsqrt,
#      mfocrf/mtocrf and 64-bit compares are matched by name; objdump -M xenon
#      names VMX128 instructions "*128");
#   2. the 32-bit helper calls the kernels make (informational);
#   3. helper self-test: qselftest.elf (both compilers, under qemu) must print
#      exactly what the host-compiled reference prints;
#   4. the runner under qemu-ppc (all kernels; if it crashes or times out, each
#      kernel again on its own so one crash does not hide the rest); kernel
#      failures that match an entry of known-failures.txt exactly (root-caused
#      toolchain defects) are reported as XFAIL and do not fail the step;
#   5. BE/LE cross-check against the host-compiled runner (informational).
#
# Exit status: 0 if steps 1, 3 and 4 passed.
set -u

: "${QEMU_BIN:=qemu-ppc}" "${QEMU_CPU:=7400}" "${QEMU_TIMEOUT:=1200}" "${QEMU_ARGS:=}"
: "${QEMU_OUT:?}" "${QEMU_X3:?}" "${QEMU_ELF:?}" "${QEMU_ST_ELF:?}" "${QEMU_HOST_ST:?}"
: "${QEMU_HOST_RUNNER:=}" "${QEMU_OBJS:=}"
: "${QEMU_KNOWN:=$(dirname "$0")/known-failures.txt}"

OBJDUMP=$QEMU_X3/bin/xenon-objdump
NM=$QEMU_X3/bin/xenon-nm
RUN=(timeout "$QEMU_TIMEOUT" "$QEMU_BIN" -cpu "$QEMU_CPU")
read -r -a OBJS <<<"$QEMU_OBJS"
read -r -a EXTRA <<<"$QEMU_ARGS"
fail=0

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
	echo "qemu-check: $QEMU_BIN not found (set QEMU_BIN)" >&2
	exit 2
fi

# ---- 1. instruction scan ---------------------------------------------------
# Prints "<object>: <function>: <insn>" for every suspicious instruction.
scan() {
	local f=$1 zero_ok=$2
	{
		"$OBJDUMP" -d -M 7400 "$f" | awk -v f="$f" -v zok="$zero_ok" -F'\t' '
			/^[0-9a-f]+ <.*>:$/ { fn = $0; sub(/^[0-9a-f]+ /, "", fn); next }
			NF >= 3 {
				insn = $3; for (i = 4; i <= NF; i++) insn = insn " " $i
				if (insn ~ /^\.long/) {
					if (zok && insn ~ /^\.long 0x0$/) next
					print f ": " fn " " insn " (not a 7400 instruction)"
				} else if (insn ~ /^(fsqrts?|fsqrts?\.|mfocrf|mtocrf|popcntb|fre|frsqrtes)[ \t]/) {
					print f ": " fn " " insn " (not on the G4)"
				} else if (insn ~ /^cmp(l|i|li)?[ \t]+cr[0-7],1,/) {
					print f ": " fn " " insn " (64-bit compare)"
				}
			}'
		"$OBJDUMP" -d -M xenon "$f" | awk -v f="$f" -F'\t' '
			/^[0-9a-f]+ <.*>:$/ { fn = $0; sub(/^[0-9a-f]+ /, "", fn); next }
			NF >= 3 { split($3, w, " "); if (w[1] ~ /128$/) print f ": " fn " " $3 " (VMX128)" }'
	}
}

echo "== qemu-check 1/5: instruction scan (no 64-bit / VMX128 / non-G4 instructions)"
nobj=0
problems=$(
	for f in "${OBJS[@]}"; do scan "$f" 0; done
	scan "$QEMU_ELF" 1
	scan "$QEMU_ST_ELF" 1
)
nobj=$((${#OBJS[@]} + 2))
ninsn=$("$OBJDUMP" -d "$QEMU_ELF" | grep -cE '^ +[0-9a-f]+:')
if [ -n "$problems" ]; then
	printf '%s\n' "$problems" | head -40
	echo "FAIL: $(printf '%s\n' "$problems" | wc -l) suspicious instructions"
	fail=1
else
	echo "ok: $nobj objects/images clean ($ninsn instructions in $(basename "$QEMU_ELF"))"
fi

# ---- 2. helper calls of the 32-bit kernel code ------------------------------
echo "== qemu-check 2/5: libgcc/libc calls of the 32-bit kernel objects (informational)"
for f in "${OBJS[@]}"; do
	case $f in */table.o) continue ;; */xenon3/*.o | */old16/*.o) ;; *) continue ;; esac
	v=$(basename "$(dirname "$f")")
	k=$(basename "$f" .o)
	"$NM" -u "$f" | awk -v v="$v" -v k="$k" '{print $NF, k "(" v ")"}'
done | awk '
	$1 ~ /^(xenon3|old16)_(lib|k)_/ { next }
	{ who[$1] = who[$1] " " $2 }
	END { for (s in who) print s ":" who[s] }' | sort | sed 's/^/  /' |
	awk '{print}
	     /^  __/ {h=1}
	     /^  (memcpy|memmove|memset|memcmp):/ {m=1}
	     /^  sqrtf?:/ {q=1}
	     /^  [a-z]/ && !/^  (memcpy|memmove|memset|memcmp|sqrtf?):/ {o=1}
	     END {
		if (h) print "  (__*: libgcc helpers for 64-bit operations, from tools/qemu/qlibgcc.c)"
		if (m) print "  (mem*: block copies/clears GCC emits with the 32-bit flags; the real build inlines them, kcheck has the final word)"
		if (q) print "  (sqrt/sqrtf: every square root, the G4 has no fsqrt; correctly rounded in tools/qemu/qmath.c)"
		if (o) print "  (other plain names: not expected in kernels, see DESIGN.md rule 6)"
		if (!(h || m || q || o)) print "  (none)"
	     }'

# ---- 3. helper self-test ------------------------------------------------------
echo "== qemu-check 3/5: helper self-test (qemu, both compilers) vs host reference"
st_qemu=$("${RUN[@]}" "$QEMU_ST_ELF" 2>&1)
st_rc=$?
st_host=$("$QEMU_HOST_ST" 2>&1)
if [ $st_rc -ne 0 ]; then
	printf '%s\n' "$st_qemu"
	echo "FAIL: qselftest.elf exited with $st_rc"
	fail=1
else
	ref=$(printf '%s\n' "$st_host" | sed -n 's/^host //p')
	st_bad=0
	for ns in xenon3 old16; do
		got=$(printf '%s\n' "$st_qemu" | sed -n "s/^$ns //p")
		if [ "$got" != "$ref" ]; then
			echo "FAIL: $ns helper results differ from the host:"
			diff <(printf '%s\n' "$ref") <(printf '%s\n' "$got") | sed 's/^/  /'
			st_bad=1
		fi
	done
	if [ $st_bad -eq 0 ]; then
		echo "ok: $(printf '%s\n' "$ref" | wc -l) helper groups identical to the host for xenon3 and old16" \
			"($(printf '%s\n' "$ref" | awk '{print $1}' | tr '\n' ' '))"
	else
		fail=1
	fi
fi

# ---- 4. the runner ---------------------------------------------------------------
echo "== qemu-check 4/5: kernels under $QEMU_BIN -cpu $QEMU_CPU (A = xenon3, B = old16)"
log=$QEMU_OUT/qemu-run.txt
"${RUN[@]}" "$QEMU_ELF" -m "${EXTRA[@]}" >"$log" 2>&1
rc=$?
grep -v '^@R ' "$log"

# Known toolchain defects: "id flags A-bits B-bits reason" per line.  Every
# failing kernel (the runner's "failed:" list and its @R lines) must match an
# entry field for field; anything else, or differing tables, fails.
known_failures() {
	local id kind tol a b fl kid kfl ka kb why
	local -a xfail=() unexpected=() xpass=()
	declare -A kflags=() ka_=() kb_=() kwhy=() isx=()
	if [ -f "$QEMU_KNOWN" ]; then
		while read -r kid kfl ka kb why; do
			case $kid in '' | '#'*) continue ;; esac
			kflags[$kid]=$kfl ka_[$kid]=$ka kb_[$kid]=$kb kwhy[$kid]=$why
		done <"$QEMU_KNOWN"
	fi
	while read -r _ id kind tol a b fl; do
		if [ "$fl" = 0000 ]; then
			[ -n "${kflags[$id]:-}" ] && xpass+=("$id")
		elif [ "${kflags[$id]:-}" = "$fl" ] && [ "${ka_[$id]}" = "$a" ] &&
			[ "${kb_[$id]}" = "$b" ]; then
			xfail+=("$id")
			isx[$id]=1
		else
			unexpected+=("$id")
		fi
	done < <(grep '^@R ' "$log")
	# kernels without an @R line (skipped: unusable descriptor) are failures
	for id in $(sed -n 's/^failed://p' "$log"); do
		[ -n "${isx[$id]:-}" ] || case " ${unexpected[*]} " in
			*" $id "*) ;;
			*) unexpected+=("$id") ;;
			esac
	done
	grep -q 'TABLES DIFFER' "$log" && unexpected+=("(A/B kernel tables differ)")
	for id in "${xfail[@]}"; do
		echo "XFAIL $id: known toolchain defect (tools/qemu/known-failures.txt): ${kwhy[$id]}"
	done
	for id in "${xpass[@]}"; do
		echo "XPASS $id: listed in tools/qemu/known-failures.txt but passes now (stale entry?)"
	done
	if [ ${#unexpected[@]} -gt 0 ]; then
		echo "unexpected failures: ${unexpected[*]}"
		return 1
	fi
	[ ${#xfail[@]} -gt 0 ] # exit status 1 without any failing kernel: a failure
}

case $rc in
0) known_failures | grep '^XPASS' || true ;;
1) known_failures || fail=1 ;;
*)
	fail=1
	if [ $rc -eq 124 ]; then
		echo "*** the runner timed out after ${QEMU_TIMEOUT}s"
	else
		echo "*** the runner exited with status $rc"
	fi
	echo "*** running every kernel on its own:"
	ids=$("${RUN[@]}" "$QEMU_ELF" -l 2>/dev/null)
	: >"$log.iso"
	n_ok=0 bad=()
	for id in $ids; do
		"${RUN[@]}" "$QEMU_ELF" -m -k "$id" "${EXTRA[@]}" >"$log.one" 2>&1
		r=$?
		grep -E '^\[|^\*\*\*' "$log.one"
		case $r in
		0) n_ok=$((n_ok + 1)) ;;
		1) bad+=("$id") ;;
		*) bad+=("$id(exit $r)"); echo "    ($id: exit status $r)" ;;
		esac
		grep '^@R ' "$log.one" >>"$log.iso"
	done
	echo "isolated runs: $((n_ok + ${#bad[@]})) kernels, $n_ok ok, ${#bad[@]} failed: ${bad[*]:-}"
	cat "$log.iso" >>"$log"
	rm -f "$log.one" "$log.iso"
	;;
esac

# ---- 5. BE/LE cross-check ----------------------------------------------------------
echo "== qemu-check 5/5: BE/LE cross-check (informational)"
if [ -n "$QEMU_HOST_RUNNER" ] && [ -x "$QEMU_HOST_RUNNER" ]; then
	hlog=$QEMU_OUT/host-run.txt
	"$QEMU_HOST_RUNNER" -c "$log" >"$hlog" 2>&1
	sed -n 's/^summary:/host run summary:/p' "$hlog"
	sed -n '/^BE\/LE cross-check/,$p' "$hlog"
elif [ -n "$QEMU_HOST_RUNNER" ]; then
	echo "skipped: the host build of the runner failed (see the messages above)"
else
	echo "skipped (QEMU_XCHECK=0)"
fi

if [ $fail -eq 0 ]; then
	echo "qemu-check: PASS"
else
	echo "qemu-check: FAIL"
fi
exit $fail
