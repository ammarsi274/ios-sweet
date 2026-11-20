/*
 * B1 (research/reports/wifi-bt-plan.md): is the WCN3990's Bluetooth alive?
 * Power it as HMD's kernel does, talk HCI over BLSP2 UART1 in PIO mode
 * (the BAM at 0xc184000 is not touched), log every byte, power it down.
 *
 * Facts (device-info/fdt.dts, research/hmd-drg-kernel):
 *   bt_wcn3990: core = pm660_l9 (RPM "ldoa" 9) 1.8-1.9 V, pa = pm660_l6
 *     ("ldoa" 6) 1.304-1.37 V, ldo = pm660_l19 ("ldoa" 19) 3.312-3.4 V,
 *     chip_pwd = pm660l_bob_pin1 ("bobb" 1, qcom,use-pin-ctrl-voltage1)
 *     3.6 V, current-level 1 uA each; clock <rpmcc 0x14> = RPM_RF_CLK1_PIN
 *     (qcom,rpmcc.h 20) = "clka" id 4 key "pccb" (clk-smd-rpm.c sdm660,
 *     DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL). bluetooth-power.c
 *     bluetooth_power(1): core, pa, ldo, chip_pwd (set_voltage(min),
 *     set_load, enable: one RPM message each, keys "swen", "uv"/"pcv1",
 *     "ma"; rpm-smd-regulator.c params[]), then the clock. No delays.
 *   uart@c1af000 "qcom,msm-hsuart-v14" (msm_serial_hs.c, _hwreg.h): core
 *     clock gcc_blsp2_uart1_apps_clk (CBCR 0x27004, RCG 0x2700c, mnd 16,
 *     hid 5, parent map 0: XO 0, GPLL0 1), iface gcc_blsp2_ahb_clk (vote
 *     0x52004 bit 15, halt 0x25004); 115200 and 2400 baud with a 7.3728 MHz
 *     clock (F(7372800, GPLL0, 1, 192, 15625)) and CSR 0xcc / 0x33
 *     (msm_hs_set_bps_locked); register offsets of the v1.4 core:
 *     MR1 0x00, MR2 0x04, IPR 0x18, RFWR 0x20, DMRX 0x34, DMEN 0x3c,
 *     NCF_TX 0x40, CSR 0xa0, SR 0xa4, CR 0xa8, MISR 0xac, IMR 0xb0,
 *     ISR 0xb4, RX_TOTAL_SNAP 0xbc, TF 0x100, RF 0x140.
 *   Pins (blsp2_uart1_*_active): GPIO16 TX, 17 CTS, 18 RX, 19 RFR,
 *     function blsp_uart5 = mux 1 (pinctrl-sdm660.c PINGROUP 16-19),
 *     2 mA, bias-disable; CENTER tile: cfg at 0x3500000 + 0x1000 * n.
 *   PIO receive as msm_serial.c msm_handle_rx_dm (stale event, snap count).
 *   HCI commands: Reset 0x0c03, Read_Local_Version 0x1001; Qualcomm
 *     EDL_PATCH_CMD_OPCODE 0xfc00 with EDL_PATCH_VER_REQ_CMD 0x19 (btqca.h).
 * Guesses, logged as such:
 *   [E] power pulses: 0xC0 at 2400 baud (power off), then 0xFC at 115200
 *     (power on), 100 ms boot time, flow control off for the pulse: upstream
 *     hci_qca.c qca_wcn3990_init / qca_send_power_pulse, from memory. HMD's
 *     4.4 hci_qca.c/btqca.c have no WCN3990 code and the vendor_b backup
 *     holds no readable BT HAL, so they could not be checked locally.
 *   [E] pm660_l5 0.8 V (icnss vdd-0.8-cx-mx, not in the BT node) is voted
 *     too, as the plan says; build with -DDRG_BT_NO_L5 to leave it out.
 * Power-down: what Linux does (regulators "swen" 0, clock 0) except the
 * BOB, which supplies much more than BT and is left as voted (3.6 V is the
 * DT value); pins, clock enables and the RCG go back to what they were.
 */
