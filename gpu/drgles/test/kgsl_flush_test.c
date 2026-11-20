/*
 * Host test of kgsl_ios.c's cache maintenance before a submit
 * (research/reports/gpu-fault-2.md §5.1): replays a drgles capture
 * (research/vendor-adreno/capture/cap1.bin, Nokia's driver on TWRP) through
 * kgsl_ioctl against a mock DrgGPU that hands out the capture's own GPU
 * addresses, records every range kgsl_ios.c would clean+invalidate
 * (KGSL_IOS_TEST_FLUSH replaces the dc civac loop), and checks per submit:
 *   FLUSH_ALL  every IB1, every IB2/draw-state group it points to, and every
 *              64-bit GPU address in them (cap-decode.py scan_refs rule) lies
 *              in a flushed line; every live non-import object is flushed
 *              whole; an imported IOSurface is never touched
 *   FLUSH_IBS  (r23) the IB1s are flushed; counts the references left out,
 *              which shows that the check above can fail
 *   DELAY      nothing flushed, at least 2 ms spent before the submit
 *   gcc -O1 -Wall -Wextra -DKGSL_IOS_TEST_FLUSH -I.. -o /tmp/kf kgsl_flush_test.c ../kgsl_ios.c ../kgsl_cap.c
 *   /tmp/kf CAPTURE
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kgsl.h"
#include "../host.h"
#include "../../drgpu/drgpu_uc.h"

typedef unsigned int mach_port_t;
mach_port_t mach_task_self_ = 0x103;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- host.h stubs; the clock advances 50 us per read ---- */
static int next_fd = 7;
static int64_t clock_ns;
int host_open(const char *path, int flags, int mode) { (void)flags; (void)mode; CHECK(!strcmp(path, "/dev/null")); return next_fd++; }
int host_close(int fd) { (void)fd; return 0; }
void *host_alloc(size_t n) { return calloc(1, n); }
void host_free(void *p) { free(p); }
void host_sleep_ns(int64_t ns) { clock_ns += ns; }
int host_clock(int c, struct host_ts *t) { (void)c; clock_ns += 50000; t->sec = clock_ns / 1000000000; t->nsec = clock_ns % 1000000000; return 0; }
long host_write(int fd, const void *b, size_t n) { (void)fd; (void)b; return (long)n; }
static int quiet;
void host_log(const char *fmt, ...) { if (quiet) return; va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); va_end(ap); }

/* ---- the flush recorder ---- */
#define NFL 4096
static struct { uint64_t a, b; } fl[NFL];	/* flushed lines [a, b), 64-byte granular */
static unsigned nfl, fl_overflow;
void kgsl_test_flush(uint64_t va, uint64_t len)
{
	if (nfl == NFL) {
		fl_overflow++;
		return;
	}
	fl[nfl].a = va & ~63ull;
	fl[nfl].b = (va + len + 63) & ~63ull;
	nfl++;
}

static int line_flushed(uint64_t addr)
{
	for (unsigned i = 0; i < nfl; i++)
		if (addr >= fl[i].a && addr < fl[i].b)
			return 1;
	return 0;
}

static int range_flushed(uint64_t a, uint64_t len)
{
	for (uint64_t x = a & ~63ull; x < a + len; x += 64)
		if (!line_flushed(x))
			return 0;
	return 1;
}

static int range_touched(uint64_t a, uint64_t len)
{
	for (unsigned i = 0; i < nfl; i++)
		if (fl[i].a < a + len && a < fl[i].b)
			return 1;
	return 0;
}

/* ---- the capture (kgsl_cap.c records) ---- */
enum { CAP_ALLOC = 1, CAP_MAP, CAP_FREE, CAP_SUBMIT, CAP_OBJ, CAP_PROP, CAP_CTX, CAP_WAIT, CAP_MEMSTORE };
static uint8_t *cap;
static size_t caplen;

