/*
 * Nokia 6.1 Plus (DRG) touch bring-up probe: power is on (pm660_l11 by ABL,
 * pm660l_l6 through drg_rpm.c), then clock, pins, Goodix reset and one I2C
 * read of the chip's ID registers. Polled, printed stage by stage.
 *
 * From this phone's device tree:
 *   goodix_ts@5d "goodix,gt9xx" on i2c@c178000 (BLSP1 QUP4), 400 kHz
 *   (qcom,clk-freq-out), reset GPIO 66, interrupt GPIO 67
 *   i2c_4_active: gpio14/15 function blsp_i2c4, 2 mA, bias-disable
 *   ts_int_default: gpio67 gpio, 16 mA, bias-disable
 *   GCC @100000, TLMM @3000000 (gpio14/15/66/67 in the NORTH tile +0x900000)
 * From HMD's DRG kernel:
 *   gcc-sdm660.c: blsp1_qup4_i2c_apps_clk_src cmd_rcgr 0x1f020, 19.2 MHz = XO
 *     (source 0, pre-divider 2 * 1 - 1); branch blsp1_qup4_i2c_apps_clk 0x1f008
 *   pinctrl-sdm660.c: gpio14/15 functions gpio, blsp_spi4, blsp_i2c4 (mux 2);
 *     pin n: ctl tile + 0x1000 n (pull 1:0, mux 5:2, drive 8:6 = mA/2 - 1,
 *     output enable 9), io +4 (in bit 0, out bit 1)
 *   i2c-msm-v2.c/.h: QUP v2 registers, FIFO mode, tags v2 (START 0x81,
 *     DATA_WRITE 0x82, DATA_READ_N_STOP 0x87, 4-byte tag = START, addr, op,
 *     len), 400 kHz = fs_div 28 ht_div 14, read data behind 2 tag bytes,
 *     done = QUP_OPERATIONAL bit 11 (max input done)
 *   gt9xx.c gtp_reset_guitar/gtp_int_sync: RST low > 10 ms, INT low (address
 *     0x5d) 2 ms, RST high 6 ms, INT low 50 ms, INT input; version at 0x8140
 * Deviation: reset is kept driven high afterwards instead of released (its
 * pin is pulled down as ABL leaves it, which would hold the chip in reset).
 */
#include <stddef.h>
#include <stdint.h>
#include "console.h"
#include "drg_ts.h"

#define GCC			0x00100000UL
#define TLMM_NORTH		0x03900000UL
#define QUP			0x0c178000UL

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

static inline uint32_t r32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void w32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static void delay_us(unsigned int us)
{
	uint64_t frq, t0, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	do {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	} while (t - t0 < frq / 1000000 * us);
}

static void gpio_out(unsigned int n, unsigned int drive_ma, int val)
{
	uintptr_t ctl = TLMM_NORTH + 0x1000UL * n;
	w32(ctl + 4, val ? 2 : 0);
	w32(ctl, (1u << 9) | ((drive_ma / 2 - 1) << 6));        /* gpio, output, no pull */
}

static void gpio_in(unsigned int n, unsigned int drive_ma)
{
	w32(TLMM_NORTH + 0x1000UL * n, (drive_ma / 2 - 1) << 6);   /* gpio, input, no pull */
}

static int qup_state(uint32_t state)
{
	for (int i = 0; i < 100000; i++)
		if (r32(QUP + QUP_STATE) & 4)
			break;
	w32(QUP + QUP_STATE, state);
	for (int i = 0; i < 100000; i++) {
		uint32_t s = r32(QUP + QUP_STATE);
		if ((s & 4) && (s & 3) == state)
			return 0;
	}
	return -1;
}

