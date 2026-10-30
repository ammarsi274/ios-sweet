/*
 * KGSL layer for the bench: the real /dev/kgsl-3d0 of TWRP's kernel.
 * Every ioctl is checked against what Nokia's driver was measured to use;
 * anything else is reported (and still passed on, so it can be measured).
 */
#include "kgsl.h"
#include "host.h"
#include "rt.h"

#define NFD 8
static int fds[NFD];
static unsigned long seen_req[64];
static unsigned seen_count[64];
static int nseen;

/* (type 0x09) nr -> name, from HMD's msm_kgsl.h */
static const struct {
	unsigned nr;
	const char *name;
} known[] = {
	{ 0x02, "DEVICE_GETPROPERTY" }, { 0x13, "DRAWCTXT_CREATE" }, { 0x14, "DRAWCTXT_DESTROY" },
	{ 0x40, "SYNCSOURCE_CREATE" }, { 0x41, "SYNCSOURCE_DESTROY" }, { 0x45, "GPUOBJ_ALLOC" },
	{ 0x46, "GPUOBJ_FREE" }, { 0x4a, "GPU_COMMAND" },
};

int kgsl_open(int flags)
{
	int fd = host_open("/dev/kgsl-3d0", flags, 0);
	if (fd >= 0)
		for (int i = 0; i < NFD; i++)
			if (!fds[i]) {
				fds[i] = fd + 1;
				break;
			}
	return fd;
}

int kgsl_is_fd(int fd)
{
	for (int i = 0; i < NFD; i++)
		if (fds[i] == fd + 1)
			return 1;
	return 0;
}

int kgsl_close(int fd)
{
	for (int i = 0; i < NFD; i++)
		if (fds[i] == fd + 1) {
			fds[i] = 0;
			host_close(fd);
			return 1;
		}
	return 0;
}

/*
 * Capture (bench flag -capture FILE): everything needed to replay the
 * Nokia driver's GPU work on our own driver core (drg/GPU-IOS-DESIGN.md I1).
 * Per GPU_COMMAND: every mapped GPU object before it (contents only when
 * changed since last written: FNV-1a hash), the command, then a wait for
 * its timestamp (WAITTIMESTAMP_CTXTID, which the driver itself never uses)
 * and every object plus the memstore after it. Structs: msm_kgsl.h (HMD).
 * Record: u32 'DRGC', u32 type, u32 payload bytes, u32 0, payload.
 */
enum { CAP_ALLOC = 1, CAP_MAP, CAP_FREE, CAP_SUBMIT, CAP_OBJ, CAP_PROP, CAP_CTX, CAP_WAIT, CAP_MEMSTORE };
struct gpuobj_alloc { uint64_t size, flags, va_len, mmapsize; uint32_t id, metadata_len; uint64_t metadata; };
struct gpuobj_free { uint64_t flags, priv; uint32_t id, type, len; };
struct gpu_command {
	uint64_t flags, cmdlist;
	uint32_t cmdsize, numcmds;
	uint64_t objlist;
	uint32_t objsize, numobjs;
	uint64_t synclist;
	uint32_t syncsize, numsyncs, context_id, timestamp;
};
struct getproperty { uint32_t type; uint32_t pad; uint64_t value; uint64_t sizebytes; };
struct drawctxt_create { uint32_t flags, drawctxt_id; };
struct waitts { uint32_t context_id, timestamp, timeout; };

#define NR(req)		((req) & 0xff)
#define IOC_WAITTS_CTXTID	0x400c0907UL	/* _IOW(0x09, 0x7, 12 bytes) */

static int cap_fd = -1;
static uint32_t cap_submits;
static struct capobj { uint32_t id, live; uint64_t size, flags, va, maplen, hash; } objs[256];
static uint64_t memstore_va, memstore_len;

static void cap_write(uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen)
{
	uint32_t h[4] = { 0x43475244, type, alen + blen, 0 };	/* "DRGC" */
	if (cap_fd < 0)
		return;
	host_write(cap_fd, h, sizeof(h));
	if (alen)
		host_write(cap_fd, a, alen);
	if (blen)
		host_write(cap_fd, b, blen);
}

