/*
 * drgles KGSL layer: the /dev/kgsl-3d0 interface the Nokia's Adreno driver
 * uses (ioctl numbers and structs from HMD's DRG kernel,
 * include/uapi/linux/msm_kgsl.h). Measured use: open, DEVICE_GETPROPERTY,
 * DRAWCTXT_CREATE/DESTROY, GPUOBJ_ALLOC/FREE + mmap(off = id << 12),
 * GPU_COMMAND, SYNCSOURCE_CREATE/DESTROY, memstore mmap.
 *
 * kgsl_bench.c: the real device under TWRP, every call checked against
 * that set and counted. kgsl_ios.c: our XNU GPU driver (DrgGPU, gpu/drgpu/drgpu_uc.h).
 */
#ifndef DRGLES_KGSL_H
#define DRGLES_KGSL_H

#include <stddef.h>
#include <stdint.h>

int kgsl_open(int flags);		/* fd or -errno */
int kgsl_is_fd(int fd);
long kgsl_ioctl(int fd, unsigned long req, void *arg);	/* result or -errno */
void *kgsl_mmap(void *addr, size_t len, int prot, int flags, int fd, long off, int *err);
int kgsl_munmap(void *addr, size_t len);	/* 1 if it was ours */
int kgsl_close(int fd);			/* 1 if it was ours */
void kgsl_report(void);
int kgsl_capture_start(const char *path);	/* bench: record GPU work for replay (0 or -errno) */
void kgsl_capture_nowait(void);
void kgsl_trace(void);				/* -trace: completion state at submits and kgsl_trace_point() */
void kgsl_trace_point(const char *what);
void kgsl_ios_options(int dsb, int deferfree);	/* kgsl_ios.c only: boot #42 isolation switches */
/* kgsl_ios.c only: DrgGPU submission framing (DRGPU_UC_OPT_*) for connections opened later */
void kgsl_ios_framing(uint32_t opts);
int kgsl_ios_faulted(void);			/* a submit was refused: GPU fault */
void kgsl_ios_readback(int mode);	/* CPU read-back before submits: 1 its objects, 2 all */
/* kgsl_ios.c only: cache maintenance before each submit (research/reports/gpu-fault-2.md §4A) */
#define KGSL_IOS_FLUSH_IBS	0	/* dc civac over the IB1 ranges, dsb sy (r23) */
#define KGSL_IOS_FLUSH_ALL	1	/* also every live object that is not an import (default since r25) */
#define KGSL_IOS_DELAY		2	/* no dc civac; dsb sy, then a 2 ms spin */
void kgsl_ios_flush(int mode);
int kgsl_ios_flush_mode(void);
/*
 * kgsl_ios.c only, root r25 (boot #55: an IOSurface render target read by the CPU was 48 % stale):
 * the GPU writes the IOSurface `surface` (a render target the CPU or the display reads later) ...
 */
void kgsl_ios_surface_gpu_written(uint32_t surface);
/*
 * ... and before the CPU reads one: wait until every context of this process retired its last
 * submitted timestamp, then dc civac over every live import of a marked surface (its GPU mapping;
 * the maintenance acts on the physical lines, so every other mapping of them sees RAM). Returns the
 * number of surfaces invalidated, or a negative errno if a wait failed (they are invalidated anyway).
 */
int kgsl_ios_cpu_sync(void);
/* totals since start: cpu_sync calls, bytes invalidated by them, ns spent in their dc civac loops */
void kgsl_ios_sync_stats(uint64_t *calls, uint64_t *bytes, uint64_t *ns);
/*
 * Self-test modes (bench -selftest; since r25 the verdict is cached in /private/var/db/drg/gpu-selftest,
 * verdict.h): bits 0-1 DrgGPU framing, bits 2-3 CPU read-back, bit 4 flushall, bit 5 delay.
 */
#define DRG_MODE_FLUSHALL	16
#define DRG_MODE_DELAY		32
#define DRG_MODE_MAX		63
static inline void kgsl_ios_mode(int mode)
{
	kgsl_ios_framing((uint32_t)mode & 3);
	kgsl_ios_readback((mode >> 2) & 3);
	kgsl_ios_flush(mode & DRG_MODE_DELAY ? KGSL_IOS_DELAY : mode & DRG_MODE_FLUSHALL ? KGSL_IOS_FLUSH_ALL
											: KGSL_IOS_FLUSH_IBS);
}
/* kgsl_ios.c only: a placeholder fd that stands for an IOSurface as a dma-buf (GPUOBJ_IMPORT type 3) */
int kgsl_ios_surface_fd(uint32_t surface);			/* capture without waiting for each submit (phase 1 = right after it) */
/* kgsl_cap.c, called by each platform's layer around every ioctl and mmap */
void kgsl_cap_before(int fd, unsigned long req, void *arg);
void kgsl_cap_after(int fd, unsigned long req, void *arg, long rc);
void kgsl_cap_mmap(void *p, size_t len, int prot, int flags, long off);
long kgsl_cap_wait(int fd, uint32_t ctx, uint32_t ts, uint32_t timeout_ms);	/* the platform's: 0 or -errno */

#endif
