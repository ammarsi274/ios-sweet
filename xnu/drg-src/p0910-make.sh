#!/bin/bash
# Build patch 0910 from the kept tree (0900-0909 applied) and dry-apply the series.
set -e
T=/root/drgxnu/tree-0904
O=/root/drgxnu/p0910-orig; N=/root/drgxnu/p0910-new
SRC=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/drg-src
FILES="iokit/Kernel/arm/DrgFramebuffer.cpp iokit/Kernel/arm/hid/DrgHID.cpp"
rm -rf $O $N
cd $T
for f in $FILES; do mkdir -p $O/$(dirname $f); [ -e $f ] && cp $f $O/$f; done; true
python3 $SRC/p0910-hooks.py
for f in $FILES; do mkdir -p $N/$(dirname $f); cp $f $N/$f; done
cd /root/drgxnu
OUT=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/patches-drg/0910-drg-scale-to-panel.patch
{ cat $SRC/p0910-header.txt; echo
  git diff --no-index p0910-orig p0910-new | sed -e 's|a/p0910-orig/|a/|g; s|b/p0910-new/|b/|g; s|a/p0910-new/|a/|g'; } > $OUT || true
grep -n "^diff --git" $OUT
rm -rf /root/drgxnu/series-test
cd /mnt/f/Nokia-HackiOs/ios-sweet
cp -a xnu/patches /root/drgxnu/series-test
python3 xnu/drg-overlay.py /root/drgxnu/series-test | tail -1
bash tools/test-apply-series.sh /root/drgxnu/xnu-src /root/drgxnu/dxb /root/drgxnu/series-test DRG 2>&1 | tail -1
