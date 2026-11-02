/*
 * drgles bench, stage 1: the Nokia's Adreno driver loaded by OUR loader
 * (elf.c) on TWRP, drawing the same checked images as gles-probe.
 *
 * Imports from Android's system libraries are forwarded to TWRP's own
 * (bionic, libm, libz, liblog, libcutils, libc++, libhardware) through a
 * counting trampoline each, so the run measures which of them the driver
 * really calls and how often. dlopen/dlsym/dladdr/dlerror/dlclose, the log
 * calls and libsync are ours already. Stage 2 replaces the forwarding,
 * group by group, with our own code that also builds for iOS.
 *
 *   bench [DIR...]      default DIR: /tmp/adreno
 * Output: test results, then "calls <count> <name>" for every import used.
 */
#include "elf.h"
#include "drgwin.h"
#include "host.h"
#include "rt.h"

/* bionic, used only by the bench itself */
extern void __libc_init(void *raw_args, void (*onexit)(void),
			int (*slingshot)(int, char **, char **), const void *structors);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int printf(const char *, ...);
extern int fflush(void *);
extern void abort(void);
extern long ioctl(int, unsigned long, ...);
#define RTLD_NOW 2

/* ---- counting trampolines (bench only: the page is RWX) ---- */
struct fwd {
	const char *name;
	void *target;
	uint64_t count;
};
#define MAXFWD 640
static struct fwd fwd[MAXFWD];
static int nfwd;
static uint32_t *tramp;
static int ntramp;

static void *make_tramp(struct fwd *f)
{
	if (!tramp)
		tramp = host_map(MAXFWD * 40, HOST_PROT_R | HOST_PROT_W | HOST_PROT_X);
	uint32_t *t = tramp + ntramp++ * 10;
	t[0] = 0x58000000 | (6 << 5) | 16;	/* ldr x16, counter (pc+24) */
	t[1] = 0xf9400211;			/* ldr x17, [x16] */
	t[2] = 0x91000631;			/* add x17, x17, #1 */
	t[3] = 0xf9000211;			/* str x17, [x16] */
	t[4] = 0x58000000 | (4 << 5) | 16;	/* ldr x16, target (pc+16) */
	t[5] = 0xd61f0200;			/* br x16 */
	*(uint64_t *)(t + 6) = (uint64_t)&f->count;
	*(uint64_t *)(t + 8) = (uint64_t)f->target;
	host_sync_icache(t, 40);
	return t;
}

/* ---- our own implementations (already host-independent in spirit) ---- */
static char dl_err[256];
static int dl_err_set;

static void *my_dlopen(const char *name, int flags)
{
	(void)flags;
	if (!name)
		return DRG_ELF_SYSTEM;	/* the main program: global lookup */
	struct drg_dso *d = drg_elf_open(name);
	host_log("dlopen(%s) -> %s\n", name, d ? "ok" : drg_elf_error());
	if (!d) {
		snprintf(dl_err, sizeof(dl_err), "%s", drg_elf_error());
		dl_err_set = 1;
	}
	return d;
}

static void *my_dlsym(void *h, const char *name)
{
	void *p;
	if (h == 0 || h == (void *)-1L)	/* RTLD_DEFAULT, RTLD_NEXT (bionic LP64) */
		p = drg_elf_sym(0, name);
	else
		p = drg_elf_sym(h, name);
	if (!p) {
		snprintf(dl_err, sizeof(dl_err), "undefined symbol: %s", name);
		dl_err_set = 1;
	}
	return p;
}

static char *my_dlerror(void)
{
	if (!dl_err_set)
		return 0;
	dl_err_set = 0;
	return dl_err;
}

static int my_dlclose(void *h)
{
	(void)h;
	return 0;	/* libraries stay loaded, like bionic for these */
}

struct dl_info {
	const char *fname;
	void *fbase;
	const char *sname;
	void *saddr;
};

static int my_dladdr(const void *a, struct dl_info *i)
{
	return drg_elf_addr(a, &i->fname, &i->fbase, &i->sname, &i->saddr);
}

static int my_log_print(int prio, const char *tag, const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	host_log("log %d %s: %s\n", prio, tag ? tag : "-", b);
	return 1;
}

static void my_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
	char b[1024] = "";
	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(b, sizeof(b), fmt, ap);
		va_end(ap);
	}
	host_die("ASSERT %s %s: %s\n", cond ? cond : "-", tag ? tag : "-", b);
}

/* libsync on HMD's fence interface (drivers/staging/android/uapi/sync.h) */
struct sync_merge_data {
	int fd2;
	char name[32];
	int fence;
};
static int my_sync_wait(int fd, int timeout)
{
	int t = timeout;
	return (int)ioctl(fd, 0x40043e00 /* SYNC_IOC_WAIT */, &t);
}
static int my_sync_merge(const char *name, int fd1, int fd2)
{
	struct sync_merge_data d = { .fd2 = fd2 };
	for (int i = 0; i < 31 && name && name[i]; i++)
		d.name[i] = name[i];
	if (ioctl(fd1, 0xc0283e01 /* SYNC_IOC_MERGE */, &d) < 0)
		return -1;
	return d.fence;
}

static uint64_t my_atrace_get_enabled_tags(void)
{
	return 0;
}

/* stage 1 only: Nokia's gralloc.sdm660.so is still loaded by TWRP's linker
 * (through libhardware) and imports this from a newer libcutils than TWRP's */
__attribute__((visibility("default"))) uint64_t atrace_get_enabled_tags(void)
{
	return 0;
}

/* stage 1 measurement: what the driver opens and reads (forwarded) */
extern void *fopen(const char *, const char *);
extern char *getenv(const char *);
static void *host_libs[8];
extern int __system_property_get(const char *, char *);
extern int open(const char *, int, ...);
static void *log_fopen(const char *path, const char *mode)
{
	void *f = fopen(path, mode);
	host_log("fopen(%s, %s) -> %s\n", path, mode, f ? "ok" : "fail");
	return f;
}
static char *log_getenv(const char *k)
{
	char *v = getenv(k);
	host_log("getenv(%s) -> %s\n", k, v ? v : "(null)");
	return v;
}
static int log_property_get(const char *k, char *v, const char *def)
{
	int (*pg)(const char *, char *, const char *) = dlsym(host_libs[4], "property_get");
	int n = pg(k, v, def);
	host_log("property_get(%s, def \"%s\") -> \"%s\"\n", k, def ? def : "", v);
	return n;
}
static int log_open2(const char *path, int flags)
{
	int fd = open(path, flags, 0);
	host_log("open(%s, 0%o) -> %d\n", path, flags, fd);
	return fd;
}

static const struct {
	const char *name;
	void *fn;
} own[] = {
	{ "fopen", log_fopen }, { "getenv", log_getenv }, { "property_get", log_property_get },
	{ "__open_2", log_open2 },
	{ "dlopen", my_dlopen }, { "dlsym", my_dlsym }, { "dlerror", my_dlerror },
	{ "dlclose", my_dlclose }, { "dladdr", my_dladdr },
	{ "__android_log_print", my_log_print }, { "__android_log_assert", my_log_assert },
	{ "sync_wait", my_sync_wait }, { "sync_merge", my_sync_merge },
	{ "atrace_get_enabled_tags", my_atrace_get_enabled_tags },
};

static const char *const host_names[] = { "libc.so", "libm.so", "libz.so", "liblog.so", "libcutils.so",
					  "libc++.so", "libhardware.so", 0 };

static void *host_libs[8];
/*
 * Measurement of what Nokia's EGL asks from gralloc. Android's HAL ABI as
 * compiled into eglSubDriverAndroid.so (LP64):
 *   hw_module_t: u32 tag, u16 module_api_version, u16 hal_api_version,
 *     id, name, author, methods, dso, reserved[25]
 *   methods: open(module, id, hw_device_t **)
 *   hw_device_t: u32 tag, u32 version, module, reserved[12], close
 *   gralloc1_device_t: hw_device_t, getCapabilities, getFunction
 * The real module (via TWRP's libhardware) is used; its open and
 * getFunction are wrapped so every function EGL takes is logged and counted.
 */
