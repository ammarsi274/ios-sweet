#!/bin/bash
# Fail if a kernel contains instructions that ARMv8.0 cores (Nokia 6.1 Plus,
# Kryo 260 = Cortex-A73/A53 class) do not implement: the first one executed
# is an undefined-instruction exception. Scans every executable section.
#
# Usage: tools/check-armv80.sh KERNEL
# Needs llvm-objdump (Linux: apt install llvm; macOS: xcrun objdump works).
set -euo pipefail
K=$1
if command -v llvm-objdump >/dev/null; then
	OBJDUMP=llvm-objdump
elif command -v xcrun >/dev/null; then
	OBJDUMP="xcrun objdump"
else
	echo "check-armv80: need llvm-objdump" >&2
	exit 2
fi
DIS=$(mktemp)
trap 'rm -f "$DIS"' EXIT
# decode every newer extension so nothing hides as an unknown word
$OBJDUMP -d --no-show-raw-insn \
	--mattr=+v8.5a,+lse,+rcpc,+dotprod,+fullfp16,+sha3,+sm4,+rdm,+pauth,+mte,+sb,+ssbs,+predres \
	"$K" > "$DIS" 2>/dev/null
lines=$(wc -l < "$DIS" | tr -d ' ')
# a kernel disassembles to ~2 million lines; MIN_LINES lowers this for tests
[ "$lines" -gt "${MIN_LINES:-100000}" ] || { echo "check-armv80: only $lines lines disassembled from $K" >&2; exit 2; }

T=$'\t'
FORBIDDEN=(
	"LSE atomics|${T}(casp?|caspa|caspl|caspal|casa|casl|casal)[bh]?${T}|${T}(ld|st)(add|clr|eor|set|smax|smin|umax|umin)(a|l|al)?[bh]?${T}|${T}swp(a|l|al)?[bh]?${T}"
	"LRCPC (ldapr)|${T}ldapr[bh]?${T}|${T}ldapur"
	"PAN/UAO/DIT/SSBS/TCO|${T}msr${T}(pan|uao|dit|ssbs|tco),"
	"DC CVAP/CVADP|${T}dc${T}(cvap|cvadp),"
	"RDM|${T}sqrdml[as]h${T}"
	"dot product|${T}[su]dot${T}"
	"SHA3/SHA512/SM3/SM4|${T}(eor3|rax1|xar|bcax|sha512h2?|sha512su[01]|sm3[a-z0-9]*|sm4[a-z]*)${T}"
	"PAC (non-hint)|${T}(ldra[ab]|bra[ab]|blra[ab]|reta[ab]|ereta[ab]|pacga|xpac[id]|pac(ia|ib|da|db)|aut(ia|ib|da|db))${T}"
	"SB barrier|${T}sb$"
	"CFP/DVP/CPP RCTX|${T}(cfp|dvp|cpp)${T}"
	"FP16 arithmetic|${T}(fadd|fmul|fsub|fmla|fmls|fdiv|fsqrt)${T}(h[0-9]|v[0-9]+\\.[48]h)"
)
bad=0
for entry in "${FORBIDDEN[@]}"; do
	name=${entry%%|*} re=${entry#*|}
	n=$(grep -cE "$re" "$DIS" || true)
	printf '%-22s %6d\n' "$name" "$n"
	if [ "$n" != 0 ]; then
		bad=1
		grep -m3 -nE "$re" "$DIS" | sed 's/^/    /'
	fi
done
# sanity: plain ARMv8.0 exclusives must be there, else the scan saw nothing
ex=$(grep -cE "${T}(ldaxr|stlxr|ldxr|stxr)${T}" "$DIS" || true)
printf '%-22s %6d (expected > 0)\n' "ldxr/stxr exclusives" "$ex"
[ "$ex" -gt 0 ] || { echo "check-armv80: no exclusives found, scan is broken" >&2; exit 2; }
if [ "$bad" = 0 ]; then
	echo "check-armv80: OK, no instructions beyond ARMv8.0 (+crc, crypto) in $K ($lines lines)"
else
	echo "check-armv80: FAILED, this kernel would crash on ARMv8.0 cores" >&2
fi
exit "$bad"
