#!/bin/bash
# Build root image r24 = r23 + (A) the wallpaper fix: libdrgposter.dylib loaded into PosterBoard
# (research/reports/wallpaper.md; tools/drg-root-poster.py) and (B) the GPU self-test modes 4, flushall,
# delay (research/reports/gpu-fault-2.md §4A: bench and libdrgeagl rebuilt with kgsl_ios.c's new flush
# modes), its chunk list for drg-flash-diff.sh, and checks (run in WSL as root). Flashing is a separate
# step, done only with the user's OK.
#   drg-make-r24.sh BENCH LIBDRGEAGL LIBDRGPOSTER
# BENCH, LIBDRGEAGL: gpu/drgles/build-ios.sh and build-eagl.sh; LIBDRGPOSTER: tools/ios/build-drgposter.sh.
# From r23, never mounted by XNU (as r23 was made from r20): flashing r24 puts the volume's first 16 GiB
# back to r24, drg-growfs grows it again at boot. The image is mounted by tools/drg-hfs-run.sh (the host's
# hfsplus, else User-Mode Linux with the same 6.18.54.1 driver).
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r24.sh BENCH LIBDRGEAGL LIBDRGPOSTER}")
LIB=$(realpath "${2:?usage: drg-make-r24.sh BENCH LIBDRGEAGL LIBDRGPOSTER}")
POSTER=$(realpath "${3:?usage: drg-make-r24.sh BENCH LIBDRGEAGL LIBDRGPOSTER}")
H23=$(sha256sum ios16-root-r23.img | cut -c1-16)
echo "r23: $H23"
[ "${H23:0:8}" = 18a4d573 ] || { echo "error: ios16-root-r23.img is not r23 (18a4d573...)" >&2; exit 1; }
cp --sparse=always ios16-root-r23.img ios16-root-r24.img
W=$(mktemp -d /root/ipsw/.r24.XXXXXX)
trap 'rm -rf "$W"' EXIT

# 1. write: the GPU bench and library (self-test job and backboardd's entry as r23), libdrgposter
cat > "$W/write.sh" <<EOF
set -euo pipefail
python3 $REPO/tools/drg-root-gpudraw.py /mnt/r24 "$BENCH" "$LIB"
python3 $REPO/tools/drg-root-poster.py /mnt/r24 "$POSTER"
cmp "$BENCH" /mnt/r24/usr/local/bin/drgles-bench
cmp "$LIB" /mnt/r24/usr/local/lib/libdrgeagl.dylib
cmp "$POSTER" /mnt/r24/usr/local/lib/libdrgposter.dylib
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r24.img rw /mnt/r24 "$W/write.sh"

# 2. r23 gives r20's "Volume header needs minor repair" from this fsck; r24 must give r23's exact report
FB=$(fsck.hfsplus -fn ios16-root-r23.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r24.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r24 differs from r23's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r23"

# 3. read-only: copy out what we added or changed, with owner and mode, then check the copies here
cat > "$W/read.sh" <<EOF
set -euo pipefail
R=/mnt/r24
ls -ln \$R/usr/local/bin/drgles-bench \$R/usr/local/lib/ \$R/Applications/PosterBoard.app/PosterBoard \
	\$R/System/Library/LaunchAngels/com.apple.PosterBoard.plist
mkdir -p "$W/out"
cp \$R/usr/local/bin/drgles-bench \$R/usr/local/lib/libdrgeagl.dylib \$R/usr/local/lib/libdrgposter.dylib \
	\$R/Applications/PosterBoard.app/PosterBoard \$R/System/Library/xpc/launchd.plist \
	\$R/System/Library/LaunchAngels/com.apple.PosterBoard.plist "$W/out/"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r24.img ro /mnt/r24 "$W/read.sh"
O=$W/out
sha256sum "$O/drgles-bench" "$O/libdrgeagl.dylib" "$O/libdrgposter.dylib" "$O/PosterBoard"
cmp "$BENCH" "$O/drgles-bench" && cmp "$LIB" "$O/libdrgeagl.dylib" && cmp "$POSTER" "$O/libdrgposter.dylib"
python3 - "$O" <<'PY'
import plistlib, sys
o = sys.argv[1]
j = plistlib.load(open(o + '/launchd.plist', 'rb'))['LaunchDaemons']
for k in ('LaunchDaemons/com.drg.growfs', 'LaunchDaemons/com.drg.gpuselftest', 'LaunchDaemons/com.apple.backboardd',
          'LaunchAngels/com.apple.PosterBoard'):
    v = j['/System/Library/%s.plist' % k]
    print(k, v.get('ProgramArguments'), v.get('EnvironmentVariables'))
a = plistlib.load(open(o + '/com.apple.PosterBoard.plist', 'rb'))
print('PosterBoard angel plist:', a.get('Label'), a.get('EnvironmentVariables'))
want = {'DYLD_INSERT_LIBRARIES': '/usr/local/lib/libdrgposter.dylib'}
assert j['/System/Library/LaunchAngels/com.apple.PosterBoard.plist']['EnvironmentVariables'] == want
assert a['EnvironmentVariables'] == want
print('gpubench jobs left:', [k for k in j if 'gpubench' in k])
PY
python3 $REPO/tools/drg-root-poster.py --check "$O/PosterBoard"
llvm-otool-21 -L "$O/PosterBoard" | tail -1
for f in "$O/drgles-bench" "$O/libdrgeagl.dylib" "$O/libdrgposter.dylib"; do
	llvm-otool-21 -l "$f" | grep -q LC_CODE_SIGNATURE || { echo "error: $f not signed" >&2; exit 1; }
	llvm-otool-21 -l "$f" | grep -A4 LC_BUILD_VERSION | grep -q 'minos 16.0' || { echo "error: $f not iOS 16" >&2; exit 1; }
	echo "$(basename "$f"): signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=50 MIN_EXCLUSIVES=0 \
		bash $REPO/tools/check-armv80.sh "$f" | tail -1)"
done

/root/chunkhash ios16-root-r24.img 16384 > ios16-root-r24.chunks
echo "r24: $(sha256sum ios16-root-r24.img | cut -c1-64)"
echo "chunks differing from r23: $(diff <(cut -d' ' -f2 ios16-root-r23.chunks) <(cut -d' ' -f2 ios16-root-r24.chunks) | grep -c '^>')"
