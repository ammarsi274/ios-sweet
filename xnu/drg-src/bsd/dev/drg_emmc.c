/*
 * DRG (Nokia 6.1 Plus): root disk on the internal eMMC.
 *
 * Every hardware fact here was measured on the phone with the shim probe
 * (shim/emmc.c, drg/BOOTLOG.md boots #10-#12):
 *   - host controller sdhci@c0c4000 ("qcom,sdhci-msm-v5", from its FDT),
 *     SD Host Controller spec 3.00; command queue engine (0xc0c5000) off
 *   - ABL leaves Host Control 1 = 0x38: 64-bit ADMA2, 8-bit bus; the card
 *     is selected and in transfer state, sector addressed
 *   - reading through the buffer data port (PIO) does not work in that
 *     state; CMD17 with one 64-bit ADMA2 descriptor does
 *   - GPT at LBA 1; "userdata" is partition 85, LBA 0x1400000-0x747BFDE
 *   - CMD24 with one ADMA2 descriptor writes a block: R1 0x900, read back
 *     equal, original restored (R6 write test, 8 GiB past the iOS image)
 *
 * So this driver keeps ABL's controller state as it is (no reset, clock,
 * power or host-control writes in normal operation) and reads one 512-byte
 * block per CMD17 (writes one per CMD24) through ADMA2. The DMA buffer is the low-bank range the
 * ADT gives in /drg-emmc "dma" (outside XNU's RAM; the shim proved ADMA2
 * into the low bank works), mapped non-cacheable, so no cache maintenance.
 *
 * It finds the GPT partition named by boot-arg emmcroot= (default
 * "userdata") and exposes it as the BSD disk emmcroot0 / remmcroot0.
 * Root on it with rd=emmcroot0. Writes are off (EROFS) unless boot-arg
 * emmcrw=1, and then only inside that partition. At start it reads
 * EXT_CSD and prints the partition access and cache state: writes are
 * refused unless the user area is selected, and if the card's volatile
 * cache is on, cache flushes (CMD6 FLUSH_CACHE) back DKIOCSYNCHRONIZE
 * and B_FUA.
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_DRG)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

/* SD Host Controller registers (offsets from hc_mem) */
#define HC_BLOCK		0x04	/* block size [15:0], block count [31:16] */
#define HC_ARGUMENT		0x08
#define HC_XFER_CMD		0x0c	/* transfer mode [15:0], command [31:16] */
#define HC_RESPONSE0		0x10
#define HC_PRESENT		0x24
#define HC_HOST_CTRL		0x28	/* host control 1 [7:0] */
#define HC_CLOCK		0x2c	/* clock [15:0], timeout [23:16], reset [31:24] */
#define HC_INT_STATUS		0x30	/* normal [15:0], error [31:16] */
#define HC_INT_ENABLE		0x34
#define HC_ADMA_ERROR		0x54
#define HC_ADMA_ADDR_LO		0x58
#define HC_ADMA_ADDR_HI		0x5c

#define PRESENT_INHIBIT		0x3u		/* command and data inhibit */
#define HOST_CTRL_DMA_MASK	(3u << 3)
#define HOST_CTRL_ADMA2_64	(3u << 3)
#define INT_CMD_DONE		(1u << 0)
#define INT_XFER_DONE		(1u << 1)
#define INT_ERROR		(1u << 15)
#define INT_ENABLE_WANT		(0x03ff0000u | INT_CMD_DONE | INT_XFER_DONE)
#define RESET_CMD_DAT		(0x6u << 24)	/* in HC_CLOCK: reset CMD and DAT lines */

#define CQ_CFG			0x08
#define CQ_CFG_ENABLE		(1u << 0)

