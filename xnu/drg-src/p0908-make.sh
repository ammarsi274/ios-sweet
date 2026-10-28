#!/bin/bash
# Build patch 0908 from the kept tree (0900-0907 applied) and dry-apply the series.
set -e
T=/root/drgxnu/tree-0904
O=/root/drgxnu/p0908-orig; N=/root/drgxnu/p0908-new
SRC=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/drg-src
FILES="bsd/conf/files osfmk/console/serial_console.c iokit/bsddev/IOKitBSDInit.cpp iokit/Kernel/arm/DrgBattery.cpp"
rm -rf $O $N
cd $T
for f in $FILES; do mkdir -p $O/$(dirname $f); cp $f $O/$f; done
python3 $SRC/p0908-hooks.py $SRC/iokit/Kernel/arm/DrgBattery.cpp
for f in $FILES; do mkdir -p $N/$(dirname $f); cp $f $N/$f; done
cd /root/drgxnu
OUT=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/patches-drg/0908-drg-nokia-only.patch
{ cat $SRC/p0908-header.txt; echo
  git diff --no-index p0908-orig p0908-new | sed -e 's|a/p0908-orig/|a/|g; s|b/p0908-new/|b/|g; s|a/p0908-new/|a/|g'; } > $OUT || true
grep -n "^diff --git" $OUT
rm -rf /root/drgxnu/series-test
cd /mnt/f/Nokia-HackiOs/ios-sweet
cp -a xnu/patches /root/drgxnu/series-test
python3 xnu/drg-overlay.py /root/drgxnu/series-test | tail -1
bash tools/test-apply-series.sh /root/drgxnu/xnu-src /root/drgxnu/dxb /root/drgxnu/series-test DRG 2>&1 | tail -1
