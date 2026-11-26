#!/bin/bash
# Build patch 0923 (IOSurface rows aligned to 256 bytes for the Adreno): DrgSurface.cpp as the series up to 0922
# leaves it (the precheck tree, checked equal to drg-src's file before this change) -> drg-src's.
set -e
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
F=iokit/Kernel/arm/DrgSurface.cpp
W=/root/drgxnu/p0923; rm -rf $W
mkdir -p $W/a/$(dirname $F) $W/b/$(dirname $F)
cp /root/drgxnu/p0923-base-DrgSurface.cpp $W/a/$F
sed 's/\r$//' $REPO/xnu/drg-src/$F > $W/b/$F
cd $W
{ cat $REPO/xnu/drg-src/p0923-header.txt; echo
  git diff --no-index a/$F b/$F | sed -e "s|a/a/|a/|g; s|b/b/|b/|g" -e "/^old mode /d; /^new mode /d" || true; } > $REPO/xnu/patches-drg/0923-drg-iosurface-row-align.patch
grep -c '^[-+][^-+]' $REPO/xnu/patches-drg/0923-drg-iosurface-row-align.patch
