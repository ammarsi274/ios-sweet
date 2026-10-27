#!/bin/bash
# Build root image r8 (pristine + drg-root-jobs.py), diff it against the phone
# copy after boot #17, and check that phone + diff == r8 before anything is flashed.
set -e
cd /root/ipsw
OLD=/mnt/f/Nokia-HackiOs/ios-root/phone-after-boot17.img
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r7 /mnt/r8 2>/dev/null || true
echo "pristine: $(sha256sum ios16-root.img | cut -c1-16)"
cp --sparse=always ios16-root.img ios16-root-r8.img
mkdir -p /mnt/r8
mount -t hfsplus -o loop,force ios16-root-r8.img /mnt/r8
python3 /mnt/f/Nokia-HackiOs/ios-sweet/tools/drg-root-jobs.py /mnt/r8
umount /mnt/r8
fsck.hfsplus -fn ios16-root-r8.img 2>&1 | tail -2
R8=$(sha256sum ios16-root-r8.img | cut -c1-64); echo "r8: $R8"
python3 /mnt/f/Nokia-HackiOs/ios-sweet/tools/sparsediff.py $OLD ios16-root-r8.img ios16-r8-from-boot17.simg
python3 - "$OLD" ios16-r8-from-boot17.simg "$R8" <<'PY'
import struct, hashlib, sys
old = open(sys.argv[1], 'rb', buffering=0); f = open(sys.argv[2], 'rb')
magic, maj, mn, fhs, chs, bs, blocks, chunks, _ = struct.unpack('<IHHHHIIII', f.read(28)); f.seek(fhs)
h = hashlib.sha256(); pos = 0
for _ in range(chunks):
    ct, _, n, tot = struct.unpack('<HHII', f.read(chs))
    if ct == 0xcac1:
        h.update(f.read(n * bs))
    else:
        left = n * bs; old.seek(pos * bs)
        while left:
            b = old.read(min(1 << 20, left)); h.update(b); left -= len(b)
    pos += n
print('phone+diff', h.hexdigest(), 'OK' if h.hexdigest() == sys.argv[3] else 'MISMATCH')
PY
cp ios16-r8-from-boot17.simg /mnt/f/Nokia-HackiOs/ios-root/
sha256sum /mnt/f/Nokia-HackiOs/ios-root/ios16-r8-from-boot17.simg
