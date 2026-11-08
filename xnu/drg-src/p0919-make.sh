#!/bin/bash
# Build patch 0919 (power off/restart, backlight with the display) from the kept tree.
set -e
T=/root/drgxnu/tree-0904
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
W=/root/drgxnu/p0919; rm -rf $W
for f in iokit/Kernel/arm/DrgPlatformExpert.cpp iokit/Kernel/arm/DrgFramebuffer.cpp; do
	mkdir -p $W/a/$(dirname $f) $W/b/$(dirname $f)
	cp $T/$f $W/a/$f
	sed 's/\r$//' $REPO/xnu/drg-src/$f > $W/b/$f
done
cd $W
{ cat $REPO/xnu/drg-src/p0919-header.txt; echo
  for f in iokit/Kernel/arm/DrgFramebuffer.cpp iokit/Kernel/arm/DrgPlatformExpert.cpp; do
    git diff --no-index a/$f b/$f | sed -e "s|a/a/|a/|g; s|b/b/|b/|g" || true
  done; } > $REPO/xnu/patches-drg/0919-drg-power-backlight.patch
grep -n '^diff --git' $REPO/xnu/patches-drg/0919-drg-power-backlight.patch
