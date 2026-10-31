/*
 * drgpu: GPU SMMU stage-1 page tables for the DRG Adreno 512 (portable C,
 * no OS headers; used by the shim and by the XNU driver).
 *
 * Format as HMD's io-pgtable-arm.c ARM_64_LPAE_S1 programs it on this phone
 * (gpu-research/TWRP-MEASURED.md §6d): 4 KiB granule, ias 39 (T0SZ 25, walk
 * starts at level 1), oas 36. PTE for IOMMU_READ|IOMMU_WRITE: nG, AP_RW, AF,
 * SH_OS, MAIR index 0 (NC); read-only adds AP_RO. Tables are 4 KiB pages the
 * OS hands out zeroed, with their physical and CPU addresses.
 */
#ifndef DRGPU_PT_H
#define DRGPU_PT_H
#include <stdint.h>

struct drgpu_page {
	uint64_t pa;		/* physical address (what the SMMU walks) */
	uint64_t *va;		/* CPU address of the same 4 KiB, zeroed */
};

struct drgpu_pt {
	struct drgpu_page l1;	/* TTBR0 points here */
	/* OS hooks: a zeroed 4 KiB page, and a barrier that makes CPU writes
	 * to tables visible to the SMMU walker (dsb on the shim's NC memory). */
	int (*alloc_table)(void *os, struct drgpu_page *out);
	uint64_t *(*table_va)(void *os, uint64_t pa);	/* CPU address of a table page */
	void (*publish)(void *os);
	void *os;
};

#define DRGPU_PT_RW	0
#define DRGPU_PT_RO	1

/* map [va, va + len) to [pa, pa + len), 4 KiB aligned; 0 or -1 (no table page, or already mapped) */
int drgpu_pt_map(struct drgpu_pt *pt, uint64_t va, uint64_t pa, uint64_t len, int prot);
/* unmap [va, va + len); returns pages cleared. The caller flushes the SMMU TLB afterwards. */
uint64_t drgpu_pt_unmap(struct drgpu_pt *pt, uint64_t va, uint64_t len);
/* physical address for va, or ~0 if not mapped (the SMMU walk done by the CPU, for checks) */
uint64_t drgpu_pt_lookup(const struct drgpu_pt *pt, uint64_t va);
/* hand every L2/L3 table page back through free_table (the L1 stays the caller's); entries are cleared */
void drgpu_pt_destroy(struct drgpu_pt *pt, void (*free_table)(void *os, uint64_t pa));

#endif
