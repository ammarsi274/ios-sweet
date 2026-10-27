#!/bin/bash
# Build patch 0906 from the kept tree (0900-0905 applied) and dry-apply the series.
set -e
T=/root/drgxnu/tree-0904
O=/root/drgxnu/p0906-orig; N=/root/drgxnu/p0906-new
SRC=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/drg-src
FILES="bsd/conf/files osfmk/arm64/sleh.c osfmk/arm64/drg_lse_emul.h bsd/dev/drg_emmc.c iokit/Kernel/arm/DrgFramebuffer.cpp iokit/Kernel/arm/DrgKeyStore.cpp"
rm -rf $O $N
cd $T
for f in $FILES; do mkdir -p $O/$(dirname $f); cp $f $O/$f; done
python3 $SRC/p0906-hooks.py $SRC/bsd/kern/drg_perf.c
for f in $FILES bsd/kern/drg_perf.c; do mkdir -p $N/$(dirname $f); cp $f $N/$f; done
cd /root/drgxnu
OUT=/mnt/f/Nokia-HackiOs/ios-sweet/xnu/patches-drg/0906-drg-perf-counters.patch
{ cat $SRC/p0906-header.txt; echo
  git diff --no-index p0906-orig p0906-new | sed -e 's|a/p0906-orig/|a/|g; s|b/p0906-new/|b/|g; s|a/p0906-new/|a/|g'; } > $OUT || true
grep -n "^diff --git" $OUT
rm -rf /root/drgxnu/series-test
cd /mnt/f/Nokia-HackiOs/ios-sweet
cp -a xnu/patches /root/drgxnu/series-test
python3 xnu/drg-overlay.py /root/drgxnu/series-test | tail -1
bash tools/test-apply-series.sh /root/drgxnu/xnu-src /root/drgxnu/dxb /root/drgxnu/series-test DRG 2>&1 | tail -1
