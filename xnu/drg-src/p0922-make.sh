#!/bin/bash
# Build patch 0922 (touch fingers 2+ at 0x8157): drg_ts.c as patch 0909 creates it -> drg-src's.
set -e
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
W=/root/drgxnu/p0922; rm -rf $W
mkdir -p $W/a/bsd/dev $W/b/bsd/dev
(cd $W/a && patch -p1 --quiet < <(awk '/^diff --git a\/bsd\/dev\/drg_ts.c/{p=1;print;next} /^diff --git/{p=0} p' $REPO/xnu/patches-drg/0909-drg-nokia-touch.patch))
sed 's/\r$//' $REPO/xnu/drg-src/bsd/dev/drg_ts.c > $W/b/bsd/dev/drg_ts.c
cd $W
{ cat $REPO/xnu/drg-src/p0922-header.txt; echo
  git diff --no-index a/bsd/dev/drg_ts.c b/bsd/dev/drg_ts.c | sed -e "s|a/a/|a/|g; s|b/b/|b/|g" -e "/^old mode /d; /^new mode /d" || true; } > $REPO/xnu/patches-drg/0922-drg-touch-fingers.patch
grep -c '^[-+][^-+]' $REPO/xnu/patches-drg/0922-drg-touch-fingers.patch
