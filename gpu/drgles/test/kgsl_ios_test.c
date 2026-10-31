/*
 * Host test of kgsl_ios.c: Nokia's measured KGSL call sequence
 * (drg/gpu-research/vendor-adreno trace kgsl-trace-2/-1-bytes) against a
 * mock of XNU's DrgGPU user client that checks every selector and argument
 * and behaves as DrgGPU.cpp does (objects at task addresses, 16 KiB pages).
 *   gcc -O1 -Wall -Wextra -I.. -o /tmp/kt kgsl_ios_test.c ../kgsl_ios.c && /tmp/kt
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kgsl.h"
#include "../../drgpu/drgpu_uc.h"

typedef unsigned int mach_port_t;
mach_port_t mach_task_self_ = 0x103;
static int fails, opened, closed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- host.h stubs ---- */
int host_open(const char *path, int flags, int mode) { (void)flags; (void)mode; CHECK(!strcmp(path, "/dev/null")); return 7; }
int host_close(int fd) { CHECK(fd == 7); closed++; return 0; }
void *host_alloc(size_t n) { return calloc(1, n); }
void host_free(void *p) { free(p); }
void host_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); va_end(ap); }

/* ---- mock DrgGPU ---- */
#define E(x) (int)(0xe0000000u | (x))
static uint64_t next_va = 0x7ffff0000ull;	/* top-down like the trace; 16 KiB pages */
static uint32_t next_id = 1, live_objs, pending_free;
static uint64_t last_submit[3];
static struct drgpu_uc_ib last_ibs[DRGPU_UC_MAX_IBS];
static size_t last_nibs;
void *IOServiceMatching(const char *name) { CHECK(!strcmp(name, "DrgGPU")); return (void *)1; }
mach_port_t IOServiceGetMatchingService(mach_port_t p, const void *m) { CHECK(p == 0 && m); return 0x200; }
int IOServiceOpen(mach_port_t s, mach_port_t task, uint32_t type, mach_port_t *c) { CHECK(s == 0x200 && task == 0x103 && type == 0); *c = 0x300; opened++; return 0; }
int IOServiceClose(mach_port_t c) { CHECK(c == 0x300); return 0; }
int IOObjectRelease(mach_port_t o) { (void)o; return 0; }
int IOConnectCallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t nin, const void *sin, size_t nsin,
			uint64_t *out, uint32_t *nout, void *sout, size_t *nsout)
{
	CHECK(c == 0x300);
	switch (sel) {
	case DRGPU_UC_GET_VERSION: out[0] = DRGPU_UC_VERSION; return 0;
	case DRGPU_UC_GETPROPERTY:
		CHECK(nin == 1 && sout && nsout);
		if (in[0] == 0x02) { CHECK(*nsout == 24); memset(sout, 0, 24); ((uint32_t *)sout)[0] = 0xf8001000; return 0; }
		if (in[0] == 0x01) { CHECK(*nsout == 40); memset(sout, 0, 40); ((uint32_t *)sout)[1] = 0x05010200; return 0; }
		return E(0x2c7);	/* unsupported -> EINVAL */
	case DRGPU_UC_SYNCSRC_CREATE: out[0] = 1; return 0;
	case DRGPU_UC_SYNCSRC_DESTROY: CHECK(nin == 1 && in[0] == 1); return 0;
	case DRGPU_UC_MEMSTORE: out[0] = 0x100abc000ull; out[1] = 0x2000; return 0;
	case DRGPU_UC_CTX_CREATE: CHECK(nin == 1 && in[0] == 0x001088d2); out[0] = 2; return 0;
	case DRGPU_UC_CTX_DESTROY: CHECK(nin == 1 && in[0] == 2); return 0;
	case DRGPU_UC_OBJ_ALLOC: {
		CHECK(nin == 2 && *nout >= 3 && in[1] == 0x100c1000);
		uint64_t sz = (in[0] + 0x3fff) & ~0x3fffull;
		next_va -= sz;
		out[0] = next_id++; out[1] = next_va; out[2] = sz;
		live_objs++;
		return 0;
	}
	case DRGPU_UC_OBJ_FREE:
		CHECK(nin == 4);
		if (in[1]) { CHECK(in[2] == 2 && in[3] == 0x1080); pending_free++; }
		live_objs--;
		return 0;
	case DRGPU_UC_SUBMIT:
		CHECK(nin == 3 && nsin % sizeof(struct drgpu_uc_ib) == 0);
		memcpy(last_submit, in, sizeof(last_submit));
		last_nibs = nsin / sizeof(struct drgpu_uc_ib);
		if (nsin) memcpy(last_ibs, sin, nsin);
		out[0] = in[2];
		return 0;
	case DRGPU_UC_WAIT: CHECK(nin == 3); return 0;
	default: CHECK(!"unexpected selector"); return E(0x2c7);
	}
}

/* msm_kgsl.h ioctl numbers as the trace shows them */
struct getproperty { uint32_t type, pad; uint64_t value, sizebytes; };
struct gpuobj_alloc { uint64_t size, flags, va_len, mmapsize; uint32_t id, metadata_len; uint64_t metadata; };
struct gpuobj_free { uint64_t flags, priv; uint32_t id, type, len; };
struct gpu_command { uint64_t flags, cmdlist; uint32_t cmdsize, numcmds; uint64_t objlist; uint32_t objsize, numobjs;
		     uint64_t synclist; uint32_t syncsize, numsyncs, context_id, timestamp; };
struct command_object { uint64_t offset, gpuaddr, size; uint32_t flags, id; };

