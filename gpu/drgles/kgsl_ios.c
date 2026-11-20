/*
 * KGSL layer for iOS: /dev/kgsl-3d0 as Nokia's driver uses it (the measured
 * set, drg/gpu-research/TWRP-MEASURED.md §4), on XNU's DrgGPU user client
 * (gpu/drgpu/drgpu_uc.h). Structs: HMD's msm_kgsl.h.
 *
 * Memory: with KGSL_MEMFLAGS_USE_CPU_MAP the GPU address of an object is
 * its CPU address. DrgGPU maps every object into this task when it is
 * allocated (and onto the GPU at the same address); the driver's
 * mmap(fd, off = id << 12) just returns that address, and its munmap of
 * an object is a no-op (the kernel removes the mapping when the object is
 * freed, or once its free timestamp retired). The memstore mmap
 * (off 0xf8001000) returns the kernel's read-only mapping.
 *
 * Every fd number given out is a real descriptor (/dev/null) so it never
 * collides with the process's own files.
 */
#include "kgsl.h"
#include "host.h"
#include "rt.h"
#include "../drgpu/drgpu_uc.h"

/* IOKit and Mach, as libSystem and IOKit.framework export them */
typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
void *IOServiceMatching(const char *name);
mach_port_t IOServiceGetMatchingService(mach_port_t main_port, const void *matching);
kern_return_t IOServiceOpen(mach_port_t service, mach_port_t owning_task, uint32_t type, mach_port_t *connect);
kern_return_t IOServiceClose(mach_port_t connect);
kern_return_t IOObjectRelease(mach_port_t object);
kern_return_t IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t input_cnt,
				  const void *input_struct, size_t input_struct_cnt, uint64_t *output, uint32_t *output_cnt,
				  void *output_struct, size_t *output_struct_cnt);

/* IOReturn (IOKit/IOReturn.h): iokit_common_err(x) */
#define IOR(x)			(int)(0xe0000000u | (x))
#define kIOReturnNoMemory	IOR(0x2bd)
#define kIOReturnNoResources	IOR(0x2be)
#define kIOReturnBadArgument	IOR(0x2c2)
#define kIOReturnUnsupported	IOR(0x2c7)
#define kIOReturnIOError	IOR(0x2ca)
#define kIOReturnBusy		IOR(0x2d5)
#define kIOReturnTimeout	IOR(0x2d6)
#define kIOReturnNotAttached	IOR(0x2d9)
#define kIOReturnNoSpace	IOR(0x2db)
#define kIOReturnAborted	IOR(0x2eb)
#define kIOReturnNotResponding	IOR(0x2ed)
#define kIOReturnNotFound	IOR(0x2f0)

/* Linux errno values the driver understands (bionic/bionic.h) */
#define E_INTR 4
#define E_IO 5
#define E_NOMEM 12
#define E_FAULT 14
#define E_BUSY 16
#define E_NODEV 19
#define E_INVAL 22
#define E_NOSPC 28
#define E_DEADLK 35
#define E_NOSYS 38
#define E_TIMEDOUT 110

/* msm_kgsl.h (HMD) */
#define KGSL_IOC_TYPE		0x09
#define NR(req)			((unsigned)(req) & 0xff)
#define TYPE(req)		(((unsigned)(req) >> 8) & 0xff)
#define KGSL_CMDLIST_IB		0x1u
#define KGSL_GPUOBJ_FREE_ON_EVENT 1
#define KGSL_GPU_EVENT_TIMESTAMP 1
#define KGSL_CMD_SYNCPOINT_TYPE_TIMESTAMP 0
#define KGSL_MEMSTORE_OFF	0xf8001000L	/* DEVICE_SHADOW gpuaddr, the mmap offset of the memstore */
#define LINUX_MAP_FIXED		0x10

struct getproperty { uint32_t type, pad; uint64_t value, sizebytes; };
struct drawctxt_create { uint32_t flags, drawctxt_id; };
struct drawctxt_destroy { uint32_t drawctxt_id; };
struct gpuobj_alloc { uint64_t size, flags, va_len, mmapsize; uint32_t id, metadata_len; uint64_t metadata; };
struct gpuobj_free { uint64_t flags, priv; uint32_t id, type, len; };
struct gpu_event_timestamp { uint32_t context_id, timestamp; };
struct gpu_command {
	uint64_t flags, cmdlist;
	uint32_t cmdsize, numcmds;
	uint64_t objlist;
	uint32_t objsize, numobjs;
	uint64_t synclist;
	uint32_t syncsize, numsyncs, context_id, timestamp;
};
struct command_object { uint64_t offset, gpuaddr, size; uint32_t flags, id; };
struct command_syncpoint { uint64_t priv, size; uint32_t type; };
struct syncpoint_timestamp { uint32_t context_id, timestamp; };
struct syncsource { uint32_t id; uint32_t reserved[3]; };

/*
 * Objects by id: the address the kernel mapped them at. Freed objects stay
 * until the driver's munmap, so that munmap is never passed to the host:
 * the range may still be the kernel's mapping (a free on a timestamp).
 */
struct kobj {
	uint32_t id;
	uint8_t live, freed;
	uint64_t va, size;
	uint64_t flags;		/* as GPUOBJ_INFO reports them */
	uint32_t imports;	/* an imported IOSurface: imports still holding it */
	uint32_t surface;	/* its IOSurface id */
};

