#!/bin/bash
# Build a small C tool as an iOS 16 Mach-O on the PC (run in WSL), the way
# gpu/drgles/build-ios.sh builds the bench: clang-21 + ld64.lld, a libSystem
# link stub written from the tool's own imports, ad hoc signature, checks.
#   tools/ios/build.sh OUT SOURCE.c...
set -euo pipefail
OUT=$1
shift
OBJ=$OUT.obj
rm -rf "$OBJ"
mkdir -p "$OBJ"
CC="clang-21 --target=arm64-apple-ios16.0 -O2 -ffreestanding -fno-stack-protector -Wall -Wextra -Werror"
for f in "$@"; do
	$CC -c "$f" -o "$OBJ/$(basename "$f").o"
done
llvm-nm-21 -u "$OBJ"/*.o | grep -v ':$' | awk 'NF{print $NF}' | sort -u > "$OBJ/imports"
{
	echo '--- !tapi-tbd'
	echo 'tbd-version: 4'
	echo 'targets: [ arm64-ios ]'
	echo "install-name: '/usr/lib/libSystem.B.dylib'"
	echo 'exports:'
	echo '  - targets: [ arm64-ios ]'
	echo "    symbols: [ $(paste -sd, "$OBJ/imports" | sed 's/,/, /g') ]"
	echo '...'
} > "$OBJ/libSystem.tbd"
ld64.lld-21 -arch arm64 -platform_version ios 16.0 16.0 -o "$OUT" -adhoc_codesign -dead_strip \
	"$OBJ"/*.o "$OBJ/libSystem.tbd"
llvm-otool-21 -L "$OUT" | tail -n +2
llvm-otool-21 -l "$OUT" | grep -q LC_CODE_SIGNATURE || { echo "error: not signed" >&2; exit 1; }
PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=10 MIN_EXCLUSIVES=0 bash "$(dirname "$0")/../check-armv80.sh" "$OUT" | tail -1
echo "imports: $(paste -sd' ' "$OBJ/imports")"
sha256sum "$OUT"