#include <stdint.h>
#include "drg_bt.h"
#include "drg_rpm.h"
#include "drg_gpu.h"
#include "console.h"

#define GCC			0x00100000UL
#define GCC_VOTE		(GCC + 0x52004)		/* APCS clock branch enable vote */
#define BLSP2_AHB_BIT		15
#define BLSP2_AHB_HALT		(GCC + 0x25004)
#define UART_CBCR		(GCC + 0x27004)
#define UART_RCG		(GCC + 0x2700c)		/* CMD, CFG +4, M +8, N +0xc, D +0x10 */
#define TLMM_CENTER		0x03500000UL
#define BT_GPIO_FIRST		16

#define UART			0x0c1af000UL
#define U_MR1			0x00
#define U_MR2			0x04
#define U_IPR			0x18
#define U_RFWR			0x20
#define U_DMRX			0x34
#define U_DMEN			0x3c
#define U_NCF_TX		0x40
#define U_CSR			0xa0
#define U_SR			0xa4
#define U_CR			0xa8
#define U_MISR			0xac
#define U_IMR			0xb0
#define U_ISR			0xb4
#define U_SNAP			0xbc
#define U_TF			0x100
#define U_RF			0x140

#define SR_RXRDY		(1u << 0)
#define SR_OVERRUN		(1u << 4)
#define SR_TXRDY		(1u << 2)
#define SR_TXEMT		(1u << 3)
#define ISR_RXSTALE		(1u << 3)
#define ISR_RXLEV		(1u << 4)
#define ISR_CTS			(1u << 6)	/* UARTDM_ISR_CURRENT_CTS_BMSK */
#define MR1_CTS_CTL		(1u << 6)
#define MR1_RX_RDY_CTL		(1u << 7)
#define CR_RX_EN		0x01
#define CR_TX_EN		0x04
#define CR_RESET_RX		0x10
#define CR_RESET_TX		0x20
#define CR_RESET_ERR		0x30
#define CR_RESET_BREAK		0x40
#define CR_RESET_STALE		0x80
#define CR_RESET_CTS		0x70
#define CR_CLEAR_TX_READY	0x300
#define CR_STALE_ENABLE		0x500
#define CR_FORCE_STALE		0x400
#define CR_RFR_LOW		0xd0

#define RPM_RES_LDOA		0x616f646cu	/* "ldoa" */
#define RPM_RES_BOBB		0x62626f62u	/* "bobb" */
#define RPM_RES_CLKA		0x616b6c63u	/* "clka" QCOM_SMD_RPM_CLK_BUF_A */
#define RPM_KEY_MA		0x0000616du	/* "ma" */
#define RPM_KEY_PCV1		0x31766370u	/* "pcv1" */
#define RPM_KEY_PCCB		0x62636370u	/* "pccb" QCOM_RPM_KEY_PIN_CTRL_CLK_BUFFER_ENABLE_KEY */

static drg_log_fn logp;
static drg_flush_fn flushp;

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
#ifndef DRG_HOST_SIM
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint32_t urd(uint32_t o) { return rd32(UART + o); }
static inline void uwr(uint32_t o, uint32_t v) { wr32(UART + o, v); }

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
#else	/* host simulation (shim/test/hwtest_sim.c): timer and a model of the UART + chip */
uint64_t sim_now(void);
uint64_t sim_freq(void);
uint32_t sim_urd(uint32_t o);
void sim_uwr(uint32_t o, uint32_t v);
static inline void dsb(void) { __sync_synchronize(); }
static inline uint32_t urd(uint32_t o) { return sim_urd(o); }
static inline void uwr(uint32_t o, uint32_t v) { sim_uwr(o, v); }
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

