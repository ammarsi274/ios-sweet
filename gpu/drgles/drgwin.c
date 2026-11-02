/*
 * drgles window buffers (drgwin.h). The private_handle_t words, as Nokia's
 * gralloc.sdm660.so filled them for a 1125x2436 RGBA_8888 render target on
 * TWRP (2026-11-02), data[] index after the 3-int native_handle header:
 *   0 fd, 1 metadata fd, 2 magic 'gmsm', 3 flags 0x00100208, 4/5 aligned
 *   width/height (1152, 2464), 6/7 width/height, 8 format, 9 buffer type 0,
 *   10 size, 11 offset, 12 metadata offset, 13-14 base (0: not mapped),
 *   15-16 metadata base, 17-18 GPU address, 19-20 id, 21-22 producer usage
 *   (0x404), 23-24 consumer usage (0x100), 25 layer count 1.
 */
#include "drgwin.h"

static uint64_t next_id = 1;

static void put64(int32_t *d, uint64_t v)
{
	d[0] = (int32_t)(uint32_t)v;
	d[1] = (int32_t)(uint32_t)(v >> 32);
}

void drgwin_handle(int32_t h[DRGWIN_HANDLE_INTS], int fd, uint32_t width, uint32_t height, uint32_t stride,
		   uint32_t rows, int32_t format, uint32_t size)
{
	int32_t *d = h + 3;
	for (int i = 0; i < DRGWIN_HANDLE_INTS; i++)
		h[i] = 0;
	h[0] = 12;	/* sizeof(native_handle_t) */
	h[1] = 2;	/* fds */
	h[2] = 24;	/* ints */
	d[0] = fd;
	d[1] = -1;
	d[2] = 0x676d736d;
	d[3] = 0x00100208;
	d[4] = (int32_t)stride;
	d[5] = (int32_t)rows;
	d[6] = (int32_t)width;
	d[7] = (int32_t)height;
	d[8] = format;
	d[10] = (int32_t)size;
	put64(d + 19, next_id++);
	put64(d + 21, DRGWIN_USAGE_GPU_RENDER | (1ULL << 2) /* CPU_READ_OFTEN */);
	put64(d + 23, DRGWIN_USAGE_GPU_TEXTURE);
	d[25] = 1;
}

static void anwb_ref(void *p)
{
	(void)p;
}

void drgwin_anwb(struct drgwin_anwb *b, const void *h, uint32_t width, uint32_t height, uint32_t stride,
		 int32_t format, uint64_t usage)
{
	struct drgwin_anwb z = { 0 };
	*b = z;
	b->magic = 0x5f626672;	/* '_bfr' */
	b->version = sizeof(*b);
	b->inc = b->dec = anwb_ref;
	b->width = (int32_t)width;
	b->height = (int32_t)height;
	b->stride = (int32_t)stride;
	b->format = format;
	b->usage_deprecated = (int32_t)usage;
	b->layer_count = 1;
	b->handle = h;
	b->usage = usage;
}
