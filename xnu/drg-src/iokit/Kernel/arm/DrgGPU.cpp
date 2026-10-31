/*
 * DRG (Nokia 6.1 Plus) GPU: the Adreno 512 for iOS userland.
 *
 * A thin XNU wrapper around the portable drgpu core (bsd/dev/drgpu, the
 * source the shim proved on this phone: drg/GPU-IOS-DESIGN.md I2, I3).
 *
 *   firmware   Nokia's files, handed over by the shim below topOfKernelData
 *              (boot-arg drgpufw=<pa>, gpu/drgpu/drgpu_fwblob.h), checked by
 *              FNV-1a before use. Never in the kernel image.
 *   memory     the core's rings, globals and page tables come from
 *              write-combined, physically contiguous pools (the GPU walks and
 *              reads them non-cacheable). Process objects are wired pages
 *              mapped into the task and, at the same address, into the task's
 *              own GPU address space (KGSL USE_CPU_MAP; Nokia's driver needs
 *              GPU address == CPU address).
 *   interrupt  kgsl-3d0 "interrupts = <0 0x12c 0>": SPI 300 = INTID 332,
 *              level, routed to the boot CPU; sleh_irq hands it to
 *              drg_gpu_irq on whichever CPU takes it. CACHE_FLUSH_TS wakes
 *              waiters; CP_HW_ERROR and MISC_HANG_DETECT start a recovery.
 *   power      GX collapse after Nokia's qcom,idle-timeout (80 ms) without
 *              work, resume on the next submission (~9 ms measured, no zap
 *              reload). SMMU registers are only touched while powered.
 *   clocks     levels 430/370/266/160 MHz (gpu-research §6g), starting at
 *              Linux's default 266; every 50 ms the busy share of the last
 *              window moves one level up (> 75 %) or down (< 30 %).
 *
 * DrgGPUUserClient offers Nokia's KGSL calls (gpu/drgpu/drgpu_uc.h).
 * Boot-args: drggpu=0 (off), drggpu_level=0..3 (fixed level, no DCVS),
 * drggpu_idle=<ms> (0: never collapse).
 */
#include <pexpert/arm64/board_config.h>

#if defined(DRG)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOInterrupts.h>
#include <libkern/c++/OSNumber.h>
#include <machine/machine_routines.h>
#include <machine/atomic.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#include "DrgSurface.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);   /* as drg_ts.c, drg_emmc.c */
#include "../../../bsd/dev/drgpu/drgpu.h"
#include "../../../bsd/dev/drgpu/drgpu_fwblob.h"
#include "../../../bsd/dev/drgpu/drgpu_uc.h"
}

#define DRG_GPU_INTID           332u            /* SPI 300 */
#define DRG_GPU_GICD            0x17a00000ull   /* qcom,msm-qgic2 / GICv3 distributor */
#define DRG_GPU_GICD_SIZE       0x10000ull
#define DRG_GPU_FW_MAX          (4ull << 20)    /* the blob is ~48 KiB */
#define DRG_GPU_POOL_CHUNK      (2u << 20)
#define DRG_GPU_POOL_CHUNKS     8
#define DRG_GPU_POOL_PAGES      (DRG_GPU_POOL_CHUNK / 0x1000)
#define DRG_GPU_BIG_ALLOCS      4               /* zap image region and meta */
#define DRG_GPU_PA_MASK         0x0000000fffffffffull   /* SMMU oas 36 */
#define DRG_GPU_KCTX            (DRGPU_MS_RB - 1)       /* the kernel's own context */
#define DRG_GPU_DEFAULT_LEVEL   2               /* 266 MHz, Linux default_pwrlevel */
#define DRG_GPU_IDLE_MS         80              /* qcom,idle-timeout */
#define DRG_GPU_DCVS_MS         50
#define DRG_GPU_OBJ_MAX         (512ull << 20)
#define DRG_GPU_CLIENT_MAX      (1024ull << 20)
#define DRG_GPU_MAX_OBJS        65536u
#define DRG_GPU_VA_MIN          0x100000000ull  /* above the kernel's global region */
#define DRG_GPU_VA_END          (1ull << 39)    /* TCR T0SZ 25 */
#define RBBM_INT_0_MASK         0x38            /* a5xx_reg.h */
#define CP_HW_FAULT             0xb1a
#define CP_INTERRUPT_STATUS     0xb18
#define CP_PROTECT_STATUS       0xb1c
#define RBBM_STATUS             0x4f5
#define CP_RB_RPTR              0x806
#define INT_CP_HW_ERROR         (1u << 9)       /* A5XX_INT_CP_HW_ERROR */
#define INT_CACHE_FLUSH_TS      (1u << 20)
#define INT_MISC_HANG_DETECT    (1u << 23)

/* GICv3 distributor (non-secure view) */
#define GICD_CTLR               0x0000
#define GICD_IGROUPR            0x0080
#define GICD_ISENABLER          0x0100
#define GICD_ICENABLER          0x0180
#define GICD_IPRIORITYR         0x0400
#define GICD_ICFGR              0x0c00
#define GICD_IROUTER            0x6000
#define GICD_CTLR_RWP           (1u << 31)

class DrgGPUUserClient;

struct drg_gpu_chunk {
	IOBufferMemoryDescriptor *md;           /* owns the pages */
	IOMemoryMap              *map;          /* write-combined view the core uses */
	uint64_t                  pa;
	uint8_t                  *va;
	uint32_t                  used[DRG_GPU_POOL_PAGES / 32];
};

struct drg_gpu_big {
	IOBufferMemoryDescriptor *md;
	IOMemoryMap              *map;          /* NULL once the memory belongs to TZ */
	uint64_t                  pa, size;
	uint8_t                  *va;
};

struct drg_gpu_ctx {
	DrgGPUUserClient *owner;        /* NULL: free */
	uint32_t          flags;
	uint32_t          ts;           /* last timestamp submitted */
	uint32_t          gen;          /* kernel-generated timestamps */
	bool              submitted;
	bool              faulted;      /* its work was dropped by a recovery */
};

class DrgGPU : public IOService {
	OSDeclareDefaultStructors(DrgGPU);
	friend class DrgGPUUserClient;

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	void interrupt(void);

private:
	struct drgpu         fG;
	IOLock              *fLock;
	IOMemoryMap         *fFwMap;
	uintptr_t            fGicd;
	struct drg_gpu_chunk fPool[DRG_GPU_POOL_CHUNKS];
	struct drg_gpu_big   fBig[DRG_GPU_BIG_ALLOCS];
	struct drg_gpu_ctx   fCtx[DRGPU_MS_RB];
	int                  fLevel;
	bool                 fDcvs;
	uint32_t             fIdleMs;
	thread_call_t        fIdleCall, fDcvsCall, fRecoverCall;
	uint64_t             fLastSubmitAbs;
	uint64_t             fDcvsLastAbs;
	uint64_t             fWiredBytes;

	/* shared with the interrupt handler (os_atomic) */
	uint32_t             fIrqLive;          /* the GPU is powered and may be read */
	uint32_t             fInIrq;
	uint32_t             fIrqEvent;         /* wait channel */
	uint32_t             fIrqCount;
	uint32_t             fFaultStatus;
	uint32_t             fLastCtx, fLastTs; /* last submission (the ring runs in order) */
	uint64_t             fBusySince;        /* 0: idle */
	uint64_t             fBusyAccum;

public:
	/* drgpu_os hooks */
	uintptr_t osMmio(uint64_t pa, uint64_t len);
	int osAlloc(uint64_t size, uint64_t align, struct drgpu_mem *out);
	void osFree(struct drgpu_mem *m);
	uint64_t *osPtov(uint64_t pa);

private:
	bool loadFirmware(struct drgpu_fw *fw);
	bool bringUp(void);
	bool selfTest(void);
	void gicSetup(void);
	void gicEnable(bool on);
	void irqLive(bool on);
	bool lastRetired(void);
	IOReturn ensurePowered(void);
	void collapse(void);
	void setLevel(int level);
	void dumpFault(uint32_t status);
	uint32_t regRead(uint32_t reg);

