#!/bin/bash
# Boot image #62 (run in WSL): kernel CI 45854091163 (+0922 touch fingers 2+), #60's ADT and boot-args, the same shim as #60
# (hardware tests off). Differs from #60 only in the kernel.
# 60 Hz panel kept (drgfb_hz=60, in the boot-args).
#   drg-make-boot62.sh KERNEL
set -euo pipefail
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
OUT=$REPO/shim/out-drg
KERNEL=${1:?usage: drg-make-boot62.sh KERNEL}
W=/root/boot62
rm -rf $W && mkdir -p $W
python3 - $OUT/payload-boot60.bin $W <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
off, size = struct.unpack_from('<QQ', b, 0x20)
open(sys.argv[2] + '/adt.bin', 'wb').write(b[off:off + size])
open(sys.argv[2] + '/cmdline', 'w').write(b[0x48:0x48 + 608].split(b'\0')[0].decode())
PY
cmp $W/adt.bin $OUT/drg.adt
CMD="$(cat $W/cmdline)"	# #60's boot-args (= #58's)
case "$CMD" in *drgusb=2*) ;; *) echo "error: #60 boot-args without drgusb=2" >&2; exit 1;; esac
echo "boot-args: $CMD"
python3 $REPO/tools/mkpayload.py "$KERNEL" $W/adt.bin $OUT/payload-boot62.bin --board drg --cmdline "$CMD"
python3 - $OUT/payload-boot62.bin "$CMD" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'NOKIAXNU' and b[0x48:0x48 + 608].split(b'\0')[0].decode() == sys.argv[2]
print('payload 62: magic and boot-args OK')
PY
cd $REPO
SHIM_CFLAGS= bash shim/build-drg.sh /mnt/f/Nokia-HackiOs/backup/boot_b.img $OUT/payload-boot62.bin > $W/build.log 2>&1
grep -iE 'warning|error' $W/build.log && { echo "build warnings/errors" >&2; exit 1; } || true
cmp $OUT/payload-boot62.bin $OUT/ramdisk.bin
cp $OUT/shim-drg-boot.img $OUT/shim-drg-boot-62.img
strings $OUT/shim.elf | grep -c '^slots' | sed 's/^/slot strings: /'
ls -la $OUT/shim-drg-boot-62.img
sha256sum $OUT/payload-boot62.bin $OUT/shim-drg-boot-62.img
