/*
 * A1 (research/reports/audio.md 3(a) + the ADSP boot of 3(a')), before XNU.
 * No sound: the amp is only read, the ADSP is booted, watched and shut down.
 *
 * (a) TI TAS2557 at I2C 0x4c, BLSP2 QUP3 (device-info/fdt.dts i2c@c1b7000
 *   "qcom,i2c-msm-v2", clocks iface gcc_blsp2_ahb (vote 0x52004 bit 15,
 *   halt 0x25004) and core gcc_blsp2_qup3_i2c_apps (gcc-sdm660.c: CBCR
 *   0x2a008, RCG 0x2a020), qcom,clk-freq-in 19.2 MHz = XO, clk-freq-out
 *   400 kHz; pins gpio26/27 function blsp_i2c7 = mux 3 (pinctrl-sdm660.c
 *   PINGROUP 26/27: blsp_spi7, blsp_uart6_a, blsp_i2c7), 2 mA, no pull,
 *   NORTH tile 0x3900000; tas2557@4c ti,cdc-reset-gpio 0x49 = GPIO73).
 *   QUP v2 FIFO mode and tags exactly as drg_ts.c (proven on the touch
 *   controller, boot #31). tas2557-regmap.c tas2557_hw_reset: reset low,
 *   5 ms, high, 2 ms. Registers (tas2557.h, book 0 page 0 = the reset
 *   default, so no page/book write is needed: nothing is written to the
 *   amp): 0x00 page, 0x7f book, 0x03 REV_PGID, 0x25 (37) SAFE_GUARD.
 *   Afterwards pins, reset line, clocks and the vote go back to what they
 *   were (the reset line as ABL left it).
 * (b) ADSP: qcom,lpass@15700000 "qcom,pil-tz-generic", qcom,pas-id 1,
 *   memory-region adsp_fw_mem 0x92a00000 + 0x1e00000 (removed-dma-pool),
 *   proxy vdd_cx = pm660l_l9_level (RPM "rwlc" 0, use-voltage-level) uV-uA
 *   <0x180 100000>, proxy clock xo = cxo_pil_lpass_clk (a voter on cxo =
 *   RPM "clk0" 0 "Enab"), no mem-protect-id (no hyp_assign), smem-id 0x1a7
 *   (SMEM_SSR_REASON_LPASS0), err-ready on SMP2P (adsp, remote pid 2,
 *   "slave-kernel" bit 1), wdog interrupt <0 0xa2 1>. Firmware adsp.mdt +
 *   adsp.bNN from this phone's modem_a /image (read-only FAT, CRCs below
 *   from the 2026-11-06 backup). Sequence = peripheral-loader.c pil_boot +
 *   subsys-pil-tz.c pil_ops_trusted: region from the loadable segments,
 *   proxy votes, PAS_INIT_IMAGE (x0 0x42000201, arginfo 0x82 = SCM_ARGS(2,
 *   VAL, RW): pas id, PA of the whole .mdt), PAS_MEM_SETUP (0x42000202,
 *   arginfo 3: id, region start, size), each segment copied and zeroed to
 *   memsz, PAS_AUTH_AND_RESET (0x42000205, arginfo 1: id); then watched
 *   5 s; then pil_shutdown_trusted: PAS_SHUTDOWN (0x42000206, arginfo 1)
 *   with the proxy votes on, and the votes removed. SMCs as drg_gpu.c's zap
 *   load (PAS 0xd, proven boot #30+) on a spare Gold core.
 * Deviations, logged: no scm_pas bus vote (as the zap load); the force-stop
 *   SMP2P bit is not raised before PAS_SHUTDOWN (no outbound SMP2P here);
 *   after a failed auth/load PAS_SHUTDOWN is still sent so TZ releases the
 *   region [E: Linux leaves that to the next subsystem restart]; the XO vote
 *   is left on (W1 leaves the same vote on).
 * -DDRG_A1_NO_ADSP: only (a).
 */
#include <stdint.h>
#include "drg_a1.h"
#include "drg_fat.h"
#include "drg_part.h"
#include "drg_smem.h"
#include "drg_rpm.h"
#include "drg_gpu.h"
#include "console.h"

