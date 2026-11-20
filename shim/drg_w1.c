/*
 * W1 (research/reports/wifi-bt-plan.md): boot the modem's Q6 through the MBA
 * and get its firmware authenticated, from the shim, before XNU. The
 * firmware is read from this phone's own modem_a partition (read-only FAT16,
 * drg_fat.c); nothing of it is in the shim or in git.
 *
 * Sequence = HMD's DRG kernel (research/hmd-drg-kernel) for this node
 * (device-info/fdt.dts qcom,mss@4080000: "qcom,pil-q6v55-mss",
 * qcom,pil-self-auth, qcom,qdsp6v62-1-5, qcom,mem-protect-id 0xf,
 * qcom,minidump-id, qcom,cx-ipeak-vote, no qcom,pil-mss-memsetup):
 *   peripheral-loader.c pil_boot: modem.mdt, pil_init_mmap (relocatable
 *     region = lowest..highest loadable segment, 4 KiB rounded), proxy
 *     votes, init_image, mem_setup (none here), assign the region to
 *     {HLOS RW, MSS RW}, load each segment (.bNN, zero to memsz,
 *     verify_blob), pil_reclaim_mem({HLOS, MSS} -> MSS RW), auth_and_reset.
 *   pil-msa.c pil_mss_make_proxy_votes / pil-q6v5.c pil_q6v5_make_proxy_votes:
 *     vdd_mx level 0x180 + enable, xo, qdss, vdd_cx level 0x180, load
 *     100 mA, enable.
 *   Order deviation: pil_mss_reset's restart_reg 0 + clocks run before the
 *     FIH NV / MBA assignments (steps 8-9) so the Q6 state is checked before
 *     any memory changes hands; nothing runs until step 12 releases the core.
 *   pil-msa.c pil_mss_reset_load_mba (CONFIG_FIH_NV=y, fih_ramtable.h):
 *     FIH NV 0xAC000000 + 8 MiB -> {HLOS RW, MSS RW}; mba.mbn into a 1 MiB
 *     buffer of pil_mba_region, -> MSS RW; pil_mss_reset: restart_reg 0,
 *     2 us, 6 active clocks, RMB_MBA_IMAGE, PMI_CODE_START/LENGTH 0 (no
 *     msadp: the partition has none), __pil_q6v55_reset (pil-q6v5.c,
 *     qdsp6v62_1_5 branch), PBL then MBA status (5 s each: minidump-id
 *     sets MBA_ENCRYPTION_TIMEOUT).
 *   pil_msa_auth_modem_mdt: whole .mdt into a buffer of the same region,
 *     -> MSS RW (4 KiB rounded), CODE_LENGTH 0, META_DATA, COMMAND 1, wait
 *     status 3 (10 s), buffer back to HLOS RWX.
 *   pil_msa_mba_verify_blob per segment; pil_msa_mba_auth: wait status 4
 *     (10 s), MBA buffer back to HLOS RWX.
 *   Error paths as pil_boot / __pil_mss_deinit_image / pil_mss_shutdown.
 * TZ: one SCM function only, hyp_assign (secure_buffer.c hyp_assign_table,
 * scm.c __scm_call2): x0 0x42000C16, x1 0x1117 (SCM_ARGS(7, RO, VAL, RO,
 * VAL, RO, VAL, VAL)), x2 = {u64 addr, u64 size}, x3 = 16, x4 = u32 src
 * VMIDs, x5 = extra args {src bytes, dest list, dest bytes, 0, 0, 0, 0};
 * dest entry {u32 vm, u32 perm, u64 ctx 0, u32 ctx_size 0} (24 bytes,
 * natural alignment: the struct is not packed). VMID_HLOS 3, VMID_MSS_MSA
 * 0xF, PERM R 4 / W 2 / X 1 (HMD include/soc/qcom/secure_buffer.h). Every
 * SMC runs on a spare Gold core (drg_probe_call): a call that never
 * returns costs that core, not the boot.
 * RPM keys (rpm-smd-regulator.c params[], DT rpm-regulator-smpb3/smpb5):
 * vdd_cx = resource "rwcx" id 0, vdd_mx = "rwmx" id 0, both
 * qcom,use-voltage-level -> key "vlvl"; enable "swen"; load "ma". XO =
 * rpmcc index 0 = cxo, "clk0" id 0 key "Enab" (clk-smd-rpm.c sdm660).
 *
 * Deviations, all logged: the QDSS proxy vote is not sent (its RPM value
 * is a clock rate the source does not pin down); CX/MX/XO votes are left
 * in place afterwards (never lower CX/MX under a running CPU; XNU's GPU
 * driver re-votes CX/MX); after authentication the modem is watched and
 * then stopped as pil_mss_shutdown does (unless built with
 * -DDRG_W1_KEEP_RUNNING), because nothing under XNU would serve it and a
 * Q6 watchdog bite or error fatal must not take the boot down [E]. Memory
 * the modem owns at that point stays its (XNU never maps the low bank).
 *
 * W2 (boot #56 proved W1 up to status 4): the watch is 30 s instead of 5 s
 * and reads SMEM read-only (drg_smem.c, HMD msm_smem.c) once per second:
 * the modem's outbound SMP2P item (SMEM_SMP2P_MODEM_BASE + apps pid 0 =
 * 435, apps<->modem partition; entry "slave-kernel" per the DT smp2pgpio
 * ssr-smp2p-1-in node; the mss node's qcom,gpio-* bits are not in this DT
 * excerpt [E: 0 err-fatal, 1 err-ready, 2 proxy-unvote, 3 stop-ack as the
 * lpass node]), the failure reason SMEM_SSR_REASON_MSS0 (421,
 * SMEM_ANY_HOST_FLAG, as pil-q6v5-mss.c log_modem_sfr, never cleared
 * here), the GLINK SMEM transport items 478/479/480 in the apps<->modem
 * partition (glink_smem_native_xprt.c: the descriptor and FIFO 0 are
 * allocated by apps, which nothing does here, so the modem's GLINK edge
 * cannot come up), RMB status/debug and the wdog SPI pending bit; a change
 * is logged, then the full state once more before the shutdown. Log lines
 * are packed (boot #56's W1 used 259 lines of the 32 KiB log).
 *
 * Safety: assignments only inside modem_fw_region, pil_mba_region and the
 * FIH NV region, all below 4 GiB; a Q6 found running (step 9: headswitch
 * on, core out of reset, core clock on) is left alone, and RMB status left
 * from an earlier boot is cleared with the restart cycle Linux uses on a
 * subsystem restart (the restart register itself reads 0 after XBL/ABL,
 * boot #55, and Linux's cold boot does not assert it); every wait has a timeout; the first access to each MSS
 * register block and to each RAM target runs on a spare core (a bus error
 * or a stall there is caught); no eMMC writes (drg_emmc_read only).
 */
#include <stdint.h>
#include "drg_w1.h"
#include "drg_fat.h"
#include "drg_part.h"
#include "drg_smem.h"
#include "drg_rpm.h"
#include "drg_gpu.h"
#include "emmc.h"
#include "console.h"

#define GCC			0x00100000UL
#define MSS_RESTART		(GCC + 0x79000)		/* restart_reg 0x179000 */
#define QDSP6			0x04080000UL		/* qdsp6_base */
#define RMB			0x04180000UL		/* rmb_base */
#define HALT_Q6			0x01f63000UL
#define HALT_MODEM		0x01f65000UL
#define HALT_NC			0x01f64000UL
#define CXIP_LM_VOTE_CLEAR	0x01fe5048UL
#define GICD			0x17a00000UL
#define MSS_WDOG_INTID		(32 + 0x1c0)		/* DT interrupts <0 0x1c0 1> */

