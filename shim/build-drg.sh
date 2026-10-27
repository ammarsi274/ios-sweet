#!/bin/bash
# Build the shim for the Nokia 6.1 Plus (DRG) and wrap it the way the stock
# kernel is packed: boot header v0, Image.gz with the stock DTBs appended.
# Needs the stock boot image (from the partition backup) as $1.
set -euo pipefail
cd "$(dirname "$0")"
STOCK=${1:?usage: build-drg.sh path/to/stock-boot.img [ramdisk]}
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -DBOARD_DRG"
OUT=out-drg
mkdir -p $OUT
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c emmc.c; do
	$CC $CFLAGS -c $f -o $OUT/${f%.*}.o
done
abs=$(aarch64-linux-gnu-readelf -r $OUT/shim.o $OUT/console.o $OUT/mmu.o $OUT/fdt.o $OUT/loader.o $OUT/ufs.o $OUT/emmc.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$OUT/start.o $OUT/shim.o $OUT/console.o $OUT/mmu.o $OUT/fdt.o $OUT/loader.o $OUT/ufs.o $OUT/emmc.o -o $OUT/shim.elf
$OBJCOPY -O binary $OUT/shim.elf $OUT/Image
size=$(aarch64-linux-gnu-nm $OUT/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" $OUT/Image
python3 ../tools/extract-appended-dtbs.py "$STOCK" $OUT/stock-dtbs.bin
gzip -9 -n -c $OUT/Image > $OUT/Image.gz
cat $OUT/Image.gz $OUT/stock-dtbs.bin > $OUT/Image.gz-dtb
if [ -n "${2:-}" ]; then
	cp "$2" $OUT/ramdisk.bin
else
	(cd $OUT && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
mkbootimg --header_version 0 --kernel $OUT/Image.gz-dtb --ramdisk $OUT/ramdisk.bin \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
	--pagesize 4096 --cmdline "console=null" -o $OUT/shim-drg-boot.img
ls -l $OUT/Image $OUT/shim-drg-boot.img
sha256sum $OUT/shim-drg-boot.img
