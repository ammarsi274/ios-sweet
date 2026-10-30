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
 * Addresses from this phone's device tree (device-info/fdt.dts):
 *   GCC 0x100000, GPUCC clock-controller@5065000, gdsc_gpu_cx 0x5066004
 *   (hw-ctrl 0x5066008), gdsc_gpu_gx 0x5066094 (domain-addr 0x5065130,
 *   sw-reset 0x5066090), msm_gfx_ldo 0x506e000, kgsl-3d0 0x5000000.
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
#define A5XX_CP_RB_RPTR_ADDR_LO	0x804
#define A5XX_CP_RB_RPTR		0x806
#define A5XX_CP_ME_CNTL		0x832
#define A5XX_RBBM_INT_0_STATUS	0x4e1
#define A5XX_RBBM_STATUS	0x4f5
#define A5XX_RBBM_STATUS3	0x530
#define A5XX_VBIF_VERSION	0x3000

#define CBCR_CLK_OFF		(1U << 31)
#define CBCR_CLK_ENABLE		(1U << 0)
#define RCG_UPDATE		(1U << 0)
#define RCG_ROOT_EN		(1U << 1)
#define GDSC_PWR_ON		(1U << 31)
#define GDSC_SW_COLLAPSE	(1U << 0)
#define GDSC_HW_CONTROL		(1U << 1)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
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

void drg_gpu_bringup(void)
{
	con_skip();
	int rc = drg_emmc_log_open();
	con_hex2("GPU LOG", "rc=", (uint32_t)rc, "lba=", (uint32_t)drg_emmc_log_lba());
	put("DRGGPULOG v1 G1+G2a\n");
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
	put("== end\n");
	flush();
}
