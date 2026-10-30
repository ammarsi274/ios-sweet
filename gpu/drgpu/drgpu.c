/*
 * drgpu core, see drgpu.h. Each sequence is the one the shim proved on the
 * phone (shim/drg_gpu.c G2a-G3r, drg/gpu-research/TWRP-MEASURED.md §6b-6g),
 * whose sources are HMD's DRG kernel files named at each step.
 */
#include "drgpu.h"
#include "drgpu_start.h"

/* ---- addresses: this phone's device tree (device-info/fdt.dts) ---- */
#define PA_GCC_GPU	0x171000ULL	/* GCC 0x100000: gpu_cfg_ahb +0x71004 .. bimc_gfx +0x7106c */
#define PA_GPUCC	0x5065000ULL	/* clock-controller@5065000 .. GDSCs 0x5066004 / 0x5066094 */
#define PA_LDO		0x506e000ULL	/* msm_gfx_ldo */
#define PA_GPU		0x5000000ULL	/* kgsl-3d0, 0x40000 */
#define PA_SMMU		0x5040000ULL	/* arm,smmu-kgsl */
#define PA_TCSR_ACC	0x1fcf000ULL	/* gfx_mem_acc_corner acc-sel-l1 0x1fcf004 */
#define PA_RPM_RAM	0x778000ULL	/* glink-rpm msgram, 0x7000 */
#define PA_RPM_IPC	0x17911000ULL	/* IPC 0x17911008 */

/* GCC branches (gcc-sdm660.c) */
#define GCC_GPU_CFG_AHB(g)	((g)->gcc + 0x004)
#define GCC_GPU_BIMC_GFX(g)	((g)->gcc + 0x010)
#define GCC_BIMC_GFX(g)		((g)->gcc + 0x06c)
/* GPUCC (gpucc-sdm660.c, gdsc DT nodes) */
#define PLL(g, n)		((g)->gpucc + 0x40UL * (n))
#define RBCPR_CBCR(g)		((g)->gpucc + 0x1054)
#define CXO_CBCR(g)		((g)->gpucc + 0x1020)
#define GFX3D_CMD(g)		((g)->gpucc + 0x1070)
#define GFX3D_CFG(g)		((g)->gpucc + 0x1074)
#define GFX3D_CBCR(g)		((g)->gpucc + 0x1098)
#define RBBMTIMER_CBCR(g)	((g)->gpucc + 0x10d0)
#define GX_DOMAIN(g)		((g)->gpucc + 0x130)
#define GDSC_CX(g)		((g)->gpucc + 0x1004)
#define GX_SW_RESET(g)		((g)->gpucc + 0x1090)
#define GDSC_GX(g)		((g)->gpucc + 0x1094)
#define LDO_PWRSWITCH_CTRL(g)	((g)->ldo + 0x1c)
#define MEM_ACC_SEL(g)		((g)->tcsr + 0x004)

#define CBCR_CLK_OFF		(1U << 31)
#define RCG_UPDATE		(1U << 0)
#define RCG_ROOT_EN		(1U << 1)
#define GDSC_PWR_ON		(1U << 31)
#define GDSC_SW_COLLAPSE	(1U << 0)
#define GDSC_HW_CONTROL		(1U << 1)

/* alpha PLL (clk-alpha-pll.c) */
#define PLL_OUTCTRL		(1U << 0)
#define PLL_BYPASSNL		(1U << 1)
#define PLL_RESET_N		(1U << 2)
#define PLL_VOTE_FSM_ENA	(1U << 20)
#define PLL_LOCK_DET		(1U << 31)
#define PLL_VCO_MASK		(3U << 20)
#define PLL_VCO_500_1000	(2U << 20)
#define PLL_POSTDIV_MASK	(0xfU << 8)
#define GPU_PLL_CONFIG_CTL	0x4001055bu

/* GPU registers (a5xx_reg.h), dword index */
#define R(g, r)			((g)->gpu + 4UL * (r))
#define RBBM_CLOCK_CNTL		0x67
#define RBBM_INT_CLEAR_CMD	0x37
#define RBBM_PERFCTR_RBBM_1_LO	0x3b2
#define RBBM_PERFCTR_RBBM_1_HI	0x3b3
#define RBBM_PERFCTR_RBBM_SEL_1	0x46c
#define RBBM_INT_0_STATUS	0x4e1
#define RBBM_STATUS		0x4f5
#define RBBM_ISDB_CNT		0x533
#define CP_RB_BASE		0x800
#define CP_RB_BASE_HI		0x801
#define CP_RB_CNTL		0x802
#define CP_RB_RPTR_ADDR_LO	0x804
#define CP_RB_RPTR_ADDR_HI	0x805
#define CP_RB_RPTR		0x806
#define CP_RB_WPTR		0x807
#define CP_ME_CNTL		0x832
#define CP_PFP_INSTR_BASE_LO	0x835
#define CP_PFP_INSTR_BASE_HI	0x836
#define CP_PM4_INSTR_BASE_LO	0x838
#define CP_PM4_INSTR_BASE_HI	0x839
#define CP_HW_FAULT		0xb1a
#define RBBM_SECVID_TRUST_CNTL	0xf400
#define A5XX_CP_RB_CNTL_DEFAULT	0x20c	/* measured */

/* PM4 (adreno_pm4types.h, a3xx_reg.h) */
#define CP_NOP			0x10
#define CP_MEM_WRITE		0x3d
#define CP_INDIRECT_BUFFER_PFE	0x3f
#define CP_EVENT_WRITE		0x46
#define CP_ME_INIT		0x48
#define CP_SET_SECURE_MODE	0x66
#define CACHE_FLUSH_TS		4

