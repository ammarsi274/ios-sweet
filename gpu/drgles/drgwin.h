/*
 * drgles window buffers: memory we own (an IOSurface on iOS) handed to
 * Nokia's EGL as an Android window buffer, so eglCreateImageKHR
 * (EGL_NATIVE_BUFFER_ANDROID) makes it a render target or texture.
 * Measured on TWRP (drg/GPU-DRAW-PLAN.md, bench -owngbuf):
 *   - EGL takes the ANativeWindowBuffer below (magic '_bfr' at +0, handle
 *     at +0x60: eglSubDriverAndroid.so 0x54a4) and asks our gralloc module
 *     (bionic/gralloc.c) only Retain, GetConsumerUsage, GetLayerCount,
 *     Perform 10 (answered 0) and 9, Release;
 *   - the memory reaches the GPU by KGSL GPUOBJ_IMPORT of the handle's
 *     first fd (type 3, dma-buf);
 *   - the handle is Nokia's private_handle_t (gralloc.sdm660.so, version
 *     12, 2 fds, 24 ints); the metadata fd may be -1.
 */
#ifndef DRGLES_DRGWIN_H
#define DRGLES_DRGWIN_H
#include <stdint.h>

struct drgwin_anwb {
	int32_t magic, version;
	void *reserved[4];
	void (*inc)(void *), (*dec)(void *);
	int32_t width, height, stride, format, usage_deprecated;
	uint64_t layer_count;
	void *reserved2;
	const void *handle;
	uint64_t usage;
	void *reserved_proc[7];
};
_Static_assert(__builtin_offsetof(struct drgwin_anwb, handle) == 0x60, "ANativeWindowBuffer handle");
_Static_assert(sizeof(struct drgwin_anwb) == 168, "ANativeWindowBuffer size");

#define DRGWIN_HANDLE_INTS	(3 + 2 + 24)	/* native_handle header, fds, ints */
#define DRGWIN_FMT_RGBA_8888	1
#define DRGWIN_FMT_BGRA_8888	5
#define DRGWIN_USAGE_GPU_RENDER	(1ULL << 10)	/* gralloc1 producer GPU_RENDER_TARGET */
#define DRGWIN_USAGE_GPU_TEXTURE (1ULL << 8)	/* gralloc1 consumer GPU_TEXTURE */

/*
 * A private_handle_t for linear memory of `size` bytes, rows `stride`
 * pixels apart, `rows` rows allocated (aligned height). fd: the dma-buf (a
 * token from kgsl_ios_surface_fd on iOS); no metadata buffer, no CPU base.
 */
void drgwin_handle(int32_t h[DRGWIN_HANDLE_INTS], int fd, uint32_t width, uint32_t height, uint32_t stride,
		   uint32_t rows, int32_t format, uint32_t size);

/* The window buffer EGL gets for handle h */
void drgwin_anwb(struct drgwin_anwb *b, const void *h, uint32_t width, uint32_t height, uint32_t stride,
		 int32_t format, uint64_t usage);

#endif
