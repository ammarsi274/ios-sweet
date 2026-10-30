/*
 * drgles ELF loader, see elf.h. Our own code; ELF64 field layouts are from
 * the ELF/AArch64 ABI the Nokia libraries are built for.
 */
#include "elf.h"
#include "host.h"
#include "rt.h"

/* ---- ELF64 ---- */
typedef struct {
	unsigned char e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;
typedef struct {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;
typedef struct {
	int64_t d_tag;
	uint64_t d_val;
} Dyn;
typedef struct {
	uint32_t st_name;
	unsigned char st_info, st_other;
	uint16_t st_shndx;
	uint64_t st_value, st_size;
} Sym;
typedef struct {
	uint64_t r_offset, r_info;
	int64_t r_addend;
} Rela;

#define ET_DYN 3
#define EM_AARCH64 183
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_GNU_RELRO 0x6474e552
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_SYMBOLIC 16
#define DT_REL 17
#define DT_PLTREL 20
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_FLAGS 30
#define DT_RELR 36
#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELA 0x60000011
#define DT_GNU_HASH 0x6ffffef5
#define DF_SYMBOLIC 0x2
#define DF_TEXTREL 0x4
#define SHN_UNDEF 0
#define STB_LOCAL 0
#define STB_WEAK 2
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027

#define MAXDEPS 32

struct drg_dso {
	char name[96];
	uint8_t *map;		/* host mapping: canary page, then the image */
	size_t maplen;
	uintptr_t bias;		/* load address - ELF vaddr */
	uintptr_t lo, hi;	/* image range in memory */
	const Dyn *dyn;
	const Sym *symtab;
	const char *strtab;
	const uint32_t *hash, *gnuhash;
	uint32_t nsyms;
	int symbolic;
	struct drg_dso *deps[MAXDEPS];
	int ndeps;
	int state;		/* 1 loading, 2 ready */
	struct drg_dso *next;
};

static struct drg_elf_config cfg;
static struct drg_dso *all;
static char err[256];
static unsigned tp_rewrites;

void drg_elf_setup(const struct drg_elf_config *c)
{
	cfg = *c;
}

const char *drg_elf_error(void)
{
	return err;
}

unsigned drg_elf_tp_rewrites(void)
{
	return tp_rewrites;
}

static void *fail(const char *fmt, const char *a, const char *b)
{
	snprintf(err, sizeof(err), fmt, a, b);
	if (cfg.verbose)
		host_log("drgles: %s\n", err);
	return 0;
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static int is_system(const char *name)
{
	for (const char *const *s = cfg.system; s && *s; s++)
		if (!strcmp(*s, name))
			return 1;
	return 0;
}

/* ---- symbol tables ---- */
static uint32_t sysv_hash(const char *s)
{
	uint32_t h = 0, g;
	while (*s) {
		h = (h << 4) + (unsigned char)*s++;
		g = h & 0xf0000000;
		if (g)
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

static uint32_t gnu_hash(const char *s)
{
	uint32_t h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static uint32_t count_syms(const struct drg_dso *d)
{
	if (d->hash)
		return d->hash[1];
	if (!d->gnuhash)
		return 0;
	uint32_t nb = d->gnuhash[0], off = d->gnuhash[1], bloom = d->gnuhash[2];
	const uint32_t *buckets = d->gnuhash + 4 + bloom * 2;
	const uint32_t *chain = buckets + nb;
	uint32_t max = 0;
	for (uint32_t i = 0; i < nb; i++)
		if (buckets[i] > max)
			max = buckets[i];
	if (max < off)
		return off;
	while (!(chain[max - off] & 1))
		max++;
	return max + 1;
}

static int usable(const Sym *s)
{
	int bind = s->st_info >> 4;
	return s->st_shndx != SHN_UNDEF && bind != STB_LOCAL && (s->st_other & 3) != 2 && (s->st_other & 3) != 1;
}

/* defined global symbol of d, or NULL */
static const Sym *dso_lookup(const struct drg_dso *d, const char *name)
{
	if (d->gnuhash) {
		uint32_t nb = d->gnuhash[0], off = d->gnuhash[1], bloom = d->gnuhash[2];
		const uint32_t *buckets = d->gnuhash + 4 + bloom * 2;
		const uint32_t *chain = buckets + nb;
		uint32_t h = gnu_hash(name), i = buckets[h % nb];
		if (i < off)
			return 0;
		for (;; i++) {
			uint32_t c = chain[i - off];
			const Sym *s = d->symtab + i;
			if ((c | 1) == (h | 1) && usable(s) && !strcmp(d->strtab + s->st_name, name))
				return s;
			if (c & 1)
				return 0;
		}
	}
	if (d->hash) {
		uint32_t nb = d->hash[0];
		const uint32_t *bucket = d->hash + 2, *chain = bucket + nb;
		for (uint32_t i = bucket[sysv_hash(name) % nb]; i; i = chain[i]) {
			const Sym *s = d->symtab + i;
			if (usable(s) && !strcmp(d->strtab + s->st_name, name))
				return s;
		}
	}
	return 0;
}

/* the order of Android's linker for one library: itself if DF_SYMBOLIC,
 * then its dependency tree breadth first; the system libraries in that
 * tree are the resolver; then everything else loaded */
static void *resolve(const struct drg_dso *d, const char *name, int call)
{
	const Sym *s;
	if (d->symbolic && (s = dso_lookup(d, name)))
		return (void *)(d->bias + s->st_value);
	const struct drg_dso *q[64];
	int qh = 0, qt = 0;
	q[qt++] = d;
	while (qh < qt) {
		const struct drg_dso *x = q[qh++];
		if ((s = dso_lookup(x, name)))
			return (void *)(x->bias + s->st_value);
		for (int i = 0; i < x->ndeps && qt < 64; i++) {
			int seen = 0;
			for (int j = 0; j < qt; j++)
				seen |= q[j] == x->deps[i];
			if (!seen)
				q[qt++] = x->deps[i];
		}
	}
	void *p = cfg.resolve ? cfg.resolve(name, call, cfg.ctx) : 0;
	if (p)
		return p;
	for (const struct drg_dso *x = all; x; x = x->next)
		if (x->state && (s = dso_lookup(x, name)))
			return (void *)(x->bias + s->st_value);
	return 0;
}

/* ---- relocation ---- */
static int relocate(struct drg_dso *d, const Rela *r, size_t n)
{
	for (size_t i = 0; i < n; i++, r++) {
		uint32_t type = (uint32_t)r->r_info, si = (uint32_t)(r->r_info >> 32);
		uint64_t *where = (uint64_t *)(d->bias + r->r_offset);
		if ((uintptr_t)where < d->lo || (uintptr_t)where + 8 > d->hi) {
			fail("%s: relocation outside the image", d->name, 0);
			return -1;
		}
		if (type == R_AARCH64_RELATIVE) {
			*where = d->bias + r->r_addend;
			continue;
		}
		if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) {
			char t[16];
			snprintf(t, sizeof(t), "%u", type);
			fail("%s: relocation type %s not supported", d->name, t);
			return -1;
		}
		const Sym *s = d->symtab + si;
		const char *name = d->strtab + s->st_name;
		void *v;
		if ((s->st_info >> 4) == STB_LOCAL && s->st_shndx != SHN_UNDEF)
			v = (void *)(d->bias + s->st_value);
		else
			v = resolve(d, name, type == R_AARCH64_JUMP_SLOT);
		if (!v && (s->st_info >> 4) != STB_WEAK) {
			fail("%s: undefined symbol %s", d->name, name);
			return -1;
		}
		*where = (uint64_t)v + r->r_addend;
	}
	return 0;
}

/* `mrs Xt, TPIDR_EL0` -> `adrp Xt, canary page` (see elf.h) */
static void rewrite_tp(struct drg_dso *d, uintptr_t start, uintptr_t end)
{
	uintptr_t page = (uintptr_t)d->map;	/* the canary page */
	for (uintptr_t a = start & ~3UL; a + 4 <= end; a += 4) {
		uint32_t w = *(uint32_t *)a;
		if ((w & 0xffffffe0) != 0xd53bd040)
			continue;
		int64_t imm = (int64_t)(page >> 12) - (int64_t)(a >> 12);
		if (imm < -(1 << 20) || imm >= (1 << 20))
			host_die("drgles: %s: canary page out of adrp range\n", d->name);
		*(uint32_t *)a = 0x90000000 | (uint32_t)(imm & 3) << 29 | (uint32_t)((imm >> 2) & 0x7ffff) << 5 | (w & 31);
		tp_rewrites++;
	}
}

static uint64_t canary;

static struct drg_dso *load(const char *want);

static int run_init(struct drg_dso *d, uint64_t init, uint64_t arr, uint64_t arrsz)
{
	static char *argv[] = { "drgles", 0 }, *envp[] = { 0 };
	typedef void (*initfn)(int, char **, char **);
	if (init)
		((initfn)(d->bias + init))(1, argv, envp);
	for (uint64_t i = 0; arr && i < arrsz / 8; i++) {
		uint64_t f = ((uint64_t *)(d->bias + arr))[i];
		if (f && f != (uint64_t)-1)
			((initfn)f)(1, argv, envp);
	}
	return 0;
}

static struct drg_dso *load(const char *want)
{
	const char *name = base_name(want);
	if (is_system(name))
		return DRG_ELF_SYSTEM;
	for (struct drg_dso *x = all; x; x = x->next)
		if (!strcmp(x->name, name))
			return x;

	size_t size = 0;
	uint8_t *file = 0;
	char path[256];
	for (const char *const *dir = cfg.dirs; dir && *dir && !file; dir++) {
		snprintf(path, sizeof(path), "%s/%s", *dir, name);
		file = host_read_file(path, &size);
	}
	if (!file)
		return fail("%s: not found%s", name, "");
	const Ehdr *eh = (const Ehdr *)file;
	if (size < sizeof(Ehdr) || memcmp(eh->e_ident, "\177ELF\2\1", 6) || eh->e_type != ET_DYN ||
	    eh->e_machine != EM_AARCH64 || eh->e_phentsize != sizeof(Phdr) ||
	    eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Phdr) > size) {
		host_free(file);
		return fail("%s: not an AArch64 ELF shared object%s", name, "");
	}
	const Phdr *ph = (const Phdr *)(file + eh->e_phoff);
	uint64_t lo = ~0UL, hi = 0;
	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_TLS) {
			host_free(file);
			return fail("%s: has a TLS segment (not supported)%s", name, "");
		}
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_offset + ph[i].p_filesz > size || ph[i].p_filesz > ph[i].p_memsz) {
			host_free(file);
			return fail("%s: bad PT_LOAD%s", name, "");
		}
		if (ph[i].p_vaddr < lo)
			lo = ph[i].p_vaddr;
		if (ph[i].p_vaddr + ph[i].p_memsz > hi)
			hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	size_t pg = host_page_size();
	lo &= ~(uint64_t)(pg - 1);
	hi = (hi + pg - 1) & ~(uint64_t)(pg - 1);

	struct drg_dso *d = host_alloc(sizeof(*d));
	snprintf(d->name, sizeof(d->name), "%s", name);
	d->maplen = pg + (hi - lo);
	d->map = host_map(d->maplen, HOST_PROT_R | HOST_PROT_W);
	if (!d->map) {
		host_free(file);
		return fail("%s: out of address space%s", name, "");
	}
	if (!canary)
		canary = host_random64() & ~0xffUL;	/* low byte 0 stops string over-reads */
	*(uint64_t *)(d->map + 0x28) = canary;
	d->bias = (uintptr_t)d->map + pg - lo;
	d->lo = (uintptr_t)d->map + pg;
	d->hi = (uintptr_t)d->map + d->maplen;
	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_LOAD)
			memcpy((void *)(d->bias + ph[i].p_vaddr), file + ph[i].p_offset, ph[i].p_filesz);
		if (ph[i].p_type == PT_DYNAMIC)
			d->dyn = (const Dyn *)(d->bias + ph[i].p_vaddr);
	}
	if (!d->dyn) {
		host_free(file);
		return fail("%s: no PT_DYNAMIC%s", name, "");
	}

	uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, init = 0, initarr = 0, initarrsz = 0, flags = 0;
	for (const Dyn *dy = d->dyn; dy->d_tag != DT_NULL; dy++) {
		switch (dy->d_tag) {
		case DT_STRTAB: d->strtab = (const char *)(d->bias + dy->d_val); break;
		case DT_SYMTAB: d->symtab = (const Sym *)(d->bias + dy->d_val); break;
		case DT_HASH: d->hash = (const uint32_t *)(d->bias + dy->d_val); break;
		case DT_GNU_HASH: d->gnuhash = (const uint32_t *)(d->bias + dy->d_val); break;
		case DT_RELA: rela = dy->d_val; break;
		case DT_RELASZ: relasz = dy->d_val; break;
		case DT_JMPREL: jmprel = dy->d_val; break;
		case DT_PLTRELSZ: pltrelsz = dy->d_val; break;
		case DT_PLTREL:
			if (dy->d_val != DT_RELA)
				goto unsupported;
			break;
		case DT_INIT: init = dy->d_val; break;
		case DT_INIT_ARRAY: initarr = dy->d_val; break;
		case DT_INIT_ARRAYSZ: initarrsz = dy->d_val; break;
		case DT_SYMBOLIC: d->symbolic = 1; break;
		case DT_FLAGS: flags = dy->d_val; break;
		case DT_REL: case DT_TEXTREL: case DT_RELR: case DT_ANDROID_REL: case DT_ANDROID_RELA:
		unsupported: {
			char t[24];
			snprintf(t, sizeof(t), "0x%llx", (unsigned long long)dy->d_tag);
			host_free(file);
			return fail("%s: dynamic tag %s not supported", name, t);
		}
		}
	}
	if (flags & DF_SYMBOLIC)
		d->symbolic = 1;
	if (!d->strtab || !d->symtab || (!d->hash && !d->gnuhash)) {
		host_free(file);
		return fail("%s: missing symbol tables%s", name, "");
	}
	d->nsyms = count_syms(d);
	d->state = 1;
	d->next = 0;
	struct drg_dso **tail = &all;
	while (*tail)
		tail = &(*tail)->next;
	*tail = d;

	for (const Dyn *dy = d->dyn; dy->d_tag != DT_NULL; dy++) {
		if (dy->d_tag != DT_NEEDED)
			continue;
		const char *dep = d->strtab + dy->d_val;
		struct drg_dso *x = load(dep);
		if (!x) {
			host_free(file);
			return fail("%s: dependency failed: %s", name, err);
		}
		if (x != DRG_ELF_SYSTEM && d->ndeps < MAXDEPS)
			d->deps[d->ndeps++] = x;
	}

	if (relocate(d, (const Rela *)(d->bias + rela), relasz / sizeof(Rela)) ||
	    relocate(d, (const Rela *)(d->bias + jmprel), pltrelsz / sizeof(Rela))) {
		host_free(file);
		return 0;
	}

	/* protections per host page: union of the segments touching it */
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X))
			rewrite_tp(d, d->bias + ph[i].p_vaddr, d->bias + ph[i].p_vaddr + ph[i].p_filesz);
	for (uintptr_t p = d->lo; p < d->hi; p += pg) {
		int prot = 0;
		for (int i = 0; i < eh->e_phnum; i++) {
			if (ph[i].p_type != PT_LOAD)
				continue;
			uintptr_t s = d->bias + ph[i].p_vaddr, e = s + ph[i].p_memsz;
			if (s < p + pg && e > p)
				prot |= (ph[i].p_flags & PF_R ? HOST_PROT_R : 0) | (ph[i].p_flags & PF_W ? HOST_PROT_W : 0) |
					(ph[i].p_flags & PF_X ? HOST_PROT_X : 0);
		}
		/* RELRO pages that hold nothing writable after relocation become read-only */
		for (int i = 0; i < eh->e_phnum; i++) {
			if (ph[i].p_type != PT_GNU_RELRO)
				continue;
			uintptr_t s = d->bias + ph[i].p_vaddr, e = s + ph[i].p_memsz;
			if (s <= p && e >= p + pg)
				prot &= ~HOST_PROT_W;
		}
		if (host_protect((void *)p, pg, prot ? prot : HOST_PROT_R))
			host_die("drgles: %s: protect %lx failed\n", name, (unsigned long)p);
		if (prot & HOST_PROT_X)
			host_sync_icache((void *)p, pg);
	}
	if (host_protect(d->map, pg, HOST_PROT_R))
		host_die("drgles: %s: canary page protect failed\n", name);
	host_free(file);

	if (cfg.verbose)
		host_log("drgles: loaded %s at %p (%zu KiB, %u symbols)\n", name, (void *)d->lo, (size_t)((d->hi - d->lo) >> 10), d->nsyms);
	run_init(d, init, initarr, initarrsz);
	d->state = 2;
	return d;
}

