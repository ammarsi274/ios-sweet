#!/usr/bin/env python3
"""Patch 0908 changes: a Nokia-only DRG kernel (run in a tree with 0900-0907).

  p0908-hooks.py DRG_BATTERY_CPP
"""
import shutil, sys


def rep(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Drivers for the other phone's hardware leave the DRG build: its UFS
# storage (with the console log and sampler it started), touch controller
# on GENI I2C, RPMh regulators, SPMI v5 arbiter and USB controller. None of
# them exists on the Nokia (eMMC, BLSP QUP I2C, RPM, SPMI v3).
for f in ('drg_ufs.c', 'drg_sample.c', 'drg_touch.c', 'drg_rpmh.c', 'drg_spmi.c', 'drg_usb.c'):
    rep('bsd/conf/files', f'bsd/dev/{f}\t\t\tstandard\n', '')

p = 'osfmk/console/serial_console.c'
rep(p, 'extern void drg_log_append(const char *c, int size);  /* bsd/dev/drg_ufs.c */\n', '')
rep(p, ' * console text still goes to the persistent log, but is not drawn over\n'
       ' * the GUI. A panic draws again.\n',
    ' * console text is not drawn over the GUI. A panic draws again.\n')
rep(p, '\tdrg_log_append(c, size);\n', '')

p = 'iokit/bsddev/IOKitBSDInit.cpp'
rep(p, '''//
//	DRG: root on a GPT partition of the internal UFS (bsd/dev/drg_ufs.c)
//
	if (strcmp(rdBootVar, "ufsroot0") == 0) {
		extern int drg_ufs_rootdev(dev_t *dev);
		OSSafeReleaseNULL(matching);
		int ufs_error = drg_ufs_rootdev(root);
		if (ufs_error) {
			panic("IOFindBSDRoot: DRG UFS root %s unavailable: %d", rdBootVar, ufs_error);
		}
		strlcpy(rootName, rdBootVar, rootNameSize);
		IOLog("BSD root: %s, major %d, minor %d\\n", rootName, major(*root), minor(*root));
		*oflags = 0;
		goto iofrootx;
	}
''', '')

# Battery: static Nokia profile, no PMIC access (was the other phone's PMIC).
shutil.copy(sys.argv[1], 'iokit/Kernel/arm/DrgBattery.cpp')
print('0908 changes applied')