#define GCC			0x00100000UL
#define GCC_VOTE		(GCC + 0x52004)
#define BLSP2_AHB_BIT		15
#define BLSP2_AHB_HALT		(GCC + 0x25004)
#define QUP3_CBCR		(GCC + 0x2a008)
#define QUP3_RCG		(GCC + 0x2a020)		/* CMD; CFG +4 */
#define TLMM_NORTH		0x03900000UL
#define GPIO_SDA		26
#define GPIO_SCL		27
#define GPIO_RESET		73
#define QUP			0x0c1b7000UL
#define TAS_ADDR		0x4c

#define QUP_CONFIG		0x000
#define QUP_STATE		0x004
#define QUP_IO_MODES		0x008
#define QUP_SW_RESET		0x00c
#define QUP_OPERATIONAL		0x018
#define QUP_ERROR_FLAGS		0x01c
#define QUP_ERROR_FLAGS_EN	0x020
#define QUP_OPERATIONAL_MASK	0x028
#define QUP_MX_OUTPUT_COUNT	0x100
#define QUP_OUT_FIFO_BASE	0x110
#define QUP_MX_WRITE_COUNT	0x150
#define QUP_MX_INPUT_COUNT	0x200
#define QUP_MX_READ_COUNT	0x208
#define QUP_IN_FIFO_BASE	0x218
#define QUP_MASTER_CLK_CTL	0x400
#define QUP_I2C_STATUS		0x404
#define QUP_MASTER_CONFIG	0x408

#define ADSP_REGION		0x92a00000UL
#define ADSP_REGION_SIZE	0x01e00000UL
#define ADSP_WDOG_INTID		(32 + 0xa2)
#define GICD			0x17a00000UL
#define PAS_ID_ADSP		1
#define SCM_PIL(cmd)		(0x42000200UL | (cmd))
#define PAS_INIT_IMAGE		1
#define PAS_MEM_SETUP		2
#define PAS_AUTH_AND_RESET	5
#define PAS_SHUTDOWN		6
#define SCM_INTERRUPTED		1
#define SCM_V2_EBUSY		(-12)
#define RPM_RES_RWLC		0x636c7772u	/* "rwlc": pm660l_l9_level */
#define RPM_RES_CLK0		0x306b6c63u	/* "clk0" */
#define RPM_KEY_ENAB		0x62616e45u	/* "Enab" */
#define RPM_KEY_MA		0x0000616du	/* "ma" */
#define LEVEL_TURBO		0x180
#define WATCH_MS		5000
#define MAX_PHDR		32

static drg_log_fn logp;
static drg_flush_fn flushp;

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
#ifndef DRG_HOST_SIM
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint32_t qrd(uint32_t o) { return rd32(QUP + o); }
static inline void qwr(uint32_t o, uint32_t v) { wr32(QUP + o, v); dsb(); }
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
#else	/* host simulation (shim/test/hwtest_sim.c) */
uint64_t sim_now(void);
uint64_t sim_freq(void);
void sim_smc(volatile uint64_t *x);
uint32_t sim_qrd(uint32_t o);
void sim_qwr(uint32_t o, uint32_t v);
static inline void dsb(void) { __sync_synchronize(); }
static inline uint32_t qrd(uint32_t o) { return sim_qrd(o); }
static inline void qwr(uint32_t o, uint32_t v) { sim_qwr(o, v); }
static uint64_t now(void) { return sim_now(); }
static uint64_t freq(void) { return sim_freq(); }
#endif

static void udelay(uint32_t us)
{
	uint64_t t0 = now(), n = (freq() * us + 999999) / 1000000;
	while (now() - t0 < n)
		;
}

static uint32_t ms_since(uint64_t t0) { return (uint32_t)((now() - t0) * 1000 / freq()); }
static void L(const char *label, uint32_t a, uint32_t v) { logp(label, a, v, 0); }
static void LX(const char *label, uint32_t a, uint32_t v) { logp(label, a, v, -1); }

/* ---- (a) TAS2557 ---- */

static uint32_t saved_cfg[3], saved_io[3], saved_vote, saved_cbcr, saved_rcg[2];
static int a_touched, qup_ok;
static const uint8_t a_pins[3] = { GPIO_SDA, GPIO_SCL, GPIO_RESET };

static uintptr_t pin(unsigned n) { return TLMM_NORTH + 0x1000UL * n; }

static int wait_branch(uintptr_t halt)
{
	for (int k = 0; k < 200; k++) {
		uint32_t v = rd32(halt) & (0x80000000u | (7u << 28));
		if (!(v & 0x80000000u) || v == (2u << 28))
			return 0;
		udelay(1);
	}
	return -1;
}

