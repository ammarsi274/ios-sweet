/*
 * Boot slots like a normal phone (drg/BOOTLOG.md 2026-11-03 "Slots"): the
 * A/B flags ABL uses live in byte 6 of each GPT entry's attributes (bits
 * 48-55, measured against fastboot getvar for both slots): bits 0-1
 * priority, 2 active, 3-5 retry count, 6 successful, 7 unbootable. iOS can't
 * mark its boots successful, so without this every start uses a try and the
 * phone ends in download mode (2026-11-03).
 *
 *   normal:    boot_a = 0x7f (priority 3, active, 7 tries, successful), boot_b
 *              not active; nothing is written when that already holds.
 *   twrp once: boot_a = 0x7a (priority 2, not active, 7 tries, successful),
 *              boot_b = 0x0f (priority 3, active, one try, not successful):
 *              ABL boots TWRP once, then finds B out of tries and falls back
 *              to A ("Slot %s is unbootable, trying alternate slot").
 *
 * Both GPT copies, each checked first (signature, header CRC, entry CRC, the
 * shape measured on this phone: 88 entries of 128 bytes at LBA 2, backup
 * header at the last LBA), written entry sector(s) first, header last, and
 * read back. Anything unexpected: no write. Pure logic over two callbacks, so
 * it is tested on the PC with the phone's own GPT (shim/test/gptslot_test.c).
 */
#include "gptslot.h"

#define GPT_N		88
#define GPT_ESZ		128
#define GPT_ENT_SECTORS	(GPT_N * GPT_ESZ / 512)
#define ATTR_OFF	(48 + 6)

static uint8_t hdr[512], ent[GPT_N * GPT_ESZ], orig[GPT_N * GPT_ESZ], chk[512];

static uint32_t crc32(const uint8_t *p, unsigned n)
{
	uint32_t c = 0xffffffffU;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = c >> 1 ^ (0xedb88320U & -(c & 1));
	}
	return ~c;
}

static uint32_t g32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t g64(const uint8_t *p)
{
	return g32(p) | (uint64_t)g32(p + 4) << 32;
}

static void p32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static int name_is(const uint8_t *e, const char *want)
{
	for (unsigned i = 0; i < 36; i++) {
		uint16_t c = (uint16_t)(e[56 + 2 * i] | e[57 + 2 * i] << 8);
		if (c != (uint8_t)want[i])
			return 0;
		if (!c)
			return 1;
	}
	return 0;
}

static int hdr_crc_ok(void)
{
	uint32_t hs = g32(hdr + 12), want = g32(hdr + 16);
	if (hs < 92 || hs > 512)
		return 0;
	p32(hdr + 16, 0);
	int ok = crc32(hdr, hs) == want;
	p32(hdr + 16, want);
	return ok;
}

/* read and check one copy; *a, *b = entry index of boot_a, boot_b */
static int load(const struct gpt_io *io, uint64_t at, uint64_t other, int *a, int *b)
{
	if (io->rd(io->ctx, at, hdr))
		return -1;
	if (g32(hdr) != 0x20494645 || g32(hdr + 4) != 0x54524150 || !hdr_crc_ok())
		return -2;
	uint64_t ents = g64(hdr + 72);
	if (g64(hdr + 24) != at || g64(hdr + 32) != other || g32(hdr + 80) != GPT_N || g32(hdr + 84) != GPT_ESZ)
		return -3;
	if (ents != (at == 1 ? 2 : at - 32))	/* measured: LBA 2; backup 32 sectors below its header */
		return -4;
	for (unsigned s = 0; s < GPT_ENT_SECTORS; s++)
		if (io->rd(io->ctx, ents + s, ent + 512 * s))
			return -5;
	if (crc32(ent, sizeof(ent)) != g32(hdr + 88))
		return -6;
	*a = *b = -1;
	for (int i = 0; i < GPT_N; i++) {
		if (name_is(ent + i * GPT_ESZ, "boot_a"))
			*a = *a < 0 ? i : -2;
		if (name_is(ent + i * GPT_ESZ, "boot_b"))
			*b = *b < 0 ? i : -2;
	}
	return *a < 0 || *b < 0 ? -7 : 0;
}

static int put(const struct gpt_io *io, uint64_t lba, const uint8_t *buf)
{
	if (io->wr(io->ctx, lba, buf) || io->rd(io->ctx, lba, chk))
		return -1;
	for (int i = 0; i < 512; i++)
		if (chk[i] != buf[i])
			return -2;
	return 0;
}

/* one copy: change the two attribute bytes, rewrite CRCs, write what changed */
static int apply(const struct gpt_io *io, uint64_t at, uint64_t other, int mode, uint8_t *was_a, uint8_t *was_b)
{
	int a, b, rc = load(io, at, other, &a, &b);
	if (rc)
		return rc;
	uint8_t *pa = ent + a * GPT_ESZ + ATTR_OFF, *pb = ent + b * GPT_ESZ + ATTR_OFF;
	*was_a = *pa, *was_b = *pb;
	uint8_t na = mode == GPT_SLOTS_TWRP_ONCE ? 0x7a : 0x7f;
	uint8_t nb = mode == GPT_SLOTS_TWRP_ONCE ? 0x0f : (uint8_t)(*pb & ~0x04);
	if (*pa == na && *pb == nb)
		return 0;
	for (unsigned i = 0; i < sizeof(ent); i++)
		orig[i] = ent[i];
	*pa = na, *pb = nb;
	p32(hdr + 88, crc32(ent, sizeof(ent)));
	p32(hdr + 16, 0);
	p32(hdr + 16, crc32(hdr, g32(hdr + 12)));
	uint64_t ents = g64(hdr + 72);
	for (unsigned s = 0; s < GPT_ENT_SECTORS; s++) {
		int same = 1;
		for (int i = 0; i < 512 && same; i++)
			same = ent[512 * s + i] == orig[512 * s + i];
		if (!same && (rc = put(io, ents + s, ent + 512 * s)))
			return -10 + rc;
	}
	if ((rc = put(io, at, hdr)))
		return -20 + rc;
	return 1;
}

int gpt_slots(const struct gpt_io *io, uint64_t last_lba, int mode, struct gpt_slots_result *r)
{
	r->primary = r->backup = 0;
	r->was_a = r->was_b = 0;
	if (io->rd(io->ctx, 1, hdr))
		return r->primary = -1;
	if (g64(hdr + 32) != last_lba)	/* the primary's backup LBA must be the measured last sector */
		return r->primary = -8;
	r->primary = apply(io, 1, last_lba, mode, &r->was_a, &r->was_b);
	if (r->primary < 0)
		return r->primary;	/* the backup is left alone */
	uint8_t a, b;
	r->backup = apply(io, last_lba, 1, mode, &a, &b);
	return r->backup < 0 ? r->backup : r->primary | r->backup;
}
