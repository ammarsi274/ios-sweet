#!/usr/bin/env python3
"""Patch 0910: scale the presented iPhone X frame onto the Nokia panel (tree with 0900-0909)."""


def rep(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


p = 'iokit/Kernel/arm/DrgFramebuffer.cpp'
rep(p, ''' * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2400 part of it is shown on the 1080x2400 panel.
 * Boot-arg drgfb_native=1 presents the real panel size instead.''',
    ''' * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * whole frame is scaled onto the Nokia's 1080x2280 panel (OTM1911A): every
 * panel pixel takes the iPhone X pixel at its centre (fMapX/fMapY). Boot
 * #30 showed the earlier centred crop cutting 78 lines off the top and the
 * bottom (status bar, home indicator). Boot-arg drgfb_native=1 presents the
 * real panel size instead.''')
rep(p, '''	int32_t       fOffX, fOffY;             /* logical origin of the panel */''',
    '''	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	uint16_t     *fMapX, *fMapY;            /* panel pixel -> presented pixel */''')
rep(p, '''	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
''', '''	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	fMapX = IONew(uint16_t, fWidth);
	fMapY = IONew(uint16_t, fHeight);
	if (fMapX == NULL || fMapY == NULL) {
		IOLog("DRG FB: no scaling tables\\n");
		return false;
	}
	for (uint32_t x = 0; x < fWidth; x++) {
		fMapX[x] = (uint16_t)(((2 * x + 1) * fLogicalW) / (2 * fWidth));
	}
	for (uint32_t y = 0; y < fHeight; y++) {
		fMapY[y] = (uint16_t)(((2 * y + 1) * fLogicalH) / (2 * fHeight));
	}
''')

# The per-layer copy becomes a scaled one: the layer's rectangle stays in
# presented coordinates; each panel pixel whose presented pixel falls inside
# it samples the surface there.
rep(p, '''void
AppleCLCD::present(const uint8_t *rec)
{''', '''/*
 * One layer onto the panel, scaled: panel pixel (px, py) shows presented
 * pixel (mapx[px], mapy[py]); the layer covers presented [dx, dx + w) x
 * [dy, dy + h) and its pixel there comes from src at (sx - dx, sy - dy) on.
 */
static void
drg_fb_blit_scaled(uintptr_t panel, uint32_t panel_bpr, uint32_t pw, uint32_t ph,
    const uint16_t *mapx, const uint16_t *mapy, const uint8_t *src, uint32_t src_bpr,
    int32_t sx, int32_t sy, int32_t dx, int32_t dy, int32_t w, int32_t h, uint32_t fmt, bool blend)
{
	uint32_t px0 = 0, px1 = 0;
	while (px0 < pw && mapx[px0] < dx) {
		px0++;
	}
	px1 = px0;
	while (px1 < pw && mapx[px1] < dx + w) {
		px1++;
	}
	for (uint32_t py = 0; py < ph; py++) {
		int32_t ly = mapy[py];
		if (ly < dy || ly >= dy + h) {
			continue;
		}
		uint32_t *d = (uint32_t *)(void *)(panel + (uint64_t)py * panel_bpr);
		const uint32_t *s = (const uint32_t *)(const void *)(src + (uint64_t)(sy + ly - dy) * src_bpr);
		for (uint32_t px = px0; px < px1; px++) {
			uint32_t v = s[sx + mapx[px] - dx];
			if (fmt == DRG_FMT_W30R) {
				d[px] = 0xff000000u | drg_fb_xr10((v >> 20) & 0x3ff) << 16 |
				    drg_fb_xr10((v >> 10) & 0x3ff) << 8 | drg_fb_xr10(v & 0x3ff);
			} else if (!blend) {
				d[px] = v;
			} else {
				uint32_t a = v >> 24;
				if (a == 0xff) {
					d[px] = v;
				} else if (a != 0) {
					uint32_t o = d[px], k = 255 - a;
					uint32_t rb = ((o & 0x00ff00ffu) * k + 0x00800080u) >> 8 & 0x00ff00ffu;
					uint32_t g = ((o & 0x0000ff00u) * k + 0x00008000u) >> 8 & 0x0000ff00u;
					d[px] = v + (rb | g);
				}
			}
		}
	}
}

void
AppleCLCD::present(const uint8_t *rec)
{''')
rep(p, '''		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}''', '''		/* Clip in presented coordinates; the blit scales to the panel. */
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fLogicalW - dx) {
			w = (int32_t)fLogicalW - dx;
		}
		if (h > (int32_t)fLogicalH - dy) {
			h = (int32_t)fLogicalH - dy;
		}''')
rep(p, '''			drg_fb_blit((uint8_t *)(fPixels + (uint64_t)dy * fRowBytes + (uint64_t)dx * 4), fRowBytes,
			    (const uint8_t *)(src + s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4),
			    s->bytesPerRow, w, h, s->pixelFormat, drawn > 0);''',
    '''			drg_fb_blit_scaled(fPixels, fRowBytes, fWidth, fHeight, fMapX, fMapY,
			    (const uint8_t *)(src + s->offset), s->bytesPerRow, sx, sy, dx, dy, w, h,
			    s->pixelFormat, drawn > 0);''')

# Touch: the whole presented frame is on the panel now, so a panel position
# is the same fraction of the presented display.
p = 'iokit/Kernel/arm/hid/DrgHID.cpp'
rep(p, '''	int native = 0;
	PE_parse_boot_argn("drgfb_native", &native, sizeof(native));
	fLogicalW = native ? 1080 : 1125;
	fLogicalH = native ? 2280 : 2436;
	fOffX = (fLogicalW - 1080) / 2;
	fOffY = (fLogicalH - 2280) / 2;''', '''	/*
	 * The presented frame (iPhone X size, or the panel with drgfb_native=1)
	 * is scaled onto the whole 1080x2280 panel (DrgFramebuffer.cpp), so a
	 * panel position is the same fraction of the presented display.
	 */
	fLogicalW = 1080;
	fLogicalH = 2280;
	fOffX = 0;
	fOffY = 0;''')
print('0910 changes applied')