struct hw_module {
	uint32_t tag;
	uint16_t mver, hver;
	const char *id, *name, *author;
	struct { int (*open)(const struct hw_module *, const char *, void **); } *methods;
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
static struct hw_module gr_mod;
static struct { int (*open)(const struct hw_module *, const char *, void **); } gr_methods;
static int (*gr_real_open)(const struct hw_module *, const char *, void **);
static void *(*gr_real_get)(void *, int32_t);
static int (*real_hw_get_module)(const char *, const struct hw_module **);
static const struct hw_module *gr_real_mod;	/* Nokia's module itself (-gbuf allocates through it) */

/*
 * Qualcomm's gralloc1 Perform (id 0x1000, variadic): which operations EGL
 * asks for (-gbuf). AAPCS64 passes the variadic integer arguments in x2..x7
 * as fixed ones, so forwarding six of them is exact for up to six.
 */
static int (*gr_real_perform)(void *, int, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static int gr_perform(void *dev, int op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
	int r = gr_real_perform(dev, op, a, b, c, d, e, f);
	host_log("gralloc1 Perform(op %d, %#llx, %#llx, %#llx, %#llx) -> %d\n", op, (unsigned long long)a,
		 (unsigned long long)b, (unsigned long long)c, (unsigned long long)d, r);
	if (b > 0x10000 && (op == 9 || op == 10))	/* what it wrote (8 bytes at the out pointer) */
		host_log("gralloc1 Perform(op %d) out: %#llx\n", op, (unsigned long long)*(const uint64_t *)(uintptr_t)b);
	return r;
}

static void *gr_get(void *dev, int32_t desc)
{
	void *f = gr_real_get(dev, desc);
	host_log("gralloc1 getFunction(%d) -> %s\n", desc, f ? "fn" : "null");
	if (!f)
		return 0;
	if (desc == 0x1000) {
		gr_real_perform = f;
		return (void *)gr_perform;
	}
	char *name = host_alloc(24);
	snprintf(name, 24, "gralloc1 fn %d", desc);
	if (nfwd == MAXFWD)
		return f;
	struct fwd *w = &fwd[nfwd++];
	w->name = name;
	w->target = f;
	make_tramp(w);
	return tramp + (w - fwd) * 10;
}

static int gr_open(const struct hw_module *m, const char *id, void **dev)
{
	int r = gr_real_open(m, id, dev);
	host_log("gralloc open(%s) -> %d\n", id, r);
	if (!r && *dev) {
		struct gr1_dev *d = *dev;
		uint32_t n = 0;
		host_log("gralloc device tag %08x version %08x\n", d->tag, d->version);
		if (d->caps) {
			int32_t caps[16];
			d->caps(d, &n, 0);
			if (n > 16)
				n = 16;
			d->caps(d, &n, caps);
			for (uint32_t i = 0; i < n; i++)
				host_log("gralloc capability %d\n", caps[i]);
		}
		gr_real_get = d->get;
		d->get = gr_get;
	}
	return r;
}

static int gr_hw_get_module(const char *id, const struct hw_module **out)
{
	const struct hw_module *m = 0;
	int r = real_hw_get_module(id, &m);
	host_log("hw_get_module(%s) -> %d\n", id, r);
	if (r || !m) {
		*out = m;
		return r;
	}
	host_log("gralloc module tag %08x api %04x hal %04x id %s name \"%s\" author \"%s\"\n", m->tag, m->mver, m->hver,
		 m->id, m->name, m->author);
	gr_mod = *m;
	gr_real_mod = m;
	gr_real_open = m->methods->open;
	gr_methods.open = gr_open;
	gr_mod.methods = (void *)&gr_methods;
	*out = &gr_mod;
	return 0;
}

static int nokia_gralloc;
static void *bench_hook(const char *name)
{
	real_hw_get_module = dlsym(host_libs[6], name);	/* libhardware: Nokia's gralloc via TWRP */
	return gr_hw_get_module;
}

/* stage 2: the bionic layer answers first; whatever it lacks is still
 * forwarded to TWRP but reported as FALLBACK with its call count */
#include "bionic/bionic.h"
#include "kgsl.h"
static int stage = 2;
static unsigned fallback_from = MAXFWD;

static void *resolver(const char *name, int call, void *ctx)
{
	(void)ctx;
	if (stage == 2) {
		void *p = bionic_lookup(name);
		if (p)
			return p;
		if (fallback_from == MAXFWD)
			fallback_from = (unsigned)nfwd;
		host_log("FALLBACK import %s\n", name);
	}
	for (unsigned i = 0; i < sizeof(own) / sizeof(own[0]); i++)
		if (!strcmp(own[i].name, name))
			return own[i].fn;
	for (int i = 0; i < nfwd; i++)
		if (!strcmp(fwd[i].name, name))
			return call ? (void *)(tramp + i * 10) : fwd[i].target;
	void *p = 0;
	for (int i = 0; host_names[i] && !p; i++)
		if (host_libs[i])
			p = dlsym(host_libs[i], name);
	if (!p) {
		host_log("drgles: no host symbol %s\n", name);
		return 0;
	}
	if (nfwd == MAXFWD)
		host_die("drgles: too many imports\n");
	struct fwd *f = &fwd[nfwd++];
	f->name = name;	/* points into the importing library's string table */
	f->target = p;
	make_tramp(f);
	return call ? (void *)(tramp + (f - fwd) * 10) : p;
}

/* ---- the test (same images as gles-probe) ---- */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef unsigned int u32;
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_SURFACE_TYPE 0x3033
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_PBUFFER_BIT 0x0001
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872

#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

#define W 64
#define H 64
static unsigned char px[W * H * 4];
static void *gl;

#define G(ret, name, args) static ret(*name) args;
G(EGLDisplay, eglGetDisplay, (void *))
G(int, eglInitialize, (EGLDisplay, int *, int *))
G(int, eglChooseConfig, (EGLDisplay, const int *, EGLConfig *, int, int *))
G(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const int *))
G(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const int *))
G(int, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
G(int, eglGetError, (void))
G(int, eglTerminate, (EGLDisplay))
G(const char *, glGetString, (u32))
G(void, glGetIntegerv, (u32, int *))
G(void, glClearColor, (float, float, float, float))
G(void, glClear, (u32))
G(void, glFinish, (void))
G(void, glViewport, (int, int, int, int))
G(void, glReadPixels, (int, int, int, int, u32, u32, void *))
G(u32, glGetError, (void))
G(u32, glCreateShader, (u32))
G(void, glShaderSource, (u32, int, const char *const *, const int *))
G(void, glCompileShader, (u32))
G(void, glGetShaderiv, (u32, u32, int *))
G(void, glGetShaderInfoLog, (u32, int, int *, char *))
G(u32, glCreateProgram, (void))
G(void, glAttachShader, (u32, u32))
G(void, glBindAttribLocation, (u32, u32, const char *))
G(void, glLinkProgram, (u32))
G(void, glGetProgramiv, (u32, u32, int *))
G(void, glUseProgram, (u32))
G(int, glGetUniformLocation, (u32, const char *))
G(void, glUniform1i, (int, int))
G(void, glVertexAttribPointer, (u32, int, u32, unsigned char, int, const void *))
G(void, glEnableVertexAttribArray, (u32))
G(void, glDisableVertexAttribArray, (u32))
G(void, glDrawArrays, (u32, int, int))
G(void, glGenTextures, (int, u32 *))
G(void, glBindTexture, (u32, u32))
G(void, glActiveTexture, (u32))
G(void, glTexImage2D, (u32, int, int, int, int, int, u32, u32, const void *))
G(void, glTexParameteri, (u32, u32, int))
G(void, glGenFramebuffers, (int, u32 *))
G(void, glBindFramebuffer, (u32, u32))
G(void, glFramebufferTexture2D, (u32, u32, u32, u32, int))
G(u32, glCheckFramebufferStatus, (u32))
G(void, glFlush, (void))
G(void, glTexSubImage2D, (u32, int, int, int, int, int, u32, u32, const void *))

static int load_gl(struct drg_dso *egl, struct drg_dso *gles)
{
#define L(lib, name)                                                  \
	if (!(*(void **)&name = drg_elf_sym(lib, #name))) {           \
		host_log("FAIL: no symbol %s\n", #name);              \
		return 1;                                             \
	}
	L(egl, eglGetDisplay) L(egl, eglInitialize) L(egl, eglChooseConfig) L(egl, eglCreatePbufferSurface)
	L(egl, eglCreateContext) L(egl, eglMakeCurrent) L(egl, eglGetError) L(egl, eglTerminate)
	L(gles, glGetString) L(gles, glGetIntegerv) L(gles, glClearColor) L(gles, glClear) L(gles, glFinish)
	L(gles, glViewport) L(gles, glReadPixels) L(gles, glGetError) L(gles, glCreateShader)
	L(gles, glShaderSource) L(gles, glCompileShader) L(gles, glGetShaderiv) L(gles, glGetShaderInfoLog)
	L(gles, glCreateProgram) L(gles, glAttachShader) L(gles, glBindAttribLocation) L(gles, glLinkProgram)
	L(gles, glGetProgramiv) L(gles, glUseProgram) L(gles, glGetUniformLocation) L(gles, glUniform1i)
	L(gles, glVertexAttribPointer) L(gles, glEnableVertexAttribArray) L(gles, glDisableVertexAttribArray)
	L(gles, glDrawArrays) L(gles, glGenTextures) L(gles, glBindTexture) L(gles, glActiveTexture)
	L(gles, glTexImage2D) L(gles, glTexParameteri)
	L(gles, glGenFramebuffers) L(gles, glBindFramebuffer) L(gles, glFramebufferTexture2D)
	L(gles, glCheckFramebufferStatus) L(gles, glFlush) L(gles, glTexSubImage2D)
	(void)gl;
	return 0;
}

static u32 rgba_at(int x, int y)
{
	unsigned char *p = px + (y * W + x) * 4;
	return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int count(u32 v)
{
	int n = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			n += rgba_at(x, y) == v;
	return n;
}

static u32 program(const char *vsrc, const char *fsrc)
{
	const char *src[2] = { vsrc, fsrc };
	u32 type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER }, prog = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		u32 s = glCreateShader(type[i]);
		int ok = 0;
		glShaderSource(s, 1, &src[i], 0);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[512] = "";
			glGetShaderInfoLog(s, sizeof(log), 0, log);
			host_log("FAIL: compile: %s\n", log);
			return 0;
		}
		glAttachShader(prog, s);
	}
	glBindAttribLocation(prog, 0, "p");
	glBindAttribLocation(prog, 1, "t");
	glLinkProgram(prog);
	int ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		host_log("FAIL: link\n");
		return 0;
	}
	return prog;
}

static int check(const char *what, int ok)
{
	host_log("%s: %s\n", what, ok ? "OK" : "WRONG");
	return ok ? 0 : 1;
}

/*
 * -layout W H (with -capture FILE): how Nokia's driver lays out a W x H
 * RGBA8 texture in GPU memory, for rendering straight into an iOS display
 * IOSurface (drg/GPU-DRAW-PLAN.md). Every texel encodes its own position:
 * R = x & 0xff, G = x >> 8, B = y & 0xff, A = y >> 8.
 *   1. texture A uploaded with that pattern (glTexImage2D), drawn once so a
 *      GPU command captures it;
 *   2. texture B, no data, as an FBO colour attachment; a shader writes the
 *      same pattern from gl_FragCoord.
 * tools/cap-layout.py then finds both objects in the capture and reads off
 * tiling and row pitch.
 */
static int layout_w, layout_h;
static int gbuf_w __attribute__((unused)), gbuf_h __attribute__((unused));	/* TWRP only */
static struct drg_dso *egl_dso;

#ifndef __APPLE__
/*
 * -gbuf W H (TWRP, with -nokiagralloc): a W x H RGBA_8888 buffer from
 * Nokia's own gralloc (gralloc.sdm660.so) as an EGLImage render target, the
 * way Android hands a display buffer to the GPU. Logged: the buffer's
 * native_handle (fds and ints), its stride, which gralloc1 functions EGL
 * itself asks for and calls ("gralloc1 fn N" in the call counts), the KGSL
 * import (kgsl_bench.c), and whether the rendered position pattern is
 * linear at gralloc's stride, read back through gralloc's own lock. Also
 * whether a clear + draw write anything outside the w x h image (gralloc's
 * padding rows and columns hold a marker).
 *
 * gralloc1 function ids: Nokia's GrallocImpl::GetFunction (gralloc.sdm660.so
 * 0xcfcc: id - 1 indexes 25 cases; resolved through its GOT relocations):
 * 2 CreateBufferDescriptor, 3 Destroy, 4 SetConsumerUsage, 5 SetBufferDimensions,
 * 6 SetColorFormat, 7 SetProducerUsage, 13 GetBufferStride, 14 AllocateBuffers,
 * 16 ReleaseBuffer, 18 LockBuffer, 20 UnlockBuffer.
 * ANativeWindowBuffer: eglSubDriverAndroid.so 0x54a4 checks magic '_bfr' at
 * +0 and loads the handle from +0x60 for target EGL_NATIVE_BUFFER_ANDROID
 * (0x3140); the rest is Android's layout around those (base 56 bytes,
 * width/height/stride/format/usage at 56..72, layerCount 80, handle 96,
 * usage64 104, 168 bytes in all).
 */
struct anwb {
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
_Static_assert(__builtin_offsetof(struct anwb, handle) == 0x60, "ANativeWindowBuffer handle");
_Static_assert(sizeof(struct anwb) == 168, "ANativeWindowBuffer size");

static void anwb_ref(void *p)
{
	(void)p;
}

#define GR_PROD_CPU_READ_OFTEN	(1ULL << 2)
#define GR_PROD_GPU_RENDER	(1ULL << 10)
#define GR_CONS_GPU_TEXTURE	(1ULL << 8)

/*
 * -owngbuf W H FMT: the iOS arrangement on TWRP. Nokia's gralloc (through
 * TWRP's libhardware) only allocates the dma-buf; EGL sees OUR module
 * (bionic/gralloc.c) and our copy of the handle with the metadata fd set to
 * -1 (iOS has no metadata buffer). FMT 1 = RGBA_8888, 5 = BGRA_8888 (iOS
 * display surfaces are 'BGRA').
 */
static int gbuf_own, gbuf_fmt = 1, gbuf_keepmeta;	/* -gbufmeta: keep Nokia's metadata fd */
static int gbuf_craft;	/* -gbufcraft: the handle built by drgwin_handle as on iOS (rows = h), not a copy */
static uint64_t gbuf_usage = 1ULL << 8;	/* -gbufusage N: what our GetConsumerUsage answers */

static int gbuf_test(EGLDisplay d)
{
	int w = gbuf_w, h = gbuf_h;
	const struct hw_module *gm = gr_real_mod;
	int (*gopen)(const struct hw_module *, const char *, void **) = gr_real_open;
	if (gbuf_own) {
		int (*hgm)(const char *, const struct hw_module **) = dlsym(host_libs[6], "hw_get_module");
		gm = 0;
		if (hgm && hgm("gralloc", &gm) == 0 && gm)
			gopen = gm->methods->open;
	}
	if (!gm) {
		host_log("gbuf: needs -nokiagralloc or Nokia's module in TWRP's libhardware path\n");
		return 1;
	}
	void *dev = 0;
	int r = gopen(gm, "gralloc", &dev);
	if (r || !dev)
		return check("gbuf: own gralloc device", 0);
	void *(*get)(void *, int32_t) = ((struct gr1_dev *)dev)->get;
	int (*create)(void *, uint64_t *) = get(dev, 2);
	int (*cons)(void *, uint64_t, uint64_t) = get(dev, 4);
	int (*dims)(void *, uint64_t, uint32_t, uint32_t) = get(dev, 5);
	int (*fmt)(void *, uint64_t, int32_t) = get(dev, 6);
	int (*prod)(void *, uint64_t, uint64_t) = get(dev, 7);
	int (*stride)(void *, const void *, uint32_t *) = get(dev, 13);
	int (*alloc)(void *, uint32_t, const uint64_t *, const void **) = get(dev, 14);
	int (*lock)(void *, const void *, uint64_t, uint64_t, const void *, void **, int32_t) = get(dev, 18);
	int (*unlock)(void *, const void *, int32_t *) = get(dev, 20);
	uint64_t desc = 0;
	const void *hnd = 0;
	r = create(dev, &desc);
	r = r ? r : dims(dev, desc, (uint32_t)w, (uint32_t)h);
	r = r ? r : fmt(dev, desc, gbuf_fmt);	/* HAL_PIXEL_FORMAT_RGBA_8888 1, BGRA_8888 5 */
	r = r ? r : prod(dev, desc, GR_PROD_GPU_RENDER | GR_PROD_CPU_READ_OFTEN);
	r = r ? r : cons(dev, desc, GR_CONS_GPU_TEXTURE);
	r = r ? r : alloc(dev, 1, &desc, &hnd);
	uint32_t st = 0;
	if (!r)
		r = stride(dev, hnd, &st);
	host_log("gbuf: gralloc allocate rc %d handle %p stride %u px\n", r, hnd, st);
	if (r || !hnd)
		return check("gbuf: gralloc allocate", 0);
	const int32_t *nh = hnd;	/* native_handle: version, numFds, numInts, data[] */
	host_log("gbuf: native_handle version %d fds %d ints %d\n", nh[0], nh[1], nh[2]);
	for (int i = 0; i < nh[1] + nh[2] && i < 48; i += 8) {
		char line[128];
		int n = snprintf(line, sizeof(line), "gbuf:   data[%2d]", i);
		for (int k = i; k < i + 8 && k < nh[1] + nh[2]; k++)
			n += snprintf(line + n, sizeof(line) - n, " %08x", (uint32_t)nh[3 + k]);
		host_log("%s\n", line);
	}
	/* the buffer's memory, writable: padding markers now, our module's CPU address */
	const uint32_t marker = 0xa55aa55a;
	size_t blen = (size_t)(uint32_t)nh[3 + 10];	/* the handle's size field */
	int ah = st ? (int)(blen / ((size_t)st * 4)) : 0;	/* rows gralloc allocated */
	int perr = 0;
	uint32_t *pad = st ? host_mmap_fd(0, blen, HOST_PROT_R | HOST_PROT_W, 1 /* MAP_SHARED */, nh[3], 0, &perr) : 0;
	if (perr)
		pad = 0;
	const void *ehnd = hnd;	/* what EGL gets */
	static int32_t own_handle[3 + 64];
	if (gbuf_own) {
		int n = 3 + nh[1] + nh[2];
		if (n > (int)(sizeof(own_handle) / 4) || nh[1] < 2 || !pad)
			return check("gbuf: own handle", 0);
		memcpy(own_handle, nh, (size_t)n * 4);
		if (!gbuf_keepmeta)
			own_handle[3 + 1] = -1;	/* no metadata buffer */
		if (gbuf_craft)
			drgwin_handle(own_handle, nh[3], (uint32_t)w, (uint32_t)h, st, (uint32_t)h, gbuf_fmt, (uint32_t)blen);
		ehnd = own_handle;
		b_gralloc_trace = 1;
		/*
		 * CPU address 0, as Nokia's module answers Perform 10 for this buffer
		 * (TWRP 2026-11-02: out 0, base not mapped here). With a real address
		 * the driver never rendered into the buffer.
		 */
		r = b_gralloc_register(ehnd, 0, (uint32_t)w, (uint32_t)h, st, gbuf_fmt, gbuf_usage);
		host_log("gbuf: own module: handle %p (fd %d, metadata fd %d) registered rc %d, format %d, usage %#llx\n",
			 ehnd, own_handle[3], own_handle[4], r, gbuf_fmt, (unsigned long long)gbuf_usage);
	}

	static struct anwb b;
	b.magic = 0x5f626672;	/* '_bfr' */
	b.version = sizeof(b);
	b.inc = b.dec = anwb_ref;
	b.width = w, b.height = h, b.stride = (int32_t)st, b.format = gbuf_fmt;
	b.usage_deprecated = (int32_t)(GR_PROD_GPU_RENDER | GR_CONS_GPU_TEXTURE);
	b.layer_count = 1;
	b.handle = ehnd;
	b.usage = GR_PROD_GPU_RENDER | GR_CONS_GPU_TEXTURE;
	void *(*gpa)(const char *) = (void *(*)(const char *))drg_elf_sym(egl_dso, "eglGetProcAddress");
	void *(*mkimg)(EGLDisplay, void *, u32, void *, const int *) = gpa ? gpa("eglCreateImageKHR") : 0;
	void (*target)(u32, void *) = gpa ? gpa("glEGLImageTargetTexture2DOES") : 0;
	if (!mkimg || !target)
		return check("gbuf: eglCreateImageKHR / glEGLImageTargetTexture2DOES", 0);
	static const int ia[] = { 0x30D2 /* EGL_IMAGE_PRESERVED_KHR */, 1, EGL_NONE };
	void *img = mkimg(d, 0, 0x3140 /* EGL_NATIVE_BUFFER_ANDROID */, &b, ia);
	host_log("gbuf: eglCreateImageKHR -> %p, EGL error %x\n", img, eglGetError());
	if (!img)
		return check("gbuf: EGLImage", 0);
	u32 tex, fbo;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	target(GL_TEXTURE_2D, img);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	u32 fs = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	/*
	 * Padding check (drg/GPU-DRAW-PLAN.md): gralloc allocates aligned rows
	 * (2464 for 1125x2436) at st pixels per row, the iOS surface has only h
	 * rows. Fill everything outside the w x h image (rows h.. and columns
	 * w..st-1 of each row) with a marker through a writable mapping of the
	 * dma-buf before the draw; afterwards count what the GPU overwrote. The
	 * marker's top byte 0xa5 never occurs in the pattern (y >> 8 <= 9).
	 */
	long pad_words = 0, pad_kept_before = 0;
	if (pad) {
		for (int y = 0; y < ah; y++)
			for (int x = y < h ? w : 0; x < (int)st; x++)
				pad[(size_t)y * st + x] = marker, pad_words++;
		for (int y = 0; y < ah; y++)
			for (int x = y < h ? w : 0; x < (int)st; x++)
				pad_kept_before += pad[(size_t)y * st + x] == marker;
	}
	host_log("gbuf: padding: buffer %zu bytes = %u px x %d rows; %ld marker words outside %dx%d, %ld read back before the draw (map err %d)\n",
		 blen, st, ah, pad_words, w, h, pad_kept_before, perr);

	u32 pw = program("attribute vec4 p;\nvoid main() { gl_Position = p; }\n",
			 "precision highp float;\nvoid main() {\n"
			 "  vec2 c = floor(gl_FragCoord.xy);\n"
			 "  gl_FragColor = vec4(mod(c.x, 256.0), floor(c.x / 256.0), mod(c.y, 256.0), floor(c.y / 256.0)) / 255.0;\n"
			 "}\n");
	static const float quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
	glUseProgram(pw);
	glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, quad);
	glEnableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glViewport(0, 0, w, h);
	/*
	 * A clear first (CoreAnimation clears too): 0x33 bytes, neither marker nor
	 * pattern. The draw then leaves out the last GL row (viewport h - 1 rows;
	 * gl_FragCoord is unchanged), so that row must show the clear colour:
	 * proof that the clear ran and went through the same buffer.
	 */
	glClearColor(0.2f, 0.2f, 0.2f, 0.2f);
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(0, 0, w, h - 1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	host_log("gbuf: fbo status %x, GL error %x\n", fs, glGetError());

	/* what the draw did to the padding: kept marker, a pattern texel, or something else */
	long row_kept = 0, row_pat = 0, row_other = 0, col_kept = 0, col_pat = 0, col_other = 0;
	int last_row = -1;	/* last padding row with any word changed */
	if (pad) {
		for (int y = 0; y < ah; y++)
			for (int x = y < h ? w : 0; x < (int)st; x++) {
				uint32_t v = pad[(size_t)y * st + x];
				const unsigned char *t = (const unsigned char *)&v;
				int kept = v == marker, pat = !kept && t[3] <= 9;
				if (y >= h) {
					row_kept += kept, row_pat += pat, row_other += !kept && !pat;
					if (!kept)
						last_row = y;
				} else {
					col_kept += kept, col_pat += pat, col_other += !kept && !pat;
				}
			}
		for (int y = h; y < ah; y += ah - h > 1 ? (ah - h - 1) / 2 : 1) {
			const uint32_t *r = pad + (size_t)y * st;
			host_log("gbuf: padding row %d: x0 %08x x%d %08x x%u %08x\n", y, r[0], w - 1, r[w - 1], st - 1, r[st - 1]);
		}
		host_log("gbuf: padding rows %d..%d after the draw: kept %ld, pattern-like %ld, other %ld; last changed row %d\n",
			 h, ah - 1, row_kept, row_pat, row_other, last_row);
		host_log("gbuf: padding columns %d..%u (rows 0..%d) after the draw: kept %ld, pattern-like %ld, other %ld\n",
			 w, st - 1, h - 1, col_kept, col_pat, col_other);
		/* which columns and rows changed, and to what (first two distinct values) */
		int cfirst = -1, clast = -1, rfirst = -1, rlast = -1, rows_changed = 0;
		uint32_t vals[2] = { 0, 0 };
		long nval[3] = { 0, 0, 0 };
		for (int y = 0; y < h; y++) {
			int any = 0;
			for (int x = w; x < (int)st; x++) {
				uint32_t v = pad[(size_t)y * st + x];
				if (v == marker)
					continue;
				any = 1;
				if (cfirst < 0 || x < cfirst)
					cfirst = x;
				if (x > clast)
					clast = x;
				int k = nval[0] && v == vals[0] ? 0 : nval[1] && v == vals[1] ? 1 : !nval[0] ? 0 : !nval[1] ? 1 : 2;
				if (k < 2)
					vals[k] = v;
				nval[k]++;
			}
			if (any) {
				rows_changed++;
				if (rfirst < 0)
					rfirst = y;
				rlast = y;
			}
		}
		host_log("gbuf: padding columns changed: x %d..%d, %d rows between %d and %d; values %08x x%ld, %08x x%ld, others x%ld\n",
			 cfirst, clast, rows_changed, rfirst, rlast, vals[0], nval[0], vals[1], nval[1], nval[2]);
		for (int y = 0; y < h; y += y < 40 ? 1 : 256) {	/* changed columns: rows 0-39, then every 256th */
			char line[200];
			int n = snprintf(line, sizeof(line), "gbuf:   row %4d cols", y);
			for (int x = w; x < (int)st && n < (int)sizeof(line) - 4; x++)
				n += snprintf(line + n, sizeof(line) - n, "%c", pad[(size_t)y * st + x] == marker ? '.' : '#');
			host_log("%s\n", line);
		}
		if (!gbuf_own)	/* else our module still hands this address to EGL */
			host_munmap(pad, blen);
	}

	/* the pattern through gralloc's own CPU mapping, rows st pixels apart */
	void *cpu = 0;
	int lock_ok;
	const int32_t rect[4] = { 0, 0, w, h };	/* gralloc1_rect {left, top, width, height} (a 0 rect: rc 3) */
	r = lock(dev, hnd, GR_PROD_CPU_READ_OFTEN, 0, rect, &cpu, -1);
	lock_ok = r == 0;
	if (r) {
		/*
		 * lock refused the usage bits (rc 3, TWRP 2026-11-02), and the base in
		 * the handle is not mapped here (SIGSEGV): map the buffer's dma-buf
		 * (the handle's first fd) and read it between DMA_BUF_IOCTL_SYNC
		 * start/end (_IOW('b', 0, u64 flags), READ = 1, END = 4).
		 */
		size_t len = (size_t)(uint32_t)nh[3 + 10];	/* the handle's size field */
		int err = 0;
		cpu = host_mmap_fd(0, len, HOST_PROT_R, 1 /* MAP_SHARED */, nh[3], 0, &err);
		host_log("gbuf: lock rc %d; dma-buf fd %d mapped (%zu bytes) at %p, err %d\n", r, nh[3], len, cpu, err);
		if (err)
			cpu = 0;
		uint64_t sync = 1;
		host_log("gbuf: dma-buf sync start rc %ld\n", host_ioctl(nh[3], 0x40086200UL, &sync));
		r = 0;
	}
	long linear = 0, first_bad_x = -1, first_bad_y = -1, cleared = 0;
	if (!r && cpu) {
		const uint32_t *last = (const uint32_t *)cpu + (size_t)(h - 1) * st;	/* left out of the draw */
		for (int x = 0; x < w; x++)
			cleared += last[x] == 0x33333333;
		host_log("gbuf: row %d (cleared, not drawn): %ld of %d texels 33333333, x0 %08x\n", h - 1, cleared, w, last[0]);
		for (int y = 0; y < h - 1; y++)
			for (int x = 0; x < w; x++) {
				const unsigned char *t = (const unsigned char *)cpu + ((size_t)y * st + x) * 4;
				/* RGBA: R = x & 0xff, G = x >> 8, B = y & 0xff, A = y >> 8; BGRA keeps B, G, R, A in memory */
				int ok = gbuf_fmt == 5 ?
					 t[0] == (y & 0xff) && t[1] == (x >> 8) && t[2] == (x & 0xff) && t[3] == (y >> 8) :
					 t[0] == (x & 0xff) && t[1] == (x >> 8) && t[2] == (y & 0xff) && t[3] == (y >> 8);
				if (ok)
					linear++;
				else if (first_bad_x < 0)
					first_bad_x = x, first_bad_y = y;
			}
		const unsigned char *t0 = cpu;
		host_log("gbuf: lock %p, first texel %02x%02x%02x%02x, linear texels %ld of %d (first other (%ld,%ld))\n",
			 cpu, t0[0], t0[1], t0[2], t0[3], linear, w * (h - 1), first_bad_x, first_bad_y);
		int32_t fence = -1;
		if (lock_ok)
			unlock(dev, hnd, &fence);
	} else {
		host_log("gbuf: lock rc %d\n", r);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	int ok = check("gbuf: EGLImage render target linear at gralloc's stride", fs == GL_FRAMEBUFFER_COMPLETE &&
		       linear == (long)w * (h - 1) && cleared == w);
	if (gbuf_own && pad) {
		/*
		 * The other direction (CoreAnimation samples IOSurfaces apps drew
		 * on the CPU): the CPU writes a pattern into the buffer, the GPU
		 * samples the EGLImage texture (NEAREST, texel centres) into a
		 * 256x256 ordinary FBO, glReadPixels compares. Memory row 0 is
		 * expected at t = 0 (as for rendering); the flipped match is counted
		 * too, to tell orientation from failure.
		 */
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				unsigned char *t = (unsigned char *)(pad + (size_t)y * st + x);
				t[0] = (unsigned char)(x * 7 + y), t[1] = (unsigned char)(x + 3 * y), t[2] = (unsigned char)(x >> 3);
				t[3] = 0xff;
			}
		__asm__ volatile("dsb sy" ::: "memory");
		enum { S = 256 };
		u32 t2, f2;
		glGenTextures(1, &t2);
		glBindTexture(GL_TEXTURE_2D, t2);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glGenFramebuffers(1, &f2);
		glBindFramebuffer(GL_FRAMEBUFFER, f2);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t2, 0);
		u32 fs2 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		char fsrc[256];
		snprintf(fsrc, sizeof(fsrc), "precision highp float;\nuniform lowp sampler2D tex;\nvoid main() {\n"
			 "  gl_FragColor = texture2D(tex, (floor(gl_FragCoord.xy) + 0.5) / vec2(%d.0, %d.0));\n}\n", w, h);
		u32 ps = program("attribute vec4 p;\nvoid main() { gl_Position = p; }\n", fsrc);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);	/* the EGLImage */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glUseProgram(ps);
		glUniform1i(glGetUniformLocation(ps, "tex"), 0);
		glViewport(0, 0, S, S);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish();
		unsigned char *got = host_alloc((size_t)S * S * 4);
		glReadPixels(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, got);
		long same = 0, flipped = 0;
		for (int y = 0; y < S; y++)
			for (int x = 0; x < S; x++) {
				const unsigned char *g = got + ((size_t)y * S + x) * 4;
				for (int k = 0; k < 2; k++) {
					int my = k ? h - 1 - y : y;
					const unsigned char *m = (const unsigned char *)(pad + (size_t)my * st + x);
					/* sampled RGBA from memory: RGBA_8888 as is, BGRA_8888 swaps R and B */
					int r = gbuf_fmt == 5 ? m[2] : m[0], b = gbuf_fmt == 5 ? m[0] : m[2];
					int eq = g[0] == r && g[1] == m[1] && g[2] == b && g[3] == m[3];
					if (k)
						flipped += eq;
					else
						same += eq;
				}
			}
		host_log("gbuf: sample: fbo %x, GL error %x, texels matching memory row y: %ld, row h-1-y: %ld of %d; "
			 "(0,0) got %02x%02x%02x%02x\n", fs2, glGetError(), same, flipped, S * S, got[0], got[1], got[2], got[3]);
		host_free(got);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		ok += check("gbuf: GPU samples CPU-written EGLImage texels (memory row 0 at t = 0)", same == S * S);
	}
	/*
	 * TWRP 2026-11-02: the draw stays inside w x h; the clear also writes
	 * columns w..align16(w)-1 (1125..1135) of every row, never a row >= h.
	 * What the iOS display surface (h rows of st pixels) needs is the latter.
	 */
	return ok + check("gbuf: GPU wrote no row past h - 1 (inside an h-row surface)", pad && row_kept == (long)(ah - h) * st);
}
#endif

