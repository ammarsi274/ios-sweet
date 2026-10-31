/*
 * drgpu: portable driver core for the DRG (Nokia 6.1 Plus) Adreno 512.
 * Used by the shim (tests on the real GPU) and by XNU's DrgGPU. Every step
 * is the one measured on this phone (drg/gpu-research/TWRP-MEASURED.md
 * §6b-6g) or HMD's DRG kernel source at the place named in each function.
 *
 * Lifetime: drgpu_init (map, allocate) -> drgpu_power_on -> drgpu_smmu_attach
 * -> drgpu_cp_start (microcode, zap shader once per boot, ME_INIT, secure
 * mode off) -> drgpu_set_level / drgpu_submit / drgpu_retired ...
 */
#ifndef DRGPU_H
#define DRGPU_H
#include "drgpu_os.h"
#include "drgpu_pt.h"

/* Nokia firmware (vendor_b /vendor/firmware), handed in by the OS */
struct drgpu_fw {
	const uint8_t *pm4;	/* a530_pm4.fw, whole file */
	uint32_t pm4_len;
	const uint8_t *pfp;	/* a530_pfp.fw */
	uint32_t pfp_len;
	const uint8_t *zap_mdt;	/* a512_zap.mdt */
	uint32_t zap_mdt_len;
	const uint8_t *zap_b02;	/* a512_zap.b02, the one loadable segment */
	uint32_t zap_b02_len;
};

/* KGSL's global GPU addresses on this phone (ftrace map_sg, TWRP) */
#define DRGPU_VA_SETSTATE	0xf8000000ULL	/* 4 KiB, a NOP IB at +1024 (KGSL_IOMMU_SETSTATE_NOP_OFFSET) */
#define DRGPU_VA_MEMSTORE	0xf8001000ULL	/* 8 KiB, DEVICE_SHADOW */
#define DRGPU_VA_SCRATCH	0xf8003000ULL	/* RPTR shadow at +0 */
#define DRGPU_VA_RING		0xf8005000ULL	/* 32 KiB */
#define DRGPU_VA_PM4		0xf8058000ULL
#define DRGPU_VA_PFP		0xf805d000ULL
#define DRGPU_VA_SMMU_INFO	0xf80e8000ULL	/* 4 KiB, struct a5xx_cp_smmu_info (0x840) */
#define DRGPU_MEMSTORE_SIZE	0x2000
#define DRGPU_RING_DWORDS	8192		/* RB_CNTL 0x20c */

/* struct kgsl_devmemstore (msm_kgsl.h): 40 bytes per context id */
#define DRGPU_MS_SOP		0
#define DRGPU_MS_EOP		8
#define DRGPU_MS_REF_WAIT	24
#define DRGPU_MS_CURCTX		32
#define DRGPU_MS_ENTRY		40
#define DRGPU_MS_RB		(DRGPU_MEMSTORE_SIZE / DRGPU_MS_ENTRY - 1 - 4)	/* KGSL_MEMSTORE_MAX: ring 0 */

#define DRGPU_LEVELS		4	/* 0 = 430, 1 = 370, 2 = 266, 3 = 160 MHz */

/*
 * A GPU address space: one per process (KGSL's per-process pagetable),
 * plus the kernel's own. Every space carries the global buffers, so the CP,
 * ring and memstore work whichever one is current. The CP switches spaces
 * in the ring (CP_SMMU_TABLE_UPDATE, adreno_iommu.c a5xx path); each has
 * its own ASID in TTBR0, so TLB entries never mix.
 */
struct drgpu_space {
	struct drgpu_pt pt;
	struct drgpu_mem l1;
	uint32_t asid;
	int live;
};

struct drgpu {
	struct drgpu_os os;
	struct drgpu_fw fw;
	uintptr_t gcc, gpucc, gpu, smmu, ldo, tcsr, rpm_ram, rpm_ipc;
	struct drgpu_mem ring, memstore, scratch, pm4, pfp, setstate, smmu_info;
	struct drgpu_space kspace;	/* the kernel's: CB0's TTBR0 after attach */
	struct drgpu_space *cur_space;	/* the one the CP has selected */
	uint8_t asid_used[256];
	uint32_t ring_w;		/* next free dword */
	uint32_t rb_ts;			/* ring 0 timestamp */
	int zap_loaded, cp_running, powered;
	int level, pll;			/* -1 = on XO */
	uint32_t cx, memacc;
	uint64_t bw;
	/* RPM client state (drgpu_rpm.c) */
	uintptr_t rpm_tx_desc, rpm_tx_fifo, rpm_rx_desc, rpm_rx_fifo;
	uint32_t rpm_tx_size, rpm_rx_size, rpm_msg_id, rpm_acks, rpm_last_ack[4];
	int rpm_up;
	uint32_t cur_ctx;		/* context of the last submission */
	const char *errmsg;
};

int drgpu_init(struct drgpu *g, const struct drgpu_os *os, const struct drgpu_fw *fw);
int drgpu_power_on(struct drgpu *g);		/* CX, BHS, gfx3d on XO, GX, branches */
int drgpu_smmu_attach(struct drgpu *g);		/* stream 0 -> CB0, globals mapped */
int drgpu_cp_start(struct drgpu *g);		/* start sequence .. secure mode off */
int drgpu_set_level(struct drgpu *g, int level);
/* idle: GX collapsed, PLLs and GPU clocks off, votes down; CX (and the SMMU in it) stays on */
int drgpu_power_off(struct drgpu *g);
int drgpu_resume(struct drgpu *g, int level);	/* power on, CP restart (zap retained), level */
uint32_t drgpu_gpu_khz(struct drgpu *g);	/* measured with an RBBM counter */

/*
 * One IB list for context ctx in space s (NULL: the kernel's), KGSL framing;
 * n = 0 is a marker (timestamp only). Returns ts, 0 on error.
 */
struct drgpu_ib { uint64_t gpuaddr; uint32_t dwords; };
uint32_t drgpu_submit(struct drgpu *g, struct drgpu_space *s, uint32_t ctx, const struct drgpu_ib *ibs, unsigned n,
		      uint32_t ts);
uint32_t drgpu_retired(struct drgpu *g, uint32_t ctx);	/* memstore eoptimestamp */
int drgpu_wait(struct drgpu *g, uint32_t ctx, uint32_t ts, uint32_t timeout_us);	/* 0 or -1 */
uint32_t drgpu_irq(struct drgpu *g);		/* reads and clears RBBM_INT_0_STATUS */

/* address spaces (one per process) */
int drgpu_space_create(struct drgpu *g, struct drgpu_space *s);
int drgpu_space_destroy(struct drgpu *g, struct drgpu_space *s);	/* GPU must be done with it */

/* GPU memory in space s (NULL: the kernel's); flush the TLB after a batch of changes */
int drgpu_map(struct drgpu *g, struct drgpu_space *s, uint64_t gpuaddr, uint64_t pa, uint64_t len, int ro);
void drgpu_unmap(struct drgpu *g, struct drgpu_space *s, uint64_t gpuaddr, uint64_t len);
int drgpu_tlb_flush(struct drgpu *g);

/* RPM requests (drgpu_rpm.c) */
int drgpu_rpm_attach(struct drgpu *g, int link_already_open);
int drgpu_rpm_kvp2(struct drgpu *g, uint32_t type, uint32_t id, uint32_t k1, uint32_t v1, uint32_t k2, uint32_t v2);
int drgpu_rpm_kvp64(struct drgpu *g, uint32_t type, uint32_t id, uint32_t key, uint64_t v);

/* diagnostics: last error text */
const char *drgpu_err(const struct drgpu *g);

#endif