#define NDEV 4
#define NLAST 16
static struct kdev {
	int used;
	int fd;			/* placeholder descriptor */
	mach_port_t conn;
	uint64_t memstore;
	struct kobj *objs;
	uint32_t nobjs, cap;
	struct { uint32_t ctx, ts, used; } last[NLAST];	/* each context's last submitted timestamp */
} devs[NDEV];

static unsigned long seen_req[64];
static unsigned seen_count[64];
static int nseen;
static int skipped_preamble_logged;

/*
 * Boot #42 isolation switches (bench -dsb / -deferfree). #41: without the
 * capture's pauses every run hit CP_OPCODE_ERROR (GPU work identical to
 * Linux when it passes). -dsb: a full barrier right before each submit, so
 * the blob's command writes are in memory before the kernel kicks the GPU.
 * -deferfree: free-on-event frees are kept here and done (immediately, in
 * DrgGPU) only once the memstore shows their timestamp retired.
 */
static int opt_dsb, opt_deferfree;
#define NDEFER 256
static struct { struct kdev *d; uint32_t id, ctx, ts; } defer[NDEFER];
static unsigned ndefer;

void kgsl_ios_options(int dsb, int deferfree)
{
	opt_dsb = dsb;
	opt_deferfree = deferfree;
}

/* DrgGPU submission framing for connections opened from now on (DRGPU_UC_OPT_*) */
static uint32_t opt_framing;
void kgsl_ios_framing(uint32_t opts)
{
	opt_framing = opts;
}

/*
 * CPU read-back before each submit (boot #51: every framing still faults on
 * back-to-back work; #40-#42: a capture, which reads every object before
 * each submit, passes even without waiting, a barrier does not). 1: the
 * command objects and the object list of the submit; 2: every live object.
 */
static int opt_readback;
void kgsl_ios_readback(int mode)
{
	opt_readback = mode;
}

/*
 * Cache maintenance before each submit (research/reports/gpu-fault-2.md §4A;
 * the self-test picks one, bench -selftest):
 *   KGSL_IOS_FLUSH_IBS    clean+invalidate each IB1's lines (r23, boot #51 fix)
 *   KGSL_IOS_FLUSH_ALL    that, plus every live object that is not an import
 *                         (IB2s and state the blob wrote outside IB1: H1)
 *   KGSL_IOS_DELAY        no cache maintenance; spin 2 ms before the kick (H2)
 * All end with dsb sy. Imported IOSurfaces are not touched before a submit
 * (they can be big render targets): they get one clean+invalidate when they
 * are imported and one after the GPU wrote them (kgsl_ios_cpu_sync).
 * Default since root r25: FLUSH_ALL (boot #55: the only mode without a GPU
 * fault, 200 back-to-back frames OK, drg/BOOTLOG.md).
 */
static int opt_flush = KGSL_IOS_FLUSH_ALL;
void kgsl_ios_flush(int mode)
{
	opt_flush = mode;
}

int kgsl_ios_flush_mode(void)
{
	return opt_flush;
}

#define DELAY_NS 2000000

#if defined(KGSL_IOS_TEST_FLUSH)	/* host test: record the ranges instead (test/kgsl_flush_test.c) */
void kgsl_test_flush(uint64_t va, uint64_t len);
static void flush_range(uint64_t va, uint64_t len)
{
	kgsl_test_flush(va, len);
}
#elif defined(__aarch64__)
static void flush_range(uint64_t va, uint64_t len)
{
	for (uint64_t a = va & ~63ull; a < va + len; a += 64)	/* Kryo 260: 64-byte lines */
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
}
#else
static void flush_range(uint64_t va, uint64_t len)
{
	(void)va;
	(void)len;
}
#endif

static void dsb_sy(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#endif
}

static void spin_ns(int64_t ns)
{
	struct host_ts t;
	if (host_clock(1, &t))
		return;
	int64_t end = t.sec * 1000000000 + t.nsec + ns;
	do {
		if (host_clock(1, &t))
			return;
	} while (t.sec * 1000000000 + t.nsec < end);
}

static uint64_t read_range(uint64_t va, uint64_t len)
{
	uint64_t s = 0;
	for (const volatile uint64_t *p = (const volatile uint64_t *)(uintptr_t)va; len >= 8; len -= 8)
		s += *p++;
	return s;
}

/* a submission was refused because a GPU fault invalidated its context */
static volatile int gpu_faulted;
int kgsl_ios_faulted(void)
{
	return gpu_faulted;
}

/*
 * IOSurfaces as dma-bufs: the window layer hands EGL a gralloc handle whose
 * first fd is a token from kgsl_ios_surface_fd; the driver's GPUOBJ_IMPORT
 * (type 3, measured on TWRP) of that fd maps the surface through DrgGPU's
 * SURFACE_MAP. One kernel object per surface at a time: XNU resets a page's
 * cache mode when any device-pager mapping of it goes away
 * (drg/GPU-DRAW-PLAN.md), so repeated imports share it, counted.
 */
#define NSURF 64
static struct { int fd; uint32_t surface; } surfs[NSURF];