static int layout_test(void)
{
	int w = layout_w, h = layout_h;
	unsigned char *pat = host_alloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			unsigned char *t = pat + ((size_t)y * w + x) * 4;
			t[0] = x & 0xff, t[1] = x >> 8, t[2] = y & 0xff, t[3] = y >> 8;
		}
	u32 tex[2], fbo;
	glGenTextures(2, tex);
	glBindTexture(GL_TEXTURE_2D, tex[0]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pat);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	/* a draw that samples texture A, so its upload is in GPU memory at a captured command */
	static const float quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
	static const float uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	u32 ps = program("attribute vec4 p;\nattribute vec2 t;\nvarying highp vec2 uv;\n"
			 "void main() { gl_Position = p; uv = t; }\n",
			 "varying highp vec2 uv;\nuniform lowp sampler2D tex;\n"
			 "void main() { gl_FragColor = texture2D(tex, uv); }\n");
	u32 pw = program("attribute vec4 p;\nvoid main() { gl_Position = p; }\n",
			 "precision highp float;\nvoid main() {\n"
			 "  vec2 c = floor(gl_FragCoord.xy);\n"
			 "  gl_FragColor = vec4(mod(c.x, 256.0), floor(c.x / 256.0), mod(c.y, 256.0), floor(c.y / 256.0)) / 255.0;\n"
			 "}\n");
	if (!ps || !pw)
		return 1;
	glUseProgram(ps);
	glUniform1i(glGetUniformLocation(ps, "tex"), 0);
	glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 0, uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glViewport(0, 0, W, H);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();

	glBindTexture(GL_TEXTURE_2D, tex[1]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
	u32 st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glUseProgram(pw);
	glDisableVertexAttribArray(1);
	glViewport(0, 0, w, h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	/* a few pixels back through GL, as the reference for the script */
	unsigned char got[4 * 4];
	static const int at[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
	int ok = st == GL_FRAMEBUFFER_COMPLETE;
	for (int i = 0; i < 3; i++)
		glReadPixels(at[i][0], at[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got + 4 * i);
	glReadPixels(w - 1, h - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got + 12);
	ok = ok && got[0] == 0 && got[4] == 1 && got[10] == 1 && got[12] == ((w - 1) & 0xff) &&
	     got[13] == (w - 1) >> 8 && got[14] == ((h - 1) & 0xff) && got[15] == (h - 1) >> 8;
	host_log("layout %dx%d: fbo status %x, readback %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
		 "%02x%02x%02x%02x, GL error %x\n", w, h, st, got[0], got[1], got[2], got[3], got[4], got[5], got[6],
		 got[7], got[8], got[9], got[10], got[11], got[12], got[13], got[14], got[15], glGetError());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	host_free(pat);
	return check("layout render target pattern", ok);
}

/*
 * -frames (both hosts; part 1 of the iOS -selftest): FRAMES frames back to
 * back, each a texture upload, a clear, a textured quad and glFlush, no
 * wait; every 50th frame two pixels are read back (that read waits). Run on
 * TWRP's KGSL it checks the test itself.
 */
static int frames_test(u32 *fbo_out)
{
	int fails = 0;
	enum { S = 256, T = 64, FRAMES = 200 };
	static const float quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
	static const float uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	u32 ptex = program("attribute vec4 p;\nattribute vec2 t;\nvarying highp vec2 uv;\n"
			   "void main() { gl_Position = p; uv = t; }\n",
			   "varying highp vec2 uv;\nuniform lowp sampler2D tex;\n"
			   "void main() { gl_FragColor = texture2D(tex, uv); }\n");
	if (!ptex)
		return 1;
	/* 1. FRAMES frames back to back: a texture upload, a clear, a textured quad, glFlush; every 50th read back */
	u32 tex, ftex, fbo;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, T, T, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenTextures(1, &ftex);
	glBindTexture(GL_TEXTURE_2D, ftex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ftex, 0);
	glUseProgram(ptex);
	glUniform1i(glGetUniformLocation(ptex, "tex"), 0);
	glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 0, uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glViewport(0, 0, S, S);
	static uint32_t texels[T * T];
	int wrong = 0, checked = 0;
	for (int f = 0; f < FRAMES; f++) {
		uint32_t v = 0xff000000u | (uint32_t)(f * 0x010305 + 0x203040) % 0x1000000;
		for (int i = 0; i < T * T; i++)
			texels[i] = v;
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, T, T, GL_RGBA, GL_UNSIGNED_BYTE, texels);
		glClearColor(1, 0, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFlush();
		if (f % 50 == 49) {
			uint32_t got[2] = { 0, 0 };
			glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &got[0]);
			glReadPixels(S / 2, S / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &got[1]);
			checked++;
			if (got[0] != v || got[1] != v) {
				wrong++;
				host_log("selftest: frame %d: read %08x %08x, want %08x\n", f, got[0], got[1], v);
			}
		}
	}
	fails += check("selftest: 200 frames back to back, texture upload each frame", !wrong && checked == FRAMES / 50);

	*fbo_out = fbo;
	return fails;
}

static int frames_on;

#ifdef __APPLE__
/*
 * -selftest FILE (boot job com.drg.gpuselftest, drg/GPU-DRAW-PLAN.md): which
 * DrgGPU submission framing survives work submitted back to back. Each mode
 * runs in its own process (a fresh connection and context, as an app
 * would): the stage tests above (their readbacks wait), then stress_test().
 * FILE gets "mode N" for the first passing mode in the order closest to
 * Linux (KGSL framing first), or "none"; libdrgeagl.dylib in backboardd
 * waits for it and uses that framing, or stays off.
 */
typedef int pid_t;
int posix_spawn(pid_t *, const char *, const void *, const void *, char *const[], char *const[]);
pid_t waitpid(pid_t, int *, int);
int open(const char *, int, ...);
int close(int);
long write(int, const void *, unsigned long);

typedef const void *CFTypeRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFString *CFStringRef;
typedef struct __IOSurface *IOSurfaceRef;
extern const char kCFTypeDictionaryKeyCallBacks[], kCFTypeDictionaryValueCallBacks[];
extern CFStringRef kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
	kIOSurfacePixelFormat;
CFMutableDictionaryRef CFDictionaryCreateMutable(void *, long, const void *, const void *);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
CFTypeRef CFNumberCreate(void *, long, const void *);
void CFRelease(CFTypeRef);
IOSurfaceRef IOSurfaceCreate(CFDictionaryRef);
uint32_t IOSurfaceGetID(IOSurfaceRef);
int IOSurfaceLock(IOSurfaceRef, uint32_t, uint32_t *);
int IOSurfaceUnlock(IOSurfaceRef, uint32_t, uint32_t *);
void *IOSurfaceGetBaseAddress(IOSurfaceRef);
unsigned long IOSurfaceGetBytesPerRow(IOSurfaceRef);
unsigned long IOSurfaceGetAllocSize(IOSurfaceRef);

static IOSurfaceRef make_surface(int w, int h, int bpr)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(0, 0, kCFTypeDictionaryKeyCallBacks,
							     kCFTypeDictionaryValueCallBacks);
	int32_t v[5] = { w, h, 4, bpr, 0x42475241 /* 'BGRA', as backboardd's display surfaces */ };
	CFStringRef k[5] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
			     kIOSurfacePixelFormat };
	for (int i = 0; i < 5; i++) {
		CFTypeRef n = CFNumberCreate(0, 3 /* kCFNumberSInt32Type */, &v[i]);
		CFDictionarySetValue(d, k[i], n);
		CFRelease(n);
	}
	IOSurfaceRef s = IOSurfaceCreate(d);
	CFRelease(d);
	return s;
}

