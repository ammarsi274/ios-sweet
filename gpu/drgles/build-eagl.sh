#!/bin/bash
# Build libdrgeagl.dylib (eagl.c: OpenGL ES for backboardd's CoreAnimation) for iOS 16 (run in WSL).
#   build-eagl.sh OUT
# As build-ios.sh: no SDK, clang-21 + ld64.lld, link stubs written from the library's own imports
# (install names and symbol names only): libSystem, libz, IOKit, IOSurface, libobjc, and OpenGLES
# (its EAGLContext class). Plain arm64, ARMv8.0 only.
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=$1
OBJ=$OUT/obj-eagl
rm -rf "$OBJ"
mkdir -p "$OBJ"
CC="clang-21 --target=arm64-apple-ios16.0 -O2 -g -ffreestanding -fno-stack-protector -fvisibility=hidden -Wall -Wextra -Werror -Wno-unused-parameter"
cd "$SRC"
for f in eagl.c drgwin.c elf.c host_darwin.c kgsl_ios.c kgsl_cap.c bionic/*.c bionic/vararg.S; do
	$CC -c "$SRC/$f" -o "$OBJ/$(echo "$f" | tr / _).o"
done

llvm-nm-21 -u "$OBJ"/*.o | grep -v ':$' | awk 'NF{print $NF}' | sort -u > "$OBJ/undef"
llvm-nm-21 -g --defined-only "$OBJ"/*.o | grep -v ':$' | awk 'NF{print $NF}' | sort -u > "$OBJ/def"
comm -23 "$OBJ/undef" "$OBJ/def" > "$OBJ/imports"
grep -E '^_IOSurface' "$OBJ/imports" > "$OBJ/iosurface" || true
grep -E '^_IO' "$OBJ/imports" | grep -v -E '^_IOSurface' > "$OBJ/iokit" || true
grep -E '^_(objc_|sel_|class_|method_|object_)' "$OBJ/imports" > "$OBJ/objc" || true
grep -E '^_OBJC_CLASS_\$_EAGLContext$' "$OBJ/imports" > "$OBJ/opengles" || true
grep -E '^_(compress|compress2|compressBound|uncompress|uncompress2|crc32|adler32|deflate.*|inflate.*|zlibVersion)$' \
	"$OBJ/imports" > "$OBJ/libz" || true
grep -v -x -F -f "$OBJ/iokit" -f "$OBJ/iosurface" -f "$OBJ/objc" -f "$OBJ/opengles" -f "$OBJ/libz" \
	"$OBJ/imports" > "$OBJ/libsystem"
[ -s "$OBJ/opengles" ] || { echo "error: EAGLContext class not imported" >&2; exit 1; }

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
stub IOSurface /System/Library/Frameworks/IOSurface.framework/IOSurface "$OBJ/iosurface"
stub objc /usr/lib/libobjc.A.dylib "$OBJ/objc"
stub OpenGLES /System/Library/Frameworks/OpenGLES.framework/OpenGLES "$OBJ/opengles"

ld64.lld-21 -arch arm64 -platform_version ios 16.0 16.0 -dylib -install_name /usr/local/lib/libdrgeagl.dylib \
	-o "$OUT/libdrgeagl.dylib" -adhoc_codesign -dead_strip "$OBJ"/*.o \
	"$OBJ/libSystem.tbd" "$OBJ/libz.tbd" "$OBJ/IOKit.tbd" "$OBJ/IOSurface.tbd" "$OBJ/objc.tbd" "$OBJ/OpenGLES.tbd"

# checks: the dylibs it loads, a dylib, iOS 16 arm64 (not arm64e), signed, ARMv8.0 only, an initialiser
llvm-otool-21 -L "$OUT/libdrgeagl.dylib"
llvm-otool-21 -hv "$OUT/libdrgeagl.dylib" | grep -q 'ARM64 *ALL *0x00 *DYLIB' || { echo "error: not a plain arm64 dylib" >&2; exit 1; }
llvm-otool-21 -l "$OUT/libdrgeagl.dylib" | grep -q LC_CODE_SIGNATURE || { echo "error: not signed" >&2; exit 1; }
llvm-otool-21 -l "$OUT/libdrgeagl.dylib" | grep -A4 LC_BUILD_VERSION | grep -q 'minos 16.0' || { echo "error: not iOS 16" >&2; exit 1; }
llvm-otool-21 -l "$OUT/libdrgeagl.dylib" | grep -q -E 'sectname __mod_init_func|sectname __init_offsets' ||
	{ echo "error: no initialiser" >&2; exit 1; }
PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash "$SRC/../../tools/check-armv80.sh" "$OUT/libdrgeagl.dylib"
for f in libsystem libz iokit iosurface objc opengles; do printf '%s %s, ' "$(wc -l < "$OBJ/$f")" $f; done; echo
shasum -a 256 "$OUT/libdrgeagl.dylib" 2>/dev/null || sha256sum "$OUT/libdrgeagl.dylib"
