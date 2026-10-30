/*
 * drgpu: what the portable GPU core needs from its OS (shim or XNU), and
 * the register accessors every MMIO access goes through.
 *
 * MMIO rule, measured on DRG (gpu-research/TWRP-MEASURED.md §6d): the GPU
 * SMMU's global registers are trapped and emulated by the hypervisor, and an
 * access with base writeback, or a store of the zero register, never
 * completes. So every access is a plain `ldr/str Wt, [Xn]` from a real
 * register (as Linux 4.4's __raw_readl/writel), for every block, always.
 */
#ifndef DRGPU_OS_H
#define DRGPU_OS_H
#include <stdint.h>
#include <stddef.h>

struct drgpu_mem {
	uint64_t pa;	/* physical, contiguous */
	void *va;	/* CPU address, uncached or write-combined */
	uint64_t size;
};

struct drgpu_os {
	void *ctx;
	/* device memory mapping of [pa, pa + len) (page aligned by the caller) */
	uintptr_t (*mmio)(void *ctx, uint64_t pa, uint64_t len);
	/* zeroed, physically contiguous, aligned to `align` (a power of two >= 4 KiB) */
	int (*alloc)(void *ctx, uint64_t size, uint64_t align, struct drgpu_mem *out);
	void (*free)(void *ctx, struct drgpu_mem *m);
	uint64_t *(*ptov)(void *ctx, uint64_t pa);	/* CPU address of memory from alloc */
	void (*log)(void *ctx, const char *msg);	/* one line, no newline needed */
	void (*sleep_us)(void *ctx, uint32_t us);	/* may sleep; the core busy-waits below 50 us itself */
};

static inline uint32_t drgpu_rd(uintptr_t a)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}

static inline void drgpu_wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]\n\tdsb sy" : : "r"(v), "r"(a) : "memory");
}

static inline void drgpu_wr64(uintptr_t a, uint64_t v)
{
	__asm__ volatile("str %x0, [%1]\n\tdsb sy" : : "r"(v), "r"(a) : "memory");
}

static inline uint64_t drgpu_ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint64_t drgpu_freq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline void drgpu_udelay(uint32_t us)
{
	uint64_t end = drgpu_ticks() + drgpu_freq() / 1000000 * us + 1;
	while (drgpu_ticks() < end)
		;
}

/* poll until (rd(a) & mask) == want: microseconds taken, or -1 after timeout_us */
static inline int drgpu_poll(uintptr_t a, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
	uint64_t t0 = drgpu_ticks(), end = t0 + drgpu_freq() / 1000000 * timeout_us;
	while ((drgpu_rd(a) & mask) != want)
		if (drgpu_ticks() > end)
			return -1;
	return (int)((drgpu_ticks() - t0) * 1000000 / drgpu_freq());
}

#endif