/* QDSP6SS (pil-q6v5.c) */
#define Q6_RESET		0x014
#define Q6_GFMUX		0x020
#define Q6_PWR_CTL		0x030
#define Q6_MEM_PWR_CTL		0x034	/* QDSP6V6SS_MEM_PWR_CTL */
#define Q6_XO_CBCR		0x038
#define Q6_BHS_STATUS		0x0c4	/* QDSP6V62SS_BHS_STATUS */

/* RMB (pil-msa.c) */
#define RMB_MBA_IMAGE		0x00
#define RMB_PBL_STATUS		0x04
#define RMB_MBA_COMMAND		0x08
#define RMB_MBA_STATUS		0x0c
#define RMB_PMI_META_DATA	0x10
#define RMB_PMI_CODE_START	0x14
#define RMB_PMI_CODE_LENGTH	0x18
#define RMB_PROTOCOL_VERSION	0x1c
#define RMB_MBA_DEBUG_INFO	0x20

/* reserved memory (device-info/fdt.dts reserved-memory, fih_ramtable.h) */
#define MODEM_FW_BASE		0x8ac00000UL
#define MODEM_FW_SIZE		0x07e00000UL
#define MBA_REGION		0x94800000UL
#define MBA_REGION_SIZE		0x00200000UL
#define MBA_BUF			MBA_REGION		/* mba_dp_size = SZ_1M, first allocation [E] */
#define MBA_BUF_SIZE		0x00100000UL
#define META_BUF		(MBA_REGION + 0x100000)	/* next allocation in the same pool [E] */
#define FIH_NV			0xac000000UL
#define FIH_NV_SIZE		0x00800000UL		/* NV_RF 2 MiB + NV_CUST 2 MiB + NV_DEFAULT 4 MiB */

#define VMID_HLOS		0x3
#define VMID_MSS_MSA		0xf
#define PERM_R			4
#define PERM_W			2
#define PERM_X			1

#define SCM_HYP_ASSIGN		0x42000c16UL	/* SMC64 | SCM_SIP_FNID(SCM_SVC_MP 0xC, MEM_PROT_ASSIGN_ID 0x16) */
#define SCM_HYP_ASSIGN_ARGS	0x1117UL
#define SCM_INTERRUPTED		1
#define SCM_V2_EBUSY		(-12)

#define RPM_RES_CLK0		0x306b6c63u	/* "clk0" QCOM_SMD_RPM_MISC_CLK */
#define RPM_KEY_ENAB		0x62616e45u	/* "Enab" QCOM_RPM_SMD_KEY_ENABLE */
#define RPM_KEY_MA		0x0000616du	/* "ma" */
#define LEVEL_TURBO		0x180		/* vdd_cx-voltage / vdd_mx-uV */

#define PBL_MBA_TIMEOUT_MS	5000	/* MBA_ENCRYPTION_TIMEOUT (qcom,minidump-id present) */
#define AUTH_TIMEOUT_MS		10000	/* modem_auth_timeout_ms */
#define WATCH_MS		30000	/* W2 */
#define WATCH_STEP_MS		1000

#define MAX_PHDR		64

static drg_log_fn logp;
static drg_flush_fn flushp;

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