int kgsl_ios_surface_fd(uint32_t surface)
{
	for (int i = 0; i < NSURF; i++)
		if (surfs[i].fd > 0 && surfs[i].surface == surface)
			return surfs[i].fd;
	for (int i = 0; i < NSURF; i++)
		if (surfs[i].fd <= 0) {
			int fd = host_open("/dev/null", 0x80002 /* O_RDWR | O_CLOEXEC */, 0);
			if (fd < 0)
				return fd;
			surfs[i].fd = fd;
			surfs[i].surface = surface;
			return fd;
		}
	return -E_NOMEM;
}

static int surface_of_fd(int fd, uint32_t *surface)
{
	for (int i = 0; i < NSURF; i++)
		if (fd > 0 && surfs[i].fd == fd) {
			*surface = surfs[i].surface;
			return 1;
		}
	return 0;
}

static int err_of(int rc)
{
	switch (rc) {
	case 0: return 0;
	case kIOReturnNoMemory: return -E_NOMEM;
	case kIOReturnNoResources: return -E_NOMEM;
	case kIOReturnNoSpace: return -E_NOSPC;
	case kIOReturnBadArgument: return -E_INVAL;
	case kIOReturnUnsupported: return -E_INVAL;	/* KGSL: EINVAL for unknown properties */
	case kIOReturnIOError: return -E_DEADLK;	/* KGSL: the context was invalidated by a GPU fault */
	case kIOReturnTimeout: return -E_TIMEDOUT;
	case kIOReturnAborted: return -E_INTR;
	case kIOReturnBusy: return -E_BUSY;
	case kIOReturnNotAttached: return -E_NODEV;
	case kIOReturnNotResponding: return -E_IO;
	case kIOReturnNotFound: return -E_INVAL;
	default: return -E_IO;
	}
}

static int call(struct kdev *d, uint32_t sel, const uint64_t *in, uint32_t nin, const void *sin, size_t nsin,
		uint64_t *out, uint32_t nout, void *sout, size_t nsout)
{
	uint32_t on = nout;
	size_t son = nsout;
	return err_of(IOConnectCallMethod(d->conn, sel, in, nin, sin, nsin, out, out ? &on : 0, sout, sout ? &son : 0));
}

static struct kdev *dev_of(int fd)
{
	for (int i = 0; i < NDEV; i++)
		if (devs[i].used && devs[i].fd == fd)
			return &devs[i];
	return 0;
}

int kgsl_open(int flags)
{
	struct kdev *d = 0;
	for (int i = 0; i < NDEV && !d; i++)
		if (!devs[i].used)
			d = &devs[i];
	if (!d)
		return -E_NOMEM;
	/* launchd may start us before DrgGPU registered: wait up to 30 s */
	mach_port_t svc = 0;
	for (int i = 0; i < 150 && !svc; i++) {
		svc = IOServiceGetMatchingService(0, IOServiceMatching(DRGPU_UC_SERVICE));
		if (!svc)
			host_sleep_ns(200000000);
	}
	if (!svc) {
		host_log("drgles: no %s service (GPU driver off or not started)\n", DRGPU_UC_SERVICE);
		return -E_NODEV;
	}
	mach_port_t conn = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self_, 0, &conn);
	IOObjectRelease(svc);
	if (kr) {
		host_log("drgles: %s open failed 0x%x\n", DRGPU_UC_SERVICE, kr);
		return err_of(kr);
	}
	uint64_t ver = 0;
	d->conn = conn;
	if (call(d, DRGPU_UC_GET_VERSION, 0, 0, 0, 0, &ver, 1, 0, 0) || ver != DRGPU_UC_VERSION) {
		host_log("drgles: %s ABI %llu, want %d\n", DRGPU_UC_SERVICE, (unsigned long long)ver, DRGPU_UC_VERSION);
		IOServiceClose(conn);
		return -E_NODEV;
	}
	if (opt_framing) {
		uint64_t in[1] = { opt_framing }, got[1] = { 0 };
		int rc = call(d, DRGPU_UC_SET_OPTIONS, in, 1, 0, 0, got, 1, 0, 0);
		host_log("drgles: submission framing 0x%x -> rc %d, in effect 0x%llx\n", opt_framing, rc,
			 (unsigned long long)got[0]);
		if (rc) {
			IOServiceClose(conn);
			return rc;
		}
	}
	int fd = host_open("/dev/null", (flags & 0x80000) | 2 /* O_RDWR, O_CLOEXEC as asked */, 0);
	if (fd < 0) {
		IOServiceClose(conn);
		return fd;
	}
	d->used = 1;
	d->fd = fd;
	d->memstore = 0;
	d->objs = 0;
	d->nobjs = d->cap = 0;
	memset(d->last, 0, sizeof(d->last));	/* a reused slot: no context of the old connection */
	return fd;
}

int kgsl_is_fd(int fd)
{
	return dev_of(fd) != 0;
}

int kgsl_close(int fd)
{
	struct kdev *d = dev_of(fd);
	if (!d)
		return 0;
	IOServiceClose(d->conn);	/* the kernel waits for this process's work, then frees all of it */
	host_close(fd);
	host_free(d->objs);
	d->used = 0;
	return 1;
}

static struct kobj *obj_find(struct kdev *d, uint32_t id)
{
	for (uint32_t i = 0; i < d->nobjs; i++)
		if (d->objs[i].live && d->objs[i].id == id)
			return &d->objs[i];
	return 0;
}

