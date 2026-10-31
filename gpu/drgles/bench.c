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

static void *gr_get(void *dev, int32_t desc)
{
	void *f = gr_real_get(dev, desc);
	host_log("gralloc1 getFunction(%d) -> %s\n", desc, f ? "fn" : "null");
	if (!f)
		return 0;
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

static int run(void)
{
	struct drg_dso *egl = drg_elf_open("libEGL_adreno.so");
	struct drg_dso *gles = egl ? drg_elf_open("libGLESv2_adreno.so") : 0;
	if (!egl || !gles) {
		host_log("FAIL: load: %s\n", drg_elf_error());
		return 1;
	}
	host_log("loaded; %u TPIDR_EL0 reads rewritten\n", drg_elf_tp_rewrites());
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
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
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
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
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
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	/* texel rows: row 0 (bottom) red, green; row 1 blue, white */
	int q = rgba_at(5, 5) == 0xff0000ff && rgba_at(60, 5) == 0x00ff00ff && rgba_at(5, 60) == 0x0000ffff &&
		rgba_at(60, 60) == 0xffffffff;
	host_log("texture: %d red %d green %d blue %d white\n", count(0xff0000ff), count(0x00ff00ff),
		 count(0x0000ffff), count(0xffffffff));
	fails += check("texture quadrants", q && count(0xff0000ff) == 1024 && count(0x00ff00ff) == 1024 &&
					       count(0x0000ffff) == 1024 && count(0xffffffff) == 1024);
	host_log("GL error %x\n", glGetError());
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