#ifndef DRG_HOST_SIM
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t now(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

static uint64_t freq(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}
#else	/* host simulation (shim/test/hwtest_sim.c): timer and SMC from the harness */
uint64_t sim_now(void);
uint64_t sim_freq(void);
void sim_smc(volatile uint64_t *x);
static inline void dsb(void) { __sync_synchronize(); }
static uint64_t now(void) { return sim_now(); }
static uint64_t freq(void) { return sim_freq(); }
#endif

static void udelay(uint32_t us)
{
	uint64_t t0 = now(), n = (freq() * us + 999999) / 1000000;
	while (now() - t0 < n)
		;
}

static uint32_t ms_since(uint64_t t0)
{
	return (uint32_t)((now() - t0) * 1000 / freq());
}

static void L(const char *label, uint32_t a, uint32_t v) { logp(label, a, v, 0); }
static void LX(const char *label, uint32_t a, uint32_t v) { logp(label, a, v, -1); }
static void flush(void) { flushp(); }

/* ---- spare cores (shim.c drg_probe_*): Gold 1-3 = indices 5-7, as drg_gpu.c ---- */

static int sp_rr, sp_dead[8];

/* 0 ok; -1 stalled (core given up), -3 fault (*esr), -4 never started, -2 no core would start */
static int on_spare(void (*fn)(void), uintptr_t addr, uint32_t *val, uint64_t *esr, unsigned timeout_us)
{
	for (int attempt = 0; attempt < 9; attempt++) {
		int core = 5 + (sp_rr++ % 3);
		if (sp_dead[core])
			continue;
		uint32_t v = 0;
		*esr = 0;
		int rc = fn ? drg_probe_call(core, fn, esr, timeout_us) : drg_probe_read(core, addr, &v, esr, timeout_us);
		if (val)
			*val = v;
		if (rc == -2) {		/* CPU_ON refused (still powering off): nothing ran, try another */
			udelay(10000);
			continue;
		}
		if (rc == -1 || rc == -4)
			sp_dead[core] = 1;
		return rc;
	}
	return -2;
}

/* one register read on a spare core; 0 ok. quiet: a zero value is not logged (W2 log budget) */
static int safe_rd_q(const char *label, uintptr_t a, uint32_t *v, int quiet)
{
	uint64_t esr = 0;
	int rc = on_spare(0, a, v, &esr, 100000);
	if (rc) {
		LX(label, (uint32_t)a, (uint32_t)rc);
		LX("w1 probe esr", (uint32_t)a, (uint32_t)esr);
	} else if (!quiet || *v) {
		L(label, (uint32_t)a, *v);
	}
	return rc;
}

static int safe_rd(const char *label, uintptr_t a, uint32_t *v) { return safe_rd_q(label, a, v, 0); }

/* RAM targets: one write + read back on a spare core */
static volatile uintptr_t touch_addr;
static volatile uint32_t touch_back;

static void touch_fn(void)
{
	volatile uint32_t *p = (volatile uint32_t *)touch_addr;
	*p = 0x57315731u;	/* "W1W1" */
	dsb();
	touch_back = *p;
}

static int touch(uintptr_t a)
{
	uint64_t esr = 0;
	touch_addr = a;
	touch_back = 0;
	dsb();
	int rc = on_spare(touch_fn, 0, 0, &esr, 100000);
	if (rc || touch_back != 0x57315731u) {
		LX("w1 ram touch FAIL", (uint32_t)a, rc ? (uint32_t)rc : touch_back);
		if (rc == -3)
			LX("w1 ram touch esr", (uint32_t)a, (uint32_t)esr);
		return -1;
	}
	L("w1 ram touch ok", (uint32_t)a, touch_back);
	return 0;
}

/* ---- hyp_assign ---- */

static volatile uint64_t smc_x[7];
static volatile uint32_t smc_rounds;

#ifdef DRG_HOST_SIM
static void smc_fn(void)
{
	sim_smc(smc_x);
	smc_rounds = 1;
}
#else
static void smc_fn(void)
{
	register uint64_t r0 __asm__("x0") = smc_x[0];
	register uint64_t r1 __asm__("x1") = smc_x[1];
	register uint64_t r2 __asm__("x2") = smc_x[2];
	register uint64_t r3 __asm__("x3") = smc_x[3];
	register uint64_t r4 __asm__("x4") = smc_x[4];
	register uint64_t r5 __asm__("x5") = smc_x[5];
	register uint64_t r6 __asm__("x6") = 0;
	uint32_t n = 0;
	do {	/* scm.c __scm_call_armv8_64: re-entered with its own results while SCM_INTERRUPTED */
		__asm__ volatile("smc #0"
				 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
				 :
				 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		smc_rounds = ++n;
	} while (r0 == SCM_INTERRUPTED && n < 100000);
	smc_x[0] = r0, smc_x[1] = r1, smc_x[2] = r2, smc_x[3] = r3;
}
#endif

/* the hypervisor reads these by physical address (MMU off: CPU address = PA) */
static volatile uint64_t mp_info[2] __attribute__((aligned(64)));	/* struct mem_prot_info */
static volatile uint32_t src_vm[4] __attribute__((aligned(64)));
static volatile uint32_t dst_vm[12] __attribute__((aligned(64)));	/* 2 x struct dest_vm_and_perm_info */
static volatile uint64_t ext_args[8] __attribute__((aligned(64)));	/* struct scm_extra_arg args64[7] */

static int allowed(uint64_t a, uint64_t s)
{
	if (!s || a + s < a || a + s > 0x100000000ULL || (a & 0xfff) || (s & 0xfff))
		return 0;
	return (a >= MODEM_FW_BASE && a + s <= MODEM_FW_BASE + MODEM_FW_SIZE) ||
	       (a >= MBA_REGION && a + s <= MBA_REGION + MBA_REGION_SIZE) ||
	       (a >= FIH_NV && a + s <= FIH_NV + FIH_NV_SIZE);
}

static int fault_core_lost;	/* a TZ call never returned: no more SMCs */

/*
 * hyp_assign_phys(addr, size, src[], dest[], perm[]) as one SCM call; 0 ok.
 * src/dest: VMID bitmask style lists given as up to two entries.
 */
static int assign(const char *label, uint64_t a, uint64_t s, uint32_t src0, uint32_t src1, uint32_t d0,
		  uint32_t p0, uint32_t d1, uint32_t p1)
{
	uint32_t nsrc = src1 ? 2 : 1, ndst = d1 ? 2 : 1;
	logp(label, (uint32_t)a, (uint32_t)s, 0);
	L(" src01/dst0p0d1p1", src0 << 8 | src1, d0 << 24 | p0 << 16 | d1 << 8 | p1);	/* packed (W2) */
	if (!allowed(a, s)) {
		LX("w1 asg REFUSED range", (uint32_t)a, (uint32_t)s);
		return -1;
	}
	if (fault_core_lost) {
		LX("w1 asg REFUSED core lost", 0, 0);
		return -1;
	}
	uintptr_t bufs[4] = { (uintptr_t)mp_info, (uintptr_t)src_vm, (uintptr_t)dst_vm, (uintptr_t)ext_args };
	for (int i = 0; i < 4; i++)
		if (bufs[i] >> 32) {
			LX("w1 asg buffer >4G", (uint32_t)(bufs[i] >> 32), (uint32_t)bufs[i]);
			return -1;
		}
	mp_info[0] = a;
	mp_info[1] = s;
	src_vm[0] = src0;
	src_vm[1] = src1;
	for (int i = 0; i < 12; i++)
		dst_vm[i] = 0;
	dst_vm[0] = d0, dst_vm[1] = p0;		/* vm, perm; ctx (u64) and ctx_size stay 0 */
	dst_vm[6] = d1, dst_vm[7] = p1;		/* second entry at +24 */
	for (int i = 0; i < 8; i++)
		ext_args[i] = 0;
	ext_args[0] = 4 * nsrc;			/* args[3]: source list bytes */
	ext_args[1] = (uintptr_t)dst_vm;	/* args[4] */
	ext_args[2] = 24 * ndst;		/* args[5]: dest list bytes */
	dsb();
	for (unsigned tries = 0;; tries++) {
		smc_x[0] = SCM_HYP_ASSIGN;
		smc_x[1] = SCM_HYP_ASSIGN_ARGS;
		smc_x[2] = (uintptr_t)mp_info;	/* args[0] */
		smc_x[3] = 16;			/* args[1]: one mem_prot_info */
		smc_x[4] = (uintptr_t)src_vm;	/* args[2] */
		smc_x[5] = (uintptr_t)ext_args;	/* x5: extra argument buffer */
		smc_rounds = 0;
		dsb();
		uint64_t esr = 0, t0 = now();
		int rc = on_spare(smc_fn, 0, 0, &esr, 3000000);
		uint32_t us = (uint32_t)((now() - t0) * 1000000 / freq());
		if (rc) {
			LX("w1 smc core rc/rounds", (uint32_t)rc, smc_rounds);
			LX("w1 smc esr", 0, (uint32_t)esr);
			if (rc != -3)
				fault_core_lost = 1;
			return -1;
		}
		int64_t x0 = (int64_t)smc_x[0];
		logp(" smc x0/us", (uint32_t)x0, us, x0 < 0 ? -1 : 0);
		if (x0 || smc_x[1] || smc_x[2] || smc_x[3] || smc_rounds != 1) {	/* anything but a plain success */
			L(" smc x1/x2", (uint32_t)smc_x[1], (uint32_t)smc_x[2]);
			L(" smc x3/rounds", (uint32_t)smc_x[3], smc_rounds);
		}
		if (x0 == SCM_V2_EBUSY && tries < 67) {	/* scm.c: 30 ms, up to 67 retries */
			udelay(30000);
			continue;
		}
		return x0 < 0 ? -1 : 0;	/* __scm_call2: only a negative x0 is an error */
	}
}

/* ---- clocks (gcc-sdm660.c, clk-branch.c clk_branch2_check_halt) ---- */

enum { CK_BRANCH, CK_VOTED, CK_GATE };
struct clk_desc {
	uint32_t en_reg, en_bit, halt_reg, kind;
};
/* qcom,active-clock-names order: iface, bus, mem, gpll0_mss, snoc_axi, mnoc_axi */
static const struct clk_desc mss_clks[6] = {
	{ 0x8a000, 0, 0x8a000, CK_BRANCH },	/* gcc_mss_cfg_ahb_clk */
	{ 0x4401c, 0, 0x4401c, CK_BRANCH },	/* gcc_bimc_mss_q6_axi_clk */
	{ 0x52004, 10, 0x38004, CK_VOTED },	/* gcc_boot_rom_ahb_clk */
	{ 0x5200c, 2, 0, CK_GATE },		/* gpll0_out_msscc (clk_gate2, udelay 1) */
	{ 0x8a03c, 0, 0x8a03c, CK_BRANCH },	/* gcc_mss_snoc_axi_clk */
	{ 0x8a004, 0, 0x8a004, CK_BRANCH },	/* gcc_mss_mnoc_bimc_axi_clk */
};
static uint32_t clk_was_on[6];
static int clks_on;

static int branch_ok(uint32_t v)
{
	uint32_t m = v & (0x80000000u | (7u << 28));
	return !(m & 0x80000000u) || m == (2u << 28);
}

static int clk_enable(int i)
{
	const struct clk_desc *c = &mss_clks[i];
	uintptr_t en = GCC + c->en_reg;
	wr32(en, rd32(en) | (1u << c->en_bit));
	dsb();
	if (c->kind == CK_GATE) {
		udelay(1);
		L("w1 clk gate", c->en_reg, rd32(en));
		return 0;
	}
	if (c->kind == CK_VOTED)
		udelay(5);
	uint32_t v = 0;
	for (int k = 0; k < 200; k++) {
		v = rd32(GCC + c->halt_reg);
		if (branch_ok(v)) {
			L("w1 clk on", c->halt_reg, v);
			return 0;
		}
		udelay(1);
	}
	LX("w1 clk STUCK OFF", c->halt_reg, v);
	return -1;
}

/* back to what each enable bit was before (pil_mss_disable_clks, but never off what was on) */
static void clks_restore(void)
{
	for (int i = 5; i >= 0; i--) {
		const struct clk_desc *c = &mss_clks[i];
		uintptr_t en = GCC + c->en_reg;
		if (!clk_was_on[i])
			wr32(en, rd32(en) & ~(1u << c->en_bit));
		dsb();
		L("w1 clk restore", c->en_reg, rd32(en));
	}
	clks_on = 0;
}

/* ---- MSS state, error paths ---- */

static int mba_assigned, q6_booted, cxip_ok;

static void rmb_dump(void)
{
	for (uint32_t o = 0; o <= RMB_MBA_DEBUG_INFO; o += 4)
		L("w1 rmb", o, rd32(RMB + o));
}

static void halt_port(uintptr_t base)
{
	wr32(base + 0, 1);	/* AXI_HALTREQ */
	dsb();
	uint32_t ack = 0;
	uint64_t t0 = now();
	while (ms_since(t0) < 25 && !(ack = rd32(base + 4)))	/* HALT_ACK_TIMEOUT_US 25000 */
		udelay(50);
	logp("w1 halt ack/idle", ack, rd32(base + 8), ack ? 0 : -1);
	wr32(base + 0, 0);
	dsb();
}

/* pil_mss_shutdown */
static void mss_shutdown(void)
{
	L("w1 shutdown", 0, 0);
	halt_port(HALT_Q6);
	halt_port(HALT_MODEM);
	halt_port(HALT_NC);
	if (cxip_ok)
		wr32(CXIP_LM_VOTE_CLEAR, 1u << 5);	/* CX_IPEAK_MSS (qcom,cx-ipeak-vote) */
	else
		L("w1 cxip clear skipped", 0, 0);
	wr32(MSS_RESTART, 1);
	dsb();
	udelay(2);
	L("w1 restart reg", MSS_RESTART, rd32(MSS_RESTART));
	if (clks_on)
		clks_restore();
	q6_booted = 0;
}

static void mba_reclaim(void)
{
	if (!mba_assigned)
		return;
	if (!assign("w1 asg mba->hlos", MBA_BUF, MBA_BUF_SIZE, VMID_MSS_MSA, 0, VMID_HLOS,
		    PERM_R | PERM_W | PERM_X, 0, 0))
		mba_assigned = 0;
}

/* __pil_mss_deinit_image(err_path = true) */
static void deinit_err(void)
{
	wr32(RMB + RMB_MBA_COMMAND, 0xffffdead);	/* CMD_PILFAIL_NFY_MBA */
	dsb();
	int32_t st = 0;
	uint64_t t0 = now();
	while (ms_since(t0) < PBL_MBA_TIMEOUT_MS) {
		st = (int32_t)rd32(RMB + RMB_MBA_STATUS);
		if (st == 6 || st < 0)
			break;
		udelay(50);
	}
	logp("w1 mba unlock status/ms", (uint32_t)st, ms_since(t0), st == 6 ? 0 : -1);
	mss_shutdown();
	mba_reclaim();
}

/* ---- firmware partition ---- */

static uint8_t mdt[16384] __attribute__((aligned(64)));
static struct fat16 fs;
static uint64_t part_first;

static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void segname(char *p, uint32_t num)	/* "image/modem.bNN" */
{
	const char *s = "image/modem.b";
	int i = 0;
	for (; s[i]; i++)
		p[i] = s[i];
	p[i++] = (char)('0' + num / 10 % 10);
	p[i++] = (char)('0' + num % 10);
	p[i] = 0;
}

/* CRC-32 of each file in the 2026-11-06 backup of this phone's modem_a (fat16_test + zlib) */
#define CRC_MBA		0x17e06536u
#define SIZE_MBA	238256u
#define CRC_MDT		0xc4bb86f7u
#define SIZE_MDT	8412u
static const uint32_t seg_crc[30] = {
	0x1eba24a1, 0xbf35f36c, 0x3c3879af, 0x09820544, 0xdd1c18ee, 0x6b735f0c, 0x8832d058, 0x08f9c60b,
	0xb47550d9, 0x82e6a387, 0x5c4c6c6a, 0x50845a5b, 0xafc412f8, 0x20760eb3, 0x923a7192, 0,
	0x7baf53d1, 0x93dcd606, 0x6daf7e59, 0,		0x158131aa, 0x1a66c7be, 0x46da6d8e, 0x2a9443c6,
	0x4f1f4e9e, 0xb93c7a24, 0x97aefef5, 0x1c5ab818, 0x27322e5d, 0,
};

struct seg {
	uint32_t num, pa, filesz, memsz;
	struct fat16_ent ent;
};
static struct seg segs[MAX_PHDR];
static uint32_t nsegs;

static void zero(uintptr_t a, uint32_t n)
{
	while (n && (a & 7)) {
		*(volatile uint8_t *)a = 0;
		a++, n--;
	}
	for (; n >= 8; n -= 8, a += 8)
		*(volatile uint64_t *)a = 0;
	while (n--)
		*(volatile uint8_t *)a++ = 0;
}

/*
 * W2: 30 s with the modem running. Each second: RMB PBL/status/debug, the
 * wdog SPI pending bit, SMP2P "slave-kernel", the first byte of the failure
 * reason, which GLINK items exist; lines when any of it changed (at most
 * 48), then everything in full.
 */
static void glink_state(uint32_t *mask, uint32_t *desc0, uint32_t *desc1)
{
	static const uint16_t ids[3] = { SMEM_GLINK_DESC, SMEM_GLINK_FIFO_0, SMEM_GLINK_FIFO_1 };
	*mask = 0, *desc0 = *desc1 = 0;
	for (int i = 0; i < 3; i++) {
		uint32_t sz = 0;
		uintptr_t p = drg_smem_item(ids[i], SMEM_HOST_MODEM, &sz);
		if (p)
			*mask |= 1u << i;
		if (p && i == 0 && sz >= 16) {	/* struct channel_desc tx {read, write}, rx {read, write} */
			*desc0 = rd32(p) << 16 | (rd32(p + 4) & 0xffff);
			*desc1 = rd32(p + 8) << 16 | (rd32(p + 12) & 0xffff);
		}
	}
}

static void watch(void)
{
	uint32_t last[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u }, lines = 0;
	uintptr_t pend_reg = GICD + 0x200 + 4 * (MSS_WDOG_INTID / 32);
	int smem = drg_smem_init(logp, "w2");
	(void)drg_smem_string(SMEM_SSR_REASON_MSS0, SMEM_HOST_ANY, logp, "w2 sfr start id/len");
	flush();
	uint64_t t0 = now();
	for (;;) {
		uint32_t ms = ms_since(t0);
		int found = 0;
		uint32_t cur[6];
		cur[0] = rd32(RMB + RMB_PBL_STATUS) << 16 | (rd32(RMB + RMB_MBA_STATUS) & 0xffff);
		cur[1] = rd32(RMB + RMB_MBA_DEBUG_INFO);
		cur[2] = (rd32(pend_reg) >> (MSS_WDOG_INTID % 32)) & 1;
		cur[3] = smem ? 0xdeadu : drg_smp2p_entry(SMEM_SMP2P_MODEM_BASE + 0, SMEM_HOST_MODEM, "slave-kernel", &found, 0, 0);
		if (!smem && !found)
			cur[3] = 0xffffffffu;	/* item or entry not there (yet) */
		cur[4] = smem ? 0 : drg_smem_string(SMEM_SSR_REASON_MSS0, SMEM_HOST_ANY, 0, 0);
		uint32_t gm = 0, d0 = 0, d1 = 0;
		if (!smem)
			glink_state(&gm, &d0, &d1);
		cur[5] = gm << 28 | (d1 & 0xffff) << 12 | (d0 & 0xfff);
		int changed = 0;
		for (int i = 0; i < 6; i++)
			changed |= cur[i] != last[i];
		if (changed && lines < 48) {
			logp("w2 t ms/pbl.mba", ms, cur[0], cur[2] ? -1 : 0);
			L(" dbg/smp2p", cur[1], cur[3]);
			logp(" wdog/sfr0", cur[2], cur[4], cur[4] ? -1 : 0);
			if (cur[5] != last[5])
				L(" glink items.idx", gm, cur[5]);
			lines += 3 + (cur[5] != last[5]);
			for (int i = 0; i < 6; i++)
				last[i] = cur[i];
			flush();
		}
		if (ms >= WATCH_MS)
			break;
		udelay(WATCH_STEP_MS * 1000);
	}
	L("w2 watch end ms/lines", ms_since(t0), lines);
	rmb_dump();
	if (!smem) {
		int found = 0;
		(void)drg_smp2p_entry(SMEM_SMP2P_MODEM_BASE + 0, SMEM_HOST_MODEM, "slave-kernel", &found, logp, "w2");
		(void)drg_smem_string(SMEM_SSR_REASON_MSS0, SMEM_HOST_ANY, logp, "w2 sfr end id/len");
		uint32_t gm, d0, d1;
		glink_state(&gm, &d0, &d1);
		L("w2 glink items", gm, 0);
		L("w2 glink tx r.w/rx r.w", d0, d1);
	}
	flush();
}

static int stop(int step, const char *why)
{
	LX("w1 STOP step", (uint32_t)step, 0);
	LX(why, (uint32_t)step, 0);
	con_hex2("W1", "STOP step=", (uint32_t)step, "", 0);
	flush();
	return -step;
}

static int rpm_ok(const char *label, int rc)
{
	uint32_t w[4];
	drg_rpm_last_ack(w);
	logp(label, (uint32_t)rc, w[1], rc ? -1 : 0);
	/* rpm-smd.c msm_rpm_get_error_from_ack: v0 ack {req, req_len, rsc_id, msg_len, id_ack}; req_len 12 = no error kvp */
	if (!rc && w[1] != 12)
		LX("w1 rpm NACK? len/w3", w[1], w[3]);
	return rc;
}

int drg_w1_test(drg_log_fn log, drg_flush_fn fl)
{
	logp = log;
	flushp = fl;
	mba_assigned = q6_booted = clks_on = cxip_ok = 0;
	nsegs = 0;
	L("w1 start", 0x57310001, 0);
	con_str("W1", "modem boot test");

	/*
	 * 1. state before anything is touched, logged only. Boot #55 read the
	 * restart register as 0: XBL/ABL leave the MSS block reset deasserted,
	 * and Linux does not need it asserted either (pil_mss_reset writes 0
	 * on a cold boot without asserting first; only pil_mss_shutdown writes
	 * 1). Whether the Q6 runs is decided in step 9 from QDSP6SS/RMB, which
	 * can only be read with the MSS clocks on.
	 */
	uint32_t rr = rd32(MSS_RESTART);
	L("w1 mss restart reg", MSS_RESTART, rr);
	for (int i = 0; i < 6; i++)
		L("w1 mss clk before", mss_clks[i].en_reg,
		  mss_clks[i].kind == CK_BRANCH ? rd32(GCC + mss_clks[i].en_reg)
						: (rd32(GCC + mss_clks[i].en_reg) >> mss_clks[i].en_bit) & 1);

	/* 2. modem_a from the GPT */
	uint64_t first = 0, last = 0;
	uint32_t pidx = 0;
	int rc_gpt = drg_gpt_find("modem_a", &first, &last, &pidx);
	logp("w1 gpt rc/index", (uint32_t)rc_gpt, pidx, rc_gpt ? -1 : 0);
	if (rc_gpt)
		return stop(2, "gpt");
	L("w1 modem_a first lba", (uint32_t)(first >> 32), (uint32_t)first);
	L("w1 modem_a last lba", (uint32_t)(last >> 32), (uint32_t)last);
	if (last < first || last - first + 1 > 0xffffffffULL)
		return stop(2, "gpt range");
	part_first = first;

	/* 3. FAT16 */
	fs.read = drg_part_read;
	fs.ctx = &part_first;
	int rc = fat16_mount(&fs, (uint32_t)(last - first + 1));
	logp("w1 fat mount rc/clusters", (uint32_t)rc, fs.clusters, rc ? -1 : 0);
	L("w1 fat spc/root_lba", fs.spc, fs.root_lba);
	L("w1 fat data_lba/total", fs.data_lba, fs.total_sectors);
	if (rc)
		return stop(3, "fat");

	/* 4. modem.mdt: ELF32, program headers inside, loadable segments */
	struct fat16_ent e;
	rc = fat16_lookup(&fs, "image/modem.mdt", &e);
	int32_t n = rc ? rc : fat16_read(&fs, &e, mdt, sizeof(mdt), 0);
	uint32_t crc = n > 0 ? drg_crc32(0, mdt, (uint32_t)n) : 0;
	logp("w1 mdt size/crc", (uint32_t)n, crc, n == (int32_t)SIZE_MDT && crc == CRC_MDT ? 0 : -2);
	if (n < 52)
		return stop(4, "mdt read");
	uint32_t mdt_size = (uint32_t)n;
	uint32_t phoff = g32(mdt + 28), entry = g32(mdt + 24);
	uint32_t phent = (uint32_t)(mdt[42] | mdt[43] << 8), phnum = (uint32_t)(mdt[44] | mdt[45] << 8);
	L("w1 mdt entry/phnum", entry, phnum);
	if (g32(mdt) != 0x464c457f || mdt[4] != 1 || phent != 32 || phnum == 0 || phnum > MAX_PHDR ||
	    phoff + phnum * 32 > (uint32_t)n)
		return stop(4, "mdt elf");
	uint32_t min_r = 0xffffffffu, max_r = 0, align = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = mdt + phoff + 32 * i;
		uint32_t type = g32(ph), pa = g32(ph + 12), fsz = g32(ph + 16), msz = g32(ph + 20);
		uint32_t flags = g32(ph + 24), al = g32(ph + 28);
		if (!(type == 1 && ((flags >> 24) & 7) != 2 && msz))	/* segment_is_loadable */
			continue;
		if (!(flags & (1u << 27)) || fsz > msz || (uint64_t)pa + msz > 0xffffffffULL) {	/* all relocatable */
			LX("w1 seg unsupp num/flags", i, flags);
			return stop(4, "segment");
		}
		if (pa < min_r)
			min_r = pa, align = al;
		if (pa + msz > max_r)
			max_r = pa + msz;
		struct seg *s = &segs[nsegs++];
		s->num = i, s->pa = pa, s->filesz = fsz, s->memsz = msz;
	}
	max_r = (max_r + 0xfff) & ~0xfffu;
	uint32_t rsize = max_r - min_r;
	uint32_t ralign = align > 0x400000 ? 0x400000 : 0x100000;	/* pil_alloc_region */
	uint32_t aligned = (rsize + ralign - 1) & ~(ralign - 1);
	L("w1 region min/max", min_r, max_r);
	L("w1 region size/aligned", rsize, aligned);
	if (!nsegs || aligned > MODEM_FW_SIZE || (MODEM_FW_BASE & (ralign - 1)))
		return stop(4, "region");
	/* relocate to modem_fw_region (removed-dma-pool: the first allocation starts at its base [E]) */
	int entry_ok = 0;
	for (uint32_t i = 0; i < nsegs; i++) {
		struct seg *s = &segs[i];
		s->pa = s->pa - min_r + (uint32_t)MODEM_FW_BASE;
		uint32_t ent_r = entry - min_r + (uint32_t)MODEM_FW_BASE;
		if (ent_r >= s->pa && ent_r < s->pa + s->memsz)
			entry_ok = 1;
		if (i && s->pa < segs[i - 1].pa + segs[i - 1].memsz) {	/* sorted, no overlap (pil_cmp_seg) */
			LX("w1 seg order num/pa", s->num, s->pa);
			return stop(4, "segment order");
		}
	}
	if (!entry_ok)
		return stop(4, "entry");

	/* 5. every .bNN present with its filesz (pil_load_seg checks this later; here before any side effect) */
	for (uint32_t i = 0; i < nsegs; i++) {
		struct seg *s = &segs[i];
		if (!s->filesz)
			continue;
		char name[20];
		segname(name, s->num);
		rc = fat16_lookup(&fs, name, &s->ent);
		if (rc || s->ent.size != s->filesz) {
			LX("w1 seg num/pa", s->num, s->pa);
			LX("w1 seg filesz/memsz", s->filesz, s->memsz);
			LX("w1 seg file rc/size", (uint32_t)rc, s->ent.size);
			return stop(5, "segment file");
		}
	}

	L("w1 segs/first", nsegs, segs[0].num);

	/* 6. RAM targets writable from here (spare core catches a protection fault), then mba.mbn */
	if (touch(MBA_BUF) || touch(META_BUF) || touch(MODEM_FW_BASE) || touch(MODEM_FW_BASE + rsize - 4))
		return stop(6, "ram");
	rc = fat16_lookup(&fs, "image/mba.mbn", &e);
	n = rc ? rc : fat16_read(&fs, &e, (uint8_t *)MBA_BUF, MBA_BUF_SIZE, 0);
	crc = n > 0 ? drg_crc32(0, (const volatile uint8_t *)MBA_BUF, (uint32_t)n) : 0;
	logp("w1 mba size/crc", (uint32_t)n, crc, n == (int32_t)SIZE_MBA && crc == CRC_MBA ? 0 : -2);
	if (n < 52 || rd32(MBA_BUF) != 0x464c457f)
		return stop(6, "mba read");
	flush();

	/* 7. proxy votes (pil_mss_make_proxy_votes): MX, XO, (QDSS), CX */
	{
		uint32_t mx[6] = { RPM_KEY_SWEN, 4, 1, RPM_KEY_VLVL, 4, LEVEL_TURBO };
		if (rpm_ok("w1 rpm rwmx vlvl180", drg_rpm_request_kvp(RPM_RES_RWMX, 0, mx, 6)))
			return stop(7, "mx vote");
		uint32_t xo[3] = { RPM_KEY_ENAB, 4, 1 };
		if (rpm_ok("w1 rpm xo clk0 Enab", drg_rpm_request_kvp(RPM_RES_CLK0, 0, xo, 3)))
			return stop(7, "xo vote");
		L("w1 qdss vote SKIPPED", 0, 0);
		uint32_t cx[9] = { RPM_KEY_SWEN, 4, 1, RPM_KEY_VLVL, 4, LEVEL_TURBO, RPM_KEY_MA, 4, 100 };
		if (rpm_ok("w1 rpm rwcx vlvl180 ma", drg_rpm_request_kvp(RPM_RES_RWCX, 0, cx, 9)))
			return stop(7, "cx vote");
	}

	/*
	 * 8. pil_mss_reset, first half: restart_reg 0, 2 us, the 6 active
	 * clocks. Linux does this after the FIH NV / MBA assignments; it is
	 * done first here (independent steps: nothing runs before the Q6 core
	 * reset is released in step 12) so that step 9 can look at the Q6
	 * before any memory changes hands.
	 */
	wr32(MSS_RESTART, 0);
	dsb();
	udelay(2);
	L("w1 restart reg now", MSS_RESTART, rd32(MSS_RESTART));
	for (int i = 0; i < 6; i++)	/* all six first: a failed enable must not make clks_restore turn off one never touched */
		clk_was_on[i] = (rd32(GCC + mss_clks[i].en_reg) >> mss_clks[i].en_bit) & 1;
	clks_on = 1;
	for (int i = 0; i < 6; i++)
		if (clk_enable(i)) {
			wr32(MSS_RESTART, 1);
			dsb();
			clks_restore();
			return stop(8, "clock");
		}

	/* 9. dry read of every QDSP6SS / RMB / halt register this test touches, on a spare core; is the Q6 running? */
	{
		static const uint16_t q6r[6] = { Q6_RESET, Q6_GFMUX, Q6_PWR_CTL, Q6_MEM_PWR_CTL, Q6_XO_CBCR, Q6_BHS_STATUS };
		uint32_t v, q6v[6] = { 0 }, rmbv[9] = { 0 };
		int bad = 0;
		for (int i = 0; i < 6 && !bad; i++)
			bad = safe_rd("w1 q6ss", QDSP6 + q6r[i], &q6v[i]);
		for (uint32_t o = 0; o <= RMB_MBA_DEBUG_INFO && !bad; o += 4)
			bad = safe_rd_q("w1 rmb (dry) !=0", RMB + o, &rmbv[o / 4], 1);
		uintptr_t hb[3] = { HALT_Q6, HALT_MODEM, HALT_NC };
		for (int i = 0; i < 3 && !bad; i++)
			for (uint32_t o = 0; o <= 8 && !bad; o += 4)
				bad = safe_rd_q("w1 halt (dry) !=0", hb[i] + o, &v, 1);
		/* the CX ipeak vote-clear register is only written on shutdown: skip it there if unreadable */
		cxip_ok = !bad && !safe_rd("w1 cxip (dry)", CXIP_LM_VOTE_CLEAR, &v);
		L("w1 dry reads bad", (uint32_t)bad, 0);
		if (bad) {
			wr32(MSS_RESTART, 1);
			dsb();
			clks_restore();
			return stop(9, "dry read");
		}
		/*
		 * Running = what __pil_q6v55_reset leaves behind: headswitch on
		 * (PWR_CTL bit 24), CORE_ARES and STOP_CORE clear (RESET bits 1, 0),
		 * core clock on (GFMUX bit 1). Such a Q6 is left alone: no restart
		 * write, clocks back as they were (they were on).
		 */
		int running = (q6v[2] & (1u << 24)) && !(q6v[0] & 3u) && (q6v[1] & 2u);
		logp("w1 q6 running?", q6v[0], q6v[1], running ? -1 : 0);
		if (running) {
			clks_restore();
			return stop(9, "q6 running");
		}
		/*
		 * PBL/MBA status left over (a modem booted before, then stopped
		 * without the restart): the restart cycle of pil_mss_shutdown ->
		 * pil_mss_reset (halt ports, restart 1, then 0), as Linux does on a
		 * subsystem restart. The 1 ms hold is a guess [E]; Linux holds it
		 * for at least the ramdump/powerup path between the two writes.
		 */
		if (rmbv[RMB_PBL_STATUS / 4] || rmbv[RMB_MBA_STATUS / 4]) {
			LX("w1 rmb stale pbl/mba", rmbv[RMB_PBL_STATUS / 4], rmbv[RMB_MBA_STATUS / 4]);
			halt_port(HALT_Q6);
			halt_port(HALT_MODEM);
			halt_port(HALT_NC);
			wr32(MSS_RESTART, 1);
			dsb();
			udelay(1000);
			wr32(MSS_RESTART, 0);
			dsb();
			udelay(2);
			uint32_t pbl = rd32(RMB + RMB_PBL_STATUS), st = rd32(RMB + RMB_MBA_STATUS);
			logp("w1 rmb after cycle", pbl, st, pbl || st ? -1 : 0);
			if (pbl || st) {
				wr32(MSS_RESTART, 1);
				dsb();
				clks_restore();
				return stop(9, "rmb not clear");
			}
		}
	}

	/* 10. FIH NV -> {HLOS RW, MSS RW} (pil_assign_mem_to_subsys_and_linux) */
	if (assign("w1 asg fih nv", FIH_NV, FIH_NV_SIZE, VMID_HLOS, 0, VMID_HLOS, PERM_R | PERM_W, VMID_MSS_MSA,
		   PERM_R | PERM_W)) {
		wr32(MSS_RESTART, 1);	/* undo step 8, as pil_mss_reset err_clks */
		dsb();
		clks_restore();
		return stop(10, "fih assign");
	}

	/* 11. MBA buffer -> MSS RW (pil_assign_mem_to_subsys) */
	if (assign("w1 asg mba", MBA_BUF, MBA_BUF_SIZE, VMID_HLOS, 0, VMID_MSS_MSA, PERM_R | PERM_W, 0, 0)) {
		wr32(MSS_RESTART, 1);
		dsb();
		clks_restore();
		return stop(11, "mba assign");
	}
	mba_assigned = 1;

	/* 12. RMB image address, no debug policy, Q6 out of reset (__pil_q6v55_reset), PBL and MBA */
	wr32(RMB + RMB_MBA_IMAGE, (uint32_t)MBA_BUF);
	dsb();
	wr32(RMB + RMB_PMI_CODE_START, 0);
	wr32(RMB + RMB_PMI_CODE_LENGTH, 0);
	dsb();
	L("w1 rmb image", RMB + RMB_MBA_IMAGE, rd32(RMB + RMB_MBA_IMAGE));
	flush();
	{
		int fail = 0;
		uint32_t val = rd32(QDSP6 + Q6_RESET);
		wr32(QDSP6 + Q6_RESET, val | 0x7);	/* CORE_ARES | BUS_ARES_ENA | STOP_CORE */
		L("w1 q6 reset", Q6_RESET, rd32(QDSP6 + Q6_RESET));
		wr32(QDSP6 + Q6_XO_CBCR, rd32(QDSP6 + Q6_XO_CBCR) | 1);	/* q6v55_branch_clk_enable */
		int k;
		for (k = 0; k < 200 && (rd32(QDSP6 + Q6_XO_CBCR) & 0x80000000u); k++)
			udelay(1);
		logp("w1 q6 xo cbcr", Q6_XO_CBCR, rd32(QDSP6 + Q6_XO_CBCR), k < 200 ? 0 : -1);
		fail = k >= 200;
		if (!fail) {
			val = rd32(QDSP6 + Q6_PWR_CTL) | (1u << 24);	/* QDSP6v55_BHS_ON */
			wr32(QDSP6 + Q6_PWR_CTL, val);
			dsb();
			udelay(1);
			for (k = 0; k < 200 && !(rd32(QDSP6 + Q6_BHS_STATUS) & 1); k++)
				udelay(1);
			logp("w1 q6 bhs status", Q6_BHS_STATUS, rd32(QDSP6 + Q6_BHS_STATUS), k < 200 ? 0 : -1);
			fail = k >= 200;
		}
		if (!fail) {
			val |= 1u << 25;	/* QDSP6v55_LDO_BYP */
			wr32(QDSP6 + Q6_PWR_CTL, val);
			val = rd32(QDSP6 + Q6_PWR_CTL) & ~(1u << 22);	/* CLAMP_QMC_MEM off */
			wr32(QDSP6 + Q6_PWR_CTL, val);
			val |= (1u << 18) | (1u << 19);	/* L2DATA_STBY_N | SLP_RET_N */
			wr32(QDSP6 + Q6_PWR_CTL, val);
			L("w1 q6 pwr_ctl", Q6_PWR_CTL, rd32(QDSP6 + Q6_PWR_CTL));
			val = rd32(QDSP6 + Q6_MEM_PWR_CTL);
			for (int i = 29; i >= 0; i--) {	/* qdsp6v62_1_5: bits 29..0, one at a time */
				val |= 1u << i;
				wr32(QDSP6 + Q6_MEM_PWR_CTL, val);
				val = rd32(QDSP6 + Q6_MEM_PWR_CTL);
				udelay(1);
			}
			L("w1 q6 mem_pwr_ctl", Q6_MEM_PWR_CTL, val);
			val = rd32(QDSP6 + Q6_PWR_CTL) & ~(1u << 21);	/* CLAMP_WL off */
			wr32(QDSP6 + Q6_PWR_CTL, val);
			val &= ~(1u << 20);	/* CLAMP_IO off */
			wr32(QDSP6 + Q6_PWR_CTL, val);
			L("w1 q6 pwr_ctl clamps off", Q6_PWR_CTL, rd32(QDSP6 + Q6_PWR_CTL));
			val = rd32(QDSP6 + Q6_RESET) & ~0x3u;	/* CORE_ARES | STOP_CORE off */
			wr32(QDSP6 + Q6_RESET, val);
			val = rd32(QDSP6 + Q6_GFMUX) | (1u << 1);	/* Q6SS_CLK_ENA */
			wr32(QDSP6 + Q6_GFMUX, val);
			dsb();
			L("w1 q6 reset/gfmux", rd32(QDSP6 + Q6_RESET), rd32(QDSP6 + Q6_GFMUX));
			q6_booted = 1;
		}
		/* pil_msa_wait_for_mba_ready */
		uint32_t st = 0;
		uint64_t t0 = now();
		if (!fail) {
			while (ms_since(t0) < PBL_MBA_TIMEOUT_MS && !(st = rd32(RMB + RMB_PBL_STATUS)))
				udelay(50);
			logp("w1 pbl status/ms", st, ms_since(t0), st == 1 ? 0 : -1);
			fail = st != 1;
		}
		if (!fail) {
			t0 = now();
			while (ms_since(t0) < PBL_MBA_TIMEOUT_MS && !(st = rd32(RMB + RMB_MBA_STATUS)))
				udelay(50);
			logp("w1 mba status/ms", st, ms_since(t0), st == 1 || st == 2 ? 0 : -1);
			fail = st != 1 && st != 2;
		}
		if (fail) {	/* pil_mss_reset err_q6v5_reset, then pil_mss_reset_load_mba err_mss_reset */
			rmb_dump();
			clks_restore();
			wr32(MSS_RESTART, 1);
			dsb();
			q6_booted = 0;
			mba_reclaim();
			return stop(12, "pbl/mba");
		}
	}
	L("w1 MBA boot done", 0, rd32(RMB + RMB_MBA_STATUS));
	con_str("W1", "MBA running");
	flush();

	/* 13. pil_msa_auth_modem_mdt: the whole .mdt, 4 KiB-rounded assignment, status 3 */
	{
		volatile uint8_t *m = (volatile uint8_t *)META_BUF;
		for (uint32_t i = 0; i < mdt_size; i++)
			m[i] = mdt[i];
		dsb();
		uint32_t msz = (mdt_size + 0xfff) & ~0xfffu;
		if (assign("w1 asg mdt", META_BUF, msz, VMID_HLOS, 0, VMID_MSS_MSA, PERM_R | PERM_W, 0, 0)) {
			rmb_dump();	/* pil_msa_auth_modem_mdt fail: */
			mss_shutdown();
			mba_reclaim();
			return stop(13, "mdt assign");
		}
		wr32(RMB + RMB_PMI_CODE_LENGTH, 0);
		wr32(RMB + RMB_PMI_META_DATA, (uint32_t)META_BUF);
		wr32(RMB + RMB_MBA_COMMAND, 1);	/* CMD_META_DATA_READY */
		dsb();
		int32_t st = 0;
		uint64_t t0 = now();
		while (ms_since(t0) < AUTH_TIMEOUT_MS) {
			st = (int32_t)rd32(RMB + RMB_MBA_STATUS);
			if (st == 3 || st < 0)
				break;
			udelay(50);
		}
		int ok = st == 3;
		logp("w1 mdt auth status/ms", (uint32_t)st, ms_since(t0), ok ? 0 : -1);
		/* back to HLOS RWX whatever the result */
		int back = assign("w1 asg mdt->hlos", META_BUF, msz, VMID_MSS_MSA, 0, VMID_HLOS, PERM_R | PERM_W | PERM_X, 0,
				  0);
		if (!ok || back) {
			rmb_dump();
			mss_shutdown();
			mba_reclaim();
			return stop(13, "mdt auth");
		}
	}
	flush();

	/* 14. no mem_setup (no qcom,pil-mss-memsetup); region -> {HLOS RW, MSS RW} */
	L("w1 mem_setup none (dt)", 0, 0);
	uint32_t region = (uint32_t)MODEM_FW_BASE;
	if (assign("w1 asg region", region, rsize, VMID_HLOS, 0, VMID_HLOS, PERM_R | PERM_W, VMID_MSS_MSA,
		   PERM_R | PERM_W)) {
		deinit_err();
		return stop(14, "region assign");
	}

	/* 15. segments: .bNN into place, zero to memsz, verify_blob */
	{
		uint64_t t0 = now();
		uint32_t bad_crc = 0;
		for (uint32_t i = 0; i < nsegs; i++) {
			struct seg *s = &segs[i];
			int32_t got = 0;
			uint32_t c = 0;
			if (s->filesz) {
				got = fat16_read(&fs, &s->ent, (uint8_t *)(uintptr_t)s->pa, s->filesz, 0);
				c = got > 0 ? drg_crc32(0, (const volatile uint8_t *)(uintptr_t)s->pa, (uint32_t)got) : 0;
			}
			if (got != (int32_t)s->filesz) {
				LX("w1 load num/rc", s->num, (uint32_t)got);
				deinit_err();
				assign("w1 asg region->hlos", region, rsize, VMID_HLOS, VMID_MSS_MSA, VMID_HLOS,
				       PERM_R | PERM_W | PERM_X, 0, 0);
				return stop(15, "segment read");
			}
			zero(s->pa + s->filesz, s->memsz - s->filesz);
			dsb();
			uint32_t want = s->num < 30 ? seg_crc[s->num] : 0;
			int crc_ok = !s->filesz || c == want;
			bad_crc += !crc_ok;
			uint32_t st0 = rd32(RMB + RMB_MBA_STATUS);
			/* pil_msa_mba_verify_blob(paddr, memsz) */
			uint32_t len = rd32(RMB + RMB_PMI_CODE_LENGTH);
			if (len == 0) {
				wr32(RMB + RMB_PMI_CODE_START, s->pa);
				wr32(RMB + RMB_MBA_COMMAND, 2);	/* CMD_LOAD_READY */
			}
			len += s->memsz;
			wr32(RMB + RMB_PMI_CODE_LENGTH, len);
			dsb();
			int32_t st = (int32_t)rd32(RMB + RMB_MBA_STATUS);
			/* one line per segment (W2): number, status before/after verify_blob, CRC */
			logp("w1 ld num.st0.st/crc", s->num << 24 | (st0 & 0xfff) << 12 | ((uint32_t)st & 0xfff), c,
			     st < 0 ? -1 : crc_ok ? 0 : -2);
			if (st < 0) {
				LX("w1 load length/status", len, (uint32_t)st);
				rmb_dump();
				deinit_err();
				assign("w1 asg region->hlos", region, rsize, VMID_HLOS, VMID_MSS_MSA, VMID_HLOS,
				       PERM_R | PERM_W | PERM_X, 0, 0);
				return stop(15, "verify_blob");
			}
		}
		L("w1 load done ms/bad crcs", ms_since(t0), bad_crc);
		L("w1 code length", rd32(RMB + RMB_PMI_CODE_LENGTH), 0);
	}
	flush();

	/* 16. pil_reclaim_mem: {HLOS, MSS} -> MSS RW */
	if (assign("w1 asg region->mss", region, rsize, VMID_HLOS, VMID_MSS_MSA, VMID_MSS_MSA, PERM_R | PERM_W, 0, 0)) {
		deinit_err();
		assign("w1 asg region->hlos", region, rsize, VMID_HLOS, VMID_MSS_MSA, VMID_HLOS, PERM_R | PERM_W | PERM_X,
		       0, 0);
		return stop(16, "reclaim to mss");
	}

	/* 17. pil_msa_mba_auth: status 4, MBA buffer back to HLOS */
	{
		int32_t st = 0;
		uint64_t t0 = now();
		while (ms_since(t0) < AUTH_TIMEOUT_MS) {
			st = (int32_t)rd32(RMB + RMB_MBA_STATUS);
			if (st == 4 || st < 0)
				break;
			udelay(50);
		}
		int ok = st == 4;
		logp("w1 auth status/ms", (uint32_t)st, ms_since(t0), ok ? 0 : -1);
		mba_reclaim();
		if (!ok) {	/* pil_boot err_auth_and_reset, then deinit_image */
			rmb_dump();
			assign("w1 asg region->hlos", region, rsize, VMID_MSS_MSA, 0, VMID_HLOS, PERM_R | PERM_W | PERM_X, 0,
			       0);
			deinit_err();
			return stop(17, "auth");
		}
	}
	L("w1 MODEM AUTHENTICATED", 0, 4);
	con_str("W1", "PASS: modem authenticated (status 4)");
	flush();

	/* 18. W2 watch (30 s, SMEM), then stop it again (see the header comment) */
	watch();
#ifdef DRG_W1_KEEP_RUNNING
	L("w1 left running (KEEP)", 0, 0);
#else
	mss_shutdown();
	L("w1 stopped; mss region", region, rsize);
#endif
	flush();
	return 0;
}
