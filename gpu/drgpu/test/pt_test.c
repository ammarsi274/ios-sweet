/*
 * Host unit test of drgpu_pt.c ("physical" = CPU memory). Build and run:
 *   clang -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I.. pt_test.c ../drgpu_pt.c -o pt_test && ./pt_test
 * GPU addresses are the ones Nokia's driver used in capture cap1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drgpu_pt.h"

static int tables;

static int alloc(void *os, struct drgpu_page *p)
{
	void *m = aligned_alloc(4096, 4096);
	if (!m)
		return -1;
	memset(m, 0, 4096);
	p->va = m;
	p->pa = (uint64_t)(uintptr_t)m;
	tables++;
	return 0;
}

static uint64_t *tva(void *os, uint64_t pa) { return (uint64_t *)(uintptr_t)pa; }
static void pub(void *os) {}

int main(void)
{
	struct drgpu_pt pt = { .alloc_table = alloc, .table_va = tva, .publish = pub };
	alloc(0, &pt.l1);
	int fail = 0;
	fail |= drgpu_pt_map(&pt, 0x7fffbb000ULL, 0x90000000ULL, 0x5000, DRGPU_PT_RW) != 0;
	fail |= drgpu_pt_map(&pt, 0x7ffff0000ULL, 0x91000000ULL, 0x4000, DRGPU_PT_RW) != 0;
	fail |= drgpu_pt_lookup(&pt, 0x7fffbb123ULL) != 0x90000123ULL;
	fail |= drgpu_pt_lookup(&pt, 0x7fffbf010ULL) != 0x90004010ULL;
	fail |= drgpu_pt_lookup(&pt, 0x7ffff3ffcULL) != 0x91003ffcULL;
	fail |= drgpu_pt_lookup(&pt, 0x7fffc0000ULL) != ~0ULL;
	fail |= drgpu_pt_map(&pt, 0x7fffbb000ULL, 0x92000000ULL, 0x1000, DRGPU_PT_RW) == 0;	/* already mapped */
	fail |= drgpu_pt_unmap(&pt, 0x7fffbb000ULL, 0x5000) != 5;
	fail |= drgpu_pt_lookup(&pt, 0x7fffbb123ULL) != ~0ULL;
	fail |= drgpu_pt_map(&pt, 0x7fffbb000ULL, 0x92000000ULL, 0x1000, DRGPU_PT_RW) != 0;
	fail |= drgpu_pt_lookup(&pt, 0x7fffbb004ULL) != 0x92000004ULL;
	fail |= drgpu_pt_map(&pt, 0x8000000000ULL, 0x93000000ULL, 0x1000, DRGPU_PT_RW) == 0;	/* beyond 39 bits */
	/* a page PTE: nG AF SH_OS AP_RW page = pa | 0xe43 (io-pgtable-arm.c) */
	uint64_t *l2 = tva(0, pt.l1.va[0x1f] & 0xfffffffff000ULL);
	uint64_t *l3 = tva(0, l2[(0x7ffff0000ULL >> 21) & 511] & 0xfffffffff000ULL);
	uint64_t pte = l3[(0x7ffff0000ULL >> 12) & 511];
	printf("pte 0x7ffff0000 = %#llx (want 0x91000e43), tables %d\n", (unsigned long long)pte, tables);
	fail |= pte != 0x91000e43ULL;
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail;
}
