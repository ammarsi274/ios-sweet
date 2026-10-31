/*
 * DRG (Nokia 6.1 Plus) GPU bring-up, steps G1 and G2a of drg/GPU-PLAN.md.
 *
 * G1: read-only snapshot of the GPU's clocks, power domains and GFX power
 *     switch as ABL leaves them.
 * G2a: power the Adreno 512 the way HMD's kernel does it on this phone
 *     (measured with ftrace under TWRP, drg/gpu-research/TWRP-MEASURED.md;
 *     sources: HMD gdsc-regulator.c gdsc_enable(), msm_gfx_ldo.c
 *     enable_bhs_mode(), gpucc-sdm660.c, gcc-sdm660.c), but with the core
 *     clock on the 19.2 MHz crystal: no PLL and no voltage corner change,
 *     the safest clock for a first power-up. Then read GPU registers.
 *
 * GPU registers are read only after both power domains report PWR_ON and
 * the clock branches report running: a read of an unpowered block hangs
 * the bus. Every step's result is written to the eMMC log (emmc.c) before
 * the next step, so a hang still leaves the last finished step on disk.
 *
 * G2b: start the command processor without the secure world. HMD's kernel
 *     sends CP_ME_INIT while the GPU is still in secure mode and only then
 *     uses the zap shader to leave it (adreno_a5xx.c a5xx_rb_start), so the
 *     CP can be tested first: replay the measured
 *     a5xx start sequence, point the CP at Nokia's microcode, send ME_INIT
 *     and a CP_MEM_WRITE.
 *     Measured 2026-10-30: the first read of the SMMU's global register space
 *     (0x5040000) hangs the CPU for good (supply gdsc_gpu_cx and its three
 *     clocks on), so it is not read. The CP test now runs with physical
 *     addresses: the GPU's memory write landing proves stream 0 untranslated.
 *     Context bank 0 (0x5048000, where kgsl writes) is read last, on its own.
 *
 * Addresses from this phone's device tree (device-info/fdt.dts):
 *   GCC 0x100000, GPUCC clock-controller@5065000, gdsc_gpu_cx 0x5066004
 *   (hw-ctrl 0x5066008), gdsc_gpu_gx 0x5066094 (domain-addr 0x5065130,
 *   sw-reset 0x5066090), msm_gfx_ldo 0x506e000, kgsl-3d0 0x5000000,
 *   arm,smmu-kgsl@5040000 (size 0x10000, gfx3d_user stream 0, gfx3d_secure
 *   stream 2; kgsl-iommu qcom,gpu-offset 0x48000 = its context banks).
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"
#include "drg_gpu.h"
#include "drg_rpm.h"
#include "drgpu_pt.h"
#include "drgpu.h"
#include "drgpu_fwblob.h"

#define GCC			0x100000UL
#define GCC_GPU_CFG_AHB_CBCR	(GCC + 0x71004)
#define GCC_GPU_BIMC_GFX_CBCR	(GCC + 0x71010)
#define GCC_BIMC_GFX_CBCR	(GCC + 0x7106c)

#define GPUCC			0x5065000UL
#define GPUCC_PLL0		(GPUCC + 0x0)
#define GPUCC_RBCPR_CMD		(GPUCC + 0x1030)
#define GPUCC_CXO_CBCR		(GPUCC + 0x1020)
#define GPUCC_RBCPR_CBCR	(GPUCC + 0x1054)
#define GPUCC_GFX3D_CMD		(GPUCC + 0x1070)
#define GPUCC_GFX3D_CFG		(GPUCC + 0x1074)
#define GPUCC_GFX3D_CBCR	(GPUCC + 0x1098)
#define GPUCC_RBBMTIMER_CMD	(GPUCC + 0x10b0)
#define GPUCC_RBBMTIMER_CBCR	(GPUCC + 0x10d0)
#define GPU_GX_DOMAIN		(GPUCC + 0x130)

#define GDSC_CX			0x5066004UL
#define GDSC_CX_HWCTRL		0x5066008UL
#define GX_SW_RESET		0x5066090UL
#define GDSC_GX			0x5066094UL

#define GFX_LDO			0x506e000UL
#define LDO_PWRSWITCH_CTRL	(GFX_LDO + 0x1c)
#define LDO_PWRSWITCH_STATUS	(GFX_LDO + 0x28)

#define GPU			0x5000000UL
#define GPU_REG(r)		(GPU + 4UL * (r))
/* a5xx_reg.h (HMD) */
#define A5XX_CP_RB_BASE		0x800
#define A5XX_CP_RB_BASE_HI	0x801
#define A5XX_CP_RB_CNTL		0x802
#define A5XX_CP_RB_RPTR_ADDR_LO	0x804
#define A5XX_CP_RB_RPTR_ADDR_HI	0x805
#define A5XX_CP_RB_RPTR		0x806
#define A5XX_CP_RB_WPTR		0x807
#define A5XX_CP_ME_CNTL		0x832
#define A5XX_CP_PFP_INSTR_BASE_LO	0x835
#define A5XX_CP_PFP_INSTR_BASE_HI	0x836
#define A5XX_CP_PM4_INSTR_BASE_LO	0x838
#define A5XX_CP_PM4_INSTR_BASE_HI	0x839
#define A5XX_CP_INTERRUPT_STATUS	0xb18
#define A5XX_CP_HW_FAULT	0xb1a
#define A5XX_CP_PROTECT_STATUS	0xb1c
#define A5XX_RBBM_INT_0_STATUS	0x4e1
#define A5XX_RBBM_INT_0_MASK	0x38
#define A5XX_RBBM_AHB_ERROR_STATUS	0x4f4
#define A5XX_RBBM_STATUS	0x4f5
#define A5XX_RBBM_STATUS3	0x530
#define A5XX_VBIF_VERSION	0x3000
#define A5XX_RBBM_SECVID_TRUST_CNTL	0xf400

/* adreno_pm4types.h (HMD) */
#define CP_TYPE7_PKT		(7U << 28)
#define CP_NOP			0x10
#define CP_MEM_WRITE		0x3d
#define CP_ME_INIT		0x48
#define CP_SET_SECURE_MODE	0x66
/* a5xx_rb_start(): A5XX_CP_RB_CNTL_DEFAULT, measured 0x20c */
#define A5XX_CP_RB_CNTL_DEFAULT	0x20c
#define RB_DWORDS		8192	/* 2 * 2^(0x20c & 0x3f) */

#define SMMU			0x5040000UL
#define SMMU_CB_DT		(SMMU + 0x8000)	/* 0x5000000 + qcom,gpu-offset */
#define SMMU_CB_SCTLR		0x00
#define SMMU_CB_TTBR0		0x20
#define SMMU_CB_FSR		0x58
#define SMMU_CB_FAR		0x60

#define CBCR_CLK_OFF		(1U << 31)
#define CBCR_CLK_ENABLE		(1U << 0)
#define RCG_UPDATE		(1U << 0)
#define RCG_ROOT_EN		(1U << 1)
#define GDSC_PWR_ON		(1U << 31)
#define GDSC_SW_COLLAPSE	(1U << 0)
#define GDSC_HW_CONTROL		(1U << 1)

#include "drgpu_start.h"

extern const uint32_t drg_pm4_fw[], drg_pm4_fw_end[], drg_pfp_fw[], drg_pfp_fw_end[];

/*
 * MMIO with a plain [Xn] address only, like Linux's __raw_readl/writel.
 * Measured (G2b runs 1, 5, 7): the GPU SMMU's global registers are trapped
 * and emulated by the hypervisor, and an access the compiler emits with
 * base writeback (str w3, [x2], #36) carries no decodable syndrome, so the
 * core never comes back. Plain ldr/str to the same registers complete.
 * Run 8: a store of the zero register (str wzr, [x0]) to TLBIALLH also
 * never came back, while the same write from w1 completed in run 6, so the
 * value always goes through a real register ("r", as Linux 4.4's writel).
 */
static inline uint32_t rd(uintptr_t a)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}

static inline void wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]\n\tdsb sy" : : "r"(v), "r"(a) : "memory");
}

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static uint64_t freq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static void udelay(unsigned us)
{
	uint64_t end = ticks() + freq() / 1000000 * us + 1;
	while (ticks() < end)
		;
}

/* poll until (rd(a) & mask) == want; returns microseconds taken or -1 */
static int poll(uintptr_t a, uint32_t mask, uint32_t want, unsigned timeout_us)
{
	uint64_t t0 = ticks(), end = t0 + freq() / 1000000 * timeout_us;
	while ((rd(a) & mask) != want)
		if (ticks() > end)
			return -1;
	return (int)((ticks() - t0) * 1000000 / freq());
}

/* ---- text log, flushed to eMMC after every step ---- */
static char logbuf[64 * 512];
static unsigned loglen;

static void put(const char *s)
{
	while (*s && loglen < sizeof(logbuf) - 1)
		logbuf[loglen++] = *s++;
}

static void puthex(uint32_t v)
{
	char b[11];
	b[0] = '0', b[1] = 'x';
	for (int i = 0; i < 8; i++) {
		unsigned d = (v >> (28 - 4 * i)) & 15;
		b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
	}
	b[10] = 0;
	put(b);
}

static void lv(const char *label, uint32_t v)
{
	put(label);
	put(" ");
	puthex(v);
	put("\n");
	con_hex2(label, "=", v, "", 0);
}

static void lreg(const char *label, uintptr_t a)
{
	put(label);
	put(" @");
	puthex((uint32_t)a);
	put(" = ");
	puthex(rd(a));
	put("\n");
}

static int flush(void)
{
	return drg_emmc_log_write(logbuf, loglen);
}

static void step(const char *name)
{
	put("== ");
	put(name);
	put("\n");
	con_str("GPU", name);
	flush();
}

