/*
 * Read-only FAT16 reader (see drg_fat.h). Layout from the FAT specification
 * (Microsoft "FAT: General Overview of On-Disk Format"): boot sector BPB at
 * 11..35, FAT16 entries 2 bytes, end of chain >= 0xFFF8, directory entries
 * 32 bytes (name 0..10, attr 11, first cluster 26..27, size 28..31),
 * 0xE5 = deleted, 0x00 = end, attr 0x0F = long-name entry, 0x08 = label,
 * 0x10 = directory. Long names are ignored: lookups use the 8.3 names
 * (modem_a's files all have one; checked on the saved image).
 * File-system sectors of 512..4096 bytes: bluetooth_a (backup 2026-11-06)
 * is FAT16 with 4096-byte sectors on the 512-byte eMMC, and its boot sector
 * claims 16384 such sectors (64 MiB) on a 1 MiB partition; that is accepted
 * (fs->oversize = 1) because every read is checked against the partition.
 */
#include "drg_fat.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* lba, count in file-system sectors */
static int rd(struct fat16 *fs, uint32_t lba, uint32_t count, void *buf)
{
	uint64_t dl = (uint64_t)lba << fs->shift, dc = (uint64_t)count << fs->shift;
	if (dl >= fs->part_sectors || dc > fs->part_sectors - dl || count == 0 || dc > FAT_MAX_READ)
		return FAT_E_IO;
	return fs->read(fs->ctx, (uint32_t)dl, (uint32_t)dc, buf) ? FAT_E_IO : 0;
}

int fat16_mount(struct fat16 *fs, uint32_t part_sectors)
{
	fs->part_sectors = part_sectors;
	fs->fat_cached = ~0u;
	fs->bps = FAT_SECTOR;
	fs->shift = 0;
	fs->oversize = 0;
	if (part_sectors < 64)
		return FAT_E_BOOT;
	int rc = rd(fs, 0, 1, fs->sec);
	if (rc)
		return rc;
	const uint8_t *b = fs->sec;
	uint32_t bps = le16(b + 11), spc = b[13], resv = le16(b + 14), nfats = b[16];
	uint32_t rootent = le16(b + 17), tot16 = le16(b + 19), fatsz = le16(b + 22), tot32 = le32(b + 32);
	uint32_t tot = tot16 ? tot16 : tot32, shift = 0;
	while (shift < 4 && (FAT_SECTOR << shift) != bps)
		shift++;
	if (b[510] != 0x55 || b[511] != 0xaa || shift > 3 || spc == 0 || (spc & (spc - 1)) || spc > 128 ||
	    resv == 0 || nfats == 0 || nfats > 2 || rootent == 0 || (rootent * 32) % bps || fatsz == 0 ||
	    (uint64_t)fatsz * bps > 0x40000)
		return FAT_E_BOOT;
	fs->oversize = ((uint64_t)tot << shift) > part_sectors;
	if (fs->oversize && shift == 0)	/* a 512-byte FS larger than its partition stays refused (modem_a) */
		return FAT_E_BOOT;
	fs->bps = bps;
	fs->shift = shift;
	fs->spc = spc;
	fs->fat_lba = resv;
	fs->fat_sectors = fatsz;
	fs->root_lba = resv + nfats * fatsz;
	fs->root_entries = rootent;
	fs->root_sectors = rootent * 32 / bps;
	fs->data_lba = fs->root_lba + fs->root_sectors;
	fs->total_sectors = tot;
	if (fs->data_lba >= tot)
		return FAT_E_BOOT;
	fs->clusters = (tot - fs->data_lba) / spc;
	/* FAT16 by cluster count; the FAT must hold an entry for every cluster */
	if (fs->clusters < 4085 || fs->clusters > 65524 || (fs->clusters + 2) * 2 > fatsz * bps)
		return FAT_E_BOOT;
	return 0;
}

