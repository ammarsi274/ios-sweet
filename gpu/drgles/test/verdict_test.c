/*
 * Host test of verdict.c (root r25: the self-test's cached verdict that bench -selftest-boot writes and
 * libdrgeagl reads): file format round trip, malformed files, the key's sensitivity to each input,
 * and the use/rerun decision.
 *   gcc -O1 -Wall -Wextra -I.. -o /tmp/vt verdict_test.c ../verdict.c && /tmp/vt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../verdict.h"
#include "../host.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

void *host_read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc(n > 0 ? (size_t)n : 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return 0;
	}
	fclose(f);
	*size = (size_t)n;
	return b;
}
void host_free(void *p) { free(p); }

static void put(const char *path, const char *text, size_t n)
{
	FILE *f = fopen(path, "wb");
	fwrite(text, 1, n, f);
	fclose(f);
}

int main(void)
{
	char buf[128];
	uint64_t k;
	int m;
	/* format / parse round trip */
	CHECK(drg_verdict_format(buf, sizeof(buf), 0x0123456789abcdefull, 16) > 0);
	CHECK(!strcmp(buf, "drg-gpu-selftest 1\nkey 0123456789abcdef\nverdict mode 16\n"));
	CHECK(drg_verdict_parse(buf, &k, &m) == 0 && k == 0x0123456789abcdefull && m == 16);
	CHECK(drg_verdict_format(buf, sizeof(buf), 42, -1) > 0);
	CHECK(!strcmp(buf, "drg-gpu-selftest 1\nkey 000000000000002a\nverdict none\n"));
	CHECK(drg_verdict_parse(buf, &k, &m) == 0 && k == 42 && m == -1);
	CHECK(drg_verdict_format(buf, 20, 42, 16) == -1);	/* too small */
	/* malformed */
	static const char *const bad[] = {
		"", "drg-gpu-selftest 2\nkey 0123456789abcdef\nverdict mode 16\n",
		"drg-gpu-selftest 1\nkey 0123456789ABCDEF\nverdict mode 16\n",
		"drg-gpu-selftest 1\nkey 0123456789abcde\nverdict mode 16\n",
		"drg-gpu-selftest 1\nkey 0123456789abcdef\nverdict mode 64\n",
		"drg-gpu-selftest 1\nkey 0123456789abcdef\nverdict mode 16",
		"drg-gpu-selftest 1\nkey 0123456789abcdef\nverdict mode 16\nx",
		"drg-gpu-selftest 1\nkey 0123456789abcdef\nverdict mode \n",
		"drg-gpu-selftest 1\nkey 0123456789abcdef\nverdict nonex\n",
		"mode 16\n", 0 };
	for (int i = 0; bad[i]; i++)
		CHECK(drg_verdict_parse(bad[i], &k, &m) == -1);
	CHECK(drg_verdict_parse(0, &k, &m) == -1);
	/* key: each file's bytes, its length, a missing file and the kernel string all change it */
	put("/tmp/vt-a", "bench", 5);
	put("/tmp/vt-b", "library", 7);
	const char *const f2[] = { "/tmp/vt-a", "/tmp/vt-b", 0 };
	int miss = -1;
	uint64_t k0 = drg_verdict_key(f2, "Darwin Kernel 1", &miss);
	CHECK(miss == 0);
	CHECK(drg_verdict_key(f2, "Darwin Kernel 1", &miss) == k0);	/* stable */
	CHECK(drg_verdict_key(f2, "Darwin Kernel 2", &miss) != k0);
	put("/tmp/vt-b", "librarY", 7);
	CHECK(drg_verdict_key(f2, "Darwin Kernel 1", &miss) != k0);
	put("/tmp/vt-a", "benchl", 6);	/* a byte moved from one file to the other */
	put("/tmp/vt-b", "ibrary", 6);
	CHECK(drg_verdict_key(f2, "Darwin Kernel 1", &miss) != k0);
	const char *const f3[] = { "/tmp/vt-a", "/tmp/vt-missing", 0 };
	drg_verdict_key(f3, "", &miss);
	CHECK(miss == 1);
	/* the decision */
	drg_verdict_format(buf, sizeof(buf), k0, 16);
	CHECK(drg_verdict_check(buf, k0, 0, &m) == DRG_VERDICT_USE && m == 16);
	CHECK(drg_verdict_check(buf, k0 ^ 1, 0, &m) == DRG_VERDICT_STALE && m == -1);
	CHECK(drg_verdict_check(buf, k0, 1, &m) == DRG_VERDICT_FAULT && m == -1);
	CHECK(drg_verdict_check(0, k0, 0, &m) == DRG_VERDICT_NONE && m == -1);
	CHECK(drg_verdict_check("junk", k0, 0, &m) == DRG_VERDICT_BAD && m == -1);
	drg_verdict_format(buf, sizeof(buf), k0, -1);
	CHECK(drg_verdict_check(buf, k0, 0, &m) == DRG_VERDICT_USE && m == -1);	/* "none" is kept: no rerun */
	for (int u = DRG_VERDICT_USE; u <= DRG_VERDICT_FAULT; u++)
		CHECK(strcmp(drg_verdict_why((enum drg_verdict_use)u), "?"));
	printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
	return fails != 0;
}
