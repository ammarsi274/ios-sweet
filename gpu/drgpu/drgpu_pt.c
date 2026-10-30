/*
 * drgpu GPU page tables, see drgpu_pt.h. Descriptor bits from HMD's
 * io-pgtable-arm.c: TYPE_TABLE / TYPE_PAGE 3, AF bit 10, SH_OS 2 << 8,
 * AP_RW 1 << 6, AP_RO 3 << 6, nG bit 11, ATTRINDX 0 (NC); output address
 * bits 47:12 (oas 36 fits).
 */
#include "drgpu_pt.h"

#define PTE_VALID_TABLE	3ULL
#define PTE_PAGE	3ULL
#define PTE_AF		(1ULL << 10)
#define PTE_SH_OS	(2ULL << 8)
#define PTE_AP_RW	(1ULL << 6)
#define PTE_AP_RO	(3ULL << 6)
#define PTE_NG		(1ULL << 11)
#define PTE_ADDR	0x0000fffffffff000ULL
#define VA_BITS		39

static unsigned idx(uint64_t va, int level)	/* level 1..3, 4 KiB granule */
{
	return (unsigned)((va >> (12 + 9 * (3 - level))) & 511);
}

/* next-level table for va at level (1 or 2), created if create; NULL if absent/no memory */
static uint64_t *next(struct drgpu_pt *pt, uint64_t *tbl, uint64_t va, int level, int create)
{
	uint64_t *e = &tbl[idx(va, level)];
	if ((*e & 3) == PTE_VALID_TABLE)
		return pt->table_va(pt->os, *e & PTE_ADDR);
	if (*e & 1)
		return 0;	/* a block mapping is in the way */
	if (!create)
		return 0;
	struct drgpu_page p;
	if (pt->alloc_table(pt->os, &p))
		return 0;
	*e = (p.pa & PTE_ADDR) | PTE_VALID_TABLE;
	return p.va;
}

int drgpu_pt_map(struct drgpu_pt *pt, uint64_t va, uint64_t pa, uint64_t len, int prot)
{
	if ((va | pa | len) & 0xfff || va + len > (1ULL << VA_BITS) || va + len < va)
		return -1;
	uint64_t attr = PTE_PAGE | PTE_AF | PTE_SH_OS | PTE_NG | (prot == DRGPU_PT_RO ? PTE_AP_RO : PTE_AP_RW);
	for (uint64_t off = 0; off < len; off += 0x1000) {
		uint64_t v = va + off;
		uint64_t *l2 = next(pt, pt->l1.va, v, 1, 1);
		uint64_t *l3 = l2 ? next(pt, l2, v, 2, 1) : 0;
		if (!l3)
			return -1;
		uint64_t *e = &l3[idx(v, 3)];
		if (*e & 1)
			return -1;
		*e = ((pa + off) & PTE_ADDR) | attr;
	}
	pt->publish(pt->os);
	return 0;
}

uint64_t drgpu_pt_unmap(struct drgpu_pt *pt, uint64_t va, uint64_t len)
{
	uint64_t n = 0;
	for (uint64_t off = 0; off < len; off += 0x1000) {
		uint64_t v = va + off;
		uint64_t *l2 = next(pt, pt->l1.va, v, 1, 0);
		uint64_t *l3 = l2 ? next(pt, l2, v, 2, 0) : 0;
		if (l3 && (l3[idx(v, 3)] & 1)) {
			l3[idx(v, 3)] = 0;
			n++;
		}
	}
	pt->publish(pt->os);
	return n;
}

uint64_t drgpu_pt_lookup(const struct drgpu_pt *pt, uint64_t va)
{
	const uint64_t *t = pt->l1.va;
	for (int level = 1; level <= 3; level++) {
		uint64_t e = t[idx(va, level)];
		if (!(e & 1))
			return ~0ULL;
		if (level < 3 && (e & 3) == 1)	/* block: 1 GiB at L1, 2 MiB at L2 */
			return (e & PTE_ADDR & ~((1ULL << (12 + 9 * (3 - level))) - 1)) |
			       (va & ((1ULL << (12 + 9 * (3 - level))) - 1));
		if (level == 3)
			return (e & PTE_ADDR) | (va & 0xfff);
		t = pt->table_va(pt->os, e & PTE_ADDR);
	}
	return ~0ULL;
}