/* an IOSurface as an EGLImage texture, the way libdrgeagl does it (drgwin.c, our gralloc, KGSL import) */
static u32 surface_texture(EGLDisplay d, IOSurfaceRef s, int w, int h)
{
	static int32_t handles[2][DRGWIN_HANDLE_INTS];
	static struct drgwin_anwb bufs[2];
	static int n;
	if (n == 2)
		return 0;
	int32_t *hd = handles[n];
	struct drgwin_anwb *b = &bufs[n++];
	uint32_t bpr = (uint32_t)IOSurfaceGetBytesPerRow(s), size = (uint32_t)IOSurfaceGetAllocSize(s);
	int fd = kgsl_ios_surface_fd(IOSurfaceGetID(s));
	if (fd < 0)
		return 0;
	drgwin_handle(hd, fd, (uint32_t)w, (uint32_t)h, bpr / 4, (uint32_t)h, DRGWIN_FMT_BGRA_8888, size);
	drgwin_anwb(b, hd, (uint32_t)w, (uint32_t)h, bpr / 4, DRGWIN_FMT_BGRA_8888,
		    DRGWIN_USAGE_GPU_RENDER | DRGWIN_USAGE_GPU_TEXTURE);
	b_gralloc_register(hd, 0, (uint32_t)w, (uint32_t)h, bpr / 4, DRGWIN_FMT_BGRA_8888, DRGWIN_USAGE_GPU_TEXTURE);
	void *(*gpa)(const char *) = (void *(*)(const char *))drg_elf_sym(egl_dso, "eglGetProcAddress");
	void *(*mkimg)(EGLDisplay, void *, u32, void *, const int *) = gpa ? gpa("eglCreateImageKHR") : 0;
	void (*target)(u32, void *) = gpa ? gpa("glEGLImageTargetTexture2DOES") : 0;
	static const int ia[] = { 0x30D2 /* EGL_IMAGE_PRESERVED_KHR */, 1, EGL_NONE };
	void *img = mkimg && target ? mkimg(d, 0, 0x3140 /* EGL_NATIVE_BUFFER_ANDROID */, b, ia) : 0;
	host_log("selftest: IOSurface %u %dx%d bpr %u size %u -> EGLImage %p (EGL error %x)\n", IOSurfaceGetID(s), w, h,
		 bpr, size, img, eglGetError());
	if (!img)
		return 0;
	u32 tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	target(GL_TEXTURE_2D, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return tex;
}

static int stress_test(EGLDisplay d)
{
	int fails = 0;
	enum { S = 256, SF = 60, SW = 1125, SH = 2436, SBPR = 4608 };
	static const float quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
	static const float uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	u32 ptex = program("attribute vec4 p;\nattribute vec2 t;\nvarying highp vec2 uv;\n"
			   "void main() { gl_Position = p; uv = t; }\n",
			   "varying highp vec2 uv;\nuniform lowp sampler2D tex;\n"
			   "void main() { gl_FragColor = texture2D(tex, uv); }\n");
	u32 ppat = program("attribute vec4 p;\nvoid main() { gl_Position = p; }\n",
			   "precision highp float;\nuniform int k;\nvoid main() {\n  vec2 c = floor(gl_FragCoord.xy);\n"
			   "  gl_FragColor = vec4(mod(c.x, 256.0), floor(c.x / 256.0), mod(c.y + float(k), 256.0),"
			   " floor(c.y / 256.0)) / 255.0;\n}\n");
	if (!ptex || !ppat)
		return 1;

	u32 fbo = 0;
	fails += frames_test(&fbo);
	/* 2. a 1125x2436 BGRA IOSurface as render target (backboardd's display size), SF frames, CPU check */
	IOSurfaceRef ds = make_surface(SW, SH, SBPR);
	u32 dtex = ds ? surface_texture(d, ds, SW, SH) : 0;
	u32 dfbo = 0, st = 0;
	if (dtex) {
		glGenFramebuffers(1, &dfbo);
		glBindFramebuffer(GL_FRAMEBUFFER, dfbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dtex, 0);
		st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glUseProgram(ppat);
		glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, quad);
		glEnableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
		glViewport(0, 0, SW, SH);
		int kl = glGetUniformLocation(ppat, "k");
		for (int f = 0; f < SF; f++) {
			glClearColor(0.2f, 0.2f, 0.2f, 0.2f);
			glClear(GL_COLOR_BUFFER_BIT);
			glUniform1i(kl, f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glFlush();
		}
		glFinish();
	}
	long good = 0;
	if (dtex && !IOSurfaceLock(ds, 1 /* read-only */, 0)) {
		const uint8_t *m = IOSurfaceGetBaseAddress(ds);
		for (int y = 0; y < SH; y++)
			for (int x = 0; x < SW; x++) {
				const uint8_t *t = m + (size_t)y * SBPR + (size_t)x * 4;
				good += t[0] == ((y + SF - 1) & 0xff) && t[1] == (x >> 8) && t[2] == (x & 0xff) && t[3] == (y >> 8);
			}
		IOSurfaceUnlock(ds, 1, 0);
	}
	host_log("selftest: IOSurface render target: fbo %x, %ld of %d texels as frame %d drew them\n", st, good,
		 SW * SH, SF - 1);
	fails += check("selftest: IOSurface render target, frames back to back", good == (long)SW * SH);

	/* 3. the CPU writes an IOSurface, the GPU samples it (CoreAnimation's app layers) */
	IOSurfaceRef cs = make_surface(S, S, S * 4);
	u32 ctex = cs ? surface_texture(d, cs, S, S) : 0;
	long same = 0;
	if (ctex && !IOSurfaceLock(cs, 0, 0)) {
		uint8_t *m = IOSurfaceGetBaseAddress(cs);
		for (int y = 0; y < S; y++)
			for (int x = 0; x < S; x++) {
				uint8_t *t = m + (size_t)y * S * 4 + (size_t)x * 4;
				t[0] = (uint8_t)(x * 7 + y), t[1] = (uint8_t)(x + 3 * y), t[2] = (uint8_t)(x >> 3), t[3] = 0xff;
			}
		IOSurfaceUnlock(cs, 0, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glUseProgram(ptex);
		glUniform1i(glGetUniformLocation(ptex, "tex"), 0);
		glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, quad);
		glVertexAttribPointer(1, 2, GL_FLOAT, 0, 0, uv);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, ctex);
		glViewport(0, 0, S, S);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish();
		uint8_t *got = host_alloc((size_t)S * S * 4);
		glReadPixels(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, got);
		for (int y = 0; y < S; y++)
			for (int x = 0; x < S; x++) {
				const uint8_t *g = got + ((size_t)y * S + x) * 4, *t = m + (size_t)y * S * 4 + (size_t)x * 4;
				same += g[0] == t[2] && g[1] == t[1] && g[2] == t[0] && g[3] == t[3];	/* BGRA memory */
			}
		host_free(got);
	}
	host_log("selftest: sampled IOSurface: %ld of %d texels\n", same, S * S);
	fails += check("selftest: GPU samples a CPU-written IOSurface", same == (long)S * S);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (kgsl_ios_faulted())
		fails += check("selftest: no GPU fault", 0);
	return fails;
}