/* CMD17 READ_SINGLE_BLOCK: data, index + CRC check, R1 */
#define CMD17			((17u << 8) | 0x20 | 0x10 | 0x08 | 0x02)
/* CMD24 WRITE_BLOCK: data, index + CRC check, R1 */
#define CMD24			((24u << 8) | 0x20 | 0x10 | 0x08 | 0x02)
/* CMD8 SEND_EXT_CSD: data, index + CRC check, R1 */
#define CMD8			((8u << 8) | 0x20 | 0x10 | 0x08 | 0x02)
/* CMD6 SWITCH: no data, index + CRC check, R1b (busy) */
#define CMD6			((6u << 8) | 0x10 | 0x08 | 0x03)
/* transfer mode: read, block count enable, DMA */
#define XFER_READ_ONE_DMA	((1u << 4) | (1u << 1) | (1u << 0))
/* transfer mode: write, block count enable, DMA */
#define XFER_WRITE_ONE_DMA	((1u << 1) | (1u << 0))
#define R1_ERRORS		0xfff98008u	/* R1 card status error bits */
#define R1_SWITCH_ERROR		(1u << 7)

/* EXT_CSD bytes */
#define EXT_CSD_FLUSH_CACHE	32
#define EXT_CSD_CACHE_CTRL	33
#define EXT_CSD_PART_CONFIG	179
#define EXT_CSD_REV		192
#define EXT_CSD_CACHE_SIZE	249		/* 4 bytes, little endian, KiB */
/* CMD6 argument: write byte FLUSH_CACHE = 1 */
#define SWITCH_FLUSH		((3u << 24) | (EXT_CSD_FLUSH_CACHE << 16) | (1u << 8))

#define ADMA2_ATTR_TRAN_END	((2u << 4) | (1u << 1) | (1u << 0))

#define SECTOR			512u
#define DESC_BYTES		64u		/* descriptor at the start of the DMA range */
#define CMD_TIMEOUT_NS		(200 * NSEC_PER_MSEC)
#define WRITE_TIMEOUT_NS	(1000 * NSEC_PER_MSEC)
#define FLUSH_TIMEOUT_NS	(2000 * NSEC_PER_MSEC)

static struct {
	vm_offset_t hc, cq;
	vm_offset_t dma;		/* non-cacheable mapping of the DMA range */
	uint64_t dma_pa, dma_size;
	uint64_t first_lba, nsect;
	int bmajor, cmajor;
	dev_t bdev;
	bool ready;
	bool writable;			/* emmcrw=1 and the user area is selected */
	bool cache_on;			/* EXT_CSD: volatile cache present and enabled */
	uint32_t errors;
} se;

static LCK_GRP_DECLARE(drg_emmc_grp, "drg_emmc");
static LCK_MTX_DECLARE(drg_emmc_lock, &drg_emmc_grp);

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(se.hc + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(se.hc + off) = v;
}

static bool
wait_clear(uint32_t off, uint32_t mask, uint64_t deadline)
{
	while (hc_rd(off) & mask) {
		if (mach_absolute_time() > deadline) {
			return false;
		}
	}
	return true;
}

/* 0, or -1 error interrupt, -2 timeout; *st = last interrupt status */
static int
wait_int(uint32_t want, uint64_t deadline, uint32_t *st)
{
	for (;;) {
		*st = hc_rd(HC_INT_STATUS);
		if (*st & INT_ERROR) {
			return -1;
		}
		if (*st & want) {
			return 0;
		}
		if (mach_absolute_time() > deadline) {
			return -2;
		}
	}
}

/*
 * After a failed command: reset the controller's CMD and DAT line state
 * machines (not the card, not the clock or bus settings) so the next
 * command can be issued. The only non-read register write besides the
 * interrupt status and the command itself.
 */
static void
recover_lines(void)
{
	uint64_t deadline;
	hc_wr(HC_CLOCK, hc_rd(HC_CLOCK) | RESET_CMD_DAT);
	nanoseconds_to_deadline(CMD_TIMEOUT_NS, &deadline);
	(void)wait_clear(HC_CLOCK, RESET_CMD_DAT, deadline);
	hc_wr(HC_INT_STATUS, 0xffffffffu);
}

