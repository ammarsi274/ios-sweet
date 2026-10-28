/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"
#include "emmc.h"

#define RESTART_BOOTLOADER	0x77665500U

#define GICD_BASE		0x17a00000UL
#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		19	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID		18	/* DT: <GIC_PPI 2 ...> */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_midr[NR_CPUS], sec_mmfr0[NR_CPUS], sec_isar0[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

#ifdef BOARD_DRG
static void __attribute__((noreturn)) drg_gold_main(void);
static volatile uint64_t handoff_dtb;
static volatile uint32_t handoff_req, gold_alive, silver_done;
#endif

void secondary_main(uint64_t idx)
{
#ifdef BOARD_DRG
	if (idx == 4 && handoff_req)
		drg_gold_main();
#endif
	sec_midr[idx] = rsr("midr_el1");
	sec_mmfr0[idx] = rsr("id_aa64mmfr0_el1");
	sec_isar0[idx] = rsr("id_aa64isar0_el1");
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

#ifdef BOARD_DRG
/* DRG cores: cluster 0 = 4x Silver (Aff0 0..3), cluster 1 = 4x Gold (Aff1 = 1). */
static uint64_t drg_mpidr(int i)
{
	return i < 4 ? (uint64_t)i : 0x100 | (uint64_t)(i - 4);
}

/* One line per core: MIDR and MMFR0 low words (TGran16 is MMFR0[23:20]). */
static void __attribute__((unused)) test_smp_drg(void)
{
	stage = "smp";
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, drg_mpidr(i), (uintptr_t)secondary_entry, i);
	delay_ms(500);
	con_hex2("CPU0", "midr=", rsr("midr_el1"), "mmfr=", rsr("id_aa64mmfr0_el1"));
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (sec_mpidr[i])
			con_hex2(label, "midr=", sec_midr[i], "mmfr=", sec_mmfr0[i]);
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=", 0xdead0000);
	}
	con_hex2("GOLD", "tg16=", (sec_mmfr0[4] >> 20) & 0xf, "atom=", (sec_isar0[4] >> 20) & 0xf);
	con_hex2("SILVER", "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf,
		 "atom=", (rsr("id_aa64isar0_el1") >> 20) & 0xf);
	con_hex2("GICR1", "grp=", sec_probe[1], "ok=", sec_probe[3]);
}

#define PSCI_AFFINITY_INFO	0xc4000004UL
#define DRG_GOLD0		0x100UL

/*
 * Runs on Gold 0x100 after the handoff. XNU will boot from here, so report
 * the ID registers that decide which ARMv8.1+ features the kernel may use.
 */
/*
 * Read-only look at the CPU clock hardware ABL leaves (efficiency plan,
 * Phase 3). The Gold cores measured 600 MHz under XNU (boot #18). Addresses
 * from this phone's device tree (qcom,clk-cpu-osm @179c0000: osm, pwrcl_pll
 * 0x17916000, perfcl_pll 0x17816000), register offsets from HMD's DRG kernel
 * (drivers/clk/qcom/clk-cpu-osm.c): each cluster has a 0x2000 OSM block
 * (pwrcl = Silver at +0, perfcl = Gold at +0x2000), ENABLE_REG 0x1004,
 * DCVS_PERF_STATE_DESIRED_REG 0x1f10, and a 40-row table of 32-byte rows
 * with FREQ_REG 0x1154 / VOLT_REG 0x1158. The PLL's L value (PLL + 4)
 * times the 19.2 MHz XO is the PLL frequency. Nothing is written.
 */
#define DRG_OSM_BASE		0x179c0000UL
#define DRG_OSM_CLUSTER		0x2000UL
#define DRG_OSM_ENABLE		0x1004
#define DRG_OSM_DESIRED		0x1f10
#define DRG_OSM_FREQ		0x1154
#define DRG_OSM_VOLT		0x1158
#define DRG_OSM_ROW		32
#define DRG_PLL_SILVER		0x17916000UL
#define DRG_PLL_GOLD		0x17816000UL