static int selftest_child = -1;

/* the parent: one child per mode, the verdict into `out` */
static int selftest(const char *self, const char *out, char **envp)
{
	static const int order[4] = { 0, 1, 3, 2 };	/* runs: the old framing first, as the control */
	static const int prefer[4] = { 1, 3, 2, 0 };	/* the verdict: closest to Linux first */
	int pass[4] = { 0, 0, 0, 0 };
	for (int i = 0; i < 4; i++) {
		char m[2] = { (char)('0' + order[i]), 0 };
		char *argv[] = { (char *)self, "-selftest-child", m, "/usr/local/drgles/adreno", 0 };
		pid_t pid = 0;
		int st = 0;
		host_log("selftest: mode %d (kgsl preempt %d, wfi %d) starting\n", order[i], order[i] & 1, order[i] >> 1);
		if (posix_spawn(&pid, self, 0, 0, argv, envp) != 0 || waitpid(pid, &st, 0) != pid) {
			host_log("selftest: mode %d: could not run\n", order[i]);
			continue;
		}
		pass[order[i]] = (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0;
		host_log("selftest: mode %d: %s (status 0x%x)\n", order[i], pass[order[i]] ? "PASS" : "FAIL", st);
	}
	char verdict[16] = "none\n";
	for (int i = 0; i < 4; i++)
		if (pass[prefer[i]]) {
			snprintf(verdict, sizeof(verdict), "mode %d\n", prefer[i]);
			break;
		}
	int fd = open(out, 0x0601 /* O_WRONLY | O_CREAT | O_TRUNC */, 0644);
	if (fd >= 0) {
		write(fd, verdict, strlen(verdict));
		close(fd);
	}
	host_log("selftest: passed %d %d %d %d (modes 0-3); verdict %s", pass[0], pass[1], pass[2], pass[3], verdict);
	return 0;
}
#endif

static int run(void)
{
	struct drg_dso *egl = drg_elf_open("libEGL_adreno.so");
	struct drg_dso *gles = egl ? drg_elf_open("libGLESv2_adreno.so") : 0;
	if (!egl || !gles) {
		host_log("FAIL: load: %s\n", drg_elf_error());
		return 1;
	}
	host_log("loaded; %u TPIDR_EL0 reads rewritten\n", drg_elf_tp_rewrites());
	egl_dso = egl;
	if (load_gl(egl, gles))
		return 1;

	int fails = 0, major = 0, minor = 0, n = 0;
	EGLConfig cfg;
	EGLDisplay d = eglGetDisplay(0);
	if (!d || !eglInitialize(d, &major, &minor)) {
		host_log("FAIL: eglInitialize, error %x\n", eglGetError());
		return 1;
	}
	static const int ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
				  EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
	static const int sa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	static const int xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };	/* iOS's GLESContext needs ES3 */
	if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) {
		host_log("FAIL: eglChooseConfig %x\n", eglGetError());
		return 1;
	}
	EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
	EGLContext c = eglCreateContext(d, cfg, 0, xa);
	if (!s || !c || !eglMakeCurrent(d, s, s, c)) {
		host_log("FAIL: context, error %x\n", eglGetError());
		return 1;
	}
	int units = 0;
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
	host_log("EGL %d.%d, GL \"%s\" \"%s\", %d texture units\n", major, minor, glGetString(GL_RENDERER),
		 glGetString(GL_VERSION), units);
	fails += check("texture units >= 3 (iOS GLESContext)", units >= 3);

	glViewport(0, 0, W, H);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	kgsl_trace_point("after glFinish");
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	kgsl_trace_point("after read");
	fails += check("clear: 4096 red", count(0xff0000ff) == W * H);

	u32 p1 = program("attribute vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
			 "precision mediump float;\nvoid main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
	if (!p1)
		return fails + 1;
	static const float tri[] = { -1, -1, 1, -1, -1, 1 };
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(p1);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, tri);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	kgsl_trace_point("after glFinish");
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	kgsl_trace_point("after read");
	int g = count(0x00ff00ff), b = count(0x0000ffff);
	host_log("triangle: %d green, %d blue\n", g, b);
	fails += check("triangle", (g == 2016 || g == 2080) && g + b == W * H);

	/* textured quad, CoreAnimation style (GLSL ES 1.00, texture2D,
	 * highp position, lowp colour): a 2x2 texture stretched over the
	 * surface with NEAREST: four 32x32 blocks */
	u32 p2 = program("attribute highp vec4 p;\nattribute highp vec2 t;\nvarying highp vec2 uv;\n"
			 "void main() { gl_Position = p; uv = t; }\n",
			 "varying highp vec2 uv;\nuniform lowp sampler2D tex;\n"
			 "void main() { gl_FragColor = texture2D(tex, uv); }\n");
	if (!p2)
		return fails + 1;
	static const unsigned char texel[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
	static const float quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
	static const float uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	u32 tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUseProgram(p2);
	glUniform1i(glGetUniformLocation(p2, "tex"), 0);
	glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 0, uv);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	kgsl_trace_point("after glFinish");
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	kgsl_trace_point("after read");
	/* texel rows: row 0 (bottom) red, green; row 1 blue, white */
	int q = rgba_at(5, 5) == 0xff0000ff && rgba_at(60, 5) == 0x00ff00ff && rgba_at(5, 60) == 0x0000ffff &&
		rgba_at(60, 60) == 0xffffffff;
	host_log("texture: %d red %d green %d blue %d white\n", count(0xff0000ff), count(0x00ff00ff),
		 count(0x0000ffff), count(0xffffffff));
	fails += check("texture quadrants", q && count(0xff0000ff) == 1024 && count(0x00ff00ff) == 1024 &&
					       count(0x0000ffff) == 1024 && count(0xffffffff) == 1024);
	host_log("GL error %x\n", glGetError());
	if (layout_w)
		fails += layout_test();
	if (frames_on) {
		u32 fbo;
		fails += frames_test(&fbo);
	}
