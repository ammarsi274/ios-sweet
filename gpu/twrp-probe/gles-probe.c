/*
 * gles-probe: run the Nokia 6.1 Plus's own Adreno OpenGL ES driver
 * (vendor_b: libEGL_adreno.so, libGLESv2_adreno.so, libgsl.so) under TWRP's
 * Linux kernel, draw known images, check them pixel by pixel, and record
 * every ioctl and mmap the driver makes on the KGSL device.
 *
 * The recording is the reference for the iOS side: the KGSL emulation must
 * answer the same calls the same way. Our own code; no NDK. Built against
 * TWRP's own bionic (/sbin/libc.so, /sbin/libdl.so) and started by
 * /sbin/linker64. Only declarations this file uses are spelled out below.
 *
 * Trace file format (little endian), one record per call:
 *   u32 magic 'KTRC', u32 kind (1 ioctl, 2 mmap, 3 munmap, 4 open), s32 fd,
 *   u32 cmd, u64 a, u64 b, s64 ret, s32 err, u32 len, then len bytes before
 *   the call and len bytes after it (ioctl argument; 0 for the others).
 * Kinds 5 and 6 carry len bytes once (no before/after pair):
 *   5 buffer behind an ioctl pointer: cmd = ioctl nr << 16 | tag, a = address
 *     (GETPROPERTY: tag = property type, after the call; GPU_COMMAND: tag 1
 *     cmdlist, 2 objlist, 3 synclist, 4 + i the IB of command i, before it)
 *   6 snapshot of one mapped KGSL object before a GPU_COMMAND: a = address
 */

typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef unsigned int u32;
typedef unsigned long u64;

/* bionic */
extern void __libc_init(void *raw_args, void (*onexit)(void),
                        int (*slingshot)(int, char **, char **), const void *structors);
extern int printf(const char *, ...);
extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
extern int fflush(void *);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern int *__errno(void);
extern void abort(void);
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern int open(const char *, int, ...);
extern ssize_t write(int, const void *, size_t);
extern int close(int);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_APPEND 02000

/* ---- raw system calls (arm64 Linux numbers) ---- */
static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
#define NR_ioctl 29
#define NR_openat 56
#define NR_munmap 215
#define NR_mmap 222
#define AT_FDCWD (-100)

/* ---- trace ---- */
static int trace_fd = -1;
static long trace_lock[10];	/* pthread_mutex_t is 40 bytes on arm64 bionic; zero = normal mutex */
static int kgsl_fd[16];
static struct { u64 addr, len; } maps[512];	/* live KGSL mappings */
static long maps_lock[10];	/* held while maps[] changes or is read out */

static int is_kgsl(int fd)
{
	for (int i = 0; i < 16; i++)
		if (kgsl_fd[i] == fd + 1)
			return 1;
	return 0;
}

static void rec(u32 kind, int fd, u32 cmd, u64 a, u64 b, long ret, int err,
                const void *before, const void *after, u32 len)
{
	struct {
		u32 magic, kind;
		int fd;
		u32 cmd;
		u64 a, b;
		long ret;
		int err;
		u32 len;
	} h = { 0x4354524b, kind, fd, cmd, a, b, ret, err, len };
	if (trace_fd < 0)
		return;
	pthread_mutex_lock(trace_lock);
	write(trace_fd, &h, sizeof(h));
	if (len) {
		write(trace_fd, before, len);
		write(trace_fd, after, len);
	}
	pthread_mutex_unlock(trace_lock);
}

static void blob(u32 kind, u32 cmd, u64 a, const void *p, u32 len)
{
	struct {
		u32 magic, kind;
		int fd;
		u32 cmd;
		u64 a, b;
		long ret;
		int err;
		u32 len;
	} h = { 0x4354524b, kind, -1, cmd, a, len, 0, 0, len };
	if (trace_fd < 0)
		return;
	pthread_mutex_lock(trace_lock);
	write(trace_fd, &h, sizeof(h));
	write(trace_fd, p, len);
	pthread_mutex_unlock(trace_lock);
}

/* only read GPU memory that is mapped into this process */
static int mapped(u64 a, u64 n)
{
	for (int i = 0; i < 512; i++)
		if (maps[i].len && a >= maps[i].addr && a + n <= maps[i].addr + maps[i].len)
			return 1;
	return 0;
}

struct cmdobj {
	u64 offset, gpuaddr, size;
	u32 flags, id;
};

