#!/bin/bash
# Boot image #54 (run in WSL): the current shim (slots, Volume Up window, charger screen, power off) built by
# build-drg.sh with a payload of the new kernel, #50's ADT and #50's boot-args plus drgusb=2 (USB
# device + ECM networking, patch 0918). 60 Hz panel kept (drgfb_hz=60, the user's requirement).
#   drg-make-boot54.sh KERNEL
set -euo pipefail
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
OUT=$REPO/shim/out-drg
KERNEL=${1:?usage: drg-make-boot54.sh KERNEL}
W=/root/boot54
rm -rf $W && mkdir -p $W
python3 - $OUT/payload-boot50.bin $W <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
off, size = struct.unpack_from('<QQ', b, 0x20)
open(sys.argv[2] + '/adt.bin', 'wb').write(b[off:off + size])
open(sys.argv[2] + '/cmdline', 'w').write(b[0x48:0x48 + 608].split(b'\0')[0].decode())
PY
cmp $W/adt.bin $OUT/drg.adt
CMD="$(cat $W/cmdline) drgusb=2"
echo "boot-args: $CMD"
python3 $REPO/tools/mkpayload.py "$KERNEL" $W/adt.bin $OUT/payload-boot54.bin --board drg --cmdline "$CMD"
python3 - $OUT/payload-boot54.bin "$CMD" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'NOKIAXNU' and b[0x48:0x48 + 608].split(b'\0')[0].decode() == sys.argv[2]
print('payload 54: magic and boot-args OK')
PY
cd $REPO
bash shim/build-drg.sh /mnt/f/Nokia-HackiOs/backup/boot_b.img $OUT/payload-boot54.bin > $W/build.log 2>&1
grep -iE 'warning|error' $W/build.log && { echo "build warnings/errors" >&2; exit 1; } || true
cmp $OUT/payload-boot54.bin $OUT/ramdisk.bin
cp $OUT/shim-drg-boot.img $OUT/shim-drg-boot-54.img
strings $OUT/shim.elf | grep -c '^slots' | sed 's/^/slot strings: /'
ls -la $OUT/shim-drg-boot-54.img
sha256sum $OUT/payload-boot54.bin $OUT/shim-drg-boot-54.img
