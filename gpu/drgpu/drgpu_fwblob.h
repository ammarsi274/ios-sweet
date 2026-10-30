/*
 * GPU firmware handed from the shim to XNU (drg/GPU-IOS-DESIGN.md): Nokia's
 * files (vendor_b /vendor/firmware) never go into git or the CI kernel. The
 * shim's loader copies them below topOfKernelData (memory XNU keeps as its
 * own) and passes the blob's physical address as boot-arg drgpufw=<pa>.
 * Layout: this header, then each file at its offset; fnv is FNV-1a 64 over
 * every byte after the header (total bytes), checked by the kernel.
 */
#ifndef DRGPU_FWBLOB_H
#define DRGPU_FWBLOB_H
#include <stdint.h>

#define DRGPU_FWBLOB_MAGIC	0x3157465550475244ULL	/* "DRGPUFW1" */
#define DRGPU_FWBLOB_PM4	1	/* a530_pm4.fw */
#define DRGPU_FWBLOB_PFP	2	/* a530_pfp.fw */
#define DRGPU_FWBLOB_ZAP_MDT	3	/* a512_zap.mdt */
#define DRGPU_FWBLOB_ZAP_B02	4	/* a512_zap.b02 */

struct drgpu_fwblob {
	uint64_t magic;
	uint32_t version;	/* 1 */
	uint32_t count;		/* entries used */
	uint64_t total;		/* bytes after this header */
	uint64_t fnv;
	struct {
		uint32_t type, off, len, pad;	/* off from the start of the blob */
	} e[4];
};

static inline uint64_t drgpu_fnv(const volatile uint8_t *p, uint64_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (uint64_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

/*
 * Check a blob (magic, version, bounds, FNV) and point fw at its files.
 * 0, or a negative step number that failed. Shared by the kernel and the
 * shim's test, so both run the same check.
 */
#ifdef DRGPU_H
static inline int drgpu_fwblob_parse(const uint8_t *blob, uint64_t maxlen, struct drgpu_fw *fw)
{
	const struct drgpu_fwblob *b = (const struct drgpu_fwblob *)blob;
	if (maxlen < sizeof(*b) || b->magic != DRGPU_FWBLOB_MAGIC || b->version != 1 || b->count != 4)
		return -1;
	if (b->total > maxlen - sizeof(*b))
		return -2;
	if (drgpu_fnv(blob + sizeof(*b), b->total) != b->fnv)
		return -3;
	const uint8_t *p[5] = { 0 };
	uint32_t n[5] = { 0 };
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t t = b->e[i].type;
		if (t < 1 || t > 4 || b->e[i].off < sizeof(*b) ||
		    (uint64_t)b->e[i].off + b->e[i].len > sizeof(*b) + b->total || p[t])
			return -4;
		p[t] = blob + b->e[i].off;
		n[t] = b->e[i].len;
	}
	fw->pm4 = p[DRGPU_FWBLOB_PM4];
	fw->pm4_len = n[DRGPU_FWBLOB_PM4];
	fw->pfp = p[DRGPU_FWBLOB_PFP];
	fw->pfp_len = n[DRGPU_FWBLOB_PFP];
	fw->zap_mdt = p[DRGPU_FWBLOB_ZAP_MDT];
	fw->zap_mdt_len = n[DRGPU_FWBLOB_ZAP_MDT];
	fw->zap_b02 = p[DRGPU_FWBLOB_ZAP_B02];
	fw->zap_b02_len = n[DRGPU_FWBLOB_ZAP_B02];
	return 0;
}
#endif

#endif