static uint64_t fnv(const uint8_t *p, uint64_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (uint64_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

static struct capobj *obj_of(uint32_t id)
{
	for (int i = 0; i < 256; i++)
		if (objs[i].live && objs[i].id == id)
			return &objs[i];
	return 0;
}

/* phase 0 = before submit n, 1 = after it completed */
static void cap_objects(uint32_t phase)
{
	for (int i = 0; i < 256; i++) {
		struct capobj *o = &objs[i];
		if (!o->live || !o->va)
			continue;
		uint64_t h = fnv((const uint8_t *)(uintptr_t)o->va, o->maplen);
		uint32_t changed = h != o->hash;
		struct { uint32_t id, phase, submit, has_data; uint64_t gpuaddr, len, hash; } r = {
			o->id, phase, cap_submits, changed, o->va, o->maplen, h
		};
		cap_write(CAP_OBJ, &r, sizeof(r), changed ? (const void *)(uintptr_t)o->va : 0,
			  changed ? (uint32_t)o->maplen : 0);
		o->hash = h;
	}
	if (memstore_va && phase == 1) {
		struct { uint32_t submit, pad; uint64_t va, len; } r = { cap_submits, 0, memstore_va, memstore_len };
		cap_write(CAP_MEMSTORE, &r, sizeof(r), (const void *)(uintptr_t)memstore_va, (uint32_t)memstore_len);
	}
}

int kgsl_capture_start(const char *path)
{
	cap_fd = host_open(path, 0x241 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644);
	return cap_fd < 0 ? cap_fd : 0;
}

static void cap_submit(int fd, struct gpu_command *c)
{
	cap_objects(0);
	uint8_t lists[4096];
	uint32_t n = 0, cl = c->cmdsize * c->numcmds, ol = c->objsize * c->numobjs, sl = c->syncsize * c->numsyncs;
	if (sizeof(*c) + cl + ol + sl <= sizeof(lists)) {
		memcpy(lists, c, sizeof(*c));
		n = sizeof(*c);
		memcpy(lists + n, (void *)(uintptr_t)c->cmdlist, cl), n += cl;
		memcpy(lists + n, (void *)(uintptr_t)c->objlist, ol), n += ol;
		memcpy(lists + n, (void *)(uintptr_t)c->synclist, sl), n += sl;
	}
	cap_write(CAP_SUBMIT, &cap_submits, 4, lists, n);
	(void)fd;
}

static void cap_after_submit(int fd, struct gpu_command *c, long rc)
{
	struct waitts w = { c->context_id, c->timestamp, 2000 };
	long wr = rc ? rc : host_ioctl(fd, IOC_WAITTS_CTXTID, &w);
	struct { uint32_t submit, ctx, ts; int32_t submit_rc, wait_rc; } r = {
		cap_submits, c->context_id, c->timestamp, (int32_t)rc, (int32_t)wr
	};
	cap_write(CAP_WAIT, &r, sizeof(r), 0, 0);
	cap_objects(1);
	cap_submits++;
}

long kgsl_ioctl(int fd, unsigned long req, void *arg)
{
	int i;
	for (i = 0; i < nseen && seen_req[i] != req; i++)
		;
	if (i == nseen && nseen < 64)
		seen_req[nseen++] = req;
	if (i < 64)
		seen_count[i]++;
	if (cap_fd < 0 || ((req >> 8) & 0xff) != 0x09)
		return host_ioctl(fd, req, arg);

	if (NR(req) == 0x46) {			/* GPUOBJ_FREE */
		struct gpuobj_free *f = arg;
		struct capobj *o = obj_of(f->id);
		cap_write(CAP_FREE, &f->id, 4, 0, 0);
		if (o)
			o->live = 0;
	}
	if (NR(req) == 0x4a)			/* GPU_COMMAND */
		cap_submit(fd, arg);
	long rc = host_ioctl(fd, req, arg);
	if (NR(req) == 0x45 && rc == 0) {	/* GPUOBJ_ALLOC */
		struct gpuobj_alloc *a = arg;
		for (int k = 0; k < 256; k++)
			if (!objs[k].live) {
				objs[k] = (struct capobj){ a->id, 1, a->mmapsize, a->flags, 0, 0, 0 };
				break;
			}
		cap_write(CAP_ALLOC, a, sizeof(*a), 0, 0);
	} else if (NR(req) == 0x4a) {
		cap_after_submit(fd, arg, rc);
	} else if (NR(req) == 0x13 && rc == 0) {	/* DRAWCTXT_CREATE */
		cap_write(CAP_CTX, arg, sizeof(struct drawctxt_create), 0, 0);
	} else if (NR(req) == 0x02 && rc == 0) {	/* DEVICE_GETPROPERTY */
		struct getproperty *g = arg;
		cap_write(CAP_PROP, g, sizeof(*g), (void *)(uintptr_t)g->value, (uint32_t)g->sizebytes);
	}
	return rc;
}

void *kgsl_mmap(void *addr, size_t len, int prot, int flags, int fd, long off, int *err)
{
	void *p = host_mmap_fd(addr, len, prot, flags, fd, off, err);
	if (cap_fd >= 0 && p != (void *)-1) {
		struct { uint64_t off, va, len; uint32_t prot, flags; } r = { (uint64_t)off, (uintptr_t)p, len, (uint32_t)prot,
								    (uint32_t)flags };
		struct capobj *o = obj_of((uint32_t)((uint64_t)off >> 12));
		if ((uint64_t)off >= 0xf8000000ULL) {	/* memstore (measured: off 0xf8001000) */
			memstore_va = (uintptr_t)p;
			memstore_len = len;
			cap_write(CAP_MEMSTORE, &r, sizeof(r), 0, 0);
		} else {
			if (o && (prot & 1)) {
				o->va = (uintptr_t)p;
				o->maplen = len;
			}
			cap_write(CAP_MAP, &r, sizeof(r), 0, 0);
		}
	}
	return p;
}

int kgsl_munmap(void *addr, size_t len)
{
	return 0;	/* the bench maps through the kernel: plain munmap is right */
}

void kgsl_report(void)
{
	for (int i = 0; i < nseen; i++) {
		unsigned long r = seen_req[i];
		const char *name = "UNEXPECTED";
		for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++)
			if (((r >> 8) & 0xff) == 0x09 && (r & 0xff) == known[k].nr)
				name = known[k].name;
		host_log("kgsl ioctl %08lx %-20s x%u\n", r, name, seen_count[i]);
	}
}
