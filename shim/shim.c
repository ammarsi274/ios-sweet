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
#include "drg_rpm.h"
#include "drg_ts.h"
#include "drg_gpu.h"
#include "fdt.h"

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
volatile uint32_t sec_entered[NR_CPUS];	/* start.S secondary_entry's first store */
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

/* drg_gpu.c: called for SPIs before the EOI (a level source must be cleared there) */
void (*volatile drg_irq_hook)(uint32_t intid);

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
	if (id >= 32 && drg_irq_hook)
		drg_irq_hook(id);
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

#ifdef BOARD_DRG
static void probe_fault(void);
#endif

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
#ifdef BOARD_DRG
	probe_fault();	/* returns only if this core is not running a probe */
#endif
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

#ifdef BOARD_DRG
/*
 * Risky reads for the GPU bring-up (drg_gpu.c): one address on a spare
 * core, so a read that never completes stalls only that core and the
 * caller sees a timeout. SError is unmasked for the read, so a bus error
 * reported asynchronously is caught too (probe_fault).
 */
enum { PROBE_IDLE, PROBE_READING, PROBE_DONE, PROBE_FAULT };
static volatile uintptr_t probe_addr[NR_CPUS];
static void (*volatile probe_fn[NR_CPUS])(void);
static volatile uint32_t probe_val[NR_CPUS], probe_state[NR_CPUS];
static volatile uint64_t probe_esr[NR_CPUS];

static unsigned core_index(void)
{
	uint64_t m = rsr("mpidr_el1");
	return (unsigned)(((m >> 8) & 0xff) ? 4 + (m & 0xff) : (m & 0xff)) % NR_CPUS;
}

