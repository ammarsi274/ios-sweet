#!/bin/bash
# Boot image #51 (run in WSL): #50's image with only its payload replaced. The shim part (kernel
# Image.gz + stock DTBs) is taken from shim-drg-boot-50.img as it is (shim 7f33117, booted as #49), so
# the shim stays byte-identical; the payload is the new kernel with #50's ADT and boot-args.
#   drg-make-boot51.sh KERNEL
set -euo pipefail
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
OUT=$REPO/shim/out-drg
KERNEL=${1:?usage: drg-make-boot51.sh KERNEL}
W=/root/boot51
rm -rf $W && mkdir -p $W
# #50's ADT and boot-args, read back out of its payload
python3 - $OUT/payload-boot50.bin $W <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
off, size = struct.unpack_from('<QQ', b, 0x20)
open(sys.argv[2] + '/adt.bin', 'wb').write(b[off:off + size])
open(sys.argv[2] + '/cmdline', 'w').write(b[0x48:0x48 + 608].split(b'\0')[0].decode())
PY
cmp $W/adt.bin $OUT/drg.adt
python3 $REPO/tools/mkpayload.py "$KERNEL" $W/adt.bin $OUT/payload-boot51.bin --board drg --cmdline "$(cat $W/cmdline)"
# the payload differs from #50's only in the kernel: same header fields after it, same ADT, same boot-args
python3 - $OUT/payload-boot50.bin $OUT/payload-boot51.bin <<'PY'
import struct, sys
a, b = (open(p, 'rb').read() for p in sys.argv[1:])
assert a[:8] == b[:8] == b'NOKIAXNU'
assert a[0x48:0x2a8] == b[0x48:0x2a8], 'boot-args differ'
for x in (a, b):
    o, s = struct.unpack_from('<QQ', x, 0x20)
    assert x[o:o + s] == open('/root/boot51/adt.bin', 'rb').read(), 'ADT differs'
print('payload 51: same magic, boot-args and ADT as 50')
PY
unpack_bootimg --boot_img $OUT/shim-drg-boot-50.img --out $W/u50 > $W/u50.txt
grep -E 'header version|page size|kernel load|ramdisk load|tags load|command line' $W/u50.txt
mkbootimg --header_version 0 --kernel $W/u50/kernel --ramdisk $OUT/payload-boot51.bin \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
	--pagesize 4096 --cmdline "console=null" -o $OUT/shim-drg-boot-51.img
# the repacked #50 (its own payload) must give #50 byte for byte: same arguments as build-drg.sh
mkbootimg --header_version 0 --kernel $W/u50/kernel --ramdisk $W/u50/ramdisk \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
	--pagesize 4096 --cmdline "console=null" -o $W/check50.img
cmp $W/check50.img $OUT/shim-drg-boot-50.img && echo "repack check: #50 rebuilt byte for byte"
cmp $W/u50/ramdisk $OUT/payload-boot50.bin && echo "#50's ramdisk is payload-boot50.bin"
sha256sum $OUT/payload-boot51.bin $OUT/shim-drg-boot-51.img
