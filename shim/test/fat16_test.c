/*
 * Host test of drg_fat.c against a saved FAT16 partition image (read-only).
 *   gcc -O2 -Wall -Wextra -I.. -o fat16_test fat16_test.c ../drg_fat.c
 *   ./fat16_test modem_a.img image/mba.mbn image/modem.mdt ...
 * Prints, per file: size, CRC-32, number of read calls, and the SHA-256 of
 * the bytes read is left to the caller (the file is written to out/ only
 * with -o DIR). Also exercises the unaligned (bounce) path and the
 * FAT_MAX_READ split, and checks reads never touch bytes past the file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "drg_fat.h"

static FILE *img;
static uint64_t img_sectors;
static uint32_t max_count_seen, calls;

static int rd(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
	(void)ctx;
	if ((uint64_t)lba + count > img_sectors || count == 0 || count > FAT_MAX_READ)
		return -1;
	if (count > max_count_seen)
		max_count_seen = count;
	calls++;
	if (fseeko(img, (off_t)lba * 512, SEEK_SET) || fread(buf, 512, count, img) != count)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *outdir = NULL;
	int a = 1;
	if (argc > 2 && !strcmp(argv[1], "-o")) {
		outdir = argv[2];
		a = 3;
	}
	if (argc - a < 2) {
		fprintf(stderr, "usage: %s [-o dir] image path...\n", argv[0]);
		return 2;
	}
	img = fopen(argv[a], "rb");
	if (!img) {
		perror(argv[a]);
		return 2;
	}
	struct stat st;
	fstat(fileno(img), &st);
	img_sectors = (uint64_t)st.st_size / 512;

	static struct fat16 fs;
	fs.read = rd;
	fs.ctx = NULL;
	int rc = fat16_mount(&fs, (uint32_t)img_sectors);
	printf("mount rc %d spc %u fat_lba %u fat_sectors %u root_lba %u root_entries %u data_lba %u clusters %u total %u\n",
	       rc, fs.spc, fs.fat_lba, fs.fat_sectors, fs.root_lba, fs.root_entries, fs.data_lba, fs.clusters,
	       fs.total_sectors);
	if (rc)
		return 1;

	int bad = 0;
	for (int i = a + 1; i < argc; i++) {
		struct fat16_ent e;
		rc = fat16_lookup(&fs, argv[i], &e);
		if (rc) {
			printf("%s: lookup rc %d\n", argv[i], rc);
			bad++;
			continue;
		}
		/* guard bytes around the destination: nothing outside [0, size) may change */
		uint32_t cap = e.size + 4096;
		uint8_t *raw = malloc(cap + 64), *dst = raw + 16;	/* 8-aligned: the direct path */
		memset(raw, 0xa5, cap + 64);
		uint32_t runs = 0;
		calls = 0;
		int32_t n = fat16_read(&fs, &e, dst, e.size, &runs);
		uint32_t crc = n >= 0 ? drg_crc32(0, dst, (uint32_t)n) : 0;
		int guard_ok = 1;
		for (uint32_t k = 0; k < 16; k++)
			guard_ok &= raw[k] == 0xa5;
		for (uint32_t k = e.size; k < cap + 48; k++)
			guard_ok &= dst[k] == 0xa5;
		/* the same through the bounce path (odd destination) */
		uint8_t *raw2 = malloc(e.size + 64), *dst2 = raw2 + 3;
		memset(raw2, 0x5a, e.size + 64);
		uint32_t runs2 = 0;
		int32_t n2 = fat16_read(&fs, &e, dst2, e.size, &runs2);
		int same = n2 == n && n >= 0 && !memcmp(dst, dst2, (size_t)n);
		for (uint32_t k = e.size; k < e.size + 61; k++)
			guard_ok &= dst2[k] == 0x5a;
		/* a destination one byte too small must be refused */
		int32_t n3 = e.size ? fat16_read(&fs, &e, dst2, e.size - 1, NULL) : FAT_E_SPACE;
		printf("%s: cluster %u size %u read %d runs %u crc32 %08x bounce_same %d guard_ok %d small_refused %d\n",
		       argv[i], e.cluster, e.size, n, runs, crc, same, guard_ok, n3 == FAT_E_SPACE);
		if (n != (int32_t)e.size || !same || !guard_ok || n3 != FAT_E_SPACE)
			bad++;
		if (outdir && n >= 0) {
			char path[1024];
			const char *base = strrchr(argv[i], '/');
			snprintf(path, sizeof(path), "%s/%s", outdir, base ? base + 1 : argv[i]);
			FILE *o = fopen(path, "wb");
			if (o) {
				fwrite(dst, 1, (size_t)n, o);
				fclose(o);
			}
		}
		free(raw);
		free(raw2);
	}
	/* negative cases */
	struct fat16_ent e;
	printf("neg: missing %d, bad name %d, file as dir %d, dir as file %d\n",
	       fat16_lookup(&fs, "image/nosuch.bin", &e), fat16_lookup(&fs, "image/toolongname.mbn", &e),
	       fat16_lookup(&fs, "image/mba.mbn/x", &e), fat16_lookup(&fs, "image", &e));
	printf("max sectors per read call %u\n", max_count_seen);
	printf("%s\n", bad ? "FAIL" : "PASS");
	return bad ? 1 : 0;
}