/* ---- RPM ---- */

static int voted_l5, voted_l9, voted_l6, voted_l19, voted_bob, voted_clk;

static int rpm(const char *label, uint32_t type, uint32_t id, const uint32_t *kvp, uint32_t words)
{
	int rc = drg_rpm_request_kvp(type, id, kvp, words);
	uint32_t w[4];
	drg_rpm_last_ack(w);
	logp(label, (uint32_t)rc, w[1], rc ? -1 : 0);
	if (!rc && w[1] != 12)	/* v0 ack req_len 12 = no error kvp (rpm-smd.c) */
		LX("bt rpm NACK? len/w3", w[1], w[3]);
	return rc;
}

static int ldo(const char *label, uint32_t id, uint32_t on, uint32_t uv)
{
	uint32_t k[9] = { RPM_KEY_SWEN, 4, on, RPM_KEY_UV, 4, uv, RPM_KEY_MA, 4, 0 };	/* 1 uA -> 0 mA */
	return rpm(label, RPM_RES_LDOA, id, k, on ? 9 : 3);
}

static void power_down_rpm(void)
{
	if (voted_clk) {
		uint32_t k[3] = { RPM_KEY_PCCB, 4, 0 };
		(void)rpm("bt rpm rf_clk1_pin 0", RPM_RES_CLKA, 4, k, 3);
		voted_clk = 0;
	}
	if (voted_bob)
		L("bt bob pin1 left voted", 0x62626f62, 3600000);
	if (voted_l19)
		(void)ldo("bt rpm l19 off", 19, 0, 0);
	if (voted_l6)
		(void)ldo("bt rpm l6 off", 6, 0, 0);
	if (voted_l9)
		(void)ldo("bt rpm l9 off", 9, 0, 0);
	if (voted_l5)
		(void)ldo("bt rpm l5 off", 5, 0, 0);
	voted_l5 = voted_l9 = voted_l6 = voted_l19 = voted_bob = 0;
}

/* ---- clocks and pins, with what was there before ---- */

static uint32_t saved_pin[4], saved_vote, saved_cbcr, saved_rcg[5];
static int hw_touched, uart_ok;	/* uart_ok: UART registers proved accessible */

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

static void hw_restore(void)
{
	if (!hw_touched)
		return;
	if (uart_ok) {
		uwr(U_IMR, 0);
		uwr(U_CR, CR_RESET_RX);
		uwr(U_CR, CR_RESET_TX);
		dsb();
		uart_ok = 0;
	}
	for (int i = 0; i < 4; i++) {
		wr32(TLMM_CENTER + 0x1000UL * (BT_GPIO_FIRST + i), saved_pin[i]);
		L("bt pin restore", BT_GPIO_FIRST + i, rd32(TLMM_CENTER + 0x1000UL * (BT_GPIO_FIRST + i)));
	}
	for (int i = 1; i < 5; i++)	/* CFG, M, N, D; then the update */
		wr32(UART_RCG + 4 * i, saved_rcg[i]);
	wr32(UART_RCG, rd32(UART_RCG) | 1);
	for (int k = 0; k < 500 && (rd32(UART_RCG) & 1); k++)
		udelay(1);
	wr32(UART_CBCR, (rd32(UART_CBCR) & ~1u) | (saved_cbcr & 1));
	if (!(saved_vote & (1u << BLSP2_AHB_BIT)))
		wr32(GCC_VOTE, rd32(GCC_VOTE) & ~(1u << BLSP2_AHB_BIT));
	dsb();
	L("bt clk restore cbcr/vote", rd32(UART_CBCR), rd32(GCC_VOTE));
	L("bt rcg restore cmd/cfg", rd32(UART_RCG), rd32(UART_RCG + 4));
	hw_touched = 0;
}

/* ---- UART, PIO ---- */

static uint32_t old_snap;

