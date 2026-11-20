#!/bin/bash
# Root r27 (run in WSL as root) = r26 + the GPU list-stride fix (3ac80fe) in both users of kgsl_ios.c:
# drgles-bench (the boot self-test, which now also restarts backboardd once after a passing verdict)
# and libdrgeagl.dylib (backboardd's drawing library). HMD's kgsl_drawobj.c steps every KGSL list by
# sizeof(entry); kgsl_ios.c stepped by the declared cmdsize (64 on two-IB submits) and skipped the
# second IB: the frame's last bins (G4R replay 2026-11-20: 37/44 bins before, 2,740,500/2,740,500 after).
#   drg-make-r27.sh BENCH EAGL
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r27.sh BENCH EAGL}")
EAGL=$(realpath "${2:?usage: drg-make-r27.sh BENCH EAGL}")
H26=$(sha256sum ios16-root-r26.img | cut -c1-16)
echo "r26: $H26"
[ "${H26:0:8}" = 38c27d79 ] || { echo "error: ios16-root-r26.img is not r26 (38c27d79...)" >&2; exit 1; }
W=$(mktemp -d /root/ipsw/.r27.XXXXXX)
trap 'rm -rf "$W"' EXIT

KEEP="usr/local/lib/libdrgposter.dylib usr/libexec/backboardd Applications/PosterBoard.app/PosterBoard
System/Library/LaunchDaemons/com.drg.gpuselftest.plist System/Library/xpc/launchd.plist"
NEW="usr/local/bin/drgles-bench usr/local/lib/libdrgeagl.dylib"
cat > "$W/old.sh" <<EOF
set -euo pipefail
mkdir -p "$W/old"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r26/\$f "$W/old/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r26/\$f > "$W/old/\$(echo \$f | tr / _).stat"
done
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r26.img ro /mnt/r26 "$W/old.sh"
cmp -s "$BENCH" "$W/old/usr_local_bin_drgles-bench" && { echo "error: BENCH is r26's" >&2; exit 1; }
cmp -s "$EAGL" "$W/old/usr_local_lib_libdrgeagl.dylib" && { echo "error: EAGL is r26's" >&2; exit 1; }
cp --sparse=always ios16-root-r26.img ios16-root-r27.img

cat > "$W/write.sh" <<EOF
set -euo pipefail
cat "$BENCH" > /mnt/r27/usr/local/bin/drgles-bench
cat "$EAGL" > /mnt/r27/usr/local/lib/libdrgeagl.dylib
cmp "$BENCH" /mnt/r27/usr/local/bin/drgles-bench
cmp "$EAGL" /mnt/r27/usr/local/lib/libdrgeagl.dylib
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r27.img rw /mnt/r27 "$W/write.sh"

FB=$(fsck.hfsplus -fn ios16-root-r26.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r27.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r27 differs from r26's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r26"

cat > "$W/read.sh" <<EOF
set -euo pipefail
mkdir -p "$W/out"
for f in $(echo $KEEP $NEW); do
	cp /mnt/r27/\$f "$W/out/\$(echo \$f | tr / _)"
	stat -c '%u %g %a' /mnt/r27/\$f > "$W/out/\$(echo \$f | tr / _).stat"
done
echo "verdict dir entries: \$(ls -A /mnt/r27/private/var/db/drg | wc -l)"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r27.img ro /mnt/r27 "$W/read.sh"
for f in $KEEP; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n" "$W/out/$n" || { echo "error: $f differs from r26's" >&2; exit 1; }
done
echo "kept r26's byte for byte: $(echo $KEEP | wc -w) files"
cmp "$BENCH" "$W/out/usr_local_bin_drgles-bench"
cmp "$EAGL" "$W/out/usr_local_lib_libdrgeagl.dylib"
for f in $NEW; do
	n=$(echo $f | tr / _)
	cmp "$W/old/$n.stat" "$W/out/$n.stat" && echo "$f owner/mode: $(cat "$W/out/$n.stat") (r26's)"
	o=$W/out/$n
	llvm-otool-21 -l "$o" | grep -q LC_CODE_SIGNATURE || { echo "error: $f not signed" >&2; exit 1; }
	llvm-otool-21 -l "$o" | grep -A4 LC_BUILD_VERSION | grep -q 'minos 16.0' || { echo "error: $f not iOS 16" >&2; exit 1; }
	echo "$f: signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=1000 bash $REPO/tools/check-armv80.sh "$o" | tail -1)"
done
llvm-otool-21 -D "$W/out/usr_local_lib_libdrgeagl.dylib" | tail -1 | grep -qx /usr/local/lib/libdrgeagl.dylib &&
	echo "libdrgeagl install name: /usr/local/lib/libdrgeagl.dylib (backboardd's load command)"
sha256sum "$BENCH" "$EAGL"

/root/chunkhash ios16-root-r27.img 16384 > ios16-root-r27.chunks
echo "r27: $(sha256sum ios16-root-r27.img | cut -c1-64)"
echo "differing chunks from r26: $(paste -d' ' <(cut -d' ' -f2 ios16-root-r26.chunks) <(cut -d' ' -f2 ios16-root-r27.chunks) |
	awk '$1 != $2 {printf "%d ", NR-1}')"