static void before_submit(const unsigned char *arg)
{
	u64 cmdlist = *(const u64 *)(arg + 8), objlist = *(const u64 *)(arg + 24);
	u64 synclist = *(const u64 *)(arg + 40);
	u32 cmdsize = *(const u32 *)(arg + 16), ncmd = *(const u32 *)(arg + 20);
	u32 objsize = *(const u32 *)(arg + 32), nobj = *(const u32 *)(arg + 36);
	u32 syncsize = *(const u32 *)(arg + 48), nsync = *(const u32 *)(arg + 52);
	if (ncmd)
		blob(5, 0x4a << 16 | 1, cmdlist, (void *)cmdlist, cmdsize * ncmd);
	if (nobj)
		blob(5, 0x4a << 16 | 2, objlist, (void *)objlist, objsize * nobj);
	if (nsync)
		blob(5, 0x4a << 16 | 3, synclist, (void *)synclist, syncsize * nsync);
	for (u32 i = 0; i < ncmd && cmdsize >= sizeof(struct cmdobj); i++) {
		const struct cmdobj *c = (const void *)(cmdlist + (u64)i * cmdsize);
		if (c->size < 0x100000 && mapped(c->gpuaddr, c->size))
			blob(5, 0x4a << 16 | (4 + i), c->gpuaddr, (void *)c->gpuaddr, (u32)c->size);
	}
	pthread_mutex_lock(maps_lock);
	for (int i = 0; i < 512; i++)
		if (maps[i].len)
			blob(6, 0, maps[i].addr, (void *)maps[i].addr, (u32)maps[i].len);
	pthread_mutex_unlock(maps_lock);
}

/* exported from the executable, so they win over libc's for every library */
__attribute__((visibility("default"))) int open(const char *path, int flags, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, flags);
	int mode = __builtin_va_arg(ap, int);
	__builtin_va_end(ap);
	long r = sys6(NR_openat, AT_FDCWD, (long)path, flags, mode, 0, 0);
	int kg = 0;
	const char *k = "/dev/kgsl-3d0";
	int i = 0;
	while (k[i] && path[i] == k[i])
		i++;
	kg = !k[i] && !path[i];
	if (r < 0) {
		*__errno() = (int)-r;
		r = -1;
	} else if (kg) {
		for (int j = 0; j < 16; j++)
			if (!kgsl_fd[j]) {
				kgsl_fd[j] = (int)r + 1;
				break;
			}
	}
	if (kg)
		rec(4, (int)r, (u32)flags, 0, 0, r, r < 0 ? *__errno() : 0, 0, 0, 0);
	return (int)r;
}

__attribute__((visibility("default"))) int __open_2(const char *path, int flags)
{
	return open(path, flags, 0);
}

__attribute__((visibility("default"))) int ioctl(int fd, int req, void *arg)
{
	u32 cmd = (u32)req, len = (cmd >> 16) & 0x3fff;
	int kg = is_kgsl(fd) || ((cmd >> 8) & 0xff) == 0x3e;	/* 0x3e: sync fences */
	void *before = 0;
	if (kg && len && arg) {
		before = malloc(len);
		memcpy(before, arg, len);
	}
	int nr = is_kgsl(fd) && ((cmd >> 8) & 0xff) == 0x09 ? (int)(cmd & 0xff) : -1;
	if (nr == 0x4a && arg)
		before_submit(arg);
	long r = sys6(NR_ioctl, fd, req, (long)arg, 0, 0, 0);
	int err = r < 0 ? (int)-r : 0;
	if (kg) {
		rec(1, fd, cmd, (u64)arg, 0, r, err, before, arg, before ? len : 0);
		free(before);
	}
	if (nr == 0x02 && arg && r == 0) {
		const unsigned char *g = arg;
		u32 type = *(const u32 *)g;
		u64 value = *(const u64 *)(g + 8), size = *(const u64 *)(g + 16);
		if (value && size < 0x10000)
			blob(5, 0x02 << 16 | type, value, (void *)value, (u32)size);
	}
	if (r < 0) {
		*__errno() = err;
		return -1;
	}
	return (int)r;
}

__attribute__((visibility("default"))) void *mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off)
{
	long r = sys6(NR_mmap, (long)addr, n, prot, flags, fd, off);
	if (fd >= 0 && is_kgsl(fd)) {
		rec(2, fd, (u32)prot | (u32)flags << 8, n, (u64)off, r, r < 0 && r > -4096 ? (int)-r : 0, 0, 0, 0);
		if (!(r < 0 && r > -4096) && (prot & 1)) {
			pthread_mutex_lock(maps_lock);
			for (int i = 0; i < 512; i++)
				if (!maps[i].len) {
					maps[i].addr = (u64)r;
					maps[i].len = n;
					break;
				}
			pthread_mutex_unlock(maps_lock);
		}
	}
	if (r < 0 && r > -4096) {
		*__errno() = (int)-r;
		return (void *)-1;
	}
	return (void *)r;
}

__attribute__((visibility("default"))) void *mmap64(void *addr, size_t n, int prot, int flags, int fd, off_t off)
{
	return mmap(addr, n, prot, flags, fd, off);
}

