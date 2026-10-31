#!/bin/bash
# Build root image r14 = r13 + jetsam memory limits for the bench and the log keeper, its chunk
# list for drg-flash-diff.sh, and check it (run in WSL as root). Flashing is a
# separate step (tools/drg-flash-diff.sh), done only with the user's OK.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
KMSG=${1:?usage: drg-make-r14.sh DRG_KMSG BENCH}
BENCH=${2:?usage: drg-make-r14.sh DRG_KMSG BENCH}
LIBS=/mnt/f/Nokia-HackiOs/research/vendor-adreno/files/lib64
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r14 2>/dev/null || true
echo "r13: $(sha256sum ios16-root-r13.img | cut -c1-16)"
cp --sparse=always ios16-root-r13.img ios16-root-r14.img
mkdir -p /mnt/r14
mount -t hfsplus -o loop,force ios16-root-r14.img /mnt/r14
python3 $REPO/tools/drg-root-kmsg.py /mnt/r14 "$KMSG"
python3 $REPO/tools/drg-root-gpubench.py /mnt/r14 "$BENCH" "$LIBS"
cmp "$BENCH" /mnt/r14/usr/local/bin/drgles-bench
cmp "$KMSG" /mnt/r14/usr/local/bin/drg-kmsg
ls -la /mnt/r14/usr/local/bin
umount /mnt/r14
# r13 (flashed and booted) gets "Volume header needs minor repair" from this fsck; r14 must give r13's exact report
FB=$(fsck.hfsplus -fn ios16-root-r13.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r14.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r14 differs from r13's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r13"
/root/chunkhash ios16-root-r14.img 16384 > ios16-root-r14.chunks
echo "r14: $(sha256sum ios16-root-r14.img | cut -c1-64)"
echo "chunks differing from r13: $(diff <(cut -d' ' -f2 ios16-root-r13.chunks) <(cut -d' ' -f2 ios16-root-r14.chunks) | grep -c '^>')"
