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
#define A5XX_RBBM_AHB_ERROR_STATUS	0x4f4
#define A5XX_RBBM_STATUS	0x4f5
#define A5XX_RBBM_STATUS3	0x530
#define A5XX_VBIF_VERSION	0x3000

/* adreno_pm4types.h (HMD) */
#define CP_TYPE7_PKT		(7U << 28)
#define CP_NOP			0x10
#define CP_MEM_WRITE		0x3d
#define CP_ME_INIT		0x48
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

#include "drg_gpu_start.h"

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
static int g2a_power(void)
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

static void g2a_registers(void)
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

static void g2b_cp(void)
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
	ring[w++] = pkt7(CP_MEM_WRITE, 3);
	ring[w++] = (uint32_t)sc;
	ring[w++] = 0;
	ring[w++] = 0xc0ffee01;
	ring[w++] = pkt7(CP_NOP, 0);
	unsigned test_end = w;
	lv("me_init header", ring[0]);
	lv("mem_write header", ring[me_init_end]);
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

	step("G2b 5 cp mem_write");
	us = rb_submit(test_end, 2000000);
	lv("mem_write rptr us", (uint32_t)us);
	int mus = poll(sc, 0xffffffff, 0xc0ffee01, 100000);
	lv("mem_write seen us", (uint32_t)mus);
	lv("scratch[0]", scratch[0]);
	cp_state("after mem_write");
	if (mus >= 0) {
		put("G2b RESULT: PASS (CP runs Nokia microcode, ME_INIT done, GPU wrote memory)\n");
		con_str("G2b", "PASS");
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
static int g2b_smmu_attach(void)
{
	uint32_t v;
	int bad = 0;

	step("G2b 1 smmu attach (spare core)");
	smmu_state("before");
	extern char _head[], _end[];
	uintptr_t lo = (uintptr_t)_head & ~0x1fffffUL;
	lv("image pa", (uint32_t)(uintptr_t)_head);
	lv("image end", (uint32_t)(uintptr_t)_end);
	if (((uintptr_t)_end - 1) >> 21 != lo >> 21 || lo >> 30 >= 512) {
		put("image not inside one 2 MiB block: stop\n");
		return -1;
	}
	smmu_l1[lo >> 30] = (uintptr_t)smmu_l2 | PTE_TABLE;
	smmu_l2[(lo >> 21) & 511] = lo | PTE_BLOCK_RW_NC;
	__asm__ volatile("dsb sy" ::: "memory");
	lv("l1 pa", (uint32_t)(uintptr_t)smmu_l1);
	lv("l1 entry", (uint32_t)smmu_l1[lo >> 30]);
	lv("l2 entry", (uint32_t)smmu_l2[(lo >> 21) & 511]);
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

void drg_gpu_bringup(void)
{
	con_skip();
	int rc = drg_emmc_log_open();
	con_hex2("GPU LOG", "rc=", (uint32_t)rc, "lba=", (uint32_t)drg_emmc_log_lba());
	put("DRGGPULOG v8 G1+G2a+G2b SMMU attach (numbered) + CP test\n");
	lv("log open rc", (uint32_t)rc);
	g1_snapshot("G1 as ABL left it");
	if (g2a_power()) {
		put("G2a RESULT: FAIL (power-up step, see above)\n");
		con_str("G2a", "FAIL power-up");
		g1_snapshot("G1 after failure");
		flush();
		return;
	}
	g1_snapshot("G1 after power-up");
	g2a_registers();
	/*
	 * run 2: the CP start reset the phone while every SMMU stream faulted.
	 * run 5: the attach stalled its core somewhere in its first three
	 * writes. run 6: each of those writes (and CB0, SMR, TLB sync writes)
	 * completed on its own core. The attach now numbers every access
	 * (attach_step), so a stall names its register; the CP only starts
	 * after the attach reads back as written.
	 */
	if (g2b_smmu_attach() == 0) {
		g2b_cp();
		smmu_state("after cp test");
	} else {
		put("G2b RESULT: STOP (SMMU attach not verified; CP not started)\n");
		con_str("G2b", "STOP smmu attach");
	}
	put("== end\n");
	flush();
}
