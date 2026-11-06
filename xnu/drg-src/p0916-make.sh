#!/bin/bash
# Build patch 0916 (DrgBattery reads the PM660 fuel gauge) from the kept tree's 0908 version.
set -e
T=/root/drgxnu/tree-0904
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
F=iokit/Kernel/arm/DrgBattery.cpp
W=/root/drgxnu/p0916; rm -rf $W; mkdir -p $W/a/$(dirname $F) $W/b/$(dirname $F)
cp $T/$F $W/a/$F; cp $REPO/xnu/drg-src/$F $W/b/$F
cd $W
{ cat $REPO/xnu/drg-src/p0916-header.txt; echo
  git diff --no-index a/$F b/$F | sed -e "s|^diff --git a/a/|diff --git a/|; s| b/b/| b/|; s|^--- a/a/|--- a/|; s|^+++ b/b/|+++ b/|" || true; } > $REPO/xnu/patches-drg/0916-drg-battery-fuel-gauge.patch
grep -n '^diff --git\|^---\|^+++' $REPO/xnu/patches-drg/0916-drg-battery-fuel-gauge.patch
