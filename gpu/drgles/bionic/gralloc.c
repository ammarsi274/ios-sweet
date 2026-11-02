/*
 * Our gralloc module for the Nokia's EGL (eglSubDriverAndroid.so), which
 * will not initialise without one ("OpenGrallocModuleAndDevice: Failed to
 * get Gralloc module", EGL_BAD_ALLOC).
 *
 * Measured with Nokia's own gralloc.sdm660.so on the TWRP bench
 * (2026-10-30): EGL calls hw_get_module("gralloc"), gets a module with tag
 * HWMT, module/HAL API 1.0, opens device "gralloc" (tag HWDT, version
 * 0x100, gralloc1 capabilities 1, 2, 3) and then asks for no gralloc1
 * function at all for pbuffer contexts. For EGLImages of window buffers
 * (IOSurfaces on iOS) it answers the functions measured below from buffers
 * registered with b_gralloc_register; anything else is logged and NULL.
 *
 * Structures: Android's HAL ABI as compiled into eglSubDriverAndroid.so
 * (LP64): hw_module_t, hw_module_methods_t, hw_device_t, gralloc1_device_t.
 */
#include "bionic.h"

struct hw_module;
struct hw_methods {
	int (*open)(const struct hw_module *, const char *, void **);
};
struct hw_module {
	uint32_t tag;
	uint16_t mver, hver;
	const char *id, *name, *author;
	const struct hw_methods *methods;
	void *dso;
	uint64_t reserved[25];
};
struct gr1_dev {
	uint32_t tag, version;
	const struct hw_module *module;
	uint64_t reserved[12];
	int (*close)(void *);
	void (*caps)(void *, uint32_t *, int32_t *);
	void *(*get)(void *, int32_t);
};

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static int dev_close(void *d)
{
	return 0;
}

static void dev_caps(void *d, uint32_t *n, int32_t *out)
{
	static const int32_t caps[3] = { 1, 2, 3 };	/* as Nokia's module reports */
	if (!out) {
		*n = 3;
		return;
	}
	uint32_t k = *n < 3 ? *n : 3;
	for (uint32_t i = 0; i < k; i++)
		out[i] = caps[i];
	*n = k;
}

/*
 * Buffers EGL may be handed (EGLImage from an ANativeWindowBuffer): ours,
 * registered by the window layer with the CPU address of the memory. What
 * EGL calls for such a buffer was measured with Nokia's module on TWRP
 * (2026-11-02, drg/GPU-DRAW-PLAN.md): Retain 15, GetConsumerUsage 9,
 * GetLayerCount 22 (3x), Perform op 10 (GetRgbDataAddress: the CPU address)
 * and op 9 (UBWC: 0 = linear), Release 16; the memory itself reaches the GPU
 * through KGSL GPUOBJ_IMPORT of the handle's first fd. The plain getters are
 * answered from the same record in case another path asks.
 */
#define NBUF 64
static struct gbuf {
	const void *handle;
	void *cpu;
	uint32_t width, height, stride;
	int32_t format;
	uint64_t usage, id;
} bufs[NBUF];
static uint64_t next_id = 1;

int b_gralloc_register(const void *handle, void *cpu, uint32_t width, uint32_t height, uint32_t stride,
		       int32_t format, uint64_t usage)
{
	for (int i = 0; i < NBUF; i++)
		if (!bufs[i].handle) {
			bufs[i] = (struct gbuf){ handle, cpu, width, height, stride, format, usage, next_id++ };
			return 0;
		}
	return -LE_NOMEM;
}

void b_gralloc_unregister(const void *handle)
{
	for (int i = 0; i < NBUF; i++)
		if (bufs[i].handle == handle)
			bufs[i].handle = 0;
}

int b_gralloc_trace;	/* bench: log every call */

static struct gbuf *buf_of(const void *handle)
{
	for (int i = 0; i < NBUF; i++)
		if (handle && bufs[i].handle == handle)
			return &bufs[i];
	b_log("drgles: gralloc: unknown buffer %p\n", handle);
	return 0;
}

#define GR1_ERROR_BAD_HANDLE 2	/* gralloc1.h GRALLOC1_ERROR_BAD_HANDLE */
#define GR1_ERROR_UNSUPPORTED 7

#define TRACE(...) do { if (b_gralloc_trace) b_log(__VA_ARGS__); } while (0)

static int32_t gr_retain(void *d, const void *h)
{
	TRACE("drgles: gralloc Retain(%p)\n", h);
	return buf_of(h) ? 0 : GR1_ERROR_BAD_HANDLE;
}

static int32_t gr_release(void *d, const void *h)
{
	TRACE("drgles: gralloc Release(%p)\n", h);
	return buf_of(h) ? 0 : GR1_ERROR_BAD_HANDLE;
}

