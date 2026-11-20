#!/bin/bash
# Build root image r26 = r25 + the self-test bench with the render-target map (gpu/drgles rt_map.c:
# which texels of the 1125x2436 IOSurface render target are wrong and what they hold, the same pages
# through the GPU import's own mapping, one "submit:" line per GPU_COMMAND of the 60 frames, cmdlist
# entry counters). Nothing else changes: libdrgeagl, the self-test job, backboardd, PosterBoard and
# libdrgposter stay r25's byte for byte. The new bench changes the self-test verdict key
# (verdict.c: bench + libdrgeagl + kern.version), so the first boot of r26 runs the self-test again.
# Chunk list for drg-flash-diff.sh and checks (run in WSL as root). Flashing is a separate step, done
# only with the user's OK.
#   drg-make-r26.sh BENCH          BENCH: gpu/drgles/build-ios.sh
# From r25, never mounted by XNU. Mounted by tools/drg-hfs-run.sh (User-Mode Linux on this WSL kernel).
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r26.sh BENCH}")
H25=$(sha256sum ios16-root-r25.img | cut -c1-16)
echo "r25: $H25"
[ "${H25:0:8}" = 6b8fa174 ] || { echo "error: ios16-root-r25.img is not r25 (6b8fa174...)" >&2; exit 1; }
W=$(mktemp -d /root/ipsw/.r26.XXXXXX)
trap 'rm -rf "$W"' EXIT

# 0. r25's files that must stay, for the byte comparison below
KEEP="usr/local/lib/libdrgeagl.dylib usr/local/lib/libdrgposter.dylib usr/libexec/backboardd
Applications/PosterBoard.app/PosterBoard System/Library/LaunchDaemons/com.drg.gpuselftest.plist
System/Library/xpc/launchd.plist"
cat > "$W/old.sh" <<EOF
set -euo pipefail
mkdir -p "$W/old"
for f in $(echo $KEEP); do cp /mnt/r25/\$f "$W/old/\$(echo \$f | tr / _)"; done
cp /mnt/r25/usr/local/bin/drgles-bench "$W/old/drgles-bench"
stat -c '%u %g %a' /mnt/r25/usr/local/bin/drgles-bench > "$W/old/bench.stat"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r25.img ro /mnt/r25 "$W/old.sh"
cmp -s "$BENCH" "$W/old/drgles-bench" && { echo "error: BENCH is r25's bench" >&2; exit 1; }
cp --sparse=always ios16-root-r25.img ios16-root-r26.img

# 1. write: the bench only, over the existing file (owner and mode kept)
cat > "$W/write.sh" <<EOF
set -euo pipefail
cat "$BENCH" > /mnt/r26/usr/local/bin/drgles-bench
cmp "$BENCH" /mnt/r26/usr/local/bin/drgles-bench
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r26.img rw /mnt/r26 "$W/write.sh"

# 2. fsck: r26 must give r25's exact report
FB=$(fsck.hfsplus -fn ios16-root-r25.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r26.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r26 differs from r25's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r25"

# 3. read-only: the bench and everything that must be r25's
cat > "$W/read.sh" <<EOF
set -euo pipefail
mkdir -p "$W/out"
for f in $(echo $KEEP); do cp /mnt/r26/\$f "$W/out/\$(echo \$f | tr / _)"; done
cp /mnt/r26/usr/local/bin/drgles-bench "$W/out/drgles-bench"
stat -c '%u %g %a' /mnt/r26/usr/local/bin/drgles-bench > "$W/out/bench.stat"
ls -ldn /mnt/r26/private/var/db/drg; echo "verdict dir entries: \$(ls -A /mnt/r26/private/var/db/drg | wc -l)"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r26.img ro /mnt/r26 "$W/read.sh"
for f in $KEEP; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n" "$W/out/$n" || { echo "error: $f differs from r25's" >&2; exit 1; }
done
echo "kept r25's byte for byte: $(echo $KEEP | wc -w) files"
cmp "$BENCH" "$W/out/drgles-bench"
cmp "$W/old/bench.stat" "$W/out/bench.stat" && echo "bench owner/mode: $(cat "$W/out/bench.stat") (r25's)"
f=$W/out/drgles-bench
llvm-otool-21 -l "$f" | grep -q LC_CODE_SIGNATURE || { echo "error: bench not signed" >&2; exit 1; }
llvm-otool-21 -l "$f" | grep -A4 LC_BUILD_VERSION | grep -q 'minos 16.0' || { echo "error: bench not iOS 16" >&2; exit 1; }
echo "drgles-bench: signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash $REPO/tools/check-armv80.sh "$f" | tail -1)"
sha256sum "$W/old/drgles-bench" "$f"

/root/chunkhash ios16-root-r26.img 16384 > ios16-root-r26.chunks
echo "r26: $(sha256sum ios16-root-r26.img | cut -c1-64)"
echo "chunks differing from r25: $(diff <(cut -d' ' -f2 ios16-root-r25.chunks) <(cut -d' ' -f2 ios16-root-r26.chunks) | grep -c '^>')"
echo "differing chunks: $(paste -d' ' <(cut -d' ' -f2 ios16-root-r25.chunks) <(cut -d' ' -f2 ios16-root-r26.chunks) |
	awk '$1 != $2 {printf "%d ", NR-1}')"
