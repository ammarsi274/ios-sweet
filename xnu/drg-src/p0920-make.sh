#!/bin/bash
# Build patch 0920 (IOSurface wrap/child selectors refused) from the kept tree.
set -e
T=/root/drgxnu/tree-0904
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
W=/root/drgxnu/p0920; rm -rf $W
f=iokit/Kernel/arm/DrgSurface.cpp
mkdir -p $W/a/$(dirname $f) $W/b/$(dirname $f)
cp $T/$f $W/a/$f
sed 's/\r$//' $REPO/xnu/drg-src/$f > $W/b/$f
cd $W
{ cat $REPO/xnu/drg-src/p0920-header.txt; echo
  git diff --no-index a/$f b/$f | sed -e "s|a/a/|a/|g; s|b/b/|b/|g" || true; } > $REPO/xnu/patches-drg/0920-drg-iosurface-wrap-refused.patch
grep -n '^diff --git' $REPO/xnu/patches-drg/0920-drg-iosurface-wrap-refused.patch
