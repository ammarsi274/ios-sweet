#!/bin/bash
# Build the drgles bench as an iOS 16 Mach-O for the Nokia 6.1 Plus (run in WSL).
#   build-ios.sh OUT
# No Mac and no SDK: clang-21 + ld64.lld. The libraries it links against are
# described by link stubs written here from the program's own imports (text
# .tbd files: install name + symbol names, nothing of Apple's): libSystem,
# libz (zlib is its own dylib on iOS) and IOKit (a shallow framework on iOS).
# lld signs the result ad hoc. Plain arm64 (the iPhone X userland), ARMv8.0
# only: the SDM636's cores have no LSE, and clang's apple-a7 default is v8.0.
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=$1
OBJ=$OUT/obj
rm -rf "$OBJ"
mkdir -p "$OBJ"
CC="clang-21 --target=arm64-apple-ios16.0 -O2 -g -ffreestanding -fno-stack-protector -Wall -Wextra -Werror -Wno-unused-parameter"
for f in bench.c elf.c host_darwin.c kgsl_ios.c bionic/*.c bionic/vararg.S; do
	$CC -c "$SRC/$f" -o "$OBJ/$(echo "$f" | tr / _).o"
done

# imports: everything undefined in our objects
llvm-nm-21 -u "$OBJ"/*.o | grep -v ':$' | awk 'NF{print $NF}' | sort -u > "$OBJ/undef"
llvm-nm-21 -g --defined-only "$OBJ"/*.o | grep -v ':$' | awk 'NF{print $NF}' | sort -u > "$OBJ/def"
comm -23 "$OBJ/undef" "$OBJ/def" > "$OBJ/imports"
grep -E '^_IO' "$OBJ/imports" > "$OBJ/iokit" || true
grep -E '^_(compress|compress2|compressBound|uncompress|uncompress2|crc32|adler32|deflate.*|inflate.*|zlibVersion)$' \
	"$OBJ/imports" > "$OBJ/libz" || true
grep -v -x -F -f "$OBJ/iokit" -f "$OBJ/libz" "$OBJ/imports" > "$OBJ/libsystem"

stub() {	# stub NAME INSTALL_NAME SYMBOL_FILE
	{
		echo '--- !tapi-tbd'
		echo 'tbd-version: 4'
		echo 'targets: [ arm64-ios ]'
		echo "install-name: '$2'"
		echo 'exports:'
		echo '  - targets: [ arm64-ios ]'
		echo "    symbols: [ $(paste -sd, "$3" | sed 's/,/, /g') ]"
		echo '...'
	} > "$OBJ/$1.tbd"
}
stub libSystem /usr/lib/libSystem.B.dylib "$OBJ/libsystem"
stub libz /usr/lib/libz.1.dylib "$OBJ/libz"
stub IOKit /System/Library/Frameworks/IOKit.framework/IOKit "$OBJ/iokit"

ld64.lld-21 -arch arm64 -platform_version ios 16.0 16.0 -o "$OUT/drgles-bench" \
	-adhoc_codesign -dead_strip "$OBJ"/*.o "$OBJ/libSystem.tbd" "$OBJ/libz.tbd" "$OBJ/IOKit.tbd"

# checks: the dylibs it loads, an LC_MAIN entry, iOS 16 arm64 (not arm64e), signed, ARMv8.0 only
llvm-otool-21 -L "$OUT/drgles-bench"
llvm-otool-21 -hv "$OUT/drgles-bench" | grep -q 'ARM64 *ALL' || { echo "error: not plain arm64" >&2; exit 1; }
llvm-otool-21 -l "$OUT/drgles-bench" | grep -q LC_MAIN || { echo "error: no LC_MAIN" >&2; exit 1; }
llvm-otool-21 -l "$OUT/drgles-bench" | grep -q LC_CODE_SIGNATURE || { echo "error: not signed" >&2; exit 1; }
llvm-otool-21 -l "$OUT/drgles-bench" | grep -A4 LC_BUILD_VERSION | grep -q 'minos 16.0' || { echo "error: not iOS 16" >&2; exit 1; }
PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash "$SRC/../../tools/check-armv80.sh" "$OUT/drgles-bench"
echo "imports: $(wc -l < "$OBJ/libsystem") libSystem, $(wc -l < "$OBJ/libz") libz, $(wc -l < "$OBJ/iokit") IOKit"
shasum -a 256 "$OUT/drgles-bench" 2>/dev/null || sha256sum "$OUT/drgles-bench"
