/*
 * Read-only FAT16 reader for the firmware partitions (modem_a, bluetooth_a)
 * and a CRC-32 for checking what was read. No writes of any kind: the only
 * access to the medium is the caller's read callback. Host-tested against
 * the saved modem_a image (shim/test/fat16_test.c).
 */
#ifndef DRG_FAT_H
#define DRG_FAT_H
#include <stdint.h>

#define FAT_SECTOR		512u
#define FAT_MAX_READ		4096u	/* sectors per read call (2 MiB, the proven eMMC DMA size) */

/* count sectors from lba (relative to the partition) into buf; 0 on success */
typedef int (*fat_read_fn)(void *ctx, uint32_t lba, uint32_t count, void *buf);

struct fat16 {
	fat_read_fn read;
	void *ctx;
	uint32_t part_sectors;		/* partition size, from the GPT */
	uint32_t spc;			/* sectors per cluster */
	uint32_t fat_lba, fat_sectors;	/* first FAT */
	uint32_t root_lba, root_sectors, root_entries;
	uint32_t data_lba, clusters;	/* cluster 2 starts at data_lba */
	uint32_t total_sectors;
	uint32_t fat_cached;		/* sector number held in fatsec, or ~0 */
	uint8_t sec[FAT_SECTOR] __attribute__((aligned(64)));	/* directory / tail scratch */
	uint8_t fatsec[FAT_SECTOR] __attribute__((aligned(64)));
};

struct fat16_ent {
	uint32_t cluster;	/* first cluster (0 for an empty file) */
	uint32_t size;
	uint8_t attr;
	uint8_t name[11];	/* 8.3 name as stored */
};

/* error codes (negative): which check failed */
#define FAT_E_IO	(-1)	/* read callback failed */
#define FAT_E_BOOT	(-2)	/* not a FAT16 boot sector we accept */
#define FAT_E_NAME	(-3)	/* path component not a valid 8.3 name */
#define FAT_E_NOENT	(-4)	/* not found */
#define FAT_E_NOTDIR	(-5)
#define FAT_E_CHAIN	(-6)	/* cluster chain broken / out of range / too short */
#define FAT_E_SPACE	(-7)	/* file larger than the destination */
#define FAT_E_ISDIR	(-8)

/* fs->read / fs->ctx must be set; part_sectors from the GPT. 0 or FAT_E_* */
int fat16_mount(struct fat16 *fs, uint32_t part_sectors);
/* path like "image/mba.mbn" (8.3 components, case-insensitive). 0 or FAT_E_* */
int fat16_lookup(struct fat16 *fs, const char *path, struct fat16_ent *e);
/*
 * Read the whole file into dst (at most max bytes); whole sectors go straight
 * to dst (one read per contiguous cluster run, <= FAT_MAX_READ sectors each),
 * the last partial sector through fs->sec, so nothing past e->size is written.
 * dst must be 8-byte aligned for the direct path; otherwise every sector goes
 * through fs->sec. Returns the byte count (= e->size) or FAT_E_*.
 * *runs (optional) = number of read calls made.
 */
int32_t fat16_read(struct fat16 *fs, const struct fat16_ent *e, uint8_t *dst, uint32_t max, uint32_t *runs);

/* CRC-32 (IEEE 802.3, as zlib crc32); 8-byte loads where aligned (uncached memory) */
uint32_t drg_crc32(uint32_t crc, const volatile uint8_t *p, uint32_t len);

#endif