static void stale_arm(void)
{
	uwr(U_CR, CR_RESET_STALE);
	uwr(U_DMRX, 0xffffff);
	uwr(U_CR, CR_STALE_ENABLE);
	dsb();
}

/* CSR code and stale count (msm_hs_set_bps_locked) */
static void set_baud(uint32_t csr, uint32_t rxstale)
{
	uint64_t t0 = now();
	while (!(urd(U_SR) & SR_TXEMT) && ms_since(t0) < 100)
		;
	uwr(U_CSR, csr);
	uwr(U_IPR, (rxstale & 0x1f) | ((rxstale << 2) & 0xffffff80u));
	uwr(U_CR, CR_RESET_TX);
	uwr(U_CR, CR_RESET_RX);
	uwr(U_CR, CR_RESET_ERR);
	uwr(U_CR, CR_RX_EN);
	uwr(U_CR, CR_TX_EN);
	old_snap = 0;
	stale_arm();
}

static void flow(int on)
{
	uint32_t mr1 = urd(U_MR1) & ~(MR1_CTS_CTL | MR1_RX_RDY_CTL);
	uwr(U_CR, CR_RFR_LOW);
	if (on)
		mr1 |= MR1_CTS_CTL | MR1_RX_RDY_CTL;
	uwr(U_MR1, mr1);
	dsb();
}

/* n bytes out; 0 when the transmitter ran empty within timeout_ms */
static int uart_tx(const uint8_t *b, uint32_t n, uint32_t timeout_ms)
{
	uint64_t t0 = now();
	uwr(U_CR, CR_CLEAR_TX_READY);
	uwr(U_NCF_TX, n);
	dsb();
	for (uint32_t i = 0; i < n; i += 4) {
		while (!(urd(U_SR) & SR_TXRDY))
			if (ms_since(t0) > timeout_ms)
				return -1;
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)b[i + k] << (8 * k);
		uwr(U_TF, w);
	}
	dsb();
	while (!(urd(U_SR) & SR_TXEMT))
		if (ms_since(t0) > timeout_ms)
			return -2;
	return 0;
}

static uint32_t rx_words(uint8_t *b, uint32_t got, uint32_t max, uint32_t count)
{
	while (count > 0 && (urd(U_SR) & SR_RXRDY)) {
		uint32_t w = urd(U_RF);
		for (uint32_t k = 0; k < 4 && k < count; k++, w >>= 8)
			if (got < max)
				b[got++] = (uint8_t)w;
		count = count > 4 ? count - 4 : 0;
	}
	return got;
}

/* bytes until a stale event ends a burst, or timeout_ms; returns the byte count */
static uint32_t uart_rx(uint8_t *b, uint32_t max, uint32_t timeout_ms)
{
	uint32_t got = 0;
	uint64_t t0 = now();
	while (ms_since(t0) < timeout_ms) {
		uint32_t isr = urd(U_ISR) | urd(U_MISR), sr = urd(U_SR);
		if (sr & SR_OVERRUN) {
			LX("bt rx overrun sr", sr, got);
			uwr(U_CR, CR_RESET_ERR);
		}
		if (isr & ISR_RXSTALE) {
			uint32_t snap = urd(U_SNAP);
			got = rx_words(b, got, max, snap - old_snap);
			old_snap = 0;
			stale_arm();
			if (got)
				return got;
		} else if (isr & ISR_RXLEV) {
			uint32_t c = 4 * urd(U_RFWR);
			got = rx_words(b, got, max, c);
			old_snap += c;
		}
	}
	/* nothing ended the burst: push out what sits in the packing buffer */
	if (urd(U_SR) & SR_RXRDY || got) {
		uwr(U_CR, CR_FORCE_STALE);
		udelay(1000);
		got = rx_words(b, got, max, urd(U_SNAP) - old_snap);
		old_snap = 0;
		stale_arm();
	}
	return got;
}