#ifndef __APPLE__
	if (gbuf_w)
		fails += gbuf_test(d);
#else
	if (selftest_child >= 0)
		fails += stress_test(d);
#endif
	eglTerminate(d);
	return fails;
}

static int main_(int argc, char **argv, char **envp)
{
	(void)envp;
	static const char *dirs[8] = { "/tmp/adreno" };
	int nd = 0;
	for (int i = 1; i < argc && nd < 7; i++) {
#ifndef __APPLE__	/* both need TWRP's own libraries */
		if (!strcmp(argv[i], "-stage1")) {
			stage = 1;
			continue;
		}
		if (!strcmp(argv[i], "-nokiagralloc")) {
			nokia_gralloc = 1;
			continue;
		}
#endif
		if (!strcmp(argv[i], "-delay") && i + 1 < argc) {	/* seconds before touching the GPU (boot #37) */
			int s = 0;
			for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++)
				s = s * 10 + (*p - '0');
			host_log("delay %d s\n", s);
			host_sleep_ns((int64_t)s * 1000000000);
			continue;
		}
#ifdef __APPLE__
		if (!strcmp(argv[i], "-selftest") && i + 1 < argc)	/* the boot job: children do the work */
			return selftest(argv[0], argv[i + 1], envp);
		if (!strcmp(argv[i], "-selftest-child") && i + 1 < argc) {
			selftest_child = argv[++i][0] - '0';
			if (selftest_child < 0 || selftest_child > 3)
				return 2;
			kgsl_ios_framing((uint32_t)selftest_child);
			host_log("selftest child: framing %d\n", selftest_child);
			continue;
		}
		if (!strcmp(argv[i], "-dsb") || !strcmp(argv[i], "-deferfree")) {	/* boot #42 */
			static int dsb, deferfree;
			dsb |= !strcmp(argv[i], "-dsb");
			deferfree |= !strcmp(argv[i], "-deferfree");
			kgsl_ios_options(dsb, deferfree);
			host_log("isolation: dsb %d deferfree %d\n", dsb, deferfree);
			continue;
		}
#endif
#ifndef __APPLE__
		if (!strcmp(argv[i], "-gbuf") && i + 2 < argc) {	/* drg/GPU-DRAW-PLAN.md */
			int v[2] = { 0, 0 };
			for (int k = 0; k < 2; k++)
				for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++)
					v[k] = v[k] * 10 + (*p - '0');
			gbuf_w = v[0], gbuf_h = v[1];
			host_log("gbuf test %dx%d\n", gbuf_w, gbuf_h);
			continue;
		}
		if (!strcmp(argv[i], "-owngbuf") && i + 3 < argc) {	/* our gralloc module, Nokia's memory */
			int v[3] = { 0, 0, 0 };
			for (int k = 0; k < 3; k++)
				for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++)
					v[k] = v[k] * 10 + (*p - '0');
			gbuf_w = v[0], gbuf_h = v[1], gbuf_fmt = v[2], gbuf_own = 1;
			host_log("own gbuf test %dx%d format %d\n", gbuf_w, gbuf_h, gbuf_fmt);
			continue;
		}
		if (!strcmp(argv[i], "-gbufmeta")) {
			gbuf_keepmeta = 1;
			continue;
		}
		if (!strcmp(argv[i], "-gbufcraft")) {
			gbuf_craft = 1;
			continue;
		}
		if (!strcmp(argv[i], "-gbufusage") && i + 1 < argc) {
			gbuf_usage = 0;
			for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++)
				gbuf_usage = gbuf_usage * 10 + (uint64_t)(*p - '0');
			continue;
		}
