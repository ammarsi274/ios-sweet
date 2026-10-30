/*
 * DRG (Nokia 6.1 Plus): read-only probe of the eMMC host controller as ABL
 * leaves it, to measure what an XNU eMMC driver can rely on.
 *
 * Addresses from this phone's FDT: sdhci@c0c4000, "qcom,sdhci-msm-v5",
 * hc_mem 0xc0c4000 (standard SD Host Controller registers), cmdq_mem
 * 0xc0c5000 (command queue engine). Register layout from the SD Host
 * Controller specification; commands from the JEDEC eMMC standard.
 *
 * No reset, clock, power or host-control writes. It only clears/enables
 * interrupt status bits and issues CMD17 (READ_SINGLE_BLOCK), polled; if
 * the command queue engine is enabled it sends nothing and just reports
 * that. R6 adds ONE card write: write_test() writes a single block in the
 * unused part of userdata (past the 16 GiB iOS image), verifies it and
 * writes the original contents back.
 *
 * Data path, from measurements (drg/BOOTLOG.md #10, #11): reading through
 * the buffer data port (PIO) returns zeros and never drains, and ABL
 * leaves Host Control 1 = 0x38, i.e. DMA select 11 = 64-bit ADMA2 and an
 * 8-bit bus. So each read uses one ADMA2 descriptor into a RAM buffer.
 * The shim runs with the MMU and caches off: the buffer is coherent.
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"

#define HC_BASE		0x0c0c4000UL
#define CQ_BASE		0x0c0c5000UL

#define SDHCI_BLOCK		0x04	/* block size [15:0], block count [31:16] */
#define SDHCI_ARGUMENT		0x08
#define SDHCI_XFER_CMD		0x0c	/* transfer mode [15:0], command [31:16] */
#define SDHCI_RESPONSE0		0x10
#define SDHCI_BUFFER		0x20
#define SDHCI_PRESENT		0x24
#define SDHCI_HOST_CTRL		0x28	/* host ctrl 1, power, block gap, wakeup */
#define SDHCI_CLOCK		0x2c	/* clock ctrl, timeout ctrl, sw reset */
#define SDHCI_INT_STATUS	0x30	/* normal [15:0], error [31:16] */
#define SDHCI_INT_ENABLE	0x34
#define SDHCI_HOST_CTRL2	0x3c	/* auto-cmd error [15:0], host ctrl 2 [31:16] */
#define SDHCI_ADMA_ERROR	0x54
#define SDHCI_ADMA_ADDR_LO	0x58
#define SDHCI_ADMA_ADDR_HI	0x5c
#define SDHCI_CAPS		0x40
#define SDHCI_CAPS1		0x44
#define SDHCI_VERSION		0xfc	/* slot int status [15:0], version [31:16] */

#define PRESENT_CMD_INHIBIT	(1U << 0)
#define PRESENT_DAT_INHIBIT	(1U << 1)
#define PRESENT_BUF_READ	(1U << 11)	/* buffer read enable */

#define INT_CMD_DONE		(1U << 0)
#define INT_XFER_DONE		(1U << 1)
#define INT_BUF_READ		(1U << 5)
#define INT_ERROR		(1U << 15)
#define INT_ERRORS		0x03ff0000U	/* error status bits we enable */

#define CQ_CFG			0x08
#define CQ_CFG_ENABLE		(1U << 0)

/* CMD17: index 17, data present, index + CRC check, 48-bit response (R1) */
#define CMD17			((17U << 8) | 0x20 | 0x10 | 0x08 | 0x02)
/* CMD24 WRITE_BLOCK: data, index + CRC check, R1 (used only by the R6 write test) */
#define CMD24			((24U << 8) | 0x20 | 0x10 | 0x08 | 0x02)
/* transfer mode: read, block count enable, single block, DMA (ADMA2 per host ctrl) */
#define XFER_READ_ONE_DMA	((1U << 4) | (1U << 1) | (1U << 0))
/* transfer mode: write (direction bit clear), block count enable, single block, DMA */
#define XFER_WRITE_ONE_DMA	((1U << 1) | (1U << 0))

/*
 * R6 write test target, from measurements: the iOS root image flashed to
 * userdata is exactly 16 GiB (0x2000000 sectors); the test block lies
 * 0x3000000 sectors (24 GiB) into userdata, 8 GiB past the image's end.
 */
#define IOS_IMAGE_SECTORS	0x2000000ULL
#define WTEST_OFFSET		0x3000000ULL
#define WTEST_MARGIN		0x100000ULL	/* 512 MiB that must stay between image and test block */

/* ADMA2 descriptor attributes: valid, end, action = transfer */
#define ADMA2_VALID		(1U << 0)
#define ADMA2_END		(1U << 1)
#define ADMA2_TRAN		(2U << 4)
#define FILL			0x5a5a5a5aU	/* shows which words the DMA did not write */

