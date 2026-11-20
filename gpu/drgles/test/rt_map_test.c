/*
 * Host test of rt_map.c (the render-target map the iOS self-test and TWRP -gbufframes log).
 *   gcc -O1 -Wall -Wextra -I.. -o /tmp/rmt rt_map_test.c ../rt_map.c && /tmp/rmt
 * Buffers: 1125x2436 BGRA, 4608 bytes per row, as boot #55/#56. Cases: all right; the bin hypothesis
 * of tools/rt-model.py (288x224 bins, only the first 21 in row order drawn: exactly 1,324,512, the
 * phone's count); one bin from an older frame; one bin shifted by a row; clear colour and junk.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rt_map.h"

static int fails, quiet;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char logbuf[1 << 16];
static size_t loglen;

void host_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
	va_end(ap);
	if (n > 0 && loglen + (size_t)n < sizeof(logbuf))
		loglen += (size_t)n;
}

enum { W = 1125, H = 2436, BPR = 4608, K = 59 };
static uint8_t *buf;

static void put(int x, int y, int k)
{
	uint8_t *t = buf + (size_t)y * BPR + (size_t)x * 4;
	t[0] = (uint8_t)(y + k), t[1] = (uint8_t)(x >> 8), t[2] = (uint8_t)x, t[3] = (uint8_t)(y >> 8);
}

static void fill(int x0, int y0, int x1, int y1, int k)
{
	for (int y = y0; y < y1 && y < H; y++)
		for (int x = x0; x < x1 && x < W; x++)
			put(x, y, k);
}

static long run(const char *name)
{
	loglen = 0;
	long n = rt_report("t", buf, BPR, W, H, K, 1);
	if (!quiet)
		printf("---- %s\n%s", name, logbuf);
	return n;
}

int main(int argc, char **argv)
{
	quiet = argc > 1 && !strcmp(argv[1], "-q");
	buf = calloc((size_t)BPR * H, 1);

	fill(0, 0, W, H, K);
	CHECK(run("all right") == (long)W * H);
	CHECK(strstr(logbuf, "rows    0-2435 right x 0-1124\n") != 0);
	CHECK(strstr(logbuf, "zero 0, clear colour 0, older frame 0, other position 0, other 0") != 0);

	/* 288x224 bins, 4 x 11; bins 0..20 in row order drawn, the rest never written (zero) */
	memset(buf, 0, (size_t)BPR * H);
	for (int b = 0; b < 21; b++)
		fill(b % 4 * 288, b / 4 * 224, b % 4 * 288 + 288, b / 4 * 224 + 224, K);
	long n = run("first 21 bins of 288x224");
	CHECK(n == 1324512);
	CHECK(strstr(logbuf, "rows    0-1119 right x 0-1124\n") != 0);
	CHECK(strstr(logbuf, "rows 1120-1343 right x 0-287\n") != 0);
	CHECK(strstr(logbuf, "rows 1344-2435 right x none\n") != 0);
	/* block row 17 = rows 1088-1151: 1088-1119 all right, then only x < 288 */
	CHECK(strstr(logbuf, "t:   1088 ####++++++++++++++\n") != 0);
	CHECK(strstr(logbuf, "t:   1344 ..................\n") != 0);

	/* one bin from frame 58, one shifted down by a row, one clear colour, junk */
	fill(0, 0, W, H, K);
	fill(288, 224, 576, 448, K - 1);
	/*
	 * The texel of (x, y - 1) at (x, y). Byte 0 = (y + k) & 0xff cannot tell a one-row shift from the
	 * previous frame: only where y >> 8 changes (row 512 here) is it "other position".
	 */
	for (int y = 448; y < 672; y++)
		for (int x = 0; x < 288; x++) {
			uint8_t *t = buf + (size_t)y * BPR + (size_t)x * 4;
			t[0] = (uint8_t)(y - 1 + K), t[3] = (uint8_t)((y - 1) >> 8);
		}
	memset(buf + (size_t)2000 * BPR, 0x33, 4 * 100);
	memset(buf + (size_t)2001 * BPR, 0xee, 4 * 10);
	n = run("older frame, shifted, clear, junk");
	CHECK(strstr(logbuf, "older frame: pattern of frame 58 (mod 256) x128736") != 0);
	CHECK(strstr(logbuf, "other position: texel of (x+0, y-1) x288") != 0);
	CHECK(strstr(logbuf, "clear colour 100,") != 0);
	CHECK(strstr(logbuf, "other 10;") != 0);
	CHECK(n == (long)W * H - 2 * 64512 - 100 - 10);

	free(buf);
	printf("rt_map_test: %s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
