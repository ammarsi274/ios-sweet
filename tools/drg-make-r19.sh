#!/bin/bash
# Build root image r19 = r15 + the isolation runs (boot #42): plain, -dsb, -deferfree, all traced,
# at boot, +60 s, +120 s (tools/drg-root-gpubench.py RUNS). No
# spindump (r16's) this time. Its chunk list for drg-flash-diff.sh, and checks (run in WSL as root).
# Flashing is a separate step, done only with the user's OK.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=${1:?usage: drg-make-r19.sh BENCH}
LIBS=/mnt/f/Nokia-HackiOs/research/vendor-adreno/files/lib64
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r19 2>/dev/null || true
echo "r15: $(sha256sum ios16-root-r15.img | cut -c1-16)"
cp --sparse=always ios16-root-r15.img ios16-root-r19.img
mkdir -p /mnt/r19
mount -t hfsplus -o loop,force ios16-root-r19.img /mnt/r19
python3 $REPO/tools/drg-root-gpubench.py /mnt/r19 "$BENCH" "$LIBS"
cmp "$BENCH" /mnt/r19/usr/local/bin/drgles-bench
python3 - /mnt/r19 <<'PY'
import plistlib, sys
r = sys.argv[1]
j = plistlib.load(open(r + '/System/Library/xpc/launchd.plist', 'rb'))['LaunchDaemons']
for k in ('com.drg.gpubench', 'com.drg.gpubench2', 'com.drg.gpubench3', 'com.drg.kmsg'):
    print(k, j['/System/Library/LaunchDaemons/%s.plist' % k]['ProgramArguments'])
o = plistlib.load(open(r + '/System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist', 'rb'))['Version4']['Daemon']['Override']
print({k: o[k] for k in o if k.startswith('com.drg.')})
PY
umount /mnt/r19
# r15 (flashed and booted) gets "Volume header needs minor repair" from this fsck; r19 must give r15's exact report
FB=$(fsck.hfsplus -fn ios16-root-r15.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r19.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r19 differs from r15's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r15"
/root/chunkhash ios16-root-r19.img 16384 > ios16-root-r19.chunks
echo "r19: $(sha256sum ios16-root-r19.img | cut -c1-64)"
echo "chunks differing from r15: $(diff <(cut -d' ' -f2 ios16-root-r15.chunks) <(cut -d' ' -f2 ios16-root-r19.chunks) | grep -c '^>')"
