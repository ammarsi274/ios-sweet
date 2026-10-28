#!/usr/bin/env python3
"""Patch 0907 changes to an xnu tree with 0900-0906 applied (run in the tree).

  p0907-hooks.py DRG_KEYS_C
"""
import shutil, sys


def rep(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# 1. Nokia buttons: own driver, started once the eMMC root disk is ready
#    (the UFS driver that used to start it never runs on DRG).
shutil.copy(sys.argv[1], 'bsd/dev/drg_keys.c')
p = 'bsd/dev/drg_emmc.c'
rep(p, 'void drg_perf_emmc(int write, uint64_t bytes, uint64_t abs);   /* bsd/kern/drg_perf.c */\n',
    'void drg_perf_emmc(int write, uint64_t bytes, uint64_t abs);   /* bsd/kern/drg_perf.c */\n'
    'void drg_keys_init(void);                                      /* bsd/dev/drg_keys.c */\n')
rep(p, '\tprintf("DRG EMMC: emmcroot0 ready, %s\\n", se.writable ? "read-write" : "read-only");\n',
    '\tprintf("DRG EMMC: emmcroot0 ready, %s\\n", se.writable ? "read-write" : "read-only");\n'
    '\t/* Nokia buttons (iokit/Kernel/arm/hid/DrgHID.cpp gets the events). */\n'
    '\tdrg_keys_init();\n')

# 2. Display: blend layers instead of copying each over the last, and
#    convert w30r (10-bit) surfaces.
p = 'iokit/Kernel/arm/DrgFramebuffer.cpp'
rep(p, '''void
AppleCLCD::present(const uint8_t *rec)
{''', '''/*
 * Put one layer's rectangle on the panel (x8r8g8b8, little endian).
 * The display hardware of an iPhone blends its layers; this one did a plain
 * copy per layer, so the lock screen's mostly transparent top layer replaced
 * the wallpaper below it with black (boots #16-#20). Now:
 *   - 'BGRA' (premultiplied, the CoreAnimation output): the bottom layer is
 *     copied, every later layer is blended source-over by its alpha
 *   - 'w30r' (10-bit RGB, wide colour, what PosterBoard renders posters into):
 *     converted to 8 bits per channel, opaque. Assumed layout R 29:20,
 *     G 19:10, B 9:0, extended range 384..894 = 0..1.
 */
#define DRG_FMT_BGRA            0x42475241u     /* 'BGRA' */
#define DRG_FMT_W30R            0x77333072u     /* 'w30r' */

static inline uint32_t
drg_fb_xr10(uint32_t c)
{
	int32_t v = ((int32_t)c - 384) * 255 / 510;
	return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void
drg_fb_blit(uint8_t *dst, uint32_t dst_bpr, const uint8_t *src, uint32_t src_bpr,
    int32_t w, int32_t h, uint32_t fmt, bool blend)
{
	for (int32_t y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)(void *)(dst + (uint64_t)y * dst_bpr);
		const uint32_t *s = (const uint32_t *)(const void *)(src + (uint64_t)y * src_bpr);
		if (fmt == DRG_FMT_W30R) {
			for (int32_t x = 0; x < w; x++) {
				uint32_t v = s[x];
				d[x] = 0xff000000u | drg_fb_xr10((v >> 20) & 0x3ff) << 16 |
				    drg_fb_xr10((v >> 10) & 0x3ff) << 8 | drg_fb_xr10(v & 0x3ff);
			}
		} else if (!blend) {
			memcpy(d, s, (size_t)w * 4);
		} else {
			for (int32_t x = 0; x < w; x++) {
				uint32_t v = s[x], a = v >> 24;
				if (a == 0xff) {
					d[x] = v;
				} else if (a != 0) {
					uint32_t o = d[x], k = 255 - a;
					uint32_t rb = ((o & 0x00ff00ffu) * k + 0x00800080u) >> 8 & 0x00ff00ffu;
					uint32_t g = ((o & 0x0000ff00u) * k + 0x00008000u) >> 8 & 0x0000ff00u;
					d[x] = v + (rb | g);
				}
			}
		}
	}
}

void
AppleCLCD::present(const uint8_t *rec)
{''')
rep(p, '''	for (uint32_t layer = 0; layer < DRG_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);''', '''	uint32_t drawn = 0;
	for (uint32_t layer = 0; layer < DRG_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);''')
rep(p, '''		if (src != 0 && bpe == 4) {
			for (int32_t y = 0; y < h; y++) {
				memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
				    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
				    (size_t)w * 4);
			}
		}''', '''		if (src != 0 && bpe == 4) {
			drg_fb_blit((uint8_t *)(fPixels + (uint64_t)dy * fRowBytes + (uint64_t)dx * 4), fRowBytes,
			    (const uint8_t *)(src + s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4),
			    s->bytesPerRow, w, h, s->pixelFormat, drawn > 0);
			drawn++;
		}''')
print('0907 changes applied')
