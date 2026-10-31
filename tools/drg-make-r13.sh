#!/bin/bash
# Build root image r13 = r12 + the kernel log keeper (drg-root-kmsg.py) + the bench with one JIT arena, its chunk
# list for drg-flash-diff.sh, and check it (run in WSL as root). Flashing is a
# separate step (tools/drg-flash-diff.sh), done only with the user's OK.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
KMSG=${1:?usage: drg-make-r13.sh DRG_KMSG BENCH}
BENCH=${2:?usage: drg-make-r13.sh DRG_KMSG BENCH}
LIBS=/mnt/f/Nokia-HackiOs/research/vendor-adreno/files/lib64
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r13 2>/dev/null || true
echo "r12: $(sha256sum ios16-root-r12.img | cut -c1-16)"
cp --sparse=always ios16-root-r12.img ios16-root-r13.img
mkdir -p /mnt/r13
mount -t hfsplus -o loop,force ios16-root-r13.img /mnt/r13
python3 $REPO/tools/drg-root-kmsg.py /mnt/r13 "$KMSG"
python3 $REPO/tools/drg-root-gpubench.py /mnt/r13 "$BENCH" "$LIBS"
cmp "$BENCH" /mnt/r13/usr/local/bin/drgles-bench
cmp "$KMSG" /mnt/r13/usr/local/bin/drg-kmsg
ls -la /mnt/r13/usr/local/bin
umount /mnt/r13
# r12 (flashed and booted) gets "Volume header needs minor repair" from this fsck; r13 must give r12's exact report
FB=$(fsck.hfsplus -fn ios16-root-r12.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r13.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r13 differs from r12's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r12"
/root/chunkhash ios16-root-r13.img 16384 > ios16-root-r13.chunks
echo "r13: $(sha256sum ios16-root-r13.img | cut -c1-64)"
echo "chunks differing from r12: $(diff <(cut -d' ' -f2 ios16-root-r12.chunks) <(cut -d' ' -f2 ios16-root-r13.chunks) | grep -c '^>')"