	static void idleTimer(thread_call_param_t p0, thread_call_param_t p1);
	static void dcvsTimer(thread_call_param_t p0, thread_call_param_t p1);
	static void recover(thread_call_param_t p0, thread_call_param_t p1);

	/* used by the user client, lock held unless noted */
	int ctxCreate(DrgGPUUserClient *owner, uint32_t flags);
	void ctxRelease(uint32_t id);
	bool ctxRetired(uint32_t id, uint32_t ts);              /* no lock */
	IOReturn waitTs(uint32_t id, uint32_t ts, uint32_t ms); /* no lock */
	IOReturn submit(DrgGPUUserClient *c, uint32_t ctx, uint32_t flags, uint32_t *ts,
	    const struct drgpu_uc_ib *ibs, unsigned n);
};

OSDefineMetaClassAndStructors(DrgGPU, IOService);

struct drg_gpu_obj {
	IOMemoryDescriptor *md;         /* owns the pages; NULL: free slot */
	IOMemoryDescriptor *phys;       /* the same pages as physical ranges */
	IOMemoryMap        *map;        /* of phys in the task, write-combined; its address is the GPU address */
	uint64_t            gpuaddr, size;
	uint32_t            pendCtx, pendTs;
	bool                pending;    /* freed on (pendCtx, pendTs) */
	bool                surface;    /* an imported IOSurface (not counted, prepared here) */
	bool                unmapped;   /* off the GPU, waiting for the TLB flush */
};

class DrgGPUUserClient : public IOUserClient {
	OSDeclareDefaultStructors(DrgGPUUserClient);
	friend class DrgGPU;

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

private:
	DrgGPU             *fGPU;
	task_t              fTask;
	struct drgpu_space  fSpace;
	bool                fSpaceLive;
	bool                fClosed;
	struct drg_gpu_obj *fObjs;
	uint32_t            fObjCap;
	uint32_t            fPending;
	uint64_t            fBytes;
	IOMemoryMap        *fMemstore;
	uint32_t            fSyncSrc;

	IOReturn kgslProperty(uint32_t type, IOExternalMethodArguments *args);
	IOReturn objInsert(IOMemoryDescriptor *md, uint64_t size, bool ro, bool surface, uint64_t *out);
	IOReturn objAlloc(uint64_t size, uint64_t flags, uint64_t *out);
	IOReturn surfaceMap(uint32_t surfaceID, uint64_t *out);
	IOReturn objFree(uint32_t id, bool onEvent, uint32_t ctx, uint32_t ts);
	struct drg_gpu_obj *obj(uint64_t id);
	void retire(bool all);
	void unmapObj(struct drg_gpu_obj *o);
	void releaseUnmapped(void);
	IOReturn ctxDestroy(uint32_t id);
	IOReturn waitCtx(uint32_t id, uint32_t ts, uint32_t ms);
	void teardown(void);
};

OSDefineMetaClassAndStructors(DrgGPUUserClient, IOUserClient);

static DrgGPU *gDrgGPU;

static uint64_t
drg_gpu_deadline_ms(uint32_t ms)
{
	uint64_t d;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &d);
	return d;
}

/*
 * Memory the GPU reads and writes uncached (it does not snoop the CPU's
 * caches), the way Linux's dma_alloc does it on arm64 for such a device:
 * ordinary pages, zeroed and cleaned out of the caches once, then used only
 * through a write-combined mapping of their physical pages. The cacheable
 * alias the pages already have is never used for them again. Mappings of a
 * physical descriptor take the cache mode per mapping (as DrgSurface maps
 * the panel), so the buffer's own memory object is never changed.
 */
static IOMemoryDescriptor *
drg_gpu_phys_view(IOMemoryDescriptor *md, uint64_t size)
{
	uint32_t n = 0;
	for (uint64_t off = 0, last = ~0ull; off < size;) {
		IOByteCount seg = 0;
		addr64_t pa = md->getPhysicalSegment(off, &seg, kIOMemoryMapperNone);
		if (pa == 0 || seg == 0) {
			return NULL;
		}
		if (seg > size - off) {
			seg = size - off;
		}
		if (pa != last) {
			n++;
		}
		last = pa + seg;
		off += seg;
	}
	IOAddressRange *r = IONew(IOAddressRange, n);
	if (r == NULL) {
		return NULL;
	}
	uint32_t i = 0;
	for (uint64_t off = 0; off < size;) {
		IOByteCount seg = 0;
		addr64_t pa = md->getPhysicalSegment(off, &seg, kIOMemoryMapperNone);
		if (seg > size - off) {
			seg = size - off;
		}
		if (i != 0 && r[i - 1].address + r[i - 1].length == pa) {
			r[i - 1].length += seg;
		} else {
			r[i].address = pa;
			r[i].length = seg;
			i++;
		}
		off += seg;
	}
	IOMemoryDescriptor *view = IOMemoryDescriptor::withAddressRanges(r, i,
	    kIODirectionInOut | kIOMemoryTypePhysical64, TASK_NULL);
	IODelete(r, IOAddressRange, n);
	return view;
}

/* Contiguous, zeroed, cleaned; *va is a write-combined kernel mapping. */
static bool
drg_gpu_contig(uint64_t size, uint64_t align, IOBufferMemoryDescriptor **mdp, IOMemoryMap **mapp,
    uint64_t *pa, uint8_t **va)
{
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, DRG_GPU_PA_MASK & ~(align - 1));
	if (md == NULL) {
		return false;
	}
	bzero(md->getBytesNoCopy(), size);
	md->performOperation(kIOMemoryIncoherentIOFlush, 0, size);
	*pa = md->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	IOMemoryDescriptor *phys = IOMemoryDescriptor::withPhysicalAddress(*pa, size, kIODirectionInOut);
	IOMemoryMap *map = phys != NULL ? phys->map(kIOMapWriteCombineCache) : NULL;
	OSSafeReleaseNULL(phys);
	if (map == NULL) {
		md->release();
		return false;
	}
	*mdp = md;
	*mapp = map;
	*va = (uint8_t *)(uintptr_t)map->getVirtualAddress();
	return true;
}

/* ---- OS hooks for the core (all called with fLock held, or during start) ---- */

static uintptr_t
drg_gpu_os_mmio(void *ctx, uint64_t pa, uint64_t len)
{
	return static_cast<DrgGPU *>(ctx)->osMmio(pa, len);
}

static int
drg_gpu_os_alloc(void *ctx, uint64_t size, uint64_t align, struct drgpu_mem *out)
{
	return static_cast<DrgGPU *>(ctx)->osAlloc(size, align, out);
}

static void
drg_gpu_os_free(void *ctx, struct drgpu_mem *m)
{
	static_cast<DrgGPU *>(ctx)->osFree(m);
}

static uint64_t *
drg_gpu_os_ptov(void *ctx, uint64_t pa)
{
	return static_cast<DrgGPU *>(ctx)->osPtov(pa);
}

static void
drg_gpu_os_log(void *ctx __unused, const char *msg)
{
	IOLog("DRG GPU: %s\n", msg);
}

static void
drg_gpu_os_sleep(void *ctx __unused, uint32_t us)
{
	if (us >= 1000) {
		IOSleep((us + 999) / 1000);
	} else {
		IODelay(us);
	}
}

