#!/usr/bin/env python3
"""Patch 0911: DrgGPU, the Adreno 512 driver (tree with 0900-0910 applied).

Usage: p0911-hooks.py REPO   (REPO = the ios-sweet checkout; run in the XNU tree)
The portable core is copied byte for byte from REPO/gpu/drgpu (the source the
shim tests on the phone); DrgGPU.cpp from REPO/xnu/drg-src.
"""
import shutil
import sys
from pathlib import Path

REPO = Path(sys.argv[1])
CORE = ['drgpu.c', 'drgpu.h', 'drgpu_os.h', 'drgpu_pt.c', 'drgpu_pt.h', 'drgpu_rpm.c',
        'drgpu_start.h', 'drgpu_fwblob.h', 'drgpu_uc.h']


def rep(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


Path('bsd/dev/drgpu').mkdir(parents=True, exist_ok=True)
for f in CORE:
    shutil.copy(REPO / 'gpu/drgpu' / f, Path('bsd/dev/drgpu') / f)
shutil.copy(REPO / 'xnu/drg-src/iokit/Kernel/arm/DrgGPU.cpp', 'iokit/Kernel/arm/DrgGPU.cpp')

rep('bsd/conf/files', 'bsd/dev/drg_keys.c\t\t\tstandard\n',
    'bsd/dev/drg_keys.c\t\t\tstandard\n'
    'bsd/dev/drgpu/drgpu.c\t\t\tstandard\n'
    'bsd/dev/drgpu/drgpu_pt.c\t\tstandard\n'
    'bsd/dev/drgpu/drgpu_rpm.c\t\tstandard\n')
rep('iokit/conf/files.arm64',
    'iokit/Kernel/arm/DrgBattery.cpp                                optional iokitcpp\n',
    'iokit/Kernel/arm/DrgBattery.cpp                                optional iokitcpp\n'
    'iokit/Kernel/arm/DrgGPU.cpp                                    optional iokitcpp\n')
rep('iokit/KernelConfigTables.cpp', '''    "     'IOMatchCategory' = DrgBattery;"
    "   },"
''', '''    "     'IOMatchCategory' = DrgBattery;"
    "   },"
    "   {"
    "     'IOClass'         = DrgGPU;"
    "     'IOProviderClass' = IOResources;"
    "     'IOResourceMatch' = IOAllCPUInitialized;"
    "     'IOMatchCategory' = DrgGPU;"
    "   },"
''')
print('0911 changes applied')