/*
 * Issue one command. With data (mode != 0): one 512-byte block through one
 * ADMA2 descriptor at DMA range offset buf_off. For R1b commands without
 * data the controller signals transfer done when the card leaves busy.
 */
static int
issue(const char *what, uint32_t cmd, uint32_t arg, uint32_t mode, uint32_t buf_off,
    bool busy, uint64_t timeout_ns, uint32_t r1_bad)
{
	volatile uint32_t *desc = (volatile uint32_t *)se.dma;
	uint64_t data_pa = se.dma_pa + buf_off;
	uint64_t deadline;
	uint32_t st = 0;
	int step;

	nanoseconds_to_deadline(timeout_ns, &deadline);
	if (!wait_clear(HC_PRESENT, PRESENT_INHIBIT, deadline)) {
		step = 1;
		goto fail;
	}

	if (mode) {
		desc[0] = (SECTOR << 16) | ADMA2_ATTR_TRAN_END;
		desc[1] = (uint32_t)data_pa;
		desc[2] = (uint32_t)(data_pa >> 32);
		desc[3] = 0;
	}
	/* the descriptor and, for writes, the data must reach memory first */
	__asm__ volatile("dsb sy" ::: "memory");

	hc_wr(HC_INT_STATUS, 0xffffffffu);
	if (mode) {
		hc_wr(HC_ADMA_ADDR_LO, (uint32_t)se.dma_pa);
		hc_wr(HC_ADMA_ADDR_HI, (uint32_t)(se.dma_pa >> 32));
		hc_wr(HC_BLOCK, (1u << 16) | SECTOR);
	}
	hc_wr(HC_ARGUMENT, arg);
	hc_wr(HC_XFER_CMD, (cmd << 16) | mode);

	if (wait_int(INT_CMD_DONE, deadline, &st)) {
		step = 2;
		goto fail;
	}
	uint32_t r1 = hc_rd(HC_RESPONSE0);
	hc_wr(HC_INT_STATUS, INT_CMD_DONE);
	if (mode || busy) {
		if (wait_int(INT_XFER_DONE, deadline, &st)) {
			step = 3;
			goto fail;
		}
		hc_wr(HC_INT_STATUS, INT_XFER_DONE);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	if (r1 & r1_bad) {
		st = r1;
		step = 4;
		goto fail;
	}
	return 0;

fail:
	if (se.errors++ < 8) {
		printf("DRG EMMC: %s 0x%x failed at step %d: status 0x%08x present 0x%08x adma 0x%08x\n",
		    what, arg, step, st, hc_rd(HC_PRESENT), hc_rd(HC_ADMA_ERROR));
	}
	recover_lines();
	return EIO;
}

/* Read one sector (absolute LBA on the eMMC) to DMA range offset buf_off. */
static int
read_sector(uint64_t lba, uint32_t buf_off)
{
	if (lba > UINT32_MAX) {
		return EINVAL;
	}
	return issue("read LBA", CMD17, (uint32_t)lba, XFER_READ_ONE_DMA, buf_off,
	           false, CMD_TIMEOUT_NS, R1_ERRORS);
}

/*
 * Write one sector from DMA range offset buf_off. Checked here again, right
 * before the command: only with writes on, and only inside the partition.
 */
static int
write_sector(uint64_t lba, uint32_t buf_off)
{
	if (!se.writable || lba < se.first_lba || lba - se.first_lba >= se.nsect ||
	    lba > UINT32_MAX) {
		return EROFS;
	}
	return issue("write LBA", CMD24, (uint32_t)lba, XFER_WRITE_ONE_DMA, buf_off,
	           false, WRITE_TIMEOUT_NS, R1_ERRORS);
}

/* CMD6 FLUSH_CACHE, only when EXT_CSD says the volatile cache is on */
static int
flush_cache(void)
{
	if (!se.cache_on) {
		return 0;
	}
	return issue("flush cache", CMD6, SWITCH_FLUSH, 0, 0,
	           true, FLUSH_TIMEOUT_NS, R1_ERRORS | R1_SWITCH_ERROR);
}

static int
read_sectors(uint64_t lba, uint32_t count, uint32_t buf_off)
{
	for (uint32_t i = 0; i < count; i++) {
		int error = read_sector(lba + i, buf_off + i * SECTOR);
		if (error) {
			return error;
		}
	}
	return 0;
}

static int
write_sectors(uint64_t lba, uint32_t count, uint32_t buf_off)
{
	for (uint32_t i = 0; i < count; i++) {
		int error = write_sector(lba + i, buf_off + i * SECTOR);
		if (error) {
			return error;
		}
	}
	return 0;
}

/*
 * EXT_CSD: print the card's partition access and cache state. Writes need
 * the user area (PARTITION_ACCESS 0), which is where the GPT was read from.
 */
static int
read_ext_csd(bool *user_area)
{
	const uint8_t *x = (const uint8_t *)(se.dma + DESC_BYTES);
	int error = issue("EXT_CSD", CMD8, 0, XFER_READ_ONE_DMA, DESC_BYTES,
	    false, CMD_TIMEOUT_NS, R1_ERRORS);
	if (error) {
		return error;
	}
	uint32_t cache_kib = (uint32_t)x[EXT_CSD_CACHE_SIZE] |
	    (uint32_t)x[EXT_CSD_CACHE_SIZE + 1] << 8 |
	    (uint32_t)x[EXT_CSD_CACHE_SIZE + 2] << 16 |
	    (uint32_t)x[EXT_CSD_CACHE_SIZE + 3] << 24;
	se.cache_on = cache_kib != 0 && (x[EXT_CSD_CACHE_CTRL] & 1);
	*user_area = (x[EXT_CSD_PART_CONFIG] & 7) == 0;
	printf("DRG EMMC: EXT_CSD rev %u, partition config 0x%02x, cache %u KiB ctrl 0x%02x: cache %s\n",
	    x[EXT_CSD_REV], x[EXT_CSD_PART_CONFIG], cache_kib, x[EXT_CSD_CACHE_CTRL],
	    se.cache_on ? "on, flushed on sync" : "off");
	return 0;
}

static int
find_partition(const char *want)
{
	const uint8_t *b = (const uint8_t *)(se.dma + DESC_BYTES);
	int error = read_sectors(1, 1, DESC_BYTES);
	if (error) {
		return error;
	}
	if (memcmp(b, "EFI PART", 8) != 0) {
		printf("DRG EMMC: no GPT header at LBA 1\n");
		return ENXIO;
	}
	uint64_t entry_lba, disk_last;
	uint32_t nent, esz;
	memcpy(&disk_last, b + 48, 8);
	memcpy(&entry_lba, b + 72, 8);
	memcpy(&nent, b + 80, 4);
	memcpy(&esz, b + 84, 4);
	uint64_t table = (uint64_t)nent * esz;
	if (esz != 128 || nent == 0 || table > se.dma_size - DESC_BYTES) {
		printf("DRG EMMC: unsupported GPT: %u entries of %u bytes\n", nent, esz);
		return ENXIO;
	}
	error = read_sectors(entry_lba, (uint32_t)((table + SECTOR - 1) / SECTOR), DESC_BYTES);
	if (error) {
		return error;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = b + i * esz;
		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
			if (ch == 0 || ch > 0x7f) {
				break;
			}
			name[c] = (char)ch;
		}
		name[c] = 0;
		if (strcmp(name, want) != 0) {
			continue;
		}
		uint64_t first, last;
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (first < 2 || last < first || last > disk_last) {
			printf("DRG EMMC: partition \"%s\" has bad bounds 0x%llx-0x%llx\n", want, first, last);
			return ENXIO;
		}
		se.first_lba = first;
		se.nsect = last - first + 1;
		printf("DRG EMMC: \"%s\" is GPT partition %u of %u: LBA 0x%llx-0x%llx, %llu sectors (%llu MiB)\n",
		    want, i + 1, nent, first, last, se.nsect, se.nsect * SECTOR >> 20);
		return 0;
	}
	printf("DRG EMMC: no GPT partition named \"%s\"\n", want);
	return ENOENT;
}

