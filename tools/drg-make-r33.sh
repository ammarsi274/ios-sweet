#!/bin/bash
# Root r33 (run in WSL as root) = r32 with libdrgeagl 3faa276e: the pixel check plus what fills each texture
# (boot #63: every frame = 3 full-screen draws, the content one samples a new texture per frame with no IOSurface
# behind it, the last with program 0): every texImageIOSurface call (first 40, arguments and result), and on sampled
# frames texture create/bind/upload (non-zero check of the data) and program switches; read-back from the frame's
# own framebuffer. Bench = r31's/r32's.
#   drg-make-r33.sh BENCH EAGL
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r33.sh BENCH EAGL}")
EAGL=$(realpath "${2:?usage: drg-make-r33.sh BENCH EAGL}")
H26=$(sha256sum ios16-root-r32.img | cut -c1-16)
echo "r32: $H26"
[ "${H26:0:8}" = 82d5a88f ] || { echo "error: ios16-root-r32.img is not r32 (82d5a88f...)" >&2; exit 1; }
W=$(mktemp -d /root/ipsw/.r33.XXXXXX)
trap 'rm -rf "$W"' EXIT

KEEP="usr/local/lib/libdrgposter.dylib usr/libexec/backboardd Applications/PosterBoard.app/PosterBoard
System/Library/LaunchDaemons/com.drg.gpuselftest.plist System/Library/xpc/launchd.plist"
NEW="usr/local/bin/drgles-bench usr/local/lib/libdrgeagl.dylib"
cat > "$W/old.sh" <<EOF
set -euo pipefail
mkdir -p "$W/old"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r32/\$f "$W/old/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r32/\$f > "$W/old/\$(echo \$f | tr / _).stat"
done
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r32.img ro /mnt/r32 "$W/old.sh"
cmp "$BENCH" "$W/old/usr_local_bin_drgles-bench" || { echo "error: BENCH is not r32's" >&2; exit 1; }
cmp -s "$EAGL" "$W/old/usr_local_lib_libdrgeagl.dylib" && { echo "error: EAGL is r32's" >&2; exit 1; }
cp --sparse=always ios16-root-r32.img ios16-root-r33.img

cat > "$W/write.sh" <<EOF
set -euo pipefail
cat "$BENCH" > /mnt/r33/usr/local/bin/drgles-bench
cat "$EAGL" > /mnt/r33/usr/local/lib/libdrgeagl.dylib
cmp "$BENCH" /mnt/r33/usr/local/bin/drgles-bench
cmp "$EAGL" /mnt/r33/usr/local/lib/libdrgeagl.dylib
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r33.img rw /mnt/r33 "$W/write.sh"

FB=$(fsck.hfsplus -fn ios16-root-r32.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r33.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r33 differs from r32's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r32"

cat > "$W/read.sh" <<EOF
set -euo pipefail
mkdir -p "$W/out"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r33/\$f "$W/out/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r33/\$f > "$W/out/\$(echo \$f | tr / _).stat"
done
echo "verdict dir entries: \$(ls -A /mnt/r33/private/var/db/drg | wc -l)"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r33.img ro /mnt/r33 "$W/read.sh"
for f in $KEEP; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n" "$W/out/$n" || { echo "error: $f differs from r32's" >&2; exit 1; }
done
echo "kept r32's byte for byte: $(echo $KEEP | wc -w) files"
cmp "$BENCH" "$W/out/usr_local_bin_drgles-bench"
cmp "$EAGL" "$W/out/usr_local_lib_libdrgeagl.dylib"
for f in $NEW; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n.stat" "$W/out/$n.stat" && echo "$f owner/mode: $(cat "$W/out/$n.stat") (r32's)"
	o=$W/out/$n
	llvm-otool-21 -l "$o" | grep LC_CODE_SIGNATURE || { echo "error: $f not signed" >&2; exit 1; }
	llvm-otool-21 -l "$o" | grep -A4 LC_BUILD_VERSION | grep 'minos 16.0' || { echo "error: $f not iOS 16" >&2; exit 1; }
	echo "$f: signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash $REPO/tools/check-armv80.sh "$o" | tail -1)"
done
llvm-otool-21 -D "$W/out/usr_local_lib_libdrgeagl.dylib" | tail -1 | grep -x /usr/local/lib/libdrgeagl.dylib &&
	echo "libdrgeagl install name: /usr/local/lib/libdrgeagl.dylib (backboardd's load command)"
sha256sum "$BENCH" "$EAGL"

/root/chunkhash ios16-root-r33.img 16384 > ios16-root-r33.chunks
echo "r33: $(sha256sum ios16-root-r33.img | cut -c1-64)"
echo "differing chunks from r32: $(paste -d' ' <(cut -d' ' -f2 ios16-root-r32.chunks) <(cut -d' ' -f2 ios16-root-r33.chunks) |
	awk '$1 != $2 {printf "%d ", NR-1}')"