__attribute__((visibility("default"))) int munmap(void *addr, size_t n)
{
	pthread_mutex_lock(maps_lock);
	for (int i = 0; i < 512; i++)
		if (maps[i].len && maps[i].addr == (u64)addr)
			maps[i].len = 0;
	long r = sys6(NR_munmap, (long)addr, n, 0, 0, 0, 0);
	pthread_mutex_unlock(maps_lock);
	rec(3, -1, 0, (u64)addr, n, r, 0, 0, 0, 0);
	if (r < 0) {
		*__errno() = (int)-r;
		return -1;
	}
	return 0;
}

/* Android's log goes to logd, which TWRP does not run: print it instead */
__attribute__((visibility("default"))) int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
	char b[1024];
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	__builtin_va_end(ap);
	printf("log %d %s: %s\n", prio, tag ? tag : "-", b);
	return 1;
}

__attribute__((visibility("default"))) void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
	char b[1024] = "";
	if (fmt) {
		__builtin_va_list ap;
		__builtin_va_start(ap, fmt);
		vsnprintf(b, sizeof(b), fmt, ap);
		__builtin_va_end(ap);
	}
	printf("ASSERT %s %s: %s\n", cond ? cond : "-", tag ? tag : "-", b);
	fflush(0);
	abort();
}

/* Nokia's gralloc.sdm660.so imports this from a newer libcutils than TWRP's;
 * tracing is off here */
__attribute__((visibility("default"))) u64 atrace_get_enabled_tags(void)
{
	return 0;
}

/* ---- EGL / GLES (values from the Khronos API the driver implements) ---- */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
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
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

static void *gles, *egl;
#define F(lib, ret, name, args) \
	static ret(*name) args;
#define L(lib, name)                                                   \
	do {                                                           \
		*(void **)&name = dlsym(lib, #name);                   \
		if (!name) {                                           \
			printf("FAIL: no symbol %s\n", #name);         \
			return 1;                                      \
		}                                                      \
	} while (0)
F(egl, EGLDisplay, eglGetDisplay, (void *))
F(egl, int, eglInitialize, (EGLDisplay, int *, int *))
F(egl, int, eglChooseConfig, (EGLDisplay, const int *, EGLConfig *, int, int *))
F(egl, EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const int *))
F(egl, EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const int *))
F(egl, int, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
F(egl, int, eglGetError, (void))
F(egl, const char *, eglQueryString, (EGLDisplay, int))
F(egl, int, eglTerminate, (EGLDisplay))
F(gles, const char *, glGetString, (u32))
F(gles, void, glClearColor, (float, float, float, float))
F(gles, void, glClear, (u32))
F(gles, void, glFinish, (void))
F(gles, void, glViewport, (int, int, int, int))
F(gles, void, glReadPixels, (int, int, int, int, u32, u32, void *))
F(gles, u32, glGetError, (void))
F(gles, u32, glCreateShader, (u32))
F(gles, void, glShaderSource, (u32, int, const char *const *, const int *))
F(gles, void, glCompileShader, (u32))
F(gles, void, glGetShaderiv, (u32, u32, int *))
F(gles, void, glGetShaderInfoLog, (u32, int, int *, char *))
F(gles, u32, glCreateProgram, (void))
F(gles, void, glAttachShader, (u32, u32))
F(gles, void, glBindAttribLocation, (u32, u32, const char *))
F(gles, void, glLinkProgram, (u32))
F(gles, void, glGetProgramiv, (u32, u32, int *))
F(gles, void, glUseProgram, (u32))
F(gles, void, glVertexAttribPointer, (u32, int, u32, unsigned char, int, const void *))
F(gles, void, glEnableVertexAttribArray, (u32))
F(gles, void, glDrawArrays, (u32, int, int))

#define W 64
#define H 64
static unsigned char px[W * H * 4];

static int expect(const char *what, int x, int y, u32 rgba)
{
	unsigned char *p = px + (y * W + x) * 4;
	u32 got = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
	printf("%s: pixel (%d,%d) = %08x, expected %08x: %s\n", what, x, y, got, rgba,
	       got == rgba ? "OK" : "WRONG");
	return got == rgba ? 0 : 1;
}

static int count(u32 rgba)
{
	int n = 0;
	for (int i = 0; i < W * H; i++) {
		unsigned char *p = px + i * 4;
		n += (u32)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) == rgba;
	}
	return n;
}

static u32 shader(u32 type, const char *src)
{
	u32 s = glCreateShader(type);
	int ok = 0;
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), 0, log);
		printf("FAIL: shader compile: %s\n", log);
		return 0;
	}
	return s;
}

