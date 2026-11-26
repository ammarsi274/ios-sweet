/*
 * libdrgeagl.dylib: OpenGL ES for backboardd's CoreAnimation on the Nokia
 * 6.1 Plus, drawn by the Adreno 512 through Nokia's own driver.
 *
 * What CoreAnimation needs (drg/gpu-research/ca-selection.md, read in
 * QuartzCore 20H392): EAGLServer::renderer() builds a GLESContext when
 * [[EAGLContext alloc] initWithAPI:3 properties:] returns a context whose
 * -API is 3 (Metal must still fail, it does: no IOAcceleratorES). After
 * that it uses only seven EAGL selectors and the 63 GL functions of a table
 * from -getMacroContextPrivate, each called as f(table[0], args...). The
 * display is an IOSurface attached to a texture (attachImage:toCoreSurface:)
 * that becomes an FBO's colour attachment.
 *
 * Here: loaded into backboardd with DYLD_INSERT_LIBRARIES (patch 0914 lets
 * dyld honour it); the EAGLContext methods are replaced at load. The GL
 * underneath is Nokia's libGLESv2_adreno on our ELF loader and bionic layer
 * (as the drgles bench), its KGSL calls on XNU's DrgGPU (kgsl_ios.c).
 * IOSurfaces reach it as Android window buffers (drgwin.c, our gralloc
 * module), imported zero-copy: measured on TWRP (render target and sampling,
 * RGBA and BGRA, 1125x2436, GPU-DRAW-PLAN.md).
 *
 * Safety: the GPU is used only if the self-test (com.drg.gpuselftest,
 * bench -selftest) found a passing submission framing; once
 * per boot (a restarted backboardd stays on SWContext); a GPU fault seen at
 * a flush aborts backboardd so that launchd restarts it on SWContext.
 * Anything unexpected makes initWithAPI: return nil: CoreAnimation then
 * draws with SWContext, as before.
 *
 * Root r25: loaded by an LC_LOAD_WEAK_DYLIB in /usr/libexec/backboardd
 * (launchd drops DYLD_INSERT_LIBRARIES for com.apple.* jobs, boot #53). The
 * verdict comes from the cache /private/var/db/drg/gpu-selftest (verdict.h),
 * read at load without waiting: unless it is "mode N" for this bench,
 * library and kernel, no GPU fault mark is set and the GPU was not tried
 * before in this boot, nothing is replaced and backboardd runs exactly as
 * without this library (CPU drawing). With the GPU, every frame ends with
 * glFinish and a CPU sync of the render targets (wait + dc civac) before
 * CoreAnimation hands the surface to the display (boot #55: GPU-written
 * surfaces read 48 % stale by the CPU without it).
 */
#include "elf.h"
#include "host.h"
#include "rt.h"
#include "kgsl.h"
#include "drgwin.h"
#include "verdict.h"
#include "bionic/bionic.h"
#include "../drgpu/drgpu_uc.h"

#define LIBS_DIR	"/usr/local/drgles/adreno"
#define LOG_FILE	"/private/var/tmp/drgeagl.log"	/* backboardd runs as mobile: /var/log and /var/run are root's */
#define BOOT_FILE	"/private/var/tmp/drgeagl-boot"

/* ---- libSystem, libobjc, IOSurface (declarations as the SDK has them) ---- */
typedef long ssize_t;
int open(const char *, int, ...);
int close(int);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int getpid(void);
int proc_name(int pid, void *buf, uint32_t size);
int sysctlbyname(const char *, void *, size_t *, void *, size_t);
void abort(void) __attribute__((noreturn));
char *strstr(const char *, const char *);
int access(const char *, int);
uint64_t mach_absolute_time(void);
struct mach_timebase_info { uint32_t numer, denom; };
int mach_timebase_info(struct mach_timebase_info *);
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT 0x0200
#define O_TRUNC 0x0400
#define O_APPEND 0x0008

typedef struct objc_class *Class;
typedef struct objc_object *id;
typedef struct objc_selector *SEL;
typedef struct objc_method *Method;
typedef void (*IMP)(void);
Class objc_getClass(const char *);
SEL sel_registerName(const char *);
Method class_getInstanceMethod(Class, SEL);
IMP method_getImplementation(Method);
IMP class_replaceMethod(Class, SEL, IMP, const char *);
Class class_getSuperclass(Class);
void objc_msgSend(void);	/* always called through a cast to the exact type */
/* OpenGLES's class: importing it makes dyld load OpenGLES (and run its initialisers) before ours */
extern char eagl_class __asm__("_OBJC_CLASS_$_EAGLContext");

typedef struct __IOSurface *IOSurfaceRef;
uint32_t IOSurfaceGetID(IOSurfaceRef);
size_t IOSurfaceGetWidth(IOSurfaceRef);
size_t IOSurfaceGetHeight(IOSurfaceRef);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef);
size_t IOSurfaceGetAllocSize(IOSurfaceRef);
size_t IOSurfaceGetPlaneCount(IOSurfaceRef);
uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef);

/* ---- GL / EGL ---- */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef void *GLsync;
typedef uint64_t GLuint64;
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLImage;

#define GL_EXTENSIONS 0x1F03
#define GL_TEXTURE_2D 0x0DE1
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADER_TYPE 0x8B4F
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#define GL_TEXTURE_RECTANGLE 0x84F5		/* Apple's ES has it; Adreno's does not (boot #61) */
#define GL_TEXTURE_BINDING_RECTANGLE 0x84F6
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_ACTIVE_UNIFORMS 0x8B86
/* a rectangle texture is a 2D texture here; the shaders get normalized coordinates (rect_*) */
#define RT(t) ((GLenum)(t) == GL_TEXTURE_RECTANGLE ? GL_TEXTURE_2D : (GLenum)(t))
#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_PBUFFER_BIT 0x0001
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_IMAGE_PRESERVED_KHR 0x30D2

/* Nokia's functions (libGLESv2_adreno.so / libEGL_adreno.so), filled by load_gl */
#define GLFUNCS(X) \
	X(void, glBindTexture, (GLenum, GLuint)) \
	X(void, glBlendFunc, (GLenum, GLenum)) \
	X(void, glClear, (GLbitfield)) \
	X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, glCopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
	X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
	X(void, glDisable, (GLenum)) \
	X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
	X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
	X(void, glEnable, (GLenum)) \
	X(void, glFinish, (void)) \
	X(void, glFlush, (void)) \
	X(void, glGenTextures, (GLsizei, GLuint *)) \
	X(GLenum, glGetError, (void)) \
	X(void, glGetIntegerv, (GLenum, GLint *)) \
	X(const unsigned char *, glGetString, (GLenum)) \
	X(void, glPixelStorei, (GLenum, GLint)) \
	X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	X(void, glTexParameterf, (GLenum, GLenum, GLfloat)) \
	X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
	X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
	X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
	X(void, glBlendEquation, (GLenum)) \
	X(void, glActiveTexture, (GLenum)) \
	X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
	X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
	X(void, glEnableVertexAttribArray, (GLuint)) \
	X(void, glDisableVertexAttribArray, (GLuint)) \
	X(void, glDeleteProgram, (GLuint)) \
	X(void, glDeleteShader, (GLuint)) \
	X(GLboolean, glIsProgram, (GLuint)) \
	X(GLuint, glCreateShader, (GLenum)) \
	X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	X(void, glCompileShader, (GLuint)) \
	X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
	X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(GLuint, glCreateProgram, (void)) \
	X(void, glAttachShader, (GLuint, GLuint)) \
	X(void, glLinkProgram, (GLuint)) \
	X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
	X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(void, glUseProgram, (GLuint)) \
	X(void, glUniform1f, (GLint, GLfloat)) \
	X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, glUniform1i, (GLint, GLint)) \
	X(void, glUniform1fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
	X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
	X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
	X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
	X(void, glBindFramebuffer, (GLenum, GLuint)) \
	X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
	X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
	X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
	X(GLenum, glCheckFramebufferStatus, (GLenum)) \
	X(void, glGenerateMipmap, (GLenum)) \
	X(GLsync, glFenceSync, (GLenum, GLbitfield)) \
	X(void, glDeleteSync, (GLsync)) \
	X(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64)) \
	X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
