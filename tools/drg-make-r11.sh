#!/bin/bash
# Build root image r11 = r10 + the GPU bench (drg-root-gpubench.py), its chunk
# list for drg-flash-diff.sh, and check it (run in WSL as root). Flashing is a
# separate step (tools/drg-flash-diff.sh), done only with the user's OK.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=${1:?usage: drg-make-r11.sh BENCH (gpu/drgles/build-ios.sh output)}
LIBS=/mnt/f/Nokia-HackiOs/research/vendor-adreno/files/lib64
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r11 2>/dev/null || true
echo "r10: $(sha256sum ios16-root-r10.img | cut -c1-16)"
cp --sparse=always ios16-root-r10.img ios16-root-r11.img
mkdir -p /mnt/r11
mount -t hfsplus -o loop,force ios16-root-r11.img /mnt/r11
python3 $REPO/tools/drg-root-gpubench.py /mnt/r11 "$BENCH" "$LIBS"
cmp "$BENCH" /mnt/r11/usr/local/bin/drgles-bench
ls -la /mnt/r11/usr/local/bin /mnt/r11/usr/local/drgles/adreno | head -30
umount /mnt/r11
# r10 (flashed and booted) gets "Volume header needs minor repair" from this fsck; r11 must give r10's exact report
F10=$(fsck.hfsplus -fn ios16-root-r10.img 2>&1 | sed 1d || true)
F11=$(fsck.hfsplus -fn ios16-root-r11.img 2>&1 | sed 1d || true)
echo "$F11" | grep -E 'needs|repair|corrupt' || true
[ "$F10" = "$F11" ] || { echo "error: fsck of r11 differs from r10's" >&2; diff <(echo "$F10") <(echo "$F11") >&2; exit 1; }
echo "fsck: same report as r10"
/root/chunkhash ios16-root-r11.img 16384 > ios16-root-r11.chunks
echo "r11: $(sha256sum ios16-root-r11.img | cut -c1-64)"
echo "chunks differing from r10: $(diff <(cut -d' ' -f2 ios16-root-r10.chunks) <(cut -d' ' -f2 ios16-root-r11.chunks) | grep -c '^>')"