#endif
		if (!strcmp(argv[i], "-layout") && i + 2 < argc) {	/* drg/GPU-DRAW-PLAN.md */
			int v[2] = { 0, 0 };
			for (int k = 0; k < 2; k++)
				for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++)
					v[k] = v[k] * 10 + (*p - '0');
			layout_w = v[0], layout_h = v[1];
			host_log("layout test %dx%d\n", layout_w, layout_h);
			continue;
		}
		if (!strcmp(argv[i], "-trace")) {
			kgsl_trace();
			continue;
		}
		if (!strcmp(argv[i], "-frames")) {
			frames_on = 1;
			continue;
		}
		if (!strcmp(argv[i], "-capture-nowait") && i + 1 < argc) {	/* boot #40: the race test */
			kgsl_capture_nowait();
			int rc = kgsl_capture_start(argv[++i]);
			host_log("capture (no wait) to %s: %d\n", argv[i], rc);
			continue;
		}
		if (!strcmp(argv[i], "-capture") && i + 1 < argc) {
			int rc = kgsl_capture_start(argv[++i]);
			host_log("capture to %s: %d\n", argv[i], rc);
			continue;
		}
		dirs[nd++] = argv[i];
		dirs[nd] = 0;
	}
	if (nokia_gralloc)
		bionic_bench_hook = bench_hook;
	static const char *const sys[] = { "libc.so", "libm.so", "libdl.so", "liblog.so", "libcutils.so", "libutils.so",
					   "libz.so", "libc++.so", "libhardware.so", "libsync.so", 0 };