/* Device memory (nGnRE), mapped the way the other DRG drivers map theirs; never unmapped. */
uintptr_t
DrgGPU::osMmio(uint64_t pa, uint64_t len)
{
	return (uintptr_t)ml_io_map((vm_offset_t)pa, (vm_size_t)len);
}

/*
 * Zeroed, physically contiguous, written and read write-combined
 * (drg_gpu_contig). Up to 16 KiB alignment from the pools (4 KiB pages: a page
 * table costs 4 KiB, not a 16 KiB CPU page); larger alignments (the zap
 * region, 1 MiB) get their own buffer.
 */
int
DrgGPU::osAlloc(uint64_t size, uint64_t align, struct drgpu_mem *out)
{
	if (size == 0 || (align & (align - 1)) != 0) {
		return -1;
	}
	size = (size + 0xfff) & ~0xfffull;
	if (align <= 0x4000 && size <= DRG_GPU_POOL_CHUNK) {
		uint32_t n = (uint32_t)(size >> 12), step = align > 0x1000 ? (uint32_t)(align >> 12) : 1;
		for (unsigned c = 0; c < DRG_GPU_POOL_CHUNKS; c++) {
			struct drg_gpu_chunk *k = &fPool[c];
			if (k->md == NULL) {
				if (!drg_gpu_contig(DRG_GPU_POOL_CHUNK, DRG_GPU_POOL_CHUNK, &k->md, &k->map, &k->pa, &k->va)) {
					return -1;
				}
				bzero(k->used, sizeof(k->used));
			}
			for (uint32_t first = 0; first + n <= DRG_GPU_POOL_PAGES; first += step) {
				uint32_t i = 0;
				while (i < n && !(k->used[(first + i) / 32] & (1u << ((first + i) % 32)))) {
					i++;
				}
				if (i < n) {
					continue;
				}
				for (i = 0; i < n; i++) {
					k->used[(first + i) / 32] |= 1u << ((first + i) % 32);
				}
				out->pa = k->pa + ((uint64_t)first << 12);
				out->va = k->va + ((uint64_t)first << 12);
				out->size = size;
				bzero(out->va, size);
				return 0;
			}
		}
		return -1;
	}
	for (unsigned b = 0; b < DRG_GPU_BIG_ALLOCS; b++) {
		struct drg_gpu_big *g = &fBig[b];
		if (g->md != NULL) {
			continue;
		}
		if (!drg_gpu_contig(size, align, &g->md, &g->map, &g->pa, &g->va)) {
			return -1;
		}
		g->size = size;
		out->pa = g->pa;
		out->va = g->va;
		out->size = size;
		return 0;
	}
	return -1;
}

void
DrgGPU::osFree(struct drgpu_mem *m)
{
	for (unsigned c = 0; c < DRG_GPU_POOL_CHUNKS; c++) {
		struct drg_gpu_chunk *k = &fPool[c];
		if (k->md != NULL && m->pa >= k->pa && m->pa < k->pa + DRG_GPU_POOL_CHUNK) {
			uint32_t first = (uint32_t)((m->pa - k->pa) >> 12), n = (uint32_t)((m->size + 0xfff) >> 12);
			for (uint32_t i = 0; i < n && first + i < DRG_GPU_POOL_PAGES; i++) {
				k->used[(first + i) / 32] &= ~(1u << ((first + i) % 32));
			}
			return;
		}
	}
	for (unsigned b = 0; b < DRG_GPU_BIG_ALLOCS; b++) {
		if (fBig[b].md != NULL && fBig[b].pa == m->pa) {
			OSSafeReleaseNULL(fBig[b].map);
			fBig[b].md->release();
			bzero(&fBig[b], sizeof(fBig[b]));
			return;
		}
	}
}

uint64_t *
DrgGPU::osPtov(uint64_t pa)
{
	for (unsigned c = 0; c < DRG_GPU_POOL_CHUNKS; c++) {
		struct drg_gpu_chunk *k = &fPool[c];
		if (k->md != NULL && pa >= k->pa && pa < k->pa + DRG_GPU_POOL_CHUNK) {
			return (uint64_t *)(void *)(k->va + (pa - k->pa));
		}
	}
	for (unsigned b = 0; b < DRG_GPU_BIG_ALLOCS; b++) {
		if (fBig[b].map != NULL && pa >= fBig[b].pa && pa < fBig[b].pa + fBig[b].size) {
			return (uint64_t *)(void *)(fBig[b].va + (pa - fBig[b].pa));
		}
	}
	return NULL;
}

/* ---- start ---- */

