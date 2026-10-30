/*
 * drgles KGSL layer: the /dev/kgsl-3d0 interface the Nokia's Adreno driver
 * uses (ioctl numbers and structs from HMD's DRG kernel,
 * include/uapi/linux/msm_kgsl.h). Measured use: open, DEVICE_GETPROPERTY,
 * DRAWCTXT_CREATE/DESTROY, GPUOBJ_ALLOC/FREE + mmap(off = id << 12),
 * GPU_COMMAND, SYNCSOURCE_CREATE/DESTROY, memstore mmap.
 *
 * kgsl_bench.c: the real device under TWRP, every call checked against
 * that set and counted. kgsl_ios.c (later): our XNU GPU driver.
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

#endif