/* ---- G1: read only ---- */
static void g1_snapshot(const char *when)
{
	step(when);
	lreg("gcc gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	lreg("gcc gpu_bimc_gfx", GCC_GPU_BIMC_GFX_CBCR);
	lreg("gcc bimc_gfx", GCC_BIMC_GFX_CBCR);
	/* GDSCs and the GFX LDO sit in the GPU clock controller's block:
	 * only read with its AHB clock running */
	if (rd(GCC_GPU_CFG_AHB_CBCR) & CBCR_CLK_OFF) {
		put("gpucc not read: gpu_cfg_ahb clock off\n");
		flush();
		return;
	}
	lreg("gdsc cx", GDSC_CX);
	lreg("gdsc cx hwctrl", GDSC_CX_HWCTRL);
	lreg("gdsc gx", GDSC_GX);
	lreg("gx sw_reset", GX_SW_RESET);
	lreg("ldo pwrswitch ctrl", LDO_PWRSWITCH_CTRL);
	lreg("ldo pwrswitch status", LDO_PWRSWITCH_STATUS);
	for (unsigned o = 0; o <= 0x34; o += 4)
		lreg("ldo", GFX_LDO + o);
	for (unsigned o = 0; o <= 0x1c; o += 4)
		lreg("pll0", GPUCC_PLL0 + o);
	lreg("gfx3d cmd", GPUCC_GFX3D_CMD);
	lreg("gfx3d cfg", GPUCC_GFX3D_CFG);
	lreg("rbbmtimer cmd", GPUCC_RBBMTIMER_CMD);
	lreg("rbcpr cmd", GPUCC_RBCPR_CMD);
	lreg("cxo cbcr", GPUCC_CXO_CBCR);
	lreg("gfx3d cbcr", GPUCC_GFX3D_CBCR);
	lreg("rbbmtimer cbcr", GPUCC_RBBMTIMER_CBCR);
	lreg("rbcpr cbcr", GPUCC_RBCPR_CBCR);
	lreg("gx domain", GPU_GX_DOMAIN);
	flush();
}

static int branch_on(const char *label, uintptr_t cbcr)
{
	wr(cbcr, rd(cbcr) | CBCR_CLK_ENABLE);
	int us = poll(cbcr, CBCR_CLK_OFF, 0, 2000);
	put(label);
	put(us < 0 ? " on: TIMEOUT " : " on: ok ");
	puthex(rd(cbcr));
	put("\n");
	return us < 0 ? -1 : 0;
}

/* ---- G2a ---- */
static __attribute__((unused)) int g2a_power(void)
{
	int us;

	step("G2a 1 gcc clocks");
	if (branch_on("gcc gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR) ||
	    branch_on("gcc bimc_gfx", GCC_BIMC_GFX_CBCR) ||
	    branch_on("gcc gpu_bimc_gfx", GCC_GPU_BIMC_GFX_CBCR))
		return -1;
	if (branch_on("gpucc cxo", GPUCC_CXO_CBCR))
		return -1;
	flush();

	step("G2a 2 gdsc cx");
	if (rd(GDSC_CX) & GDSC_HW_CONTROL) {
		put("gdsc cx under hw control: stop\n");
		return -1;
	}
	wr(GDSC_CX, rd(GDSC_CX) & ~GDSC_SW_COLLAPSE);
	udelay(1);
	us = poll(GDSC_CX, GDSC_PWR_ON, GDSC_PWR_ON, 2000);
	lv("gdsc cx us", (uint32_t)us);
	lreg("gdsc cx", GDSC_CX);
	if (us < 0)
		return -1;
	flush();

	/* msm_gfx_ldo.c enable_bhs_mode(): the GFX rail from CX through the
	 * BHS switch, handed to the GX GDSC (LDO mode is off for every corner
	 * in this phone's DT) */
	step("G2a 3 gfx bhs");
	uint32_t ctl = rd(LDO_PWRSWITCH_CTRL);
	ctl |= 1U << 4;		/* LDO under SW control */
	wr(LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~(1U << 9);	/* power source: BHS */
	wr(LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~(1U << 1);	/* BHS_EN_FEW to gdsc */
	wr(LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~(1U << 2);	/* BHS_EN_REST to gdsc */
	wr(LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~(1U << 0);	/* BHS under GDSC control */
	wr(LDO_PWRSWITCH_CTRL, ctl);
	lreg("ldo pwrswitch ctrl", LDO_PWRSWITCH_CTRL);
	lreg("ldo pwrswitch status", LDO_PWRSWITCH_STATUS);
	flush();

	/* gfx3d on the crystal (gpucc-sdm660.c: F(19200000, P_XO, 1, 0, 0):
	 * source 0, divider field 2*1-1 = 1) */
	step("G2a 4 gfx3d rcg xo");
	wr(GPUCC_GFX3D_CFG, (rd(GPUCC_GFX3D_CFG) & ~0x71fU) | 0x001);
	wr(GPUCC_GFX3D_CMD, (rd(GPUCC_GFX3D_CMD) & ~RCG_UPDATE) | RCG_UPDATE);
	us = poll(GPUCC_GFX3D_CMD, RCG_UPDATE, 0, 2000);
	lv("gfx3d update us", (uint32_t)us);
	lreg("gfx3d cfg", GPUCC_GFX3D_CFG);
	if (us < 0)
		return -1;
	/* force-enable-root-clk while the GX domain powers up (gdsc_enable) */
	wr(GPUCC_GFX3D_CMD, (rd(GPUCC_GFX3D_CMD) & ~RCG_UPDATE) | RCG_ROOT_EN);
	lreg("gfx3d cmd", GPUCC_GFX3D_CMD);
	flush();

	/* gdsc_enable() with sw-reset and domain-addr (reset-aon-logic) */
	step("G2a 5 gdsc gx");
	if (rd(GDSC_GX) & GDSC_HW_CONTROL) {
		put("gdsc gx under hw control: stop\n");
		return -1;
	}
	wr(GX_SW_RESET, rd(GX_SW_RESET) | 1);
	udelay(1);
	wr(GX_SW_RESET, rd(GX_SW_RESET) & ~1U);
	wr(GPU_GX_DOMAIN, rd(GPU_GX_DOMAIN) | (1U << 4));	/* GMEM_RESET */
	udelay(1);
	wr(GPU_GX_DOMAIN, rd(GPU_GX_DOMAIN) & ~(1U << 4));
	wr(GPU_GX_DOMAIN, rd(GPU_GX_DOMAIN) & ~1U);	/* CLAMP_IO off */
	lreg("gx domain", GPU_GX_DOMAIN);
	wr(GDSC_GX, rd(GDSC_GX) & ~GDSC_SW_COLLAPSE);
	udelay(1);
	us = poll(GDSC_GX, GDSC_PWR_ON, GDSC_PWR_ON, 2000);	/* gds-timeout 2000 us */
	lv("gdsc gx us", (uint32_t)us);
	lreg("gdsc gx", GDSC_GX);
	lreg("ldo pwrswitch status", LDO_PWRSWITCH_STATUS);
	udelay(2);
	wr(GPUCC_GFX3D_CMD, rd(GPUCC_GFX3D_CMD) & ~(RCG_ROOT_EN | RCG_UPDATE));
	if (us < 0)
		return -1;
	flush();

	step("G2a 6 gpu clocks");
	if (branch_on("gfx3d", GPUCC_GFX3D_CBCR) || branch_on("rbbmtimer", GPUCC_RBBMTIMER_CBCR) ||
	    branch_on("rbcpr", GPUCC_RBCPR_CBCR))
		return -1;
	flush();
	return 0;
}

static __attribute__((unused)) void g2a_registers(void)
{
	step("G2a 7 gpu registers");
	lreg("rbbm status", GPU_REG(A5XX_RBBM_STATUS));
	lreg("rbbm status3", GPU_REG(A5XX_RBBM_STATUS3));
	lreg("rbbm int0 status", GPU_REG(A5XX_RBBM_INT_0_STATUS));
	lreg("cp me cntl", GPU_REG(A5XX_CP_ME_CNTL));
	lreg("cp rb rptr", GPU_REG(A5XX_CP_RB_RPTR));
	flush();
	lreg("vbif version", GPU_REG(A5XX_VBIF_VERSION));
	flush();
	/* write/read back a register the stopped CP does not use yet */
	uint32_t old = rd(GPU_REG(A5XX_CP_RB_RPTR_ADDR_LO));
	wr(GPU_REG(A5XX_CP_RB_RPTR_ADDR_LO), 0x12345000);
	uint32_t got = rd(GPU_REG(A5XX_CP_RB_RPTR_ADDR_LO));
	wr(GPU_REG(A5XX_CP_RB_RPTR_ADDR_LO), old);
	lv("rptr_addr old", old);
	lv("rptr_addr wrote 0x12345000 read", got);
	put(got == 0x12345000 ? "G2a RESULT: PASS (GPU powered, registers answer)\n" : "G2a RESULT: FAIL (readback)\n");
	con_str("G2a", got == 0x12345000 ? "PASS" : "FAIL readback");
	flush();
}

/* ---- G2b ---- */

/* GPU-visible buffers: MMU off, so these addresses are physical and the
 * memory is Device (no cache maintenance between CPU and GPU). */
static uint32_t ring[RB_DWORDS] __attribute__((aligned(4096)));
static uint32_t rptr_shadow[1024] __attribute__((aligned(4096)));
static uint32_t scratch[1024] __attribute__((aligned(4096)));

static uint32_t parity(uint32_t v)	/* pm4_calc_odd_parity_bit() */
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^
		(v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t pkt7(uint32_t op, uint32_t cnt)	/* cp_type7_packet() */
{
	return CP_TYPE7_PKT | cnt | parity(cnt) << 15 | (op & 0x7f) << 16 | parity(op) << 23;
}

static void cp_state(const char *when)
{
	put("-- cp ");
	put(when);
	put("\n");
	lreg("rbbm status", GPU_REG(A5XX_RBBM_STATUS));
	lreg("rbbm int0 status", GPU_REG(A5XX_RBBM_INT_0_STATUS));
	lreg("rbbm ahb error", GPU_REG(A5XX_RBBM_AHB_ERROR_STATUS));
	lreg("cp me cntl", GPU_REG(A5XX_CP_ME_CNTL));
	lreg("cp rb rptr", GPU_REG(A5XX_CP_RB_RPTR));
	lreg("cp rb wptr", GPU_REG(A5XX_CP_RB_WPTR));
	lreg("cp hw fault", GPU_REG(A5XX_CP_HW_FAULT));
	lreg("cp int status", GPU_REG(A5XX_CP_INTERRUPT_STATUS));
	lreg("cp protect status", GPU_REG(A5XX_CP_PROTECT_STATUS));
	lv("rptr shadow", rptr_shadow[0]);
	flush();
}

/* hand the CP everything up to wptr; microseconds until it read it, or -1 */
static int rb_submit(uint32_t wptr, unsigned timeout_us)
{
	wr(GPU_REG(A5XX_CP_RB_WPTR), wptr);
	return poll(GPU_REG(A5XX_CP_RB_RPTR), 0xffffffff, wptr, timeout_us);
}

static int pr(const char *name, uintptr_t a, uint32_t *v);
static int zap_load(void);
static unsigned ring_w;
static int cp_ok;

static __attribute__((unused)) void g2b_cp(void)
{
	step("G2b 2 a5xx start sequence");
	for (unsigned i = 0; i < sizeof(a5xx_start_seq) / sizeof(a5xx_start_seq[0]); i++)
		wr(GPU_REG(a5xx_start_seq[i][0]), a5xx_start_seq[i][1]);
	lv("writes", sizeof(a5xx_start_seq) / sizeof(a5xx_start_seq[0]));
	cp_state("after start sequence");

	step("G2b 3 ringbuffer and microcode");
	uintptr_t rb = (uintptr_t)ring, sh = (uintptr_t)rptr_shadow, sc = (uintptr_t)scratch;
	uintptr_t pm4 = (uintptr_t)drg_pm4_fw, pfp = (uintptr_t)drg_pfp_fw;
	lv("ring pa", (uint32_t)rb);
	lv("rptr shadow pa", (uint32_t)sh);
	lv("scratch pa", (uint32_t)sc);
	lv("pm4 pa", (uint32_t)pm4);
	lv("pm4 dwords", (uint32_t)(drg_pm4_fw_end - drg_pm4_fw));
	lv("pm4 version", drg_pm4_fw[0]);
	lv("pfp pa", (uint32_t)pfp);
	lv("pfp dwords", (uint32_t)(drg_pfp_fw_end - drg_pfp_fw));
	lv("pfp version", drg_pfp_fw[0]);
	if ((rb | sh | sc | pm4 | pfp) >> 32) {
		put("buffer above 4 GiB: stop\n");
		flush();
		return;
	}
	rptr_shadow[0] = 0xdeadbeef;
	scratch[0] = 0;
	/* ring contents first, then the registers (a5xx_rb_start order) */
	unsigned w = 0;
	ring[w++] = pkt7(CP_ME_INIT, 8);	/* a5xx_send_me_init(), _set_ordinals() for a512 */
	ring[w++] = 0x2f;			/* CP_INIT_MASK */
	ring[w++] = 0x3;			/* multiple HW contexts */
	ring[w++] = 0x20000000;			/* error detection */
	ring[w++] = 0;				/* header dump address */
	ring[w++] = 0;				/* header dump size */
	ring[w++] = 0;				/* ucode workarounds: none for a512 */
	ring[w++] = 0;
	ring[w++] = 0;
	unsigned me_init_end = w;		/* 9, the kernel's first WPTR */
	lv("me_init header", ring[0]);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(GPU_REG(A5XX_CP_RB_RPTR_ADDR_LO), (uint32_t)sh);
	wr(GPU_REG(A5XX_CP_RB_RPTR_ADDR_HI), 0);
	wr(GPU_REG(A5XX_CP_RB_CNTL), A5XX_CP_RB_CNTL_DEFAULT);
	wr(GPU_REG(A5XX_CP_RB_BASE), (uint32_t)rb);
	wr(GPU_REG(A5XX_CP_RB_BASE_HI), 0);
	wr(GPU_REG(A5XX_CP_PM4_INSTR_BASE_LO), (uint32_t)pm4);
	wr(GPU_REG(A5XX_CP_PM4_INSTR_BASE_HI), 0);
	wr(GPU_REG(A5XX_CP_PFP_INSTR_BASE_LO), (uint32_t)pfp);
	wr(GPU_REG(A5XX_CP_PFP_INSTR_BASE_HI), 0);
	cp_state("before start");

	/* kernel order: zap through TZ after the INSTR_BASE writes, before ME_CNTL */
	int zap = zap_load();
	unsigned secure_end = me_init_end;
	if (zap == 0) {
		ring[w++] = pkt7(CP_SET_SECURE_MODE, 1);	/* cp_secure_mode(), a5xx */
		ring[w++] = 0;
		secure_end = w;				/* 0xb, the kernel's second WPTR */
	}
	ring[w++] = pkt7(CP_MEM_WRITE, 3);
	ring[w++] = (uint32_t)sc;
	ring[w++] = 0;
	ring[w++] = 0xc0ffee01;
	ring[w++] = pkt7(CP_NOP, 0);
	unsigned test_end = w;
	lv("mem_write header", ring[secure_end]);
	__asm__ volatile("dsb sy" ::: "memory");

	step("G2b 4 cp start + me_init");
	wr(GPU_REG(A5XX_CP_ME_CNTL), 0);	/* clear the ME/PFP halt */
	udelay(100);
	cp_state("after me_cntl 0");
	int us = rb_submit(me_init_end, 2000000);	/* kernel: 2000 ms spin */
	lv("me_init us", (uint32_t)us);
	cp_state("after me_init");
	if (us < 0) {
		put("G2b RESULT: FAIL (CP did not consume ME_INIT)\n");
		con_str("G2b", "FAIL me_init");
		flush();
		return;
	}

	int unsecure = 0;
	if (zap == 0) {
		step("G2c 2 leave secure mode");
		uint32_t t0 = 0xffffffff;
		int t0rc = pr("rbbm secvid trust cntl before", GPU_REG(A5XX_RBBM_SECVID_TRUST_CNTL), &t0);
		us = rb_submit(secure_end, 2000000);	/* kernel: 2000 ms spin */
		lv("set_secure_mode rptr us", (uint32_t)us);
		/* adreno_spin_idle: RBBM_STATUS & busy_mask (a512 0xfffffffe) == 0 */
		int ius = us < 0 ? -1 : poll(GPU_REG(A5XX_RBBM_STATUS), 0xfffffffe, 0, 2000000);
		lv("idle us", (uint32_t)ius);
		cp_state("after set_secure_mode 0");
		uint32_t t = 0xffffffff;
		int trc = pr("rbbm secvid trust cntl", GPU_REG(A5XX_RBBM_SECVID_TRUST_CNTL), &t);
		unsecure = us >= 0 && ius >= 0 && trc == 0 && t == 0;
		lv("trust cntl changed", t0rc == 0 && trc == 0 && t0 != t);
		put(unsecure ? "G2c: GPU out of secure mode\n" : "G2c: secure mode exit NOT confirmed\n");
		flush();
	}

	step("G2b 5 cp mem_write");
	us = rb_submit(test_end, 2000000);
	lv("mem_write rptr us", (uint32_t)us);
	int mus = poll(sc, 0xffffffff, 0xc0ffee01, 100000);
	lv("mem_write seen us", (uint32_t)mus);
	lv("scratch[0]", scratch[0]);
	cp_state("after mem_write");
	ring_w = test_end;
	cp_ok = mus >= 0;
	if (mus >= 0) {
		put("G2b RESULT: PASS (CP runs Nokia microcode, ME_INIT done, GPU wrote memory)\n");
		con_str("G2b", "PASS");
		put(zap ? "G2c RESULT: FAIL (zap shader not loaded)\n"
			: unsecure ? "G2c RESULT: PASS (zap loaded by TZ, GPU out of secure mode, GPU wrote memory after)\n"
				   : "G2c RESULT: PARTIAL (zap loaded, secure mode exit not confirmed)\n");
		con_str("G2c", zap ? "FAIL" : unsecure ? "PASS" : "PARTIAL");
	} else {
		put(us >= 0 ? "G2b RESULT: PARTIAL (CP runs, GPU memory write not seen)\n"
			    : "G2b RESULT: FAIL (CP stopped after ME_INIT)\n");
		con_str("G2b", us >= 0 ? "PARTIAL" : "FAIL");
	}
	flush();
}

/*
 * GPU SMMU. Measured (run 4, every read on a spare core): IDR1 0x20008005
 * (4 KiB pages, context banks at +0x8000, 5 banks), IDR2 0x7511 (IAS 36,
 * UBS 49), sCR0 0x00200626 (enabled, unmatched streams fault), SMR0-2
 * invalid, S2CR0-2 0x000200ff (fault). CB0 and CB1 read as zero; reading
 * CB2 stalls a core for good (that read hung Gold 0 in run 1). With every
 * stream set to fault, the CP's first fetch could only fault: the reset of
 * run 2. A core whose read stalls is not used again; this core never
 * touches the SMMU itself. Silver cores 1-3 did not start within 20 ms in
 * run 3, so only Gold 1-3 (indices 5-7) are used.
 */
static int cores[6] = { 5, 6, 7 }, ncores = 3, rr, probe_dead[8], probe_left = 3;

static int next_core(void)
{
	for (int k = 0; k < ncores; k++) {
		int core = cores[(rr + k) % ncores];
		if (!probe_dead[core]) {
			rr = (rr + k + 1) % ncores;
			return core;
		}
	}
	return cores[0];	/* not reached while probe_left > 0 */
}

/* log a probe outcome; marks the core dead on a stall */
static void probe_result(int rc, int core, uint64_t esr)
{
	switch (rc) {
	case 0:
		break;
	case -1:
		put("STALLED on core ");
		puthex((uint32_t)core);
		probe_dead[core] = 1;
		probe_left--;
		break;
	case -3:
		put("FAULT esr ");
		puthex((uint32_t)esr);
		break;
	default:
		put("probe rc ");
		puthex((uint32_t)rc);
		break;
	}
}

static int pr(const char *name, uintptr_t a, uint32_t *v)
{
	put(name);
	put(" @");
	puthex((uint32_t)a);
	put(" = ");
	if (!probe_left) {
		put("not read (no spare core left)\n");
		return -5;
	}
	int core = next_core();
	uint64_t esr = 0;
	int rc = drg_probe_read(core, a, v, &esr, 20000);
	if (!rc)
		puthex(*v);
	probe_result(rc, core, esr);
	put("\n");
	flush();
	return rc;
}

/* a register write on a spare core (a stalled write stalls only that core) */
static volatile uintptr_t pw_addr;
static volatile uint32_t pw_val;

static void pw_fn(void)
{
	wr(pw_addr, pw_val);
}

static int __attribute__((unused)) pwr(const char *name, uintptr_t a, uint32_t v)
{
	put(name);
	put(" @");
	puthex((uint32_t)a);
	put(" <- ");
	puthex(v);
	put(": ");
	if (!probe_left) {
		put("not written (no spare core left)\n");
		return -5;
	}
	int core = next_core();
	uint64_t esr = 0;
	pw_addr = a;
	pw_val = v;
	int rc = drg_probe_call(core, pw_fn, &esr, 20000);
	if (!rc)
		put("done");
	probe_result(rc, core, esr);
	put("\n");
	flush();
	return rc;
}


#define SMMU_GR0_SCR0		0x000
#define SMMU_GR0_SGFSR		0x048
#define SMMU_GR0_TLBIALLNSNH	0x068
#define SMMU_GR0_TLBIALLH	0x06c
#define SMMU_GR0_STLBGSYNC	0x070
#define SMMU_GR0_STLBGSTATUS	0x074
#define SMMU_GR0_SMR(n)		(0x800 + 4 * (n))
#define SMMU_GR0_S2CR(n)	(0xc00 + 4 * (n))
#define SMMU_GR1		(SMMU + 0x1000)	/* IDR1 PAGESIZE 0: 4 KiB */
#define SMMU_GR1_CBAR(n)	(4 * (n))
#define SMMU_GR1_CBA2R(n)	(0x800 + 4 * (n))
#define SMMU_MICRO_MMU_CTRL	(SMMU + 6 * 0x1000)	/* ARM_SMMU_IMPL_DEF1 */
#define SMMU_CB0		SMMU_CB_DT	/* IDR1 NUMPAGENDXB 2: SMMU + 8 pages */
#define SMMU_CB_ACTLR		0x04
#define SMMU_CB_TCR2		0x10
#define SMMU_CB_TTBR1		0x28
#define SMMU_CB_TCR		0x30
#define SMMU_CB_MAIR0		0x38
#define SMMU_CB_MAIR1		0x3c
#define SMMU_CB_FAR		0x60
#define SMMU_CB_FSYNR0		0x68
#define SMMU_CB_TLBIALL		0x618
#define SMMU_CB_TLBSYNC		0x7f0
#define SMMU_CB_TLBSTATUS	0x7f4

/*
 * What HMD's arm-smmu.c and io-pgtable-arm.c program on kgsl's first attach
 * (gfx3d_user, stream 0, first free context bank CB0), with this SMMU's IDs:
 *   arm_smmu_device_reset: clear sGFSR, TLBIALLH + TLBIALLNSNH, sync,
 *     sCR0 |= GFRE|GFIE|GCFGFRE|GCFGFIE|VMIDPNE|PTM|USFCFG, &= ~CLIENTPD|FB|BSU
 *   arm_smmu_impl_def_programming: halt, the DT's attach-impl-defs, resume
 *   arm_smmu_init_context_bank: CBA2R RW64, CBAR S1 (S2 bypass) BPSHCFG NSH
 *     MEMATTR WB VMID cbndx+2, TTBR0/1 with ASID cbndx+1, TCR, MAIR, ACTLR
 *     QCOM ISH|OSH|NSH, SCTLR CFIE|CFRE|AFE|TRE|M|ASIDPNE and, for kgsl's
 *     pagefault policy 0 (TWRP sysfs ft_pagefault_policy), HUPCF not CFCFG
 *   io-pgtable ARM_64_LPAE_S1, ias = min(UBS 49, 39) = 39, oas = IAS 36,
 *     page table not coherent: TCR SH0 OS, IRGN0/ORGN0 NC, TG0 4K, IPS 36,
 *     T0SZ 25, EPD1, SEP_UPSTREAM; MAIR0 NC 0x44 | WBRWA 0xff | DEVICE 0x04
 *   arm_smmu_domain_add_master: SMR0 = VALID | id 0, S2CR0 = TRANS | CB0
 * The page table maps the shim's 2 MiB block VA = PA (GPU address =
 * physical address), PTE as arm_lpae_prot_to_pte/__arm_lpae_init_pte for
 * IOMMU_READ|IOMMU_WRITE: nG, AP_RW, AF, SH_OS, block, attr index 0 (NC).
 */
#define SCR0_SET	(0x2 | 0x4 | 0x10 | 0x20 | 0x400 | 0x800 | 0x1000)
#define SCR0_CLEAR	(0x1 | 0x2000 | 0xc000)
#define CB0_CBAR	(0x1u << 16 | 0x3u << 8 | 0xfu << 12 | 2u)	/* 0x0001f302 */
#define CB0_ASID	1ULL
#define CB0_TCR		0x00802019u	/* SH0 OS, TG0 4K, T0SZ 25, EPD1 */
#define CB0_TCR2	0x00038001u	/* IPS 36 bit, SEP_UPSTREAM */
#define CB0_MAIR0	0x0004ff44u
#define CB0_ACTLR	0x70000000u
#define CB0_SCTLR	0x1167u		/* M TRE AFE CFRE CFIE HUPCF ASIDPNE */
#define PTE_BLOCK_RW_NC	(0x800 | 0x40 | 0x400 | 0x200 | 0x1)	/* nG AP_RW AF SH_OS block */
#define PTE_TABLE	0x3

static const uint32_t impl_defs[][2] = {	/* arm,smmu-kgsl@5040000 attach-impl-defs */
	{ 0x6000, 0x2378 }, { 0x6060, 0x1055 }, { 0x678c, 0x8 }, { 0x6794, 0x28 },
	{ 0x6800, 0x6 }, { 0x6900, 0x3ff }, { 0x6924, 0x204 }, { 0x6928, 0x11000 },
	{ 0x6930, 0x800 }, { 0x6960, 0xffffffff }, { 0x6b64, 0x1a5551 }, { 0x6b68, 0x9a82a382 },
};

static uint64_t smmu_l1[512] __attribute__((aligned(4096)));
static uint64_t smmu_l2[512] __attribute__((aligned(4096)));
static volatile uint32_t attach_step, attach_halt_us, attach_sync_ok, attach_cbsync_ok;
static volatile uint32_t attach_scr0_old;

static inline void wr64(uintptr_t a, uint64_t v)
{
	__asm__ volatile("str %x0, [%1]\n\tdsb sy" : : "r"(v), "r"(a) : "memory");
}

/* runs on a spare core: attach_step says how far it got */
static void smmu_attach(void)
{
	uint32_t v;

	attach_step = 10;
	wr(SMMU + SMMU_GR0_SGFSR, rd(SMMU + SMMU_GR0_SGFSR));
	attach_step = 20;
	wr(SMMU + SMMU_GR0_TLBIALLH, 0);
	attach_step = 30;
	wr(SMMU + SMMU_GR0_TLBIALLNSNH, 0);
	attach_step = 40;
	wr(SMMU + SMMU_GR0_STLBGSYNC, 0);
	attach_step = 50;
	attach_sync_ok = poll(SMMU + SMMU_GR0_STLBGSTATUS, 1, 0, 30000) >= 0;
	attach_step = 60;
	v = rd(SMMU + SMMU_GR0_SCR0);
	attach_scr0_old = v;
	attach_step = 70;
	wr(SMMU + SMMU_GR0_SCR0, (v | SCR0_SET) & ~SCR0_CLEAR);

	attach_step = 80;
	wr(SMMU_MICRO_MMU_CTRL, rd(SMMU_MICRO_MMU_CTRL) | 0x4);	/* LOCAL_HALT_REQ */
	attach_step = 90;
	attach_halt_us = (uint32_t)poll(SMMU_MICRO_MMU_CTRL, 0x8, 0x8, 30000);	/* IDLE */
	for (unsigned i = 0; i < sizeof(impl_defs) / sizeof(impl_defs[0]); i++) {
		attach_step = 100 + i;	/* 100..111 */
		wr(SMMU + impl_defs[i][0], impl_defs[i][1]);
	}
	attach_step = 120;
	wr(SMMU_MICRO_MMU_CTRL, rd(SMMU_MICRO_MMU_CTRL) & ~0x4u);

	attach_step = 130;
	wr(SMMU_GR1 + SMMU_GR1_CBA2R(0), 1);	/* RW64 */
	attach_step = 140;
	wr(SMMU_GR1 + SMMU_GR1_CBAR(0), CB0_CBAR);
	attach_step = 150;
	wr64(SMMU_CB0 + SMMU_CB_TTBR0, (uintptr_t)smmu_l1 | CB0_ASID << 48);
	attach_step = 160;
	wr64(SMMU_CB0 + SMMU_CB_TTBR1, CB0_ASID << 48);
	attach_step = 170;
	wr(SMMU_CB0 + SMMU_CB_TCR, CB0_TCR);
	attach_step = 180;
	wr(SMMU_CB0 + SMMU_CB_TCR2, CB0_TCR2);
	attach_step = 190;
	wr(SMMU_CB0 + SMMU_CB_MAIR0, CB0_MAIR0);
	attach_step = 200;
	wr(SMMU_CB0 + SMMU_CB_MAIR1, 0);
	attach_step = 210;
	wr(SMMU_CB0 + SMMU_CB_ACTLR, CB0_ACTLR);
	attach_step = 220;
	wr(SMMU_CB0 + SMMU_CB_SCTLR, CB0_SCTLR);

	attach_step = 230;
	wr(SMMU + SMMU_GR0_SMR(0), 0x80000000u);	/* VALID, id 0, mask 0 */
	attach_step = 240;
	wr(SMMU + SMMU_GR0_S2CR(0), 0);			/* TRANS, CB0 */

	attach_step = 250;
	wr(SMMU_CB0 + SMMU_CB_TLBIALL, 0);
	attach_step = 260;
	wr(SMMU_CB0 + SMMU_CB_TLBSYNC, 0);
	attach_step = 270;
	attach_cbsync_ok = poll(SMMU_CB0 + SMMU_CB_TLBSTATUS, 1, 0, 30000) >= 0;
	attach_step = 999;
}

static void smmu_state(const char *when)
{
	uint32_t v;

	put("-- smmu ");
	put(when);
	put("\n");
	pr("scr0", SMMU + SMMU_GR0_SCR0, &v);
	pr("sgfsr", SMMU + SMMU_GR0_SGFSR, &v);
	pr("sgfsynr0", SMMU + 0x50, &v);
	pr("smr0", SMMU + SMMU_GR0_SMR(0), &v);
	pr("s2cr0", SMMU + SMMU_GR0_S2CR(0), &v);
	pr("cbar0", SMMU_GR1 + SMMU_GR1_CBAR(0), &v);
	pr("cba2r0", SMMU_GR1 + SMMU_GR1_CBA2R(0), &v);
	pr("cb0 sctlr", SMMU_CB0 + SMMU_CB_SCTLR, &v);
	pr("cb0 ttbr0lo", SMMU_CB0 + SMMU_CB_TTBR0, &v);
	pr("cb0 ttbr0hi", SMMU_CB0 + SMMU_CB_TTBR0 + 4, &v);
	pr("cb0 tcr", SMMU_CB0 + SMMU_CB_TCR, &v);
	pr("cb0 tcr2", SMMU_CB0 + SMMU_CB_TCR2, &v);
	pr("cb0 mair0", SMMU_CB0 + SMMU_CB_MAIR0, &v);
	pr("cb0 fsr", SMMU_CB0 + SMMU_CB_FSR, &v);
	pr("cb0 farlo", SMMU_CB0 + SMMU_CB_FAR, &v);
	pr("cb0 fsynr0", SMMU_CB0 + SMMU_CB_FSYNR0, &v);
	pr("micro_mmu_ctrl", SMMU_MICRO_MMU_CTRL, &v);
}

/* 0 if CB0 now translates stream 0 with our table, as read back */
static __attribute__((unused)) int g2b_smmu_attach(void)
{
	uint32_t v;
	int bad = 0;

	step("G2b 1 smmu attach (spare core)");
	smmu_state("before");
	extern char _head[], _end[];
	uintptr_t lo = (uintptr_t)_head & ~0x1fffffUL;
	lv("image pa", (uint32_t)(uintptr_t)_head);
	lv("image end", (uint32_t)(uintptr_t)_end);
	/* the whole image, in 2 MiB blocks inside one 1 GiB L1 entry */
	if (((uintptr_t)_end - 1) >> 30 != lo >> 30 || lo >> 30 >= 512) {
		put("image not inside one 1 GiB region: stop\n");
		return -1;
	}
	smmu_l1[lo >> 30] = (uintptr_t)smmu_l2 | PTE_TABLE;
	for (uintptr_t b = lo; b < (uintptr_t)_end; b += 0x200000)
		smmu_l2[(b >> 21) & 511] = b | PTE_BLOCK_RW_NC;
	__asm__ volatile("dsb sy" ::: "memory");
	lv("l1 pa", (uint32_t)(uintptr_t)smmu_l1);
	lv("l1 entry", (uint32_t)smmu_l1[lo >> 30]);
	lv("l2 entry", (uint32_t)smmu_l2[(lo >> 21) & 511]);
	lv("identity blocks", (uint32_t)(((uintptr_t)_end - lo + 0x1fffff) >> 21));
	flush();

	if (!probe_left) {
		put("no spare core left: stop\n");
		return -1;
	}
	int core = next_core();
	uint64_t esr = 0;
	attach_step = 0;
	int rc = drg_probe_call(core, smmu_attach, &esr, 200000);
	put("attach: ");
	probe_result(rc, core, esr);
	lv("rc", (uint32_t)rc);
	lv("step reached", attach_step);
	lv("scr0 before", attach_scr0_old);
	lv("tlb sync ok", attach_sync_ok);
	lv("halt us", attach_halt_us);
	lv("cb0 tlb sync ok", attach_cbsync_ok);
	flush();
	if (rc || attach_step != 999)
		return -1;

	smmu_state("after attach");
	/* the values HMD's driver would have written must read back */
	bad |= pr("check smr0", SMMU + SMMU_GR0_SMR(0), &v) || v != 0x80000000u;
	bad |= pr("check s2cr0", SMMU + SMMU_GR0_S2CR(0), &v) || (v & 0x300ff) != 0;
	bad |= pr("check cbar0", SMMU_GR1 + SMMU_GR1_CBAR(0), &v) || v != CB0_CBAR;
	bad |= pr("check cb0 sctlr", SMMU_CB0 + SMMU_CB_SCTLR, &v) || v != CB0_SCTLR;
	bad |= pr("check cb0 ttbr0lo", SMMU_CB0 + SMMU_CB_TTBR0, &v) || v != (uint32_t)(uintptr_t)smmu_l1;
	bad |= pr("check scr0", SMMU + SMMU_GR0_SCR0, &v) || (v & 1);
	put(bad ? "G2b SMMU attach: readback MISMATCH\n" : "G2b SMMU attach: readback OK\n");
	flush();
	return bad ? -1 : 0;
}

/*
 * ---- G2c: zap shader ----
 * HMD's kernel loads Nokia's a512_zap through TZ between the microcode base
 * writes and the CP start (adreno_a5xx.c a5xx_microcode_load, measured in
 * the ftrace as PAS 0x42000201/0x202/0x205 for pas 0xd), and then leaves the
 * GPU's secure mode with CP_SET_SECURE_MODE 0 (a5xx_switch_to_unsecure_mode).
 * The load follows HMD's PIL (peripheral-loader.c pil_boot, subsys-pil-tz.c):
 *   - region: loadable segments (PT_LOAD, not hash, memsz) -> lowest
 *     relocatable start .. highest end rounded to 4 KiB; the region itself
 *     is 1 MiB aligned (pil_alloc_region, p_align <= 4 MiB);
 *   - PAS_INIT_IMAGE(pas, metadata = the whole .mdt, physically contiguous)
 *   - PAS_MEM_SETUP(pas, region start, region size)
 *   - copy each segment's .bNN, zero to memsz
 *   - PAS_AUTH_AND_RESET(pas)
 * qcom,kgsl-hyp has no mem-protect-id (no hyp assign), no clocks or supplies.
 * Not done: the scm_pas bus vote around INIT_IMAGE and AUTH_AND_RESET (its
 * RPM values are not measured). Each SMC runs on a spare core, so a TZ call
 * that does not return costs that core only.
 * SMC as HMD's scm.c __scm_call_armv8_64: x0 = function, x1 = arginfo,
 * x2.. = args, x6 = 0; re-entered with its own results while it returns
 * SCM_INTERRUPTED; SCM_V2_EBUSY retried after 30 ms, up to 67 times.
 */
#define SCM_SIP_PIL(cmd)	(0x42000200u | (cmd))	/* SMC64 SIP, SCM_SVC_PIL; as the trace */
#define PAS_INIT_IMAGE		1
#define PAS_MEM_SETUP		2
#define PAS_AUTH_AND_RESET	5
#define PAS_ID_GPU		0xd	/* qcom,kgsl-hyp qcom,pas-id */
#define SCM_INTERRUPTED		1
#define SCM_V2_EBUSY		(-12)

extern const uint8_t drg_zap_mdt[], drg_zap_mdt_end[], drg_zap_b02[], drg_zap_b02_end[];

static uint8_t zap_mdata[8192] __attribute__((aligned(4096)));
static uint8_t zap_pool[0x100000 + 0x4000] __attribute__((aligned(4096)));

static volatile uint64_t scm_x[7];
static volatile uint32_t scm_rounds;

/* runs on a spare core */
static void scm_fn(void)
{
	register uint64_t r0 __asm__("x0") = scm_x[0];
	register uint64_t r1 __asm__("x1") = scm_x[1];
	register uint64_t r2 __asm__("x2") = scm_x[2];
	register uint64_t r3 __asm__("x3") = scm_x[3];
	register uint64_t r4 __asm__("x4") = scm_x[4];
	register uint64_t r5 __asm__("x5") = scm_x[5];
	register uint64_t r6 __asm__("x6") = 0;
	uint32_t n = 0;

	do {
		__asm__ volatile("smc #0"
				 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
				 :
				 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
				   "memory");
		scm_rounds = ++n;
	} while (r0 == SCM_INTERRUPTED && n < 100000);
	scm_x[0] = r0, scm_x[1] = r1, scm_x[2] = r2, scm_x[3] = r3;
}

/* one SCM call; returns x0 (0 = ok) and *res = x1 (the TZ result), or -100 if the core did not return */
static int scm(const char *name, uint32_t cmd, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2,
	       uint64_t *res)
{
	for (unsigned tries = 0;; tries++) {
		scm_x[0] = SCM_SIP_PIL(cmd), scm_x[1] = arginfo;
		scm_x[2] = a0, scm_x[3] = a1, scm_x[4] = a2, scm_x[5] = 0;
		scm_rounds = 0;
		put("scm ");
		put(name);
		put(" fn ");
		puthex(SCM_SIP_PIL(cmd));
		put(": ");
		if (!probe_left) {
			put("no spare core left\n");
			flush();
			return -100;
		}
		int core = next_core();
		uint64_t esr = 0;
		uint64_t t0 = ticks();
		int rc = drg_probe_call(core, scm_fn, &esr, 2000000);
		uint32_t us = (uint32_t)((ticks() - t0) * 1000000 / freq());
		if (rc) {
			probe_result(rc, core, esr);
			put(" rounds ");
			puthex(scm_rounds);
			put("\n");
			flush();
			return -100;
		}
		put("x0 ");
		puthex((uint32_t)scm_x[0]);
		put(" x1 ");
		puthex((uint32_t)scm_x[1]);
		put(" x2 ");
		puthex((uint32_t)scm_x[2]);
		put(" x3 ");
		puthex((uint32_t)scm_x[3]);
		put(" rounds ");
		puthex(scm_rounds);
		put(" us ");
		puthex(us);
		put("\n");
		flush();
		if ((int64_t)scm_x[0] == SCM_V2_EBUSY && tries < 67) {
			udelay(30000);
			continue;
		}
		*res = scm_x[1];
		return (int)(int64_t)scm_x[0];
	}
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* 0 when TZ accepted and started the zap shader */
static int zap_load(void)
{
	const uint8_t *m = drg_zap_mdt;
	uint32_t msz = (uint32_t)(drg_zap_mdt_end - drg_zap_mdt);
	uint32_t bsz = (uint32_t)(drg_zap_b02_end - drg_zap_b02);
	uint64_t res = 0;

	step("G2c 1 zap shader load (a512_zap, pas 0xd)");
	lv("mdt bytes", msz);
	lv("b02 bytes", bsz);
	if (msz < 0x34 || le32(m) != 0x464c457f || m[4] != 1) {
		put("zap: mdt is not ELF32: stop\n");
		return -1;
	}
	uint32_t phoff = le32(m + 0x1c), entry = le32(m + 0x18);
	unsigned phent = le16(m + 0x2a), phnum = le16(m + 0x2c);
	if (phent != 32 || phoff + phnum * 32u > msz) {
		put("zap: program headers not within mdt: stop\n");
		return -1;
	}
	uint32_t min_r = 0xffffffff, max_r = 0, align = 0, seg_pa = 0, seg_f = 0, seg_m = 0;
	int nload = 0, seg_num = -1, nonreloc = 0;
	for (unsigned i = 0; i < phnum; i++) {
		const uint8_t *ph = m + phoff + 32 * i;
		uint32_t type = le32(ph), pa = le32(ph + 12), fsz = le32(ph + 16), mem = le32(ph + 20);
		uint32_t flags = le32(ph + 24), al = le32(ph + 28);
		put("phdr ");
		puthex(i);
		put(" type ");
		puthex(type);
		put(" pa ");
		puthex(pa);
		put(" filesz ");
		puthex(fsz);
		put(" memsz ");
		puthex(mem);
		put(" flags ");
		puthex(flags);
		put(" align ");
		puthex(al);
		put("\n");
		if (!(type == 1 && ((flags >> 24) & 7) != 2 && mem))	/* segment_is_loadable */
			continue;
		nload++;
		seg_num = (int)i, seg_pa = pa, seg_f = fsz, seg_m = mem;
		if (!(flags & (1u << 27))) {	/* segment_is_relocatable */
			nonreloc++;
			continue;
		}
		if (pa < min_r)
			min_r = pa, align = al;
		if (pa + mem > max_r)
			max_r = pa + mem;
	}
	max_r = (max_r + 0xfff) & ~0xfffu;
	/* what is embedded: exactly one relocatable loadable segment, number 2 (.b02) */
	if (nload != 1 || nonreloc || seg_num != 2 || seg_f != bsz || seg_m < seg_f || align > 0x400000) {
		put("zap: segment layout is not the embedded one (.b02): stop\n");
		flush();
		return -1;
	}
	uint32_t size = max_r - min_r;
	uintptr_t region = ((uintptr_t)zap_pool + 0xfffff) & ~(uintptr_t)0xfffff;	/* 1 MiB */
	if (region + size > (uintptr_t)zap_pool + sizeof(zap_pool) || msz > sizeof(zap_mdata) ||
	    (region >> 32)) {
		put("zap: buffers do not fit: stop\n");
		flush();
		return -1;
	}
	for (uint32_t i = 0; i < msz; i++)
		zap_mdata[i] = m[i];
	lv("metadata pa", (uint32_t)(uintptr_t)zap_mdata);
	lv("region pa", (uint32_t)region);
	lv("region size", size);
	lv("entry (relocated)", entry - min_r + (uint32_t)region);
	__asm__ volatile("dsb sy" ::: "memory");
	flush();

	int rc = scm("init_image", PAS_INIT_IMAGE, 0x82, PAS_ID_GPU, (uintptr_t)zap_mdata, 0, &res);
	if (rc || res) {
		put("zap: PAS_INIT_IMAGE failed\n");
		flush();
		return -1;
	}
	rc = scm("mem_setup", PAS_MEM_SETUP, 0x3, PAS_ID_GPU, region, size, &res);
	if (rc || res) {
		put("zap: PAS_MEM_SETUP failed\n");
		flush();
		return -1;
	}
	volatile uint8_t *dst = (volatile uint8_t *)(region + (seg_pa - min_r));
	for (uint32_t i = 0; i < seg_m; i++)
		dst[i] = i < seg_f ? drg_zap_b02[i] : 0;
	__asm__ volatile("dsb sy" ::: "memory");
	lv("segment 2 at", (uint32_t)(uintptr_t)dst);
	rc = scm("auth_and_reset", PAS_AUTH_AND_RESET, 0x1, PAS_ID_GPU, 0, 0, &res);
	if (rc || res) {
		put("zap: PAS_AUTH_AND_RESET failed\n");
		flush();
		return -1;
	}
	put("zap: loaded and started by TZ\n");
	flush();
	return 0;
}

/*
 * ---- G2d: GPU clock 19.2 MHz (XO) -> 266 MHz (PLL0 532 MHz / 2) ----
 * Measured on this phone:
 *   - ftrace (kgsl power-up): PLL0 L 0x1b, ALPHA 0x55555600, ALPHA_U 0xb5;
 *     MODE 2 -> 6 -> (lock) -> 7; gfx3d RCG CFG 0x103 (source 1 = PLL0, div 2)
 *     with UPDATE; MX (pm660l_s5_level, RPM rwmx) level 192 voted, CX
 *     (pm660l_s3_level) >= 128 already met (no RPM message).
 *   - TWRP, kgsl forced to 266 MHz (pwrlevel 2): PLL0 USER_CTL 0x01200001
 *     (VCO 2 = 500-1000 MHz, ALPHA_EN, main out), USER_CTL_U 4, CONFIG_CTL
 *     0x4001055b; PM660L S3 (CX) 764 mV, S5 (MX) 852 mV, the same as with
 *     the GPU off (VOLTAGE_CTL 0x40/0x41, mV, SID 3 peripherals 0x1a/0x20).
 * HMD sources: gpucc-sdm660.c (gpu_pll0_config, gfx3d ping-pong: from XO the
 * first PLL is PLL0, CFG = source << 8 | (2 * div - 1)), clk-alpha-pll.c
 * (clk_alpha_pll_configure, _enable: BYPASSNL, 5 us, RESET_N, LOCK_DET,
 * OUTCTRL), clk-rcg2.c (update_config).
 * Rails: the shim only votes a rail that reads below the measured level, so
 * a vote can only raise it (an RPM vote replaces the APPS vote; a blind lower
 * value could drop what ABL set for the display).
 * GPU clock is measured, not assumed: RBBM perf counter 1, countable 0
 * (every GPU cycle), against the 19.2 MHz CPU timer.
 */
#define PLL0_MODE		(GPUCC_PLL0 + 0x00)
#define PLL0_L			(GPUCC_PLL0 + 0x04)
#define PLL0_ALPHA		(GPUCC_PLL0 + 0x08)
#define PLL0_ALPHA_U		(GPUCC_PLL0 + 0x0c)
#define PLL0_USER_CTL		(GPUCC_PLL0 + 0x10)
#define PLL0_USER_CTL_U		(GPUCC_PLL0 + 0x14)
#define PLL0_CONFIG_CTL		(GPUCC_PLL0 + 0x18)
#define PLL_OUTCTRL		(1U << 0)
#define PLL_BYPASSNL		(1U << 1)
#define PLL_RESET_N		(1U << 2)
#define PLL_VOTE_FSM_ENA	(1U << 20)
#define PLL_LOCK_DET		(1U << 31)
#define PLL_VCO_MASK		(3U << 20)
#define PLL_VCO_500_1000	(2U << 20)
#define GFX3D_CFG_PLL0_DIV2	0x103
#define A5XX_RBBM_PERFCTR_RBBM_1_LO	0x3b2
#define A5XX_RBBM_PERFCTR_RBBM_1_HI	0x3b3
#define A5XX_RBBM_PERFCTR_RBBM_SEL_1	0x46c
#define PM660L_S3_CTRL		0x31a	/* SID 3, peripheral 0x1a (type 0x1c sub 0x0a) */
#define PM660L_S5_CTRL		0x320
#define CX_MV_AT_266		764	/* TWRP, GPU at 266 MHz */
#define MX_MV_AT_266		852

static uint64_t gpu_cycles(void)
{
	uint32_t hi, lo;
	do {
		hi = rd(GPU_REG(A5XX_RBBM_PERFCTR_RBBM_1_HI));
		lo = rd(GPU_REG(A5XX_RBBM_PERFCTR_RBBM_1_LO));
	} while (hi != rd(GPU_REG(A5XX_RBBM_PERFCTR_RBBM_1_HI)));
	return (uint64_t)hi << 32 | lo;
}

/*
 * Hardware clock gating, as HMD's a5xx_hwcg_set(): off = 0 to every HWCG
 * register, RBBM_CLOCK_CNTL 0, ISDB_CNT 0x180; on = the values the kernel
 * wrote on this phone (the hwcg block that opens a5xx_start_seq, up to its
 * final RBBM_CLOCK_CNTL 0xaaa82a00). Run 10: with gating on, an idle GPU
 * counted 90 cycles in 2 ms, so the clock is measured with gating off.
 */
#define A5XX_RBBM_CLOCK_CNTL	0x67
#define A5XX_RBBM_ISDB_CNT	0x533

static unsigned hwcg_end(void)	/* index after the final RBBM_CLOCK_CNTL of the hwcg block */
{
	for (unsigned i = 1; i < sizeof(a5xx_start_seq) / sizeof(a5xx_start_seq[0]); i++)
		if (a5xx_start_seq[i][0] == A5XX_RBBM_CLOCK_CNTL && a5xx_start_seq[i][1] == 0xaaa82a00)
			return i + 1;
	return 0;
}

static void hwcg_set(int on)
{
	unsigned end = hwcg_end();
	for (unsigned i = 1; i < end; i++) {
		uint32_t r = a5xx_start_seq[i][0];
		if (on)
			wr(GPU_REG(r), a5xx_start_seq[i][1]);
		else if (r != A5XX_RBBM_CLOCK_CNTL && r != A5XX_RBBM_ISDB_CNT)
			wr(GPU_REG(r), 0);
	}
	if (!on) {
		wr(GPU_REG(A5XX_RBBM_CLOCK_CNTL), 0);
		wr(GPU_REG(A5XX_RBBM_ISDB_CNT), 0x180);
	}
}

/* GPU clock in kHz over 2 ms of CPU time, clock gating off while counting */
static uint32_t gpu_khz(const char *when)
{
	if (!hwcg_end()) {
		put("hwcg block not found in the start sequence\n");
		return 0;
	}
	hwcg_set(0);
	wr(GPU_REG(A5XX_RBBM_PERFCTR_RBBM_SEL_1), 0);	/* countable 0: every cycle */
	udelay(10);
	uint64_t t0 = ticks(), c0 = gpu_cycles();
	udelay(2000);
	uint64_t t1 = ticks(), c1 = gpu_cycles();
	hwcg_set(1);
	uint64_t us = (t1 - t0) * 1000000 / freq();
	uint32_t khz = us ? (uint32_t)((c1 - c0) * 1000 / us) : 0;
	put("gpu clock ");
	put(when);
	put(": cycles ");
	puthex((uint32_t)(c1 - c0));
	put(" in us ");
	puthex((uint32_t)us);
	put(" = kHz ");
	puthex(khz);
	put("\n");
	flush();
	return khz;
}

/* millivolts of a PM660L buck (VOLTAGE_CTL1/2), or 0 */
static uint32_t rail_mv(const char *name, uint32_t ppid)
{
	uint8_t lo = 0, hi = 0, en = 0;
	int rc = drg_pmic_read(ppid, 0x40, &lo) | drg_pmic_read(ppid, 0x41, &hi) | drg_pmic_read(ppid, 0x46, &en);
	uint32_t mv = rc ? 0 : (uint32_t)hi << 8 | lo;
	put(name);
	put(": rc ");
	puthex((uint32_t)rc);
	put(" mV ");
	puthex(mv);
	put(" en ");
	puthex(en);
	put("\n");
	flush();
	return mv;
}

/* ring_w: next free ring dword after g2b_cp; cp_ok: its memory write landed */

static int g2d_ok;

static __attribute__((unused)) void g2d_clocks(void)
{
	step("G2d 1 gpu clock at xo");
	uint32_t k0 = gpu_khz("at xo");
	if (k0 < 18000 || k0 > 20500) {
		put("G2d RESULT: STOP (cycle counter does not read 19.2 MHz at xo)\n");
		con_str("G2d", "STOP counter");
		flush();
		return;
	}

	step("G2d 2 rails (cx, mx)");
	uint32_t cx = rail_mv("cx pm660l s3", PM660L_S3_CTRL), mx = rail_mv("mx pm660l s5", PM660L_S5_CTRL);
	if (cx && cx < CX_MV_AT_266) {
		put("cx below the level measured at 266 MHz: vote rwcx vlvl 128\n");
		lv("rpm rc", (uint32_t)drg_rpm_request2(RPM_RES_RWCX, 0, RPM_KEY_SWEN, 1, RPM_KEY_VLVL, 128));
		udelay(1000);
		cx = rail_mv("cx after vote", PM660L_S3_CTRL);
	}
	if (mx && mx < MX_MV_AT_266) {
		put("mx below the level measured at 266 MHz: vote rwmx vlvl 192\n");
		lv("rpm rc", (uint32_t)drg_rpm_request2(RPM_RES_RWMX, 0, RPM_KEY_SWEN, 1, RPM_KEY_VLVL, 192));
		udelay(1000);
		mx = rail_mv("mx after vote", PM660L_S5_CTRL);
	}
	if (cx < CX_MV_AT_266 || mx < MX_MV_AT_266) {
		put("G2d RESULT: STOP (rails below the levels measured at 266 MHz)\n");
		con_str("G2d", "STOP rails");
		flush();
		return;
	}

	step("G2d 3 pll0 532 MHz");
	uint32_t mode = rd(PLL0_MODE);
	lv("pll0 mode", mode);
	lv("pll0 user_ctl", rd(PLL0_USER_CTL));
	lv("pll0 user_ctl_u", rd(PLL0_USER_CTL_U));
	lv("pll0 config_ctl", rd(PLL0_CONFIG_CTL));
	if (mode & (PLL_VOTE_FSM_ENA | PLL_OUTCTRL | PLL_RESET_N | PLL_BYPASSNL)) {
		put("pll0 not off / in fsm mode: stop\n");
		flush();
		return;
	}
	/* clk_alpha_pll_configure (gpu_pll0_config): only the VCO field differs from what ABL left */
	wr(PLL0_USER_CTL, (rd(PLL0_USER_CTL) & ~PLL_VCO_MASK) | PLL_VCO_500_1000);
	wr(PLL0_L, 0x1b);
	wr(PLL0_ALPHA, 0x55555600);
	wr(PLL0_ALPHA_U, 0xb5);
	wr(PLL0_MODE, rd(PLL0_MODE) | PLL_BYPASSNL);
	udelay(5);
	wr(PLL0_MODE, rd(PLL0_MODE) | PLL_RESET_N);
	int us = poll(PLL0_MODE, PLL_LOCK_DET, PLL_LOCK_DET, 1000);
	lv("pll0 lock us", (uint32_t)us);
	if (us < 0) {
		wr(PLL0_MODE, rd(PLL0_MODE) & ~(PLL_RESET_N | PLL_BYPASSNL));
		lv("pll0 mode (off again)", rd(PLL0_MODE));
		put("G2d RESULT: FAIL (pll0 did not lock; gpu stays on xo)\n");
		con_str("G2d", "FAIL pll lock");
		flush();
		return;
	}
	wr(PLL0_MODE, rd(PLL0_MODE) | PLL_OUTCTRL);
	lv("pll0 mode", rd(PLL0_MODE));
	lv("pll0 user_ctl", rd(PLL0_USER_CTL));
	lv("pll0 l", rd(PLL0_L));
	flush();

	step("G2d 4 gfx3d to pll0 / 2");
	lv("gfx3d cfg before", rd(GPUCC_GFX3D_CFG));
	wr(GPUCC_GFX3D_CFG, GFX3D_CFG_PLL0_DIV2);
	wr(GPUCC_GFX3D_CMD, rd(GPUCC_GFX3D_CMD) | RCG_UPDATE);
	us = poll(GPUCC_GFX3D_CMD, RCG_UPDATE, 0, 2000);
	lv("gfx3d update us", (uint32_t)us);
	lv("gfx3d cfg", rd(GPUCC_GFX3D_CFG));
	lv("gfx3d cmd", rd(GPUCC_GFX3D_CMD));
	lv("gfx3d cbcr", rd(GPUCC_GFX3D_CBCR));
	flush();
	uint32_t k1 = gpu_khz("after switch");
	int clk_ok = us >= 0 && k1 > 260000 && k1 < 272000;

	step("G2d 5 cp at the new clock");
	int wrote = 0;
	if (cp_ok && ring_w + 5 < RB_DWORDS) {
		unsigned w = ring_w;
		scratch[1] = 0;
		ring[w++] = pkt7(CP_MEM_WRITE, 3);
		ring[w++] = (uint32_t)(uintptr_t)&scratch[1];
		ring[w++] = 0;
		ring[w++] = 0xc0ffee02;
		ring[w++] = pkt7(CP_NOP, 0);
		__asm__ volatile("dsb sy" ::: "memory");
		int rus = rb_submit(w, 2000000);
		int mus = poll((uintptr_t)&scratch[1], 0xffffffff, 0xc0ffee02, 100000);
		lv("rptr us", (uint32_t)rus);
		lv("mem_write seen us", (uint32_t)mus);
		lv("scratch[1]", scratch[1]);
		cp_state("at 266 MHz");
		wrote = rus >= 0 && mus >= 0;
		ring_w = w;
	}
	put(clk_ok && wrote ? "G2d RESULT: PASS (gpu at 266 MHz measured, CP wrote memory at it)\n"
		: clk_ok ? "G2d RESULT: PARTIAL (266 MHz measured, CP write not seen)\n"
			 : "G2d RESULT: FAIL (clock not measured at 266 MHz)\n");
	con_str("G2d", clk_ok && wrote ? "PASS" : clk_ok ? "PARTIAL" : "FAIL");
	g2d_ok = clk_ok && wrote;
	flush();
}

/*
 * ---- G2e: every GPU power level, 160/266/370/430 MHz ----
 * Measured in TWRP (research/vendor-adreno/trace/levels-trace.txt, kgsl
 * stepped 3 -> 2 -> 1 -> 0 -> 2 with the GPU forced on):
 *   level  GPU    PLL      L     ALPHA       ALPHA_U
 *   3      160    640 /2*  0x21  0x55555600  0x55
 *   2      266    532      0x1b  0x55555600  0xb5
 *   1      370    740      0x26  0xaaaaab00  0x8a
 *   0      430    860      0x2c  0xaaaaab00  0xca
 *   (* PLL post-divider 2: gpucc-sdm660.c pll_data {640 MHz, 0x1})
 *   - ping-pong: each change uses the PLL that is off (430 -> 266 ran 532 MHz
 *     on PLL1); RCG CFG = source << 8 | 3 (div 2), PLL0 source 1, PLL1 source 3
 *   - a PLL is programmed while off, enabled (2, 6, lock, 7), the RCG muxed
 *     to it with UPDATE, then the other PLL turned off (0x80000006, 0x80000000);
 *   - bus: gpubw -> RPM bslv 0 (EBI) "bw" = the APPS aggregate msm_bus sends
 *     (sum_ab, bus_agg_bw in the trace), which includes the display:
 *     439229440 (levels 2, 3), 1007557632 (level 1), 1592663040 (level 0);
 *   - CX (rwcx vlvl) 192 and mem-acc corner 2 only at level 0, CX 128 and
 *     mem-acc corner 1 below (cpr4 gfx_corner: closed loop and LDO mode off
 *     for all corners in the DT, so it only votes CX and mem-acc);
 *     mem-acc = TCSR 0x1fcf004 bit 0, corner 1 -> 1, corner 2 -> 0
 *     (gfx_mem_acc_corner, corner-acc-map <1 0>, read-modify-write);
 *   - MX (rwmx) vlvl 192 with swen 1 while a GPU PLL runs;
 *   - PM660L S3 (CX) 764 mV and S5 (MX) 852 mV at every level.
 * Order here is the safe one: voltage, mem-acc and bus up before a clock
 * goes up, down only after it came down. (Linux's clock framework sends the
 * CX vote just after switching to 430 MHz.)
 */
#define PLL_BASE(n)		(GPUCC_PLL0 + 0x40UL * (n))
#define PLL_POSTDIV_MASK	(0xfU << 8)
#define MEM_ACC_SEL		0x1fcf004UL
#define GPU_PLL_CONFIG_CTL	0x4001055bu	/* gpu_pll0_config, applied to both PLLs at probe */

static const struct gpu_level {
	uint32_t khz, l, alpha, alpha_u, postdiv, cx, memacc;
	uint64_t bw;
} gpu_levels[4] = {
	{ 430000, 0x2c, 0xaaaaab00, 0xca, 0, 192, 2, 1592663040ULL },
	{ 370000, 0x26, 0xaaaaab00, 0x8a, 0, 128, 1, 1007557632ULL },
	{ 266000, 0x1b, 0x55555600, 0xb5, 0, 128, 1, 439229440ULL },
	{ 160000, 0x21, 0x55555600, 0x55, 1, 128, 1, 439229440ULL },
};

static int cur_level = 2, cur_pll = 0;	/* after G2d: 266 MHz on PLL0 */
static uint64_t cur_bw;
static uint32_t cur_cx = 128, cur_memacc = 1;

static int rpm_ack_ok(const char *what, int rc)
{
	uint32_t w[4];
	drg_rpm_last_ack(w);
	put("rpm ");
	put(what);
	put(" rc ");
	puthex((uint32_t)rc);
	put(" ack ");
	for (int i = 0; i < 4; i++) {
		puthex(w[i]);
		put(" ");
	}
	put("\n");
	flush();
	return rc;
}

static int set_bw(uint64_t bw)
{
	if (bw == cur_bw)
		return 0;
	int rc = rpm_ack_ok("bslv 0 bw", drg_rpm_request64(RPM_RES_BSLV, RPM_BSLV_EBI, RPM_KEY_BW, bw));
	if (!rc)
		cur_bw = bw;
	return rc;
}

static int set_cx(uint32_t lvl)
{
	if (lvl == cur_cx)
		return 0;
	int rc = rpm_ack_ok("rwcx vlvl", drg_rpm_request2(RPM_RES_RWCX, 0, RPM_KEY_SWEN, 1, RPM_KEY_VLVL, lvl));
	if (!rc)
		cur_cx = lvl;
	return rc;
}

/* mem_acc_regulator_set_voltage: one corner at a time, RMW of bit 0 */
static void set_memacc(uint32_t corner)
{
	while (cur_memacc != corner) {
		cur_memacc += corner > cur_memacc ? 1 : -1;
		uint32_t bit = cur_memacc == 1 ? 1 : 0;
		wr(MEM_ACC_SEL, (rd(MEM_ACC_SEL) & ~1U) | bit);
	}
	lv("mem-acc sel", rd(MEM_ACC_SEL));
}

/* program PLL n (off) and lock it; 0 or -1 */
static int pll_on(unsigned n, const struct gpu_level *g)
{
	uintptr_t p = PLL_BASE(n);
	uint32_t mode = rd(p);
	if (mode & (PLL_VOTE_FSM_ENA | PLL_OUTCTRL | PLL_RESET_N | PLL_BYPASSNL)) {
		lv("pll busy, mode", mode);
		return -1;
	}
	wr(p + 0x18, GPU_PLL_CONFIG_CTL);
	wr(p + 0x10, (rd(p + 0x10) & ~(PLL_VCO_MASK | PLL_POSTDIV_MASK)) | PLL_VCO_500_1000 | g->postdiv << 8);
	wr(p + 0x04, g->l);
	wr(p + 0x08, g->alpha);
	wr(p + 0x0c, g->alpha_u);
	wr(p, rd(p) | PLL_BYPASSNL);
	udelay(5);
	wr(p, rd(p) | PLL_RESET_N);
	int us = poll(p, PLL_LOCK_DET, PLL_LOCK_DET, 1000);
	if (us < 0) {
		wr(p, rd(p) & ~(PLL_RESET_N | PLL_BYPASSNL));
		lv("pll did not lock, mode", rd(p));
		return -1;
	}
	wr(p, rd(p) | PLL_OUTCTRL);
	put("pll ");
	puthex(n);
	put(" locked us ");
	puthex((uint32_t)us);
	put(" mode ");
	puthex(rd(p));
	put(" user_ctl ");
	puthex(rd(p + 0x10));
	put("\n");
	return 0;
}

static void pll_off(unsigned n)
{
	uintptr_t p = PLL_BASE(n);
	wr(p, rd(p) & ~PLL_OUTCTRL);
	wr(p, rd(p) & ~(PLL_RESET_N | PLL_BYPASSNL));
}

/* switch the GPU clock to gpu_levels[lvl]; 0 when measured there */
static int gpu_set_level(int lvl)
{
	const struct gpu_level *g = &gpu_levels[lvl];
	int up = g->khz > gpu_levels[cur_level].khz;
	unsigned n = cur_pll ^ 1;	/* ping-pong: the PLL that is off */
	if (up) {
		if (set_bw(g->bw) || set_cx(g->cx > cur_cx ? g->cx : cur_cx))
			return -1;
		set_memacc(g->memacc > cur_memacc ? g->memacc : cur_memacc);
	}
	if (pll_on(n, g))
		return -1;
	wr(GPUCC_GFX3D_CFG, (n ? 3U : 1U) << 8 | 3);	/* PLL1 source 3, PLL0 source 1; div 2 */
	wr(GPUCC_GFX3D_CMD, rd(GPUCC_GFX3D_CMD) | RCG_UPDATE);
	int us = poll(GPUCC_GFX3D_CMD, RCG_UPDATE, 0, 2000);
	if (us < 0) {
		lv("rcg update timeout, cfg", rd(GPUCC_GFX3D_CFG));
		pll_off(n);
		return -1;
	}
	pll_off(cur_pll);
	cur_pll = n;
	cur_level = lvl;
	if (!up) {
		set_memacc(g->memacc);
		set_cx(g->cx);
		set_bw(g->bw);
	}
	return 0;
}

static int g2e_ok;

static __attribute__((unused)) void g2e_levels(void)
{
	static const int order[] = { 3, 2, 1, 0, 2 };	/* the TWRP trace's order */
	uint32_t v;
	int pass = 1;

	step("G2e 1 pll1 and mem-acc reachable (spare core reads)");
	if (pr("pll1 mode", PLL_BASE(1), &v) || pr("pll1 user_ctl", PLL_BASE(1) + 0x10, &v) ||
	    pr("mem-acc sel", MEM_ACC_SEL, &v)) {
		put("G2e RESULT: STOP (pll1 or mem-acc not readable)\n");
		con_str("G2e", "STOP reads");
		flush();
		return;
	}
	cur_memacc = (v & 1) ? 1 : 2;
	lv("mem-acc corner now", cur_memacc);

	step("G2e 2 bus and mx votes for the running GPU");
	rpm_ack_ok("rwmx swen 1 vlvl 192", drg_rpm_request2(RPM_RES_RWMX, 0, RPM_KEY_SWEN, 1, RPM_KEY_VLVL, 192));
	set_bw(gpu_levels[2].bw);

	for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		int lvl = order[i];
		char name[] = "G2e 3 level x";
		name[12] = (char)('0' + lvl);
		step(name);
		int rc = gpu_set_level(lvl);
		uint32_t khz = gpu_khz("at level");
		uint32_t want = gpu_levels[lvl].khz;
		int clk_ok = !rc && khz > want - want / 50 && khz < want + want / 50;
		rail_mv("cx", PM660L_S3_CTRL);
		rail_mv("mx", PM660L_S5_CTRL);
		/* one CP memory write at this clock */
		int wrote = 0;
		if (cp_ok && ring_w + 5 < RB_DWORDS) {
			unsigned w = ring_w;
			scratch[2] = 0;
			ring[w++] = pkt7(CP_MEM_WRITE, 3);
			ring[w++] = (uint32_t)(uintptr_t)&scratch[2];
			ring[w++] = 0;
			ring[w++] = 0xc0de0000 | want / 1000;
			ring[w++] = pkt7(CP_NOP, 0);
			__asm__ volatile("dsb sy" ::: "memory");
			int rus = rb_submit(w, 2000000);
			int mus = poll((uintptr_t)&scratch[2], 0xffffffff, 0xc0de0000 | want / 1000, 100000);
			wrote = rus >= 0 && mus >= 0;
			ring_w = w;
		}
		lv("cp write", (uint32_t)wrote);
		lv("cp hw fault", rd(GPU_REG(A5XX_CP_HW_FAULT)));
		put(clk_ok && wrote ? "level OK\n" : "level FAILED\n");
		flush();
		if (!(clk_ok && wrote)) {
			pass = 0;
			break;
		}
	}
	put(pass ? "G2e RESULT: PASS (160/266/370/430 MHz measured, CP works at each, back at 266)\n"
		 : "G2e RESULT: FAIL (see the level above)\n");
	con_str("G2e", pass ? "PASS" : "FAIL");
	g2e_ok = pass;
	flush();
}

/*
 * Core power states (not GPU), read only. Measured (run s1-run1, 2026-10-30):
 * after the handoff only Gold 0 is on, every Silver core and Gold 1-3 are off
 * (AFFINITY_INFO 1). A CPU_ON of Silver 1 at that point is accepted (rc 0)
 * but the core stays ON_PENDING (2) and never executes its first
 * instruction (start.S marker), so the shim never powers Silver on: the
 * cluster stays off, and it could not run XNU anyway (no 16 KB granule,
 * MMFR0 TGran16 0, boot #3). XNU uses the four Gold cores.
 */
static void core_states(void)
{
	step("S1 core power states (psci affinity_info: 0 on, 1 off, 2 on pending)");
	int silver_off = 1;
	for (int i = 0; i < 8; i++) {
		int64_t a = drg_psci_affinity(i);
		put("core ");
		puthex((uint32_t)i);
		put(" aff ");
		puthex((uint32_t)a);
		put("\n");
		if (i < 4 && a != 1)
			silver_off = 0;
	}
	put(silver_off ? "silver cluster: all off\n" : "silver cluster: NOT all off\n");
	flush();
}

/*
 * ---- G3r: replay Nokia-driver GPU work on our own driver path ----
 * drg/GPU-IOS-DESIGN.md gate I2. The capture (drgles bench -capture in
 * TWRP, kgsl_bench.c) holds every GPU object the Nokia driver had (id, GPU
 * address, size, bytes before each GPU_COMMAND), each GPU_COMMAND, and every
 * object after it completed on Linux's KGSL. Here each object gets pages of
 * its own, mapped at the same GPU address through drgpu_pt (the portable
 * page-table code the XNU driver will use); the IBs run with the framing
 * HMD's adreno_ringbuffer.c gives them (CP_INDIRECT_BUFFER_PFE per IB, then
 * a CP_EVENT_WRITE CACHE_FLUSH_TS end-of-pipe fence; MARKER commands carry
 * no work and are not run, as in KGSL); after each command every object is
 * compared with Linux's result (FNV-1a hash; differing bytes where the
 * capture kept the data). Record format: kgsl_bench.c.
 */
extern const uint8_t drg_cap[], drg_cap_end[];

#define CP_INDIRECT_BUFFER_PFE	0x3f	/* adreno_pm4types.h */
#define CP_EVENT_WRITE		0x46
#define CACHE_FLUSH_TS		4	/* a3xx_reg.h, all Adreno generations */
#define KGSL_CMDBATCH_MARKER	0x2	/* msm_kgsl.h */
#define KGSL_CMDLIST_IB		0x1

static uint8_t gpu_heap[0x200000] __attribute__((aligned(4096)));
static uint64_t pt_pool[32][512] __attribute__((aligned(4096)));
static unsigned heap_used, pt_used;

static int os_alloc_table(void *os, struct drgpu_page *p)
{
	(void)os;
	if (pt_used >= 32)
		return -1;
	uint64_t *t = pt_pool[pt_used++];
	for (int i = 0; i < 512; i++)
		t[i] = 0;
	p->va = t;
	p->pa = (uintptr_t)t;	/* MMU off: physical = CPU address */
	return 0;
}

static uint64_t *os_table_va(void *os, uint64_t pa)
{
	(void)os;
	return (uint64_t *)(uintptr_t)pa;
}

static void os_publish(void *os)
{
	(void)os;
	__asm__ volatile("dsb sy" ::: "memory");
}

static struct drgpu_pt gpu_pt;

static volatile uint32_t tlb_ok;

static void tlb_fn(void)	/* spare core: CB0 TLBIALL + sync, as the attach does */
{
	wr(SMMU_CB0 + SMMU_CB_TLBIALL, 0);
	wr(SMMU_CB0 + SMMU_CB_TLBSYNC, 0);
	tlb_ok = poll(SMMU_CB0 + SMMU_CB_TLBSTATUS, 1, 0, 30000) >= 0;
}

static int tlb_flush(void)
{
	if (!probe_left)
		return -1;
	int core = next_core();
	uint64_t esr = 0;
	tlb_ok = 0;
	int rc = drg_probe_call(core, tlb_fn, &esr, 100000);
	if (rc) {
		put("tlb flush: ");
		probe_result(rc, core, esr);
		put("\n");
	}
	return rc || !tlb_ok ? -1 : 0;
}

static uint32_t cle32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t cle64(const uint8_t *p) { return cle32(p) | (uint64_t)cle32(p + 4) << 32; }

static uint64_t fnv_mem(uintptr_t a, uint64_t n)
{
	const volatile uint8_t *p = (const volatile uint8_t *)a;
	uint64_t h = 0xcbf29ce484222325ULL;
	for (uint64_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

static struct robj { uint32_t id, live; uint64_t size, va, len, pa, h0; } robjs[64];

static struct robj *robj_of(uint32_t id)
{
	for (int i = 0; i < 64; i++)
		if (robjs[i].live && robjs[i].id == id)
			return &robjs[i];
	return 0;
}

static __attribute__((unused)) void g3_replay(void)
{
	step("G3r 1 replay of the Nokia driver's GPU work (cap1)");
	gpu_pt = (struct drgpu_pt){ { (uintptr_t)smmu_l1, smmu_l1 }, os_alloc_table, os_table_va, os_publish, 0 };
	lv("capture bytes", (uint32_t)(drg_cap_end - drg_cap));
	const uint8_t *p = drg_cap, *end = drg_cap_end;
	int tlb_dirty = 0, bad = 0, executed = 0, markers = 0;
	uint32_t compared = 0, matched = 0, gpu_changed = 0, gpu_changed_ok = 0, seq = 0;

	while (p + 16 <= end && !bad) {
		uint32_t magic = cle32(p), type = cle32(p + 4), len = cle32(p + 8);
		const uint8_t *d = p + 16;
		if (magic != 0x43475244 || d + len > end) {
			put("capture: bad record\n");
			bad = 1;
			break;
		}
		p = d + len;
		if (type == 1) {			/* ALLOC: size, flags, va_len, mmapsize, id */
			for (int i = 0; i < 64; i++)
				if (!robjs[i].live) {
					robjs[i] = (struct robj){ cle32(d + 32), 1, cle64(d + 24), 0, 0, 0, 0 };
					break;
				}
		} else if (type == 2) {			/* MAP: off, va, len, prot */
			struct robj *o = robj_of((uint32_t)(cle64(d) >> 12));
			uint64_t va = cle64(d + 8), n = (cle64(d + 16) + 0xfff) & ~0xfffULL;
			if (o && (cle32(d + 24) & 1) && !o->pa) {
				if (heap_used + n > sizeof(gpu_heap)) {
					put("gpu heap full\n");
					bad = 1;
					break;
				}
				o->pa = (uintptr_t)gpu_heap + heap_used;
				heap_used += (unsigned)n;
				o->va = va;
				o->len = cle64(d + 16);
				if (drgpu_pt_map(&gpu_pt, va, o->pa, n, DRGPU_PT_RW)) {
					put("gpu map failed\n");
					bad = 1;
					break;
				}
				tlb_dirty = 1;
			}
		} else if (type == 3) {			/* FREE: id */
			struct robj *o = robj_of(cle32(d));
			if (o) {
				if (o->pa)
					drgpu_pt_unmap(&gpu_pt, o->va, (o->len + 0xfff) & ~0xfffULL);
				o->live = 0;
				tlb_dirty = 1;
			}
		} else if (type == 5) {			/* OBJ: id, phase, submit, has_data, gpuaddr, len, hash, data */
			struct robj *o = robj_of(cle32(d));
			uint32_t phase = cle32(d + 4), has = cle32(d + 12);
			uint64_t n = cle64(d + 24), h = cle64(d + 32);
			if (!o || !o->pa || n > o->len) {
				put("object record without our mapping\n");
				continue;
			}
			if (phase == 0) {
				if (has) {
					volatile uint8_t *dst = (volatile uint8_t *)o->pa;
					for (uint64_t i = 0; i < n; i++)
						dst[i] = d[40 + i];
				}
				o->h0 = h;
			} else {
				uint64_t mine = fnv_mem(o->pa, n);
				compared++;
				int gpu = h != o->h0;
				gpu_changed += gpu;
				if (mine == h) {
					matched++;
					gpu_changed_ok += gpu;
				} else {
					put("MISMATCH obj ");
					puthex(o->id);
					put(" gpuaddr ");
					puthex((uint32_t)(o->va >> 32));
					puthex((uint32_t)o->va);
					put(gpu ? " (gpu-written)" : " (not gpu-written in capture)");
					if (has) {
						uint32_t diff = 0;
						const volatile uint8_t *m = (const volatile uint8_t *)o->pa;
						for (uint64_t i = 0; i < n; i++)
							diff += m[i] != d[40 + i];
						put(" bytes differing ");
						puthex(diff);
						put(" of ");
						puthex((uint32_t)n);
					}
					put("\n");
				}
				if (gpu && mine == h) {
					put("gpu-written obj ");
					puthex(o->id);
					put(" matches Linux\n");
				}
			}
		} else if (type == 4) {			/* SUBMIT: n, kgsl_gpu_command, cmdlist, objlist, synclist */
			uint32_t sub = cle32(d);
			const uint8_t *c = d + 4;
			uint64_t flags = cle64(c);
			uint32_t cmdsize = cle32(c + 16), numcmds = cle32(c + 20);
			put("submit ");
			puthex(sub);
			put(" flags ");
			puthex((uint32_t)flags);
			put(" cmds ");
			puthex(numcmds);
			if (flags & KGSL_CMDBATCH_MARKER) {
				put(": marker, no work\n");
				markers++;
				flush();
				continue;
			}
			put("\n");
			if (tlb_dirty) {
				if (tlb_flush()) {
					put("tlb flush failed\n");
					bad = 1;
					break;
				}
				tlb_dirty = 0;
			}
			unsigned w = ring_w;
			if (w + 4 * numcmds + 8 > RB_DWORDS) {
				put("ring full\n");
				bad = 1;
				break;
			}
			for (uint32_t i = 0; i < numcmds; i++) {
				const uint8_t *co = c + 64 + i * cmdsize;
				uint64_t ga = cle64(co + 8), sz = cle64(co + 16);
				if (!(cle32(co + 24) & KGSL_CMDLIST_IB))
					continue;
				ring[w++] = pkt7(CP_INDIRECT_BUFFER_PFE, 3);
				ring[w++] = (uint32_t)ga;
				ring[w++] = (uint32_t)(ga >> 32);
				ring[w++] = (uint32_t)(sz >> 2);
			}
			seq++;
			scratch[8] = 0;
			ring[w++] = pkt7(CP_EVENT_WRITE, 4);
			ring[w++] = CACHE_FLUSH_TS;
			ring[w++] = (uint32_t)(uintptr_t)&scratch[8];
			ring[w++] = 0;
			ring[w++] = seq;
			__asm__ volatile("dsb sy" ::: "memory");
			int rus = rb_submit(w, 2000000);
			int fus = poll((uintptr_t)&scratch[8], 0xffffffff, seq, 2000000);
			ring_w = w;
			lv("rptr us", (uint32_t)rus);
			lv("fence us", (uint32_t)fus);
			uint32_t hw = rd(GPU_REG(A5XX_CP_HW_FAULT)), irq = rd(GPU_REG(A5XX_RBBM_INT_0_STATUS));
			lv("cp hw fault", hw);
			lv("rbbm int0", irq);
			flush();
			executed++;
			if (rus < 0 || fus < 0 || hw) {
				uint32_t v;
				cp_state("replay stopped");
				pr("cb0 fsr", SMMU_CB0 + SMMU_CB_FSR, &v);
				pr("cb0 farlo", SMMU_CB0 + SMMU_CB_FAR, &v);
				pr("cb0 farhi", SMMU_CB0 + SMMU_CB_FAR + 4, &v);
				pr("cb0 fsynr0", SMMU_CB0 + SMMU_CB_FSYNR0, &v);
				bad = 1;
				break;
			}
		}
	}
	lv("submits run", (uint32_t)executed);
	lv("markers", (uint32_t)markers);
	lv("objects compared", compared);
	lv("objects matching Linux", matched);
	lv("gpu-written objects", gpu_changed);
	lv("gpu-written matching", gpu_changed_ok);
	lv("heap used", heap_used);
	lv("page-table pages", pt_used);
	int pass = !bad && executed && compared && matched == compared && gpu_changed && gpu_changed_ok == gpu_changed;
	put(pass ? "G3r RESULT: PASS (Nokia GPU work replayed on our driver path, every object identical to Linux)\n"
		 : "G3r RESULT: FAIL (see above)\n");
	con_str("G3r", pass ? "PASS" : "FAIL");
	flush();
}

/*
 * ---- G4: the portable driver core (gpu/drgpu) on the phone ----
 * drg/GPU-IOS-DESIGN.md gate I3: the same calls XNU's DrgGPU will make,
 * with this file as the OS layer (MMU off: physical = CPU address). RPM in
 * attach mode: drg_rpm.c opened the channel earlier in this boot, exactly
 * the situation XNU is in. Then cap1 replayed through drgpu_map/submit/wait
 * and every object compared with Linux, as G3r did with the old code.
 */
static uint8_t core_pool[0x400000] __attribute__((aligned(4096)));
static uint64_t core_pool_used;

static uintptr_t c_mmio(void *ctx, uint64_t pa, uint64_t len)
{
	(void)ctx;
	(void)len;
	return (uintptr_t)pa;
}

static int c_alloc(void *ctx, uint64_t size, uint64_t align, struct drgpu_mem *out)
{
	(void)ctx;
	uint64_t base = (uintptr_t)core_pool, at = (base + core_pool_used + align - 1) & ~(align - 1);
	if (at - base + size > sizeof(core_pool))	/* offsets: no end-address literal (absolute relocation) */
		return -1;
	core_pool_used = at + size - base;
	volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)at;
	for (uint64_t i = 0; i < size; i++)
		p[i] = 0;
	out->pa = at;
	out->va = (void *)(uintptr_t)at;
	out->size = size;
	return 0;
}

static void c_free(void *ctx, struct drgpu_mem *m)
{
	(void)ctx;
	(void)m;	/* bump allocator: the test frees nothing it needs back */
}

static uint64_t *c_ptov(void *ctx, uint64_t pa)
{
	(void)ctx;
	return (uint64_t *)(uintptr_t)pa;
}

static void c_log(void *ctx, const char *msg)
{
	(void)ctx;
	put(msg);
	put("\n");
	flush();
}

static void c_sleep(void *ctx, uint32_t us)
{
	(void)ctx;
	udelay(us);
}

static struct drgpu core_gpu;
static struct drgpu_space space_a, space_b;	/* two process address spaces */
uint64_t drg_fwblob_write(uintptr_t pa);	/* loader.c */
/* the replay's last draw and its render target, for the power-cycle test */
static struct drgpu_ib last_ibs[8];
static unsigned last_n;
static uint32_t last_ctx, last_ts;
static uint64_t last_rt_pa, last_rt_len, last_rt_hash;
extern void (*volatile drg_irq_hook)(uint32_t intid);	/* shim.c */
int drg_gic_spi(uint32_t intid, int on, uint32_t *grp_before, uint32_t *grp_after);
static volatile uint32_t gpu_irqs, gpu_irq_status;
static volatile uint64_t gpu_irq_t;

static void gpu_irq_hook(uint32_t intid)
{
	if (intid != 332)
		return;
	gpu_irq_status |= drgpu_irq(&core_gpu);	/* clear the level source before the EOI */
	gpu_irq_t = ticks();
	gpu_irqs++;
}
extern const uint8_t drg_pm4_file[], drg_pm4_file_end[], drg_pfp_file[], drg_pfp_file_end[];

/* after a failed submission: where the CP stopped and why (a5xx_cp_hw_err_callback, SMMU fault regs) */
static void gpu_fail_dump(struct drgpu *g)
{
	lreg("cp hw fault", GPU_REG(A5XX_CP_HW_FAULT));
	lreg("cp int status", GPU_REG(A5XX_CP_INTERRUPT_STATUS));
	lreg("cp protect status", GPU_REG(A5XX_CP_PROTECT_STATUS));
	lreg("rbbm int 0 status", GPU_REG(A5XX_RBBM_INT_0_STATUS));
	lreg("rbbm status", GPU_REG(A5XX_RBBM_STATUS));
	lreg("rbbm status3", GPU_REG(A5XX_RBBM_STATUS3));
	lreg("rbbm ahb error", GPU_REG(A5XX_RBBM_AHB_ERROR_STATUS));
	lreg("cp rb rptr", GPU_REG(A5XX_CP_RB_RPTR));
	lv("ring write dword", g->ring_w);
	lreg("smmu cb0 fsr", SMMU_CB0 + SMMU_CB_FSR);
	lreg("smmu cb0 far lo", SMMU_CB0 + SMMU_CB_FAR);
	lreg("smmu cb0 far hi", SMMU_CB0 + SMMU_CB_FAR + 4);
	lreg("smmu cb0 ttbr0 lo", SMMU_CB0 + SMMU_CB_TTBR0);
	lreg("smmu cb0 ttbr0 hi", SMMU_CB0 + SMMU_CB_TTBR0 + 4);
	const volatile uint32_t *si = (const volatile uint32_t *)g->smmu_info.va;
	lv("pt info ttbr0 lo (written by the switch)", si[0x800 / 4]);
	lv("pt info ttbr0 hi", si[0x800 / 4 + 1]);
	/* the ring around the read pointer */
	uint32_t r = rd(GPU_REG(A5XX_CP_RB_RPTR));
	const volatile uint32_t *ring = (const volatile uint32_t *)g->ring.va;
	for (uint32_t i = (r >= 8 ? r - 8 : 0); i < r + 4 && i < DRGPU_RING_DWORDS; i++) {
		put("ring ");
		puthex(i);
		put(" ");
		puthex(ring[i]);
		put("\n");
	}
	/* a hung GPU keeps hang detect asserted: no GPU interrupts from here on */
	wr(GPU_REG(A5XX_RBBM_INT_0_MASK), 0);
	flush();
}

static void g4_core(void)
{
	/* built at run time: the shim is not relocated, so no function pointers in static data */
	struct drgpu_os os;
	os.ctx = 0;
	os.mmio = c_mmio;
	os.alloc = c_alloc;
	os.free = c_free;
	os.alloc_tz = c_alloc;	/* as before: the shim never maps its zap memory again */
	os.ptov = c_ptov;
	os.log = c_log;
	os.sleep_us = c_sleep;
	/*
	 * Firmware the way XNU gets it: the loader's blob writer (loader.c),
	 * then the shared parser (drgpu_fwblob.h); every file must come back
	 * byte for byte.
	 */
	step("G4 0 firmware blob: loader writer -> shared parser");
	struct drgpu_fw fw;
	struct drgpu_mem blobm = { 0, 0, 0 };
	int prc = -9;
	uint64_t blen = 0;
	if (!c_alloc(0, 0x10000, 0x4000, &blobm)) {
		blen = drg_fwblob_write((uintptr_t)blobm.pa);
		prc = drgpu_fwblob_parse((const uint8_t *)blobm.va, blobm.size, &fw);
	}
	lv("blob bytes", (uint32_t)blen);
	lv("parse rc", (uint32_t)prc);
	int same_fw = !prc && fw.pm4_len == (uint32_t)(drg_pm4_file_end - drg_pm4_file) &&
		      fw.pfp_len == (uint32_t)(drg_pfp_file_end - drg_pfp_file) &&
		      fw.zap_mdt_len == (uint32_t)(drg_zap_mdt_end - drg_zap_mdt) &&
		      fw.zap_b02_len == (uint32_t)(drg_zap_b02_end - drg_zap_b02);
	for (uint32_t i = 0; same_fw && i < fw.pm4_len; i++)
		same_fw = fw.pm4[i] == drg_pm4_file[i];
	for (uint32_t i = 0; same_fw && i < fw.pfp_len; i++)
		same_fw = fw.pfp[i] == drg_pfp_file[i];
	for (uint32_t i = 0; same_fw && i < fw.zap_mdt_len; i++)
		same_fw = fw.zap_mdt[i] == drg_zap_mdt[i];
	for (uint32_t i = 0; same_fw && i < fw.zap_b02_len; i++)
		same_fw = fw.zap_b02[i] == drg_zap_b02[i];
	lv("files identical", (uint32_t)same_fw);
	if (same_fw) {	/* a flipped byte must be caught by the FNV check */
		((volatile uint8_t *)blobm.va)[blen - 1] ^= 0x40;
		struct drgpu_fw fw_bad;
		lv("parse of a damaged blob (want -3)", (uint32_t)drgpu_fwblob_parse((const uint8_t *)blobm.va, blobm.size, &fw_bad));
		((volatile uint8_t *)blobm.va)[blen - 1] ^= 0x40;
	}
	flush();
	if (!same_fw) {
		put("G4 RESULT: FAIL (firmware blob)\n");
		con_str("G4", "FAIL blob");
		flush();
		return;
	}
	struct drgpu *g = &core_gpu;
	int pass = 1;

	step("G4 1 drgpu_init + power + smmu + cp start");
	uint64_t t0 = ticks();
	int rc = drgpu_init(g, &os, &fw);
	lv("init", (uint32_t)rc);
	if (!rc)
		lv("rpm attach", (uint32_t)(rc = drgpu_rpm_attach(g, 1)));
	if (!rc)
		lv("power_on", (uint32_t)(rc = drgpu_power_on(g)));
	if (!rc)
		lv("smmu_attach", (uint32_t)(rc = drgpu_smmu_attach(g)));
	if (!rc)
		lv("cp_start", (uint32_t)(rc = drgpu_cp_start(g)));
	lv("bring-up us", (uint32_t)((ticks() - t0) * 1000000 / freq()));
	if (rc) {
		put("G4 RESULT: FAIL (");
		put(drgpu_err(g));
		put(")\n");
		con_str("G4", "FAIL bring-up");
		flush();
		return;
	}
	lv("trust cntl", rd(GPU_REG(A5XX_RBBM_SECVID_TRUST_CNTL)));
	lv("kHz on xo", drgpu_gpu_khz(g));

	step("G4 2 levels 160/266/370/430/266");
	static const int order[] = { 3, 2, 1, 0, 2 };
	static const uint32_t want[] = { 430000, 370000, 266000, 160000 };
	for (unsigned i = 0; i < 5; i++) {
		int l = order[i];
		rc = drgpu_set_level(g, l);
		uint32_t khz = drgpu_gpu_khz(g);
		put("level ");
		puthex((uint32_t)l);
		put(" rc ");
		puthex((uint32_t)rc);
		put(" kHz ");
		puthex(khz);
		put(" mem-acc ");
		puthex(g->memacc);
		put("\n");
		flush();
		if (rc || khz < want[l] - want[l] / 50 || khz > want[l] + want[l] / 50)
			pass = 0;
	}

	step("G4 3 cap1 replay through the core, in process space A");
	lv("space a create", (uint32_t)drgpu_space_create(g, &space_a));
	lv("space a asid", space_a.asid);
	const uint8_t *p = drg_cap, *end = drg_cap_end;
	struct cobj { uint32_t id, live, stale; uint64_t va, len, pa, h0; } objs[64];
	for (int i = 0; i < 64; i++)
		objs[i].live = 0;
	int dirty = 0, executed = 0, bad = 0;
	uint32_t compared = 0, matched = 0, gpuw = 0, gpuw_ok = 0;
	/*
	 * After the last draw only its results are compared: the capture's later
	 * frees, maps and uploads are not applied, so that draw's memory stays as
	 * it ran for the G4 5 redraw.
	 */
	const uint8_t *last_draw = 0;
	for (const uint8_t *q = p; q + 16 <= end && cle32(q) == 0x43475244 && q + 16 + cle32(q + 8) <= end;
	     q += 16 + cle32(q + 8))
		if (cle32(q + 4) == 4 && !(cle64(q + 20) & KGSL_CMDBATCH_MARKER))
			last_draw = q;
	while (p + 16 <= end && !bad) {
		uint32_t type = cle32(p + 4), len = cle32(p + 8);
		const uint8_t *d = p + 16;
		if (cle32(p) != 0x43475244 || d + len > end) {
			bad = 1;
			break;
		}
		int after = last_draw && p > last_draw;	/* p: this record */
		p = d + len;
		if (after && type != 5)
			continue;
		if (type == 1) {
			for (int i = 0; i < 64; i++)
				if (!objs[i].live) {
					objs[i] = (struct cobj){ cle32(d + 32), 1, 0, 0, cle64(d + 24), 0, 0 };
					break;
				}
		} else if (type == 2 || type == 3 || type == 5) {
			uint32_t id = type == 2 ? (uint32_t)(cle64(d) >> 12) : cle32(d);
			struct cobj *o = 0;
			for (int i = 0; i < 64; i++)
				if (objs[i].live && objs[i].id == id)
					o = &objs[i];
			if (!o)
				continue;
			if (type == 2 && (cle32(d + 24) & 1) && !o->pa) {
				struct drgpu_mem m;
				uint64_t n = (cle64(d + 16) + 0xfff) & ~0xfffULL;
				if (c_alloc(0, n, 0x1000, &m) || drgpu_map(g, &space_a, cle64(d + 8), m.pa, n, 0)) {
					put("map failed\n");
					bad = 1;
					break;
				}
				o->pa = m.pa;
				o->va = cle64(d + 8);
				o->len = cle64(d + 16);
				dirty = 1;
			} else if (type == 3) {
				if (o->pa)
					drgpu_unmap(g, &space_a, o->va, (o->len + 0xfff) & ~0xfffULL);
				o->live = 0;
				dirty = 1;
			} else if (type == 5 && o->pa) {
				uint32_t phase = cle32(d + 4), has = cle32(d + 12);
				uint64_t n = cle64(d + 24), h = cle64(d + 32);
				if (phase == 0 && after) {
					if (fnv_mem(o->pa, n) != h)
						o->stale = 1;	/* Linux changed it after the last draw: not comparable */
				} else if (phase == 0) {
					volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)o->pa;
					if (has)
						for (uint64_t i = 0; i < n; i++)
							dst[i] = d[40 + i];
					o->h0 = h;
				} else if (!o->stale) {
					uint64_t mine = fnv_mem(o->pa, n);
					compared++;
					gpuw += h != o->h0;
					if (mine == h) {
						matched++;
						gpuw_ok += h != o->h0;
						if (h != o->h0) {	/* render target after the last draw */
							last_rt_pa = o->pa;
							last_rt_len = n;
							last_rt_hash = h;
						}
					} else {
						put("MISMATCH obj ");
						puthex(o->id);
						put("\n");
					}
				}
			}
		} else if (type == 4) {
			const uint8_t *c = d + 4;
			uint64_t flags = cle64(c);
			uint32_t cmdsize = cle32(c + 16), numcmds = cle32(c + 20), ctx = cle32(c + 56), ts = cle32(c + 60);
			if (flags & KGSL_CMDBATCH_MARKER)
				continue;
			if (dirty && drgpu_tlb_flush(g)) {
				bad = 1;
				break;
			}
			dirty = 0;
			struct drgpu_ib ibs[8];
			unsigned n = 0;
			for (uint32_t i = 0; i < numcmds && n < 8; i++) {
				const uint8_t *co = c + 64 + i * cmdsize;
				if (cle32(co + 24) & KGSL_CMDLIST_IB)
					ibs[n++] = (struct drgpu_ib){ cle64(co + 8), (uint32_t)(cle64(co + 16) >> 2) };
			}
			for (unsigned i = 0; i < n; i++)
				last_ibs[i] = ibs[i];
			last_n = n;
			last_ctx = ctx;
			last_ts = ts;
			uint64_t s0 = ticks();
			uint32_t got = drgpu_submit(g, &space_a, ctx, ibs, n, ts);
			int w = got ? drgpu_wait(g, ctx, ts, 2000000) : -1;
			put("submit ctx ");
			puthex(ctx);
			put(" ts ");
			puthex(ts);
			put(" wait ");
			puthex((uint32_t)w);
			put(" us ");
			puthex((uint32_t)((ticks() - s0) * 1000000 / freq()));
			put(" retired ");
			puthex(drgpu_retired(g, ctx));
			put(" irq ");
			puthex(drgpu_irq(g));
			put("\n");
			flush();
			executed++;
			if (w || rd(GPU_REG(A5XX_CP_HW_FAULT))) {
				put(drgpu_err(g));
				put("\n");
				gpu_fail_dump(g);
				bad = 1;
				break;
			}
		}
	}
	lv("submits run", (uint32_t)executed);
	lv("objects compared", compared);
	lv("objects matching Linux", matched);
	lv("gpu-written", gpuw);
	lv("gpu-written matching", gpuw_ok);
	lv("pool used", (uint32_t)core_pool_used);
	if (bad || executed != 3 || !compared || matched != compared || !gpuw || gpuw_ok != gpuw)
		pass = 0;
	if (bad) {	/* the GPU is stuck: the later stages would only hang the phone */
		put("G4 RESULT: FAIL (replay; later stages skipped)\n");
		con_str("G4", "FAIL");
		flush();
		return;
	}

	/*
	 * GPU interrupt to this core: kgsl-3d0 "interrupts = <0 0x12c 0>" =
	 * SPI 300 = INTID 332. The end-of-pipe CP_EVENT_WRITE carries bit 31
	 * (interrupt); the hook clears RBBM_INT_0 (a level source) before EOI.
	 */
	step("G4 4 gpu interrupt (SPI 300 = INTID 332) to this core");
	uint32_t gb = 9, ga = 9;
	rc = drg_gic_spi(332, 1, &gb, &ga);
	lv("gic setup", (uint32_t)rc);
	lv("group bit before", gb);
	lv("group bit after", ga);
	(void)drgpu_irq(g);
	gpu_irqs = 0;
	gpu_irq_status = 0;
	drg_irq_hook = gpu_irq_hook;
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	uint32_t got_irqs = 0, lat_max = 0, lat_sum = 0;
	for (uint32_t k = 1; k <= 5; k++) {
		uint32_t before = gpu_irqs;
		uint64_t s0 = ticks();
		if (!drgpu_submit(g, 0, 3, 0, 0, k))	/* kernel space: switches back from A */
			break;
		uint64_t end2 = s0 + freq() / 10;
		while (gpu_irqs == before && ticks() < end2)
			;
		if (gpu_irqs != before) {
			got_irqs++;
			uint32_t us = (uint32_t)((gpu_irq_t - s0) * 1000000 / freq());
			lat_sum += us;
			if (us > lat_max)
				lat_max = us;
		}
	}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	drg_irq_hook = 0;
	(void)drg_gic_spi(332, 0, &gb, &ga);
	lv("interrupts for 5 submits", got_irqs);
	lv("int status seen", gpu_irq_status);
	lv("latency us max", lat_max);
	lv("latency us avg", got_irqs ? lat_sum / got_irqs : 0);
	lv("retired ctx 3", drgpu_retired(g, 3));
	if (got_irqs != 5 || !(gpu_irq_status & (1U << 20)))
		pass = 0;

	/*
	 * Idle power collapse and restore, three times: GX off (clocks, PLL,
	 * votes down), then resume at 266 MHz without reloading the zap shader
	 * (a512: ADRENO_CPZ_RETENTION) and re-run the replay's last draw into a
	 * cleared render target: it must come back identical to Linux's.
	 */
	step("G4 5 power collapse / restore x3 (last draw re-run each time)");
	wr(SMMU_CB0 + SMMU_CB_FSR, rd(SMMU_CB0 + SMMU_CB_FSR));	/* fresh fault state */
	for (int cyc = 0; cyc < 3 && last_n && last_rt_pa; cyc++) {
		uint64_t c0 = ticks();
		int orc = drgpu_power_off(g);
		uint32_t off_us = (uint32_t)((ticks() - c0) * 1000000 / freq());
		uint32_t gx = rd(GDSC_GX), clk = rd(GPUCC_GFX3D_CBCR);
		udelay(20000);
		volatile uint8_t *rt = (volatile uint8_t *)(uintptr_t)last_rt_pa;
		for (uint64_t i = 0; i < last_rt_len; i++)
			rt[i] = 0;
		c0 = ticks();
		int rrc = drgpu_resume(g, 2);
		uint32_t on_us = (uint32_t)((ticks() - c0) * 1000000 / freq());
		uint32_t khz = drgpu_gpu_khz(g);
		last_ts += 0x100;
		uint32_t got = rrc ? 0 : drgpu_submit(g, &space_a, last_ctx, last_ibs, last_n, last_ts);
		int w = got ? drgpu_wait(g, last_ctx, last_ts, 2000000) : -1;
		int same = fnv_mem(last_rt_pa, last_rt_len) == last_rt_hash;
		put("cycle ");
		puthex((uint32_t)cyc);
		put(" off rc ");
		puthex((uint32_t)orc);
		put(" us ");
		puthex(off_us);
		put(" gx gdsc ");
		puthex(gx);
		put(" gfx3d cbcr ");
		puthex(clk);
		put(" | resume rc ");
		puthex((uint32_t)rrc);
		put(" us ");
		puthex(on_us);
		put(" kHz ");
		puthex(khz);
		put(" trust ");
		puthex(rd(GPU_REG(A5XX_RBBM_SECVID_TRUST_CNTL)));
		put(" | redraw wait ");
		puthex((uint32_t)w);
		put(" identical ");
		puthex((uint32_t)same);
		put(" | ttbr0 hi ");
		puthex(rd(SMMU_CB0 + SMMU_CB_TTBR0 + 4));
		put("\n");
		if (rrc)
			put(drgpu_err(g)), put("\n");
		flush();
		if (w) {	/* stuck GPU: dump and stop collapsing it */
			gpu_fail_dump(g);
			pass = 0;
			break;
		}
		if (orc || rrc || w || !same || (gx & GDSC_PWR_ON) || !(clk & CBCR_CLK_OFF) || khz < 260000 || khz > 272000)
			pass = 0;
	}
	if (!last_n || !last_rt_pa)
		pass = 0;

	/*
	 * Per-process address spaces: A and B map the same GPU address X to
	 * different pages and the same IB address Y to different IBs. Queued
	 * A B A B; then B, the CP's current space, is destroyed (CPU path back
	 * to the kernel's space) and A runs again.
	 */
	step("G4 6 per-process address spaces (same GPU address, different pages)");
	const uint64_t X = 0x700000000ULL, Y = 0x700010000ULL;
	struct drgpu_mem pa1, pa2, iba, ibb;
	int irc = drgpu_space_create(g, &space_b);
	irc |= c_alloc(0, 0x1000, 0x1000, &pa1) | c_alloc(0, 0x1000, 0x1000, &pa2) |
	       c_alloc(0, 0x1000, 0x1000, &iba) | c_alloc(0, 0x1000, 0x1000, &ibb);
	if (!irc) {
		irc |= drgpu_map(g, &space_a, X, pa1.pa, 0x1000, 0) | drgpu_map(g, &space_a, Y, iba.pa, 0x1000, 1);
		irc |= drgpu_map(g, &space_b, X, pa2.pa, 0x1000, 0) | drgpu_map(g, &space_b, Y, ibb.pa, 0x1000, 1);
		irc |= drgpu_tlb_flush(g);
	}
	lv("setup rc", (uint32_t)irc);
	lv("space b asid", space_b.asid);
	if (!irc) {
		volatile uint32_t *ia = iba.va, *ib = ibb.va, *p1 = pa1.va, *p2 = pa2.va;
		ia[0] = pkt7(CP_MEM_WRITE, 3), ia[1] = (uint32_t)X, ia[2] = (uint32_t)(X >> 32), ia[3] = 0xaaaa0001;
		ib[0] = pkt7(CP_MEM_WRITE, 3), ib[1] = (uint32_t)X, ib[2] = (uint32_t)(X >> 32), ib[3] = 0xbbbb0001;
		p1[0] = 0, p2[0] = 0;
		__asm__ volatile("dsb sy" ::: "memory");
		struct drgpu_ib one = { Y, 4 };
		uint32_t ok = 0;
		ok += !!drgpu_submit(g, &space_a, 4, &one, 1, 1);
		ok += !!drgpu_submit(g, &space_b, 5, &one, 1, 1);
		ok += !!drgpu_submit(g, &space_a, 4, &one, 1, 2);
		ok += !!drgpu_submit(g, &space_b, 5, &one, 1, 2);
		int wa = drgpu_wait(g, 4, 2, 2000000), wb = drgpu_wait(g, 5, 2, 2000000);
		lv("queued", ok);
		lv("wait a", (uint32_t)wa);
		lv("wait b", (uint32_t)wb);
		lv("page under X in A (want aaaa0001)", p1[0]);
		lv("page under X in B (want bbbb0001)", p2[0]);
		lv("cb0 ttbr0 hi (asid of the current space)", rd(SMMU_CB0 + SMMU_CB_TTBR0 + 4));
		lv("cp hw fault", rd(GPU_REG(A5XX_CP_HW_FAULT)));
		int iso_ok = ok == 4 && !wa && !wb && p1[0] == 0xaaaa0001 && p2[0] == 0xbbbb0001;
		/* destroy B while it is current, then A again */
		p1[0] = 0;
		int drc = drgpu_space_destroy(g, &space_b);
		uint32_t ttbr_after = rd(SMMU_CB0 + SMMU_CB_TTBR0 + 4);
		uint32_t got3 = drgpu_submit(g, &space_a, 4, &one, 1, 3);
		int w3 = got3 ? drgpu_wait(g, 4, 3, 2000000) : -1;
		lv("destroy b rc", (uint32_t)drc);
		lv("ttbr0 hi after destroy (kernel asid 1 << 16)", ttbr_after);
		lv("a again: wait", (uint32_t)w3);
		lv("a again: page (want aaaa0001)", p1[0]);
		lv("b's page untouched (want bbbb0001)", p2[0]);
		lv("cp hw fault", rd(GPU_REG(A5XX_CP_HW_FAULT)));
		lv("smmu cb0 fsr", rd(SMMU_CB0 + SMMU_CB_FSR));
		iso_ok &= !drc && !w3 && p1[0] == 0xaaaa0001 && p2[0] == 0xbbbb0001 && (ttbr_after >> 16) == 1;
		put(iso_ok ? "per-process spaces: isolated and switched correctly\n" : "per-process spaces: FAILED\n");
		flush();
		if (!iso_ok) {
			gpu_fail_dump(g);
			pass = 0;
		}
	} else {
		pass = 0;
	}
	put(pass ? "G4 RESULT: PASS (portable core: bring-up, all levels, Nokia work identical to Linux)\n"
		 : "G4 RESULT: FAIL (see above)\n");
	con_str("G4", pass ? "PASS" : "FAIL");
	flush();
}

void drg_gpu_bringup(void)
{
	con_skip();
	int rc = drg_emmc_log_open();
	con_hex2("GPU LOG", "rc=", (uint32_t)rc, "lba=", (uint32_t)drg_emmc_log_lba());
	put("DRGGPULOG v15 G1 + G4 portable core (bring-up, levels, replay, irq, collapse, spaces)\n");
	lv("log open rc", (uint32_t)rc);
	core_states();
	g1_snapshot("G1 as ABL left it");
	g4_core();
	put("== end\n");
	flush();
}