static struct kobj *obj_slot(struct kdev *d)
{
	for (uint32_t i = 0; i < d->nobjs; i++)
		if (!d->objs[i].live && !d->objs[i].freed)
			return &d->objs[i];
	if (d->nobjs == d->cap) {
		uint32_t cap = d->cap ? d->cap * 2 : 64;
		struct kobj *n = host_alloc(cap * sizeof(*n));
		if (!n)
			return 0;
		if (d->objs)
			memcpy(n, d->objs, d->nobjs * sizeof(*n));
		host_free(d->objs);
		d->objs = n;
		d->cap = cap;
	}
	return &d->objs[d->nobjs++];
}

static long gpuobj_alloc(struct kdev *d, struct gpuobj_alloc *a)
{
	uint64_t in[2] = { a->size, a->flags }, out[3] = { 0 };
	struct kobj *o = obj_slot(d);
	if (!o)
		return -E_NOMEM;
	int rc = call(d, DRGPU_UC_OBJ_ALLOC, in, 2, 0, 0, out, 3, 0, 0);
	if (rc)
		return rc;
	*o = (struct kobj){ (uint32_t)out[0], 1, 0, out[1], out[2], a->flags, 0, 0 };
	a->id = (uint32_t)out[0];
	a->size = out[2];
	a->mmapsize = out[2];
	a->va_len = out[2];
	return 0;
}

#define KGSL_USER_MEM_TYPE_DMABUF	3
#define KGSL_MEMFLAGS_USE_CPU_MAP	0x10000000ull
#define KGSL_MEMALIGN_MASK		0x00ff0000ull
#define KGSL_MEMFLAGS_USERMEM_MASK	0x000000e0ull
#define KGSL_MEMFLAGS_USERMEM_ION	0x00000080ull	/* (KGSL_USER_MEM_TYPE_ION + 1) << 5 */
struct gpuobj_import { uint64_t priv, priv_len, flags; uint32_t type, id; };
struct gpuobj_info { uint64_t gpuaddr, flags, size, va_len, va_addr; uint32_t id; };

/*
 * TWRP, Linux's answer for a gralloc buffer: flags in 0x10001200 -> out
 * 0x141280 (USE_CPU_MAP dropped, the ION user-memory type set, alignment
 * 2^20 for its own GPU address). Here the GPU address is the task address
 * of the import (USE_CPU_MAP as for every object): its real alignment.
 */
static long gpuobj_import(struct kdev *d, struct gpuobj_import *im)
{
	uint32_t surface = 0;
	int fd = -1;
	if (im->type != KGSL_USER_MEM_TYPE_DMABUF || im->priv_len < sizeof(int) || !im->priv)
		return -E_INVAL;
	memcpy(&fd, (const void *)(uintptr_t)im->priv, sizeof(fd));
	if (!surface_of_fd(fd, &surface)) {
		host_log("drgles: GPUOBJ_IMPORT of fd %d: not a surface token\n", fd);
		return -E_INVAL;
	}
	for (uint32_t i = 0; i < d->nobjs; i++) {
		struct kobj *o = &d->objs[i];
		if (o->live && o->imports && o->surface == surface) {	/* already imported: share it */
			o->imports++;
			im->id = o->id;
			im->flags = o->flags;
			return 0;
		}
	}
	struct kobj *o = obj_slot(d);
	uint64_t in[1] = { surface }, out[3] = { 0 };
	if (!o)
		return -E_NOMEM;
	int rc = call(d, DRGPU_UC_SURFACE_MAP, in, 1, 0, 0, out, 3, 0, 0);
	if (rc) {
		host_log("drgles: SURFACE_MAP of IOSurface %u: rc %d\n", surface, rc);
		return rc;
	}
	unsigned align = 12;
	while (align < 20 && !(out[1] & (1ull << align)))
		align++;
	uint64_t flags = (im->flags & ~(KGSL_MEMFLAGS_USE_CPU_MAP | KGSL_MEMALIGN_MASK | KGSL_MEMFLAGS_USERMEM_MASK)) |
			 KGSL_MEMFLAGS_USERMEM_ION | (uint64_t)align << 16;
	*o = (struct kobj){ (uint32_t)out[0], 1, 0, out[1], out[2], flags, 1, surface };
	/*
	 * Before the GPU first touches it: clean+invalidate its lines (dirty lines
	 * from whoever wrote it through a cached alias, e.g. the kernel zeroing a
	 * new surface, would otherwise be written back over what the GPU draws).
	 * DrgGPU's own flush at SURFACE_MAP is not enough for a big surface: XNU's
	 * dcache_incoherent_io_flush64 (osfmk/arm/caches.c 153) does a set/way
	 * flush of the calling core only once the length reaches the L2 size
	 * (osfmk/arm/cpuid.c 367); dc civac by VA is broadcast to every core.
	 */
	flush_range(o->va, o->size);
	dsb_sy();
	im->id = o->id;
	im->flags = flags;
	host_log("drgles: IOSurface %u imported: object %u at 0x%llx, 0x%llx bytes, flags 0x%llx\n", surface, o->id,
		 (unsigned long long)o->va, (unsigned long long)o->size, (unsigned long long)flags);
	return 0;
}

static long gpuobj_info(struct kdev *d, struct gpuobj_info *gi)
{
	struct kobj *o = obj_find(d, gi->id);
	if (!o)
		return -E_INVAL;
	gi->gpuaddr = o->va;
	gi->flags = o->flags;
	gi->size = o->size;
	gi->va_len = o->size;
	gi->va_addr = o->imports ? 0 : o->va;	/* Linux: 0 for the import (not mapped by the driver) */
	return 0;
}