static void log_bytes(const char *label, const uint8_t *b, uint32_t n)
{
	L(label, n, n ? b[0] : 0);
	for (uint32_t i = 0; i < n && i < 64; i += 8) {
		uint32_t a = 0, v = 0;
		for (uint32_t k = 0; k < 4; k++) {
			a = a << 8 | (i + k < n ? b[i + k] : 0);
			v = v << 8 | (i + 4 + k < n ? b[i + 4 + k] : 0);
		}
		L(" bytes", a, v);	/* the bytes in order, 8 per line */
	}
}

static uint8_t rx[96];

/* one HCI command, its event logged; returns the byte count */
static uint32_t hci(const char *label, const uint8_t *cmd, uint32_t n)
{
	uint32_t isr = urd(U_ISR);
	L("bt cts/isr before tx", (isr & ISR_CTS) ? 1 : 0, isr);
	int rc = uart_tx(cmd, n, 200);
	if (rc) {
		LX("bt tx timeout rc/sr", (uint32_t)rc, urd(U_SR));
		uwr(U_CR, CR_RESET_TX);
		uwr(U_CR, CR_TX_EN);
		return 0;
	}
	uint32_t got = uart_rx(rx, sizeof(rx), 500);
	log_bytes(label, rx, got);
	return got;
}

static int stop(int step, const char *why)
{
	LX("bt STOP step", (uint32_t)step, 0);
	LX(why, (uint32_t)step, 0);
	con_hex2("BT", "STOP step=", (uint32_t)step, "", 0);
	return -step;
}

