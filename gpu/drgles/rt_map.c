/*
 * drgles render-target map (bench: the iOS self-test and TWRP -gbufframes):
 * which texels of the position pattern a GPU-rendered buffer holds, and
 * what the wrong ones hold. Pattern of frame k (BGRA memory): byte 0 =
 * (y + k) & 0xff, 1 = x >> 8, 2 = x & 0xff, 3 = y >> 8 (RGBA: bytes 0 and 2
 * swapped). No GL here: host-tested (test/rt_map_test.c).
 */
#include "rt_map.h"
#include "host.h"
#include "rt.h"

/* What texel (x, y) holds, as a class: 0 right, 1 zero, 2 clear colour, 3 older frame, 4 other position, 5 other */
int rt_class(const uint8_t *t, int x, int y, int k, int bgra, int *dk, int *dx, int *dy)
{
	int b = bgra ? t[0] : t[2], g = t[1], r = bgra ? t[2] : t[0], a = t[3];
	if (b == ((y + k) & 0xff) && g == (x >> 8) && r == (x & 0xff) && a == (y >> 8))
		return 0;
	if (!b && !g && !r && !a)
		return 1;
	if (b == 0x33 && g == 0x33 && r == 0x33 && a == 0x33)
		return 2;
	if (g == (x >> 8) && r == (x & 0xff) && a == (y >> 8)) {
		*dk = (b - y) & 0xff;	/* the frame whose pattern this is (mod 256) */
		return 3;
	}
	if (a <= 9 && g <= 4) {	/* a pattern texel of another position (frame k assumed) */
		int px = g << 8 | r, py = a << 8 | ((b - k) & 0xff);
		*dx = px - x, *dy = py - y;
		return 4;
	}
	return 5;
}

/*
 * Logs (prefix tag): the count; classes of the wrong texels (zero / clear /
 * older frame k' / other position dx,dy / other) with the most common k' and
 * offset; a 64x64-block map ('#' all right, '.' none, '+' some, one line per
 * block row, GL row 0 first); then the right texels of each row as column
 * runs, one line per band of rows with the same runs (at most 64 lines):
 * enough to tell bins (rectangles), address ranges (row prefixes), pages and
 * timing apart. Returns the number of right texels.
 */
long rt_report(const char *tag, const uint8_t *m, size_t bpr, int w, int h, int k, int bgra)
{
	long good = 0, cls[6] = { 0 };
	enum { NK = 8 };
	int kv[NK], dxv[NK], dyv[NK];
	long kn[NK] = { 0 }, on[NK] = { 0 };
	int nk = 0, no = 0, fx = -1, fy = -1;
	uint32_t fval = 0;
	enum { B = 64 };
	int bw = (w + B - 1) / B, bh = (h + B - 1) / B;
	if (bw > 64)
		bw = 64;
	static uint32_t blk[64 * 64][2];	/* right, total per block (at most 64 x 64 blocks logged) */
	for (int i = 0; i < 64 * 64; i++)
		blk[i][0] = blk[i][1] = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const uint8_t *t = m + (size_t)y * bpr + (size_t)x * 4;
			int dk = 0, dx = 0, dy = 0, c = rt_class(t, x, y, k, bgra, &dk, &dx, &dy);
			cls[c]++;
			int bi = (y / B) * 64 + x / B;
			if (y / B < 64 && x / B < 64)
				blk[bi][0] += !c, blk[bi][1]++;
			if (!c) {
				good++;
				continue;
			}
			if (fx < 0)
				fx = x, fy = y, fval = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
			if (c == 3) {
				int i = 0;
				while (i < nk && kv[i] != dk)
					i++;
				if (i == nk && nk < NK)
					kv[nk++] = dk;
				if (i < nk)
					kn[i]++;
			}
			if (c == 4) {
				int i = 0;
				while (i < no && (dxv[i] != dx || dyv[i] != dy))
					i++;
				if (i == no && no < NK)
					dxv[no] = dx, dyv[no++] = dy;
				if (i < no)
					on[i]++;
			}
		}
	host_log("%s: %ld of %d texels right; wrong: zero %ld, clear colour %ld, older frame %ld, other position %ld, other %ld;"
		 " first wrong (%d,%d) = %08x (memory bytes, little-endian)\n",
		 tag, good, w * h, cls[1], cls[2], cls[3], cls[4], cls[5], fx, fy, fval);
	for (int i = 0; i < nk; i++)
		host_log("%s:   older frame: pattern of frame %d (mod 256) x%ld\n", tag, kv[i], kn[i]);
	for (int i = 0; i < no; i++)
		host_log("%s:   other position: texel of (x%+d, y%+d) x%ld\n", tag, dxv[i], dyv[i], on[i]);
	host_log("%s: %dx%d blocks of %dx%d, '#' all right '.' none '+' some; block row 0 = GL/memory rows 0-%d\n", tag, bw,
		 bh, B, B, B - 1);
	for (int by = 0; by < bh && by < 64; by++) {
		char line[80];
		int n = snprintf(line, sizeof(line), "%s:   %4d ", tag, by * B);
		for (int bx = 0; bx < bw && n < (int)sizeof(line) - 2; bx++) {
			const uint32_t *b = blk[by * 64 + bx];
			line[n++] = b[0] == b[1] ? '#' : !b[0] ? '.' : '+';
		}
		line[n] = 0;
		host_log("%s\n", line);
	}
	/* column runs of right texels per row; a line per band of equal rows */
	enum { NRUN = 8 };
	int prev[NRUN * 2] = { 0 }, np = -1, band = 0, lines = 0;
	for (int y = 0; y <= h; y++) {
		int cur[NRUN * 2] = { 0 }, nc = 0, more = 0;
		if (y < h) {
			for (int x = 0; x < w;) {
				const uint8_t *row = m + (size_t)y * bpr;
				int dk, dx, dy;
				while (x < w && rt_class(row + (size_t)x * 4, x, y, k, bgra, &dk, &dx, &dy))
					x++;
				if (x == w)
					break;
				int x0 = x;
				while (x < w && !rt_class(row + (size_t)x * 4, x, y, k, bgra, &dk, &dx, &dy))
					x++;
				if (nc < NRUN * 2)
					cur[nc++] = x0, cur[nc++] = x - 1;
				else
					more = 1;
			}
			if (more)
				cur[NRUN * 2 - 1] = -cur[NRUN * 2 - 1] - 1;	/* marks "more runs" */
		}
		int same = np == nc;
		for (int i = 0; same && i < nc; i++)
			same = prev[i] == cur[i];
		if (y < h && same)
			continue;
		if (np >= 0 && lines < 64) {
			char line[200];
			int n = snprintf(line, sizeof(line), "%s:   rows %4d-%4d right x", tag, band, y - 1);
			for (int i = 0; i < np && n < (int)sizeof(line) - 24; i += 2)
				n += snprintf(line + n, sizeof(line) - n, " %d-%d", prev[i],
					      prev[i + 1] < 0 ? -prev[i + 1] - 1 : prev[i + 1]);
			if (np && prev[np - 1] < 0)
				snprintf(line + n, sizeof(line) - n, " ...");
			if (!np)
				snprintf(line + n, sizeof(line) - n, " none");
			host_log("%s\n", line);
			lines++;
		}
		for (int i = 0; i < nc; i++)
			prev[i] = cur[i];
		np = nc;
		band = y;
	}
	if (lines >= 64)
		host_log("%s:   (row bands after the 64th not logged)\n", tag);
	return good;
}
