#!/bin/bash
# Copy the iOS volume off the phone fast: only the 1 MiB chunks that differ
# from the image last flashed. Run in Git Bash with the phone in TWRP (RAM boot).
#   drg-pull-changed.sh BASE_IMAGE_WSL OUT_IMAGE_WIN
#   BASE_IMAGE_WSL  path inside WSL of the image the phone held after the last flash
#   OUT_IMAGE_WIN   Git Bash path for the result (a copy of the phone's first 16 GiB)
# Checks: every chunk of the result is hashed again and compared with the phone's list.
set -euo pipefail
BASE=$1
OUT=$2
N=16384                                  # 16 GiB in 1 MiB chunks
T=/f/Nokia-HackiOs/ios-sweet/tools
W=/mnt/f/Nokia-HackiOs/ios-sweet/tools
WORK=/f/Nokia-HackiOs/logs/pull
mkdir -p $WORK

OUTW=/mnt$OUT                            # the same file as seen from WSL
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "cd $W && aarch64-linux-gnu-gcc -O2 -static -o chunkhash.arm64 chunkhash.c && gcc -O2 -o /root/chunkhash chunkhash.c && sync"
for i in $(seq 1 20); do [ -s $T/chunkhash.arm64 ] && break; sleep 1; done
DEV=$(MSYS_NO_PATHCONV=1 adb shell 'readlink -f /dev/block/bootdevice/by-name/userdata' | tr -d '\r')
echo "userdata = $DEV, mounted: $(MSYS_NO_PATHCONV=1 adb shell "grep -c $DEV /proc/mounts" | tr -d '\r')"
MSYS_NO_PATHCONV=1 adb push F:/Nokia-HackiOs/ios-sweet/tools/chunkhash.arm64 /tmp/chunkhash >/dev/null
MSYS_NO_PATHCONV=1 adb shell chmod 755 /tmp/chunkhash

echo "$(date +%T) hashing on the phone and on the PC"
MSYS_NO_PATHCONV=1 adb shell "/tmp/chunkhash $DEV $N" | tr -d '\r' > $WORK/phone.txt &
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "/root/chunkhash $BASE $N" > $WORK/base.txt
wait
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
print('\n'.join(f'{s} {n}' for s,n in runs))" $WORK/base.txt $WORK/phone.txt > $WORK/runs.txt
echo "$(date +%T) changed: $(awk '{s+=$2} END {print s+0}' $WORK/runs.txt) MiB in $(wc -l < $WORK/runs.txt) runs"

echo "$(date +%T) copying base image"
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "cp $BASE $OUTW"
echo "$(date +%T) pulling changed chunks"
# runs.txt comes from Windows Python: strip CR, or count= gets "9\r" and dd sends nothing
tr -d '\r' < $WORK/runs.txt | while read -r s n; do
	MSYS_NO_PATHCONV=1 adb exec-out "dd if=$DEV bs=1048576 skip=$s count=$n 2>/dev/null" > $WORK/run.bin
	[ "$(stat -c %s $WORK/run.bin)" -eq $((n * 1048576)) ] || { echo "short pull at chunk $s"; exit 1; }
	dd if=$WORK/run.bin of=$OUT bs=1048576 seek=$s conv=notrunc status=none
done
rm -f $WORK/run.bin

echo "$(date +%T) verifying every chunk against the phone's list"
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "/root/chunkhash $OUTW $N" > $WORK/out.txt
if cmp -s $WORK/out.txt $WORK/phone.txt; then echo "$(date +%T) OK: copy equals the phone (all $N chunks)"; else echo "MISMATCH"; exit 1; fi