struct drg_dso *drg_elf_open(const char *name)
{
	err[0] = 0;
	return load(name);
}

void *drg_elf_sym(struct drg_dso *d, const char *name)
{
	const Sym *s;
	if (d == DRG_ELF_SYSTEM)
		return cfg.resolve ? cfg.resolve(name, 0, cfg.ctx) : 0;
	if (d) {
		if ((s = dso_lookup(d, name)))
			return (void *)(d->bias + s->st_value);
		return resolve(d, name, 0);
	}
	for (struct drg_dso *x = all; x; x = x->next)
		if ((s = dso_lookup(x, name)))
			return (void *)(x->bias + s->st_value);
	return cfg.resolve ? cfg.resolve(name, 0, cfg.ctx) : 0;
}

int drg_elf_addr(const void *addr, const char **file, void **base, const char **sym, void **symaddr)
{
	uintptr_t a = (uintptr_t)addr;
	for (struct drg_dso *d = all; d; d = d->next) {
		if (a < d->lo || a >= d->hi)
			continue;
		const Sym *best = 0;
		for (uint32_t i = 1; i < d->nsyms; i++) {
			const Sym *s = d->symtab + i;
			uintptr_t v = d->bias + s->st_value;
			if (s->st_shndx != SHN_UNDEF && v <= a && (!best || v > d->bias + best->st_value))
				best = s;
		}
		*file = d->name;
		*base = (void *)d->lo;
		*sym = best ? d->strtab + best->st_name : 0;
		*symaddr = best ? (void *)(d->bias + best->st_value) : 0;
		return 1;
	}
	return 0;
}
