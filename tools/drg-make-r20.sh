#!/bin/bash
# Build root image r20 = r15 + drg-growfs (grows / to the userdata partition minus the shim's 64 MiB
# scratch area, at every boot until it has that size) and without the GPU bench jobs, its chunk list
# for drg-flash-diff.sh, and checks (run in WSL as root). Flashing is a separate step, done only with
# the user's OK.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
GROWFS=${1:?usage: drg-make-r20.sh DRG_GROWFS}
LIBS=/mnt/f/Nokia-HackiOs/research/vendor-adreno/files/lib64
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r20 2>/dev/null || true
echo "r15: $(sha256sum ios16-root-r15.img | cut -c1-16)"
cp --sparse=always ios16-root-r15.img ios16-root-r20.img
mkdir -p /mnt/r20
mount -t hfsplus -o loop,force ios16-root-r20.img /mnt/r20
python3 $REPO/tools/drg-root-growfs.py /mnt/r20 "$GROWFS"
cmp "$GROWFS" /mnt/r20/usr/local/bin/drg-growfs
python3 - /mnt/r20 <<'PY'
import plistlib, sys
r = sys.argv[1]
j = plistlib.load(open(r + '/System/Library/xpc/launchd.plist', 'rb'))['LaunchDaemons']
for k in ('com.drg.growfs', 'com.drg.kmsg'):
    print(k, j['/System/Library/LaunchDaemons/%s.plist' % k]['ProgramArguments'])
o = plistlib.load(open(r + '/System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist', 'rb'))['Version4']['Daemon']['Override']
print({k: o[k] for k in o if k.startswith('com.drg.')})
PY
umount /mnt/r20
# r15 (flashed and booted) gets "Volume header needs minor repair" from this fsck; r20 must give r15's exact report
FB=$(fsck.hfsplus -fn ios16-root-r15.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r20.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r20 differs from r15's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r15"
/root/chunkhash ios16-root-r20.img 16384 > ios16-root-r20.chunks
echo "r20: $(sha256sum ios16-root-r20.img | cut -c1-64)"
echo "chunks differing from r15: $(diff <(cut -d' ' -f2 ios16-root-r15.chunks) <(cut -d' ' -f2 ios16-root-r20.chunks) | grep -c '^>')"