static void probe_fault(void)
{
	unsigned i = core_index();
	if (probe_state[i] != PROBE_READING)
		return;
	probe_esr[i] = rsr("esr_el1");
	probe_state[i] = PROBE_FAULT;
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static uint64_t drg_mpidr(int i);

static int probe_run(int idx, uintptr_t addr, void (*fn)(void), uint32_t *val, uint64_t *esr,
		     unsigned timeout_us)
{
	probe_fn[idx] = fn;
	probe_addr[idx] = addr;
	probe_state[idx] = PROBE_IDLE;
	probe_esr[idx] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	int64_t r = smc(PSCI_CPU_ON, drg_mpidr(idx), (uintptr_t)secondary_entry, (uint64_t)idx);
	if (r) {
		*val = (uint32_t)r;
		return -2;	/* CPU_ON refused: *val = PSCI error */
	}
	uint64_t deadline = now() + rsr("cntfrq_el0") / 1000000 * timeout_us;
	while (probe_state[idx] < PROBE_DONE && now() < deadline)
		;
	*val = probe_val[idx];
	*esr = probe_esr[idx];
	switch (probe_state[idx]) {
	case PROBE_DONE: return 0;
	case PROBE_FAULT: return -3;
	case PROBE_READING: return -1;	/* read never completed */
	default: return -4;		/* core never started */
	}
}

int drg_probe_read(int idx, uintptr_t addr, uint32_t *val, uint64_t *esr, unsigned timeout_us)
{
	return probe_run(idx, addr, 0, val, esr, timeout_us);
}

/* Same, but run fn on the spare core (register writes that might stall). */
int drg_probe_call(int idx, void (*fn)(void), uint64_t *esr, unsigned timeout_us)
{
	uint32_t v;

	return probe_run(idx, 0, fn, &v, esr, timeout_us);
}
#endif

void secondary_main(uint64_t idx)
{
#ifdef BOARD_DRG
	if (idx == 4 && handoff_req)
		drg_gold_main();
	if (probe_addr[idx] || probe_fn[idx]) {
		probe_state[idx] = PROBE_READING;
		__asm__ volatile("dsb sy; msr daifclr, #4; isb" ::: "memory");
		uint32_t v = 0;
		if (probe_fn[idx])
			probe_fn[idx]();
		else
			/* plain [Xn] load: a hypervisor-trapped register needs a decodable access */
			__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(probe_addr[idx]) : "memory");
		__asm__ volatile("dsb sy; isb" ::: "memory");	/* take a pending SError here */
		probe_val[idx] = v;
		probe_state[idx] = PROBE_DONE;
		__asm__ volatile("dsb sy" ::: "memory");
		smc(PSCI_CPU_OFF, 0, 0, 0);
	}
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

/*
 * drg_gpu.c: route one SPI to the calling core as Group 1 NS, level,
 * priority 0xa0 (what pe_fiq.c does for the timer PPI), or switch it off.
 * GICD offsets (GICv3 spec): IGROUPR 0x80, ISENABLER 0x100, ICENABLER 0x180,
 * IPRIORITYR 0x400, ICFGR 0xc00, IROUTER 0x6000 + 8n. With two security
 * states a secure SPI's group bit is RAZ/WI for us: *grp_after says which.
 */
int drg_gic_spi(uint32_t intid, int on, uint32_t *grp_before, uint32_t *grp_after)
{
	uint32_t n = intid / 32, bit = 1U << (intid % 32);
	if (!on) {
		wr32(GICD_BASE + 0x180 + 4 * n, bit);
		gicd_wait_rwp();
		return 0;
	}
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	wsr("icc_pmr_el1", 0xff);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	wr32(GICD_BASE, rd32(GICD_BASE) | 0x13);	/* ARE_NS, EnableGrp1A, EnableGrp1 (as Linux) */
	gicd_wait_rwp();
	uintptr_t r = find_gicr();
	if (!r)
		return -1;
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	*grp_before = (rd32(GICD_BASE + 0x80 + 4 * n) & bit) ? 1 : 0;
	wr32(GICD_BASE + 0x80 + 4 * n, rd32(GICD_BASE + 0x80 + 4 * n) | bit);
	*grp_after = (rd32(GICD_BASE + 0x80 + 4 * n) & bit) ? 1 : 0;
	wr8(GICD_BASE + 0x400 + intid, 0xa0);
	uintptr_t cfg = GICD_BASE + 0xc00 + 4 * (intid / 16);
	wr32(cfg, rd32(cfg) & ~(2U << (2 * (intid % 16))));	/* level */
	uint64_t m = rsr("mpidr_el1");
	wr32(GICD_BASE + 0x6000 + 8 * intid, (uint32_t)(m & 0xffffff));	/* Aff2.Aff1.Aff0, IRM 0 */
	wr32(GICD_BASE + 0x6004 + 8 * intid, (uint32_t)((m >> 32) & 0xff));	/* Aff3 */
	wr32(GICD_BASE + 0x100 + 4 * n, bit);
	gicd_wait_rwp();
	return 0;
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

/* PSCI AFFINITY_INFO of core idx (level 0): 0 on, 1 off, 2 on pending */
int64_t drg_psci_affinity(int idx)
{
	return (int64_t)smc(PSCI_AFFINITY_INFO, drg_mpidr(idx), 0, 0);
}

uint32_t drg_sec_entered(int idx)
{
	return sec_entered[idx & 7];
}

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
 * Gold CPU voltage path, read-only (drg/CPU-SPEED.md): what ABL leaves in the
 * Gold cluster's SAW4 ("gold-l2", 0x17812000; register offsets from HMD's
 * msm-spm.c msm_spm_reg_offsets_saw2_v4_1), the CPRh status (cprh-ctrl
 * 0x179c4000, PERF_CPRH_STATUS 0x179c7aa4 from the device tree's panic list),
 * the Gold PLL (perfcl_pll 0x17816000), and the Gold cores' real clock from
 * the cycle counter over 10 ms. Plain reads, as drg_osm_probe's of the OSM
 * block next to them: boot #45 tried a spare core (drg_probe_read) and every
 * CPU_ON was refused at this point of the boot (core never started, then
 * PSCI refused). CPRh is not read: it needs its own core clock. Results go to
 * the userdata log area (emmc.c) for TWRP. Nothing is written to any of these
 * blocks.
 *
 * Boot #46 showed the SAW has never sent a level (VCTL, PMIC_STS 0), so the
 * Gold rail sits at what ABL/RPM left. That level is in the PMIC: a buck
 * scan over SPMI, read only through the observer channel (drg_spmi_read, as
 * the keys and LDO probes). PM660 = SID 1, PM660L = SID 3 (regulators, boot
 * #25); every peripheral reports TYPE 0x04 / SUBTYPE 0x05. Bucks, from HMD's
 * spm-regulator.c: TYPE 0x1C (FTS2 0x08, FTS2.5 0x09, FTS426 0x0A), HF 0x03,
 * ULT_HF 0x22. For each: STATUS1 0x08, voltage 0x40-0x43 (FTS426: setpoint
 * LB/UB 0x40/0x41, valid LB/UB 0x42/0x43, in mV), MODE 0x45, EN_CTL 0x46,
 * ULS 0x68/0x69, and the channel's owner EE (cnfg + 0x700 + 4 * apid, bits
 * 2:0, as drg_l6_enable). Line: "buck" (ppid << 8 | reg) value.
 */
#define DRG_SAW_GOLD		0x17812000UL

static int drg_spmi_read(unsigned int apid, uint8_t addr, uint8_t *val);
static unsigned int drg_spmi_apid(uint32_t ppid);

static char cpu_log[16384];
static unsigned cpu_len;

static void cpu_put(const char *label, uint32_t a, uint32_t v, int rc)
{
	static const char hex[] = "0123456789abcdef";
	char t[64];
	unsigned n = 0;
	while (*label && n < 24)
		t[n++] = *label++;
	t[n++] = ' ';
	for (int sh = 28; sh >= 0; sh -= 4)
		t[n++] = hex[(a >> sh) & 15];
	t[n++] = ' ';
	for (int sh = 28; sh >= 0; sh -= 4)
		t[n++] = hex[(v >> sh) & 15];
	t[n++] = ' ';
	t[n++] = rc == 0 ? 'o' : 'x';
	t[n++] = rc == 0 ? 'k' : (char)('0' - rc);
	t[n++] = '\n';
	for (unsigned i = 0; i < n && cpu_len < sizeof(cpu_log); i++)
		cpu_log[cpu_len++] = t[i];
}

static void cpu_rd(const char *label, uintptr_t a)
{
	cpu_put(label, (uint32_t)a, rd32(a), 0);
}

static void drg_buck_scan(void)
{
	static const uint8_t sids[2] = { 1, 3 };
	static const uint8_t regs[] = { 0x08, 0x40, 0x41, 0x42, 0x43, 0x45, 0x46, 0x68, 0x69 };
	for (unsigned int i = 0; i < 2; i++) {
		for (unsigned int per = 0x10; per <= 0x3f; per++) {
			uint32_t ppid = ((uint32_t)sids[i] << 8) | per;
			unsigned int apid = drg_spmi_apid(ppid);
			uint8_t type = 0, sub = 0;
			if (apid == 0xffff || drg_spmi_read(apid, 0x04, &type) ||
			    (type != 0x1c && type != 0x03 && type != 0x22))
				continue;
			int rc = drg_spmi_read(apid, 0x05, &sub);
			cpu_put("buck type", ppid << 8 | 0x04, (uint32_t)type << 8 | sub, rc ? 1 : 0);
			cpu_put("buck owner", ppid << 8, rd32(0x0800a000UL + 0x700 + 4 * apid), 0);
			for (unsigned int r = 0; r < sizeof(regs); r++) {
				uint8_t v = 0;
				rc = drg_spmi_read(apid, regs[r], &v);
				cpu_put("buck", ppid << 8 | regs[r], v, rc ? 1 : 0);
			}
		}
	}
}

/* real clock: PMU cycle counter (PMCR.E, PMCNTENSET bit 31) over 10 ms of the generic timer */
static uint32_t gold_khz(void)
{
	uint64_t pmcr = rsr("pmcr_el0");
	wsr("pmcr_el0", (pmcr | 1) & ~(1UL << 3));	/* E on, D off (count every cycle), as drg_perf.c */
	wsr("pmcntenset_el0", 1UL << 31);
	__asm__ volatile("isb");
	uint64_t c0 = rsr("pmccntr_el0");
	delay_ms(10);
	uint64_t c1 = rsr("pmccntr_el0");
	wsr("pmcr_el0", pmcr);
	return (uint32_t)((c1 - c0) / 10);
}

static void delay_us(unsigned us)
{
	uint64_t frq, t0, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	uint64_t n = (frq * us + 999999) / 1000000;
	do {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	} while (t - t0 < n);
}

/*
 * Gold clock 600 MHz -> 1,804.8 MHz (PLL L 0x20 -> 0x5e), the voltage left as
 * it is. Facts (drg/CPU-SPEED.md, boots #46/#47): the Gold rail is one of the
 * PM660 bucks S2/S3 (the CPU rails: the only bucks neither the RPM nor anyone
 * else manages), both at 868 mV, and the SAW/CPRh never change it under iOS;
 * this chip needs at most 824 mV at 1,804.8 MHz (CPR corner 5 ceiling, fuse
 * combo 26), Linux runs it there. The cache memory settings (mem-acc, APM)
 * follow the voltage, which does not change.
 *
 * Sequence, from HMD's clk-cpu-osm.c (the OSM sequencer's own steps and
 * clk_osm_setup_cluster_pll):
 *   1. RCG to GPLL0: CFG (0x17811054) source = GPLL_SEL 0x400, CMD
 *      (0x17811050) = RCG_UPDATE 0x3, wait for RCG_UPDATE_SUCCESS 0x2
 *      (update bit clear). A glitch-free switch: it completes only once both
 *      sources run, else the core stays where it was (timeout: back to GPLL0).
 *      Boot #48 read CFG 0x401: ABL already runs Gold on GPLL0 (the 600.1 MHz
 *      measured since boot #18 is GPLL0, not the idle PLL's 614.4), so this
 *      step is skipped then.
 *   2. PLL (0x17816000): MODE 0, L 0x5e, MODE 2, 10 us (PLL_WAIT_LOCK_TIME_US),
 *      MODE 6, 50-75 us, MODE 7. USER_CTL / CONFIG stay as ABL left them
 *      (Linux writes them only with qcom,osm-pll-setup, absent on this phone).
 *   3. RCG to the PLL as the sequencer does: PLL_EARLY_SEL 0x500, then
 *      PLL_MAIN_SEL 0x300, the divider bits of CFG kept.
 * Done only if all of these read as expected first: PLL MODE 7 and L 0x20,
 * the RCG source GPLL0 or a PLL one, no update pending, S2 and S3 on with at
 * least 840 mV, and the clock on GPLL0 measured in 100-700 MHz. On the PLL's
 * early output the clock must measure 1.6-1.95 GHz, else back to GPLL0.
 * Every step is logged; the result is the measured clock.
 */
#define DRG_GOLD_CMD_RCGR	0x17811050UL
#define DRG_GOLD_CFG_RCGR	0x17811054UL
#define DRG_GOLD_L_FAST		0x5e

static int gold_rcg_switch(uint32_t cfg)
{
	wr32(DRG_GOLD_CFG_RCGR, cfg);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(DRG_GOLD_CMD_RCGR, 0x3);
	__asm__ volatile("dsb sy" ::: "memory");
	for (int i = 0; i < 1000; i++) {
		if (!(rd32(DRG_GOLD_CMD_RCGR) & 1))
			return 0;
		delay_us(1);
	}
	return 1;
}

static uint32_t drg_gold_speedup(void)
{
	uint32_t mode = rd32(DRG_PLL_GOLD), l = rd32(DRG_PLL_GOLD + 4);
	uint32_t cmd = rd32(DRG_GOLD_CMD_RCGR), cfg = rd32(DRG_GOLD_CFG_RCGR);
	cpu_put("speed pll mode", DRG_PLL_GOLD, mode, 0);
	cpu_put("speed pll L", DRG_PLL_GOLD + 4, l, 0);
	cpu_put("speed rcg cmd", DRG_GOLD_CMD_RCGR, cmd, 0);
	cpu_put("speed rcg cfg", DRG_GOLD_CFG_RCGR, cfg, 0);
	uint32_t src = cfg & 0x700;
	if (mode != 0x7 || l != 0x20 || (src != 0x400 && src != 0x300 && src != 0x500) || (cmd & 1)) {
		cpu_put("speed SKIP state", 0, 1, 1);
		return gold_khz();
	}
	static const uint32_t rails[2] = { 0x117, 0x11a };	/* PM660 S2, S3 */
	for (unsigned i = 0; i < 2; i++) {
		unsigned int apid = drg_spmi_apid(rails[i]);
		uint8_t en = 0, lb = 0, ub = 0;
		int rc = apid == 0xffff ? -1 : drg_spmi_read(apid, 0x46, &en);
		if (!rc)
			rc = drg_spmi_read(apid, 0x42, &lb);
		if (!rc)
			rc = drg_spmi_read(apid, 0x43, &ub);
		uint32_t mv = (uint32_t)ub << 8 | lb;
		cpu_put("speed rail mV", rails[i] << 8 | 0x42, mv, rc ? 1 : 0);
		if (rc || !(en & 0x80) || mv < 840) {
			cpu_put("speed SKIP rail", 0, 2, 1);
			return gold_khz();
		}
	}

	uint32_t gpll0 = (cfg & ~0x700u) | 0x400;
	if (src != 0x400 && gold_rcg_switch(gpll0)) {
		cpu_put("speed rcg->gpll0 timeout", DRG_GOLD_CMD_RCGR, rd32(DRG_GOLD_CMD_RCGR), 1);
		(void)gold_rcg_switch(cfg);
		return gold_khz();
	}
	uint32_t khz = gold_khz();
	cpu_put("speed kHz on gpll0", DRG_GOLD_CFG_RCGR, khz, 0);
	if (khz < 100000 || khz > 700000) {
		int rc = src != 0x400 ? gold_rcg_switch(cfg) : 0;
		cpu_put("speed SKIP gpll0 clock, back", 0, (uint32_t)rc, 1);
		return gold_khz();
	}

	wr32(DRG_PLL_GOLD, 0x0);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(DRG_PLL_GOLD + 4, DRG_GOLD_L_FAST);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(DRG_PLL_GOLD, 0x2);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_us(10);
	wr32(DRG_PLL_GOLD, 0x6);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_us(75);
	wr32(DRG_PLL_GOLD, 0x7);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_us(100);
	cpu_put("speed pll mode new", DRG_PLL_GOLD, rd32(DRG_PLL_GOLD), 0);
	cpu_put("speed pll L new", DRG_PLL_GOLD + 4, rd32(DRG_PLL_GOLD + 4), 0);

	int rc = gold_rcg_switch((cfg & ~0x700u) | 0x500);
	cpu_put("speed rcg->pll early", DRG_GOLD_CMD_RCGR, rd32(DRG_GOLD_CMD_RCGR), rc ? 1 : 0);
	if (rc) {
		cpu_put("speed back to gpll0", 0, (uint32_t)gold_rcg_switch(gpll0), 1);
		return gold_khz();
	}
	khz = gold_khz();
	cpu_put("speed kHz on pll early", 0, khz, 0);
	if (khz < 1600000 || khz > 1950000) {
		cpu_put("speed back to gpll0", 0, (uint32_t)gold_rcg_switch(gpll0), 1);
		return gold_khz();
	}
	rc = gold_rcg_switch((cfg & ~0x700u) | 0x300);
	cpu_put("speed rcg->pll main", DRG_GOLD_CMD_RCGR, rd32(DRG_GOLD_CMD_RCGR), rc ? 1 : 0);
	if (rc) {
		cpu_put("speed back to gpll0", 0, (uint32_t)gold_rcg_switch(gpll0), 1);
		return gold_khz();
	}
	khz = gold_khz();
	cpu_put("speed kHz final", DRG_GOLD_CFG_RCGR, khz, 0);
	cpu_put("speed rcg cfg final", DRG_GOLD_CFG_RCGR, rd32(DRG_GOLD_CFG_RCGR), 0);
	return khz;
}

/*
 * Normal-phone buttons (user, 2026-11-05): Power alone = iOS, Volume Up held
 * at power-on = TWRP once, Volume Down = ABL's own download mode (ABL handles
 * it before this image runs). Volume Up is PON RT_STS bit 1 (resin), read
 * as in boots #21/#22 (drg_spmi_read below). Every start makes sure slot A
 * stays active, successful, 7 tries (gptslot.c); with Volume Up it makes B
 * a one-try slot and resets: ABL starts TWRP once, then falls back to A.
 */
static int drg_spmi_read(unsigned int apid, uint8_t addr, uint8_t *val);
static unsigned int drg_spmi_apid(uint32_t ppid);
#define DRG_PON_PPID_SLOTS	0x008		/* SID 0, peripheral 0x08: PON */

static void __attribute__((noreturn)) reset_normal(void)
{
	wr32(IMEM_RESTART_REASON, 0);	/* no reason: ABL's normal start, which picks the active slot */
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* PON RT_STS (bit 0 power, bit 1 volume up) in the low byte, or 0x100 | -rc on a failed read */
static uint32_t pon_rt_raw(void)
{
	unsigned int apid = drg_spmi_apid(DRG_PON_PPID_SLOTS);
	uint8_t st = 0;
	int rc = apid == 0xffff ? 0xff : drg_spmi_read(apid, 0x10, &st);
	return rc ? 0x100u | ((uint32_t)-rc & 0xff) : st;
}

static void cpu_put(const char *label, uint32_t a, uint32_t v, int rc);

/*
 * Boot #53: no TWRP however Volume Up was held, and the one read at this
 * point said "not held" (the read itself is right: TWRP's Linux reports
 * Volume Up as PON resin). The key is now sampled when the shim starts on
 * Silver (vol_up_early) and again for 1 s here, with a hint on screen.
 */
static volatile uint32_t vol_up_early = 0x200;	/* not sampled */

static void drg_slots(void)
{
	uint32_t raw_early = vol_up_early;
	int up = raw_early < 0x100 && ((raw_early >> 1) & 1);
	uint32_t raw = 0x200, waited = 0;
	if (!up) {
		con_str("BUTTONS", "hold Volume Up now for TWRP");
		for (waited = 0; waited < 10 && !up; waited++) {
			raw = pon_rt_raw();
			up = raw < 0x100 && ((raw >> 1) & 1);
			if (!up)
				delay_ms(100);
		}
	}
	struct gpt_slots_result r;
	int rc = drg_emmc_slots(up ? GPT_SLOTS_TWRP_ONCE : GPT_SLOTS_NORMAL, &r);
	cpu_put("slots pon early late", raw_early, raw, 0);
	cpu_put("slots vol-up mode", waited, (uint32_t)up, 0);
	cpu_put("slots rc primary backup", (uint32_t)r.primary << 16 | ((uint32_t)r.backup & 0xffff), (uint32_t)rc,
		rc < 0);
	cpu_put("slots was a b", r.was_a, r.was_b, 0);
	con_hex2("SLOTS", "rc=", (uint32_t)rc, "volup=", (uint32_t)up);
	if (up && rc >= 0) {
		int lr = drg_emmc_log_open();
		if (lr == 0)
			(void)drg_emmc_log_write(cpu_log, cpu_len);
		con_str("SLOTS", "TWRP once: resetting");
		delay_ms(200);
		reset_normal();
	}
}

static void drg_cpu_probe(void)
{
	static const uint16_t saw[] = { 0xc04, 0xfd0, 0x00, 0x04, 0x0c, 0x900, 0x904, 0x908, 0x90c,
					0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0xc0c, 0xc10, 0xc14, 0xc18 };
	cpu_len = 0;
	drg_slots();
	cpu_put("PLL L gold (direct)", (uint32_t)(DRG_PLL_GOLD + 4), rd32(DRG_PLL_GOLD + 4), 0);
	for (unsigned i = 0; i < sizeof(saw) / sizeof(saw[0]); i++)
		cpu_rd("saw4", DRG_SAW_GOLD + saw[i]);
	for (unsigned o = 0; o < 0x40; o += 4)
		cpu_rd("pll gold", DRG_PLL_GOLD + o);
	uint32_t khz = gold_khz();
	cpu_put("gold kHz (cycles/10ms)", 0, khz, 0);
	drg_buck_scan();
	/* the log goes out before the speed-up too, so a hang there still leaves the readings */
	int rc = drg_emmc_log_open();
	if (rc == 0)
		rc = drg_emmc_log_write(cpu_log, cpu_len);
	con_hex2("CPU PROBE", "log=", (uint32_t)rc, "kHz=", khz);
	if (rc == 0) {
		khz = drg_gold_speedup();
		rc = drg_emmc_log_write(cpu_log, cpu_len);
		con_hex2("CPU SPEED", "log=", (uint32_t)rc, "kHz=", khz);
	}
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

/* One PMIC register through the observer channel (read only), for drg_gpu.c. */
int drg_pmic_read(uint32_t ppid, uint8_t addr, uint8_t *val)
{
	unsigned int apid = drg_spmi_apid(ppid);
	if (apid == 0xffff)
		return -2;
	return drg_spmi_read(apid, addr, val);
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
/*
 * Switch on the touch analog supply pm660l_l6 (SID 3 peripheral 0x45), off
 * as ABL leaves it (boot #26: EN_CTL 0x00, voltage set to 3312 mV), the
 * user's go-ahead given for exactly this. Only if the arbiter's ownership
 * table gives the channel to EE 0 (the application cores):
 *   owner = cnfg 0x800a000 + 0x700 + 4 * apid, bits 2:0
 *   write = chnls 0x8400000 + 0x8000 * apid: WDATA0 +0x10, CMD +0, STATUS +8
 *   CMD = EXT_WRITEL (0) << 27 | (addr & 0xff) << 4 | (bytes - 1)
 * (drivers/spmi/spmi-pmic-arb.c, v3: the hardware enforces ownership and
 * answers a refused write with STATUS bit 2.) One byte: EN_CTL 0x46 = 0x80.
 */
#define DRG_SPMI_CNFG		0x0800a000UL
#define DRG_SPMI_CHNLS		0x08400000UL

static void drg_l6_enable(void)
{
	unsigned int apid = drg_spmi_apid(0x345);
	if (apid == 0xffff) {
		con_str("L6", "not mapped: nothing written");
		return;
	}
	uint32_t owner = rd32(DRG_SPMI_CNFG + 0x700 + 4 * apid) & 7;
	con_hex2("L6 OWNER", "apid=", apid, "owner-ee=", owner);
	if (owner != 0) {
		con_str("L6", "owned by another EE: nothing written");
		return;
	}
	uintptr_t ch = DRG_SPMI_CHNLS + 0x8000UL * apid;
	wr32(ch + 0x10, 0x80);
	wr32(ch + 0x0, (0u << 27) | (0x46u << 4) | 0);
	uint32_t st = 0;
	for (int i = 0; i < 100000; i++) {
		st = rd32(ch + 0x8);
		if (st & 1)
			break;
	}
	delay_ms(10);
	uint8_t en = 0xee, ready = 0xee;
	(void)drg_spmi_read(apid, 0x46, &en);
	(void)drg_spmi_read(apid, 0x08, &ready);
	con_hex2("L6 WRITE", "status=", st, "en/st=", (uint32_t)en << 8 | ready);
}

/* One PMIC byte through the arbiter's channel, only if EE 0 owns the peripheral. */
static int drg_spmi_write(uint32_t ppid, uint8_t addr, uint8_t val)
{
	unsigned int apid = drg_spmi_apid(ppid);
	if (apid == 0xffff || (rd32(DRG_SPMI_CNFG + 0x700 + 4 * apid) & 7) != 0)
		return -2;
	uintptr_t ch = DRG_SPMI_CHNLS + 0x8000UL * apid;
	wr32(ch + 0x10, val);
	wr32(ch + 0x0, (uint32_t)addr << 4);
	for (int i = 0; i < 100000; i++) {
		uint32_t st = rd32(ch + 0x8);
		if (st & 1)
			return (st & 0xe) ? -(int)st : 0;
	}
	return -1;
}

/*
 * Power off as HMD msm-poweroff.c do_msm_poweroff + qpnp-power-on.c
 * qpnp_pon_reset_config (GEN2 PONs: RESET_EN = PS_HOLD_RST_CTL2 0x5b bit 7):
 * PM660 (PPID 0x008) and PM660L (0x208) get PS_HOLD reset type 4 (shutdown),
 * then PSCI SYSTEM_OFF, then PS_HOLD 0x10ac000 = 0. The kernel does the same
 * (patch 0919, DrgPlatformExpert).
 */
static void __attribute__((noreturn)) power_off(void)
{
	static const uint32_t pons[2] = { 0x008, 0x208 };
	for (unsigned int k = 0; k < 2; k++) {
		unsigned int apid = drg_spmi_apid(pons[k]);
		uint8_t ctl = 0, ctl2 = 0;
		if (apid == 0xffff || drg_spmi_read(apid, 0x5a, &ctl) || drg_spmi_read(apid, 0x5b, &ctl2))
			continue;
		(void)drg_spmi_write(pons[k], 0x5b, ctl2 & 0x7f);
		delay_us(500);
		(void)drg_spmi_write(pons[k], 0x5a, (uint8_t)((ctl & 0xf0) | 4));
		(void)drg_spmi_write(pons[k], 0x5b, ctl2 | 0x80);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(0x84000008UL, 0, 0, 0);	/* PSCI SYSTEM_OFF */
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static void put_dec(char *buf, unsigned v)
{
	char t[8];
	int n = 0;
	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < 7);
	for (int i = 0; i < n; i++)
		buf[i] = t[n - 1 - i];
	buf[n] = 0;
}

/*
 * Off-mode charging. A charger plugged into a phone that is off makes ABL
 * start this image with "androidboot.mode=charger" (ABL: "charger path",
 * "Set off mode charger"); before, the shim booted iOS anyway. The PMIC
 * charges by itself (boot #53: cold starts show charger state 3, full-on).
 * Here: a black screen with the fuel gauge's percentage (0x4009/0x400a, as
 * DrgBattery.cpp), the backlight off after 20 s (WLED MODULE_EN 0xd846
 * bit 7, PPID 0x3d8), a short Power press lights it again, Power held 2 s
 * starts iOS (normal reset: a warm start is not ABL's charger path), and
 * the charger unplugged for 3 s powers the phone off.
 */
static void __attribute__((noreturn)) drg_charger_mode(void)
{
	unsigned int soc = drg_spmi_apid(0x040), usb = drg_spmi_apid(0x013), chg = drg_spmi_apid(0x010);
	unsigned int lit = 200, held = 0, gone = 0, shown = 0x1000;
	int wled = drg_spmi_write(0x3d8, 0x46, 0x80);
	for (unsigned int tick = 0;; tick++) {
		uint8_t s1 = 0, s2 = 1, u = 0x10, c = 0;
		(void)drg_spmi_read(soc, 0x09, &s1);
		(void)drg_spmi_read(soc, 0x0a, &s2);
		(void)drg_spmi_read(usb, 0x10, &u);
		(void)drg_spmi_read(chg, 0x06, &c);
		uint32_t pon = pon_rt_raw();
		int power = pon < 0x100 && (pon & 1);

		unsigned int pct = s1 == s2 ? (s1 == 255 ? 100 : s1 == 0 ? 0 : ((s1 - 1u) * 98 + 126) / 253 + 1) : 0xfff;
		unsigned int state = (unsigned int)(c & 7);
		if (pct != shown || tick % 100 == 0) {
			char line[24] = "", num[8];
			put_dec(num, pct);
			int n = 0;
			for (const char *p = pct == 0xfff ? "--" : num; *p; p++)
				line[n++] = *p;
			line[n++] = ' ';
			line[n++] = '%';
			line[n] = 0;
			con_clear(0xff000000U);
			con_skip(); con_skip(); con_skip(); con_skip(); con_skip(); con_skip();
			con_str(state <= 4 ? "CHARGING" : state == 7 ? "PAUSED" : "FULL", line);
			con_skip();
			con_str("POWER", "hold 2 s to start iOS");
			con_hex2("PMIC", "chg=", c, "wled=", (uint32_t)wled);
			shown = pct;
		}

		if (power) {
			if (lit == 0)
				(void)drg_spmi_write(0x3d8, 0x46, 0x80);
			lit = 200;
			if (++held >= 20) {
				con_str("POWER", "starting iOS");
				delay_ms(300);
				reset_normal();
			}
		} else {
			held = 0;
		}
		if (lit && --lit == 0)
			(void)drg_spmi_write(0x3d8, 0x46, 0x00);
		if (!((u >> 4) & 1)) {
			if (++gone >= 30)
				power_off();
		} else {
			gone = 0;
		}
		delay_ms(100);
	}
}

static void drg_ldo_probe(void)
{
	/*
	 * PM660 (SID 1) and PM660L (SID 3) regulators are TYPE 0x04 (LDO,
	 * drivers/regulator/qpnp-regulator.c), LDO n at peripheral 0x3f + n
	 * (boot #25). Per PMIC over 0x40-0x5f: bitmasks of enabled (EN_CTL 0x46
	 * bit 7) and ready (STATUS1 0x08 bit 7). Then the touch supplies raw:
	 * pm660_l11 = SID 1 0x4a, pm660l_l6 = SID 3 0x45: TYPE/SUBTYPE/EN/STATUS
	 * and the voltage registers 0x40/0x41.
	 */
	static const uint8_t sids[2] = { 1, 3 };
	for (unsigned int i = 0; i < 2; i++) {
		uint32_t on = 0, ready = 0;
		for (unsigned int per = 0x40; per <= 0x5f; per++) {
			unsigned int apid = drg_spmi_apid(((uint32_t)sids[i] << 8) | per);
			uint8_t type = 0, en = 0, st = 0;
			if (apid == 0xffff || drg_spmi_read(apid, 0x04, &type) || type != 0x04)
				continue;
			if (!drg_spmi_read(apid, 0x46, &en) && (en & 0x80))
				on |= 1u << (per - 0x40);
			if (!drg_spmi_read(apid, 0x08, &st) && (st & 0x80))
				ready |= 1u << (per - 0x40);
		}
		con_hex2(i ? "LDO PM660L" : "LDO PM660", "on=", on, "ready=", ready);
	}
	static const uint32_t ts[2] = { 0x14a, 0x345 };     /* l11 vcc_i2c, l6 vdd_ana */
	for (unsigned int k = 0; k < 2; k++) {
		unsigned int apid = drg_spmi_apid(ts[k]);
		uint8_t type = 0xee, sub = 0xee, en = 0xee, st = 0xee, v0 = 0xee, v1 = 0xee;
		if (apid != 0xffff) {
			(void)drg_spmi_read(apid, 0x04, &type);
			(void)drg_spmi_read(apid, 0x05, &sub);
			(void)drg_spmi_read(apid, 0x46, &en);
			(void)drg_spmi_read(apid, 0x08, &st);
			(void)drg_spmi_read(apid, 0x40, &v0);
			(void)drg_spmi_read(apid, 0x41, &v1);
		}
		con_hex2(k ? "TS L6" : "TS L11", "t/s/en/st=",
		    (uint32_t)type << 24 | (uint32_t)sub << 16 | (uint32_t)en << 8 | st,
		    "v40/41=", (uint32_t)v0 << 8 | v1);
	}
	drg_l6_enable();
	/* L6 belongs to the RPM (boot #27): ask it, then read L6 back over SPMI. */
	int rc = drg_rpm_ldo_enable(RPM_RES_LDOB, 6, 3300000);
	delay_ms(10);
	unsigned int apid = drg_spmi_apid(0x345);
	uint8_t en = 0xee, st = 0xee;
	if (apid != 0xffff) {
		(void)drg_spmi_read(apid, 0x46, &en);
		(void)drg_spmi_read(apid, 0x08, &st);
	}
	con_hex2("RPM L6", "rc=", (uint32_t)rc, "en/st=", (uint32_t)en << 8 | st);
	/* Only with both touch supplies on: clock, pins, reset, one I2C read. */
	if (en == 0x80 && (st & 0x80))
		con_hex2("TS2", "rc=", (uint32_t)drg_ts_probe(), "", 0);
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
	stage = "gold cpu probe";
	drg_cpu_probe();
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
#ifdef DRG_EMMC_MB_TEST
	/* only the multi-block eMMC test (emmc.c): no GPU, no R6 */
	stage = "gold emmc mb";
	con_skip();
	drg_emmc_mb_test();
	con_skip();
#else
	stage = "gold gpu";
	drg_gpu_bringup();

	stage = "gold emmc";
	con_skip();
	drg_emmc_probe();
	con_skip();
#endif

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

	vol_up_early = pon_rt_raw();	/* as early as possible: see drg_slots */
	stage = "drg charger";
	if (fdt_chosen_has(dtb, "bootargs", "androidboot.mode=charger"))
		drg_charger_mode();

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