/* drg_ts.c qup_state, bounded by time */
static int qup_state(uint32_t state)
{
	uint64_t t0 = now();
	while (!(qrd(QUP_STATE) & 4) && ms_since(t0) < 10)
		;
	qwr(QUP_STATE, state);
	t0 = now();
	while (ms_since(t0) < 10) {
		uint32_t s = qrd(QUP_STATE);
		if ((s & 4) && (s & 3) == state)
			return 0;
	}
	return -1;
}

/* one register of the amp: write reg, repeated start, read 1 byte (i2c-msm-v2 tags v2, as drg_ts.c); 0 ok */
static int tas_read(uint8_t reg, uint32_t *val)
{
	qwr(QUP_SW_RESET, 1);
	udelay(10);
	if (qup_state(0))
		return -2;
	qwr(QUP_CONFIG, 0x207);
	qwr(QUP_ERROR_FLAGS_EN, 0x3c);
	qwr(QUP_OPERATIONAL_MASK, 0x300);
	qwr(QUP_MASTER_CONFIG, 1);
	qwr(QUP_MX_INPUT_COUNT, 0);
	qwr(QUP_MX_OUTPUT_COUNT, 0);
	qwr(QUP_MX_READ_COUNT, 1 + 2);
	qwr(QUP_MX_WRITE_COUNT, 9);
	qwr(QUP_IO_MODES, (qrd(QUP_IO_MODES) & ~0x1fc00u) | (1u << 14) | (1u << 15));
	qwr(QUP_OPERATIONAL_MASK, 0);
	if (qup_state(1))
		return -4;
	qwr(QUP_MASTER_CLK_CTL, (14u << 16) | 28u);	/* 400 kHz from 19.2 MHz */
	if (qup_state(3))
		return -5;
	/* 81 98 82 01 | reg | 81 99 87 01: START w, DATA_WRITE 1, reg, START r, DATA_READ_N_STOP 1 */
	qwr(QUP_OUT_FIFO_BASE, 0x01820081u | (TAS_ADDR << 9));
	qwr(QUP_OUT_FIFO_BASE, 0x87008100u | reg | ((TAS_ADDR << 1 | 1u) << 16));
	qwr(QUP_OUT_FIFO_BASE, 0x00000001u);
	if (qup_state(1))
		return -6;
	uint32_t op = 0;
	uint64_t t0 = now();
	while (ms_since(t0) < 50 && !((op = qrd(QUP_OPERATIONAL)) & (1u << 11)))
		udelay(100);
	uint32_t st = qrd(QUP_I2C_STATUS), err = qrd(QUP_ERROR_FLAGS);
	if (!(op & (1u << 11)) || err) {
		LX("a1 i2c reg/op", reg, op);
		LX(" i2c status/err", st, err);
		(void)qup_state(0);
		return -7;
	}
	uint32_t w = qrd(QUP_IN_FIFO_BASE);	/* 2 input tag bytes, then the data */
	*val = (w >> 16) & 0xff;
	(void)qup_state(0);
	return 0;
}

static void a_restore(void)
{
	if (!a_touched)
		return;
	if (qup_ok) {
		qwr(QUP_SW_RESET, 1);
		qup_ok = 0;
	}
	for (int i = 2; i >= 0; i--) {	/* reset line first back as it was, then the bus pins */
		wr32(pin(a_pins[i]) + 4, saved_io[i] & 2);
		wr32(pin(a_pins[i]), saved_cfg[i]);
	}
	dsb();
	L("a1 pin73 cfg/io now", rd32(pin(GPIO_RESET)), rd32(pin(GPIO_RESET) + 4));
	wr32(QUP3_CBCR, (rd32(QUP3_CBCR) & ~1u) | (saved_cbcr & 1));
	wr32(QUP3_RCG + 4, saved_rcg[1]);
	wr32(QUP3_RCG, rd32(QUP3_RCG) | 1);
	for (int k = 0; k < 500 && (rd32(QUP3_RCG) & 1); k++)
		udelay(1);
	if (!(saved_vote & (1u << BLSP2_AHB_BIT)))
		wr32(GCC_VOTE, rd32(GCC_VOTE) & ~(1u << BLSP2_AHB_BIT));
	dsb();
	L("a1 clk restore cbcr/vote", rd32(QUP3_CBCR), rd32(GCC_VOTE));
	a_touched = 0;
}