struct rec { uint32_t type, len; const uint8_t *p; };
static int rec_at(size_t *pos, struct rec *r)
{
	uint32_t h[4];
	if (*pos + 16 > caplen)
		return 0;
	memcpy(h, cap + *pos, 16);
	if (h[0] != 0x43475244 || *pos + 16 + h[2] > caplen) {
		printf("FAIL: bad record at %zu\n", *pos);
		fails++;
		return 0;
	}
	r->type = h[1];
	r->len = h[2];
	r->p = cap + *pos + 16;
	*pos += 16 + h[2];
	return 1;
}

/* object contents as the capture last saw them (by capture id), and their addresses */
#define NOBJ 1024
static struct cobj { uint32_t id; int live, import; uint64_t va, size; uint8_t *data; uint64_t dlen; } cobjs[NOBJ];
static unsigned ncobj;

static struct cobj *cobj_va(uint64_t va)
{
	for (unsigned i = 0; i < ncobj; i++)
		if (cobjs[i].live && va >= cobjs[i].va && va < cobjs[i].va + cobjs[i].size)
			return &cobjs[i];
	return 0;
}

static struct cobj *cobj_id(uint32_t id)
{
	for (unsigned i = 0; i < ncobj; i++)
		if (cobjs[i].live && cobjs[i].id == id)
			return &cobjs[i];
	return 0;
}

/* ---- mock DrgGPU ---- */
#define E(x) (int)(0xe0000000u | (x))
static uint64_t pend_va, pend_size;
static uint32_t pend_id;
static int submits_seen;
#define IMPORT_VA 0x500000000ull
#define IMPORT_SIZE 0xac4000ull
void *IOServiceMatching(const char *name) { CHECK(!strcmp(name, "DrgGPU")); return (void *)1; }
mach_port_t IOServiceGetMatchingService(mach_port_t p, const void *m) { (void)p; (void)m; return 0x200; }
int IOServiceOpen(mach_port_t s, mach_port_t task, uint32_t type, mach_port_t *c) { (void)s; (void)task; (void)type; *c = 0x300; return 0; }
int IOServiceClose(mach_port_t c) { (void)c; return 0; }
int IOObjectRelease(mach_port_t o) { (void)o; return 0; }
int IOConnectCallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t nin, const void *sin, size_t nsin,
			uint64_t *out, uint32_t *nout, void *sout, size_t *nsout)
{
	(void)c; (void)nin; (void)sin; (void)nsin; (void)nout; (void)sout; (void)nsout;
	switch (sel) {
	case DRGPU_UC_GET_VERSION: out[0] = DRGPU_UC_VERSION; return 0;
	case DRGPU_UC_SET_OPTIONS: out[0] = in[0]; return 0;
	case DRGPU_UC_OBJ_ALLOC:	/* the capture's id and address (its 4 KiB mmapsize, not DrgGPU's 16 KiB) */
		CHECK(in[0] <= pend_size);
		out[0] = pend_id; out[1] = pend_va; out[2] = pend_size;
		return 0;
	case DRGPU_UC_OBJ_FREE: return 0;
	case DRGPU_UC_SUBMIT: submits_seen++; out[0] = in[2]; return 0;
	case DRGPU_UC_WAIT: return 0;
	case DRGPU_UC_SURFACE_MAP: out[0] = 9999; out[1] = IMPORT_VA; out[2] = IMPORT_SIZE; return 0;
	default: printf("FAIL: unexpected selector %u\n", sel); fails++; return E(0x2c7);
	}
}

struct gpuobj_alloc { uint64_t size, flags, va_len, mmapsize; uint32_t id, metadata_len; uint64_t metadata; };
struct gpuobj_free { uint64_t flags, priv; uint32_t id, type, len; };
struct gpu_command { uint64_t flags, cmdlist; uint32_t cmdsize, numcmds; uint64_t objlist; uint32_t objsize, numobjs;
		     uint64_t synclist; uint32_t syncsize, numsyncs, context_id, timestamp; };
struct command_object { uint64_t offset, gpuaddr, size; uint32_t flags, id; };
struct gpuobj_import { uint64_t priv, priv_len, flags; uint32_t type, id; };

