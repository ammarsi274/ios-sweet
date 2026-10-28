#!/usr/bin/env python3
"""Patch 0909: Nokia touch (Goodix GT915L) driver and panel values (tree with 0900-0908).

  p0909-hooks.py DRG_TS_C
"""
import shutil, sys


def rep(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


shutil.copy(sys.argv[1], 'bsd/dev/drg_ts.c')
rep('bsd/conf/files', 'bsd/dev/drg_emmc.c\t\t\tstandard\n',
    'bsd/dev/drg_emmc.c\t\t\tstandard\nbsd/dev/drg_ts.c\t\t\tstandard\n')

p = 'bsd/dev/drg_emmc.c'
rep(p, 'void drg_keys_init(void);                                      /* bsd/dev/drg_keys.c */\n',
    'void drg_keys_init(void);                                      /* bsd/dev/drg_keys.c */\n'
    'void drg_ts_init(void);                                        /* bsd/dev/drg_ts.c */\n')
rep(p, '\tdrg_keys_init();\n', '\tdrg_keys_init();\n\t/* Nokia touch screen, brought up by the shim. */\n\tdrg_ts_init();\n')

# The touch service still carried the other phone's 2400-line panel and its
# size. The Nokia panel (DRG CTC OTM1911A) is 1080 x 2280, 63 x 134 mm
# (qcom,mdss-dsi-panel-width/height, qcom,mdss-pan-physical-*-dimension).
p = 'iokit/Kernel/arm/hid/DrgHID.cpp'
rep(p, 'y = y < 0 ? 0 : (y > 2399 ? 2399 : y);', 'y = y < 0 ? 0 : (y > 2279 ? 2279 : y);')
rep(p, 'y < DRG_TOUCH_EDGE_PX || y > 2399 - DRG_TOUCH_EDGE_PX;', 'y < DRG_TOUCH_EDGE_PX || y > 2279 - DRG_TOUCH_EDGE_PX;')
rep(p, 'setProperty("SurfaceWidth", (unsigned long long)(68 << 16), 32);\t/* mm, 16.16 */',
    'setProperty("SurfaceWidth", (unsigned long long)(63 << 16), 32);\t/* mm, 16.16: OTM1911A panel */')
rep(p, 'setProperty("SurfaceHeight", (unsigned long long)(151 << 16), 32);',
    'setProperty("SurfaceHeight", (unsigned long long)(134 << 16), 32);')
print('0909 changes applied')
