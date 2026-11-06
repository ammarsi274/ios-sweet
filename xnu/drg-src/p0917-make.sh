#!/bin/bash
# Build patch 0917 (AppleARMBacklight) on top of the kept tree (0900-0911 applied).
set -e
T=/root/drgxnu/tree-0904
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
W=/root/drgxnu/p0917; rm -rf $W; mkdir -p $W/a/iokit/conf $W/b/iokit/conf $W/a/iokit/Kernel/arm $W/b/iokit/Kernel/arm
cp $T/iokit/KernelConfigTables.cpp $W/a/iokit/; cp $T/iokit/conf/files.arm64 $W/a/iokit/conf/
cp $T/iokit/KernelConfigTables.cpp $W/b/iokit/; cp $T/iokit/conf/files.arm64 $W/b/iokit/conf/
cp $REPO/xnu/drg-src/iokit/Kernel/arm/DrgBacklight.cpp $W/b/iokit/Kernel/arm/
python3 - $W/b <<'PY'
import sys
b = sys.argv[1]
p = b + '/iokit/KernelConfigTables.cpp'; s = open(p).read()
a = '''    "   {"
    "     'IOClass'         = DrgBattery;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = DrgBattery;"
    "   },"
'''
assert s.count(a) == 1
s = s.replace(a, a + '''    "   {"
    "     'IOClass'         = AppleARMBacklight;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = AppleARMBacklight;"
    "   },"
''')
open(p, 'w').write(s)
p = b + '/iokit/conf/files.arm64'; s = open(p).read()
a = 'iokit/Kernel/arm/DrgBattery.cpp                                optional iokitcpp\n'
assert s.count(a) == 1
s = s.replace(a, a + 'iokit/Kernel/arm/DrgBacklight.cpp                              optional iokitcpp\n')
open(p, 'w').write(s)
PY
cd $W
{ cat $REPO/xnu/drg-src/p0917-header.txt; echo
  for f in iokit/KernelConfigTables.cpp iokit/conf/files.arm64 iokit/Kernel/arm/DrgBacklight.cpp; do
    [ -e a/$f ] && A=a/$f || A=/dev/null
    git diff --no-index $A b/$f | sed -e "s|a/a/|a/|g; s|a/b/|a/|g; s|b/b/|b/|g; s|^--- a/dev/null|--- /dev/null|" || true
  done; } > $REPO/xnu/patches-drg/0917-drg-backlight.patch
grep -n '^diff --git\|^---\|^+++' $REPO/xnu/patches-drg/0917-drg-backlight.patch