static int32_t gr_consumer_usage(void *d, const void *h, uint64_t *out)
{
	struct gbuf *b = buf_of(h);
	if (!b)
		return GR1_ERROR_BAD_HANDLE;
	*out = b->usage;
	TRACE("drgles: gralloc GetConsumerUsage(%p) -> %#llx\n", h, (unsigned long long)b->usage);
	return 0;
}

static int32_t gr_producer_usage(void *d, const void *h, uint64_t *out)
{
	return gr_consumer_usage(d, h, out);
}

static int32_t gr_layer_count(void *d, const void *h, uint32_t *out)
{
	TRACE("drgles: gralloc GetLayerCount(%p)\n", h);
	if (!buf_of(h))
		return GR1_ERROR_BAD_HANDLE;
	*out = 1;
	return 0;
}

static int32_t gr_dimensions(void *d, const void *h, uint32_t *w, uint32_t *hh)
{
	struct gbuf *b = buf_of(h);
	if (!b)
		return GR1_ERROR_BAD_HANDLE;
	*w = b->width;
	*hh = b->height;
	return 0;
}

static int32_t gr_format(void *d, const void *h, int32_t *out)
{
	struct gbuf *b = buf_of(h);
	if (!b)
		return GR1_ERROR_BAD_HANDLE;
	*out = b->format;
	return 0;
}

static int32_t gr_stride(void *d, const void *h, uint32_t *out)
{
	struct gbuf *b = buf_of(h);
	if (!b)
		return GR1_ERROR_BAD_HANDLE;
	*out = b->stride;
	return 0;
}

static int32_t gr_backing_store(void *d, const void *h, uint64_t *out)
{
	struct gbuf *b = buf_of(h);
	if (!b)
		return GR1_ERROR_BAD_HANDLE;
	*out = b->id;
	return 0;
}

/*
 * Perform (id 0x1000) is variadic for its callers. Android's AAPCS64 passes
 * variadic integers in x2..x7 like fixed ones; Darwin would read them from
 * the stack. Fixed parameters therefore match the caller on both hosts.
 */
static int32_t gr_perform(void *d, int32_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t e)
{
	struct gbuf *g = buf_of((const void *)(uintptr_t)a);
	TRACE("drgles: gralloc Perform(op %d, %#llx, %#llx) cpu %p\n", op, (unsigned long long)a, (unsigned long long)b,
	      g ? g->cpu : 0);
	switch (op) {
	case 10:	/* GetRgbDataAddress(handle, void **) */
		if (!g)
			return GR1_ERROR_BAD_HANDLE;
		*(void **)(uintptr_t)b = g->cpu;
		return 0;
	case 9:		/* (handle, int *): UBWC; ours are linear */
		if (!g)
			return GR1_ERROR_BAD_HANDLE;
		*(int32_t *)(uintptr_t)b = 0;
		return 0;
	default:
		b_log("drgles: gralloc1 Perform op %d (%#llx, %#llx, %#llx) not provided\n", op, (unsigned long long)b,
		      (unsigned long long)c, (unsigned long long)e);
		return GR1_ERROR_UNSUPPORTED;
	}
}

static void *dev_get(void *d, int32_t desc)
{
	TRACE("drgles: gralloc getFunction(%d)\n", desc);
	switch (desc) {
	case 8: return (void *)gr_backing_store;
	case 9: return (void *)gr_consumer_usage;
	case 10: return (void *)gr_dimensions;
	case 11: return (void *)gr_format;
	case 12: return (void *)gr_producer_usage;
	case 13: return (void *)gr_stride;
	case 15: return (void *)gr_retain;
	case 16: return (void *)gr_release;
	case 22: return (void *)gr_layer_count;
	case 0x1000: return (void *)gr_perform;
	}
	b_log("drgles: gralloc1 function %d requested (not provided)\n", desc);
	return 0;
}

static int mod_open(const struct hw_module *m, const char *id, void **dev);
static const struct hw_methods methods = { mod_open };
static const struct hw_module module = {
	TAG('H', 'W', 'M', 'T'), 0x0100, 0x0100, "gralloc", "drgles gralloc", "drgles", &methods, 0, { 0 }
};
static struct gr1_dev device = {
	TAG('H', 'W', 'D', 'T'), 0x0100, &module, { 0 }, dev_close, dev_caps, dev_get
};

static int mod_open(const struct hw_module *m, const char *id, void **dev)
{
	if (strcmp(id, "gralloc")) {
		b_log("drgles: gralloc device %s requested (not provided)\n", id);
		return -LE_INVAL;
	}
	*dev = &device;
	return 0;
}

int b_gralloc_get_module(const char *id, const void **out)
{
	if (strcmp(id, "gralloc")) {
		b_log("drgles: hw_get_module(%s): not provided\n", id);
		return -LE_NOENT;
	}
	*out = &module;
	return 0;
}
