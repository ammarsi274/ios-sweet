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
};

#define NDEV 4
static struct kdev {
	int used;
	int fd;			/* placeholder descriptor */
	mach_port_t conn;
	uint64_t memstore;
	struct kobj *objs;
	uint32_t nobjs, cap;
} devs[NDEV];

static unsigned long seen_req[64];
static unsigned seen_count[64];
static int nseen;
static int skipped_preamble_logged;

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
	*o = (struct kobj){ (uint32_t)out[0], 1, 0, out[1], out[2] };
	a->id = (uint32_t)out[0];
	a->size = out[2];
	a->mmapsize = out[2];
	a->va_len = out[2];
	return 0;
}

static long gpuobj_free(struct kdev *d, struct gpuobj_free *f)
{
	struct kobj *o = obj_find(d, f->id);
	uint64_t in[4] = { f->id, 0, 0, 0 };
	if (!o)
		return -E_INVAL;
	if (f->flags & KGSL_GPUOBJ_FREE_ON_EVENT) {
		struct gpu_event_timestamp ev;
		if (f->type != KGSL_GPU_EVENT_TIMESTAMP || f->len != sizeof(ev) || !f->priv)
			return -E_INVAL;
		memcpy(&ev, (const void *)(uintptr_t)f->priv, sizeof(ev));
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

static long gpu_command(struct kdev *d, struct gpu_command *c)
{
	struct drgpu_uc_ib ibs[DRGPU_UC_MAX_IBS];
	unsigned n = 0;
	for (uint32_t i = 0; i < c->numsyncs; i++) {	/* wait first, as KGSL's syncpoints do */
		struct command_syncpoint sp;
		struct syncpoint_timestamp ts;
		memcpy(&sp, (const uint8_t *)(uintptr_t)c->synclist + (uint64_t)i * c->syncsize,
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
		memcpy(&co, (const uint8_t *)(uintptr_t)c->cmdlist + (uint64_t)i * c->cmdsize,
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
	uint64_t in[3] = { c->context_id, c->flags, c->timestamp }, out[1] = { 0 };
	int rc = call(d, DRGPU_UC_SUBMIT, in, 3, n ? ibs : 0, n * sizeof(ibs[0]), out, 1, 0, 0);
	if (rc)
		return rc;
	c->timestamp = (uint32_t)out[0];
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
		return call(d, DRGPU_UC_CTX_DESTROY, in, 1, 0, 0, 0, 0, 0, 0);
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
	case 0x45:	/* GPUOBJ_ALLOC */
		return gpuobj_alloc(d, arg);
	case 0x46:	/* GPUOBJ_FREE */
		return gpuobj_free(d, arg);
	case 0x4a:	/* GPU_COMMAND */
		return gpu_command(d, arg);
	default:
		host_log("drgles: KGSL ioctl %08lx not in the measured set\n", req);
		return -E_INVAL;
	}
}

long kgsl_ioctl(int fd, unsigned long req, void *arg)
{
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
}

