#!/bin/bash
# Build libdrgposter.dylib (tools/ios/drgposter.c: PosterBoard's home-screen preview on the CPU) for iOS 16
# (run in WSL). As gpu/drgles/build-eagl.sh: no SDK, clang-21 + ld64.lld, link stubs written from the
# library's own imports (install names and symbol names only): libSystem, libobjc, Metal. Plain arm64,
# ARMv8.0 only, ad hoc signed.
#   tools/ios/build-drgposter.sh OUT_DIR
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=$1
OBJ=$OUT/obj-drgposter
LIB=$OUT/libdrgposter.dylib
rm -rf "$OBJ"
mkdir -p "$OBJ"
clang-21 --target=arm64-apple-ios16.0 -O2 -g -ffreestanding -fblocks -fno-stack-protector -fvisibility=hidden \
	-Wall -Wextra -Werror -c "$SRC/drgposter.c" -o "$OBJ/drgposter.o"

llvm-nm-21 -u "$OBJ/drgposter.o" | awk 'NF{print $NF}' | sort -u > "$OBJ/imports"
grep -E '^_(objc_|sel_|class_|method_|object_)' "$OBJ/imports" > "$OBJ/objc" || true
grep -E '^_MTL' "$OBJ/imports" > "$OBJ/metal" || true
grep -v -x -F -f "$OBJ/objc" -f "$OBJ/metal" "$OBJ/imports" > "$OBJ/libsystem"
[ -s "$OBJ/metal" ] || { echo "error: MTLCreateSystemDefaultDevice not imported" >&2; exit 1; }

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
stub objc /usr/lib/libobjc.A.dylib "$OBJ/objc"
stub Metal /System/Library/Frameworks/Metal.framework/Metal "$OBJ/metal"

ld64.lld-21 -arch arm64 -platform_version ios 16.0 16.0 -dylib -install_name /usr/local/lib/libdrgposter.dylib \
	-o "$LIB" -adhoc_codesign -dead_strip "$OBJ/drgposter.o" "$OBJ/libSystem.tbd" "$OBJ/objc.tbd" "$OBJ/Metal.tbd"

# checks: the dylibs it loads, a dylib, iOS 16 arm64 (not arm64e), signed, ARMv8.0 only, an initialiser
llvm-otool-21 -L "$LIB"
llvm-otool-21 -hv "$LIB" | grep -q 'ARM64 *ALL *0x00 *DYLIB' || { echo "error: not a plain arm64 dylib" >&2; exit 1; }
llvm-otool-21 -l "$LIB" | grep -q LC_CODE_SIGNATURE || { echo "error: not signed" >&2; exit 1; }
llvm-otool-21 -l "$LIB" | grep -A4 LC_BUILD_VERSION | grep -q 'minos 16.0' || { echo "error: not iOS 16" >&2; exit 1; }
llvm-otool-21 -l "$LIB" | grep -q -E 'sectname __mod_init_func|sectname __init_offsets' ||
	{ echo "error: no initialiser" >&2; exit 1; }
PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=50 MIN_EXCLUSIVES=0 bash "$SRC/../check-armv80.sh" "$LIB"
echo "imports: $(paste -sd' ' "$OBJ/imports")"
sha256sum "$LIB"