static int tas_probe(void)
{
	for (int i = 0; i < 3; i++) {
		saved_cfg[i] = rd32(pin(a_pins[i]));
		saved_io[i] = rd32(pin(a_pins[i]) + 4);
	}
	saved_vote = rd32(GCC_VOTE);
	saved_cbcr = rd32(QUP3_CBCR);
	saved_rcg[0] = rd32(QUP3_RCG), saved_rcg[1] = rd32(QUP3_RCG + 4);
	L("a1 saved pin26/27 cfg", saved_cfg[0], saved_cfg[1]);
	L("a1 saved pin73 cfg/io", saved_cfg[2], saved_io[2]);
	L("a1 saved vote/cbcr", saved_vote, saved_cbcr);
	L("a1 saved rcg cmd/cfg", saved_rcg[0], saved_rcg[1]);
	a_touched = 1;
	/* clocks: AHB vote, RCG = XO (source 0, divider 1), branch */
	wr32(GCC_VOTE, rd32(GCC_VOTE) | (1u << BLSP2_AHB_BIT));
	dsb();
	udelay(5);
	if (wait_branch(BLSP2_AHB_HALT)) {
		LX("a1 blsp2 ahb halt", (uint32_t)BLSP2_AHB_HALT, rd32(BLSP2_AHB_HALT));
		return -1;
	}
	wr32(QUP3_RCG + 4, (rd32(QUP3_RCG + 4) & ~(0x1fu | (7u << 8) | (3u << 12))) | 1u);
	wr32(QUP3_RCG, rd32(QUP3_RCG) | 1);
	dsb();
	int k = 0;
	for (; k < 500 && (rd32(QUP3_RCG) & 1); k++)
		udelay(1);
	wr32(QUP3_CBCR, rd32(QUP3_CBCR) | 1);
	dsb();
	int rc = wait_branch(QUP3_CBCR);
	logp("a1 rcg cfg/cbcr", rd32(QUP3_RCG + 4), rd32(QUP3_CBCR), rc || k >= 500 ? -1 : 0);
	if (rc || k >= 500)
		return -2;
	/* pins: blsp_i2c7 (mux 3), 2 mA, no pull */
	wr32(pin(GPIO_SDA), 3u << 2);
	wr32(pin(GPIO_SCL), 3u << 2);
	dsb();
	/* first QUP access on a spare core */
	{
		uint32_t v = 0;
		uint64_t esr = 0;
		int r = -2;
		for (int c = 5; c <= 7 && (r == -2 || r == -4); c++)
			r = drg_probe_read(c, QUP + QUP_STATE, &v, &esr, 100000);
		logp("a1 qup probe rc/state", (uint32_t)r, v, r ? -1 : 0);
		if (r) {
			L("a1 qup probe esr", (uint32_t)(esr >> 32), (uint32_t)esr);
			return -3;
		}
		qup_ok = 1;
	}
	/* tas2557_hw_reset: low 5 ms, high 2 ms (gpio, output, 2 mA, no pull) */
	wr32(pin(GPIO_RESET) + 4, 0);
	wr32(pin(GPIO_RESET), 1u << 9);
	dsb();
	udelay(5000);
	wr32(pin(GPIO_RESET) + 4, 2);
	dsb();
	udelay(2000 + 1000);
	L("a1 reset pulsed cfg/io", rd32(pin(GPIO_RESET)), rd32(pin(GPIO_RESET) + 4));
	static const uint8_t regs[4] = { 0x00, 0x7f, 0x03, 0x25 };
	uint32_t vals[4] = { 0 }, bad = 0;
	for (int i = 0; i < 4; i++) {
		int r = tas_read(regs[i], &vals[i]);
		logp("a1 tas reg/val", regs[i], r ? (uint32_t)r : vals[i], r ? -1 : 0);
		bad += r != 0;
	}
	/* REV_PGID: upper nibble revision, lower PGID [E: exact meaning per TI datasheet, not in HMD's driver] */
	if (!bad)
		con_hex2("A1", "TAS2557 rev/pgid=", vals[2], " safeguard=", vals[3]);
	else
		con_str("A1", "TAS2557 did not answer");
	return bad ? -4 : 0;
}

/* ---- (b) ADSP ---- */