#define GPT_ENTRIES_MAX		128
#define ANDROID_USERDATA_KB	50585583ULL	/* /proc/partitions mmcblk0p85, measured */

static uint32_t sector[128] __attribute__((aligned(64)));
/* one 64-bit ADMA2 descriptor; 16 bytes so a 96- or 128-bit layout reads the same first 12 */
static volatile uint32_t adma_desc[4] __attribute__((aligned(16)));

/* measured on the last read_block(), for drg_emmc_probe() to report */
static uint32_t last_block_reg, last_adma_err, last_int_after;

static inline uint32_t hc_rd(uint32_t off) { return *(volatile uint32_t *)(HC_BASE + off); }
static inline void hc_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(HC_BASE + off) = v; }

static uint64_t now(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

static uint64_t ticks_ms(unsigned ms)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f / 1000 * ms;
}

/* Wait for any bit of want in the interrupt status. 0 ok, -1 error, -2 timeout. */
static int wait_int(uint32_t want, uint32_t *st)
{
	uint64_t end = now() + ticks_ms(200);
	do {
		*st = hc_rd(SDHCI_INT_STATUS);
		if (*st & INT_ERROR)
			return -1;
		if (*st & want)
			return 0;
	} while (now() < end);
	return -2;
}

/*
 * One 512-byte block through ADMA2 between sector[] and the card. Reads fill
 * sector[] with FILL first (to see what the DMA wrote); writes send sector[]
 * as it is. Returns 0, or a negative step code; *resp = R1 card status.
 */
static int xfer_block(uint32_t lba, int write, uint32_t *resp, uint32_t *st)
{
	uint64_t end = now() + ticks_ms(200);
	while (hc_rd(SDHCI_PRESENT) & (PRESENT_CMD_INHIBIT | PRESENT_DAT_INHIBIT))
		if (now() > end)
			return -1;

	if (!write)
		for (int i = 0; i < 128; i++)
			sector[i] = FILL;
	uint64_t buf = (uintptr_t)sector;	/* MMU off: VA = PA */
	adma_desc[0] = (512U << 16) | ADMA2_TRAN | ADMA2_END | ADMA2_VALID;
	adma_desc[1] = (uint32_t)buf;
	adma_desc[2] = (uint32_t)(buf >> 32);
	adma_desc[3] = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	hc_wr(SDHCI_INT_STATUS, 0xffffffffU);
	hc_wr(SDHCI_ADMA_ADDR_LO, (uint32_t)(uintptr_t)adma_desc);
	hc_wr(SDHCI_ADMA_ADDR_HI, (uint32_t)((uintptr_t)adma_desc >> 32));
	hc_wr(SDHCI_BLOCK, (1U << 16) | 512);
	last_block_reg = hc_rd(SDHCI_BLOCK);
	hc_wr(SDHCI_ARGUMENT, lba);		/* high-capacity card: sector address */
	hc_wr(SDHCI_XFER_CMD, write ? (CMD24 << 16) | XFER_WRITE_ONE_DMA : (CMD17 << 16) | XFER_READ_ONE_DMA);

	int rc = 0;
	if (wait_int(INT_CMD_DONE, st)) {
		rc = -2;
	} else {
		*resp = hc_rd(SDHCI_RESPONSE0);
		hc_wr(SDHCI_INT_STATUS, INT_CMD_DONE);
		if (wait_int(INT_XFER_DONE, st))
			rc = -4;
		else
			hc_wr(SDHCI_INT_STATUS, INT_XFER_DONE);
	}
	last_adma_err = hc_rd(SDHCI_ADMA_ERROR);
	last_int_after = hc_rd(SDHCI_INT_STATUS);
	__asm__ volatile("dsb sy" ::: "memory");
	return rc;
}

static int read_block(uint32_t lba, uint32_t *resp, uint32_t *st)
{
	return xfer_block(lba, 0, resp, st);
}

static uint8_t byte_at(unsigned off)
{
	return (uint8_t)(sector[off / 4] >> (8 * (off % 4)));
}

static uint32_t u32_at(unsigned off)
{
	return byte_at(off) | byte_at(off + 1) << 8 | byte_at(off + 2) << 16 | (uint32_t)byte_at(off + 3) << 24;
}

static uint64_t u64_at(unsigned off)
{
	return u32_at(off) | (uint64_t)u32_at(off + 4) << 32;
}

