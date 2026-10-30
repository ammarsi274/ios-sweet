#!/bin/bash
# Build gles-probe and libsync.so for TWRP on the Nokia 6.1 Plus (run in WSL).
#   build.sh SYSLIBS OUT
# SYSLIBS: TWRP's /sbin/libc.so and /sbin/libdl.so pulled from the phone
# (link inputs only, never shipped). No NDK: clang-21 + lld, bionic ABI.
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
SYS=$1
OUT=$2
mkdir -p "$OUT"
CC="clang-21 --target=aarch64-linux-android29 -O2 -fPIC -fno-stack-protector -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld -Wall -Werror"
$CC -shared -Wl,-soname,libsync.so -o "$OUT/libsync.so" "$SRC/libsync.c" "$SYS/libc.so"
$CC -pie -Wl,--export-dynamic -Wl,--dynamic-linker=/sbin/linker64 -Wl,-z,max-page-size=4096 \
	-Wl,--hash-style=both -Wl,-z,noexecstack -Wl,-e,_start \
	-o "$OUT/gles-probe" "$SRC/gles-probe.c" "$SYS/libc.so" "$SYS/libdl.so"
llvm-readelf -d "$OUT/gles-probe" | grep -E 'NEEDED|FLAGS'
llvm-nm -D --defined-only "$OUT/gles-probe" | awk '{print $3}' | tr '\n' ' '; echo
llvm-nm -D --defined-only "$OUT/libsync.so" | awk '{print $3}' | tr '\n' ' '; echo
$CC -shared -Wl,-soname,android.hardware.graphics.common@1.1.so -o "$OUT/android.hardware.graphics.common@1.1.so" "$SRC/empty-hidl.c"
