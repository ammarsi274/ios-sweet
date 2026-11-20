/*
 * Read-only SMEM (see drg_smem.h). From HMD's DRG kernel:
 *   device tree qcom,smem@86000000: smem 0x86000000 + 0x200000,
 *     smem_targ_info_reg 0x1fd4000 (8 bytes: u64 PA of struct
 *     smem_targ_info_type {u32 identifier "SIII" 0x49494953, u32 size,
 *     u64 phys_base_addr, u32 max_items}); msm_smem_probe uses it when it
 *     is valid, else the DT region.
 *   msm_smem.c: partition TOC in the top 4 KiB of the region ("$TOC"
 *     0x434f5424; header 32 bytes, entries 48: offset, size, flags, u16
 *     host0, u16 host1, size_cacheline, ...); partition header "$PRT"
 *     0x54525024 {id, u16 host0, u16 host1, size, offset_free_uncached,
 *     offset_free_cached, 3 reserved} = 32 bytes; uncached allocations
 *     upwards from +32: {u16 canary 0xa5a5, u16 type, u32 size, u16
 *     padding_data, u16 padding_hdr, u32 reserved} then padding_hdr, item;
 *     cached allocations downwards from the end (header aligned to
 *     size_cacheline, item below it). Partition for remote host h: the TOC
 *     entry with hosts {0 (apps), h}; SMEM_ANY_HOST_FLAG: the common
 *     partition (hosts 0xfffe/0xfffe) if there is one, else the legacy TOC
 *     (struct smem_shared: 4 proc_comm (64 bytes), version[32] (128), heap
 *     info (16), heap_toc[] of {allocated, offset, size, reserved = aux base
 *     | 2 bits} at +208).
 * Only partitions shared with apps and the common one are read (the region
 * is MPU-protected: other partitions may fault). The first read of the
 * target-info register, the region and the TOC runs on a spare core.
 */
#include "drg_smem.h"
#include "drg_gpu.h"

#define SMEM_DT_BASE		0x86000000UL
#define SMEM_DT_SIZE		0x00200000UL
#define TARG_INFO_REG		0x01fd4000UL
#define TOC_ID			0x434f5424u
#define PRT_ID			0x54525024u
#define CANARY			0xa5a5u
#define SMP2P_MAGIC		0x504d5324u

static drg_log_fn logp;
static uintptr_t base;
static uint32_t size, max_items = 512;
static int ready;

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint16_t rd16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }

static int spare_rd(uintptr_t a, uint32_t *v)
{
	uint64_t esr = 0;
	int rc = -2;
	for (int c = 5; c <= 7 && (rc == -2 || rc == -4 || rc == -1); c++)
		rc = drg_probe_read(c, a, v, &esr, 100000);
	if (rc)
		logp("smem probe FAIL addr/rc", (uint32_t)a, (uint32_t)rc, -1);
	return rc;
}

int drg_smem_init(drg_log_fn log, const char *tag)
{
	(void)tag;
	logp = log;
	ready = 0;
	base = SMEM_DT_BASE;
	size = SMEM_DT_SIZE;
	uint32_t lo = 0, hi = 0, v = 0;
	if (!spare_rd(TARG_INFO_REG, &lo) && !spare_rd(TARG_INFO_REG + 4, &hi) && !hi && lo &&
	    !spare_rd(lo, &v)) {
		uint32_t id = v, sz = rd32(lo + 4), pb = rd32(lo + 8), pbh = rd32(lo + 12), mi = rd32(lo + 16);
		logp("smem targ info pa/id", lo, id, id == 0x49494953u ? 0 : -2);
		if (id == 0x49494953u && !pbh && pb && sz >= 0x10000 && sz <= 0x1000000) {
			base = pb, size = sz;
			if (mi && mi <= 4096)
				max_items = mi;
		}
	}
	logp("smem base/size", (uint32_t)base, size, 0);
	if (spare_rd(base, &v) || spare_rd(base + size - 4096, &v))
		return -1;
	ready = 1;
	uintptr_t toc = base + size - 4096;
	uint32_t n = rd32(toc + 8);
	logp("smem toc id/entries", rd32(toc), n, rd32(toc) == TOC_ID ? 0 : -2);
	if (rd32(toc) == TOC_ID)
		for (uint32_t i = 0; i < n && i < 32; i++) {
			uintptr_t e = toc + 32 + 48 * i;
			logp("smem part hosts/off", (uint32_t)rd16(e + 12) << 16 | rd16(e + 14), rd32(e), 0);
		}
	logp("smem max items/ver", max_items, rd32(base + 64 + 4 * 8), 0);	/* version[8]: SMEM_VERSION_INFO slot (log only) */
	return 0;
}

/* partition header for {apps, host} (host 0xfffe: the common one), or 0 */
static uintptr_t partition(uint32_t host, uint32_t *psize, uint32_t *cl)
{
	uintptr_t toc = base + size - 4096;
	if (rd32(toc) != TOC_ID)
		return 0;
	uint32_t n = rd32(toc + 8);
	for (uint32_t i = 0; i < n && i < 32; i++) {
		uintptr_t e = toc + 32 + 48 * i;
		uint32_t off = rd32(e), sz = rd32(e + 4), h0 = rd16(e + 12), h1 = rd16(e + 14), c = rd32(e + 16);
		int match = host == 0xfffe ? (h0 == 0xfffe && h1 == 0xfffe) : ((h0 == 0 && h1 == host) || (h1 == 0 && h0 == host));
		if (!match || !off || !sz || !c || off >= size || sz > size - off)
			continue;
		uintptr_t hdr = base + off;
		if (rd32(hdr) != PRT_ID || rd32(hdr + 8) != sz)
			return 0;
		*psize = sz;
		*cl = c;
		return hdr;
	}
	return 0;
}