static int read_or_report(uint32_t lba, const char *what, int verbose)
{
	uint32_t resp = 0, st = 0;
	int rc = read_block(lba, &resp, &st);
	if (verbose || rc) {
		con_str("READ", what);
		con_hex2(" block", "reg=", last_block_reg, "adma=", last_adma_err);
		con_hex2(" data", "w0=", sector[0], "w127=", sector[127]);
	}
	if (rc) {
		con_hex2("FAILED", "step=", (uint32_t)-rc, "int=", st);
		con_hex2(" state", "pres=", hc_rd(SDHCI_PRESENT), "resp=", resp);
		con_hex2(" after", "int=", last_int_after, 0, 0);
	}
	return rc;
}

static uint32_t saved[128];

static unsigned count_diff(const uint32_t *want)
{
	unsigned bad = 0;
	for (int i = 0; i < 128; i++)
		bad += sector[i] != want[i];
	return bad;
}

/*
 * R6: write ONE block in the unused part of userdata, read it back, then
 * write the original contents back and read again. Refuses unless the block
 * is inside userdata and past the iOS image plus a margin.
 */
static void write_test(uint64_t first, uint64_t last)
{
	static uint32_t pattern[128];
	uint64_t lba = first + WTEST_OFFSET;
	uint32_t resp = 0, st = 0;
	int rc;

	con_skip();
	con_hex("WTEST lba", lba);
	if (lba < first + IOS_IMAGE_SECTORS + WTEST_MARGIN || lba > last || lba > 0xffffffffULL) {
		con_str("WTEST", "refused: block not in the safe range");
		return;
	}
	if (read_or_report((uint32_t)lba, "WTEST original", 0))
		return;
	for (int i = 0; i < 128; i++) {
		saved[i] = sector[i];
		pattern[i] = 0x4e4f4b41u ^ (uint32_t)i * 0x01010101u;	/* "NOKA" ^ index */
		sector[i] = pattern[i];
	}
	rc = xfer_block((uint32_t)lba, 1, &resp, &st);
	con_hex2("WRITE", "rc=", (uint32_t)-rc, "r1=", resp);
	if (rc) {
		con_hex2(" fail", "int=", st, "adma=", last_adma_err);
		return;
	}
	if (read_or_report((uint32_t)lba, "WTEST readback", 0))
		return;
	unsigned bad = count_diff(pattern);
	con_hex2("VERIFY", "bad=", bad, "w0=", sector[0]);

	for (int i = 0; i < 128; i++)
		sector[i] = saved[i];
	rc = xfer_block((uint32_t)lba, 1, &resp, &st);
	con_hex2("RESTORE", "rc=", (uint32_t)-rc, "r1=", resp);
	if (rc || read_or_report((uint32_t)lba, "WTEST restore", 0))
		return;
	unsigned bad2 = count_diff(saved);
	con_hex2(" check", "bad=", bad2, 0, 0);
	con_str("WTEST", bad == 0 && bad2 == 0 ? "PASS: write, readback, restore" : "FAIL");
}

/* GPT name: UTF-16LE at entry offset 56 */
static int name_is(unsigned entry_off, const char *want)
{
	for (unsigned i = 0; i < 36; i++) {
		uint16_t c = byte_at(entry_off + 56 + 2 * i) | byte_at(entry_off + 57 + 2 * i) << 8;
		if (c != (uint8_t)want[i])
			return 0;
		if (c == 0)
			return 1;
	}
	return 0;
}

