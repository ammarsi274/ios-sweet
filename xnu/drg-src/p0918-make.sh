#!/bin/bash
# Build patch 0918 (USB device + ECM on DRG addresses) from the kept tree.
set -e
T=/root/drgxnu/tree-0904
REPO=/mnt/f/Nokia-HackiOs/ios-sweet
W=/root/drgxnu/p0918; rm -rf $W
for f in bsd/dev/drg_usb.c bsd/conf/files bsd/dev/drg_emmc.c; do mkdir -p $W/a/$(dirname $f) $W/b/$(dirname $f); cp $T/$f $W/a/$f; cp $T/$f $W/b/$f; done
python3 - $W/b <<'PY'
import sys
b = sys.argv[1]
def edit(f, pairs):
    p = b + '/' + f; s = open(p).read()
    for a, n in pairs:
        assert s.count(a) == 1, (f, a)
        s = s.replace(a, n)
    open(p, 'w').write(s)
edit('bsd/dev/drg_usb.c', [
 (' * high-speed) at 0x0a600000 behind', ' * high-speed) at 0x0a800000 behind'),
 (' * QUSB2 PHY at 0x088e2000; its power domain', ' * QUSB2 v1 PHY at 0x0c012000; its power domain'),
 ('clocks come from GCC (offsets from Linux gcc-sdmmagpie.c)', 'clocks come from GCC (offsets from HMD gcc-sdm660.c)'),
 ('	{ "master", 0x0f010 },\n	{ "sleep", 0x0f014 },\n	{ "utmi", 0x0f018 },\n	{ "cfg-noc-axi", 0x0502c },\n	{ "aggre-axi", 0x8201c },\n	{ "ahb2phy", 0x6a004 },\n	{ "clkref", 0x8c010 },',
  '	{ "master", 0x0f008 },\n	{ "sleep", 0x0f00c },\n	{ "utmi", 0x0f010 },\n	{ "cfg-noc-axi", 0x05018 },\n	{ "aggre-axi", 0x0f03c },\n	{ "ahb2phy", 0x6a004 },\n	{ "clkref", 0x8800c },'),
 ('extern uint32_t drg_log_copy(uint64_t *pos, char *out, uint32_t max);\nextern uint64_t drg_log_position(void);',
  '/* the live log ring lives in the other phone\'s drg_ufs.c, not built here: an empty log (no live log yet) */\nstatic uint32_t drg_log_copy(uint64_t *pos __unused, char *out __unused, uint32_t max __unused) { return 0; }\nstatic uint64_t drg_log_position(void) { return 0; }'),
 ('#define DWC3_PA                 0x0a600000', '#define DWC3_PA                 0x0a800000'),
 ('#define QUSB2_PA                0x088e2000', '#define QUSB2_PA                0x0c012000'),
 ('#define QSCRATCH_PA             0x0a6f8800', '#define QSCRATCH_PA             0x0a8f8800'),
 ('QUSB2 PLL_STATUS 0x%08x PWR_CTRL1 0x%08x PORT_TUNE1 0x%08x TEST1 0x%08x', 'QUSB2 v1 PLL_STATUS 0x%08x PORT_POWERDOWN 0x%08x TUNE1 0x%08x TUNE2 0x%08x'),
 ('usb_rd(phy, 0x1a0), usb_rd(phy, 0x210), usb_rd(phy, 0x240), usb_rd(phy, 0x254)', 'usb_rd(phy, 0x38), usb_rd(phy, 0xb4), usb_rd(phy, 0x80), usb_rd(phy, 0x84)'),
])
edit('bsd/dev/drg_emmc.c', [
 ('	/* Nokia touch screen, brought up by the shim. */\n	drg_ts_init();\n	return 0;',
  '	/* Nokia touch screen, brought up by the shim. */\n	drg_ts_init();\n	/* USB device + ECM networking (patch 0918; boot-arg drgusb) */\n	extern void drg_usb_probe(void);\n	drg_usb_probe();\n	return 0;'),
])
p = b + '/bsd/conf/files'; s = open(p).read()
assert 'bsd/dev/drg_usb.c' not in s
a = [l for l in s.split('\n') if l.startswith('bsd/dev/drg_emmc.c')]
assert len(a) == 1, a
s = s.replace(a[0] + '\n', a[0] + '\n' + 'bsd/dev/drg_usb.c\t\t\tstandard\n')
open(p, 'w').write(s)
PY
grep -n '0x0a6\|0x088e' $W/b/bsd/dev/drg_usb.c && { echo "sweet address left"; exit 1; } || true
cd $W
{ cat $REPO/xnu/drg-src/p0918-header.txt; echo
  for f in bsd/conf/files bsd/dev/drg_emmc.c bsd/dev/drg_usb.c; do git diff --no-index a/$f b/$f | sed -e "s|a/a/|a/|g; s|b/b/|b/|g" || true; done; } > $REPO/xnu/patches-drg/0918-drg-usb-ecm.patch
grep -n '^diff --git' $REPO/xnu/patches-drg/0918-drg-usb-ecm.patch