/* ---- what one submit's IBs reference ---- */
struct stats { unsigned ibs, ib2s, groups, refs, refs_out, global_refs, unowned, ib2_out, walk_stops; };

static const uint32_t *words_at(uint64_t va, uint64_t bytes)
{
	struct cobj *o = cobj_va(va);
	if (!o || !o->data || va + bytes > o->va + o->dlen)
		return 0;
	return (const uint32_t *)(o->data + (va - o->va));
}

static void scan_refs(const uint32_t *w, unsigned nw, struct stats *s, const char *what, int sub)
{
	for (unsigned k = 0; k + 1 < nw; k++) {
		uint32_t lo = w[k], hi = w[k + 1];
		if (hi == 0 && lo >= 0xf8000000u && lo < 0xfa000000u) {
			s->global_refs++;	/* the kernel's global objects (memstore): not ours to flush */
			continue;
		}
		if (!(hi == 7 && lo >= 0xf0000000u))
			continue;
		uint64_t a = (uint64_t)hi << 32 | lo;
		s->refs++;
		if (!cobj_va(a))
			s->unowned++;
		if (!line_flushed(a)) {
			s->refs_out++;
			if (s->refs_out <= 3 && !quiet)
				printf("    submit %d: %s ref %#llx not flushed\n", sub, what, (unsigned long long)a);
		}
	}
}

/* walk an IB's packets (type 4/7); follow CP_INDIRECT_BUFFER_PFE/PFD and CP_SET_DRAW_STATE groups once */
static void walk(uint64_t va, uint64_t bytes, int depth, struct stats *s, int sub)
{
	const uint32_t *w = words_at(va, bytes);
	unsigned nw = (unsigned)(bytes / 4);
	if (!w) {
		s->walk_stops++;
		return;
	}
	scan_refs(w, nw, s, depth ? "IB2" : "IB1", sub);
	for (unsigned k = 0; k < nw;) {
		uint32_t d = w[k], cnt;
		if (d >> 28 == 7) {
			uint32_t op = (d >> 16) & 0x7f;
			cnt = d & 0x3fff;
			if ((op == 0x3f || op == 0x37) && cnt >= 3 && k + 3 < nw && !depth) {
				uint64_t a = (uint64_t)w[k + 2] << 32 | w[k + 1], n = (uint64_t)(w[k + 3] & 0xfffff) * 4;
				s->ib2s++;
				if (!range_flushed(a, n))
					s->ib2_out++;
				walk(a, n, 1, s, sub);
			}
			if (op == 0x43 && !depth)	/* CP_SET_DRAW_STATE: {count | flags, lo, hi} per group */
				for (uint32_t g = 0; g + 2 < cnt && k + 3 + g < nw; g += 3) {
					uint64_t a = (uint64_t)w[k + 3 + g] << 32 | w[k + 2 + g];
					uint64_t n = (uint64_t)(w[k + 1 + g] & 0xffff) * 4;
					if (!n || !a)
						continue;
					s->groups++;
					if (!range_flushed(a, n))
						s->ib2_out++;
					walk(a, n, 1, s, sub);
				}
		} else if (d >> 28 == 4) {
			cnt = d & 0x7f;
		} else {
			s->walk_stops++;	/* not a packet header: stop (the IB tail of a dword we can't parse) */
			return;
		}
		k += 1 + cnt;
	}
}

static uint64_t find_map_va(size_t pos, uint32_t id, uint64_t *len)
{
	struct rec r;
	while (rec_at(&pos, &r)) {
		if (r.type == CAP_MAP) {
			uint64_t v[3];
			memcpy(v, r.p, sizeof(v));
			if (v[0] >> 12 == id) {
				*len = v[2];
				return v[1];
			}
		}
		if (r.type == CAP_FREE && *(const uint32_t *)r.p == id)
			break;
	}
	return 0;
}