int drg_ts_probe(void)
{
	/* 1. clock: QUP4 I2C core clock from XO, then its branch */
	w32(GCC + 0x1f024, (0u << 8) | 1u);             /* CFG_RCGR: XO, pre-div 1 */
	w32(GCC + 0x1f020, 1u);                          /* CMD_RCGR: update */
	for (int i = 0; i < 100000 && (r32(GCC + 0x1f020) & 1); i++)
		;
	w32(GCC + 0x1f008, r32(GCC + 0x1f008) | 1u);    /* CBCR: enable */
	for (int i = 0; i < 100000 && (r32(GCC + 0x1f008) & 0x80000000u); i++)
		;
	con_hex2("TS2 CLK", "rcg=", r32(GCC + 0x1f020), "cbcr=", r32(GCC + 0x1f008));
	if (r32(GCC + 0x1f008) & 0x80000000u)
		return -1;

	/* 2. I2C pins 14/15: blsp_i2c4 (mux 2), 2 mA, no pull */
	w32(TLMM_NORTH + 0x1000UL * 14, 2u << 2);
	w32(TLMM_NORTH + 0x1000UL * 15, 2u << 2);

	/* 3. Goodix reset, address 0x5d (INT low while reset rises) */
	gpio_out(66, 2, 0);
	delay_us(20000);
	gpio_out(67, 16, 0);
	delay_us(2000);
	gpio_out(66, 2, 1);
	delay_us(6000);
	gpio_out(67, 16, 0);
	delay_us(50000);
	gpio_in(67, 16);
	delay_us(10000);
	con_hex2("TS2 PINS", "rst io=", r32(TLMM_NORTH + 0x1000UL * 66 + 4),
	    "int io=", r32(TLMM_NORTH + 0x1000UL * 67 + 4));

	/* 4. QUP init (i2c_msm_qup_init) */
	w32(QUP + QUP_SW_RESET, 1);
	if (qup_state(0))
		return -2;
	w32(QUP + QUP_CONFIG, 0x207);                    /* N = 8 bits, mini core I2C */
	w32(QUP + QUP_ERROR_FLAGS_EN, 0x3c);
	w32(QUP + QUP_OPERATIONAL_MASK, 0x300);
	w32(QUP + QUP_MASTER_CONFIG, 1);                 /* version-two tags */

	/* 5. transfer: write 0x8140, repeated start, read 6 bytes */
	if (qup_state(0))
		return -3;
	w32(QUP + QUP_MX_INPUT_COUNT, 0);
	w32(QUP + QUP_MX_OUTPUT_COUNT, 0);
	w32(QUP + QUP_MX_READ_COUNT, 6 + 2);
	w32(QUP + QUP_MX_WRITE_COUNT, 2 + 8);
	w32(QUP + QUP_CONFIG, 0x207);
	w32(QUP + QUP_IO_MODES, (r32(QUP + QUP_IO_MODES) & ~0x1fc00u) | (1u << 14) | (1u << 15));
	w32(QUP + QUP_OPERATIONAL_MASK, 0);
	if (qup_state(1))
		return -4;
	w32(QUP + QUP_MASTER_CLK_CTL, (14u << 16) | 28u);
	if (qup_state(3))
		return -5;
	/* bytes: 81 ba 82 02 | 81 40 | 81 bb 87 06 (packed little endian) */
	w32(QUP + QUP_OUT_FIFO_BASE, 0x0282ba81u);
	w32(QUP + QUP_OUT_FIFO_BASE, 0xbb814081u);
	w32(QUP + QUP_OUT_FIFO_BASE, 0x00000687u);
	if (qup_state(1))
		return -6;
	uint32_t op = 0;
	for (int i = 0; i < 2000; i++) {
		op = r32(QUP + QUP_OPERATIONAL);
		if (op & (1u << 11))
			break;
		delay_us(100);
	}
	uint32_t st = r32(QUP + QUP_I2C_STATUS), err = r32(QUP + QUP_ERROR_FLAGS);
	con_hex2("TS2 I2C", "op=", op, "status=", st);
	con_hex2("TS2 I2C", "err=", err, "", 0);
	if (!(op & (1u << 11)))
		return -7;
	uint32_t w0 = r32(QUP + QUP_IN_FIFO_BASE), w1 = r32(QUP + QUP_IN_FIFO_BASE);
	/* first two bytes are the input tag; then ID 0x8140.. */
	con_hex2("TS2 ID", "w0=", w0, "w1=", w1);
	(void)qup_state(0);
	return 0;
}