static void drg_osm_probe(void)
{
	static const char *const cl[2] = { "OSM SIL", "OSM GOLD" };

	for (unsigned int c = 0; c < 2; c++) {
		uintptr_t b = DRG_OSM_BASE + c * DRG_OSM_CLUSTER;
		con_hex2(cl[c], "en=", rd32(b + DRG_OSM_ENABLE), "want=", rd32(b + DRG_OSM_DESIRED));
	}
	con_hex2("PLL L", "sil=", rd32(DRG_PLL_SILVER + 4), "gold=", rd32(DRG_PLL_GOLD + 4));
	/* Gold table rows were all zero in boot #19; row 0 shows if that changes. */
	con_hex2("GOLD LUT", "f=", rd32(DRG_OSM_BASE + DRG_OSM_CLUSTER + DRG_OSM_FREQ),
	    "v=", rd32(DRG_OSM_BASE + DRG_OSM_CLUSTER + DRG_OSM_VOLT));
}

/*
 * Nokia buttons, read-only, watched for 15 s so they can be pressed (boot #21).
 * From this phone's device tree: PM660 (SPMI SID 0) qcom,power-on@800 with
 * kpdpwr = power (code 0x74) and resin = volume up (code 0x73, pon_2);
 * gpio-keys vol_down = GPIO 7 of the PM660L (SID 2, qcom,qpnp-pin gpio@c600,
 * active low; boot #21 showed TLMM GPIO 7 is not it). SPMI arbiter @800f000:
 * core 0x800f000, obsrvr 0x9400000. Layout from HMD's DRG kernel
 * (drivers/spmi/spmi-pmic-arb.c v2/v3, drivers/pinctrl/qcom/pinctrl-sdm660.c):
 * APID map at core + 0x800 + 4n (PPID in bits 19:8); observer channel of
 * EE 0 at obsrvr + 0x8000 * apid with CMD +0, STATUS +8 (bit 0 done),
 * RDATA0 +0x18; command = opcode << 27 | (addr & 0xff) << 4 | (bytes - 1),
 * EXT_READL = 1. PON RT_STS is 0x810 (bit 0 kpdpwr, bit 1 resin).
 * PMIC GPIO input level: RT_STS 0x10 bit 0 (drivers/pinctrl/qcom/pinctrl-spmi-gpio.c).
 *
 * Touch (Goodix gt9xx @5d on i2c@c178000 = BLSP1 QUP4, irq TLMM GPIO 67, reset
 * GPIO 66, both NORTH tile): read-only look at what ABL leaves. GCC @100000
 * (drivers/clk/qcom/gcc-sdm660.c): blsp1_ahb_clk halt 0x17004, blsp1_qup4_i2c
 * _apps_clk 0x1f008, bit 31 = CLK_OFF. TLMM pin n: tile + 0x1000 * n (ctl),
 * +4 (io). The QUP itself is read only when both clocks are on.
 */
#define DRG_SPMI_CORE		0x0800f000UL
#define DRG_SPMI_OBS		0x09400000UL
#define DRG_PON_PPID		0x008		/* SID 0, peripheral 0x08 */
#define DRG_PON_RT_STS		0x10
#define DRG_VOLDN_PPID		0x2c6		/* SID 2, peripheral 0xc6 */
#define DRG_GPIO_RT_STS		0x10
#define DRG_GCC			0x00100000UL
#define DRG_TLMM_NORTH		(0x03000000UL + 0x00900000UL)
#define DRG_QUP4		0x0c178000UL

static int drg_spmi_read(unsigned int apid, uint8_t addr, uint8_t *val)
{
	uintptr_t ch = DRG_SPMI_OBS + 0x8000UL * apid;
	wr32(ch + 0x0, (1u << 27) | ((uint32_t)addr << 4));
	for (int i = 0; i < 100000; i++) {
		uint32_t st = rd32(ch + 0x8);
		if (st & 1) {
			if (st & 0xe)
				return -(int)st;
			*val = (uint8_t)rd32(ch + 0x18);
			return 0;
		}
	}
	return -1;
}

static unsigned int drg_spmi_apid(uint32_t ppid)
{
	for (unsigned int n = 0; n < 512; n++)
		if (((rd32(DRG_SPMI_CORE + 0x800 + 4 * n) >> 8) & 0xfff) == ppid)
			return n;
	return 0xffff;
}

