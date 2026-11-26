#!/bin/bash
# Root r30 (run in WSL as root) = r29 + bench/libdrgeagl (DRG_EXTRA_CFLAGS=-DDRG_EAGL_GPU) with: GL table calls off
# the frame thread give the context back (ae4f7b4; boot #59: EGL_BAD_ACCESS on the render thread, black screen) and
# KGSL TIMESTAMP_EVENT fences (ac667b2; boot #59: syncobj create failed). Kernel: CI 45762997330 (0921 live log).
#   drg-make-r30.sh BENCH EAGL
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r30.sh BENCH EAGL}")
EAGL=$(realpath "${2:?usage: drg-make-r30.sh BENCH EAGL}")
H26=$(sha256sum ios16-root-r29.img | cut -c1-16)
echo "r29: $H26"
[ "${H26:0:8}" = d8be1e4f ] || { echo "error: ios16-root-r29.img is not r29 (d8be1e4f...)" >&2; exit 1; }
W=$(mktemp -d /root/ipsw/.r30.XXXXXX)
trap 'rm -rf "$W"' EXIT

KEEP="usr/local/lib/libdrgposter.dylib usr/libexec/backboardd Applications/PosterBoard.app/PosterBoard
System/Library/LaunchDaemons/com.drg.gpuselftest.plist System/Library/xpc/launchd.plist"
NEW="usr/local/bin/drgles-bench usr/local/lib/libdrgeagl.dylib"
cat > "$W/old.sh" <<EOF
set -euo pipefail
mkdir -p "$W/old"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r29/\$f "$W/old/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r29/\$f > "$W/old/\$(echo \$f | tr / _).stat"
done
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r29.img ro /mnt/r29 "$W/old.sh"
cmp -s "$BENCH" "$W/old/usr_local_bin_drgles-bench" && { echo "error: BENCH is r29's" >&2; exit 1; }
cmp -s "$EAGL" "$W/old/usr_local_lib_libdrgeagl.dylib" && { echo "error: EAGL is r29's" >&2; exit 1; }
cp --sparse=always ios16-root-r29.img ios16-root-r30.img

cat > "$W/write.sh" <<EOF
set -euo pipefail
cat "$BENCH" > /mnt/r30/usr/local/bin/drgles-bench
cat "$EAGL" > /mnt/r30/usr/local/lib/libdrgeagl.dylib
cmp "$BENCH" /mnt/r30/usr/local/bin/drgles-bench
cmp "$EAGL" /mnt/r30/usr/local/lib/libdrgeagl.dylib
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r30.img rw /mnt/r30 "$W/write.sh"

FB=$(fsck.hfsplus -fn ios16-root-r29.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r30.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r30 differs from r29's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r29"

cat > "$W/read.sh" <<EOF
set -euo pipefail
mkdir -p "$W/out"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r30/\$f "$W/out/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r30/\$f > "$W/out/\$(echo \$f | tr / _).stat"
done
echo "verdict dir entries: \$(ls -A /mnt/r30/private/var/db/drg | wc -l)"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r30.img ro /mnt/r30 "$W/read.sh"
for f in $KEEP; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n" "$W/out/$n" || { echo "error: $f differs from r29's" >&2; exit 1; }
done
echo "kept r29's byte for byte: $(echo $KEEP | wc -w) files"
cmp "$BENCH" "$W/out/usr_local_bin_drgles-bench"
cmp "$EAGL" "$W/out/usr_local_lib_libdrgeagl.dylib"
for f in $NEW; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n.stat" "$W/out/$n.stat" && echo "$f owner/mode: $(cat "$W/out/$n.stat") (r29's)"
	o=$W/out/$n
	llvm-otool-21 -l "$o" | grep LC_CODE_SIGNATURE || { echo "error: $f not signed" >&2; exit 1; }
	llvm-otool-21 -l "$o" | grep -A4 LC_BUILD_VERSION | grep 'minos 16.0' || { echo "error: $f not iOS 16" >&2; exit 1; }
	echo "$f: signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash $REPO/tools/check-armv80.sh "$o" | tail -1)"
done
llvm-otool-21 -D "$W/out/usr_local_lib_libdrgeagl.dylib" | tail -1 | grep -x /usr/local/lib/libdrgeagl.dylib &&
	echo "libdrgeagl install name: /usr/local/lib/libdrgeagl.dylib (backboardd's load command)"
sha256sum "$BENCH" "$EAGL"

/root/chunkhash ios16-root-r30.img 16384 > ios16-root-r30.chunks
echo "r30: $(sha256sum ios16-root-r30.img | cut -c1-64)"
echo "differing chunks from r29: $(paste -d' ' <(cut -d' ' -f2 ios16-root-r29.chunks) <(cut -d' ' -f2 ios16-root-r30.chunks) |
	awk '$1 != $2 {printf "%d ", NR-1}')"