/* the memstore's eop of ctx (kgsl_devmemstore: 40-byte entries, eop at +8) has reached ts */
static int ms_retired(struct kdev *d, uint32_t ctx, uint32_t ts)
{
	if (!d->memstore) {
		uint64_t out[2] = { 0 };
		if (call(d, DRGPU_UC_MEMSTORE, 0, 0, 0, 0, out, 2, 0, 0))
			return 0;
		d->memstore = out[0];
	}
	volatile const uint32_t *ms = (volatile const uint32_t *)(uintptr_t)d->memstore;
	return (int32_t)(ms[ctx * 10 + 2] - ts) >= 0;
}

static void defer_reap(void)
{
	for (unsigned i = 0; i < ndefer;) {
		if (ms_retired(defer[i].d, defer[i].ctx, defer[i].ts)) {
			uint64_t in[4] = { defer[i].id, 0, 0, 0 };
			(void)call(defer[i].d, DRGPU_UC_OBJ_FREE, in, 4, 0, 0, 0, 0, 0, 0);
			defer[i] = defer[--ndefer];
		} else {
			i++;
		}
	}
}

static long gpuobj_free(struct kdev *d, struct gpuobj_free *f)
{
	struct kobj *o = obj_find(d, f->id);
	uint64_t in[4] = { f->id, 0, 0, 0 };
	if (!o)
		return -E_INVAL;
	if (o->imports > 1) {	/* another import of the same surface still uses it */
		o->imports--;
		return 0;
	}
	if (f->flags & KGSL_GPUOBJ_FREE_ON_EVENT) {
		struct gpu_event_timestamp ev;
		if (f->type != KGSL_GPU_EVENT_TIMESTAMP || f->len != sizeof(ev) || !f->priv)
			return -E_INVAL;
		memcpy(&ev, (const void *)(uintptr_t)f->priv, sizeof(ev));
		if (opt_deferfree && ndefer < NDEFER) {
			defer[ndefer].d = d;
			defer[ndefer].id = f->id;
			defer[ndefer].ctx = ev.context_id;
			defer[ndefer].ts = ev.timestamp;
			ndefer++;
			o->live = 0;
			o->freed = 1;
			return 0;
		}
		in[1] = 1;
		in[2] = ev.context_id;
		in[3] = ev.timestamp;
	}
	int rc = call(d, DRGPU_UC_OBJ_FREE, in, 4, 0, 0, 0, 0, 0, 0);
	if (rc)
		return rc;
	o->live = 0;
	o->freed = 1;	/* until the driver's munmap of it */
	return 0;
}

/*
 * GPUOBJ_SYNC (0x49): the driver's own cache maintenance requests. Boot #55
 * (logs/boot55/drgles-selftest.log): Nokia's driver called it 18 times in the
 * flushall run (16 in the 200-frame test, 2 around the sampled IOSurface) and
 * got EINVAL (not in the measured set). HMD kgsl.c kgsl_ioctl_gpuobj_sync:
 * 1..128 entries, entries copied with min(obj_len, 24) bytes at a stride of
 * 24, unknown ids skipped, op must have CLEAN and/or INV, without CACHE_RANGE
 * the whole object; Linux does nothing for its write-combined objects. Here:
 * dc civac over the range for every op (EL0 has no invalidate-only dc), the
 * range clipped to the object, so a call never fails on a found object.
 */
#define KGSL_GPUMEM_CACHE_CLEAN	0x1u
#define KGSL_GPUMEM_CACHE_INV	0x2u
#define KGSL_GPUMEM_CACHE_RANGE	0x80000000u
struct gpuobj_sync { uint64_t objs; uint32_t obj_len, count; };
struct gpuobj_sync_obj { uint64_t offset, length; uint32_t id, op; };
static unsigned sync_ioctls, sync_logged;

static long gpuobj_sync(struct kdev *d, struct gpuobj_sync *s)
{
	if (!s->count || s->count > 128 || !s->objs)
		return -E_INVAL;
	for (uint32_t i = 0; i < s->count; i++) {
		struct gpuobj_sync_obj ob;
		memset(&ob, 0, sizeof(ob));
		memcpy(&ob, (const uint8_t *)(uintptr_t)s->objs + (uint64_t)i * sizeof(ob),
		       s->obj_len < sizeof(ob) ? s->obj_len : sizeof(ob));
		struct kobj *o = obj_find(d, ob.id);
		if (!o)
			continue;
		if (!(ob.op & (KGSL_GPUMEM_CACHE_CLEAN | KGSL_GPUMEM_CACHE_INV)))
			return -E_INVAL;
		uint64_t off = 0, len = o->size;
		if (ob.op & KGSL_GPUMEM_CACHE_RANGE) {
			off = ob.offset < o->size ? ob.offset : o->size;
			len = ob.length < o->size - off ? ob.length : o->size - off;
		}
		if (sync_logged < 4) {
			sync_logged++;
			host_log("drgles: GPUOBJ_SYNC object %u%s op 0x%x offset 0x%llx length 0x%llx (obj_len %u, count %u)\n",
				 ob.id, o->imports ? " (import)" : "", ob.op, (unsigned long long)off,
				 (unsigned long long)len, s->obj_len, s->count);
		}
		if (len)
			flush_range(o->va + off, len);
	}
	dsb_sy();
	sync_ioctls++;
	return 0;
}

