#!/bin/bash
# Build root image r21 = r20 + GPU drawing for backboardd (tools/drg-root-gpudraw.py: the iOS bench with
# -selftest, libdrgeagl.dylib, job com.drg.gpuselftest, DYLD_INSERT_LIBRARIES for backboardd), its chunk
# list for drg-flash-diff.sh, and checks (run in WSL as root). Flashing is a separate step, done only with
# the user's OK.
# From r20, not from the phone's live copy: the phone's volume was used by iOS and not cleanly unmounted,
# its journal holds transactions XNU replays at mount (2026-11-02: 3,031,040 bytes) over anything Linux
# writes there. r20 was never mounted by XNU (journal empty, "needs init"). Flashing r21 puts the
# volume's first 16 GiB back to r21 (as every earlier root flash did); drg-growfs grows it again at boot.
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=${1:?usage: drg-make-r21.sh BENCH LIBDRGEAGL}
LIB=${2:?usage: drg-make-r21.sh BENCH LIBDRGEAGL}
lsmod | grep -q hfsplus || insmod /root/wslk/src/fs/hfsplus/hfsplus.ko
umount /mnt/r21 2>/dev/null || true
echo "r20: $(sha256sum ios16-root-r20.img | cut -c1-16)"
cp --sparse=always ios16-root-r20.img ios16-root-r21.img
mkdir -p /mnt/r21
mount -t hfsplus -o loop,force ios16-root-r21.img /mnt/r21
python3 $REPO/tools/drg-root-gpudraw.py /mnt/r21 "$BENCH" "$LIB"
cmp "$BENCH" /mnt/r21/usr/local/bin/drgles-bench
cmp "$LIB" /mnt/r21/usr/local/lib/libdrgeagl.dylib
python3 - /mnt/r21 <<'PY'
import plistlib, sys
r = sys.argv[1]
j = plistlib.load(open(r + '/System/Library/xpc/launchd.plist', 'rb'))['LaunchDaemons']
for k in ('com.drg.growfs', 'com.drg.gpuselftest', 'com.apple.backboardd'):
    v = j['/System/Library/LaunchDaemons/%s.plist' % k]
    print(k, v.get('ProgramArguments'), v.get('EnvironmentVariables'))
print('gpubench jobs left:', [k for k in j if 'gpubench' in k])
PY
umount /mnt/r21
# r20 gives "Volume header needs minor repair" from this fsck; r21 must give r20's exact report
FB=$(fsck.hfsplus -fn ios16-root-r20.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r21.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r21 differs from r20's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r20"
/root/chunkhash ios16-root-r21.img 16384 > ios16-root-r21.chunks
echo "r21: $(sha256sum ios16-root-r21.img | cut -c1-64)"
echo "chunks differing from r20: $(diff <(cut -d' ' -f2 ios16-root-r20.chunks) <(cut -d' ' -f2 ios16-root-r21.chunks) | grep -c '^>')"
