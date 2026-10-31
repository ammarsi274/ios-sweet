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

#ifdef KERNEL
/*
 * XNU: the kernel's clock (mach_absolute_time at its ADT timebase), as the
 * rest of DrgGPU uses. Boot #35 with the raw counter registers: a 20 ms
 * wait for gcc_bimc_gfx gave up within 13 us (CBCR 0x80000000 before,
 * 0x1 = running right after), so every core timeout was ~0 under XNU.
 */
uint64_t drgpu_os_ticks(void);
uint64_t drgpu_os_freq(void);

static inline uint64_t drgpu_ticks(void)
{
	return drgpu_os_ticks();
}

static inline uint64_t drgpu_freq(void)
{
	return drgpu_os_freq();
}
#else
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
#endif

static inline void drgpu_udelay(uint32_t us)
{
	uint64_t end = drgpu_ticks() + drgpu_freq() / 1000000 * us + 1;
	while (drgpu_ticks() < end)
		;
}

/*
 * poll until (rd(a) & mask) == want: microseconds taken, or -1 after timeout_us.
 * A success is never negative (the old (int) cast of a large count could be).
 */
static inline int drgpu_poll(uintptr_t a, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
	uint64_t f = drgpu_freq(), t0 = drgpu_ticks(), end = t0 + f / 1000000 * timeout_us;
	while ((drgpu_rd(a) & mask) != want)
		if (drgpu_ticks() > end)
			return -1;
	uint64_t us = f >= 1000000 ? (drgpu_ticks() - t0) / (f / 1000000) : 0;
	return us > 0x7fffffff ? 0x7fffffff : (int)us;
}

#endif
