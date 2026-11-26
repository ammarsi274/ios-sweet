#!/bin/bash
# Build rect-probe for TWRP (run in WSL): the rectangle-texture rewrite from eagl.c, tested on Adreno.
#   build-rect.sh SYSLIBS OUT    (SYSLIBS: TWRP's libc.so and libdl.so, link inputs only)
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
SYS=$1
OUT=$2
mkdir -p "$OUT"
E=$SRC/../drgles/eagl.c
a=$(grep -n '^static int ident_char' "$E" | cut -d: -f1)
b=$(grep -n '^/\* after a link' "$E" | cut -d: -f1)
[ -n "$a" ] && [ -n "$b" ] || { echo "error: rect code not found in eagl.c" >&2; exit 1; }
sed -n "${a},$((b - 1))p" "$E" > "$OUT/rect_gen.h"
CC="clang-21 --target=aarch64-linux-android29 -O2 -fPIC -fno-stack-protector -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld -Wall -Werror"
$CC -I"$OUT" -pie -Wl,--export-dynamic -Wl,--dynamic-linker=/sbin/linker64 -Wl,-z,max-page-size=4096 \
	-Wl,--hash-style=both -Wl,-z,noexecstack -Wl,-e,_start \
	-o "$OUT/rect-probe" "$SRC/rect-probe.c" "$SYS/libc.so" "$SYS/libdl.so"
llvm-readelf -d "$OUT/rect-probe" | grep -E 'NEEDED'