/* SMMU (arm-smmu.c, measured IDs: 4 KiB pages, CBs at +0x8000) */
#define SMMU_SGFSR(g)		((g)->smmu + 0x048)
#define SMMU_TLBIALLNSNH(g)	((g)->smmu + 0x068)
#define SMMU_TLBIALLH(g)	((g)->smmu + 0x06c)
#define SMMU_STLBGSYNC(g)	((g)->smmu + 0x070)
#define SMMU_STLBGSTATUS(g)	((g)->smmu + 0x074)
#define SMMU_SCR0(g)		((g)->smmu + 0x000)
#define SMMU_SMR0(g)		((g)->smmu + 0x800)
#define SMMU_S2CR0(g)		((g)->smmu + 0xc00)
#define SMMU_CBAR0(g)		((g)->smmu + 0x1000)
#define SMMU_CBA2R0(g)		((g)->smmu + 0x1800)
#define SMMU_MICRO_MMU_CTRL(g)	((g)->smmu + 0x6000)
#define CB0(g, o)		((g)->smmu + 0x8000 + (o))
#define CB_SCTLR		0x00
#define CB_ACTLR		0x04
#define CB_TCR2			0x10
#define CB_TTBR0		0x20
#define CB_TTBR1		0x28
#define CB_TCR			0x30
#define CB_MAIR0		0x38
#define CB_MAIR1		0x3c
#define CB_TLBIALL		0x618
#define CB_TLBSYNC		0x7f0
#define CB_TLBSTATUS		0x7f4

/* RPM resources and keys (rpm-smd-regulator.c, msm-bus-board.h) */
#define RPM_RES_RWCX		0x78637772u
#define RPM_RES_RWMX		0x786d7772u
#define RPM_RES_BSLV		0x766c7362u
#define RPM_KEY_SWEN		0x6e657773u
#define RPM_KEY_VLVL		0x6c766c76u
#define RPM_KEY_BW		0x00007762u

/* ---- helpers ---- */
static int fail(struct drgpu *g, const char *msg)
{
	g->errmsg = msg;
	g->os.log(g->os.ctx, msg);
	return -1;
}

static void logx(struct drgpu *g, const char *label, uint64_t v)
{
	char b[96];
	unsigned n = 0;
	while (*label && n < 60)
		b[n++] = *label++;
	b[n++] = ' ';
	b[n++] = '0';
	b[n++] = 'x';
	int started = 0;
	for (int i = 15; i >= 0; i--) {
		unsigned d = (unsigned)(v >> (4 * i)) & 15;
		if (d || started || i == 0) {
			b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
			started = 1;
		}
	}
	b[n] = 0;
	g->os.log(g->os.ctx, b);
}

static uint32_t pkt7(uint32_t op, uint32_t cnt)	/* cp_type7_packet() */
{
	uint32_t p1 = cnt ^ cnt >> 4 ^ cnt >> 8 ^ cnt >> 12, p2 = op ^ op >> 4;
	return 7U << 28 | cnt | ((0x9669 >> (p1 & 15)) & 1) << 15 | (op & 0x7f) << 16 |
	       ((0x9669 >> (p2 & 15)) & 1) << 23;
}

static int branch_on(uintptr_t cbcr)
{
	drgpu_wr(cbcr, drgpu_rd(cbcr) | 1);
	return drgpu_poll(cbcr, CBCR_CLK_OFF, 0, 2000) < 0 ? -1 : 0;
}

static int pt_alloc(void *ctx, struct drgpu_page *p)
{
	struct drgpu *g = ctx;
	struct drgpu_mem m;
	if (g->os.alloc(g->os.ctx, 0x1000, 0x1000, &m))
		return -1;
	p->pa = m.pa;
	p->va = m.va;
	return 0;
}

static uint64_t *pt_va(void *ctx, uint64_t pa)
{
	struct drgpu *g = ctx;
	return g->os.ptov(g->os.ctx, pa);
}