static int run(void)
{
	int fails = 0, major = 0, minor = 0, n = 0;
	EGLConfig cfg;
	egl = dlopen("libEGL_adreno.so", RTLD_NOW | RTLD_GLOBAL);
	gles = dlopen("libGLESv2_adreno.so", RTLD_NOW | RTLD_GLOBAL);
	if (!egl || !gles) {
		printf("FAIL: dlopen: %s\n", dlerror());
		return 1;
	}
	L(egl, eglGetDisplay); L(egl, eglInitialize); L(egl, eglChooseConfig);
	L(egl, eglCreatePbufferSurface); L(egl, eglCreateContext); L(egl, eglMakeCurrent);
	L(egl, eglGetError); L(egl, eglQueryString); L(egl, eglTerminate);
	L(gles, glGetString); L(gles, glClearColor); L(gles, glClear); L(gles, glFinish);
	L(gles, glViewport); L(gles, glReadPixels); L(gles, glGetError);
	L(gles, glCreateShader); L(gles, glShaderSource); L(gles, glCompileShader);
	L(gles, glGetShaderiv); L(gles, glGetShaderInfoLog); L(gles, glCreateProgram);
	L(gles, glAttachShader); L(gles, glBindAttribLocation); L(gles, glLinkProgram);
	L(gles, glGetProgramiv); L(gles, glUseProgram); L(gles, glVertexAttribPointer);
	L(gles, glEnableVertexAttribArray); L(gles, glDrawArrays);

	EGLDisplay d = eglGetDisplay(0);
	if (!d || !eglInitialize(d, &major, &minor)) {
		printf("FAIL: eglInitialize, error %x\n", eglGetError());
		return 1;
	}
	printf("EGL %d.%d vendor \"%s\" version \"%s\"\n", major, minor,
	       eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION));
	static const int ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
				  EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) {
		printf("FAIL: eglChooseConfig, error %x n %d\n", eglGetError(), n);
		return 1;
	}
	static const int sa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	static const int xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
	EGLContext c = eglCreateContext(d, cfg, 0, xa);
	if (!s || !c || !eglMakeCurrent(d, s, s, c)) {
		printf("FAIL: surface %p context %p, error %x\n", s, c, eglGetError());
		return 1;
	}
	printf("GL vendor \"%s\" renderer \"%s\" version \"%s\"\n", glGetString(GL_VENDOR),
	       glGetString(GL_RENDERER), glGetString(GL_VERSION));

	/* test 1: clear to a known colour */
	glViewport(0, 0, W, H);
	glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	fails += expect("clear", 0, 0, 0xff0000ff);
	fails += expect("clear", W - 1, H - 1, 0xff0000ff);
	printf("clear: %d of %d pixels red\n", count(0xff0000ff), W * H);
	fails += count(0xff0000ff) != W * H;

	/* test 2: a shaded triangle (runs the shader compiler and a draw) */
	u32 vs = shader(GL_VERTEX_SHADER,
			"attribute vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n");
	u32 fs = shader(GL_FRAGMENT_SHADER,
			"precision mediump float;\nvoid main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
	if (!vs || !fs)
		return fails + 1;
	u32 prog = glCreateProgram();
	int ok = 0;
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		printf("FAIL: link\n");
		return fails + 1;
	}
	/* covers the lower-left half of the surface: x + y < 0 in clip space */
	static const float tri[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f };
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(prog);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, tri);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	fails += expect("triangle", 2, 2, 0x00ff00ff);
	fails += expect("triangle", W - 3, H - 3, 0x0000ffff);
	int g = count(0x00ff00ff), b = count(0x0000ffff);
	/* pixel centres strictly below the diagonal: 64*63/2 = 2016; the 64
	 * centres exactly on it go either way by the fill rule */
	printf("triangle: %d green, %d blue, %d other (expected 2016 or 2080 green, no other)\n",
	       g, b, W * H - g - b);
	fails += (g != 2016 && g != 2080) || g + b != W * H;
	printf("GL error %x\n", glGetError());
	eglTerminate(d);
	return fails;
}

static int main_(int argc, char **argv, char **envp)
{
	(void)envp;
	trace_fd = (int)sys6(NR_openat, AT_FDCWD, (long)(argc > 1 ? argv[1] : "/tmp/kgsl-trace.bin"),
			     O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
	int fails = run();
	printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	fflush(0);
	if (trace_fd >= 0)
		close(trace_fd);
	return fails ? 1 : 0;
}

/* bionic start-up: the dynamic linker jumps to _start with sp at argc */
__attribute__((used)) static void start_c(void *raw_args)
{
	static void *structors[6];	/* no preinit/init/fini arrays */
	__libc_init(raw_args, 0, main_, structors);
}
__asm__(".text\n.globl _start\n.type _start,%function\n_start:\n"
	"mov x29, #0\nmov x30, #0\nmov x0, sp\nb start_c\n");
