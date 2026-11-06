/*
 * gptslot.c on the PC, against the phone's own GPT (backup/gpt-2026-11-03:
 * gpt-head.bin = LBA 0..33, gpt-tail.bin = the last 33 LBAs of 122,142,720).
 *   gptslot_test HEAD TAIL MODE OUTHEAD OUTTAIL [corrupt]
 * MODE 0 normal, 1 twrp once. Prints the result and every LBA written; the
 * output tables are checked independently by gptslot_check.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gptslot.h"

#define TOTAL 122142720ULL
static unsigned char head[34 * 512], tail[33 * 512];

static unsigned char *at(uint64_t lba)
{
	if (lba < 34)
		return head + 512 * lba;
	if (lba >= TOTAL - 33 && lba < TOTAL)
		return tail + 512 * (lba - (TOTAL - 33));
	return NULL;
}

static int rd(void *ctx, uint64_t lba, uint8_t *buf)
{
	unsigned char *p = at(lba);
	if (!p)
		return -1;
	memcpy(buf, p, 512);
	return 0;
}

static int wr(void *ctx, uint64_t lba, const uint8_t *buf)
{
	unsigned char *p = at(lba);
	if (!p || lba == 0)
		return -1;
	printf("  write LBA %llu\n", (unsigned long long)lba);
	memcpy(p, buf, 512);
	return 0;
}

static void load(const char *f, unsigned char *b, size_t n)
{
	FILE *fp = fopen(f, "rb");
	if (!fp || fread(b, 1, n, fp) != n) {
		fprintf(stderr, "read %s\n", f);
		exit(2);
	}
	fclose(fp);
}

static void save(const char *f, unsigned char *b, size_t n)
{
	FILE *fp = fopen(f, "wb");
	fwrite(b, 1, n, fp);
	fclose(fp);
}

int main(int argc, char **argv)
{
	if (argc < 6)
		return 2;
	load(argv[1], head, sizeof(head));
	load(argv[2], tail, sizeof(tail));
	if (argc > 6 && !strcmp(argv[6], "hdr"))
		head[512 + 40] ^= 1;	/* first usable LBA: inside the CRC'd header */
	if (argc > 6 && !strcmp(argv[6], "ent"))
		head[1024 + 200] ^= 1;	/* an entry byte: entry CRC fails */
	struct gpt_io io = { rd, wr, 0 };
	struct gpt_slots_result r;
	int rc = gpt_slots(&io, TOTAL - 1, atoi(argv[3]), &r);
	printf("rc %d primary %d backup %d was a %#x b %#x\n", rc, r.primary, r.backup, r.was_a, r.was_b);
	save(argv[4], head, sizeof(head));
	save(argv[5], tail, sizeof(tail));
	return rc < 0 ? 1 : 0;
}
