#!/bin/bash
# Build root image r25 = r24 + (1) cache coherency for GPU-written memory and flushall as the default
# (bench and libdrgeagl from gpu/drgles: kgsl_ios.c GPUOBJ_SYNC, CPU sync of render targets after the
# GPU, a clean of every import when it is imported), (2) the self-test once per root image
# (bench -selftest-boot: verdict cached in /private/var/db/drg/gpu-selftest, run at nice 10 after
# SpringBoard + 60 s), (3) libdrgeagl loaded into backboardd by an LC_LOAD_WEAK_DYLIB
# (tools/drg-root-poster.py patch_exe, as PosterBoard in r24), on the GPU only with a cached passing
# verdict; its chunk list for drg-flash-diff.sh, and checks (run in WSL as root). Flashing is a separate
# step, done only with the user's OK.
#   drg-make-r25.sh BENCH LIBDRGEAGL
# BENCH, LIBDRGEAGL: gpu/drgles/build-ios.sh and build-eagl.sh. libdrgposter and PosterBoard stay r24's.
# From r24, never mounted by XNU. Mounted by tools/drg-hfs-run.sh (User-Mode Linux on this WSL kernel).
set -euo pipefail
cd /root/ipsw
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
BENCH=$(realpath "${1:?usage: drg-make-r25.sh BENCH LIBDRGEAGL}")
LIB=$(realpath "${2:?usage: drg-make-r25.sh BENCH LIBDRGEAGL}")
EAGL=/usr/local/lib/libdrgeagl.dylib
POSTERLIB=/usr/local/lib/libdrgposter.dylib
H24=$(sha256sum ios16-root-r24.img | cut -c1-16)
echo "r24: $H24"
[ "${H24:0:8}" = e54d42c2 ] || { echo "error: ios16-root-r24.img is not r24 (e54d42c2...)" >&2; exit 1; }
W=$(mktemp -d /root/ipsw/.r25.XXXXXX)
trap 'rm -rf "$W"' EXIT

# 0. r24's backboardd and PosterBoard, for the byte comparison below
cat > "$W/old.sh" <<EOF
set -euo pipefail
mkdir -p "$W/old"
cp /mnt/r24/usr/libexec/backboardd /mnt/r24/Applications/PosterBoard.app/PosterBoard "$W/old/"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r24.img ro /mnt/r24 "$W/old.sh"
cp --sparse=always ios16-root-r24.img ios16-root-r25.img

# 1. write: bench, library, self-test job, /private/var/db/drg, backboardd's weak load command
cat > "$W/write.sh" <<EOF
set -euo pipefail
python3 $REPO/tools/drg-root-gpudraw.py /mnt/r25 "$BENCH" "$LIB"
cmp "$BENCH" /mnt/r25/usr/local/bin/drgles-bench
cmp "$LIB" /mnt/r25$EAGL
echo "write: done"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r25.img rw /mnt/r25 "$W/write.sh"

# 2. r24 gives r20's "Volume header needs minor repair" from this fsck; r25 must give r24's exact report
FB=$(fsck.hfsplus -fn ios16-root-r24.img 2>&1 | sed 1d || true)
FN=$(fsck.hfsplus -fn ios16-root-r25.img 2>&1 | sed 1d || true)
echo "$FN" | grep -E 'needs|repair|corrupt' || true
[ "$FB" = "$FN" ] || { echo "error: fsck of r25 differs from r24's" >&2; diff <(echo "$FB") <(echo "$FN") >&2; exit 1; }
echo "fsck: same report as r24"

# 3. read-only: copy out what we added or changed, with owner and mode, then check the copies here
cat > "$W/read.sh" <<EOF
set -euo pipefail
R=/mnt/r25
ls -ln \$R/usr/local/bin/drgles-bench \$R/usr/local/lib/ \$R/usr/libexec/backboardd \
	\$R/Applications/PosterBoard.app/PosterBoard \$R/System/Library/LaunchDaemons/com.drg.gpuselftest.plist
ls -ldn \$R/private/var/db/drg; echo "verdict dir entries: \$(ls -A \$R/private/var/db/drg | wc -l)"
ls -ldn \$R/private/var/tmp
mkdir -p "$W/out"
cp \$R/usr/local/bin/drgles-bench \$R$EAGL \$R$POSTERLIB \$R/usr/libexec/backboardd \
	\$R/Applications/PosterBoard.app/PosterBoard \$R/System/Library/xpc/launchd.plist \
	\$R/System/Library/LaunchDaemons/com.drg.gpuselftest.plist \
	\$R/System/Library/LaunchDaemons/com.apple.backboardd.plist "$W/out/"