#ifndef DRG_A1_NO_ADSP
static volatile uint64_t smc_x[7];
static volatile uint32_t smc_rounds;
static int core_lost, sp_rr;

#ifdef DRG_HOST_SIM
static void smc_fn(void) { sim_smc(smc_x); smc_rounds = 1; }
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
	do {
		__asm__ volatile("smc #0"
				 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
				 :
				 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		smc_rounds = ++n;
	} while (r0 == SCM_INTERRUPTED && n < 100000);
	smc_x[0] = r0, smc_x[1] = r1, smc_x[2] = r2, smc_x[3] = r3;
}
#endif

/* one PAS call on a spare core: 0 when x0 == 0 and the TZ result (x1) == 0 */
static int pas(const char *label, uint32_t cmd, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2)
{
	if (core_lost) {
		LX(label, 0xdead, 0);
		return -1;
	}
	for (unsigned tries = 0;; tries++) {
		smc_x[0] = SCM_PIL(cmd), smc_x[1] = arginfo, smc_x[2] = a0, smc_x[3] = a1, smc_x[4] = a2, smc_x[5] = 0;
		smc_rounds = 0;
		dsb();
		uint64_t esr = 0, t0 = now();
		int rc = -2;
		for (int a = 0; a < 6 && rc == -2; a++) {
			rc = drg_probe_call(5 + (sp_rr++ % 3), smc_fn, &esr, 3000000);
			if (rc == -2)
				udelay(10000);
		}
		uint32_t us = (uint32_t)((now() - t0) * 1000000 / freq());
		if (rc) {
			LX(label, 0xc0de0000u | ((uint32_t)rc & 0xffff), smc_rounds);
			LX(" smc esr", (uint32_t)(esr >> 32), (uint32_t)esr);
			if (rc != -3)
				core_lost = 1;
			return -1;
		}
		int64_t x0 = (int64_t)smc_x[0];
		int ok = x0 == 0 && smc_x[1] == 0;
		logp(label, (uint32_t)x0, (uint32_t)smc_x[1], ok ? 0 : -1);
		L(" pas us/rounds", us, smc_rounds);
		if (x0 == SCM_V2_EBUSY && tries < 67) {
			udelay(30000);
			continue;
		}
		return ok ? 0 : -1;
	}
}

static int rpm_vote(const char *label, uint32_t type, const uint32_t *kvp, uint32_t words)
{
	int rc = drg_rpm_request_kvp(type, 0, kvp, words);
	uint32_t w[4];
	drg_rpm_last_ack(w);
	logp(label, (uint32_t)rc, w[1], rc ? -1 : !rc && w[1] != 12 ? -2 : 0);
	return rc;
}

static int voted;
static void unvote(void)
{
	if (!voted)
		return;
	uint32_t cx[9] = { RPM_KEY_SWEN, 4, 0, RPM_KEY_VLVL, 4, 0, RPM_KEY_MA, 4, 0 };	/* disable_regulators */
	(void)rpm_vote("a1 rpm rwlc off", RPM_RES_RWLC, cx, 9);
	L("a1 xo vote left on", 0, 0);
	voted = 0;
}

static uint8_t amdt[8192] __attribute__((aligned(4096)));
static struct fat16 afs;
static uint64_t apart_first;
struct aseg {
	uint32_t num, pa, filesz, memsz;
	struct fat16_ent ent;
};
static struct aseg asegs[MAX_PHDR];
static uint32_t nasegs;

static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* CRC-32 of adsp.mdt / adsp.bNN in the 2026-11-06 backup of modem_a (b00/b01: not loaded, b23 absent) */
#define CRC_AMDT	0xc5725844u
#define SIZE_AMDT	8028u
static const uint32_t aseg_crc[24] = {
	0, 0, 0x607db180, 0x88fd26ea, 0xa7fa289a, 0xbfe22bd1, 0xfe645957, 0x1caa4630,
	0xc1cc4c52, 0x60cc352e, 0xa7309430, 0xe3edaa9e, 0x513fe36d, 0x03f5002b, 0x9cdc9c19, 0xdcd1cbd8,
	0x9932f727, 0xa86dc3be, 0xce95f787, 0x41cbc15e, 0xb46873eb, 0x86f84294, 0xcbd4a3e8, 0,
};