/* The shim's blob (static memory below topOfKernelData): map, check, point fw at it. */
bool
DrgGPU::loadFirmware(struct drgpu_fw *fw)
{
	uint64_t pa = 0;
	if (!PE_parse_boot_argn("drgpufw", &pa, sizeof(pa)) || pa == 0 || (pa & 0x3fff) != 0) {
		IOLog("DRG GPU: no firmware from the shim (boot-arg drgpufw)\n");
		return false;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(pa, 0x4000, kIODirectionIn);
	IOMemoryMap *head = md != NULL ? md->map(kIOMapReadOnly) : NULL;
	OSSafeReleaseNULL(md);
	if (head == NULL) {
		return false;
	}
	const struct drgpu_fwblob *b = (const struct drgpu_fwblob *)(uintptr_t)head->getVirtualAddress();
	uint64_t len = b->magic == DRGPU_FWBLOB_MAGIC ? sizeof(*b) + b->total : 0;
	head->release();
	if (len == 0 || len > DRG_GPU_FW_MAX) {
		IOLog("DRG GPU: firmware blob at 0x%llx is not valid\n", pa);
		return false;
	}
	md = IOMemoryDescriptor::withPhysicalAddress(pa, (len + 0x3fff) & ~0x3fffull, kIODirectionIn);
	fFwMap = md != NULL ? md->map(kIOMapReadOnly) : NULL;
	OSSafeReleaseNULL(md);
	if (fFwMap == NULL) {
		return false;
	}
	int rc = drgpu_fwblob_parse((const uint8_t *)(uintptr_t)fFwMap->getVirtualAddress(), len, fw);
	if (rc != 0) {
		IOLog("DRG GPU: firmware blob check failed (step %d)\n", -rc);
		return false;
	}
	return true;
}

/*
 * SPI 332: Group 1, priority 0xa0, level, routed to the boot CPU (it never
 * goes offline), as the shim proved it (drg_gic_spi) and Linux sets it up.
 */
void
DrgGPU::gicSetup(void)
{
	const uint32_t n = DRG_GPU_INTID / 32, bit = 1u << (DRG_GPU_INTID % 32);
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t mpidr = topology->boot_cpu->phys_id;
	uint32_t v = drgpu_rd(fGicd + GICD_CTLR);
	if ((v & 0x13) != 0x13) {       /* ARE_NS, EnableGrp1A, EnableGrp1 (Linux gic_dist_init) */
		drgpu_wr(fGicd + GICD_CTLR, v | 0x13);
	}
	drgpu_wr(fGicd + GICD_IGROUPR + 4 * n, drgpu_rd(fGicd + GICD_IGROUPR + 4 * n) | bit);
	uintptr_t pr = fGicd + GICD_IPRIORITYR + (DRG_GPU_INTID & ~3u);
	uint32_t sh = 8 * (DRG_GPU_INTID & 3);
	drgpu_wr(pr, (drgpu_rd(pr) & ~(0xffu << sh)) | 0xa0u << sh);
	uintptr_t cfg = fGicd + GICD_ICFGR + 4 * (DRG_GPU_INTID / 16);
	drgpu_wr(cfg, drgpu_rd(cfg) & ~(2u << (2 * (DRG_GPU_INTID % 16))));     /* level */
	drgpu_wr(fGicd + GICD_IROUTER + 8 * DRG_GPU_INTID, (uint32_t)(mpidr & 0xffffff));
	drgpu_wr(fGicd + GICD_IROUTER + 8 * DRG_GPU_INTID + 4, (uint32_t)((mpidr >> 32) & 0xff));
}

void
DrgGPU::gicEnable(bool on)
{
	const uint32_t n = DRG_GPU_INTID / 32, bit = 1u << (DRG_GPU_INTID % 32);
	drgpu_wr(fGicd + (on ? GICD_ISENABLER : GICD_ICENABLER) + 4 * n, bit);
	for (int i = 0; i < 100000 && (drgpu_rd(fGicd + GICD_CTLR) & GICD_CTLR_RWP); i++) {
		;
	}
}

static void
drg_gpu_irq(void *target __unused, void *refCon __unused, void *nub __unused, int source)
{
	if ((uint32_t)source == DRG_GPU_INTID && gDrgGPU != NULL) {
		gDrgGPU->interrupt();
	}
}

/* sleh_irq calls this CPU's handler for every SPI with its INTID. */
static void
drg_gpu_install_handler(void *arg __unused)
{
	ml_install_interrupt_handler(NULL, (int)DRG_GPU_INTID, NULL, drg_gpu_irq, NULL);
}

/*
 * Interrupt context. The GPU is only read while fIrqLive (powered); the
 * power-off path disables the SPI, clears fIrqLive and waits for fInIrq.
 */
void
DrgGPU::interrupt(void)
{
	os_atomic_inc(&fInIrq, acquire);
	if (os_atomic_load(&fIrqLive, acquire)) {
		uint32_t st = drgpu_irq(&fG);
		if (st & (INT_CP_HW_ERROR | INT_MISC_HANG_DETECT)) {
			/* a hung GPU keeps these asserted: no more GPU interrupts until recovered */
			drgpu_wr(fG.gpu + 4 * RBBM_INT_0_MASK, 0);
			if (os_atomic_cmpxchg(&fFaultStatus, 0u, st | 1u, acq_rel)) {
				thread_call_enter(fRecoverCall);
			}
		}
		if (st & INT_CACHE_FLUSH_TS) {
			os_atomic_inc(&fIrqCount, relaxed);
			if (lastRetired()) {
				uint64_t since = os_atomic_xchg(&fBusySince, 0ull, acq_rel);
				if (since != 0) {
					os_atomic_add(&fBusyAccum, mach_absolute_time() - since, relaxed);
				}
			}
			thread_wakeup((event_t)&fIrqEvent);
		}
	}
	os_atomic_dec(&fInIrq, release);
}

void
DrgGPU::irqLive(bool on)
{
	if (on) {
		os_atomic_store(&fIrqLive, 1u, release);
		gicEnable(true);
	} else {
		gicEnable(false);
		os_atomic_store(&fIrqLive, 0u, release);
		while (os_atomic_load(&fInIrq, acquire) != 0) {
			;
		}
	}
}

uint32_t
DrgGPU::regRead(uint32_t reg)
{
	return drgpu_rd(fG.gpu + 4 * reg);
}

bool
DrgGPU::lastRetired(void)
{
	uint32_t ctx = os_atomic_load(&fLastCtx, acquire);
	return ctx == 0 || ctxRetired(ctx, os_atomic_load(&fLastTs, acquire));
}

bool
DrgGPU::ctxRetired(uint32_t id, uint32_t ts)
{
	return (int32_t)(drgpu_retired(&fG, id) - ts) >= 0;  /* KGSL timestamp compare */
}

bool
DrgGPU::bringUp(void)
{
	if (drgpu_rpm_attach(&fG, 1) != 0) {
		IOLog("DRG GPU: RPM channel not found (the shim opens it)\n");
		return false;
	}
	if (drgpu_power_on(&fG) || drgpu_smmu_attach(&fG) || drgpu_cp_start(&fG) ||
	    drgpu_set_level(&fG, fLevel)) {
		IOLog("DRG GPU: bring-up failed: %s\n", drgpu_err(&fG));
		return false;
	}
	return true;
}

/* A marker through the ring, retired through the interrupt: CP, memstore, IRQ. */
bool
DrgGPU::selfTest(void)
{
	uint32_t irqs = os_atomic_load(&fIrqCount, relaxed);
	uint64_t t0 = mach_absolute_time();
	IOLockLock(fLock);
	fCtx[DRG_GPU_KCTX].ts++;
	uint32_t ts = fCtx[DRG_GPU_KCTX].ts;
	uint32_t got = drgpu_submit(&fG, NULL, DRG_GPU_KCTX, NULL, 0, ts);
	if (got != 0) {
		os_atomic_store(&fLastTs, ts, release);
		os_atomic_store(&fLastCtx, (uint32_t)DRG_GPU_KCTX, release);
	}
	IOLockUnlock(fLock);
	IOReturn rc = got != 0 ? waitTs(DRG_GPU_KCTX, ts, 500) : kIOReturnError;
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	IOLockLock(fLock);
	uint32_t khz = drgpu_gpu_khz(&fG);
	IOLockUnlock(fLock);
	IOLog("DRG GPU: self-test %s: marker retired in %llu us, %u interrupt(s), %u kHz\n",
	    rc == kIOReturnSuccess ? "PASS" : "FAIL", ns / 1000,
	    os_atomic_load(&fIrqCount, relaxed) - irqs, khz);
	return rc == kIOReturnSuccess && os_atomic_load(&fIrqCount, relaxed) != irqs;
}

bool
DrgGPU::start(IOService *provider)
{
	uint32_t on = 1, level = 0xff, idle = DRG_GPU_IDLE_MS;
	struct drgpu_fw fw;
	struct drgpu_os os;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_parse_boot_argn("drggpu", &on, sizeof(on));
	if (!on) {
		IOLog("DRG GPU: off (drggpu=0)\n");
		return false;
	}
	PE_parse_boot_argn("drggpu_level", &level, sizeof(level));
	PE_parse_boot_argn("drggpu_idle", &idle, sizeof(idle));
	fDcvs = level >= DRGPU_LEVELS;
	fLevel = fDcvs ? DRG_GPU_DEFAULT_LEVEL : (int)level;
	fIdleMs = idle;
	fLock = IOLockAlloc();
	fIdleCall = thread_call_allocate(idleTimer, this);
	fDcvsCall = thread_call_allocate(dcvsTimer, this);
	fRecoverCall = thread_call_allocate(recover, this);
	if (fLock == NULL || fIdleCall == NULL || fDcvsCall == NULL || fRecoverCall == NULL) {
		return false;
	}
	bzero(&fw, sizeof(fw));
	if (!loadFirmware(&fw)) {
		return false;
	}
	os.ctx = this;
	os.mmio = drg_gpu_os_mmio;
	os.alloc = drg_gpu_os_alloc;
	os.free = drg_gpu_os_free;
	os.ptov = drg_gpu_os_ptov;
	os.log = drg_gpu_os_log;
	os.sleep_us = drg_gpu_os_sleep;
	bzero(&fG, sizeof(fG));
	if (drgpu_init(&fG, &os, &fw) != 0) {
		IOLog("DRG GPU: init failed: %s\n", drgpu_err(&fG));
		return false;
	}
	fGicd = osMmio(DRG_GPU_GICD, DRG_GPU_GICD_SIZE);
	if (fGicd == 0) {
		return false;
	}
	fCtx[DRG_GPU_KCTX].owner = NULL;
	fCtx[DRG_GPU_KCTX].submitted = true;    /* reserved: never handed out */
	IOLockLock(fLock);
	bool up = bringUp();
	if (up && fG.zap_loaded) {
		/* the zap region now belongs to TZ: no CPU mapping of it stays (its pages stay ours) */
		for (unsigned b = 0; b < DRG_GPU_BIG_ALLOCS; b++) {
			OSSafeReleaseNULL(fBig[b].map);
		}
	}
	IOLockUnlock(fLock);
	if (!up) {
		return false;
	}
	gDrgGPU = this;
	cpu_broadcast_xcall_simple(TRUE, drg_gpu_install_handler, NULL);
	gicSetup();
	irqLive(true);
	if (!selfTest()) {
		irqLive(false);
		IOLockLock(fLock);
		(void)drgpu_power_off(&fG);
		IOLockUnlock(fLock);
		return false;
	}
	IOLockLock(fLock);
	fDcvsLastAbs = mach_absolute_time();
	fLastSubmitAbs = fDcvsLastAbs;
	if (fDcvs) {
		thread_call_enter_delayed(fDcvsCall, drg_gpu_deadline_ms(DRG_GPU_DCVS_MS));
	}
	if (fIdleMs != 0) {
		thread_call_enter_delayed(fIdleCall, drg_gpu_deadline_ms(fIdleMs));
	}
	IOLockUnlock(fLock);
	setProperty("IOUserClientClass", "DrgGPUUserClient");
	setProperty("DrgGPUChipID", 0x05010200ull, 32);
	registerService();
	IOLog("DRG GPU: Adreno 512 up (level %d%s, idle collapse %u ms)\n", fLevel,
	    fDcvs ? ", DCVS" : ", fixed", fIdleMs);
	return true;
}

/* ---- power, clocks, recovery ---- */

/* Lock held. After a collapse (or a failed one) the GPU comes back at the current level. */
IOReturn
DrgGPU::ensurePowered(void)
{
	if (fG.powered && fG.cp_running) {
		return kIOReturnSuccess;
	}
	if (drgpu_resume(&fG, fLevel) != 0) {
		IOLog("DRG GPU: resume failed: %s\n", drgpu_err(&fG));
		return kIOReturnNotResponding;
	}
	irqLive(true);
	fDcvsLastAbs = mach_absolute_time();
	os_atomic_store(&fBusyAccum, 0ull, relaxed);
	if (fDcvs) {
		thread_call_enter_delayed(fDcvsCall, drg_gpu_deadline_ms(DRG_GPU_DCVS_MS));
	}
	return kIOReturnSuccess;
}

/* Lock held, all work retired: GX off, clocks and votes down; CX (and the SMMU) stay on. */
void
DrgGPU::collapse(void)
{
	if (!fG.powered) {
		return;
	}
	irqLive(false);
	if (drgpu_power_off(&fG) != 0) {
		IOLog("DRG GPU: collapse failed: %s\n", drgpu_err(&fG));
		if (fG.powered && fG.cp_running) {
			irqLive(true);
		}
		return;
	}
	thread_call_cancel(fDcvsCall);
	os_atomic_store(&fBusySince, 0ull, relaxed);    /* idle time is not busy time */
}

void
DrgGPU::setLevel(int level)
{
	if (drgpu_set_level(&fG, level) == 0) {
		fLevel = level;
	} else {
		IOLog("DRG GPU: level %d failed: %s\n", level, drgpu_err(&fG));
	}
}

void
DrgGPU::idleTimer(thread_call_param_t p0, thread_call_param_t p1 __unused)
{
	DrgGPU *me = static_cast<DrgGPU *>(p0);
	IOLockLock(me->fLock);
	if (me->fG.powered) {
		uint64_t idle;
		nanoseconds_to_absolutetime((uint64_t)me->fIdleMs * NSEC_PER_MSEC, &idle);
		if (me->lastRetired() && mach_absolute_time() - me->fLastSubmitAbs >= idle &&
		    os_atomic_load(&me->fFaultStatus, acquire) == 0) {
			me->collapse();
		} else {
			thread_call_enter_delayed(me->fIdleCall, drg_gpu_deadline_ms(me->fIdleMs));
		}
	}
	IOLockUnlock(me->fLock);
}

/* Busy share of the last window (submission to retirement, ring order) moves the level one step. */
void
DrgGPU::dcvsTimer(thread_call_param_t p0, thread_call_param_t p1 __unused)
{
	DrgGPU *me = static_cast<DrgGPU *>(p0);
	IOLockLock(me->fLock);
	if (me->fG.powered && me->fG.cp_running && me->fDcvs) {
		uint64_t now = mach_absolute_time();
		uint64_t busy = os_atomic_xchg(&me->fBusyAccum, 0ull, relaxed);
		uint64_t since = os_atomic_load(&me->fBusySince, acquire);
		if (since != 0 && os_atomic_cmpxchg(&me->fBusySince, since, now, acq_rel)) {
			busy += now - since;
		}
		uint64_t window = now - me->fDcvsLastAbs;
		me->fDcvsLastAbs = now;
		if (window != 0) {
			uint64_t pct = busy * 100 / window;
			if (pct > 75 && me->fLevel > 0) {
				me->setLevel(me->fLevel - 1);
			} else if (pct < 30 && me->fLevel < DRGPU_LEVELS - 1) {
				me->setLevel(me->fLevel + 1);
			}
		}
		thread_call_enter_delayed(me->fDcvsCall, drg_gpu_deadline_ms(DRG_GPU_DCVS_MS));
	}
	IOLockUnlock(me->fLock);
}

void
DrgGPU::dumpFault(uint32_t status)
{
	IOLog("DRG GPU: fault, int status 0x%x: cp hw fault 0x%x, cp int 0x%x, protect 0x%x, "
	    "rbbm status 0x%x, rptr 0x%x wptr 0x%x\n", status & ~1u, regRead(CP_HW_FAULT),
	    regRead(CP_INTERRUPT_STATUS), regRead(CP_PROTECT_STATUS), regRead(RBBM_STATUS),
	    regRead(CP_RB_RPTR), fG.ring_w);
}

/*
 * CP_HW_ERROR or a hang (adreno_dispatch fault handling, simplified): power
 * the GPU off and on again (zap retained). Contexts whose work had started
 * are marked faulted (KGSL invalidates the guilty context); every submitted
 * timestamp is written as retired so nobody waits for lost work.
 */
void
DrgGPU::recover(thread_call_param_t p0, thread_call_param_t p1 __unused)
{
	DrgGPU *me = static_cast<DrgGPU *>(p0);
	IOLockLock(me->fLock);
	uint32_t st = os_atomic_load(&me->fFaultStatus, acquire);
	if (st != 0 && me->fG.powered) {
		me->dumpFault(st);
		me->irqLive(false);
		me->fG.cp_running = 0;          /* hung: no idle wait on the way down */
		(void)drgpu_power_off(&me->fG);
	}
	uint8_t *ms = (uint8_t *)me->fG.memstore.va;
	for (uint32_t i = 1; i < DRGPU_MS_RB; i++) {
		struct drg_gpu_ctx *c = &me->fCtx[i];
		if (!c->submitted || me->ctxRetired(i, c->ts)) {
			continue;
		}
		volatile uint32_t *sop = (volatile uint32_t *)(void *)(ms + i * DRGPU_MS_ENTRY + DRGPU_MS_SOP);
		volatile uint32_t *eop = (volatile uint32_t *)(void *)(ms + i * DRGPU_MS_ENTRY + DRGPU_MS_EOP);
		if (*sop != *eop) {
			c->faulted = true;      /* its submission had started */
		}
		*sop = c->ts;
		*eop = c->ts;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	os_atomic_store(&me->fFaultStatus, 0u, release);
	if (me->ensurePowered() != kIOReturnSuccess) {
		IOLog("DRG GPU: recovery failed, GPU stays off\n");
	} else {
		IOLog("DRG GPU: recovered\n");
	}
	thread_wakeup((event_t)&me->fIrqEvent);
	IOLockUnlock(me->fLock);
}

/* ---- contexts and submissions ---- */

int
DrgGPU::ctxCreate(DrgGPUUserClient *owner, uint32_t flags)
{
	for (uint32_t i = 1; i < DRG_GPU_KCTX; i++) {
		if (fCtx[i].owner == NULL) {
			bzero(&fCtx[i], sizeof(fCtx[i]));
			fCtx[i].owner = owner;
			fCtx[i].flags = flags;
			/* a fresh memstore entry (kgsl context init), as no GPU work refers to it */
			bzero((uint8_t *)fG.memstore.va + i * DRGPU_MS_ENTRY, DRGPU_MS_ENTRY);
			return (int)i;
		}
	}
	return 0;
}

void
DrgGPU::ctxRelease(uint32_t id)
{
	bzero(&fCtx[id], sizeof(fCtx[id]));
}

/* No lock: sleeps on the GPU interrupt (wakeups are never lost: assert first, then check). */
IOReturn
DrgGPU::waitTs(uint32_t id, uint32_t ts, uint32_t ms)
{
	uint64_t deadline = drg_gpu_deadline_ms(ms);
	for (;;) {
		if (ctxRetired(id, ts)) {
			return kIOReturnSuccess;
		}
		uint64_t now = mach_absolute_time();
		if (now >= deadline) {
			return kIOReturnTimeout;
		}
		uint64_t slice = drg_gpu_deadline_ms(100);      /* a lost interrupt costs 100 ms, not a hang */
		assert_wait_deadline((event_t)&fIrqEvent, THREAD_ABORTSAFE, slice < deadline ? slice : deadline);
		if (ctxRetired(id, ts)) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			return kIOReturnSuccess;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_INTERRUPTED) {
			return kIOReturnAborted;
		}
	}
}

/* Lock held. */
IOReturn
DrgGPU::submit(DrgGPUUserClient *c, uint32_t ctx, uint32_t flags, uint32_t *ts,
    const struct drgpu_uc_ib *ibs, unsigned n)
{
	struct drgpu_ib kib[DRGPU_UC_MAX_IBS];

	if (ctx == 0 || ctx >= DRG_GPU_KCTX || fCtx[ctx].owner != c || n > DRGPU_UC_MAX_IBS) {
		return kIOReturnBadArgument;
	}
	if (fCtx[ctx].faulted) {
		return kIOReturnIOError;        /* KGSL: the context was invalidated by a fault */
	}
	if (flags & DRGPU_KGSL_CMDBATCH_MARKER) {
		n = 0;                          /* markers only carry the timestamp */
	}
	for (unsigned i = 0; i < n; i++) {
		if ((ibs[i].gpuaddr & 3) != 0 || ibs[i].gpuaddr == 0 || ibs[i].gpuaddr >= DRG_GPU_VA_END ||
		    ibs[i].bytes == 0 || (ibs[i].bytes & 3) != 0 || ibs[i].bytes / 4 > 0xfffff) {
			return kIOReturnBadArgument;
		}
		kib[i].gpuaddr = ibs[i].gpuaddr;
		kib[i].dwords = (uint32_t)(ibs[i].bytes / 4);
	}
	if (!(fCtx[ctx].flags & DRGPU_KGSL_CONTEXT_USER_GENERATED_TS)) {
		*ts = ++fCtx[ctx].gen;
	}
	if (*ts == 0) {
		return kIOReturnBadArgument;
	}
	IOReturn rc = ensurePowered();
	if (rc != kIOReturnSuccess) {
		return rc;
	}
	if (drgpu_submit(&fG, &c->fSpace, ctx, kib, n, *ts) == 0) {
		IOLog("DRG GPU: submit failed: %s\n", drgpu_err(&fG));
		return kIOReturnNoResources;
	}
	fCtx[ctx].ts = *ts;
	fCtx[ctx].submitted = true;
	os_atomic_store(&fLastTs, *ts, release);
	os_atomic_store(&fLastCtx, ctx, release);
	uint64_t now = mach_absolute_time();
	(void)os_atomic_cmpxchg(&fBusySince, 0ull, now, acq_rel);
	fLastSubmitAbs = now;
	if (fIdleMs != 0) {
		thread_call_enter_delayed(fIdleCall, drg_gpu_deadline_ms(fIdleMs));
	}
	return kIOReturnSuccess;
}

/* ---- user client ---- */


/* KGSL property answers Nokia's driver got on this phone (kgsl-trace-2.txt), byte for byte. */
static const struct {
	uint32_t type, len;
	uint8_t  b[40];
} drg_gpu_props[] = {
	{ 0x01, 40, { 0x01, 0, 0, 0, 0x00, 0x02, 0x01, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0,     /* DEVICE_INFO */
		      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		      0x00, 0x40, 0x04, 0, 0, 0, 0, 0 } },
	{ 0x02, 24, { 0x00, 0x10, 0x00, 0xf8, 0, 0, 0, 0, 0x00, 0x20, 0, 0, 0, 0, 0, 0,        /* DEVICE_SHADOW */
		      0x04, 0x02, 0, 0, 0, 0, 0, 0 } },
	{ 0x13, 8, { 0x00, 0x00, 0x10, 0x00, 0, 0, 0, 0 } },                                     /* UCHE_GMEM_VADDR */
	{ 0x15, 8, { 0x12, 0xf1, 0x5f, 0x00, 0x66, 0xf0, 0x5f, 0x00 } },                         /* UCODE_VERSION */
	{ 0x17, 4, { 0x0e, 0, 0, 0 } },                                                          /* HIGHEST_BANK_BIT */
	{ 0x18, 4, { 0x30, 0, 0, 0 } },                                                          /* DEVICE_BITNESS */
	{ 0x19, 16, { 0 } },                                                                     /* DEVICE_QDSS_STM */
	{ 0x20, 16, { 0 } },                                                                     /* DEVICE_QTIMER */
};

bool
DrgGPUUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (type != 0 || !IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
DrgGPUUserClient::start(IOService *provider)
{
	fGPU = OSDynamicCast(DrgGPU, provider);
	if (fGPU == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	IOLockLock(fGPU->fLock);
	bzero(&fSpace, sizeof(fSpace));
	fSpaceLive = drgpu_space_create(&fGPU->fG, &fSpace) == 0;
	IOLockUnlock(fGPU->fLock);
	if (!fSpaceLive) {
		IOLog("DRG GPU: no GPU address space for a new client: %s\n", drgpu_err(&fGPU->fG));
		return false;
	}
	return true;
}

struct drg_gpu_obj *
DrgGPUUserClient::obj(uint64_t id)
{
	if (id == 0 || id > fObjCap || fObjs[id - 1].md == NULL || fObjs[id - 1].pending) {
		return NULL;
	}
	return &fObjs[id - 1];
}

/*
 * Lock held. md owns wired pages: map them (as physical ranges, write-
 * combined) into the task anywhere, then at the same address into this task's
 * GPU space. out: id, GPU address, size.
 */
IOReturn
DrgGPUUserClient::objInsert(IOMemoryDescriptor *md, uint64_t size, bool ro, bool surface, uint64_t *out)
{
	uint32_t slot = 0;
	while (slot < fObjCap && fObjs[slot].md != NULL) {
		slot++;
	}
	if (slot == fObjCap) {
		uint32_t cap = fObjCap ? fObjCap * 2 : 64;
		if (cap > DRG_GPU_MAX_OBJS) {
			return kIOReturnNoResources;
		}
		struct drg_gpu_obj *objs = IONewZero(struct drg_gpu_obj, cap);
		if (objs == NULL) {
			return kIOReturnNoMemory;
		}
		if (fObjs != NULL) {
			bcopy(fObjs, objs, fObjCap * sizeof(*objs));
			IODelete(fObjs, struct drg_gpu_obj, fObjCap);
		}
		fObjs = objs;
		fObjCap = cap;
	}
	IOMemoryDescriptor *phys = drg_gpu_phys_view(md, size);
	if (phys == NULL) {
		return kIOReturnNoMemory;
	}
	/* KGSL_MEMFLAGS_GPUREADONLY limits the GPU only: the CPU fills the object */
	IOMemoryMap *map = phys->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
	if (map == NULL) {
		phys->release();
		return kIOReturnNoMemory;
	}
	uint64_t va = map->getAddress(), off = 0;
	if (va < DRG_GPU_VA_MIN || va + size > DRG_GPU_VA_END || map->getLength() < size) {
		map->release();
		phys->release();
		return kIOReturnNoSpace;
	}
	while (off < size) {
		IOByteCount seg = 0;
		uint64_t pa = phys->getPhysicalSegment(off, &seg, kIOMemoryMapperNone);
		if (seg > size - off) {
			seg = size - off;
		}
		if (pa == 0 || ((pa | seg) & 0xfff) != 0 || seg == 0 ||
		    drgpu_map(&fGPU->fG, &fSpace, va + off, pa, seg, ro) != 0) {
			drgpu_unmap(&fGPU->fG, &fSpace, va, off);
			(void)drgpu_tlb_flush(&fGPU->fG);
			map->release();
			phys->release();
			return kIOReturnNoMemory;
		}
		off += seg;
	}
	struct drg_gpu_obj *o = &fObjs[slot];
	bzero(o, sizeof(*o));
	md->retain();
	o->md = md;
	o->phys = phys;
	o->map = map;
	o->gpuaddr = va;
	o->size = size;
	o->surface = surface;
	out[0] = slot + 1;
	out[1] = va;
	out[2] = size;
	return kIOReturnSuccess;
}

IOReturn
DrgGPUUserClient::objAlloc(uint64_t size, uint64_t flags, uint64_t *out)
{
	if (size == 0 || size > DRG_GPU_OBJ_MAX) {
		return kIOReturnBadArgument;
	}
	size = round_page(size);
	if (fBytes + size > DRG_GPU_CLIENT_MAX) {
		return kIOReturnNoMemory;
	}
	/* KGSL's default cache mode (memflags bits 26-27 = 0) is write-combined */
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut, size, page_size);
	if (md == NULL) {
		return kIOReturnNoMemory;
	}
	bzero(md->getBytesNoCopy(), size);
	md->performOperation(kIOMemoryIncoherentIOFlush, 0, size);
	IOReturn rc = objInsert(md, size, (flags & DRGPU_KGSL_MEMFLAGS_GPUREADONLY) != 0, false, out);
	md->release();
	if (rc == kIOReturnSuccess) {
		fBytes += size;
	}
	return rc;
}

/*
 * An IOSurface's pages at a new address in the task, the same address on the
 * GPU. The task's other mappings of the surface keep their cache mode: CPU
 * writers through a cached one call OBJ_SYNC before the GPU reads, and after
 * the GPU wrote, before they read.
 */
IOReturn
DrgGPUUserClient::surfaceMap(uint32_t surfaceID, uint64_t *out)
{
	DrgSurface *s = drg_surface_copy(surfaceID);
	if (s == NULL || s->memory == NULL) {
		OSSafeReleaseNULL(s);
		return kIOReturnNotFound;
	}
	IOMemoryDescriptor *md = s->memory;
	uint64_t size = round_page(md->getLength());
	IOReturn rc = md->prepare();
	if (rc == kIOReturnSuccess) {
		md->performOperation(kIOMemoryIncoherentIOFlush, 0, md->getLength());
		rc = objInsert(md, size, false, true, out);
		if (rc != kIOReturnSuccess) {
			md->complete();
		}
	}
	s->release();
	return rc;
}

/* Lock held: off the GPU (the caller flushes the TLB, then releaseUnmapped). */
void
DrgGPUUserClient::unmapObj(struct drg_gpu_obj *o)
{
	drgpu_unmap(&fGPU->fG, &fSpace, o->gpuaddr, o->size);
	o->unmapped = true;
}

/* Lock held, TLB flushed: give the pages back. */
void
DrgGPUUserClient::releaseUnmapped(void)
{
	for (uint32_t i = 0; i < fObjCap; i++) {
		struct drg_gpu_obj *o = &fObjs[i];
		if (o->md == NULL || !o->unmapped) {
			continue;
		}
		o->map->release();
		o->phys->release();
		if (o->surface) {
			o->md->complete();
		} else {
			fBytes -= o->size;
		}
		o->md->release();
		bzero(o, sizeof(*o));
	}
}

/* Lock held: frees whose timestamp retired (all: every pending one). */
void
DrgGPUUserClient::retire(bool all)
{
	if (fPending == 0) {
		return;
	}
	bool any = false;
	for (uint32_t i = 0; i < fObjCap; i++) {
		struct drg_gpu_obj *o = &fObjs[i];
		if (o->md != NULL && o->pending && (all || fGPU->ctxRetired(o->pendCtx, o->pendTs))) {
			unmapObj(o);
			o->pending = false;
			fPending--;
			any = true;
		}
	}
	if (any) {
		(void)drgpu_tlb_flush(&fGPU->fG);
		releaseUnmapped();
	}
}

/* Lock held. */
IOReturn
DrgGPUUserClient::objFree(uint32_t id, bool onEvent, uint32_t ctx, uint32_t ts)
{
	struct drg_gpu_obj *o = obj(id);
	if (o == NULL) {
		return kIOReturnBadArgument;
	}
	if (onEvent) {
		if (ctx == 0 || ctx >= DRG_GPU_KCTX || fGPU->fCtx[ctx].owner != this) {
			return kIOReturnBadArgument;
		}
		if (!fGPU->ctxRetired(ctx, ts)) {
			o->pending = true;
			o->pendCtx = ctx;
			o->pendTs = ts;
			fPending++;
			return kIOReturnSuccess;
		}
	}
	unmapObj(o);
	(void)drgpu_tlb_flush(&fGPU->fG);
	releaseUnmapped();
	return kIOReturnSuccess;
}

IOReturn
DrgGPUUserClient::waitCtx(uint32_t id, uint32_t ts, uint32_t ms)
{
	if (id == 0 || id >= DRG_GPU_KCTX || fGPU->fCtx[id].owner != this) {
		return kIOReturnBadArgument;
	}
	IOReturn rc = fGPU->waitTs(id, ts, ms);
	if (rc == kIOReturnSuccess && fGPU->fCtx[id].faulted) {
		rc = kIOReturnIOError;
	}
	return rc;
}

/* No lock on entry: waits for the context's work (2 s, then a GPU recovery drops it). */
IOReturn
DrgGPUUserClient::ctxDestroy(uint32_t id)
{
	IOLockLock(fGPU->fLock);
	bool mine = id != 0 && id < DRG_GPU_KCTX && fGPU->fCtx[id].owner == this;
	uint32_t ts = fGPU->fCtx[id < DRG_GPU_KCTX ? id : 0].ts;
	bool submitted = mine && fGPU->fCtx[id].submitted;
	IOLockUnlock(fGPU->fLock);
	if (!mine) {
		return kIOReturnBadArgument;
	}
	if (submitted && fGPU->waitTs(id, ts, 2000) != kIOReturnSuccess) {
		IOLog("DRG GPU: context %u did not retire 0x%x, recovering\n", id, ts);
		if (os_atomic_cmpxchg(&fGPU->fFaultStatus, 0u, 1u, acq_rel)) {
			thread_call_enter(fGPU->fRecoverCall);
		}
		(void)fGPU->waitTs(id, ts, 2000);
	}
	IOLockLock(fGPU->fLock);
	retire(false);
	/* frees still waiting on this context: its work is done or dropped */
	bool any = false;
	for (uint32_t i = 0; i < fObjCap; i++) {
		struct drg_gpu_obj *o = &fObjs[i];
		if (o->md != NULL && o->pending && o->pendCtx == id) {
			unmapObj(o);
			o->pending = false;
			fPending--;
			any = true;
		}
	}
	if (any) {
		(void)drgpu_tlb_flush(&fGPU->fG);
		releaseUnmapped();
	}
	fGPU->ctxRelease(id);
	IOLockUnlock(fGPU->fLock);
	return kIOReturnSuccess;
}

IOReturn
DrgGPUUserClient::kgslProperty(uint32_t type, IOExternalMethodArguments *args)
{
	for (unsigned i = 0; i < sizeof(drg_gpu_props) / sizeof(drg_gpu_props[0]); i++) {
		if (drg_gpu_props[i].type != type) {
			continue;
		}
		if (args->structureOutputDescriptor != NULL || args->structureOutputSize != drg_gpu_props[i].len) {
			return kIOReturnBadArgument;
		}
		bcopy(drg_gpu_props[i].b, args->structureOutput, drg_gpu_props[i].len);
		return kIOReturnSuccess;
	}
	return kIOReturnUnsupported;    /* KGSL: EINVAL (0x21, 0x23, 0x24 on this phone) */
}

/* Everything this client holds, after its work is done. */
void
DrgGPUUserClient::teardown(void)
{
	for (uint32_t id = 1; id < DRG_GPU_KCTX; id++) {
		IOLockLock(fGPU->fLock);
		bool mine = fGPU->fCtx[id].owner == this;
		IOLockUnlock(fGPU->fLock);
		if (mine) {
			(void)ctxDestroy(id);
		}
	}
	IOLockLock(fGPU->fLock);
	retire(true);
	for (uint32_t i = 0; i < fObjCap; i++) {
		if (fObjs[i].md != NULL) {
			unmapObj(&fObjs[i]);
		}
	}
	(void)drgpu_tlb_flush(&fGPU->fG);
	releaseUnmapped();
	if (fSpaceLive && drgpu_space_destroy(&fGPU->fG, &fSpace) == 0) {
		fSpaceLive = false;
	} else if (fSpaceLive) {
		IOLog("DRG GPU: address space kept: %s\n", drgpu_err(&fGPU->fG));
	}
	IOLockUnlock(fGPU->fLock);
	if (fObjs != NULL) {
		IODelete(fObjs, struct drg_gpu_obj, fObjCap);
		fObjs = NULL;
		fObjCap = 0;
	}
	OSSafeReleaseNULL(fMemstore);
}

IOReturn
DrgGPUUserClient::clientClose(void)
{
	if (!fClosed) {
		fClosed = true;
		teardown();
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
DrgGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	uint64_t out[3] = { 0, 0, 0 };
	uint32_t outCount = 0;
	const uint64_t *in = args->scalarInput;
	uint32_t nin = args->scalarInputCount;
	IOReturn rc = kIOReturnSuccess;

	if (fClosed || !fSpaceLive) {
		return kIOReturnNotAttached;
	}
	switch (selector) {
	case DRGPU_UC_GETPROPERTY:
		rc = nin >= 1 ? kgslProperty((uint32_t)in[0], args) : kIOReturnBadArgument;
		break;
	case DRGPU_UC_CTX_CREATE: {
		IOLockLock(fGPU->fLock);
		int id = fGPU->ctxCreate(this, nin >= 1 ? (uint32_t)in[0] : 0);
		IOLockUnlock(fGPU->fLock);
		rc = id != 0 ? kIOReturnSuccess : kIOReturnNoResources;
		out[0] = (uint64_t)id;
		outCount = 1;
		break;
	}
	case DRGPU_UC_CTX_DESTROY:
		rc = nin >= 1 ? ctxDestroy((uint32_t)in[0]) : kIOReturnBadArgument;
		break;
	case DRGPU_UC_OBJ_ALLOC:
		if (nin < 2) {
			rc = kIOReturnBadArgument;
			break;
		}
		IOLockLock(fGPU->fLock);
		retire(false);
		rc = objAlloc(in[0], in[1], out);
		IOLockUnlock(fGPU->fLock);
		outCount = 3;
		break;
	case DRGPU_UC_OBJ_FREE:
		if (nin < 4) {
			rc = kIOReturnBadArgument;
			break;
		}
		IOLockLock(fGPU->fLock);
		rc = objFree((uint32_t)in[0], in[1] != 0, (uint32_t)in[2], (uint32_t)in[3]);
		retire(false);
		IOLockUnlock(fGPU->fLock);
		break;
	case DRGPU_UC_SUBMIT: {
		unsigned n = (unsigned)(args->structureInputSize / sizeof(struct drgpu_uc_ib));
		if (nin < 3 || args->structureInputDescriptor != NULL ||
		    args->structureInputSize % sizeof(struct drgpu_uc_ib) != 0 || n > DRGPU_UC_MAX_IBS) {
			rc = kIOReturnBadArgument;
			break;
		}
		struct drgpu_uc_ib ibs[DRGPU_UC_MAX_IBS];
		if (n != 0) {
			bcopy(args->structureInput, ibs, n * sizeof(ibs[0]));
		}
		uint32_t ts = (uint32_t)in[2];
		IOLockLock(fGPU->fLock);
		retire(false);
		rc = fGPU->submit(this, (uint32_t)in[0], (uint32_t)in[1], &ts, ibs, n);
		IOLockUnlock(fGPU->fLock);
		out[0] = ts;
		outCount = 1;
		break;
	}
	case DRGPU_UC_WAIT:
		rc = nin >= 3 ? waitCtx((uint32_t)in[0], (uint32_t)in[1], (uint32_t)in[2]) : kIOReturnBadArgument;
		break;
	case DRGPU_UC_MEMSTORE:
		IOLockLock(fGPU->fLock);
		if (fMemstore == NULL) {
			IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(fGPU->fG.memstore.pa,
			    fGPU->fG.memstore.size, kIODirectionIn);
			if (md != NULL) {
				fMemstore = md->createMappingInTask(fTask, 0,
				    kIOMapAnywhere | kIOMapReadOnly | kIOMapWriteCombineCache);
				md->release();
			}
		}
		IOLockUnlock(fGPU->fLock);
		if (fMemstore == NULL) {
			rc = kIOReturnNoMemory;
			break;
		}
		out[0] = fMemstore->getAddress();
		out[1] = DRGPU_MEMSTORE_SIZE;
		outCount = 2;
		break;
	case DRGPU_UC_SYNCSRC_CREATE:
		out[0] = ++fSyncSrc;
		outCount = 1;
		break;
	case DRGPU_UC_SYNCSRC_DESTROY:
		break;
	case DRGPU_UC_SURFACE_MAP:
		if (nin < 1) {
			rc = kIOReturnBadArgument;
			break;
		}
		IOLockLock(fGPU->fLock);
		rc = surfaceMap((uint32_t)in[0], out);
		IOLockUnlock(fGPU->fLock);
		outCount = 3;
		break;
	case DRGPU_UC_OBJ_SYNC: {
		IOLockLock(fGPU->fLock);
		struct drg_gpu_obj *o = nin >= 1 ? obj(in[0]) : NULL;
		if (o != NULL) {
			o->md->performOperation(kIOMemoryIncoherentIOFlush, 0, o->size);
		}
		IOLockUnlock(fGPU->fLock);
		rc = o != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	}
	case DRGPU_UC_GET_VERSION:
		out[0] = DRGPU_UC_VERSION;
		outCount = 1;
		break;
	default:
		rc = kIOReturnUnsupported;
		break;
	}
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	return rc;
}
#endif /* DRG */