static void
drg_emmc_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * SECTOR;
	uint32_t count = buf_count(bp);
	uint64_t part_bytes = se.nsect * SECTOR;
	uint32_t chunk = (uint32_t)(se.dma_size - DESC_BYTES) & ~(SECTOR - 1);

	bool is_read = (buf_flags(bp) & B_READ) != 0;

	buf_setresid(bp, count);
	if (!is_read && !se.writable) {
		error = EROFS;
		goto done;
	}
	if (!se.ready || off > part_bytes || count > part_bytes - off || (count % SECTOR)) {
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}
	lck_mtx_lock(&drg_emmc_lock);
	for (uint32_t done = 0; done < count;) {
		uint32_t want = MIN(count - done, chunk);
		uint64_t lba = se.first_lba + (off + done) / SECTOR;
		if (is_read) {
			error = read_sectors(lba, want / SECTOR, DESC_BYTES);
			if (error) {
				break;
			}
			memcpy(vaddr + done, (const void *)(se.dma + DESC_BYTES), want);
		} else {
			memcpy((void *)(se.dma + DESC_BYTES), vaddr + done, want);
			error = write_sectors(lba, want / SECTOR, DESC_BYTES);
			if (error) {
				break;
			}
		}
		done += want;
		buf_setresid(bp, count - done);
	}
	if (!error && !is_read && (buf_flags(bp) & B_FUA)) {
		error = flush_cache();
	}
	lck_mtx_unlock(&drg_emmc_lock);
	buf_unmap(bp);