static uintptr_t in_partition(uintptr_t hdr, uint32_t psz, uint32_t cl, uint32_t id, uint32_t *isz)
{
	uint32_t fu = rd32(hdr + 12), fc = rd32(hdr + 16);
	if (fu > psz || fc > psz)
		return 0;
	/* uncached, upwards */
	for (uintptr_t a = hdr + 32, guard = 0; a + 16 <= hdr + fu && guard < 4096; guard++) {
		uint32_t isize = rd32(a + 4), pd = rd16(a + 8), ph = rd16(a + 10);
		if (rd16(a) != CANARY || isize > psz || ph > psz || pd > isize)
			return 0;
		if (rd16(a + 2) == id) {
			*isz = isize - pd;
			return a + 16 + ph;
		}
		a += 16 + ph + isize;
	}
	/* cached, downwards */
	uint32_t ah = (16 + cl - 1) / cl * cl;
	for (uintptr_t a = hdr + psz - ah, guard = 0; a > hdr + fc && guard < 4096; guard++) {
		uint32_t isize = rd32(a + 4), pd = rd16(a + 8);
		if (rd16(a) != CANARY || isize > psz || pd > isize)
			return 0;
		if (rd16(a + 2) == id) {
			*isz = isize - pd;
			return a - isize;
		}
		a -= isize + ah;
	}
	return 0;
}

uintptr_t drg_smem_item(uint32_t id, uint32_t host, uint32_t *isz)
{
	*isz = 0;
	if (!ready || id >= max_items)
		return 0;
	uint32_t psz = 0, cl = 0;
	uintptr_t hdr = partition(host == SMEM_HOST_ANY ? 0xfffe : host, &psz, &cl);
	if (hdr)
		return in_partition(hdr, psz, cl, id, isz);
	if (host != SMEM_HOST_ANY)
		return 0;
	uintptr_t t = base + 208 + 16 * id;	/* legacy heap_toc[id] */
	if (!rd32(t))
		return 0;
	uint32_t off = rd32(t + 4), sz = rd32(t + 8), aux = rd32(t + 12) & ~3u;
	if (aux && aux != base)	/* another SMEM area: not read */
		return 0;
	if (off >= size || sz > size - off)
		return 0;
	*isz = sz;
	return base + off;
}

static uint32_t pack(uintptr_t p, uint32_t n)	/* up to 4 bytes, first byte in the top: hex reads in order */
{
	uint32_t w = 0;
	for (uint32_t k = 0; k < 4; k++)
		w = w << 8 | (k < n ? rd8(p + k) : 0);
	return w;
}

uint32_t drg_smp2p_entry(uint32_t id, uint32_t host, const char *name, int *found, drg_log_fn log, const char *tag)
{
	(void)tag;
	uint32_t sz = 0;
	uintptr_t p = drg_smem_item(id, host, &sz);
	*found = 0;
	if (!p || sz < 20 || rd32(p) != SMP2P_MAGIC) {
		if (log)
			log("smp2p item/size", id, sz, p ? -2 : -1);
		return 0;
	}
	uint32_t tot = rd32(p + 12) & 0xffff, valid = rd32(p + 12) >> 16, val = 0;
	if (log) {
		log("smp2p ver/rem-loc", rd32(p + 4), rd32(p + 8), 0);
		log("smp2p valid-total/flg", rd32(p + 12), rd32(p + 16), 0);
	}
	for (uint32_t i = 0; i < valid && i < tot && 20 + 20 * (i + 1) <= sz; i++) {
		uintptr_t e = p + 20 + 20 * i;
		int k = 0;
		while (k < 16 && name[k] && rd8(e + k) == (uint8_t)name[k])
			k++;
		if (k < 16 && !name[k] && !rd8(e + k)) {
			*found = 1;
			val = rd32(e + 16);
		}
		if (log)
			log(" smp2p entry/value", pack(e, 4), rd32(e + 16), 0);	/* first 4 name chars */
	}
	return val;
}

uint32_t drg_smem_string(uint32_t id, uint32_t host, drg_log_fn log, const char *label)
{
	uint32_t sz = 0;
	uintptr_t p = drg_smem_item(id, host, &sz);
	if (!p) {
		if (log)
			log(label, id, 0xffffffffu, -1);
		return 0;
	}
	uint32_t first = sz ? rd8(p) : 0;
	if (log) {
		uint32_t n = 0;
		while (n < sz && n < 128 && rd8(p + n))	/* MAX_SSR_REASON_LEN 130 (pil-q6v5-mss.c) */
			n++;
		log(label, id, sz << 16 | n, 0);
		for (uint32_t i = 0; i < n; i += 8)
			log(" sfr", pack(p + i, n - i), i + 4 < n ? pack(p + i + 4, n - i - 4) : 0, 0);
	}
	return first;
}
