#!/bin/bash
# Bring the PC's copy of the phone's iOS volume up to date in place: only the
# 1 MiB chunks that differ from the copy's own hash list are pulled and
# written, and only those are hashed again. Run in Git Bash with the phone in
# TWRP (RAM boot).
#   drg-pull-live.sh COPY_IMAGE_GITBASH
# COPY_IMAGE.chunks is the copy's hash list (tools/chunkhash format), kept
# next to it; seed it once from a verified pull (drg-pull-changed.sh's
# logs/pull/phone.txt). drg-pull-changed.sh copied 16 GiB and hashed 16 GiB
# for each pull (boot #36: 19 min for 102 MiB of changes).
set -euo pipefail
COPY=$1
LIST=$COPY.chunks
N=16384                                  # 16 GiB in 1 MiB chunks
WORK=/f/Nokia-HackiOs/logs/pull
mkdir -p $WORK
[ -s "$LIST" ] && [ "$(wc -l < "$LIST")" = $N ] || { echo "no hash list $LIST"; exit 1; }
DEV=$(MSYS_NO_PATHCONV=1 adb shell 'readlink -f /dev/block/bootdevice/by-name/userdata' | tr -d '\r')
[ "$(MSYS_NO_PATHCONV=1 adb shell "grep -c $DEV /proc/mounts" | tr -d '\r')" = 0 ] || { echo "userdata is mounted"; exit 1; }
MSYS_NO_PATHCONV=1 adb push F:/Nokia-HackiOs/ios-sweet/tools/chunkhash.arm64 /tmp/chunkhash >/dev/null
MSYS_NO_PATHCONV=1 adb shell chmod 755 /tmp/chunkhash

echo "$(date +%T) hashing on the phone"
MSYS_NO_PATHCONV=1 adb shell "/tmp/chunkhash $DEV $N" | tr -d '\r' > $WORK/phone.txt
[ "$(wc -l < $WORK/phone.txt)" = $N ] || { echo "phone list incomplete"; exit 1; }
python -c "
import sys
b=[l.split()[1] for l in open(sys.argv[1])]; p=[l.split()[1] for l in open(sys.argv[2])]
runs=[]; i=0
while i<len(p):
    if b[i]!=p[i]:
        j=i
        while j<len(p) and b[j]!=p[j]: j+=1
        runs.append((i,j-i)); i=j
    else: i+=1
print('\n'.join(f'{s} {n}' for s,n in runs))" "$LIST" $WORK/phone.txt > $WORK/runs.txt
echo "$(date +%T) changed: $(awk '{s+=$2} END {print s+0}' $WORK/runs.txt) MiB in $(wc -l < $WORK/runs.txt) runs"

# runs.txt comes from Windows Python: strip CR, or count= gets "9\r" and dd sends nothing
tr -d '\r' < $WORK/runs.txt | while read -r s n; do
	MSYS_NO_PATHCONV=1 adb exec-out "dd if=$DEV bs=1048576 skip=$s count=$n 2>/dev/null" > $WORK/run.bin
	[ "$(stat -c %s $WORK/run.bin)" -eq $((n * 1048576)) ] || { echo "short pull at chunk $s"; exit 1; }
	dd if=$WORK/run.bin of="$COPY" bs=1048576 seek=$s conv=notrunc status=none
	# the written chunks, hashed from the copy, must be the phone's
	python -c "
import struct, sys
s, n = int(sys.argv[2]), int(sys.argv[3])
want = [l.split()[1] for l in open(sys.argv[4])][s:s + n]
M = (1 << 64) - 1
with open(sys.argv[1], 'rb') as f:
    f.seek(s << 20)
    for i in range(n):
        h = 0xcbf29ce484222325 ^ (s + i)	# chunkhash.c: FNV-1a over 64-bit words, seeded with the index
        for w in struct.unpack('<131072Q', f.read(1 << 20)):
            h = ((h ^ w) * 0x100000001b3) & M
        if '%016x' % h != want[i]:
            sys.exit('MISMATCH at chunk %d' % (s + i))" "$COPY" $s $n $WORK/phone.txt
done
rm -f $WORK/run.bin
cp $WORK/phone.txt "$LIST"
echo "$(date +%T) OK: copy equals the phone (changed chunks verified, list updated)"
