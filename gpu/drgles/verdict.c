/* The GPU self-test's cached verdict (verdict.h). Portable: host.h's file read, no system headers. */
#include "verdict.h"
#include "host.h"
#include "rt.h"

#define FNV_OFF 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * FNV_PRIME;
	return h;
}

uint64_t drg_verdict_key(const char *const *files, const char *extra, int *missing)
{
	uint64_t h = FNV_OFF;
	int miss = 0;
	for (int i = 0; files && files[i]; i++) {
		size_t n = 0;
		uint8_t *b = host_read_file(files[i], &n);
		if (!b) {
			miss++;
			h = fnv(h, (const uint8_t *)"-", 1);
			continue;
		}
		uint64_t len = n;
		h = fnv(h, (const uint8_t *)&len, sizeof(len));	/* length first: no two lists hash alike by shifting bytes */
		h = fnv(h, b, n);
		host_free(b);
	}
	if (extra)
		h = fnv(h, (const uint8_t *)extra, strlen(extra));
	if (missing)
		*missing = miss;
	return h;
}

#ifdef __APPLE__
int sysctlbyname(const char *, void *, size_t *, void *, size_t);
uint64_t drg_verdict_key_now(int *missing)
{
	static const char *const files[] = { DRG_BENCH_PATH, DRG_EAGL_PATH, 0 };
	char v[512];
	size_t n = sizeof(v) - 1;
	if (sysctlbyname("kern.version", v, &n, 0, 0) || n >= sizeof(v))
		n = 0;
	v[n] = 0;
	return drg_verdict_key(files, v, missing);
}
#endif

int drg_verdict_format(char *buf, size_t n, uint64_t key, int mode)
{
	int r = mode >= 0 ? snprintf(buf, n, "drg-gpu-selftest 1\nkey %016llx\nverdict mode %d\n", (unsigned long long)key, mode)
			  : snprintf(buf, n, "drg-gpu-selftest 1\nkey %016llx\nverdict none\n", (unsigned long long)key);
	return r > 0 && (size_t)r < n ? r : -1;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int drg_verdict_parse(const char *buf, uint64_t *key, int *mode)
{
	static const char h1[] = "drg-gpu-selftest 1\nkey ";
	if (!buf || strncmp(buf, h1, sizeof(h1) - 1))
		return -1;
	const char *p = buf + sizeof(h1) - 1;
	uint64_t k = 0;
	for (int i = 0; i < 16; i++, p++) {
		int v = hexval(*p);
		if (v < 0)
			return -1;
		k = k << 4 | (uint64_t)v;
	}
	if (strncmp(p, "\nverdict ", 9))
		return -1;
	p += 9;
	int m;
	if (!strcmp(p, "none\n")) {
		m = -1;
	} else if (!strncmp(p, "mode ", 5) && p[5] >= '0' && p[5] <= '9') {
		m = 0;
		for (p += 5; *p >= '0' && *p <= '9'; p++) {
			m = m * 10 + (*p - '0');
			if (m > 63)	/* DRG_MODE_MAX */
				return -1;
		}
		if (strcmp(p, "\n"))
			return -1;
	} else {
		return -1;
	}
	*key = k;
	*mode = m;
	return 0;
}

enum drg_verdict_use drg_verdict_check(const char *text, uint64_t key, int fault, int *mode)
{
	uint64_t k = 0;
	int m = -1;
	*mode = -1;
	if (!text)
		return DRG_VERDICT_NONE;
	if (drg_verdict_parse(text, &k, &m))
		return DRG_VERDICT_BAD;
	if (k != key)
		return DRG_VERDICT_STALE;
	if (fault)
		return DRG_VERDICT_FAULT;
	*mode = m;
	return DRG_VERDICT_USE;
}

const char *drg_verdict_why(enum drg_verdict_use u)
{
	switch (u) {
	case DRG_VERDICT_USE: return "cached verdict for this bench, library and kernel";
	case DRG_VERDICT_NONE: return "no cached verdict (first boot of this root image)";
	case DRG_VERDICT_BAD: return "cached verdict malformed";
	case DRG_VERDICT_STALE: return "cached verdict is for another bench, library or kernel";
	case DRG_VERDICT_FAULT: return "a GPU fault was recorded since the verdict";
	}
	return "?";
}