static volatile uintptr_t touch_addr;
static volatile uint32_t touch_back;
static void touch_fn(void)
{
	volatile uint32_t *p = (volatile uint32_t *)touch_addr;
	*p = 0x41314131u;	/* "A1A1" */
	dsb();
	touch_back = *p;
}
static int touch(uintptr_t a)
{
	uint64_t esr = 0;
	touch_addr = a, touch_back = 0;
	dsb();
	int rc = -2;
	for (int c = 5; c <= 7 && (rc == -2 || rc == -4); c++)
		rc = drg_probe_call(c, touch_fn, &esr, 100000);
	logp("a1 ram touch", (uint32_t)a, rc ? (uint32_t)rc : touch_back, rc || touch_back != 0x41314131u ? -1 : 0);
	return rc || touch_back != 0x41314131u ? -1 : 0;
}

static void zero(uintptr_t a, uint32_t n)
{
	while (n && (a & 7))
		*(volatile uint8_t *)a++ = 0, n--;
	for (; n >= 8; n -= 8, a += 8)
		*(volatile uint64_t *)a = 0;
	while (n--)
		*(volatile uint8_t *)a++ = 0;
}

static void segname(char *p, uint32_t num)	/* "image/adsp.bNN" */
{
	const char *s = "image/adsp.b";
	int i = 0;
	for (; s[i]; i++)
		p[i] = s[i];
	p[i++] = (char)('0' + num / 10 % 10);
	p[i++] = (char)('0' + num % 10);
	p[i] = 0;
}

static void adsp_watch(void)
{
	uint32_t last[4] = { ~0u, ~0u, ~0u, ~0u }, lines = 0;
	uintptr_t pend_reg = GICD + 0x200 + 4 * (ADSP_WDOG_INTID / 32);
	int smem = drg_smem_init(logp, "a1");
	uint64_t t0 = now();
	for (;;) {
		uint32_t ms = ms_since(t0), cur[4];
		int found = 0;
		cur[0] = smem ? 0xdeadu : drg_smp2p_entry(SMEM_SMP2P_AUDIO_BASE + 0, SMEM_HOST_Q6, "slave-kernel", &found, 0, 0);
		if (!smem && !found)
			cur[0] = 0xffffffffu;
		cur[1] = smem ? 0 : drg_smem_string(SMEM_SSR_REASON_LPASS0, SMEM_HOST_ANY, 0, 0);
		cur[2] = (rd32(pend_reg) >> (ADSP_WDOG_INTID % 32)) & 1;
		uint32_t sz = 0;
		cur[3] = smem ? 0 : (drg_smem_item(SMEM_GLINK_DESC, SMEM_HOST_Q6, &sz) ? 1 : 0) |
					    (drg_smem_item(SMEM_GLINK_FIFO_1, SMEM_HOST_Q6, &sz) ? 4 : 0);
		if ((cur[0] != last[0] || cur[1] != last[1] || cur[2] != last[2] || cur[3] != last[3]) && lines < 16) {
			logp("a1 t ms/smp2p", ms, cur[0], 0);
			logp(" sfr0/wdog.glink", cur[1], cur[2] << 16 | cur[3], cur[1] || cur[2] ? -1 : 0);
			lines += 2;
			for (int i = 0; i < 4; i++)
				last[i] = cur[i];
		}
		if (ms >= WATCH_MS)
			break;
		udelay(250000);
	}
	if (!smem) {
		int found = 0;
		(void)drg_smp2p_entry(SMEM_SMP2P_AUDIO_BASE + 0, SMEM_HOST_Q6, "slave-kernel", &found, logp, "a1");
		(void)drg_smem_string(SMEM_SSR_REASON_LPASS0, SMEM_HOST_ANY, logp, "a1 sfr id/len");
	}
	if (!smem && (last[0] & 2) && last[0] != 0xffffffffu)
		con_str("A1", "ADSP err-ready (SMP2P bit 1)");
}

static int astop(int step, const char *why)
{
	LX("a1 adsp STOP step", (uint32_t)step, 0);
	LX(why, (uint32_t)step, 0);
	con_hex2("A1", "ADSP STOP step=", (uint32_t)step, "", 0);
	flushp();
	return -step;
}

