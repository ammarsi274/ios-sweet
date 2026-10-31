#!/bin/bash
# Build patch 0911 from the kept tree (0900-0910 applied) and dry-apply the series.
set -e
T=/root/drgxnu/tree-0904
O=/root/drgxnu/p0911-orig; N=/root/drgxnu/p0911-new
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
SRC=$REPO/xnu/drg-src
FILES="bsd/conf/files iokit/conf/files.arm64 iokit/KernelConfigTables.cpp iokit/Kernel/arm/DrgGPU.cpp
bsd/dev/drgpu/drgpu.c bsd/dev/drgpu/drgpu.h bsd/dev/drgpu/drgpu_os.h bsd/dev/drgpu/drgpu_pt.c
bsd/dev/drgpu/drgpu_pt.h bsd/dev/drgpu/drgpu_rpm.c bsd/dev/drgpu/drgpu_start.h
bsd/dev/drgpu/drgpu_fwblob.h bsd/dev/drgpu/drgpu_uc.h"
cd $T
# re-runnable: put back what an earlier run changed (originals in $O; new files removed)
if [ -d $O ]; then
	for f in $FILES; do if [ -e $O/$f ]; then cp $O/$f $f; else rm -f $f; fi; done
fi
rm -rf $O $N
for f in $FILES; do mkdir -p $O/$(dirname $f); [ -e $f ] && cp $f $O/$f; done; true
python3 $SRC/p0911-hooks.py $REPO
for f in $FILES; do mkdir -p $N/$(dirname $f); cp $f $N/$f; done
cd /root/drgxnu
OUT=$REPO/xnu/patches-drg/0911-drg-gpu.patch
{ cat $SRC/p0911-header.txt; echo
  git diff --no-index p0911-orig p0911-new | sed -e 's|a/p0911-orig/|a/|g; s|b/p0911-new/|b/|g; s|a/p0911-new/|a/|g'; } > $OUT || true
grep -n "^diff --git" $OUT
rm -rf /root/drgxnu/series-test
cd $REPO
cp -a xnu/patches /root/drgxnu/series-test
python3 xnu/drg-overlay.py /root/drgxnu/series-test | tail -1
bash tools/test-apply-series.sh /root/drgxnu/xnu-src /root/drgxnu/dxb /root/drgxnu/series-test DRG 2>&1 | tail -1