static int bt_run(void)
{
	/* 1. supplies and clock buffer, in bluetooth_power(1) order */
#ifndef DRG_BT_NO_L5
	if (ldo("bt rpm l5 0.8V [E]", 5, 1, 800000))
		return stop(1, "bt rpm l5");
	voted_l5 = 1;
#endif
	if (ldo("bt rpm l9 1.8V core", 9, 1, 1800000))
		return stop(1, "bt rpm l9");
	voted_l9 = 1;
	if (ldo("bt rpm l6 1.304V pa", 6, 1, 1304000))
		return stop(1, "bt rpm l6");
	voted_l6 = 1;
	if (ldo("bt rpm l19 3.312V ldo", 19, 1, 3312000))
		return stop(1, "bt rpm l19");
	voted_l19 = 1;
	{
		uint32_t k[6] = { RPM_KEY_SWEN, 4, 1, RPM_KEY_PCV1, 4, 3600000 };
		if (rpm("bt rpm bob pin1 3.6V", RPM_RES_BOBB, 1, k, 6))
			return stop(1, "bt rpm bob");
		voted_bob = 1;
	}
	{
		uint32_t k[3] = { RPM_KEY_PCCB, 4, 1 };
		if (rpm("bt rpm rf_clk1_pin 1", RPM_RES_CLKA, 4, k, 3))
			return stop(1, "bt rpm clk");
		voted_clk = 1;
	}

	/* 2. UART clocks: AHB vote, core branch, RCG 7.3728 MHz (GPLL0 * 192 / 15625) */
	for (int i = 0; i < 4; i++)
		saved_pin[i] = rd32(TLMM_CENTER + 0x1000UL * (BT_GPIO_FIRST + i));
	saved_vote = rd32(GCC_VOTE);
	saved_cbcr = rd32(UART_CBCR);
	for (int i = 0; i < 5; i++)
		saved_rcg[i] = rd32(UART_RCG + 4 * i);
	L("bt saved vote/cbcr", saved_vote, saved_cbcr);
	L("bt saved rcg cmd/cfg", saved_rcg[0], saved_rcg[1]);
	L("bt saved pins 16/17", saved_pin[0], saved_pin[1]);
	L("bt saved pins 18/19", saved_pin[2], saved_pin[3]);
	hw_touched = 1;
	wr32(GCC_VOTE, rd32(GCC_VOTE) | (1u << BLSP2_AHB_BIT));
	dsb();
	udelay(5);
	int rc = wait_branch(BLSP2_AHB_HALT);
	logp("bt blsp2 ahb halt", (uint32_t)BLSP2_AHB_HALT, rd32(BLSP2_AHB_HALT), rc ? -1 : 0);
	if (rc)
		return stop(2, "bt ahb clock");
	wr32(UART_CBCR, rd32(UART_CBCR) | 1);
	dsb();
	rc = wait_branch(UART_CBCR);
	logp("bt uart cbcr", (uint32_t)UART_CBCR, rd32(UART_CBCR), rc ? -1 : 0);
	if (rc)
		return stop(2, "bt uart clock");
	wr32(UART_RCG + 0x8, (rd32(UART_RCG + 0x8) & ~0xffffu) | 192);			/* M */
	wr32(UART_RCG + 0xc, (rd32(UART_RCG + 0xc) & ~0xffffu) | (~(15625u - 192u) & 0xffff));	/* N */
	wr32(UART_RCG + 0x10, (rd32(UART_RCG + 0x10) & ~0xffffu) | (~15625u & 0xffff));	/* D */
	/* CFG: pre_div 2*1-1, source 1 (GPLL0 main), dual edge (MND) */
	wr32(UART_RCG + 0x4, (rd32(UART_RCG + 0x4) & ~(0x1fu | (7u << 8) | (3u << 12))) | 1u | (1u << 8) | (2u << 12));
	wr32(UART_RCG, rd32(UART_RCG) | 1);	/* CMD_UPDATE */
	dsb();
	int k = 0;
	for (; k < 500 && (rd32(UART_RCG) & 1); k++)
		udelay(1);
	logp("bt rcg cmd/cfg", rd32(UART_RCG), rd32(UART_RCG + 4), k < 500 ? 0 : -1);
	L("bt rcg m/n", rd32(UART_RCG + 8), rd32(UART_RCG + 0xc));
	if (k >= 500)
		return stop(2, "bt rcg update");

	/* 3. pins: blsp_uart5 (mux 1), 2 mA, no pull */
	for (int i = 0; i < 4; i++)
		wr32(TLMM_CENTER + 0x1000UL * (BT_GPIO_FIRST + i), 1u << 2);
	dsb();
	L("bt pins 16/19 cfg", rd32(TLMM_CENTER + 0x1000UL * 16), rd32(TLMM_CENTER + 0x1000UL * 19));

	/* 4. first UART access on a spare core (a protection fault is caught there) */
	{
		uint32_t v = 0;
		uint64_t esr = 0;
		int r = -2;
		for (int c = 5; c <= 7 && r == -2; c++)
			r = drg_probe_read(c, UART + U_SR, &v, &esr, 100000);
		logp("bt uart probe sr", (uint32_t)r, v, r ? -1 : 0);
		if (r) {
			L("bt uart probe esr", (uint32_t)(esr >> 32), (uint32_t)esr);
			return stop(4, "bt uart access");
		}
		uart_ok = 1;
	}
	L("bt uart mr1/mr2", urd(U_MR1), urd(U_MR2));
	L("bt uart ipr/dmen", urd(U_IPR), urd(U_DMEN));

	/* 5. PIO setup (msm_hs_startup without the BAM): 8N1, 2400 baud, flow control off */
	uwr(U_IMR, 0);
	uwr(U_DMEN, 0);
	uwr(U_CR, CR_RESET_RX);
	uwr(U_CR, CR_RESET_TX);
	uwr(U_CR, CR_RESET_ERR);
	uwr(U_CR, CR_RESET_BREAK);
	uwr(U_CR, CR_RESET_STALE);
	uwr(U_CR, CR_RESET_CTS);
	uwr(U_MR2, 0x74);		/* 8 bits, 1 stop, no parity, ERROR_MODE */
	uwr(U_RFWR, 8);
	flow(0);
	uwr(U_IMR, ISR_RXSTALE | ISR_RXLEV);
	set_baud(0x33, 1);		/* 2400 */
	L("bt uart sr/isr", urd(U_SR), urd(U_ISR));

	/* 6. [E] power pulses */
	static const uint8_t off_pulse[1] = { 0xc0 }, on_pulse[1] = { 0xfc };
	rc = uart_tx(off_pulse, 1, 100);
	logp("bt pulse 0xC0 @2400 [E]", (uint32_t)rc, urd(U_SR), rc ? -1 : 0);
	udelay(10000);
	set_baud(0xcc, 31);		/* 115200 */
	rc = uart_tx(on_pulse, 1, 100);
	logp("bt pulse 0xFC @115200 [E]", (uint32_t)rc, urd(U_SR), rc ? -1 : 0);
	uint32_t got = uart_rx(rx, sizeof(rx), 100);	/* the 100 ms boot wait, logging anything sent */
	log_bytes("bt rx after pulse", rx, got);
	set_baud(0xcc, 31);		/* reopen: RX/TX reset (qca_wcn3990_init closes and opens the port) */
	flow(1);
	L("bt flow on mr1/isr", urd(U_MR1), urd(U_ISR));

	/* 7. HCI Reset -> 04 0E 04 01 03 0C 00 */
	static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
	got = hci("bt rx hci reset", reset, 4);
	int reset_ok = got >= 7 && rx[0] == 0x04 && rx[1] == 0x0e && rx[4] == 0x03 && rx[5] == 0x0c && rx[6] == 0x00;
	logp("bt HCI RESET event ok", got, reset_ok, reset_ok ? 0 : -1);
	con_str("BT", reset_ok ? "HCI reset answered" : "no HCI reset answer");

	/* 8. Read_Local_Version -> manufacturer 0x001D */
	static const uint8_t ver[4] = { 0x01, 0x01, 0x10, 0x00 };
	got = hci("bt rx local version", ver, 4);
	if (got >= 15 && rx[0] == 0x04 && rx[1] == 0x0e && rx[4] == 0x01 && rx[5] == 0x10) {
		uint32_t manuf = rx[11] | rx[12] << 8, subver = rx[13] | rx[14] << 8;
		logp("bt manufacturer/lmp sub", manuf, subver, manuf == 0x1d ? 0 : -2);
		L("bt hci ver/rev", rx[7], (uint32_t)(rx[8] | rx[9] << 8));
	}

	/* 9. Qualcomm EDL get version (0xFC00, 0x19) */
	static const uint8_t edl[5] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
	got = hci("bt rx edl version", edl, 5);
	if (!got) {	/* a slow vendor event: keep listening a little longer */
		got = uart_rx(rx, sizeof(rx), 300);
		log_bytes("bt rx edl late", rx, got);
	}
	return reset_ok ? 0 : -7;
}

int drg_bt_test(drg_log_fn log, drg_flush_fn fl)
{
	logp = log;
	flushp = fl;
	voted_l5 = voted_l9 = voted_l6 = voted_l19 = voted_bob = voted_clk = hw_touched = uart_ok = 0;
	L("bt start", 0x42310001, 0);
	con_str("BT", "WCN3990 HCI test");
	int rc = bt_run();
	flushp();
	/* power down: [E] power-off pulse at 2400 with flow control off, then UART, pins, clocks, supplies */
	if (uart_ok) {
		static const uint8_t off_pulse[1] = { 0xc0 };
		flow(0);
		set_baud(0x33, 1);
		int r = uart_tx(off_pulse, 1, 100);
		logp("bt off pulse 0xC0 [E]", (uint32_t)r, urd(U_SR), r ? -1 : 0);
		udelay(10000);
	}
	hw_restore();
	power_down_rpm();
	L("bt end rc", (uint32_t)rc, 0);
	flushp();
	return rc;
}