EOF
bash $REPO/tools/drg-hfs-run.sh ios16-root-r25.img ro /mnt/r25 "$W/read.sh"
O=$W/out
sha256sum "$O/drgles-bench" "$O/libdrgeagl.dylib" "$O/libdrgposter.dylib" "$O/backboardd" "$O/PosterBoard"
cmp "$BENCH" "$O/drgles-bench" && cmp "$LIB" "$O/libdrgeagl.dylib"
cmp "$W/old/PosterBoard" "$O/PosterBoard" && echo "PosterBoard: r24's, byte for byte"
python3 - "$O" <<'PY'
import plistlib, sys
o = sys.argv[1]
j = plistlib.load(open(o + '/launchd.plist', 'rb'))['LaunchDaemons']
for k in ('LaunchDaemons/com.drg.growfs', 'LaunchDaemons/com.drg.gpuselftest', 'LaunchDaemons/com.apple.backboardd',
          'LaunchAngels/com.apple.PosterBoard'):
    v = j['/System/Library/%s.plist' % k]
    print(k, v.get('ProgramArguments'), v.get('EnvironmentVariables'), 'Nice', v.get('Nice'),
          'LowPriorityIO', v.get('LowPriorityIO'), 'POSIXSpawnType', v.get('POSIXSpawnType'))
st = j['/System/Library/LaunchDaemons/com.drg.gpuselftest.plist']
own = plistlib.load(open(o + '/com.drg.gpuselftest.plist', 'rb'))
assert st == own, 'launchd cache and the job plist differ'
assert st['ProgramArguments'] == ['/usr/local/bin/drgles-bench', '-selftest-boot', '/private/var/db/drg/gpu-selftest', '60']
assert st['Nice'] == 10 and st['LowPriorityIO'] is True and 'POSIXSpawnType' not in st
assert st['UserName'] == 'root' and st['LaunchOnlyOnce'] is True and st['RunAtLoad'] is True
bb = j['/System/Library/LaunchDaemons/com.apple.backboardd.plist']
assert bb['ProgramArguments'] == ['/usr/libexec/backboardd'] and bb['UserName'] == 'mobile' and bb['KeepAlive'] is True
print('selftest job and backboardd job: OK')
PY
python3 $REPO/tools/drg-root-poster.py --check "$O/backboardd" $EAGL
python3 $REPO/tools/drg-root-poster.py --check "$O/PosterBoard" $POSTERLIB
python3 $REPO/tools/drg-root-poster.py --check "$W/old/backboardd" $EAGL >/dev/null 2>&1 &&
	{ echo "error: r24's backboardd already loads the library?" >&2; exit 1; } || true
llvm-otool-21 -L "$O/backboardd" | tail -1
# only the two header pages and the signature (its CodeDirectory's hashes) may differ from r24's backboardd
P=$(cmp -l "$W/old/backboardd" "$O/backboardd" | awk '{print int(($1-1)/4096)}' | uniq | tr '\n' ' ')
echo "backboardd 4 KiB pages changed: $P"
SIGPAGE=$(python3 - "$O/backboardd" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
ncmds = struct.unpack_from('<I', d, 16)[0]; off = 32
for _ in range(ncmds):
    c, cs = struct.unpack_from('<II', d, off)
    if c == 0x1d:
        a, n = struct.unpack_from('<II', d, off + 8); print(a // 4096, (a + n - 1) // 4096)
    off += cs
PY
)
for p in $P; do
	[ "$p" -le 1 ] || { [ "$p" -ge ${SIGPAGE% *} ] && [ "$p" -le ${SIGPAGE#* } ]; } ||
		{ echo "error: backboardd page $p changed outside the header and signature" >&2; exit 1; }
done
[ "$(stat -c %s "$W/old/backboardd")" = "$(stat -c %s "$O/backboardd")" ] || { echo "error: backboardd size changed" >&2; exit 1; }
echo "backboardd: changes only in the header (pages 0-1) and the signature (pages ${SIGPAGE/ /-}); size unchanged"
for f in "$O/drgles-bench" "$O/libdrgeagl.dylib" "$O/libdrgposter.dylib"; do
	llvm-otool-21 -l "$f" | grep -q LC_CODE_SIGNATURE || { echo "error: $f not signed" >&2; exit 1; }
	llvm-otool-21 -l "$f" | grep -A4 LC_BUILD_VERSION | grep -q 'minos 16.0' || { echo "error: $f not iOS 16" >&2; exit 1; }
	echo "$(basename "$f"): signed, iOS 16; $(PATH=/usr/lib/llvm-21/bin:$PATH MIN_LINES=50 MIN_EXCLUSIVES=0 \
		bash $REPO/tools/check-armv80.sh "$f" | tail -1)"
done
# the library's install name is the path backboardd's load command names
llvm-otool-21 -D "$O/libdrgeagl.dylib" | tail -1 | grep -qx "$EAGL" || { echo "error: libdrgeagl install name" >&2; exit 1; }
echo "libdrgeagl install name: $EAGL"

/root/chunkhash ios16-root-r25.img 16384 > ios16-root-r25.chunks
echo "r25: $(sha256sum ios16-root-r25.img | cut -c1-64)"
echo "chunks differing from r24: $(diff <(cut -d' ' -f2 ios16-root-r24.chunks) <(cut -d' ' -f2 ios16-root-r25.chunks) | grep -c '^>')"
echo "differing chunks: $(paste -d' ' <(cut -d' ' -f2 ios16-root-r24.chunks) <(cut -d' ' -f2 ios16-root-r25.chunks) |
	awk '$1 != $2 {printf "%d ", NR-1}')"
