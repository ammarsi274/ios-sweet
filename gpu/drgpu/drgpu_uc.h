/*
 * drgpu user client ABI: XNU's DrgGPU (iokit/Kernel/arm/DrgGPU.cpp) and the
 * drgles host layer that turns Nokia's KGSL calls into IOConnectCallMethod.
 *
 * One method per KGSL call Nokia's driver makes (drg/gpu-research/
 * TWRP-MEASURED.md §4), plus WAIT (sleeps on the GPU interrupt instead of
 * polling the memstore) and the IOSurface import a window system needs.
 * Every scalar is a uint64_t. Open the service named DRGPU_UC_SERVICE,
 * connection type 0. GPU addresses are addresses in the calling task
 * (KGSL USE_CPU_MAP): the kernel maps each object into the task and, at the
 * same address, into the task's own GPU address space.
 */
#ifndef DRGPU_UC_H
#define DRGPU_UC_H
#include <stdint.h>

#define DRGPU_UC_SERVICE	"DrgGPU"
#define DRGPU_UC_VERSION	1

enum drgpu_uc_selector {
	/* in: KGSL property type; struct out: its bytes, exactly the size KGSL returns */
	DRGPU_UC_GETPROPERTY = 0,
	/* in: KGSL context flags; out: context id */
	DRGPU_UC_CTX_CREATE = 1,
	/* in: context id (waits for its submissions first) */
	DRGPU_UC_CTX_DESTROY = 2,
	/* in: size, KGSL memflags; out: id, GPU address (= task address), mapped size */
	DRGPU_UC_OBJ_ALLOC = 3,
	/* in: id, on_event, context id, timestamp: on_event 1 frees once the
	 * context retired the timestamp (KGSL_GPUOBJ_FREE_ON_EVENT), 0 now */
	DRGPU_UC_OBJ_FREE = 4,
	/* in: context id, KGSL cmdbatch flags, timestamp; struct in: n x
	 * struct drgpu_uc_ib (n <= DRGPU_UC_MAX_IBS); out: timestamp */
	DRGPU_UC_SUBMIT = 5,
	/* in: context id, timestamp, timeout in ms */
	DRGPU_UC_WAIT = 6,
	/* out: task address of the memstore (read-only, KGSL's DEVICE_SHADOW), size */
	DRGPU_UC_MEMSTORE = 7,
	/* out: id */
	DRGPU_UC_SYNCSRC_CREATE = 8,
	/* in: id */
	DRGPU_UC_SYNCSRC_DESTROY = 9,
	/* in: IOSurface id; out: object id, GPU address (= task address), size */
	DRGPU_UC_SURFACE_MAP = 10,
	/* in: object id: clean and invalidate the CPU caches over it (for memory
	 * the CPU maps cached, such as IOSurfaces created cacheable) */
	DRGPU_UC_OBJ_SYNC = 11,
	/* out: DRGPU_UC_VERSION */
	DRGPU_UC_GET_VERSION = 12,
	DRGPU_UC_COUNT
};

struct drgpu_uc_ib {
	uint64_t gpuaddr;	/* 4-byte aligned */
	uint64_t bytes;		/* multiple of 4 */
};

#define DRGPU_UC_MAX_IBS	16

/* KGSL values the kernel interprets (HMD msm_kgsl.h) */
#define DRGPU_KGSL_CONTEXT_USER_GENERATED_TS	0x00000080u
#define DRGPU_KGSL_CMDBATCH_MARKER		0x00000002u
#define DRGPU_KGSL_MEMFLAGS_GPUREADONLY		0x01000000u

#endif