/* one replay of the whole capture with flush mode `mode`; returns the summed stats */
static struct stats replay(int selfmode, int mode, int *nsub_out, unsigned *delay_bad)
{
	struct stats tot = { 0 };
	int nsub = 0;
	for (unsigned i = 0; i < ncobj; i++)
		free(cobjs[i].data);
	memset(cobjs, 0, sizeof(cobjs));
	ncobj = 0;
	submits_seen = 0;
	*delay_bad = 0;
	if (selfmode)
		kgsl_ios_mode(selfmode);	/* the self-test's number, as bench and libdrgeagl set it */
	else
		kgsl_ios_flush(mode);	/* mode 4 also reads back: not on the host, the addresses are the capture's */
	int fd = kgsl_open(0x80002);
	CHECK(fd >= 0);
	int imported = 0;
	size_t pos = 0;
	struct rec r;
	uint64_t fake_va = 0x600000000ull;
	while (rec_at(&pos, &r)) {
		if (r.type == CAP_ALLOC) {
			struct gpuobj_alloc a;
			memcpy(&a, r.p, sizeof(a));
			uint64_t len = 0, va = find_map_va(pos, a.id, &len);
			if (!va) {	/* never mapped by the driver: somewhere else */
				va = fake_va;
				fake_va += (a.mmapsize + 0xfff) & ~0xfffull;
			}
			pend_id = a.id, pend_va = va, pend_size = a.mmapsize;
			struct gpuobj_alloc b = { a.size, a.flags, 0, 0, 0, 0, 0 };
			CHECK(kgsl_ioctl(fd, 0xc0300945, &b) == 0 && b.id == a.id);
			CHECK(ncobj < NOBJ);
			cobjs[ncobj++] = (struct cobj){ a.id, 1, 0, va, a.mmapsize, 0, 0 };
		} else if (r.type == CAP_FREE) {
			uint32_t id;
			memcpy(&id, r.p, 4);
			struct cobj *o = cobj_id(id);
			struct gpuobj_free f = { 0, 0, id, 0, 0 };
			if (o) {
				CHECK(kgsl_ioctl(fd, 0x40200946, &f) == 0);
				CHECK(kgsl_munmap((void *)(uintptr_t)o->va, o->size) == 1);
				o->live = 0;
			}
		} else if (r.type == CAP_OBJ) {
			struct { uint32_t id, phase, submit, has_data; uint64_t gpuaddr, len, hash; } h;
			memcpy(&h, r.p, sizeof(h));
			struct cobj *o = cobj_id(h.id);
			if (h.has_data && o && r.len >= sizeof(h) + h.len) {
				free(o->data);
				o->data = malloc(h.len);
				memcpy(o->data, r.p + sizeof(h), h.len);
				o->dlen = h.len;
			}
		} else if (r.type == CAP_SUBMIT) {
			if (!imported) {	/* an IOSurface import live during every submit: must never be flushed */
				int sfd = kgsl_ios_surface_fd(2);
				struct gpuobj_import im = { (uintptr_t)&sfd, 4, 0x10001200, 3, 0 };
				CHECK(kgsl_ioctl(fd, 0xc0200948, &im) == 0 && im.id == 9999);
				CHECK(ncobj < NOBJ);
				cobjs[ncobj++] = (struct cobj){ 9999, 1, 1, IMPORT_VA, IMPORT_SIZE, 0, 0 };
				imported = 1;
			}
			struct gpu_command c;
			uint32_t sub;
			memcpy(&sub, r.p, 4);
			memcpy(&c, r.p + 4, sizeof(c));
			const uint8_t *lists = r.p + 4 + sizeof(c);
			uint32_t cl = c.cmdsize * c.numcmds, ol = c.objsize * c.numobjs;
			CHECK(4 + sizeof(c) + cl + ol <= r.len);
			c.cmdlist = (uintptr_t)lists;
			c.objlist = (uintptr_t)(lists + cl);
			CHECK(c.numsyncs == 0);	/* cap1 has none; their priv pointers would not survive a replay */
			c.numsyncs = 0;
			nfl = 0;
			int64_t t0 = clock_ns;
			CHECK(kgsl_ioctl(fd, 0xc040094a, &c) == 0);
			int64_t spent = clock_ns - t0;
			struct stats s = { 0 };
			for (uint32_t i = 0; i < c.numcmds; i++) {
				struct command_object co;
				memcpy(&co, lists + i * c.cmdsize, sizeof(co));
				if (!(co.flags & 1))
					continue;
				s.ibs++;
				if (mode != KGSL_IOS_DELAY)
					CHECK(range_flushed(co.gpuaddr, co.size));	/* every mode but delay: IB1 flushed */
				walk(co.gpuaddr, co.size, 0, &s, (int)sub);
			}
			CHECK(!range_touched(IMPORT_VA, IMPORT_SIZE));		/* imports never */
			if (mode == KGSL_IOS_FLUSH_ALL) {
				for (unsigned i = 0; i < ncobj; i++)
					if (cobjs[i].live && !cobjs[i].import)
						CHECK(range_flushed(cobjs[i].va, cobjs[i].size));
				for (unsigned i = 0; i < nfl; i++) {	/* and nothing else: only live objects */
					struct cobj *o = cobj_va(fl[i].a);
					CHECK(o && !o->import);
				}
				CHECK(s.refs_out == 0 && s.ib2_out == 0);
			}
			if (mode == KGSL_IOS_DELAY) {
				CHECK(nfl == 0);
				if (spent < 2000000)
					(*delay_bad)++;
			}
			if (!quiet)
				printf("  submit %u: %u IB1, %u IB2, %u draw-state groups, %u refs (%u global), %u not flushed, "
				       "%u IB2/groups not flushed, %u flush ranges, %.2f ms before the kick\n",
				       sub, s.ibs, s.ib2s, s.groups, s.refs, s.global_refs, s.refs_out, s.ib2_out, nfl,
				       spent / 1e6);
			tot.ibs += s.ibs, tot.ib2s += s.ib2s, tot.groups += s.groups, tot.refs += s.refs;
			tot.refs_out += s.refs_out, tot.global_refs += s.global_refs, tot.unowned += s.unowned;
			tot.ib2_out += s.ib2_out, tot.walk_stops += s.walk_stops;
			nsub++;
		}
	}
	CHECK(submits_seen == nsub);
	CHECK(fl_overflow == 0);
	CHECK(kgsl_close(fd) == 1);
	*nsub_out = nsub;
	return tot;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s CAPTURE.bin\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	caplen = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	cap = malloc(caplen);
	if (fread(cap, 1, caplen, f) != caplen)
		return 2;
	fclose(f);