#ifndef __APPLE__	/* iOS: stage 2 only, nothing to forward to */
	for (int i = 0; host_names[i]; i++)
		host_libs[i] = dlopen(host_names[i], RTLD_NOW);
#endif
	struct drg_elf_config c = { dirs, sys, resolver, 0, 1 };
	drg_elf_setup(&c);
	int fails = run();
	host_log("RESULT: %s (%d failures), stage %d\n", fails ? "FAIL" : "PASS", fails, stage);
	kgsl_report();
	void b_sys_report(void);
	b_sys_report();
	unsigned fb_called = 0;
	for (int i = 0; i < nfwd; i++)
		if (fwd[i].count) {
			host_log("calls %llu %s%s\n", (unsigned long long)fwd[i].count, fwd[i].name,
				 stage == 2 ? " FALLBACK" : "");
			fb_called++;
		}
	if (stage == 2)
		host_log("fallbacks called: %u\n", fb_called);
	host_log("imports forwarded: %d, called: ", nfwd);
	int used = 0;
	for (int i = 0; i < nfwd; i++)
		used += fwd[i].count != 0;
	host_log("%d\n", used);
	return fails ? 1 : 0;
}

#ifdef __APPLE__
/* iOS: dyld and libSystem start the process */
int main(int argc, char **argv, char **envp)
{
	return main_(argc, argv, envp);
}
#else
__attribute__((used)) static void start_c(void *raw_args)
{
	static void *structors[6];
	__libc_init(raw_args, 0, main_, structors);
}
__asm__(".text\n.globl _start\n.type _start,%function\n_start:\n"
	"mov x29, #0\nmov x30, #0\nmov x0, sp\nb start_c\n");
#endif