done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
drg_emmc_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!se.ready) {
		return ENXIO;
	}
	return ((flags & FWRITE) && !se.writable) ? EROFS : 0;
}

static int
drg_emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
drg_emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	uint32_t *f = (uint32_t *)data;
	uint64_t *o = (uint64_t *)data;
	int error;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *f == SECTOR ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*o = se.nsect;
		return 0;
	case DKIOCISWRITABLE:
		*f = se.writable ? 1 : 0;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		if (!se.writable) {
			return 0;
		}
		lck_mtx_lock(&drg_emmc_lock);
		error = flush_cache();
		lck_mtx_unlock(&drg_emmc_lock);
		return error;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = (se.dma_size - DESC_BYTES) / SECTOR;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = se.writable ? (se.dma_size - DESC_BYTES) / SECTOR : 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
drg_emmc_psize(__unused dev_t dev)
{
	return se.ready ? (int)MIN(se.nsect, INT_MAX) : -1;
}

static int
drg_emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(drg_emmc_strategy, NULL, dev, B_READ, minphys, uio, SECTOR);
}

static int
drg_emmc_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (!se.writable) {
		return EROFS;
	}
	return physio(drg_emmc_strategy, NULL, dev, B_WRITE, minphys, uio, SECTOR);
}

static const struct bdevsw drg_emmc_bdevsw = {
	.d_open     = drg_emmc_open,
	.d_close    = drg_emmc_close,
	.d_strategy = drg_emmc_strategy,
	.d_ioctl    = drg_emmc_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = drg_emmc_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw drg_emmc_cdevsw = {
	.d_open       = drg_emmc_open,
	.d_close      = drg_emmc_close,
	.d_read       = drg_emmc_read,
	.d_write      = drg_emmc_write,
	.d_ioctl      = drg_emmc_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static int
drg_emmc_init(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL, *dma = NULL;
	unsigned int reg_size = 0, dma_size = 0;
	char want[64] = "userdata";
	uint32_t rw = 0;
	bool user_area = false;
	int error;

	if (SecureDTLookupEntry(NULL, "/drg-emmc", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &reg_size) != kSuccess ||
	    reg_size < 4 * sizeof(uintptr_t) ||
	    SecureDTGetProperty(node, "dma", (void const **)&dma, &dma_size) != kSuccess ||
	    dma_size < 2 * sizeof(uintptr_t) || dma[1] < DESC_BYTES + SECTOR) {
		printf("DRG EMMC: no usable /drg-emmc node in the device tree\n");
		return ENXIO;
	}
	PE_parse_boot_argn("emmcroot", want, sizeof(want));
	PE_parse_boot_argn("emmcrw", &rw, sizeof(rw));

	se.hc = ml_io_map(reg[0], reg[1]);
	se.cq = ml_io_map(reg[2], reg[3]);
	se.dma_pa = dma[0];
	se.dma_size = dma[1];
	se.dma = ml_io_map_wcomb(se.dma_pa, se.dma_size);
	if (!se.hc || !se.cq || !se.dma) {
		printf("DRG EMMC: cannot map the controller or the DMA range\n");
		return ENOMEM;
	}

	/* Only drive the controller in the state measured under ABL. */
	uint32_t cqcfg = *(volatile uint32_t *)(se.cq + CQ_CFG);
	uint32_t hostctl = hc_rd(HC_HOST_CTRL) & 0xff;
	printf("DRG EMMC: host ctrl 0x%02x, CQE cfg 0x%x, present 0x%08x, DMA range 0x%llx+0x%llx\n",
	    hostctl, cqcfg, hc_rd(HC_PRESENT), se.dma_pa, se.dma_size);
	if (cqcfg & CQ_CFG_ENABLE) {
		printf("DRG EMMC: command queue engine is on; not touching the card\n");
		return ENXIO;
	}
	if ((hostctl & HOST_CTRL_DMA_MASK) != HOST_CTRL_ADMA2_64) {
		printf("DRG EMMC: host control is not 64-bit ADMA2; not touching the card\n");
		return ENXIO;
	}
	hc_wr(HC_INT_ENABLE, hc_rd(HC_INT_ENABLE) | INT_ENABLE_WANT);

	/*
	 * EXT_CSD is only needed for writes: if it cannot be read, stay
	 * read-only (user_area stays false) rather than fail the root disk.
	 */
	lck_mtx_lock(&drg_emmc_lock);
	if (read_ext_csd(&user_area)) {
		printf("DRG EMMC: EXT_CSD unreadable; staying read-only\n");
		user_area = false;
		se.cache_on = false;
	}
	error = find_partition(want);
	lck_mtx_unlock(&drg_emmc_lock);
	if (error) {
		return error;
	}
	if (rw && !user_area) {
		printf("DRG EMMC: the user area is not confirmed; staying read-only\n");
	}
	se.writable = rw && user_area;

	se.bmajor = bdevsw_add(-1, &drg_emmc_bdevsw);
	if (se.bmajor < 0) {
		return ENXIO;
	}
	se.cmajor = cdevsw_add_with_bdev(-1, &drg_emmc_cdevsw, se.bmajor);
	if (se.cmajor < 0) {
		return ENXIO;
	}
	se.bdev = makedev(se.bmajor, 0);
	int mode = se.writable ? 0640 : 0440;
	devfs_make_node(se.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, mode, "emmcroot0");
	devfs_make_node(makedev(se.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, mode, "remmcroot0");
	se.ready = true;
	printf("DRG EMMC: emmcroot0 ready, %s\n", se.writable ? "read-write" : "read-only");
	return 0;
}

int drg_emmc_rootdev(dev_t *dev);

/* rd=emmcroot0 (iokit/bsddev/IOKitBSDInit.cpp) */
int
drg_emmc_rootdev(dev_t *dev)
{
	if (!se.ready) {
		int error = drg_emmc_init();
		if (error) {
			return error;
		}
	}
	*dev = se.bdev;
	return 0;
}

#endif /* ARM64_BOARD_CONFIG_DRG */
