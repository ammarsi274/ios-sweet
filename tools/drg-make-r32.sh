#!/bin/bash
# Root r32 (run in WSL as root) = r31 with libdrgeagl 39771550: + pixel check (boots #60/#62 drew without
# errors but the screen was black): frames 3-6 and every 600th log each draw's program/texture/surface, 7 pixels
# read back by the GPU and the same pixels from the surface memory after the CPU sync. Bench = r31's (unchanged).
#   drg-make-r32.sh BENCH EAGL   (BENCH = r31's drgles-bench)
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r32.sh BENCH EAGL}")
EAGL=$(realpath "${2:?usage: drg-make-r32.sh BENCH EAGL}")
H26=$(sha256sum ios16-root-r31.img | cut -c1-16)
echo "r31: $H26"
[ "${H26:0:8}" = f17c90a2 ] || { echo "error: ios16-root-r31.img is not r31 (f17c90a2...)" >&2; exit 1; }
W=$(mktemp -d /root/ipsw/.r32.XXXXXX)
trap 'rm -rf "$W"' EXIT

KEEP="usr/local/lib/libdrgposter.dylib usr/libexec/backboardd Applications/PosterBoard.app/PosterBoard
System/Library/LaunchDaemons/com.drg.gpuselftest.plist System/Library/xpc/launchd.plist"
NEW="usr/local/bin/drgles-bench usr/local/lib/libdrgeagl.dylib"
cat > "$W/old.sh" <<EOF
set -euo pipefail
mkdir -p "$W/old"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r31/\$f "$W/old/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r31/\$f > "$W/old/\$(echo \$f | tr / _).stat"
done
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r31.img ro /mnt/r31 "$W/old.sh"
cmp "$BENCH" "$W/old/usr_local_bin_drgles-bench" || { echo "error: BENCH is not r31's" >&2; exit 1; }
cmp -s "$EAGL" "$W/old/usr_local_lib_libdrgeagl.dylib" && { echo "error: EAGL is r31's" >&2; exit 1; }
cp --sparse=always ios16-root-r31.img ios16-root-r32.img

cat > "$W/write.sh" <<EOF
set -euo pipefail
cat "$BENCH" > /mnt/r32/usr/local/bin/drgles-bench
cat "$EAGL" > /mnt/r32/usr/local/lib/libdrgeagl.dylib
cmp "$BENCH" /mnt/r32/usr/local/bin/drgles-bench
cmp "$EAGL" /mnt/r32/usr/local/lib/libdrgeagl.dylib
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r32.img rw /mnt/r32 "$W/write.sh"

FB=$(fsck.hfsplus -fn ios16-root-r31.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r32.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r32 differs from r31's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r31"

cat > "$W/read.sh" <<EOF
set -euo pipefail
mkdir -p "$W/out"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r32/\$f "$W/out/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r32/\$f > "$W/out/\$(echo \$f | tr / _).stat"
done
echo "verdict dir entries: \$(ls -A /mnt/r32/private/var/db/drg | wc -l)"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r32.img ro /mnt/r32 "$W/read.sh"
for f in $KEEP; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n" "$W/out/$n" || { echo "error: $f differs from r31's" >&2; exit 1; }
done
echo "kept r31's byte for byte: $(echo $KEEP | wc -w) files"
cmp "$BENCH" "$W/out/usr_local_bin_drgles-bench"
cmp "$EAGL" "$W/out/usr_local_lib_libdrgeagl.dylib"
for f in $NEW; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n.stat" "$W/out/$n.stat" && echo "$f owner/mode: $(cat "$W/out/$n.stat") (r31's)"
	o=$W/out/$n
	llvm-otool-21 -l "$o" | grep LC_CODE_SIGNATURE || { echo "error: $f not signed" >&2; exit 1; }
	llvm-otool-21 -l "$o" | grep -A4 LC_BUILD_VERSION | grep 'minos 16.0' || { echo "error: $f not iOS 16" >&2; exit 1; }
	echo "$f: signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash $REPO/tools/check-armv80.sh "$o" | tail -1)"
done
llvm-otool-21 -D "$W/out/usr_local_lib_libdrgeagl.dylib" | tail -1 | grep -x /usr/local/lib/libdrgeagl.dylib &&
	echo "libdrgeagl install name: /usr/local/lib/libdrgeagl.dylib (backboardd's load command)"
sha256sum "$BENCH" "$EAGL"

/root/chunkhash ios16-root-r32.img 16384 > ios16-root-r32.chunks
echo "r32: $(sha256sum ios16-root-r32.img | cut -c1-64)"
echo "differing chunks from r31: $(paste -d' ' <(cut -d' ' -f2 ios16-root-r31.chunks) <(cut -d' ' -f2 ios16-root-r32.chunks) |
	awk '$1 != $2 {printf "%d ", NR-1}')"
