/*
 * Our gralloc module for the Nokia's EGL (eglSubDriverAndroid.so), which
 * will not initialise without one ("OpenGrallocModuleAndDevice: Failed to
 * get Gralloc module", EGL_BAD_ALLOC).
 *
 * Measured with Nokia's own gralloc.sdm660.so on the TWRP bench
 * (2026-10-30): EGL calls hw_get_module("gralloc"), gets a module with tag
 * HWMT, module/HAL API 1.0, opens device "gralloc" (tag HWDT, version
 * 0x100, gralloc1 capabilities 1, 2, 3) and then asks for no gralloc1
 * function at all for pbuffer contexts. This module answers exactly that;
 * any function EGL asks for is logged and answered with NULL, so the later
 * IOSurface work shows what it needs.
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

static void *dev_get(void *d, int32_t desc)
{
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
