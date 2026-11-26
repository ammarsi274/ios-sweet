#!/bin/bash
# Build patch 0921 (console log ring, live log over USB) on top of the kept tree + 0918's bsd/conf/files.
set -e
T=/root/drgxnu/tree-0904
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
W=/root/drgxnu/p0921; rm -rf $W
mkdir -p $W/a/bsd/conf $W/b/bsd/conf $W/a/osfmk/console $W/b/osfmk/console $W/b/bsd/dev
cp $T/bsd/conf/files $W/a/bsd/conf/files
# 0918 adds drg_usb.c to bsd/conf/files first: 0921's "a" side is that state
(cd $W/a && patch -p1 --quiet < <(awk '/^diff --git a\/bsd\/conf\/files/{p=1} /^diff --git/ && !/bsd\/conf\/files/{p=0} p' $REPO/xnu/patches-drg/0918-drg-usb-ecm.patch))
grep -q 'bsd/dev/drg_usb.c' $W/a/bsd/conf/files
cp $W/a/bsd/conf/files $W/b/bsd/conf/files
python3 - $W/b/bsd/conf/files <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
a=[l for l in s.split('\n') if l.startswith('bsd/dev/drg_usb.c')]
assert len(a)==1, a
s=s.replace(a[0]+'\n', a[0]+'\n'+'bsd/dev/drg_log.c\t\t\tstandard\n')
open(p,'w').write(s)
PY
cp $T/osfmk/console/serial_console.c $W/a/osfmk/console/
sed 's/\r$//' $REPO/xnu/drg-src/osfmk/console/serial_console.c > $W/b/osfmk/console/serial_console.c
sed 's/\r$//' $REPO/xnu/drg-src/bsd/dev/drg_log.c > $W/b/bsd/dev/drg_log.c
cd $W
{ cat $REPO/xnu/drg-src/p0921-header.txt; echo
  for f in bsd/conf/files bsd/dev/drg_log.c osfmk/console/serial_console.c; do
    [ -e a/$f ] && A=a/$f || A=/dev/null
    git diff --no-index $A b/$f | sed -e "s|a/a/|a/|g; s|a/b/|a/|g; s|b/b/|b/|g; s|^--- a/dev/null|--- /dev/null|" || true
  done; } > $REPO/xnu/patches-drg/0921-drg-live-log.patch
grep -n '^diff --git\|^---\|^+++' $REPO/xnu/patches-drg/0921-drg-live-log.patch
