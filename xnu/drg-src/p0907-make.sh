#!/bin/bash
# Build patch 0907 from the kept tree (0900-0906 applied) and dry-apply the series.
set -e
T=/root/drgxnu/tree-0904
O=/root/drgxnu/p0907-orig; N=/root/drgxnu/p0907-new
SRC=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/drg-src
FILES="bsd/dev/drg_keys.c bsd/dev/drg_emmc.c iokit/Kernel/arm/DrgFramebuffer.cpp"
rm -rf $O $N
cd $T
for f in $FILES; do mkdir -p $O/$(dirname $f); cp $f $O/$f; done
python3 $SRC/p0907-hooks.py $SRC/bsd/dev/drg_keys.c
for f in $FILES; do mkdir -p $N/$(dirname $f); cp $f $N/$f; done
cd /root/drgxnu
OUT=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/patches-drg/0907-drg-nokia-keys-layer-blend.patch
{ cat $SRC/p0907-header.txt; echo
  git diff --no-index p0907-orig p0907-new | sed -e 's|a/p0907-orig/|a/|g; s|b/p0907-new/|b/|g; s|a/p0907-new/|a/|g'; } > $OUT || true
grep -n "^diff --git" $OUT
rm -rf /root/drgxnu/series-test
cd /mnt/f/Nokia-HackiOs/ios-sweet
cp -a xnu/patches /root/drgxnu/series-test
python3 xnu/drg-overlay.py /root/drgxnu/series-test | tail -1
bash tools/test-apply-series.sh /root/drgxnu/xnu-src /root/drgxnu/dxb /root/drgxnu/series-test DRG 2>&1 | tail -1