static void drg_touch_probe(void)
{
	uint32_t ahb = rd32(DRG_GCC + 0x17004), qup = rd32(DRG_GCC + 0x1f008);
	con_hex2("TS CLK", "ahb=", ahb, "qup4=", qup);
	con_hex2("TS RST", "ctl=", rd32(DRG_TLMM_NORTH + 0x1000UL * 66), "io=", rd32(DRG_TLMM_NORTH + 0x1000UL * 66 + 4));
	con_hex2("TS IRQ", "ctl=", rd32(DRG_TLMM_NORTH + 0x1000UL * 67), "io=", rd32(DRG_TLMM_NORTH + 0x1000UL * 67 + 4));
	if (!(ahb & 0x80000000u) && !(qup & 0x80000000u))
		con_hex2("TS QUP", "cfg=", rd32(DRG_QUP4 + 0x0), "state=", rd32(DRG_QUP4 + 0x4));
	else
		con_str("TS QUP", "clocks off: QUP not read");
}

static void __attribute__((unused)) drg_keys_watch(void)   /* boots #21, #22 */
{
	unsigned int apid = drg_spmi_apid(DRG_PON_PPID);
	unsigned int vapid = drg_spmi_apid(DRG_VOLDN_PPID);
	con_hex2("SPMI", "ver=", rd32(DRG_SPMI_CORE), "pon-apid=", apid);
	con_hex2("SPMI", "voldn-apid=", vapid, "", 0);
	if (apid == 0xffff || vapid == 0xffff)
		return;
	con_str("KEYS", "press power / vol+ / vol- now (15 s)");
	uint8_t pon = 0, last_pon = 0xff;
	uint32_t vd, last_vd = 0xffffffff;
	uint64_t frq, t0, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	do {
		uint8_t g = 0;
		int rc = drg_spmi_read(apid, DRG_PON_RT_STS, &pon);
		if (!rc)
			rc = drg_spmi_read(vapid, DRG_GPIO_RT_STS, &g);
		vd = g & 1;
		if (rc || pon != last_pon || vd != last_vd) {
			/* pon bit0 = power, bit1 = vol+; vd 0 = vol- pressed */
			con_hex2("KEY", "pon=", rc ? (uint32_t)rc : pon, "voldn_in=", vd);
			last_pon = pon;
			last_vd = vd;
			if (rc)
				return;
		}
		delay_ms(20);
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	} while (t - t0 < 15 * frq);
}

/*
 * Touch supplies (read-only): the device tree names them only as RPM
 * resources, ldoa11 (pm660_l11, vcc_i2c) and ldob6 (pm660l_l6, vdd_ana).
 * Their regulator peripherals sit on PM660 SID 1 and PM660L SID 3. Every
 * PMIC peripheral reports TYPE at 0x04 and SUBTYPE at 0x05; an LDO is TYPE
 * 0x21. For each LDO found: EN_CTL 0x46 (bit 7 enable) and STATUS1 0x08
 * (bit 7 VREG_READY). Prints "LDO sid/periph" and en << 8 | status.
 */
static void drg_ldo_probe(void)
{
	/*
	 * Boot #24 printed no LDO at all and did not say why. Now three bitmasks
	 * per PMIC over peripherals 0x40-0x5f (bit n = peripheral 0x40 + n):
	 * mapped in the arbiter's channel table, read answered, TYPE == 0x21.
	 * Then the TYPE/SUBTYPE/EN_CTL/STATUS1 of 0x40 and 0x4a as raw samples.
	 */
	static const uint8_t sids[2] = { 1, 3 };
	for (unsigned int i = 0; i < 2; i++) {
		uint32_t mapped = 0, answered = 0, ldo = 0;
		for (unsigned int per = 0x40; per <= 0x5f; per++) {
			unsigned int apid = drg_spmi_apid(((uint32_t)sids[i] << 8) | per);
			uint8_t type = 0;
			if (apid == 0xffff)
				continue;
			mapped |= 1u << (per - 0x40);
			if (drg_spmi_read(apid, 0x04, &type))
				continue;
			answered |= 1u << (per - 0x40);
			if (type == 0x21)
				ldo |= 1u << (per - 0x40);
		}
		con_hex2(i ? "PMIC 3" : "PMIC 1", "mapped=", mapped, "answered=", answered);
		con_hex2(i ? "PMIC 3" : "PMIC 1", "type21=", ldo, "", 0);
		for (unsigned int k = 0; k < 2; k++) {
			unsigned int per = k ? 0x4a : 0x40;
			unsigned int apid = drg_spmi_apid(((uint32_t)sids[i] << 8) | per);
			uint8_t type = 0xee, sub = 0xee, en = 0xee, st = 0xee;
			if (apid != 0xffff) {
				(void)drg_spmi_read(apid, 0x04, &type);
				(void)drg_spmi_read(apid, 0x05, &sub);
				(void)drg_spmi_read(apid, 0x46, &en);
				(void)drg_spmi_read(apid, 0x08, &st);
			}
			con_hex2("PER", "sid/per=", ((uint32_t)sids[i] << 8) | per,
			    "t/s/en/st=", (uint32_t)type << 24 | (uint32_t)sub << 16 | (uint32_t)en << 8 | st);
		}
	}
}

