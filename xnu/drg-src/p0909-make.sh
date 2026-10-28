#!/bin/bash
# Build patch 0909 from the kept tree (0900-0908 applied) and dry-apply the series.
set -e
T=/root/drgxnu/tree-0904
O=/root/drgxnu/p0909-orig; N=/root/drgxnu/p0909-new
SRC=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/drg-src
FILES="bsd/conf/files bsd/dev/drg_emmc.c bsd/dev/drg_ts.c iokit/Kernel/arm/hid/DrgHID.cpp"
rm -rf $O $N
cd $T
for f in $FILES; do mkdir -p $O/$(dirname $f); [ -e $f ] && cp $f $O/$f; done; true
python3 $SRC/p0909-hooks.py $SRC/bsd/dev/drg_ts.c
for f in $FILES; do mkdir -p $N/$(dirname $f); cp $f $N/$f; done
cd /root/drgxnu
OUT=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/patches-drg/0909-drg-nokia-touch.patch
{ cat $SRC/p0909-header.txt; echo
  git diff --no-index p0909-orig p0909-new | sed -e 's|a/p0909-orig/|a/|g; s|b/p0909-new/|b/|g; s|a/p0909-new/|a/|g'; } > $OUT || true
grep -n "^diff --git" $OUT
rm -rf /root/drgxnu/series-test
cd /mnt/f/Nokia-HackiOs/ios-sweet
cp -a xnu/patches /root/drgxnu/series-test
python3 xnu/drg-overlay.py /root/drgxnu/series-test | tail -1
bash tools/test-apply-series.sh /root/drgxnu/xnu-src /root/drgxnu/dxb /root/drgxnu/series-test DRG 2>&1 | tail -1
