#!/bin/bash
# Build the drgles bench for TWRP on the Nokia 6.1 Plus (run in WSL).
#   build-bench.sh SYSLIBS OUT
# SYSLIBS: TWRP's /sbin/libc.so, libdl.so, libm.so, libz.so pulled from the
# phone (link inputs only, never shipped): the bench's host C library.
# No NDK: clang-21 + lld, bionic ABI. Atomics stay inline (ldxr/stxr): the
# SDM636's cores are ARMv8.0 without LSE, and there is no compiler-rt.
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
SYS=$1
OUT=$2
mkdir -p "$OUT"
CC="clang-21 --target=aarch64-linux-android29 -O2 -g -fPIC -fno-stack-protector -ffreestanding -nostdlib -fuse-ld=lld -mno-outline-atomics -Wall -Wextra -Werror -Wno-unused-parameter"
$CC -pie -Wl,--dynamic-linker=/sbin/linker64 -Wl,-z,max-page-size=4096 -Wl,--hash-style=both \
	-Wl,--export-dynamic-symbol=atrace_get_enabled_tags \
	-Wl,-z,noexecstack -Wl,-e,_start -o "$OUT/drgles-bench" \
	"$SRC/bench.c" "$SRC/elf.c" "$SRC/host_linux.c" "$SRC/kgsl_bench.c" "$SRC/kgsl_cap.c" \
	"$SRC"/bionic/*.c "$SRC/bionic/vararg.S" \
	"$SYS/libc.so" "$SYS/libdl.so" "$SYS/libm.so" "$SYS/libz.so"
llvm-readelf -d "$OUT/drgles-bench" | grep NEEDED
