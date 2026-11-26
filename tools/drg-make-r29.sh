#!/bin/bash
# Root r29 (run in WSL as root) = r28 with backboardd's GPU drawing on again: bench and libdrgeagl built with
# DRG_EXTRA_CFLAGS=-DDRG_EAGL_GPU, plus the GPU-path fixes for boot #58: the GL context borrowed and given back
# by EAGLContext methods (c0f470f), 'b3a8' surfaces through plane 0 (475c745). Needs kernel CI 45754474135
# (patch 0920: IOSurface wrap selectors refused instead of a zero lock result).
#   drg-make-r29.sh BENCH EAGL
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r29.sh BENCH EAGL}")
EAGL=$(realpath "${2:?usage: drg-make-r29.sh BENCH EAGL}")
H26=$(sha256sum ios16-root-r28.img | cut -c1-16)
echo "r28: $H26"
[ "${H26:0:8}" = 470e3b6f ] || { echo "error: ios16-root-r28.img is not r28 (470e3b6f...)" >&2; exit 1; }
W=$(mktemp -d /root/ipsw/.r29.XXXXXX)
trap 'rm -rf "$W"' EXIT

KEEP="usr/local/lib/libdrgposter.dylib usr/libexec/backboardd Applications/PosterBoard.app/PosterBoard
System/Library/LaunchDaemons/com.drg.gpuselftest.plist System/Library/xpc/launchd.plist"
NEW="usr/local/bin/drgles-bench usr/local/lib/libdrgeagl.dylib"
cat > "$W/old.sh" <<EOF
set -euo pipefail
mkdir -p "$W/old"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r28/\$f "$W/old/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r28/\$f > "$W/old/\$(echo \$f | tr / _).stat"
done
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r28.img ro /mnt/r28 "$W/old.sh"
cmp -s "$BENCH" "$W/old/usr_local_bin_drgles-bench" && { echo "error: BENCH is r28's" >&2; exit 1; }
cmp -s "$EAGL" "$W/old/usr_local_lib_libdrgeagl.dylib" && { echo "error: EAGL is r28's" >&2; exit 1; }
cp --sparse=always ios16-root-r28.img ios16-root-r29.img

cat > "$W/write.sh" <<EOF
set -euo pipefail
cat "$BENCH" > /mnt/r29/usr/local/bin/drgles-bench
cat "$EAGL" > /mnt/r29/usr/local/lib/libdrgeagl.dylib
cmp "$BENCH" /mnt/r29/usr/local/bin/drgles-bench
cmp "$EAGL" /mnt/r29/usr/local/lib/libdrgeagl.dylib
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r29.img rw /mnt/r29 "$W/write.sh"

FB=$(fsck.hfsplus -fn ios16-root-r28.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r29.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r29 differs from r28's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r28"

cat > "$W/read.sh" <<EOF
set -euo pipefail
mkdir -p "$W/out"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r29/\$f "$W/out/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r29/\$f > "$W/out/\$(echo \$f | tr / _).stat"
done
echo "verdict dir entries: \$(ls -A /mnt/r29/private/var/db/drg | wc -l)"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r29.img ro /mnt/r29 "$W/read.sh"
for f in $KEEP; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n" "$W/out/$n" || { echo "error: $f differs from r28's" >&2; exit 1; }
done
echo "kept r28's byte for byte: $(echo $KEEP | wc -w) files"
cmp "$BENCH" "$W/out/usr_local_bin_drgles-bench"
cmp "$EAGL" "$W/out/usr_local_lib_libdrgeagl.dylib"
for f in $NEW; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n.stat" "$W/out/$n.stat" && echo "$f owner/mode: $(cat "$W/out/$n.stat") (r28's)"
	o=$W/out/$n
	llvm-otool-21 -l "$o" | grep LC_CODE_SIGNATURE || { echo "error: $f not signed" >&2; exit 1; }
	llvm-otool-21 -l "$o" | grep -A4 LC_BUILD_VERSION | grep 'minos 16.0' || { echo "error: $f not iOS 16" >&2; exit 1; }
	echo "$f: signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash $REPO/tools/check-armv80.sh "$o" | tail -1)"
done
llvm-otool-21 -D "$W/out/usr_local_lib_libdrgeagl.dylib" | tail -1 | grep -x /usr/local/lib/libdrgeagl.dylib &&
	echo "libdrgeagl install name: /usr/local/lib/libdrgeagl.dylib (backboardd's load command)"
sha256sum "$BENCH" "$EAGL"

/root/chunkhash ios16-root-r29.img 16384 > ios16-root-r29.chunks
echo "r29: $(sha256sum ios16-root-r29.img | cut -c1-64)"
echo "differing chunks from r28: $(paste -d' ' <(cut -d' ' -f2 ios16-root-r28.chunks) <(cut -d' ' -f2 ios16-root-r29.chunks) |
	awk '$1 != $2 {printf "%d ", NR-1}')"