static int adsp_run(void)
{
	L("a1 adsp start", 0x41310002, PAS_ID_ADSP);
	con_str("A1", "ADSP boot via TZ PAS 1");
	/* 1. modem_a, adsp.mdt */
	uint64_t first = 0, last = 0;
	uint32_t idx = 0;
	int rc = drg_gpt_find("modem_a", &first, &last, &idx);
	logp("a1 gpt rc/index", (uint32_t)rc, idx, rc ? -1 : 0);
	if (rc || last < first || last - first + 1 > 0xffffffffULL)
		return astop(1, "a1 gpt");
	apart_first = first;
	afs.read = drg_part_read;
	afs.ctx = &apart_first;
	rc = fat16_mount(&afs, (uint32_t)(last - first + 1));
	if (rc) {
		LX("a1 fat rc", (uint32_t)rc, 0);
		return astop(1, "a1 fat");
	}
	struct fat16_ent e;
	rc = fat16_lookup(&afs, "image/adsp.mdt", &e);
	int32_t n = rc ? rc : fat16_read(&afs, &e, amdt, sizeof(amdt), 0);
	uint32_t crc = n > 0 ? drg_crc32(0, amdt, (uint32_t)n) : 0;
	logp("a1 mdt size/crc", (uint32_t)n, crc, n == (int32_t)SIZE_AMDT && crc == CRC_AMDT ? 0 : -2);
	if (n < 52 || ((uintptr_t)amdt >> 32))
		return astop(1, "a1 mdt read");
	uint32_t msz = (uint32_t)n;
	/* 2. ELF32 program headers; pil_init_mmap region; all relocatable, must be the DT region */
	uint32_t phoff = g32(amdt + 28), phent = (uint32_t)(amdt[42] | amdt[43] << 8);
	uint32_t phnum = (uint32_t)(amdt[44] | amdt[45] << 8);
	if (g32(amdt) != 0x464c457f || amdt[4] != 1 || phent != 32 || !phnum || phnum > MAX_PHDR || phoff + phnum * 32 > msz)
		return astop(2, "a1 mdt elf");
	uint32_t min_r = 0xffffffffu, max_r = 0;
	nasegs = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = amdt + phoff + 32 * i;
		uint32_t type = g32(ph), pa = g32(ph + 12), fsz = g32(ph + 16), mem = g32(ph + 20), fl = g32(ph + 24);
		if (!(type == 1 && ((fl >> 24) & 7) != 2 && mem))
			continue;
		if (!(fl & (1u << 27)) || fsz > mem || (uint64_t)pa + mem > 0xffffffffULL) {
			LX("a1 seg unsupp num/flags", i, fl);
			return astop(2, "a1 segment");
		}
		if (pa < min_r)
			min_r = pa;
		if (pa + mem > max_r)
			max_r = pa + mem;
		struct aseg *s = &asegs[nasegs++];
		s->num = i, s->pa = pa, s->filesz = fsz, s->memsz = mem;
	}
	max_r = (max_r + 0xfff) & ~0xfffu;
	L("a1 region min/max", min_r, max_r);
	L("a1 segs/entry", nasegs, g32(amdt + 24));
	/* removed-dma-pool: the region starts at the pool base; the image must fit it */
	uint32_t rsize = max_r - min_r;
	if (!nasegs || rsize > ADSP_REGION_SIZE)
		return astop(2, "a1 region");
	for (uint32_t i = 0; i < nasegs; i++) {
		struct aseg *s = &asegs[i];
		s->pa = s->pa - min_r + (uint32_t)ADSP_REGION;
		if (i && s->pa < asegs[i - 1].pa + asegs[i - 1].memsz)
			return astop(2, "a1 segment order");
		if (!s->filesz)
			continue;
		char name[20];
		segname(name, s->num);
		rc = fat16_lookup(&afs, name, &s->ent);
		if (rc || s->ent.size != s->filesz) {
			LX("a1 seg file num/rc", s->num, (uint32_t)rc);
			return astop(2, "a1 segment file");
		}
	}
	/* 3. the region is writable from here (spare core catches a fault) */
	if (touch(ADSP_REGION) || touch(ADSP_REGION + rsize - 4))
		return astop(3, "a1 ram");
	flushp();

	/* 4. proxy votes: vdd_cx (pm660l_l9_level) level 0x180, 100 mA, enable; xo */
	{
		uint32_t cx[9] = { RPM_KEY_SWEN, 4, 1, RPM_KEY_VLVL, 4, LEVEL_TURBO, RPM_KEY_MA, 4, 100 };
		voted = 1;
		if (rpm_vote("a1 rpm rwlc vlvl180 ma100", RPM_RES_RWLC, cx, 9)) {
			unvote();
			return astop(4, "a1 cx vote");
		}
		uint32_t xo[3] = { RPM_KEY_ENAB, 4, 1 };
		if (rpm_vote("a1 rpm xo clk0 Enab", RPM_RES_CLK0, xo, 3)) {
			unvote();
			return astop(4, "a1 xo vote");
		}
	}
	/* 5. PAS_INIT_IMAGE with the whole .mdt */
	dsb();
	if (pas("a1 pas init x0/x1", PAS_INIT_IMAGE, 0x82, PAS_ID_ADSP, (uintptr_t)amdt, 0)) {
		unvote();
		return astop(5, "a1 init_image");
	}
	/* 6. PAS_MEM_SETUP(region, size) */
	if (pas("a1 pas memsetup x0/x1", PAS_MEM_SETUP, 0x3, PAS_ID_ADSP, ADSP_REGION, rsize)) {
		unvote();
		return astop(6, "a1 mem_setup");
	}
	flushp();
	/* 7. segments */
	{
		uint64_t t0 = now();
		uint32_t bad = 0;
		for (uint32_t i = 0; i < nasegs; i++) {
			struct aseg *s = &asegs[i];
			int32_t got = 0;
			uint32_t c = 0;
			if (s->filesz) {
				got = fat16_read(&afs, &s->ent, (uint8_t *)(uintptr_t)s->pa, s->filesz, 0);
				c = got > 0 ? drg_crc32(0, (const volatile uint8_t *)(uintptr_t)s->pa, (uint32_t)got) : 0;
			}
			if (got != (int32_t)s->filesz) {
				LX("a1 load num/rc", s->num, (uint32_t)got);
				(void)pas("a1 pas shutdown x0/x1", PAS_SHUTDOWN, 0x1, PAS_ID_ADSP, 0, 0);
				unvote();
				return astop(7, "a1 segment read");
			}
			zero(s->pa + s->filesz, s->memsz - s->filesz);
			uint32_t want = s->num < 24 ? aseg_crc[s->num] : 0;
			int ok = !s->filesz || c == want;
			bad += !ok;
			if (!ok)
				logp("a1 load num/crc BAD", s->num, c, -2);
		}
		dsb();
		L("a1 load ms/bad crcs", ms_since(t0), bad);
	}
	flushp();
	/* 8. PAS_AUTH_AND_RESET */
	if (pas("a1 pas auth x0/x1", PAS_AUTH_AND_RESET, 0x1, PAS_ID_ADSP, 0, 0)) {
		(void)drg_smem_init(logp, "a1");
		(void)drg_smem_string(SMEM_SSR_REASON_LPASS0, SMEM_HOST_ANY, logp, "a1 sfr id/len");
		(void)pas("a1 pas shutdown x0/x1", PAS_SHUTDOWN, 0x1, PAS_ID_ADSP, 0, 0);
		unvote();
		return astop(8, "a1 auth_and_reset");
	}
	L("a1 ADSP AUTHENTICATED", 0, 0);
	con_str("A1", "ADSP out of reset (TZ)");
	flushp();
	/* 9. watch, 10. shut down (pil_shutdown_trusted) */
	adsp_watch();
	flushp();
	rc = pas("a1 pas shutdown x0/x1", PAS_SHUTDOWN, 0x1, PAS_ID_ADSP, 0, 0);
	unvote();
	con_str("A1", rc ? "ADSP shutdown FAILED" : "ADSP shut down");
	return rc ? -10 : 0;
}
#endif

int drg_a1_test(drg_log_fn log, drg_flush_fn fl)
{
	logp = log;
	flushp = fl;
	a_touched = qup_ok = 0;
	L("a1 start", 0x41310001, 0);
	con_str("A1", "audio: TAS2557 probe, ADSP boot");
	int ra = tas_probe();
	a_restore();
	L("a1 tas rc", (uint32_t)ra, 0);
	flushp();
	int rb = 0;
#ifndef DRG_A1_NO_ADSP
	core_lost = 0;
	voted = 0;
	rb = adsp_run();
	L("a1 adsp rc", (uint32_t)rb, 0);
#endif
	L("a1 end rc", (uint32_t)ra, (uint32_t)rb);
	flushp();
	return ra ? ra : rb;
}