/* next cluster of cl, or a negative error */
static int32_t fat_next(struct fat16 *fs, uint32_t cl)
{
	uint32_t off = cl * 2, s = fs->fat_lba + off / fs->bps;
	if (fs->fat_cached != s) {
		int rc = rd(fs, s, 1, fs->fatsec);
		if (rc) {
			fs->fat_cached = ~0u;
			return rc;
		}
		fs->fat_cached = s;
	}
	return le16(fs->fatsec + off % fs->bps);
}

static int cl_ok(const struct fat16 *fs, uint32_t cl)
{
	return cl >= 2 && cl < fs->clusters + 2;
}

static uint32_t cl_lba(const struct fat16 *fs, uint32_t cl)
{
	return fs->data_lba + (cl - 2) * fs->spc;
}

static char up(char c)
{
	return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* one path component (ends at '/' or NUL) to an 11-byte 8.3 name; returns its length or -1 */
static int to83(const char *p, uint8_t n[11])
{
	int i = 0, k = 0, ext = 0;
	for (int j = 0; j < 11; j++)
		n[j] = ' ';
	for (; p[i] && p[i] != '/'; i++) {
		char c = p[i];
		if (c == '.') {
			if (ext || i == 0)
				return -1;
			ext = 1;
			k = 8;
			continue;
		}
		if ((unsigned char)c <= ' ' || c == '"' || c == '*' || c == '+' || c == ',' || c == ':' || c == ';' ||
		    c == '<' || c == '=' || c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' || c == '|')
			return -1;
		if ((!ext && k >= 8) || (ext && k >= 11))
			return -1;
		n[k++] = (uint8_t)up(c);
	}
	return i ? i : -1;
}

/* look for name in the directory at cluster dir (0 = the fixed root directory) */
static int dir_find(struct fat16 *fs, uint32_t dir, const uint8_t name[11], struct fat16_ent *e)
{
	uint32_t cl = dir, guard = 0;
	for (;;) {
		uint32_t lba, n;
		if (dir == 0) {
			lba = fs->root_lba;
			n = fs->root_sectors;
		} else {
			if (!cl_ok(fs, cl) || ++guard > fs->clusters)
				return FAT_E_CHAIN;
			lba = cl_lba(fs, cl);
			n = fs->spc;
		}
		for (uint32_t s = 0; s < n; s++) {
			int rc = rd(fs, lba + s, 1, fs->sec);
			if (rc)
				return rc;
			for (uint32_t k = 0; k < fs->bps / 32; k++) {
				const uint8_t *d = fs->sec + 32 * k;
				if (d[0] == 0)
					return FAT_E_NOENT;
				if (d[0] == 0xe5 || d[11] == 0x0f || (d[11] & 0x08))
					continue;
				int same = 1;
				for (int j = 0; j < 11 && same; j++)
					same = up((char)d[j]) == (char)name[j];
				if (!same)
					continue;
				volatile uint8_t *en = e->name;	/* no memcpy in the shim */
				for (int j = 0; j < 11; j++)
					en[j] = d[j];
				e->attr = d[11];
				e->cluster = le16(d + 26);	/* FAT16: high word (20..21) is 0 */
				e->size = le32(d + 28);
				return 0;
			}
		}
		if (dir == 0)
			return FAT_E_NOENT;
		int32_t nx = fat_next(fs, cl);
		if (nx < 0)
			return (int)nx;
		if (nx >= 0xfff8)
			return FAT_E_NOENT;
		cl = (uint32_t)nx;
	}
}

int fat16_lookup(struct fat16 *fs, const char *path, struct fat16_ent *e)
{
	uint32_t dir = 0;
	uint8_t name[11];
	while (*path == '/')
		path++;
	for (;;) {
		int len = to83(path, name);
		if (len < 0)
			return FAT_E_NAME;
		int rc = dir_find(fs, dir, name, e);
		if (rc)
			return rc;
		path += len;
		while (*path == '/')
			path++;
		if (!*path)
			return (e->attr & 0x10) ? FAT_E_ISDIR : 0;
		if (!(e->attr & 0x10))
			return FAT_E_NOTDIR;
		dir = e->cluster;
		if (!cl_ok(fs, dir))
			return FAT_E_CHAIN;
	}
}

int32_t fat16_read(struct fat16 *fs, const struct fat16_ent *e, uint8_t *dst, uint32_t max, uint32_t *runs)
{
	uint32_t size = e->size, done = 0, nruns = 0;
	uint32_t cbytes = fs->spc * fs->bps, maxs = FAT_MAX_READ >> fs->shift;
	int direct = ((uintptr_t)dst & 7) == 0;
	volatile uint8_t *vd = dst;	/* byte copies stay byte stores: no memcpy in the shim */
	if (runs)
		*runs = 0;
	if (e->attr & 0x10)
		return FAT_E_ISDIR;
	if (size > max)
		return FAT_E_SPACE;
	if (size == 0)
		return 0;
	uint32_t need = (uint32_t)(((uint64_t)size + cbytes - 1) / cbytes);	/* clusters the file needs */
	if (need > fs->clusters)
		return FAT_E_CHAIN;
	uint32_t cl = e->cluster, used = 0;
	while (done < size) {
		if (!cl_ok(fs, cl) || used >= need)
			return FAT_E_CHAIN;
		/* a run of consecutive clusters, no longer than the file still needs */
		uint32_t first = cl, n = 1;
		used++;
		for (;;) {
			int32_t nx = fat_next(fs, cl);
			if (nx < 0)
				return (int32_t)nx;
			if (used == need) {
				cl = (uint32_t)nx;	/* end of the file's clusters: nx should be EOC */
				break;
			}
			if ((uint32_t)nx != cl + 1 || !cl_ok(fs, (uint32_t)nx)) {
				cl = (uint32_t)nx;
				break;
			}
			cl = (uint32_t)nx;
			n++;
			used++;
		}
		uint32_t lba = cl_lba(fs, first);
		uint32_t bytes = n * cbytes;
		if (bytes > size - done)
			bytes = size - done;
		uint32_t full = bytes / fs->bps, tail = bytes % fs->bps;
		for (uint32_t s = 0; s < full;) {
			uint32_t c = full - s;
			if (!direct)
				c = 1;
			else if (c > maxs)
				c = maxs;
			uint8_t *to = direct ? dst + done : fs->sec;
			int rc = rd(fs, lba + s, c, to);
			nruns++;
			if (rc)
				return rc;
			if (!direct)
				for (uint32_t i = 0; i < fs->bps; i++)
					vd[done + i] = fs->sec[i];
			done += c * fs->bps;
			s += c;
		}
		if (tail) {
			int rc = rd(fs, lba + full, 1, fs->sec);
			nruns++;
			if (rc)
				return rc;
			for (uint32_t i = 0; i < tail; i++)
				vd[done + i] = fs->sec[i];
			done += tail;
		}
	}
	if (cl < 0xfff8)	/* the chain must end right after the file's last cluster */
		return FAT_E_CHAIN;
	if (runs)
		*runs = nruns;
	return (int32_t)done;
}

static uint32_t crc_tab[256];
static int crc_ready;

uint32_t drg_crc32(uint32_t crc, const volatile uint8_t *p, uint32_t len)
{
	if (!crc_ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			crc_tab[i] = c;
		}
		crc_ready = 1;
	}
	crc = ~crc;
	while (len && ((uintptr_t)p & 7)) {
		crc = crc_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
		len--;
	}
	for (; len >= 8; len -= 8, p += 8) {
		uint64_t w = *(const volatile uint64_t *)p;	/* one 8-byte load: the shim's memory is uncached */
		for (int k = 0; k < 8; k++, w >>= 8)
			crc = crc_tab[(crc ^ (uint32_t)w) & 0xff] ^ (crc >> 8);
	}
	while (len--)
		crc = crc_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}
