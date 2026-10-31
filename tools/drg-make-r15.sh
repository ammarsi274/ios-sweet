#!/bin/bash
# Build root image r15 = r14 + the bench with -delay 240 -capture, + the dyld shared cache's
# log strings (/private/var/db/uuidtext/dsc, 101 MB) that logd_helper otherwise builds on the
# first boot after a flash (boot #35: 4 min of kernel time). The two dsc files come from the
# phone's own boot #35 copy, byte-identical in the #36 copy. Its chunk list for drg-flash-diff.sh,
# and checks (run in WSL as root). Flashing is a separate step, done only with the user's OK.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=${1:?usage: drg-make-r15.sh BENCH}
LIBS=/mnt/f/Nokia-HackiOs/research/vendor-adreno/files/lib64
SRC=/mnt/f/Nokia-HackiOs/ios-root/phone-after-boot35.img
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r15 /mnt/b35 2>/dev/null || true
echo "r14: $(sha256sum ios16-root-r14.img | cut -c1-16)"
cp --sparse=always ios16-root-r14.img ios16-root-r15.img
mkdir -p /mnt/r15 /mnt/b35
mount -t hfsplus -o loop,force ios16-root-r15.img /mnt/r15
mount -t hfsplus -o ro,loop $SRC /mnt/b35
python3 $REPO/tools/drg-root-gpubench.py /mnt/r15 "$BENCH" "$LIBS"
cmp "$BENCH" /mnt/r15/usr/local/bin/drgles-bench
D=private/var/db/uuidtext/dsc
mkdir -p /mnt/r15/$D
chown 272:272 /mnt/r15/$D && chmod 755 /mnt/r15/$D
for f in 8F970452C9E33E88B114FEB1864BC9E0 BF7C980420CE372B804DFAA2C6A10CDE; do
	cp /mnt/b35/$D/$f /mnt/r15/$D/$f
	chown 272:272 /mnt/r15/$D/$f && chmod 644 /mnt/r15/$D/$f
	cmp /mnt/b35/$D/$f /mnt/r15/$D/$f
done
stat -c '%u:%g %a %s %n' /mnt/r15/$D /mnt/r15/$D/*
python3 -c "import plistlib; print(plistlib.load(open('/mnt/r15/System/Library/xpc/launchd.plist','rb'))['LaunchDaemons']['/System/Library/LaunchDaemons/com.drg.gpubench.plist']['ProgramArguments'])"
umount /mnt/b35
umount /mnt/r15
# r14 (flashed and booted) gets "Volume header needs minor repair" from this fsck; r15 must give r14's exact report
FB=$(fsck.hfsplus -fn ios16-root-r14.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r15.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r15 differs from r14's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r14"
/root/chunkhash ios16-root-r15.img 16384 > ios16-root-r15.chunks
echo "r15: $(sha256sum ios16-root-r15.img | cut -c1-64)"
echo "chunks differing from r14: $(diff <(cut -d' ' -f2 ios16-root-r14.chunks) <(cut -d' ' -f2 ios16-root-r15.chunks) | grep -c '^>')"