#define EGLFUNCS(X) \
	X(EGLDisplay, eglGetDisplay, (void *)) \
	X(int, eglInitialize, (EGLDisplay, int *, int *)) \
	X(int, eglChooseConfig, (EGLDisplay, const int *, EGLConfig *, int, int *)) \
	X(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const int *)) \
	X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const int *)) \
	X(int, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
	X(int, eglGetError, (void)) \
	X(void *, eglGetProcAddress, (const char *))
#define DECL(ret, name, args) static ret(*n_##name) args;
GLFUNCS(DECL)
EGLFUNCS(DECL)
static EGLImage (*n_eglCreateImageKHR)(EGLDisplay, EGLContext, GLenum, void *, const int *);
static void (*n_glEGLImageTargetTexture2DOES)(GLenum, EGLImage);

/* ---- log ---- */
#define LOG(...) host_log("drgeagl: " __VA_ARGS__)

static double now_ms(void)
{
	static struct mach_timebase_info tb;
	if (!tb.denom)
		mach_timebase_info(&tb);
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

/* ---- the GL state of one EAGLContext ---- */
#define TABLE_SLOTS (0x2010 / 8)
struct ectx {
	id owner;		/* the EAGLContext */
	EGLContext egl;
	void *table[TABLE_SLOTS];	/* [0] = this; the functions at QuartzCore's offsets */
};
static EGLDisplay dpy;
static EGLConfig cfg;
static EGLSurface pbuf;
static struct ectx *ctxs[8];
static __thread struct ectx *cur;	/* the context current on this thread */

/*
 * An EGL context can be current on one thread at a time; initWithAPI:
 * releases it again, so the thread that draws (CoreAnimation's render
 * thread) can take it. Each new thread is logged once.
 */
static uint64_t threads_seen[8];
static void make_current(struct ectx *c)
{
	static unsigned failed;
	if (cur == c)
		return;
	if (!n_eglMakeCurrent(dpy, pbuf, pbuf, c->egl)) {
		if (failed++ < 8)
			LOG("eglMakeCurrent failed on thread %llu: EGL error %x\n", (unsigned long long)host_thread_self(),
			    n_eglGetError());
		return;
	}
	cur = c;
	uint64_t t = host_thread_self();
	for (int i = 0; i < 8; i++) {
		if (threads_seen[i] == t)
			break;
		if (!threads_seen[i]) {
			threads_seen[i] = t;
			LOG("context %p current on thread %llu\n", (void *)c, (unsigned long long)t);
			break;
		}
	}
}

/* ---- frames: flush = finish (the panel copy must see a whole frame), fault check, timing ---- */
static double flush_ms, sync_ms, last_flush;
static unsigned frame_work, win_draws, win_empty;	/* GL work since the last frame end; per 120-frame window */
static unsigned frames, sync_errs;

/* the next boot's self-test reruns, and until then this library stays off (verdict.h) */
static void mark_fault(const char *why)
{
	int fd = open(DRG_FAULT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, why, strlen(why));
		close(fd);
	}
	LOG("fault mark %s: %s (fd %d)\n", DRG_FAULT_FILE, why, fd);
}

static volatile uint64_t frame_thread;	/* the thread that ends frames: it keeps the context (MC below) */

static void frame_end(void)
{
	frame_thread = host_thread_self();	/* this thread draws: it keeps the context (MC) */
	/*
	 * Nothing drawn or uploaded since the last frame end: the GPU has nothing new for the display, so
	 * no finish and no CPU sync (boot #60: 24 frame ends a second while idle, 10.8 ms of sync each).
	 */
	int empty = !frame_work;
	frame_work = 0;
	if (empty) {
		win_empty++;
	} else {
		double t0 = now_ms();
		n_glFinish();
		double t1 = now_ms();
		if (kgsl_ios_faulted()) {
			LOG("GPU fault seen at a flush after %u frames: aborting backboardd (launchd restarts it on SWContext)\n",
			    frames);
			mark_fault("GPU fault in backboardd\n");
			abort();
		}
		/* the render targets this frame wrote (attachImage surfaces), before the display reads them */
		int sr = kgsl_ios_cpu_sync();
		double t2 = now_ms();
		if (sr < 0 && sync_errs++ < 8)
			LOG("CPU sync after frame %u: wait error %d\n", frames, sr);
		flush_ms += t1 - t0;
		sync_ms += t2 - t1;
	}
	frames++;
	if (frames % 120 == 0) {
		double t3 = now_ms();
		uint64_t calls, bytes, ns;
		static uint64_t last_bytes;
		kgsl_ios_sync_stats(&calls, &bytes, &ns);
		LOG("%u frames; last 120: %.1f ms between frame ends, %u empty, %u draws; per frame %.2f ms waiting "
		    "for the GPU, %.2f ms CPU sync; %llu MiB invalidated\n", frames, (t3 - last_flush) / 120, win_empty,
		    win_draws, flush_ms / 120, sync_ms / 120, (unsigned long long)((bytes - last_bytes) >> 20));
		last_bytes = bytes;
		flush_ms = sync_ms = 0;
		win_empty = win_draws = 0;
		last_flush = t3;
	}
}

/*
 * What each frame wrote (boot #60: frame_end invalidated both 13.9 MB display surfaces every frame,
 * 10.8 ms, also for frames that drew nothing). Textures backed by an attachImage surface, the colour
 * texture of each framebuffer, the bound draw framebuffer: a draw marks only its target's surface
 * dirty (kgsl_ios_surface_dirty), anything not known marks all of them (id 0). One EGL context draws
 * (boot #60's log), so plain statics.
 */
#define NTRACK 32
static struct { GLuint tex; uint32_t surface; } tex_surf[NTRACK];
static struct { GLuint fbo, tex; } fbo_tex[NTRACK];
static GLuint cur_fbo;

static uint32_t surface_of_tex(GLuint t)
{
	for (int i = 0; i < NTRACK; i++)
		if (t && tex_surf[i].tex == t)
			return tex_surf[i].surface;
	return 0;
}

static void tex_set(GLuint t, uint32_t surface)
{
	int slot = -1;
	for (int i = 0; i < NTRACK; i++) {
		if (tex_surf[i].tex == t) {
			slot = i;
			break;
		}
		if (!tex_surf[i].tex && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		LOG("more than %d surface textures: texture %u not tracked\n", NTRACK, t);
		return;
	}
	tex_surf[slot].tex = t;
	tex_surf[slot].surface = surface;
}

static void tex_forget(GLuint t)
{
	for (int i = 0; i < NTRACK; i++)
		if (t && tex_surf[i].tex == t)
			tex_surf[i].tex = 0;
	for (int i = 0; i < NTRACK; i++)
		if (t && fbo_tex[i].tex == t)
			fbo_tex[i].tex = 0;
}

static void fbo_forget(GLuint f)
{
	for (int i = 0; i < NTRACK; i++)
		if (f && fbo_tex[i].fbo == f)
			fbo_tex[i].fbo = fbo_tex[i].tex = 0;
	if (cur_fbo == f)
		cur_fbo = 0;
}

static void fbo_attach(GLuint f, GLuint t)
{
	int slot = -1;
	for (int i = 0; i < NTRACK; i++) {
		if (fbo_tex[i].fbo == f) {
			slot = i;
			break;
		}
		if (!fbo_tex[i].fbo && slot < 0)
			slot = i;
	}
	if (!f)
		return;	/* framebuffer 0 is the 1x1 pbuffer */
	if (slot < 0) {
		LOG("more than %d framebuffers: framebuffer %u not tracked\n", NTRACK, f);
		return;
	}
	fbo_tex[slot].fbo = f;
	fbo_tex[slot].tex = t;
}

static GLuint bound_tex(GLenum target)
{
	GLint t = 0;
	n_glGetIntegerv(target == GL_TEXTURE_EXTERNAL_OES ? GL_TEXTURE_BINDING_EXTERNAL_OES : GL_TEXTURE_BINDING_2D, &t);
	return (GLuint)t;
}

/* a draw (clear, draw arrays/elements) into the bound framebuffer */
static void drew(void)
{
	frame_work++;
	win_draws++;
	if (!cur_fbo)
		return;	/* the pbuffer: no surface */
	for (int i = 0; i < NTRACK; i++)
		if (fbo_tex[i].fbo == cur_fbo) {
			uint32_t s = surface_of_tex(fbo_tex[i].tex);
			if (s)
				kgsl_ios_surface_dirty(s);
			return;
		}
	kgsl_ios_surface_dirty(0);	/* a framebuffer not seen attached: sync every surface */
}

/* the bound texture of `target` gets new contents (upload, copy, mipmaps) */
static void tex_written(GLenum target)
{
	frame_work++;
	if (target != GL_TEXTURE_2D && target != GL_TEXTURE_EXTERNAL_OES) {
		kgsl_ios_surface_dirty(0);
		return;
	}
	uint32_t s = surface_of_tex(bound_tex(target));
	if (s)
		kgsl_ios_surface_dirty(s);
}

/* glTexImage2D gives the bound texture new storage: no longer the surface */
static void tex_replaced(GLenum target)
{
	frame_work++;
	if (target == GL_TEXTURE_2D)
		tex_forget(bound_tex(target));
}

/*
 * The table's functions: f(table[0], GL args...) as QuartzCore calls them.
 * Each makes its context current on the calling thread and calls Nokia's.
 */
#define C struct ectx *c
/*
 * Boot #59: after initWithAPI released the context, CoreAnimation's GL calls on its setup thread made it
 * current there again and nothing released it, so the render thread got EGL_BAD_ACCESS (0x3002) on every
 * frame and the screen stayed black. Now a table call on any thread but the one that ends frames
 * (frame_end: glFinish/presentation) gives the context back when it returns; that thread keeps it.
 */
static void give_back(struct ectx *c, struct ectx *prev);
struct mc_guard { struct ectx *c, *prev; };
static void mc_end(struct mc_guard *g)
{
	if (g->prev != g->c && host_thread_self() != frame_thread)
		give_back(g->c, g->prev);
}
#define MC struct mc_guard mc_g __attribute__((cleanup(mc_end))) = { c, cur }; make_current(c)
static void t_glBindTexture(C, GLenum a, GLuint b) { MC; n_glBindTexture(RT(a), b); }
static void t_glBlendFunc(C, GLenum a, GLenum b) { MC; n_glBlendFunc(a, b); }
static void t_glClear(C, GLbitfield a) { MC; drew(); n_glClear(a); }
static void t_glClearColor(C, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { MC; n_glClearColor(r, g, b, a); }
static void t_glCopyTexSubImage2D(C, GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{ MC; t = RT(t); tex_written(t); n_glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h); }
static void t_glDeleteTextures(C, GLsizei n, const GLuint *t) { MC; for (GLsizei i = 0; i < n; i++) tex_forget(t[i]); n_glDeleteTextures(n, t); }
static void t_glDisable(C, GLenum a) { MC; if (a != GL_TEXTURE_RECTANGLE) n_glDisable(a); }
static void rect_update(void);
static void t_glDrawArrays(C, GLenum m, GLint f, GLsizei n) { MC; drew(); rect_update(); n_glDrawArrays(m, f, n); }
static void t_glDrawElements(C, GLenum m, GLsizei n, GLenum t, const void *p) { MC; drew(); rect_update(); n_glDrawElements(m, n, t, p); }
static void t_glEnable(C, GLenum a) { MC; if (a != GL_TEXTURE_RECTANGLE) n_glEnable(a); }
static void t_glFinish(C) { MC; frame_end(); }
static void t_glFlush(C) { MC; frame_end(); }
static void t_glGenTextures(C, GLsizei n, GLuint *t) { MC; n_glGenTextures(n, t); }
static GLenum t_glGetError(C) { MC; return n_glGetError(); }
static void t_glGetIntegerv(C, GLenum p, GLint *v) { MC; n_glGetIntegerv(p == GL_TEXTURE_BINDING_RECTANGLE ? GL_TEXTURE_BINDING_2D : p, v); }
static void t_glPixelStorei(C, GLenum p, GLint v) { MC; n_glPixelStorei(p, v); }
static void t_glScissor(C, GLint x, GLint y, GLsizei w, GLsizei h) { MC; n_glScissor(x, y, w, h); }
static void t_glTexImage2D(C, GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,
			   const void *p) { MC; t = RT(t); tex_replaced(t); n_glTexImage2D(t, l, i, w, h, b, f, ty, p); }
static void t_glTexParameterf(C, GLenum t, GLenum p, GLfloat v) { MC; n_glTexParameterf(RT(t), p, v); }
static void t_glTexParameteri(C, GLenum t, GLenum p, GLint v) { MC; n_glTexParameteri(RT(t), p, v); }
static void t_glTexSubImage2D(C, GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty,
			      const void *p) { MC; t = RT(t); tex_written(t); n_glTexSubImage2D(t, l, x, y, w, h, f, ty, p); }
static void t_glViewport(C, GLint x, GLint y, GLsizei w, GLsizei h) { MC; n_glViewport(x, y, w, h); }
static void t_glBlendFuncSeparate(C, GLenum a, GLenum b, GLenum d, GLenum e) { MC; n_glBlendFuncSeparate(a, b, d, e); }
static void t_glBlendEquation(C, GLenum a) { MC; n_glBlendEquation(a); }
static void t_glActiveTexture(C, GLenum a) { MC; n_glActiveTexture(a); }
static void t_glCompressedTexImage2D(C, GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b, GLsizei n,
				     const void *p) { MC; t = RT(t); tex_replaced(t); n_glCompressedTexImage2D(t, l, i, w, h, b, n, p); }
static void t_glFinishObjectAPPLE(C, GLenum o, GLint n) { MC; n_glFinish(); }
static void t_glVertexAttribPointer(C, GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p)
{ MC; n_glVertexAttribPointer(i, s, t, n, st, p); }
static void t_glEnableVertexAttribArray(C, GLuint i) { MC; n_glEnableVertexAttribArray(i); }
static void t_glDisableVertexAttribArray(C, GLuint i) { MC; n_glDisableVertexAttribArray(i); }
/* one slot for both (QuartzCore 0x1280): shaders and programs share one name space in ES */
static void t_glDeleteObject(C, GLuint n)
{
	MC;
	if (n_glIsProgram(n))
		n_glDeleteProgram(n);
	else
		n_glDeleteShader(n);
}
static GLuint t_glCreateShader(C, GLenum t) { MC; return n_glCreateShader(t); }
static void t_glCompileShader(C, GLuint s);
static void t_glShaderSource(C, GLuint s, GLsizei n, const GLchar *const *src, const GLint *len);
static GLuint t_glCreateProgram(C) { MC; return n_glCreateProgram(); }
static void t_glAttachShader(C, GLuint p, GLuint s) { MC; n_glAttachShader(p, s); }
static void t_glLinkProgram(C, GLuint p);
static GLuint cur_prog;
static void t_glUseProgram(C, GLuint p) { MC; cur_prog = p; n_glUseProgram(p); }
static void t_glUniform1f(C, GLint l, GLfloat a) { MC; n_glUniform1f(l, a); }
static void t_glUniform4f(C, GLint l, GLfloat a, GLfloat b, GLfloat d, GLfloat e) { MC; n_glUniform4f(l, a, b, d, e); }
static void t_glUniform1i(C, GLint l, GLint a) { MC; n_glUniform1i(l, a); }
static void t_glUniform1fv(C, GLint l, GLsizei n, const GLfloat *v) { MC; n_glUniform1fv(l, n, v); }
static void t_glUniform2fv(C, GLint l, GLsizei n, const GLfloat *v) { MC; n_glUniform2fv(l, n, v); }
static void t_glUniform3fv(C, GLint l, GLsizei n, const GLfloat *v) { MC; n_glUniform3fv(l, n, v); }
static void t_glUniform4fv(C, GLint l, GLsizei n, const GLfloat *v) { MC; n_glUniform4fv(l, n, v); }
static void t_glUniformMatrix3fv(C, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { MC; n_glUniformMatrix3fv(l, n, t, v); }
static void t_glUniformMatrix4fv(C, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { MC; n_glUniformMatrix4fv(l, n, t, v); }
static GLint t_glGetUniformLocation(C, GLuint p, const GLchar *n) { MC; return n_glGetUniformLocation(p, n); }
static void t_glBindAttribLocation(C, GLuint p, GLuint i, const GLchar *n) { MC; n_glBindAttribLocation(p, i, n); }
static void t_glBindFramebuffer(C, GLenum t, GLuint f) { MC; if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) cur_fbo = f; n_glBindFramebuffer(t, f); }
static void t_glDeleteFramebuffers(C, GLsizei n, const GLuint *f) { MC; for (GLsizei i = 0; i < n; i++) fbo_forget(f[i]); n_glDeleteFramebuffers(n, f); }
static void t_glGenFramebuffers(C, GLsizei n, GLuint *f) { MC; n_glGenFramebuffers(n, f); }
static void t_glFramebufferTexture2D(C, GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{ MC; if (a == GL_COLOR_ATTACHMENT0) fbo_attach(cur_fbo, tex); n_glFramebufferTexture2D(t, a, RT(tt), tex, l); }
static void t_glGenerateMipmap(C, GLenum t) { MC; t = RT(t); tex_written(t); n_glGenerateMipmap(t); }
/* GL_APPLE_sync = ES3 sync objects (same semantics); not advertised, so CoreAnimation should not call them */
static GLsync t_glFenceSync(C, GLenum cond, GLbitfield f) { MC; return n_glFenceSync(cond, f); }
static void t_glDeleteSync(C, GLsync s) { MC; n_glDeleteSync(s); }
static GLenum t_glClientWaitSync(C, GLsync s, GLbitfield f, GLuint64 t) { MC; return n_glClientWaitSync(s, f, t); }
/* debug labels and markers, and FramebufferParameteriAPPLE (0x8d40, 0x8a79, ...): nothing for Nokia's GL to do */
static void t_glLabelObjectEXT(C, GLenum t, GLuint o, GLsizei n, const GLchar *s) { }
static void t_glInsertEventMarkerEXT(C, GLsizei n, const GLchar *s) { }
static void t_glPushGroupMarkerEXT(C, GLsizei n, const GLchar *s) { }
static void t_glPopGroupMarkerEXT(C) { }
static void t_glFramebufferParameteriAPPLE(C, GLenum t, GLenum p, GLint v) { }

/*
 * glGetString: GL_EXTENSIONS limited to the names CoreAnimation probes that
 * Nokia's GL also reports, without GL_EXT_shader_framebuffer_fetch (CA
 * would then write gl_LastFragData shaders) and without GL_APPLE_fence;
 * 0x8a1e (read with the extensions; not a Khronos enum) is "" (never NULL).
 */
static char extensions[512];
static const unsigned char *t_glGetString(C, GLenum n)
{
	MC;
	if (n == GL_EXTENSIONS)
		return (const unsigned char *)extensions;
	const unsigned char *s = n_glGetString(n);
	return s ? s : (const unsigned char *)"";
}

static void make_extensions(void)
{
	static const char *const want[] = { "GL_OES_standard_derivatives", "GL_EXT_texture_filter_anisotropic",
					     "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear",
					     "GL_OES_texture_float", "GL_OES_texture_float_linear", 0 };
	const char *all = (const char *)n_glGetString(GL_EXTENSIONS);
	size_t n = 0;
	extensions[0] = 0;
	for (int i = 0; want[i] && all; i++) {
		size_t l = strlen(want[i]);
		for (const char *p = all; (p = strstr(p, want[i])) != 0; p += l)
			if ((p == all || p[-1] == ' ') && (p[l] == ' ' || p[l] == 0)) {
				if (n + l + 2 < sizeof(extensions)) {
					memcpy(extensions + n, want[i], l);
					n += l;
					extensions[n++] = ' ';
					extensions[n] = 0;
				}
				break;
			}
	}
	if (n)
		extensions[n - 1] = 0;
	LOG("GL_EXTENSIONS for CoreAnimation: \"%s\"\n", extensions);
}

/*
 * Shaders: CoreAnimation generates GLSL ES 1.00 at run time; QuartzCore
 * never asks for compile or link status. Fragment shaders get a default
 * precision after their #extension lines (GLSL ES 1.00 requires one; a
 * later declaration in the source overrides it), and every failure is
 * logged with the source (first 8), the only way to see them.
 */
static unsigned shader_fails;

/*
 * Rectangle textures (boot #61: CoreAnimation's colour lookup filter, "uniform lowp sampler2DRect
 * texture0; ... texture2DRect(texture0, texcoord0_0)", did not compile on Adreno: "Reserved word").
 * The texture is an ordinary 2D texture here (RT()); the shader gets sampler2D and
 * texture2D(S, (coord) * drg_rinv_S), where drg_rinv_S = 1 / the size of the texture bound to S's unit,
 * set before each draw with that program (rect_update).
 */
#define RECT_NAMES 4
#define RECT_PROGS 16
static struct { GLuint prog; int n; GLint samp[RECT_NAMES], rinv[RECT_NAMES]; } rprog[RECT_PROGS];

static int ident_char(char ch)
{
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

static size_t count_of(const char *s, const char *w)
{
	size_t n = 0, l = strlen(w);
	for (const char *p = strstr(s, w); p; p = strstr(p + l, w))
		n++;
	return n;
}

/* a new source with the rectangle samplers rewritten (host_free it), or 0 if there are none or it fails */
static char *rect_rewrite(const char *all)
{
	static const char T[] = "sampler2DRect", F[] = "texture2DRect";
	if (!strstr(all, T))
		return 0;
	char names[RECT_NAMES][32];
	int nn = 0;
	for (const char *p = strstr(all, T); p; p = strstr(p + sizeof(T) - 1, T)) {
		const char *q = p + sizeof(T) - 1;
		while (*q == ' ' || *q == '\t')
			q++;
		size_t l = 0;
		while (ident_char(q[l]))
			l++;
		if (!l || l >= sizeof(names[0]) || nn >= RECT_NAMES) {
			LOG("rect: sampler declaration not understood (or more than %d)\n", RECT_NAMES);
			return 0;
		}
		memcpy(names[nn], q, l);
		names[nn++][l] = 0;
	}
	if (strstr(all, "texture2DRectProj")) {
		LOG("rect: texture2DRectProj not supported\n");
		return 0;
	}
	size_t total = strlen(all), calls = count_of(all, F);
	size_t cap = total + (size_t)nn * 64 + calls * 64 + 1;
	char *out = host_alloc(cap);
	if (!out)
		return 0;
	size_t o = 0;
	int declared = 0;
	for (const char *p = all; *p;) {
		if (!declared && !strncmp(p, T, sizeof(T) - 1)) {
			/* the uniforms go right before the line of the first rectangle sampler (global scope) */
			size_t ls = o;
			while (ls && out[ls - 1] != '\n')
				ls--;
			char decl[RECT_NAMES * 64];
			size_t dl = 0;
			for (int i = 0; i < nn; i++)
				dl += (size_t)snprintf(decl + dl, sizeof(decl) - dl, "uniform highp vec2 drg_rinv_%s;\n", names[i]);
			memmove(out + ls + dl, out + ls, o - ls);
			memcpy(out + ls, decl, dl);
			o += dl;
			declared = 1;
		}
		if (!strncmp(p, T, sizeof(T) - 1) && (p == all || !ident_char(p[-1]))) {
			memcpy(out + o, "sampler2D", 9);
			o += 9;
			p += sizeof(T) - 1;
			continue;
		}
		if (!strncmp(p, F, sizeof(F) - 1) && (p == all || !ident_char(p[-1]))) {
			const char *q = p + sizeof(F) - 1;
			while (*q == ' ' || *q == '\t')
				q++;
			if (*q != '(')
				goto fail;
			q++;
			while (*q == ' ' || *q == '\t')
				q++;
			size_t l = 0;
			while (ident_char(q[l]))
				l++;
			const char *a = q + l;
			while (*a == ' ' || *a == '\t')
				a++;
			if (!l || l >= 32 || *a != ',')
				goto fail;
			/* the coordinate: up to the matching ')' */
			const char *b = a + 1;
			int depth = 0;
			for (; *b && !(depth == 0 && *b == ')'); b++)
				depth += *b == '(' ? 1 : *b == ')' ? -1 : 0;
			if (*b != ')')
				goto fail;
			o += (size_t)snprintf(out + o, cap - o, "texture2D(%.*s, (%.*s) * drg_rinv_%.*s)", (int)l, q,
					     (int)(b - a - 1), a + 1, (int)l, q);
			p = b + 1;
			continue;
		}
		out[o++] = *p++;
	}
	out[o] = 0;
	return out;
fail:
	LOG("rect: texture2DRect call not understood\n");
	host_free(out);
	return 0;
}

/* after a link: the program's drg_rinv_S uniforms and their samplers */
static void rect_link(GLuint p)
{
	static void (*active)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
	if (!active)
		*(void **)&active = drg_elf_sym(0, "glGetActiveUniform");
	for (int i = 0; i < RECT_PROGS; i++)
		if (rprog[i].prog == p)
			rprog[i].prog = 0;	/* relinked: its old entry goes */
	GLint nu = 0;
	n_glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
	int n = 0;
	GLint samp[RECT_NAMES], rinv[RECT_NAMES];
	for (GLint i = 0; active && i < nu && n < RECT_NAMES; i++) {
		char name[64];
		GLsizei l = 0;
		GLint size = 0;
		GLenum type = 0;
		active(p, (GLuint)i, sizeof(name), &l, &size, &type, name);
		if (l <= 9 || strncmp(name, "drg_rinv_", 9))
			continue;
		rinv[n] = n_glGetUniformLocation(p, name);
		samp[n] = n_glGetUniformLocation(p, name + 9);
		if (rinv[n] >= 0 && samp[n] >= 0)
			n++;
	}
	if (!n)
		return;
	int slot = -1;
	for (int i = 0; i < RECT_PROGS && slot < 0; i++)
		if (!rprog[i].prog)
			slot = i;
	if (slot < 0) {
		LOG("rect: more than %d programs with rectangle textures: program %u not tracked\n", RECT_PROGS, p);
		return;
	}
	rprog[slot].prog = p;
	rprog[slot].n = n;
	memcpy(rprog[slot].samp, samp, sizeof(samp));
	memcpy(rprog[slot].rinv, rinv, sizeof(rinv));
	LOG("rect: program %u: %d rectangle sampler(s)\n", p, n);
}

/* before a draw: 1 / size of the texture on each rectangle sampler's unit */
static void rect_update(void)
{
	static void (*getu)(GLuint, GLint, GLint *);
	static void (*level)(GLenum, GLint, GLenum, GLint *);
	static unsigned sizeless;
	if (!cur_prog)
		return;
	for (int i = 0; i < RECT_PROGS; i++) {
		if (rprog[i].prog != cur_prog)
			continue;
		if (!getu)
			*(void **)&getu = drg_elf_sym(0, "glGetUniformiv");
		if (!level)
			*(void **)&level = drg_elf_sym(0, "glGetTexLevelParameteriv");
		if (!getu || !level)
			return;
		GLint act = GL_TEXTURE0;
		n_glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
		for (int k = 0; k < rprog[i].n; k++) {
			GLint unit = 0, w = 0, h = 0;
			getu(cur_prog, rprog[i].samp[k], &unit);
			n_glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
			level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
			level(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
			if (w > 0 && h > 0) {
				GLfloat v[2] = { 1.0f / (GLfloat)w, 1.0f / (GLfloat)h };
				n_glUniform2fv(rprog[i].rinv[k], 1, v);
			} else if (sizeless++ < 8) {
				LOG("rect: program %u unit %d: texture without a size (%dx%d)\n", cur_prog, unit, w, h);
			}
		}
		n_glActiveTexture((GLenum)act);
		return;
	}
}

static void t_glShaderSource(C, GLuint s, GLsizei n, const GLchar *const *src, const GLint *len)
{
	MC;
	GLint type = 0;
	n_glGetShaderiv(s, GL_SHADER_TYPE, &type);
	if (type != GL_FRAGMENT_SHADER || n < 1 || n > 15) {
		n_glShaderSource(s, n, src, len);
		return;
	}
	/* the whole source in one string, then the precision line after the last #extension line */
	size_t total = 0;
	for (int i = 0; i < n; i++)
		total += len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]);
	char *all = host_alloc(total + 64);
	if (!all) {
		n_glShaderSource(s, n, src, len);
		return;
	}
	size_t at = 0;
	for (int i = 0; i < n; i++) {
		size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]);
		memcpy(all + at, src[i], l);
		at += l;
	}
	all[at] = 0;
	char *rect = rect_rewrite(all);
	if (rect) {
		host_free(all);
		all = rect;
		at = total = strlen(all);
	}
	size_t ins = 0;
	for (const char *p = all; *p;) {
		const char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
		const char *q = p;
		while (*q == ' ' || *q == '\t')
			q++;
		if (!strncmp(q, "#extension", 10) || !strncmp(q, "#version", 8))
			ins = (size_t)(p - all) + l;
		p += l;
	}
	static const char prec[] = "precision highp float;\n";
	char *out = host_alloc(total + sizeof(prec) + 2);
	if (!out) {
		host_free(all);
		n_glShaderSource(s, n, src, len);
		return;
	}
	memcpy(out, all, ins);
	if (ins && out[ins - 1] != '\n')
		out[ins++] = '\n';
	memcpy(out + ins, prec, sizeof(prec) - 1);
	memcpy(out + ins + sizeof(prec) - 1, all + ins, at - ins + 1);
	const GLchar *one = out;
	n_glShaderSource(s, 1, &one, 0);
	host_free(out);
	host_free(all);
}

static void log_source(GLuint s)
{
	static char buf[4096];
	GLsizei n = 0;
	void (*get)(GLuint, GLsizei, GLsizei *, GLchar *) = drg_elf_sym(0, "glGetShaderSource");
	if (!get)
		return;
	get(s, sizeof(buf), &n, buf);
	for (char *p = buf; p < buf + n;) {	/* line by line (host_log's buffer is 1 KiB) */
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		LOG("  | %s\n", p);
		p = e ? e + 1 : buf + n;
	}
}

static void t_glCompileShader(C, GLuint s)
{
	MC;
	n_glCompileShader(s);
	GLint ok = 1;
	n_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok && shader_fails++ < 8) {
		char log[512] = "";
		n_glGetShaderInfoLog(s, sizeof(log), 0, log);
		LOG("shader %u does not compile: %s\n", s, log);
		log_source(s);
	}
}

static void t_glLinkProgram(C, GLuint p)
{
	MC;
	n_glLinkProgram(p);
	GLint ok = 1;
	n_glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok && shader_fails++ < 8) {
		char log[512] = "";
		n_glGetProgramInfoLog(p, sizeof(log), 0, log);
		LOG("program %u does not link: %s\n", p, log);
	}
	if (ok)
		rect_link(p);
}
#undef C
#undef MC

/* QuartzCore's offsets (ca-selection.md 2.3) */
static const struct { unsigned off; void *fn; const char *name; } slots[] = {
#define S(off, f) { off, (void *)t_##f, #f },
	S(0x30, glBindTexture) S(0x40, glBlendFunc) S(0x58, glClear) S(0x68, glClearColor)
	S(0x1c8, glCopyTexSubImage2D) S(0x1e0, glDeleteTextures) S(0x200, glDisable) S(0x210, glDrawArrays)
	S(0x220, glDrawElements) S(0x248, glEnable) S(0x2d0, glFinish) S(0x2d8, glFlush) S(0x318, glGenTextures)
	S(0x338, glGetError) S(0x348, glGetIntegerv) S(0x3b0, glGetString) S(0x620, glPixelStorei)
	S(0x7e0, glScissor) S(0x970, glTexImage2D) S(0x978, glTexParameterf) S(0x988, glTexParameteri)
	S(0x9a0, glTexSubImage2D) S(0xa80, glViewport) S(0xa88, glBlendFuncSeparate) S(0xa98, glBlendEquation)
	S(0xab8, glActiveTexture) S(0xbe8, glCompressedTexImage2D) S(0xeb8, glFinishObjectAPPLE)
	S(0x1000, glVertexAttribPointer) S(0x1008, glEnableVertexAttribArray) S(0x1010, glDisableVertexAttribArray)
	S(0x1280, glDeleteObject) S(0x1298, glCreateShader) S(0x12a0, glShaderSource) S(0x12a8, glCompileShader)
	S(0x12b0, glCreateProgram) S(0x12b8, glAttachShader) S(0x12c0, glLinkProgram) S(0x12c8, glUseProgram)
	S(0x12d8, glUniform1f) S(0x12f0, glUniform4f) S(0x12f8, glUniform1i) S(0x1318, glUniform1fv)
	S(0x1320, glUniform2fv) S(0x1328, glUniform3fv) S(0x1330, glUniform4fv) S(0x1360, glUniformMatrix3fv)
	S(0x1368, glUniformMatrix4fv) S(0x1390, glGetUniformLocation) S(0x13b8, glBindAttribLocation)
	S(0x1508, glBindFramebuffer) S(0x1510, glDeleteFramebuffers) S(0x1518, glGenFramebuffers)
	S(0x1530, glFramebufferTexture2D) S(0x1550, glGenerateMipmap) S(0x1860, glFenceSync)
	S(0x1870, glDeleteSync) S(0x1878, glClientWaitSync) S(0x1978, glLabelObjectEXT)
	S(0x1988, glInsertEventMarkerEXT) S(0x1990, glPushGroupMarkerEXT) S(0x1998, glPopGroupMarkerEXT)
	S(0x2008, glFramebufferParameteriAPPLE)
#undef S
};

/*
 * Every other slot: a stub that logs its offset once (the scan behind the
 * 63 has limits, ca-selection.md 2.3) and returns 0. Code made in the
 * loader's JIT arena: movz x9, #slot; ldr x16, 8; br x16; .quad unknown_slot.
 */
static uint8_t slot_logged[TABLE_SLOTS];
__attribute__((used)) static uint64_t unknown_slot(void *c, uint64_t a, uint64_t b, uint64_t d, uint64_t e, uint64_t f, uint64_t g,
			     uint64_t h, uint64_t slot)
{
	(void)c, (void)a, (void)b, (void)d, (void)e, (void)f, (void)g, (void)h;
	if (slot < TABLE_SLOTS && !slot_logged[slot]++)
		LOG("GL table slot 0x%llx called (not provided; returned 0)\n", (unsigned long long)slot * 8);
	return 0;
}

static uint32_t *stubs;

/*
 * The literal sits after the 4 words, so each stub is 24 bytes; the slot
 * number goes in x8 and the common handler takes it from there.
 */
uint64_t unknown_slot_x8(void) __asm__("_unknown_slot_x8");
__asm__(".text\n.globl _unknown_slot_x8\n.private_extern _unknown_slot_x8\n.p2align 2\n_unknown_slot_x8:\n"
	"\tstp x29, x30, [sp, #-32]!\n\tmov x29, sp\n\tstr x8, [sp, #16]\n"
	"\tsub sp, sp, #16\n\tstr x8, [sp]\n"	/* the 9th argument, on the stack */
	"\tbl _unknown_slot\n\tadd sp, sp, #16\n\tldp x29, x30, [sp], #32\n\tret\n");

static int build_stubs(void)
{
	uint32_t *p = host_map(TABLE_SLOTS * 24, HOST_PROT_R | HOST_PROT_W | HOST_PROT_X);
	if (!p)
		return -1;
	for (unsigned i = 0; i < TABLE_SLOTS; i++) {
		uint32_t *t = p + i * 6;
		t[0] = 0xd2800000 | (i << 5) | 8;	/* movz x8, #i */
		t[1] = 0x58000050;			/* ldr x16, pc + 8 */
		t[2] = 0xd61f0200;			/* br x16 */
		t[3] = 0xd503201f;			/* nop */
		*(uint64_t *)(t + 4) = (uint64_t)(uintptr_t)unknown_slot_x8;
	}
	host_sync_icache(p, TABLE_SLOTS * 24);
	stubs = p;
	return 0;
}

static void fill_table(struct ectx *c)
{
	for (unsigned i = 0; i < TABLE_SLOTS; i++)
		c->table[i] = stubs + i * 6;
	for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
		c->table[slots[i].off / 8] = slots[i].fn;
	c->table[0] = c;
}

/* ---- the driver ---- */
/* every import, dlopen and the variadic log calls included, is the bionic layer's (bionic/table.c) */
static void *resolver(const char *name, int call, void *ctx)
{
	(void)call, (void)ctx;
	void *p = bionic_lookup(name);
	if (!p)
		LOG("no import %s\n", name);
	return p;
}

static int load_gl(void)
{
	static const char *const dirs[] = { LIBS_DIR, 0 };
	static const char *const sys[] = { "libc.so", "libm.so", "libdl.so", "liblog.so", "libcutils.so", "libutils.so",
					   "libz.so", "libc++.so", "libhardware.so", "libsync.so", 0 };
	struct drg_elf_config cf = { dirs, sys, resolver, 0, 0 };
	drg_elf_setup(&cf);
	struct drg_dso *egl = drg_elf_open("libEGL_adreno.so");
	struct drg_dso *gles = egl ? drg_elf_open("libGLESv2_adreno.so") : 0;
	if (!egl || !gles) {
		LOG("loading Nokia's GL failed: %s\n", drg_elf_error());
		return -1;
	}
#define LOADGL(ret, name, args) \
	if (!(*(void **)&n_##name = drg_elf_sym(gles, #name))) { LOG("no %s\n", #name); return -1; }
#define LOADEGL(ret, name, args) \
	if (!(*(void **)&n_##name = drg_elf_sym(egl, #name))) { LOG("no %s\n", #name); return -1; }
	GLFUNCS(LOADGL)
	EGLFUNCS(LOADEGL)
	*(void **)&n_eglCreateImageKHR = n_eglGetProcAddress("eglCreateImageKHR");
	*(void **)&n_glEGLImageTargetTexture2DOES = n_eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!n_eglCreateImageKHR || !n_glEGLImageTargetTexture2DOES) {
		LOG("no EGLImage functions\n");
		return -1;
	}
	int major = 0, minor = 0, n = 0;
	dpy = n_eglGetDisplay(0);
	if (!dpy || !n_eglInitialize(dpy, &major, &minor)) {
		LOG("eglInitialize failed: %x\n", n_eglGetError());
		return -1;
	}
	static const int ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
				  EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
	static const int sa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
	if (!n_eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1 || !(pbuf = n_eglCreatePbufferSurface(dpy, cfg, sa))) {
		LOG("EGL config / pbuffer failed: %x\n", n_eglGetError());
		return -1;
	}
	LOG("EGL %d.%d; %u TPIDR_EL0 reads rewritten\n", major, minor, drg_elf_tp_rewrites());
	return 0;
}

/* ---- IOSurfaces as EGLImages (drgwin.c; TWRP-measured path) ---- */
struct simg {
	uint32_t surface;
	EGLImage img;
	int32_t handle[DRGWIN_HANDLE_INTS];
	struct drgwin_anwb anwb;
};
#define NIMG 96
static struct simg *imgs[NIMG];
static unsigned unsupported_logged;

static EGLImage surface_image(IOSurfaceRef s)
{
	uint32_t id = IOSurfaceGetID(s);
	for (int i = 0; i < NIMG; i++)
		if (imgs[i] && imgs[i]->surface == id)
			return imgs[i]->img;
	uint32_t w = (uint32_t)IOSurfaceGetWidth(s), h = (uint32_t)IOSurfaceGetHeight(s);
	uint32_t bpr = (uint32_t)IOSurfaceGetBytesPerRow(s), size = (uint32_t)IOSurfaceGetAllocSize(s);
	uint32_t fourcc = IOSurfaceGetPixelFormat(s);
	int32_t fmt = fourcc == 0x42475241 ? DRGWIN_FMT_BGRA_8888 : fourcc == 0x52474241 ? DRGWIN_FMT_RGBA_8888 : 0;
	size_t planes = IOSurfaceGetPlaneCount(s);
	/*
	 * 'b3a8' (boot #58: backboardd's 1125x2436 display surfaces once on the GPU, 2 planes, bpe 4, bpr 4544,
	 * 13,893,632 B): plane 0 = 4 bytes per pixel at offset 0, plane 1 = an 8-bit alpha plane behind it.
	 * Plane 0 is drawn as BGRA and the alpha plane left alone [E: channel order not measured].
	 */
	if (fourcc == 0x62336138 && planes == 2) {
		fmt = DRGWIN_FMT_BGRA_8888;
		planes = 1;
	}
	if (!fmt || planes > 1 || bpr % 4 || !w || !h || (uint64_t)bpr * h > size) {
		if (unsupported_logged++ < 16)
			LOG("IOSurface %u %ux%u '%c%c%c%c' bpr %u planes %zu: not supported\n", id, w, h, fourcc >> 24,
			    fourcc >> 16 & 0xff, fourcc >> 8 & 0xff, fourcc & 0xff, bpr, IOSurfaceGetPlaneCount(s));
		return 0;
	}
	int slot = -1;
	for (int i = 0; i < NIMG && slot < 0; i++)
		if (!imgs[i])
			slot = i;
	struct simg *m = slot >= 0 ? host_alloc(sizeof(*m)) : 0;
	int fd = m ? kgsl_ios_surface_fd(id) : -1;
	if (fd < 0) {
		host_free(m);
		LOG("IOSurface %u: no slot or token fd\n", id);
		return 0;
	}
	m->surface = id;
	drgwin_handle(m->handle, fd, w, h, bpr / 4, h, fmt, size);
	drgwin_anwb(&m->anwb, m->handle, w, h, bpr / 4, fmt, DRGWIN_USAGE_GPU_RENDER | DRGWIN_USAGE_GPU_TEXTURE);
	b_gralloc_register(m->handle, 0, w, h, bpr / 4, fmt, DRGWIN_USAGE_GPU_TEXTURE);
	static const int ia[] = { EGL_IMAGE_PRESERVED_KHR, 1, EGL_NONE };
	m->img = n_eglCreateImageKHR(dpy, 0, EGL_NATIVE_BUFFER_ANDROID, &m->anwb, ia);
	if (!m->img) {
		LOG("IOSurface %u: eglCreateImageKHR failed: %x\n", id, n_eglGetError());
		b_gralloc_unregister(m->handle);
		host_free(m);
		return 0;
	}
	imgs[slot] = m;
	LOG("IOSurface %u %ux%u '%c%c%c%c' bpr %u size %u: EGLImage\n", id, w, h, fourcc >> 24, fourcc >> 16 & 0xff,
	    fourcc >> 8 & 0xff, fourcc & 0xff, bpr, size);
	return m->img;
}

/* ---- the GPU, once per boot, if the cached self-test verdict says so ---- */
static int gpu_state;	/* 0 not tried, 1 up, -1 off */
static int gpu_mode = -1;	/* the verdict's mode, set at load by decide() */

/* this boot's identity: kern.bootsessionuuid (random per boot, XNU kern_sysctl.c 2769); the phone has no RTC */
static void boot_id(char *b, size_t n)
{
	size_t l = n - 1;
	if (sysctlbyname("kern.bootsessionuuid", b, &l, 0, 0) || l >= n) {
		struct { long sec; int usec; } tv = { 0, 0 };
		size_t tl = sizeof(tv);
		snprintf(b, n, "boottime %ld", sysctlbyname("kern.boottime", &tv, &tl, 0, 0) ? -1L : tv.sec);
		return;
	}
	b[l] = 0;
}

static int read_small(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t r = read(fd, buf, n - 1);
	close(fd);
	buf[r > 0 ? r : 0] = 0;
	return r > 0 ? 0 : -1;
}

/*
 * At load, no waiting: 1 if the GPU may be used in this process. Off unless the cached verdict is
 * "mode N" for this bench, library and kernel, no fault mark is set, and the GPU was not tried in
 * this boot before (a restarted backboardd stays on the CPU, and that restart sets the fault mark).
 */
#ifdef DRG_EAGL_GPU
static volatile int gpu_build = 1;
#else
static volatile int gpu_build = 0;	/* volatile: the GPU path stays in the library (build-eagl.sh checks its imports) */
#endif

static int decide(void)
{
	if (!gpu_build) {
	/*
	 * Root r28: off unless built with -DDRG_EAGL_GPU. Boot #58 (r27): backboardd on the GPU crashed in
	 * CA::Render::iosurface_get_edr_factor (an IOSurface client with no shared state page, read at
	 * 0x30), eglMakeCurrent failed on CoreAnimation's render thread (EGL 3002) and a 2-plane 'b3a8'
	 * surface was not supported; every boot's self-test re-enabled it, so backboardd crashed each boot.
	 */
		LOG("GPU drawing off in this build (DRG_EAGL_GPU not set): CPU drawing\n");
		return 0;
	}
	char text[256], boot[80], prev[80];
	int missing = 0, mode = -1;
	double t0 = now_ms();
	uint64_t key = drg_verdict_key_now(&missing);
	int have = read_small(DRG_VERDICT_FILE, text, sizeof(text)) == 0;
	int fault = access(DRG_FAULT_FILE, 0) == 0;
	enum drg_verdict_use u = drg_verdict_check(have ? text : 0, key, fault, &mode);
	LOG("verdict: key %016llx (%d of 2 files unreadable, %.1f ms), %s, mode %d\n", (unsigned long long)key, missing,
	    now_ms() - t0, drg_verdict_why(u), mode);
	if (u != DRG_VERDICT_USE || mode < 0 || mode > DRG_MODE_MAX) {
		LOG("staying on CPU drawing: nothing replaced\n");
		return 0;
	}
	boot_id(boot, sizeof(boot));
	if (read_small(BOOT_FILE, prev, sizeof(prev)) == 0 && !strcmp(prev, boot)) {
		LOG("GPU already tried in this boot (%s): backboardd restarted, staying on CPU drawing\n", boot);
		mark_fault("backboardd restarted in a boot that used the GPU\n");
		return 0;
	}
	int fd = open(BOOT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, boot, strlen(boot));
		close(fd);
	}
	gpu_mode = mode;
	LOG("GPU drawing allowed with self-test mode %d (boot %s)\n", mode, boot);
	return 1;
}

static int gpu_up(void)
{
	if (gpu_state)
		return gpu_state > 0;
	gpu_state = -1;
	if (gpu_mode < 0)
		return 0;
	/* mode: framing bits 0-1, CPU read-back bits 2-3, flushall bit 4, delay bit 5 (bench -selftest) */
	kgsl_ios_mode(gpu_mode);
	kgsl_ios_dirty_tracking(1);	/* CPU sync: only the surfaces a frame wrote (frame_end) */
	if (load_gl() || build_stubs())
		return 0;
	gpu_state = 1;
	return 1;
}

static struct ectx *ectx_of(id self)
{
	for (int i = 0; i < 8; i++)
		if (ctxs[i] && ctxs[i]->owner == self)
			return ctxs[i];
	return 0;
}

/* ---- EAGLContext methods ---- */
static SEL s_release, s_dealloc;
static IMP super_dealloc;

static id m_initWithAPI(id self, SEL cmd, uint64_t api, id props)
{
	(void)cmd, (void)props;
	LOG("initWithAPI:%llu\n", (unsigned long long)api);
	if (api != 3 || !gpu_up()) {
		((void (*)(id, SEL))objc_msgSend)(self, s_release);
		return 0;
	}
	int slot = -1;
	for (int i = 0; i < 8 && slot < 0; i++)
		if (!ctxs[i])
			slot = i;
	struct ectx *c = slot >= 0 ? host_alloc(sizeof(*c)) : 0;
	static const int xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	EGLContext share = 0;
	for (int i = 0; i < 8 && !share; i++)
		if (ctxs[i])
			share = ctxs[i]->egl;
	if (c)
		c->egl = n_eglCreateContext(dpy, cfg, share, xa);
	if (!c || !c->egl) {
		LOG("eglCreateContext failed: %x\n", n_eglGetError());
		host_free(c);
		((void (*)(id, SEL))objc_msgSend)(self, s_release);
		return 0;
	}
	c->owner = self;
	fill_table(c);
	ctxs[slot] = c;
	make_current(c);
	if (!extensions[0])
		make_extensions();
	GLint units = 0;
	n_glGetIntegerv(0x8872 /* GL_MAX_TEXTURE_IMAGE_UNITS */, &units);
	LOG("EAGLContext %p: ES3 context, GL \"%s\" \"%s\", %d texture units\n", (void *)self,
	    n_glGetString(0x1F01), n_glGetString(0x1F02), units);
	if (!n_eglMakeCurrent(dpy, 0, 0, 0))	/* free for whichever thread draws */
		LOG("initWithAPI: eglMakeCurrent(none) failed: EGL error %x\n", n_eglGetError());
	cur = 0;
	return self;
}

static uint64_t m_API(id self, SEL cmd)
{
	(void)cmd;
	return ectx_of(self) ? 3 : 0;
}

static int m_setParameter(id self, SEL cmd, int32_t p, const int32_t *v)
{
	(void)self, (void)cmd;
	LOG("setParameter:0x%x to:%d (ignored)\n", p, v ? *v : -1);
	return 1;
}

static void *m_getMacroContextPrivate(id self, SEL cmd)
{
	(void)cmd;
	struct ectx *c = ectx_of(self);
	return c ? c->table : 0;
}

/*
 * EAGLContext methods borrow the context and give it back. Boot #58: attachImage: (CoreAnimation
 * attaching the display's surfaces on its setup thread) made the context current there for good, and
 * the render thread's eglMakeCurrent failed from then on with EGL_BAD_ACCESS (0x3002): a context can
 * be current on one thread only. The GL table calls (the render thread's own drawing) keep it.
 */
static struct ectx *borrow(struct ectx *c)
{
	struct ectx *prev = cur;
	make_current(c);
	return prev;
}

static void give_back(struct ectx *c, struct ectx *prev)
{
	if (prev == c || cur != c)
		return;
	if (prev) {
		make_current(prev);
	} else if (n_eglMakeCurrent(dpy, 0, 0, 0)) {
		cur = 0;
	} else {
		LOG("give_back: eglMakeCurrent(none) failed: EGL error %x\n", n_eglGetError());
	}
}

static int bind_surface_current(GLenum target, IOSurfaceRef s, int invert, const char *what)
{
	if (invert) {
		LOG("%s: inverted surfaces not supported\n", what);
		return 0;
	}
	EGLImage img = s ? surface_image(s) : 0;
	if (!img)
		return 0;
	/*
	 * glGetError returns the oldest pending error: boot #60's "attachImage: error 500" for the first
	 * display surface (the second, identical one worked) may have been an earlier call's. Drain first.
	 */
	static unsigned stale_logged;
	for (int i = 0; i < 8; i++) {
		GLenum old = n_glGetError();
		if (!old)
			break;
		if (stale_logged++ < 16)
			LOG("%s: GL error %x pending from an earlier call (cleared)\n", what, old);
	}
	n_glEGLImageTargetTexture2DOES(target, img);
	GLenum e = n_glGetError();
	if (e) {
		LOG("%s: glEGLImageTargetTexture2DOES(target %x, surface %u) error %x\n", what, target, IOSurfaceGetID(s), e);
		return 0;
	}
	if (target == GL_TEXTURE_2D || target == GL_TEXTURE_EXTERNAL_OES)
		tex_set(bound_tex(target), IOSurfaceGetID(s));
	return 1;
}

static int bind_surface(struct ectx *c, GLenum target, IOSurfaceRef s, int invert, const char *what)
{
	struct ectx *prev = borrow(c);
	int ok = cur == c && bind_surface_current(target, s, invert, what);
	give_back(c, prev);
	return ok;
}

/* OpenGLES 0x1d3f51c60: x2 target, x3 the IOSurface (its pixel format read first), x4 invertedRender */
static int m_attachImage(id self, SEL cmd, uint64_t target, IOSurfaceRef s, _Bool inverted)
{
	(void)cmd;
	struct ectx *c = ectx_of(self);
	if (c && s)	/* the display's surfaces: the GPU draws them, the display reads them (kgsl_ios_cpu_sync) */
		kgsl_ios_surface_gpu_written(IOSurfaceGetID(s));
	return c ? bind_surface(c, RT(target), s, inverted, "attachImage") : 0;
}

/*
 * Argument layout read in OpenGLES (-[EAGLContext texImageIOSurface:...] 0x1d3f51efc): x2 surface, x3-x7
 * target, internalFormat, width, height, format; on the stack type at +0 (8 bytes: NSUInteger), plane
 * at +8 (32 bits), invert at +12 (one byte). Darwin packs stack arguments by their own size, so the
 * types below must be exactly these.
 */
static int m_texImageIOSurface(id self, SEL cmd, IOSurfaceRef s, uint64_t target, uint64_t ifmt, uint32_t w,
			       uint32_t h, uint64_t fmt, uint64_t type, uint32_t plane, _Bool invert)
{
	(void)cmd, (void)ifmt, (void)w, (void)h, (void)fmt, (void)type;
	struct ectx *c = ectx_of(self);
	if (!c || plane != 0)
		return 0;
	return bind_surface(c, RT(target), s, invert, "texImageIOSurface");
}

static void m_dealloc(id self, SEL cmd)
{
	struct ectx *c = ectx_of(self);
	if (c) {	/* the EGL context stays (shared objects, display surfaces); the slot is freed */
		LOG("EAGLContext %p deallocated\n", (void *)self);
		for (int i = 0; i < 8; i++)
			if (ctxs[i] == c)
				ctxs[i] = 0;
		if (cur == c)
			cur = 0;
		host_free(c);
	}
	((void (*)(id, SEL))super_dealloc)(self, cmd);
}

static int replace(Class k, const char *sel, IMP fn, const char *types)
{
	SEL s = sel_registerName(sel);
	if (!class_getInstanceMethod(k, s))
		LOG("EAGLContext has no -%s: adding it\n", sel);
	class_replaceMethod(k, s, fn, types);
	return 0;
}

__attribute__((constructor)) static void drgeagl_init(void)
{
	char name[64] = "";
	proc_name(getpid(), name, sizeof(name));
	if (strcmp(name, "backboardd"))
		return;	/* only backboardd (its executable's weak load command; an inherited DYLD_INSERT_LIBRARIES) */
	int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0)
		host_log_to(fd);
	/* also on the console (the kernel log): proof of the insertion even if the file cannot be written */
	int con = open("/dev/console", O_WRONLY);
	if (con >= 0) {
		char m[96];
		int n = snprintf(m, sizeof(m), "drgeagl: loaded into backboardd (pid %d), log fd %d\n", getpid(), fd);
		write(con, m, (size_t)n);
		/* every later line too: boot #60's drgeagl.log was readable only after a TWRP pull (live log, 0921) */
		host_log_tee(con);
	}
	LOG("loaded into backboardd (pid %d)\n", getpid());
	if (!decide())
		return;	/* CPU drawing: EAGLContext untouched, backboardd as without this library */
	Class k = (Class)(void *)&eagl_class;
	Class super = k ? class_getSuperclass(k) : 0;
	s_release = sel_registerName("release");
	s_dealloc = sel_registerName("dealloc");
	super_dealloc = super ? method_getImplementation(class_getInstanceMethod(super, s_dealloc)) : 0;
	if (!k || !super_dealloc) {
		LOG("no EAGLContext class: nothing replaced\n");
		return;
	}
	replace(k, "initWithAPI:properties:", (IMP)m_initWithAPI, "@32@0:8Q16@24");
	replace(k, "API", (IMP)m_API, "Q16@0:8");
	replace(k, "setParameter:to:", (IMP)m_setParameter, "B28@0:8i16^i20");
	replace(k, "getMacroContextPrivate", (IMP)m_getMacroContextPrivate, "^v16@0:8");
	replace(k, "attachImage:toCoreSurface:invertedRender:", (IMP)m_attachImage, "B32@0:8I16^v20B28");
	replace(k, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:",
		(IMP)m_texImageIOSurface, "B56@0:8^v16I24I28i32i36I40I44I48B52");
	replace(k, "dealloc", (IMP)m_dealloc, "v16@0:8");
	LOG("EAGLContext methods replaced\n");
}