static void __attribute__((noreturn)) drg_gold_main(void)
{
	stage = "gold";
	gold_alive = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	while (!silver_done)
		;
	delay_ms(100);	/* let Silver CPU0 finish CPU_OFF */

	stage = "gold osm";
	drg_osm_probe();
	stage = "gold touch";
	drg_touch_probe();
	stage = "gold ldo";
	drg_ldo_probe();

	/* A NOKIAXNU payload in the ramdisk: boot XNU from here (returns only on error). */
	stage = "gold loader";
	con_skip();
	con_str("GOLD", "loading xnu on the Gold core");
	con_str("LOADER", loader_boot(handoff_dtb));

	/*
	 * No payload: eMMC probe (R3a) and the R6 one-block write test, which
	 * only writes past the iOS image in userdata. The Gold core's ID registers
	 * shown here before are in drg/BOOTLOG.md (boot #4).
	 */
	stage = "gold emmc";
	con_skip();
	drg_emmc_probe();
	con_skip();

	stage = "gold done";
	con_str("DONE", "from Gold; fastboot in ~90s");
	delay_ms(50000);
	reset(40);
}

/*
 * DRG: hand the boot over from Silver CPU0 (where ABL enters) to Gold
 * 0x100, which loads and runs XNU with 16K pages. Silver powers itself off.
 */
static void __attribute__((noreturn)) drg_main(uint64_t dtb)
{
	stage = "drg wdt";
	wr32(WDT_BASE + WDT_EN, 0);
	__asm__ volatile("dsb sy" ::: "memory");

	stage = "drg console";
	con_clear(0xff102040U);
	con_str("SHIM", "DRG: gold handoff + xnu loader");
	con_hex2("EL", "cur=", rsr("currentel") >> 2, "cpu=", rsr("mpidr_el1"));
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "wdt=", rd32(WDT_BASE + WDT_EN));
	con_skip();

	stage = "handoff";
	handoff_dtb = dtb;
	handoff_req = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	int64_t ret = smc(PSCI_CPU_ON, DRG_GOLD0, (uintptr_t)secondary_entry, 4);
	con_hex2("CPU_ON", "ret=", (uint32_t)ret, "mpidr=", DRG_GOLD0);
	uint64_t deadline = now() + rsr("cntfrq_el0");	/* 1 s */
	while (!gold_alive && now() < deadline)
		;
	if (!gold_alive) {
		con_str("HANDOFF", "Gold did not answer");
		reset(40);
	}
	con_str("HANDOFF", "Gold alive; Silver CPU_OFF");
	silver_done = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
	/* Only reached if CPU_OFF failed: stay out of Gold's way. */
	for (;;)
		__asm__ volatile("wfe");
}
#endif

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

#ifdef BOARD_DRG
	drg_main(dtb);
#endif

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", "sweet milestone 4: xnu loader");
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	/* No payload: probe the internal UFS while the MMU is still off. */
	stage = "ufs";
	test_ufs();
	con_skip();
	/* Keep the probe results on screen (later tests clear it); reset after 3 min. */
	con_str("UFS", "probe done, reset in ~3 min");
	delay_ms(180000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
