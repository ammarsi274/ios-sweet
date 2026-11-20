/*
 * GPT lookup by name, read-only (see drg_part.h). Header at LBA 1 ("EFI
 * PART"), entry array LBA at +72, count +80, entry size +84 (must be 128);
 * entry: first LBA +32, last LBA +40, name UTF-16LE at +56 (36 chars).
 * Moved here from drg_w1.c (boot #55/#56 found modem_a with it).
 */
#include "drg_part.h"
#include "emmc.h"

static uint8_t gsec[512] __attribute__((aligned(64)));

static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t g64(const uint8_t *p) { return g32(p) | (uint64_t)g32(p + 4) << 32; }

int drg_gpt_find(const char *name, uint64_t *first, uint64_t *last, uint32_t *index)
{
	if (drg_emmc_read(1, 1, gsec) || g32(gsec) != 0x20494645 || g32(gsec + 4) != 0x54524150)	/* "EFI PART" */
		return -1;
	uint64_t ent = g64(gsec + 72);
	uint32_t n = g32(gsec + 80), esz = g32(gsec + 84);
	if (esz != 128 || n == 0 || n > 128)
		return -2;
	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0 && drg_emmc_read(ent + i / 4, 1, gsec))
			return -3;
		const uint8_t *e = gsec + (i % 4) * 128;
		int k = 0;
		for (; k < 36; k++) {
			uint16_t c = (uint16_t)(e[56 + 2 * k] | e[57 + 2 * k] << 8);
			if (c != (uint8_t)name[k] || c == 0)
				break;
		}
		if (k < 36 && name[k] == 0 && e[56 + 2 * k] == 0 && e[57 + 2 * k] == 0) {
			*first = g64(e + 32);
			*last = g64(e + 40);
			if (index)
				*index = i + 1;
			return 0;
		}
	}
	return -4;
}

int drg_part_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
	return drg_emmc_read(*(const uint64_t *)ctx + lba, count, buf);
}