/* each context's last submitted timestamp (for kgsl_ios_cpu_sync) */
static void last_set(struct kdev *d, uint32_t ctx, uint32_t ts)
{
	int free_slot = -1;
	for (int i = 0; i < NLAST; i++) {
		if (d->last[i].used && d->last[i].ctx == ctx) {
			d->last[i].ts = ts;
			return;
		}
		if (!d->last[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {	/* more contexts than slots: forget the oldest entry's context (logged) */
		host_log("drgles: more than %d contexts submitting; context %u not tracked for CPU sync\n", NLAST,
			 d->last[0].ctx);
		free_slot = 0;
	}
	d->last[free_slot].ctx = ctx;
	d->last[free_slot].ts = ts;
	d->last[free_slot].used = 1;
}

static void last_drop(struct kdev *d, uint32_t ctx)
{
	for (int i = 0; i < NLAST; i++)
		if (d->last[i].used && d->last[i].ctx == ctx)
			d->last[i].used = 0;
}

/* surfaces the GPU writes (render targets), by IOSurface id */
static uint32_t gpu_written[NSURF];
static unsigned ngpu_written;
static uint64_t cs_calls, cs_bytes, cs_ns;

void kgsl_ios_surface_gpu_written(uint32_t surface)
{
	for (unsigned i = 0; i < ngpu_written; i++)
		if (gpu_written[i] == surface)
			return;
	if (ngpu_written < NSURF)
		gpu_written[ngpu_written++] = surface;
	else
		host_log("drgles: more than %d GPU-written surfaces; %u not synced for the CPU\n", NSURF, surface);
}

static int marked(uint32_t surface)
{
	for (unsigned i = 0; i < ngpu_written; i++)
		if (gpu_written[i] == surface)
			return 1;
	return 0;
}

static int64_t now_ns(void)
{
	struct host_ts t;
	return host_clock(1, &t) ? 0 : t.sec * 1000000000 + t.nsec;
}

int kgsl_ios_cpu_sync(void)
{
	int err = 0, n = 0;
	for (int k = 0; k < NDEV; k++) {	/* 1. the GPU is done with everything submitted so far */
		struct kdev *d = &devs[k];
		if (!d->used)
			continue;
		for (int i = 0; i < NLAST; i++) {
			if (!d->last[i].used)
				continue;
			uint64_t in[3] = { d->last[i].ctx, d->last[i].ts, 2000 };
			int rc = call(d, DRGPU_UC_WAIT, in, 3, 0, 0, 0, 0, 0, 0);
			if (rc && !err) {
				err = rc;
				host_log("drgles: CPU sync: wait for context %u timestamp %u: %d\n", d->last[i].ctx,
					 d->last[i].ts, rc);
			}
		}
	}
	int64_t t0 = now_ns();
	uint64_t bytes = 0;
	for (int k = 0; k < NDEV; k++) {	/* 2. no stale line of a GPU-written surface stays in any CPU cache */
		struct kdev *d = &devs[k];
		if (!d->used)
			continue;
		for (uint32_t i = 0; i < d->nobjs; i++) {
			struct kobj *o = &d->objs[i];
			if (o->live && o->imports && marked(o->surface)) {
				flush_range(o->va, o->size);
				bytes += o->size;
				n++;
			}
		}
	}
	dsb_sy();
	cs_calls++;
	cs_bytes += bytes;
	cs_ns += now_ns() - t0;
	return err ? err : n;
}

const void *kgsl_ios_surface_va(uint32_t surface, uint64_t *size)
{
	for (int k = 0; k < NDEV; k++) {
		struct kdev *d = &devs[k];
		if (!d->used)
			continue;
		for (uint32_t i = 0; i < d->nobjs; i++) {
			struct kobj *o = &d->objs[i];
			if (o->live && o->imports && o->surface == surface) {
				*size = o->size;
				return (const void *)(uintptr_t)o->va;
			}
		}
	}
	*size = 0;
	return 0;
}

void kgsl_ios_sync_stats(uint64_t *calls, uint64_t *bytes, uint64_t *ns)
{
	*calls = cs_calls;
	*bytes = cs_bytes;
	*ns = cs_ns;
}

static long gpu_command(struct kdev *d, struct gpu_command *c)
{
	struct drgpu_uc_ib ibs[DRGPU_UC_MAX_IBS];
	unsigned n = 0;
	for (uint32_t i = 0; i < c->numsyncs; i++) {	/* wait first, as KGSL's syncpoints do */
		struct command_syncpoint sp;
		struct syncpoint_timestamp ts;
		memcpy(&sp, (const uint8_t *)(uintptr_t)c->synclist + (uint64_t)i * sizeof(sp),
		       c->syncsize < sizeof(sp) ? c->syncsize : sizeof(sp));
		if (sp.type != KGSL_CMD_SYNCPOINT_TYPE_TIMESTAMP || sp.size != sizeof(ts) || !sp.priv) {
			host_log("drgles: GPU_COMMAND syncpoint type %u not supported\n", sp.type);
			return -E_INVAL;
		}
		memcpy(&ts, (const void *)(uintptr_t)sp.priv, sizeof(ts));
		uint64_t in[3] = { ts.context_id, ts.timestamp, 10000 };
		int rc = call(d, DRGPU_UC_WAIT, in, 3, 0, 0, 0, 0, 0, 0);
		if (rc)
			return rc;
	}
	for (uint32_t i = 0; i < c->numcmds; i++) {
		struct command_object co;
		memcpy(&co, (const uint8_t *)(uintptr_t)c->cmdlist + (uint64_t)i * sizeof(co),
		       c->cmdsize < sizeof(co) ? c->cmdsize : sizeof(co));
		if (!(co.flags & KGSL_CMDLIST_IB)) {	/* preambles: not in the measured set */
			if (!skipped_preamble_logged++)
				host_log("drgles: GPU_COMMAND entry flags 0x%x skipped\n", co.flags);
			continue;
		}
		if (n == DRGPU_UC_MAX_IBS)
			return -E_INVAL;
		ibs[n].gpuaddr = co.gpuaddr;	/* as adreno_ringbuffer: the IB's own address */
		ibs[n].bytes = co.size;
		n++;
	}
	if (opt_readback) {
		static volatile uint64_t sink __attribute__((used));
		uint64_t s = 0;
		for (uint32_t i = 0; i < d->nobjs; i++) {
			struct kobj *o = &d->objs[i];
			if (!o->live)
				continue;
			int want = opt_readback >= 2;
			for (unsigned k = 0; k < n && !want; k++)
				want = ibs[k].gpuaddr >= o->va && ibs[k].gpuaddr < o->va + o->size;
			for (uint32_t k = 0; k < c->numobjs && !want; k++) {
				struct command_object ob;
				memcpy(&ob, (const uint8_t *)(uintptr_t)c->objlist + (uint64_t)k * sizeof(ob),
				       c->objsize < sizeof(ob) ? c->objsize : sizeof(ob));
				want = ob.gpuaddr >= o->va && ob.gpuaddr < o->va + o->size;
			}
			if (want)
				s += read_range(o->va, o->size);
		}
		sink = s;	/* keeps the reads */
		(void)sink;
	}
	uint64_t in[3] = { c->context_id, c->flags, c->timestamp }, out[1] = { 0 };
	/*
	 * Boot #51 (research/reports/gpu-fault.md): the GPU read the newest lines
	 * of each IB as zeros. Clean and invalidate every IB's lines to the point
	 * of coherency (what iOS's sys_dcache_flush does at EL0) and wait for it,
	 * in case any alias of them is cacheable; the kernel then drains every
	 * core's write buffer and checks the IB in RAM before it kicks the GPU.
	 * FLUSH_ALL also covers every live non-import object (IB2s, state), DELAY
	 * none of it (see kgsl_ios_flush).
	 */
	if (opt_flush != KGSL_IOS_DELAY) {
		for (unsigned k = 0; k < n; k++)
			flush_range(ibs[k].gpuaddr, ibs[k].bytes);
	}
	if (opt_flush == KGSL_IOS_FLUSH_ALL) {
		for (uint32_t i = 0; i < d->nobjs; i++) {
			struct kobj *o = &d->objs[i];
			if (o->live && !o->imports)
				flush_range(o->va, o->size);
		}
	}
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	if (opt_flush == KGSL_IOS_DELAY)
		spin_ns(DELAY_NS);
	(void)opt_dsb;
	int rc = call(d, DRGPU_UC_SUBMIT, in, 3, n ? ibs : 0, n * sizeof(ibs[0]), out, 1, 0, 0);
	if (rc == -E_DEADLK && !gpu_faulted) {
		gpu_faulted = 1;
		host_log("drgles: context %u invalidated by a GPU fault\n", c->context_id);
	}
	if (rc)
		return rc;
	c->timestamp = (uint32_t)out[0];
	last_set(d, c->context_id, c->timestamp);
	return 0;
}

static long ioctl_(int fd, unsigned long req, void *arg)
{
	struct kdev *d = dev_of(fd);
	int i;
	for (i = 0; i < nseen && seen_req[i] != req; i++)
		;
	if (i == nseen && nseen < 64)
		seen_req[nseen++] = req;
	if (i < 64)
		seen_count[i]++;
	if (!d || TYPE(req) != KGSL_IOC_TYPE || !arg)
		return -E_INVAL;
	switch (NR(req)) {
	case 0x02: {	/* DEVICE_GETPROPERTY */
		struct getproperty *g = arg;
		uint64_t in[1] = { g->type };
		if (!g->value)
			return -E_FAULT;
		return call(d, DRGPU_UC_GETPROPERTY, in, 1, 0, 0, 0, 0, (void *)(uintptr_t)g->value, g->sizebytes);
	}
	case 0x13: {	/* DRAWCTXT_CREATE */
		struct drawctxt_create *c = arg;
		uint64_t in[1] = { c->flags }, out[1] = { 0 };
		int rc = call(d, DRGPU_UC_CTX_CREATE, in, 1, 0, 0, out, 1, 0, 0);
		if (!rc)
			c->drawctxt_id = (uint32_t)out[0];
		return rc;
	}
	case 0x14: {	/* DRAWCTXT_DESTROY */
		uint64_t in[1] = { ((struct drawctxt_destroy *)arg)->drawctxt_id };
		int rc = call(d, DRGPU_UC_CTX_DESTROY, in, 1, 0, 0, 0, 0, 0, 0);
		if (!rc)
			last_drop(d, (uint32_t)in[0]);	/* DrgGPU waited for its work */
		return rc;
	}
	case 0x40: {	/* SYNCSOURCE_CREATE */
		uint64_t out[1] = { 0 };
		int rc = call(d, DRGPU_UC_SYNCSRC_CREATE, 0, 0, 0, 0, out, 1, 0, 0);
		if (!rc)
			((struct syncsource *)arg)->id = (uint32_t)out[0];
		return rc;
	}
	case 0x41: {	/* SYNCSOURCE_DESTROY */
		uint64_t in[1] = { ((struct syncsource *)arg)->id };
		return call(d, DRGPU_UC_SYNCSRC_DESTROY, in, 1, 0, 0, 0, 0, 0, 0);
	}
	case 0x07: {	/* DEVICE_WAITTIMESTAMP_CTXTID: {context, timestamp, timeout ms} (EGLImage path, TWRP) */
		const uint32_t *w = arg;
		uint64_t in[3] = { w[0], w[1], w[2] };
		return call(d, DRGPU_UC_WAIT, in, 3, 0, 0, 0, 0, 0, 0);
	}
	case 0x45:	/* GPUOBJ_ALLOC */
		return gpuobj_alloc(d, arg);
	case 0x46:	/* GPUOBJ_FREE */
		return gpuobj_free(d, arg);
	case 0x47:	/* GPUOBJ_INFO */
		return gpuobj_info(d, arg);
	case 0x48:	/* GPUOBJ_IMPORT */
		return gpuobj_import(d, arg);
	case 0x49:	/* GPUOBJ_SYNC (root r25) */
		return gpuobj_sync(d, arg);
	case 0x4a:	/* GPU_COMMAND */
		return gpu_command(d, arg);
	default:
		host_log("drgles: KGSL ioctl %08lx not in the measured set\n", req);
		return -E_INVAL;
	}
}

long kgsl_ioctl(int fd, unsigned long req, void *arg)
{
	if (ndefer)
		defer_reap();
	kgsl_cap_before(fd, req, arg);
	long rc = ioctl_(fd, req, arg);
	kgsl_cap_after(fd, req, arg, rc);
	return rc;
}

/* kgsl_cap.c: wait for a captured submit (DrgGPU WAIT, timeout in ms) */
long kgsl_cap_wait(int fd, uint32_t ctx, uint32_t ts, uint32_t timeout_ms)
{
	struct kdev *d = dev_of(fd);
	uint64_t in[3] = { ctx, ts, timeout_ms };
	return d ? call(d, DRGPU_UC_WAIT, in, 3, 0, 0, 0, 0, 0, 0) : -E_INVAL;
}

static void *mmap_(void *addr, size_t len, int prot, int flags, int fd, long off, int *err)
{
	struct kdev *d = dev_of(fd);
	*err = E_INVAL;
	if (!d)
		return (void *)-1;
	if (off == KGSL_MEMSTORE_OFF) {
		if (!d->memstore) {
			uint64_t out[2] = { 0 };
			int rc = call(d, DRGPU_UC_MEMSTORE, 0, 0, 0, 0, out, 2, 0, 0);
			if (rc) {
				*err = -rc;
				return (void *)-1;
			}
			d->memstore = out[0];
		}
		if (len > 0x2000 || (prot & 2)) {	/* read-only, as KGSL maps it */
			*err = 13;	/* EACCES */
			return (void *)-1;
		}
		*err = 0;
		return (void *)(uintptr_t)d->memstore;
	}
	struct kobj *o = off > 0 && !(off & 0xfff) ? obj_find(d, (uint32_t)(off >> 12)) : 0;
	if (!o || len > o->size || ((flags & LINUX_MAP_FIXED) && (uintptr_t)addr != o->va))
		return (void *)-1;
	*err = 0;
	return (void *)(uintptr_t)o->va;	/* USE_CPU_MAP: the CPU address is the GPU address */
}

void *kgsl_mmap(void *addr, size_t len, int prot, int flags, int fd, long off, int *err)
{
	void *p = mmap_(addr, len, prot, flags, fd, off, err);
	kgsl_cap_mmap(p, len, prot, flags, off);
	return p;
}

int kgsl_munmap(void *addr, size_t len)
{
	uint64_t a = (uintptr_t)addr;
	for (int k = 0; k < NDEV; k++) {
		struct kdev *d = &devs[k];
		if (!d->used)
			continue;
		if (d->memstore && a == d->memstore)
			return 1;
		for (uint32_t i = 0; i < d->nobjs; i++) {
			struct kobj *o = &d->objs[i];
			if ((o->live || o->freed) && a >= o->va && a + len <= o->va + o->size) {
				if (o->freed)
					o->freed = 0;	/* slot reusable */
				return 1;
			}
		}
	}
	return 0;
}

void kgsl_report(void)
{
	for (int i = 0; i < nseen; i++)
		host_log("kgsl ioctl %08lx x%u\n", seen_req[i], seen_count[i]);
	host_log("cache: flush mode %d, %u GPUOBJ_SYNC calls; CPU sync %llu calls, %llu MiB invalidated in %.2f ms\n",
		 opt_flush, sync_ioctls, (unsigned long long)cs_calls, (unsigned long long)(cs_bytes >> 20), cs_ns / 1e6);
	kgsl_submit_report();
}