static void pt_publish(void *ctx)
{
	(void)ctx;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ---- init ---- */
static int alloc_fw(struct drgpu *g, const uint8_t *src, uint32_t len, struct drgpu_mem *m)
{
	/* _load_firmware: the file without its first dword */
	if (len < 8 || g->os.alloc(g->os.ctx, (len - 4 + 0xfff) & ~0xfffU, 0x1000, m))
		return -1;
	volatile uint8_t *d = m->va;
	for (uint32_t i = 4; i < len; i++)
		d[i - 4] = src[i];
	return 0;
}

int drgpu_init(struct drgpu *g, const struct drgpu_os *os, const struct drgpu_fw *fw)
{
	/* field by field: no memcpy needed (the shim has no C library) */
	g->os.ctx = os->ctx;
	g->os.mmio = os->mmio;
	g->os.alloc = os->alloc;
	g->os.free = os->free;
	g->os.ptov = os->ptov;
	g->os.log = os->log;
	g->os.sleep_us = os->sleep_us;
	g->fw.pm4 = fw->pm4;
	g->fw.pm4_len = fw->pm4_len;
	g->fw.pfp = fw->pfp;
	g->fw.pfp_len = fw->pfp_len;
	g->fw.zap_mdt = fw->zap_mdt;
	g->fw.zap_mdt_len = fw->zap_mdt_len;
	g->fw.zap_b02 = fw->zap_b02;
	g->fw.zap_b02_len = fw->zap_b02_len;
	g->level = -1;
	g->pll = -1;
	g->cx = 128;	/* Linux's APPS CX vote with the GPU on (§6g) */
	g->memacc = 0;	/* read from the hardware below */
	g->gcc = os->mmio(os->ctx, PA_GCC_GPU, 0x1000);
	g->gpucc = os->mmio(os->ctx, PA_GPUCC, 0x9000);
	g->ldo = os->mmio(os->ctx, PA_LDO, 0x1000);
	g->gpu = os->mmio(os->ctx, PA_GPU, 0x40000);
	g->smmu = os->mmio(os->ctx, PA_SMMU, 0x10000);
	g->tcsr = os->mmio(os->ctx, PA_TCSR_ACC, 0x1000);
	g->rpm_ram = os->mmio(os->ctx, PA_RPM_RAM, 0x7000);
	g->rpm_ipc = os->mmio(os->ctx, PA_RPM_IPC, 0x1000);
	if (!g->gcc || !g->gpucc || !g->ldo || !g->gpu || !g->smmu || !g->tcsr || !g->rpm_ram || !g->rpm_ipc)
		return fail(g, "drgpu: mmio map failed");
	g->memacc = (drgpu_rd(MEM_ACC_SEL(g)) & 1) ? 1 : 2;	/* corner-acc-map <1 0> */
	if (os->alloc(os->ctx, 4 * DRGPU_RING_DWORDS, 0x1000, &g->ring) ||
	    os->alloc(os->ctx, DRGPU_MEMSTORE_SIZE, 0x1000, &g->memstore) ||
	    os->alloc(os->ctx, 0x1000, 0x1000, &g->scratch) ||
	    os->alloc(os->ctx, 0x1000, 0x1000, &g->l1) ||
	    alloc_fw(g, fw->pm4, fw->pm4_len, &g->pm4) || alloc_fw(g, fw->pfp, fw->pfp_len, &g->pfp))
		return fail(g, "drgpu: memory");
	g->pt = (struct drgpu_pt){ { g->l1.pa, g->l1.va }, pt_alloc, pt_va, pt_publish, g };
	return 0;
}

/* ---- power (G2a: gdsc-regulator.c gdsc_enable, msm_gfx_ldo.c enable_bhs_mode) ---- */
int drgpu_power_on(struct drgpu *g)
{
	if (branch_on(GCC_GPU_CFG_AHB(g)) || branch_on(GCC_BIMC_GFX(g)) || branch_on(GCC_GPU_BIMC_GFX(g)) ||
	    branch_on(CXO_CBCR(g)))
		return fail(g, "drgpu: gcc/cxo branch did not start");
	if (drgpu_rd(GDSC_CX(g)) & GDSC_HW_CONTROL)
		return fail(g, "drgpu: cx gdsc under hw control");
	drgpu_wr(GDSC_CX(g), drgpu_rd(GDSC_CX(g)) & ~GDSC_SW_COLLAPSE);
	drgpu_udelay(1);
	if (drgpu_poll(GDSC_CX(g), GDSC_PWR_ON, GDSC_PWR_ON, 2000) < 0)
		return fail(g, "drgpu: cx gdsc did not power on");
	uint32_t ctl = drgpu_rd(LDO_PWRSWITCH_CTRL(g));
	ctl |= 1U << 4;		/* LDO under SW control */
	drgpu_wr(LDO_PWRSWITCH_CTRL(g), ctl);
	ctl &= ~(1U << 9);	/* power source BHS */
	drgpu_wr(LDO_PWRSWITCH_CTRL(g), ctl);
	ctl &= ~(1U << 1);	/* BHS_EN_FEW to gdsc */
	drgpu_wr(LDO_PWRSWITCH_CTRL(g), ctl);
	ctl &= ~(1U << 2);	/* BHS_EN_REST to gdsc */
	drgpu_wr(LDO_PWRSWITCH_CTRL(g), ctl);
	ctl &= ~(1U << 0);	/* BHS under GDSC control */
	drgpu_wr(LDO_PWRSWITCH_CTRL(g), ctl);
	/* gfx3d on the crystal: source 0, divider field 1 */
	drgpu_wr(GFX3D_CFG(g), (drgpu_rd(GFX3D_CFG(g)) & ~0x71fU) | 0x001);
	drgpu_wr(GFX3D_CMD(g), drgpu_rd(GFX3D_CMD(g)) | RCG_UPDATE);
	if (drgpu_poll(GFX3D_CMD(g), RCG_UPDATE, 0, 2000) < 0)
		return fail(g, "drgpu: gfx3d rcg update");
	drgpu_wr(GFX3D_CMD(g), (drgpu_rd(GFX3D_CMD(g)) & ~RCG_UPDATE) | RCG_ROOT_EN);
	if (drgpu_rd(GDSC_GX(g)) & GDSC_HW_CONTROL)
		return fail(g, "drgpu: gx gdsc under hw control");
	drgpu_wr(GX_SW_RESET(g), drgpu_rd(GX_SW_RESET(g)) | 1);
	drgpu_udelay(1);
	drgpu_wr(GX_SW_RESET(g), drgpu_rd(GX_SW_RESET(g)) & ~1U);
	drgpu_wr(GX_DOMAIN(g), drgpu_rd(GX_DOMAIN(g)) | (1U << 4));	/* GMEM_RESET */
	drgpu_udelay(1);
	drgpu_wr(GX_DOMAIN(g), drgpu_rd(GX_DOMAIN(g)) & ~(1U << 4));
	drgpu_wr(GX_DOMAIN(g), drgpu_rd(GX_DOMAIN(g)) & ~1U);	/* CLAMP_IO off */
	drgpu_wr(GDSC_GX(g), drgpu_rd(GDSC_GX(g)) & ~GDSC_SW_COLLAPSE);
	drgpu_udelay(1);
	int us = drgpu_poll(GDSC_GX(g), GDSC_PWR_ON, GDSC_PWR_ON, 2000);
	drgpu_udelay(2);
	drgpu_wr(GFX3D_CMD(g), drgpu_rd(GFX3D_CMD(g)) & ~(RCG_ROOT_EN | RCG_UPDATE));
	if (us < 0)
		return fail(g, "drgpu: gx gdsc did not power on");
	if (branch_on(GFX3D_CBCR(g)) || branch_on(RBBMTIMER_CBCR(g)) || branch_on(RBCPR_CBCR(g)))
		return fail(g, "drgpu: gpu branch did not start");
	g->powered = 1;
	g->level = -1;
	g->pll = -1;
	return 0;
}

/* ---- SMMU (G2b: arm-smmu.c device_reset, impl_def_programming, init_context_bank, add_master) ---- */
static const uint32_t impl_defs[][2] = {	/* arm,smmu-kgsl@5040000 attach-impl-defs */
	{ 0x6000, 0x2378 }, { 0x6060, 0x1055 }, { 0x678c, 0x8 }, { 0x6794, 0x28 },
	{ 0x6800, 0x6 }, { 0x6900, 0x3ff }, { 0x6924, 0x204 }, { 0x6928, 0x11000 },
	{ 0x6930, 0x800 }, { 0x6960, 0xffffffff }, { 0x6b64, 0x1a5551 }, { 0x6b68, 0x9a82a382 },
};

int drgpu_tlb_flush(struct drgpu *g)
{
	drgpu_wr(CB0(g, CB_TLBIALL), 0);
	drgpu_wr(CB0(g, CB_TLBSYNC), 0);
	return drgpu_poll(CB0(g, CB_TLBSTATUS), 1, 0, 30000) < 0 ? fail(g, "drgpu: tlb sync") : 0;
}

int drgpu_map(struct drgpu *g, uint64_t gpuaddr, uint64_t pa, uint64_t len, int ro)
{
	return drgpu_pt_map(&g->pt, gpuaddr, pa, len, ro ? DRGPU_PT_RO : DRGPU_PT_RW);
}

void drgpu_unmap(struct drgpu *g, uint64_t gpuaddr, uint64_t len)
{
	drgpu_pt_unmap(&g->pt, gpuaddr, len);
}

int drgpu_smmu_attach(struct drgpu *g)
{
	if (drgpu_map(g, DRGPU_VA_MEMSTORE, g->memstore.pa, DRGPU_MEMSTORE_SIZE, 0) ||
	    drgpu_map(g, DRGPU_VA_SCRATCH, g->scratch.pa, 0x1000, 0) ||
	    drgpu_map(g, DRGPU_VA_RING, g->ring.pa, 4 * DRGPU_RING_DWORDS, 1) ||
	    drgpu_map(g, DRGPU_VA_PM4, g->pm4.pa, g->pm4.size, 1) ||
	    drgpu_map(g, DRGPU_VA_PFP, g->pfp.pa, g->pfp.size, 1))
		return fail(g, "drgpu: global mappings");
	/* device_reset */
	drgpu_wr(SMMU_SGFSR(g), drgpu_rd(SMMU_SGFSR(g)));
	drgpu_wr(SMMU_TLBIALLH(g), 0);
	drgpu_wr(SMMU_TLBIALLNSNH(g), 0);
	drgpu_wr(SMMU_STLBGSYNC(g), 0);
	if (drgpu_poll(SMMU_STLBGSTATUS(g), 1, 0, 30000) < 0)
		return fail(g, "drgpu: smmu global tlb sync");
	uint32_t v = drgpu_rd(SMMU_SCR0(g));
	v |= 0x2 | 0x4 | 0x10 | 0x20 | 0x400 | 0x800 | 0x1000;	/* GFRE GFIE GCFGFRE GCFGFIE USFCFG VMIDPNE PTM */
	v &= ~(0x1U | 0x2000 | 0xc000);				/* CLIENTPD FB BSU */
	drgpu_wr(SMMU_SCR0(g), v);
	/* impl_def_programming: halt, values, resume */
	drgpu_wr(SMMU_MICRO_MMU_CTRL(g), drgpu_rd(SMMU_MICRO_MMU_CTRL(g)) | 0x4);
	(void)drgpu_poll(SMMU_MICRO_MMU_CTRL(g), 0x8, 0x8, 30000);
	for (unsigned i = 0; i < sizeof(impl_defs) / sizeof(impl_defs[0]); i++)
		drgpu_wr(g->smmu + impl_defs[i][0], impl_defs[i][1]);
	drgpu_wr(SMMU_MICRO_MMU_CTRL(g), drgpu_rd(SMMU_MICRO_MMU_CTRL(g)) & ~0x4U);
	/* CB0: S1 with S2 bypass, VMID 2, ASID 1; io-pgtable 39-bit/36-bit, 4 KiB, NC walks */
	drgpu_wr(SMMU_CBA2R0(g), 1);
	drgpu_wr(SMMU_CBAR0(g), 0x0001f302);
	drgpu_wr64(CB0(g, CB_TTBR0), g->l1.pa | 1ULL << 48);
	drgpu_wr64(CB0(g, CB_TTBR1), 1ULL << 48);
	drgpu_wr(CB0(g, CB_TCR), 0x00802019);
	drgpu_wr(CB0(g, CB_TCR2), 0x00038001);
	drgpu_wr(CB0(g, CB_MAIR0), 0x0004ff44);
	drgpu_wr(CB0(g, CB_MAIR1), 0);
	drgpu_wr(CB0(g, CB_ACTLR), 0x70000000);
	drgpu_wr(CB0(g, CB_SCTLR), 0x1167);	/* kgsl ft_pagefault_policy 0: HUPCF */
	drgpu_wr(SMMU_SMR0(g), 0x80000000u);	/* stream 0 */
	drgpu_wr(SMMU_S2CR0(g), 0);		/* translate, CB0 */
	if (drgpu_tlb_flush(g))
		return -1;
	if (drgpu_rd(SMMU_SMR0(g)) != 0x80000000u || drgpu_rd(SMMU_CBAR0(g)) != 0x0001f302 ||
	    drgpu_rd(CB0(g, CB_SCTLR)) != 0x1167 || (drgpu_rd(SMMU_SCR0(g)) & 1))
		return fail(g, "drgpu: smmu readback differs");
	return 0;
}

/* ---- zap shader (G2c: peripheral-loader.c pil_boot, subsys-pil-tz.c, scm.c) ---- */
#define SCM_INTERRUPTED		1
#define SCM_V2_EBUSY		(-12)

static int64_t scm(uint32_t fn, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *x1)
{
	for (unsigned tries = 0;; tries++) {
		register uint64_t r0 __asm__("x0") = fn;
		register uint64_t r1 __asm__("x1") = arginfo;
		register uint64_t r2 __asm__("x2") = a0;
		register uint64_t r3 __asm__("x3") = a1;
		register uint64_t r4 __asm__("x4") = a2;
		register uint64_t r5 __asm__("x5") = 0;
		register uint64_t r6 __asm__("x6") = 0;
		do {
			__asm__ volatile("smc #0"
					 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
					 :
					 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
					   "memory");
		} while (r0 == SCM_INTERRUPTED);
		if ((int64_t)r0 == SCM_V2_EBUSY && tries < 67) {
			drgpu_udelay(30000);
			continue;
		}
		*x1 = r1;
		return (int64_t)r0;
	}
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int zap_load(struct drgpu *g)
{
	const uint8_t *m = g->fw.zap_mdt;
	uint32_t msz = g->fw.zap_mdt_len;
	if (msz < 0x34 || le32(m) != 0x464c457f || m[4] != 1)
		return fail(g, "drgpu: zap mdt is not ELF32");
	uint32_t phoff = le32(m + 0x1c), phnum = m[0x2c] | m[0x2d] << 8;
	if ((m[0x2a] | m[0x2b] << 8) != 32 || phoff + 32 * phnum > msz)
		return fail(g, "drgpu: zap program headers");
	uint32_t min_r = 0xffffffff, max_r = 0, seg_pa = 0, seg_f = 0, seg_m = 0, nload = 0, seg = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = m + phoff + 32 * i;
		uint32_t flags = le32(ph + 24);
		if (!(le32(ph) == 1 && ((flags >> 24) & 7) != 2 && le32(ph + 20)))	/* segment_is_loadable */
			continue;
		if (!(flags & (1U << 27)))
			return fail(g, "drgpu: zap segment not relocatable");
		nload++, seg = i;
		seg_pa = le32(ph + 12), seg_f = le32(ph + 16), seg_m = le32(ph + 20);
		if (seg_pa < min_r)
			min_r = seg_pa;
		if (seg_pa + seg_m > max_r)
			max_r = seg_pa + seg_m;
	}
	if (nload != 1 || seg != 2 || seg_f != g->fw.zap_b02_len || seg_m < seg_f)
		return fail(g, "drgpu: zap layout is not mdt + b02");
	uint32_t size = ((max_r + 0xfff) & ~0xfffU) - min_r;
	struct drgpu_mem meta, region;
	if (g->os.alloc(g->os.ctx, (msz + 0xfff) & ~0xfffU, 0x1000, &meta) ||
	    g->os.alloc(g->os.ctx, size, 0x100000, &region))	/* pil_alloc_region: 1 MiB */
		return fail(g, "drgpu: zap memory");
	volatile uint8_t *d = meta.va;
	for (uint32_t i = 0; i < msz; i++)
		d[i] = m[i];
	__asm__ volatile("dsb sy" ::: "memory");
	uint64_t x1 = 0;
	if (scm(0x42000201, 0x82, 0xd, meta.pa, 0, &x1) || x1)	/* PAS_INIT_IMAGE */
		return fail(g, "drgpu: PAS_INIT_IMAGE");
	if (scm(0x42000202, 0x3, 0xd, region.pa, size, &x1) || x1)	/* PAS_MEM_SETUP */
		return fail(g, "drgpu: PAS_MEM_SETUP");
	d = (volatile uint8_t *)region.va + (seg_pa - min_r);
	for (uint32_t i = 0; i < seg_m; i++)
		d[i] = i < seg_f ? g->fw.zap_b02[i] : 0;
	__asm__ volatile("dsb sy" ::: "memory");
	if (scm(0x42000205, 0x1, 0xd, 0, 0, &x1) || x1)	/* PAS_AUTH_AND_RESET */
		return fail(g, "drgpu: PAS_AUTH_AND_RESET");
	g->os.free(g->os.ctx, &meta);
	/* the region now belongs to TZ: never touched again, never freed */
	logx(g, "drgpu: zap shader loaded by TZ at", region.pa);
	g->zap_loaded = 1;
	return 0;
}

/* ---- ringbuffer ---- */
static uint32_t *ringp(struct drgpu *g) { return (uint32_t *)g->ring.va; }
static volatile uint32_t *ms(struct drgpu *g, uint32_t id, uint32_t field)
{
	return (volatile uint32_t *)((uint8_t *)g->memstore.va + id * DRGPU_MS_ENTRY + field);
}

/*
 * Room for n dwords at ring_w (adreno_ringbuffer_allocspace). Unread data is
 * [rptr, ring_w) circularly; rptr comes from the RPTR shadow the CP writes.
 * Without a wrap the new packets must end before rptr when rptr is ahead.
 * A wrap pads the tail with one NOP once the GPU has left the tail, then
 * needs [0, n) free: the GPU caught up, or past n.
 */
static int ring_space(struct drgpu *g, uint32_t n)
{
	volatile uint32_t *shadow = g->scratch.va;
	uint64_t end = drgpu_ticks() + drgpu_freq() * 2;
	if (g->ring_w + n + 1 > DRGPU_RING_DWORDS) {
		for (;;) {
			uint32_t r = *shadow;
			if (r <= g->ring_w && (r == g->ring_w || r > n))
				break;
			if (drgpu_ticks() > end)
				return fail(g, "drgpu: ring full (wrap)");
		}
		ringp(g)[g->ring_w] = pkt7(CP_NOP, DRGPU_RING_DWORDS - g->ring_w - 1);
		g->ring_w = 0;
		return 0;
	}
	for (;;) {
		uint32_t r = *shadow;
		if (r <= g->ring_w || g->ring_w + n < r)
			return 0;
		if (drgpu_ticks() > end)
			return fail(g, "drgpu: ring full");
	}
}

static void emit(struct drgpu *g, uint32_t v) { ringp(g)[g->ring_w++] = v; }

static void emit_mem_write(struct drgpu *g, uint64_t gpuaddr, uint32_t v)	/* cp_mem_write */
{
	emit(g, pkt7(CP_MEM_WRITE, 3));
	emit(g, (uint32_t)gpuaddr);
	emit(g, (uint32_t)(gpuaddr >> 32));
	emit(g, v);
}

static void kick(struct drgpu *g)
{
	__asm__ volatile("dsb sy" ::: "memory");
	drgpu_wr(R(g, CP_RB_WPTR), g->ring_w);
}

static int ring_wait_idle(struct drgpu *g, uint32_t timeout_us)
{
	if (drgpu_poll(R(g, CP_RB_RPTR), 0xffffffff, g->ring_w, timeout_us) < 0)
		return -1;
	return drgpu_poll(R(g, RBBM_STATUS), 0xfffffffe, 0, timeout_us) < 0 ? -1 : 0;	/* a512 busy_mask */
}

/* ---- CP start (G2b/G2c: a5xx_start, a5xx_rb_start, a5xx_send_me_init, secure mode) ---- */
int drgpu_cp_start(struct drgpu *g)
{
	for (unsigned i = 0; i < sizeof(a5xx_start_seq) / sizeof(a5xx_start_seq[0]); i++)
		drgpu_wr(R(g, a5xx_start_seq[i][0]), a5xx_start_seq[i][1]);
	*(volatile uint32_t *)g->scratch.va = 0;
	g->ring_w = 0;
	drgpu_wr(R(g, CP_RB_RPTR_ADDR_LO), (uint32_t)DRGPU_VA_SCRATCH);
	drgpu_wr(R(g, CP_RB_RPTR_ADDR_HI), (uint32_t)(DRGPU_VA_SCRATCH >> 32));
	drgpu_wr(R(g, CP_RB_CNTL), A5XX_CP_RB_CNTL_DEFAULT);
	drgpu_wr(R(g, CP_RB_BASE), (uint32_t)DRGPU_VA_RING);
	drgpu_wr(R(g, CP_RB_BASE_HI), (uint32_t)(DRGPU_VA_RING >> 32));
	drgpu_wr(R(g, CP_PM4_INSTR_BASE_LO), (uint32_t)DRGPU_VA_PM4);
	drgpu_wr(R(g, CP_PM4_INSTR_BASE_HI), (uint32_t)(DRGPU_VA_PM4 >> 32));
	drgpu_wr(R(g, CP_PFP_INSTR_BASE_LO), (uint32_t)DRGPU_VA_PFP);
	drgpu_wr(R(g, CP_PFP_INSTR_BASE_HI), (uint32_t)(DRGPU_VA_PFP >> 32));
	if (!g->zap_loaded && zap_load(g))	/* once per boot; a512 has CPZ retention */
		return -1;
	drgpu_wr(R(g, CP_ME_CNTL), 0);
	/* ME_INIT, _set_ordinals for a512 */
	emit(g, pkt7(CP_ME_INIT, 8));
	emit(g, 0x2f);
	emit(g, 0x3);
	emit(g, 0x20000000);
	emit(g, 0);
	emit(g, 0);
	emit(g, 0);
	emit(g, 0);
	emit(g, 0);
	kick(g);
	if (drgpu_poll(R(g, CP_RB_RPTR), 0xffffffff, g->ring_w, 2000000) < 0)
		return fail(g, "drgpu: CP did not consume ME_INIT");
	emit(g, pkt7(CP_SET_SECURE_MODE, 1));	/* a5xx_switch_to_unsecure_mode */
	emit(g, 0);
	kick(g);
	if (ring_wait_idle(g, 2000000) || drgpu_rd(R(g, RBBM_SECVID_TRUST_CNTL)) != 0)
		return fail(g, "drgpu: secure mode exit");
	if (drgpu_rd(R(g, CP_HW_FAULT)))
		return fail(g, "drgpu: CP hw fault after start");
	g->cp_running = 1;
	return 0;
}

/* ---- submissions (adreno_ringbuffer_addcmds / submitcmd framing) ---- */
uint32_t drgpu_submit(struct drgpu *g, uint32_t ctx, const struct drgpu_ib *ibs, unsigned n, uint32_t ts)
{
	uint64_t msva = DRGPU_VA_MEMSTORE;
	if (!g->cp_running || ctx == 0 || ctx >= DRGPU_MS_RB)
		return 0;
	if (ring_space(g, 4 * n + 32))
		return 0;
	g->rb_ts++;
	if (ctx != g->cur_ctx) {	/* adreno_drawctxt_switch: current_context */
		emit_mem_write(g, msva + DRGPU_MS_RB * DRGPU_MS_ENTRY + DRGPU_MS_CURCTX, ctx);
		emit_mem_write(g, msva + DRGPU_MS_CURCTX, ctx);
		g->cur_ctx = ctx;
	}
	emit_mem_write(g, msva + ctx * DRGPU_MS_ENTRY + DRGPU_MS_SOP, ts);
	emit_mem_write(g, msva + DRGPU_MS_RB * DRGPU_MS_ENTRY + DRGPU_MS_SOP, g->rb_ts);
	for (unsigned i = 0; i < n; i++) {
		emit(g, pkt7(CP_INDIRECT_BUFFER_PFE, 3));
		emit(g, (uint32_t)ibs[i].gpuaddr);
		emit(g, (uint32_t)(ibs[i].gpuaddr >> 32));
		emit(g, ibs[i].dwords);
	}
	emit(g, pkt7(CP_EVENT_WRITE, 4));	/* end of pipe: per-context timestamp, interrupt */
	emit(g, CACHE_FLUSH_TS | 1U << 31);
	emit(g, (uint32_t)(msva + ctx * DRGPU_MS_ENTRY + DRGPU_MS_EOP));
	emit(g, (uint32_t)((msva + ctx * DRGPU_MS_ENTRY + DRGPU_MS_EOP) >> 32));
	emit(g, ts);
	emit_mem_write(g, msva + DRGPU_MS_RB * DRGPU_MS_ENTRY + DRGPU_MS_EOP, g->rb_ts);
	kick(g);
	return ts;
}

uint32_t drgpu_retired(struct drgpu *g, uint32_t ctx)
{
	return *ms(g, ctx, DRGPU_MS_EOP);
}

int drgpu_wait(struct drgpu *g, uint32_t ctx, uint32_t ts, uint32_t timeout_us)
{
	uint64_t end = drgpu_ticks() + drgpu_freq() / 1000000 * timeout_us;
	while ((int32_t)(drgpu_retired(g, ctx) - ts) < 0) {	/* KGSL_TIMESTAMP_WINDOW compare */
		if (drgpu_ticks() > end)
			return -1;
	}
	return 0;
}

uint32_t drgpu_irq(struct drgpu *g)
{
	uint32_t st = drgpu_rd(R(g, RBBM_INT_0_STATUS));
	if (st)
		drgpu_wr(R(g, RBBM_INT_CLEAR_CMD), st);
	return st;
}

/* ---- levels (G2e: gpucc ping-pong, clk-alpha-pll.c, mem-acc, RPM votes) ---- */
static const struct level {
	uint32_t khz, l, alpha, alpha_u, postdiv, cx, memacc;
	uint64_t bw;
} levels[DRGPU_LEVELS] = {
	{ 430000, 0x2c, 0xaaaaab00, 0xca, 0, 192, 2, 1592663040ULL },
	{ 370000, 0x26, 0xaaaaab00, 0x8a, 0, 128, 1, 1007557632ULL },
	{ 266000, 0x1b, 0x55555600, 0xb5, 0, 128, 1, 439229440ULL },
	{ 160000, 0x21, 0x55555600, 0x55, 1, 128, 1, 439229440ULL },
};

static int set_bw(struct drgpu *g, uint64_t bw)
{
	if (bw == g->bw)
		return 0;
	if (drgpu_rpm_kvp64(g, RPM_RES_BSLV, 0, RPM_KEY_BW, bw))
		return fail(g, "drgpu: rpm bus vote");
	g->bw = bw;
	return 0;
}

static int set_cx(struct drgpu *g, uint32_t lvl)
{
	if (lvl == g->cx)
		return 0;
	if (drgpu_rpm_kvp2(g, RPM_RES_RWCX, 0, RPM_KEY_SWEN, 1, RPM_KEY_VLVL, lvl))
		return fail(g, "drgpu: rpm cx vote");
	g->cx = lvl;
	return 0;
}

static void set_memacc(struct drgpu *g, uint32_t corner)
{
	while (g->memacc != corner) {	/* one corner at a time, read-modify-write of bit 0 */
		g->memacc += corner > g->memacc ? 1 : -1;
		drgpu_wr(MEM_ACC_SEL(g), (drgpu_rd(MEM_ACC_SEL(g)) & ~1U) | (g->memacc == 1 ? 1U : 0U));
	}
}

static int pll_on(struct drgpu *g, unsigned n, const struct level *lv)
{
	uintptr_t p = PLL(g, n);
	if (drgpu_rd(p) & (PLL_VOTE_FSM_ENA | PLL_OUTCTRL | PLL_RESET_N | PLL_BYPASSNL))
		return fail(g, "drgpu: pll busy");
	drgpu_wr(p + 0x18, GPU_PLL_CONFIG_CTL);
	drgpu_wr(p + 0x10, (drgpu_rd(p + 0x10) & ~(PLL_VCO_MASK | PLL_POSTDIV_MASK)) | PLL_VCO_500_1000 | lv->postdiv << 8);
	drgpu_wr(p + 0x04, lv->l);
	drgpu_wr(p + 0x08, lv->alpha);
	drgpu_wr(p + 0x0c, lv->alpha_u);
	drgpu_wr(p, drgpu_rd(p) | PLL_BYPASSNL);
	drgpu_udelay(5);
	drgpu_wr(p, drgpu_rd(p) | PLL_RESET_N);
	if (drgpu_poll(p, PLL_LOCK_DET, PLL_LOCK_DET, 1000) < 0) {
		drgpu_wr(p, drgpu_rd(p) & ~(PLL_RESET_N | PLL_BYPASSNL));
		return fail(g, "drgpu: pll did not lock");
	}
	drgpu_wr(p, drgpu_rd(p) | PLL_OUTCTRL);
	return 0;
}

static void pll_off(struct drgpu *g, unsigned n)
{
	uintptr_t p = PLL(g, n);
	drgpu_wr(p, drgpu_rd(p) & ~PLL_OUTCTRL);
	drgpu_wr(p, drgpu_rd(p) & ~(PLL_RESET_N | PLL_BYPASSNL));
}

int drgpu_set_level(struct drgpu *g, int lvl)
{
	if (lvl < 0 || lvl >= DRGPU_LEVELS || !g->powered)
		return fail(g, "drgpu: bad level");
	if (lvl == g->level)
		return 0;
	const struct level *lv = &levels[lvl];
	uint32_t cur_khz = g->level < 0 ? 19200 : levels[g->level].khz;
	int up = lv->khz > cur_khz;
	unsigned n = g->pll < 0 ? 0 : (unsigned)(g->pll ^ 1);	/* from XO the first PLL is PLL0 */
	if (g->level < 0) {	/* first PLL use: MX as Linux votes it while a GPU PLL runs */
		if (drgpu_rpm_kvp2(g, RPM_RES_RWMX, 0, RPM_KEY_SWEN, 1, RPM_KEY_VLVL, 192))
			return fail(g, "drgpu: rpm mx vote");
	}
	if (up) {	/* voltage, then mem-acc (the level's own corner, as Linux), and bus first */
		if (set_bw(g, lv->bw) || set_cx(g, lv->cx > g->cx ? lv->cx : g->cx))
			return -1;
		set_memacc(g, lv->memacc);
	}
	if (pll_on(g, n, lv))
		return -1;
	drgpu_wr(GFX3D_CFG(g), (n ? 3U : 1U) << 8 | 3);	/* PLL1 source 3, PLL0 source 1; div 2 */
	drgpu_wr(GFX3D_CMD(g), drgpu_rd(GFX3D_CMD(g)) | RCG_UPDATE);
	if (drgpu_poll(GFX3D_CMD(g), RCG_UPDATE, 0, 2000) < 0) {
		pll_off(g, n);
		return fail(g, "drgpu: gfx3d rcg update");
	}
	if (g->pll >= 0)
		pll_off(g, (unsigned)g->pll);
	g->pll = (int)n;
	g->level = lvl;
	if (!up) {	/* down: only after the clock came down */
		set_memacc(g, lv->memacc);
		if (set_cx(g, lv->cx) || set_bw(g, lv->bw))
			return -1;
	}
	logx(g, "drgpu: gpu level kHz", lv->khz);
	return 0;
}

/*
 * Idle power-off, Linux's slumber on this phone (ftrace 30.3119-30.3124):
 * interrupt mask 0, VBIF halt and drain (XIN_HALT_CTRL0 0xf, wait for
 * CTRL1, 0), bus vote down to the non-GPU APPS aggregate, rbcpr /
 * rbbmtimer / gfx3d branches off, PLL off, MX vote vlvl 16 and swen 0,
 * gpu_bimc_gfx off, GX GDSC collapse with the IO clamp (gdsc_disable with
 * force-root). CX stays on: the SMMU in it keeps its configuration.
 */
#define RBBM_INT_0_MASK		0x38
#define VBIF_XIN_HALT_CTRL0	0x3080
#define VBIF_XIN_HALT_CTRL1	0x3081
#define BW_GPU_OFF		240000000ULL	/* §6g: the APPS aggregate without the GPU's share */

int drgpu_power_off(struct drgpu *g)
{
	if (!g->powered)
		return 0;
	if (g->cp_running && ring_wait_idle(g, 100000))
		return fail(g, "drgpu: not idle at power off");
	drgpu_wr(R(g, RBBM_INT_0_MASK), 0);
	drgpu_wr(R(g, VBIF_XIN_HALT_CTRL0), 0xf);
	(void)drgpu_poll(R(g, VBIF_XIN_HALT_CTRL1), 0xf, 0xf, 100);
	drgpu_wr(R(g, VBIF_XIN_HALT_CTRL0), 0);
	g->cp_running = 0;
	if (set_bw(g, BW_GPU_OFF))
		return -1;
	drgpu_wr(RBCPR_CBCR(g), drgpu_rd(RBCPR_CBCR(g)) & ~1U);
	drgpu_wr(RBBMTIMER_CBCR(g), drgpu_rd(RBBMTIMER_CBCR(g)) & ~1U);
	drgpu_wr(GFX3D_CBCR(g), drgpu_rd(GFX3D_CBCR(g)) & ~1U);
	if (g->pll >= 0)
		pll_off(g, (unsigned)g->pll);
	if (g->level >= 0 && drgpu_rpm_kvp2(g, RPM_RES_RWMX, 0, RPM_KEY_VLVL, 16, RPM_KEY_SWEN, 0))
		return fail(g, "drgpu: rpm mx vote off");
	if (set_cx(g, 128))
		return -1;
	drgpu_wr(GCC_GPU_BIMC_GFX(g), drgpu_rd(GCC_GPU_BIMC_GFX(g)) & ~1U);
	drgpu_wr(GFX3D_CMD(g), drgpu_rd(GFX3D_CMD(g)) | RCG_ROOT_EN);
	drgpu_wr(GDSC_GX(g), drgpu_rd(GDSC_GX(g)) | GDSC_SW_COLLAPSE);
	int us = drgpu_poll(GDSC_GX(g), GDSC_PWR_ON, 0, 2000);
	drgpu_wr(GX_DOMAIN(g), drgpu_rd(GX_DOMAIN(g)) | 1);	/* CLAMP_IO */
	drgpu_wr(GFX3D_CMD(g), drgpu_rd(GFX3D_CMD(g)) & ~(RCG_ROOT_EN | RCG_UPDATE));
	g->powered = 0;
	g->level = -1;
	g->pll = -1;
	g->cur_ctx = 0;
	return us < 0 ? fail(g, "drgpu: gx gdsc did not collapse") : 0;
}

int drgpu_resume(struct drgpu *g, int level)
{
	if (drgpu_power_on(g) || drgpu_cp_start(g))
		return -1;
	return drgpu_set_level(g, level);
}

/* GPU clock in kHz: RBBM perf counter 1, countable 0, clock gating off (a5xx_hwcg_set) */
static unsigned hwcg_end(void)
{
	for (unsigned i = 1; i < sizeof(a5xx_start_seq) / sizeof(a5xx_start_seq[0]); i++)
		if (a5xx_start_seq[i][0] == RBBM_CLOCK_CNTL && a5xx_start_seq[i][1] == 0xaaa82a00)
			return i + 1;
	return 0;
}

static uint64_t cycles(struct drgpu *g)
{
	uint32_t hi, lo;
	do {
		hi = drgpu_rd(R(g, RBBM_PERFCTR_RBBM_1_HI));
		lo = drgpu_rd(R(g, RBBM_PERFCTR_RBBM_1_LO));
	} while (hi != drgpu_rd(R(g, RBBM_PERFCTR_RBBM_1_HI)));
	return (uint64_t)hi << 32 | lo;
}

uint32_t drgpu_gpu_khz(struct drgpu *g)
{
	unsigned end = hwcg_end();
	if (!end)
		return 0;
	for (unsigned i = 1; i < end; i++)
		if (a5xx_start_seq[i][0] != RBBM_CLOCK_CNTL && a5xx_start_seq[i][0] != RBBM_ISDB_CNT)
			drgpu_wr(R(g, a5xx_start_seq[i][0]), 0);
	drgpu_wr(R(g, RBBM_CLOCK_CNTL), 0);
	drgpu_wr(R(g, RBBM_ISDB_CNT), 0x180);
	drgpu_wr(R(g, RBBM_PERFCTR_RBBM_SEL_1), 0);
	drgpu_udelay(10);
	uint64_t t0 = drgpu_ticks(), c0 = cycles(g);
	drgpu_udelay(2000);
	uint64_t t1 = drgpu_ticks(), c1 = cycles(g);
	for (unsigned i = 1; i < end; i++)
		drgpu_wr(R(g, a5xx_start_seq[i][0]), a5xx_start_seq[i][1]);
	uint64_t us = (t1 - t0) * 1000000 / drgpu_freq();
	return us ? (uint32_t)((c1 - c0) * 1000 / us) : 0;
}

const char *drgpu_err(const struct drgpu *g)
{
	return g->errmsg ? g->errmsg : "";
}
