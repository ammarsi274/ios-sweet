#!/bin/bash
# Build root image r12 = r11 + a newer GPU bench (diagnostics after boot #32) (drg-root-gpubench.py), its chunk
# list for drg-flash-diff.sh, and check it (run in WSL as root). Flashing is a
# separate step (tools/drg-flash-diff.sh), done only with the user's OK.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=${1:?usage: drg-make-r12.sh BENCH (gpu/drgles/build-ios.sh output)}
LIBS=/mnt/f/Nokia-HackiOs/research/vendor-adreno/files/lib64
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r12 2>/dev/null || true
echo "r11: $(sha256sum ios16-root-r11.img | cut -c1-16)"
cp --sparse=always ios16-root-r11.img ios16-root-r12.img
mkdir -p /mnt/r12
mount -t hfsplus -o loop,force ios16-root-r12.img /mnt/r12
python3 $REPO/tools/drg-root-gpubench.py /mnt/r12 "$BENCH" "$LIBS"
cmp "$BENCH" /mnt/r12/usr/local/bin/drgles-bench
ls -la /mnt/r12/usr/local/bin /mnt/r12/usr/local/drgles/adreno | head -30
umount /mnt/r12
# r11 (flashed and booted) gets "Volume header needs minor repair" from this fsck; r12 must give r11's exact report
FB=$(fsck.hfsplus -fn ios16-root-r11.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r12.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r12 differs from r11's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r11"
/root/chunkhash ios16-root-r12.img 16384 > ios16-root-r12.chunks
echo "r12: $(sha256sum ios16-root-r12.img | cut -c1-64)"
echo "chunks differing from r11: $(diff <(cut -d' ' -f2 ios16-root-r11.chunks) <(cut -d' ' -f2 ios16-root-r12.chunks) | grep -c '^>')"