int main(void)
{
	int err;
	int fd = kgsl_open(0x80002);
	CHECK(fd == 7 && kgsl_is_fd(7) && opened == 1);

	uint32_t ss[4] = { 0 };
	CHECK(kgsl_ioctl(fd, 0xc0100940, ss) == 0 && ss[0] == 1);	/* SYNCSOURCE_CREATE */
	CHECK(kgsl_ioctl(fd, 0xc0100941, ss) == 0);			/* SYNCSOURCE_DESTROY */

	uint8_t shadow[24], info[40];
	struct getproperty gp = { 0x2, 0, (uintptr_t)shadow, 24 };
	CHECK(kgsl_ioctl(fd, 0xc0180902, &gp) == 0 && *(uint32_t *)shadow == 0xf8001000);
	void *ms = kgsl_mmap(0, 0x2000, 1, 1, fd, 0xf8001000L, &err);
	CHECK(ms == (void *)0x100abc000ull && err == 0);
	CHECK(kgsl_mmap(0, 0x2000, 3, 1, fd, 0xf8001000L, &err) == (void *)-1);	/* memstore is read-only */
	gp = (struct getproperty){ 0x1, 0, (uintptr_t)info, 40 };
	CHECK(kgsl_ioctl(fd, 0xc0180902, &gp) == 0 && ((uint32_t *)info)[1] == 0x05010200);
	gp = (struct getproperty){ 0x21, 0, (uintptr_t)info, 4 };
	CHECK(kgsl_ioctl(fd, 0xc0180902, &gp) == -22);			/* 0x21, 0x23, 0x24: EINVAL on the phone */

	uint32_t dc[2] = { 0x001088d2, 0 };
	CHECK(kgsl_ioctl(fd, 0xc0080913, dc) == 0 && dc[1] == 2);	/* DRAWCTXT_CREATE */

	/* 36 objects as in the trace: ALLOC, then mmap(off = id << 12) */
	uint64_t va[40];
	uint32_t id[40];
	for (int i = 0; i < 36; i++) {
		struct gpuobj_alloc a = { i % 4 ? 0x1000 : 0x4000, 0x100c1000, 0, 0, 0, 0, 0 };
		CHECK(kgsl_ioctl(fd, 0xc0300945, &a) == 0);
		CHECK(a.mmapsize >= a.size && a.mmapsize % 0x4000 == 0);
		void *p = kgsl_mmap(0, a.mmapsize, 3, 1, fd, (long)a.id << 12, &err);
		CHECK(p != (void *)-1 && err == 0);
		CHECK(kgsl_mmap(0, a.mmapsize + 0x4000, 3, 1, fd, (long)a.id << 12, &err) == (void *)-1);	/* too long */
		va[i] = (uintptr_t)p;
		id[i] = a.id;
	}
	CHECK(kgsl_mmap(0, 0x1000, 3, 1, fd, (long)999 << 12, &err) == (void *)-1);	/* unknown id */

	/* GPU_COMMAND: one IB in object 24's mapping, as the trace (0x7fffbb000 + 0x314) */
	struct command_object co = { 0, va[24] + 0x314, 0x548, 1, id[24] };
	struct gpu_command gc = { 0x8, (uintptr_t)&co, sizeof(co), 1, 0, 0, 0, 0, 0, 0, 2, 0x2100 };
	CHECK(kgsl_ioctl(fd, 0xc040094a, &gc) == 0 && gc.timestamp == 0x2100);
	CHECK(last_submit[0] == 2 && last_submit[1] == 0x8 && last_submit[2] == 0x2100);
	CHECK(last_nibs == 1 && last_ibs[0].gpuaddr == va[24] + 0x314 && last_ibs[0].bytes == 0x548);
	struct command_object pre = { 0, va[3], 8, 2, id[3] };	/* a context-switch preamble: skipped */
	struct command_object two[2] = { pre, co };
	gc = (struct gpu_command){ 0x2, (uintptr_t)two, sizeof(co), 2, 0, 0, 0, 0, 0, 0, 2, 0x3180 };
	CHECK(kgsl_ioctl(fd, 0xc040094a, &gc) == 0 && last_nibs == 1);

	/* frees: immediate, then on a timestamp (flags 1, priv -> {ctx, ts}), each followed by munmap */
	struct gpuobj_free f = { 0, 0, id[0], 0, 0 };
	CHECK(kgsl_ioctl(fd, 0x40200946, &f) == 0);
	CHECK(kgsl_munmap((void *)(uintptr_t)va[0], 0x4000) == 1);	/* never passed to the host */
	uint32_t ev[2] = { 2, 0x1080 };
	f = (struct gpuobj_free){ 1, (uintptr_t)ev, id[1], 1, 8 };
	CHECK(kgsl_ioctl(fd, 0x40200946, &f) == 0 && pending_free == 1);
	CHECK(kgsl_munmap((void *)(uintptr_t)va[1], 0x1000) == 1);
	CHECK(kgsl_ioctl(fd, 0x40200946, &f) == -22);			/* double free */
	CHECK(kgsl_munmap((void *)0x12345000, 0x1000) == 0);		/* not ours: to the host */
	CHECK(kgsl_munmap(ms, 0x2000) == 1);

	CHECK(kgsl_ioctl(fd, 0xc0080999, dc) == -22);			/* outside the measured set */
	uint32_t dd[1] = { 2 };
	CHECK(kgsl_ioctl(fd, 0x40040914, dd) == 0);			/* DRAWCTXT_DESTROY */
	CHECK(live_objs == 34);
	CHECK(kgsl_close(fd) == 1 && closed == 1 && !kgsl_is_fd(fd));
	printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
	return fails != 0;
}