	static const struct { int self, mode; const char *name; } modes[] = {
		{ DRG_MODE_FLUSHALL, KGSL_IOS_FLUSH_ALL, "flushall (mode 16)" },
		{ 0, KGSL_IOS_FLUSH_IBS, "IB1 only (r23, mode 4)" },
		{ DRG_MODE_DELAY, KGSL_IOS_DELAY, "delay (mode 32)" },
	};
	struct stats st[3];
	int nsub[3];
	unsigned dbad[3];
	for (int m = 0; m < 3; m++) {
		printf("mode %s:\n", modes[m].name);
		quiet = 0;
		st[m] = replay(modes[m].self, modes[m].mode, &nsub[m], &dbad[m]);
		printf("  => %d submits, %u IB1, %u IB2, %u groups, %u refs (%u global, %u in no captured object), "
		       "%u refs and %u IB2/groups outside the flushed set, %u walk stops\n",
		       nsub[m], st[m].ibs, st[m].ib2s, st[m].groups, st[m].refs, st[m].global_refs, st[m].unowned,
		       st[m].refs_out, st[m].ib2_out, st[m].walk_stops);
	}
	CHECK(nsub[0] > 0 && st[0].refs > 0);
	CHECK(st[0].unowned == 0);			/* every ref is in a captured object, so the check is complete */
	CHECK(st[0].refs_out == 0 && st[0].ib2_out == 0);	/* flushall covers everything */
	CHECK(st[1].refs_out > 0);			/* r23's IB1-only flush misses refs: the check has teeth */
	CHECK(dbad[2] == 0);				/* delay: >= 2 ms before every submit */

	printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
	return fails != 0;
}