void drg_emmc_probe(void)
{
	con_str("EMMC", "probe + R6 1-block write test");
	con_hex2("VERSION", "ver=", hc_rd(SDHCI_VERSION) >> 16, "cq=", *(volatile uint32_t *)CQ_BASE);
	con_hex2("CAPS", "lo=", hc_rd(SDHCI_CAPS), "hi=", hc_rd(SDHCI_CAPS1));
	con_hex2("PRESENT", "=", hc_rd(SDHCI_PRESENT), "int=", hc_rd(SDHCI_INT_STATUS));
	con_hex2("HOSTCTL", "=", hc_rd(SDHCI_HOST_CTRL), "ctl2=", hc_rd(SDHCI_HOST_CTRL2));
	con_hex2("CLOCK", "=", hc_rd(SDHCI_CLOCK), "inten=", hc_rd(SDHCI_INT_ENABLE));
	uint32_t cqcfg = *(volatile uint32_t *)(CQ_BASE + CQ_CFG);
	con_hex2("CQE", "cfg=", cqcfg, 0, 0);
	if (cqcfg & CQ_CFG_ENABLE) {
		con_str("EMMC", "CQE enabled: no commands sent");
		return;
	}

	/* status bits must be enabled to show up in INT_STATUS */
	hc_wr(SDHCI_INT_ENABLE, hc_rd(SDHCI_INT_ENABLE) | INT_ERRORS | INT_CMD_DONE | INT_XFER_DONE | INT_BUF_READ);

	uint32_t resp = 0, st = 0;
	if (read_or_report(0, "LBA 0", 1))
		return;
	read_block(0, &resp, &st);	/* same block again to show the R1 status */
	con_hex2("LBA0", "sig=", byte_at(510) | byte_at(511) << 8, "type=", byte_at(0x1c2));
	con_hex2(" R1", "=", resp, "state=", (resp >> 9) & 0xf);

	if (read_or_report(1, "LBA 1", 1))
		return;
	int gpt_ok = u32_at(0) == 0x20494645 && u32_at(4) == 0x54524150;	/* "EFI PART" */
	uint64_t ent_lba = u64_at(72);
	uint32_t n = u32_at(80), esz = u32_at(84);
	con_hex2("GPT", "ok=", gpt_ok, "entries=", n);
	con_hex2(" ent", "lba=", (uint32_t)ent_lba, "size=", esz);
	con_hex("DISK end", u64_at(48));
	if (!gpt_ok || esz != 128 || n == 0)
		return;
	if (n > GPT_ENTRIES_MAX)
		n = GPT_ENTRIES_MAX;

	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0 && read_or_report((uint32_t)ent_lba + i / 4, "GPT entries", 0))
			return;
		unsigned off = (i % 4) * 128;
		if (name_is(off, "userdata")) {
			uint64_t first = u64_at(off + 32), last = u64_at(off + 40);
			con_hex2("USERDATA", "part=", i + 1, "of=", n);
			con_hex("  first", first);
			con_hex("  last", last);
			con_hex("  sectors", last - first + 1);
			con_hex("  android", ANDROID_USERDATA_KB * 2);
			write_test(first, last);
			return;
		}
	}
	con_str("USERDATA", "not found in GPT");
}

/*
 * GPU bring-up log (G2): up to LOG_BLOCKS blocks of text written to the
 * unused part of userdata, right after the R6 test block, so TWRP can read
 * the result back (dd from userdata at LOG_OFFSET). Same safety rule as the
 * R6 test: only past the 16 GiB iOS image plus the margin (the iOS HFS+
 * volume measured 2026-10-30: block size 4096, 4194304 blocks = 16 GiB).
 */
#define LOG_OFFSET		(WTEST_OFFSET + 0x100ULL)
#define LOG_BLOCKS		64

static uint64_t log_lba;	/* 0: not found / not safe */

int drg_emmc_log_open(void)
{
	uint32_t resp = 0, st = 0;
	log_lba = 0;
	if (*(volatile uint32_t *)(CQ_BASE + CQ_CFG) & CQ_CFG_ENABLE)
		return -1;
	hc_wr(SDHCI_INT_ENABLE, hc_rd(SDHCI_INT_ENABLE) | INT_ERRORS | INT_CMD_DONE | INT_XFER_DONE | INT_BUF_READ);
	if (read_block(1, &resp, &st) || u32_at(0) != 0x20494645 || u32_at(4) != 0x54524150)
		return -2;
	uint64_t ent_lba = u64_at(72);
	uint32_t n = u32_at(80), esz = u32_at(84);
	if (esz != 128 || !n)
		return -3;
	if (n > GPT_ENTRIES_MAX)
		n = GPT_ENTRIES_MAX;
	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0 && read_block((uint32_t)ent_lba + i / 4, &resp, &st))
			return -4;
		unsigned off = (i % 4) * 128;
		if (!name_is(off, "userdata"))
			continue;
		uint64_t first = u64_at(off + 32), last = u64_at(off + 40);
		uint64_t lba = first + LOG_OFFSET;
		if (lba < first + IOS_IMAGE_SECTORS + WTEST_MARGIN || lba + LOG_BLOCKS - 1 > last ||
		    lba + LOG_BLOCKS > 0xffffffffULL)
			return -5;
		log_lba = lba;
		return 0;
	}
	return -6;
}

uint64_t drg_emmc_log_lba(void)
{
	return log_lba;
}

/* write len bytes (zero padded to whole blocks, at most LOG_BLOCKS) */
int drg_emmc_log_write(const char *buf, unsigned len)
{
	uint32_t resp = 0, st = 0;
	if (!log_lba)
		return -1;
	unsigned nb = (len + 511) / 512;
	if (nb > LOG_BLOCKS)
		nb = LOG_BLOCKS;
	for (unsigned b = 0; b < nb; b++) {
		unsigned char *s = (unsigned char *)sector;
		for (unsigned i = 0; i < 512; i++) {
			unsigned k = b * 512 + i;
			s[i] = k < len ? (unsigned char)buf[k] : 0;
		}
		if (xfer_block((uint32_t)(log_lba + b), 1, &resp, &st))
			return -2 - (int)b;
	}
	return 0;
}
