/*
 * rect-probe: check libdrgeagl's rectangle-texture rewrite (eagl.c rect_rewrite, built in from
 * rect_gen.h, extracted from eagl.c by build-rect.sh) against Nokia's Adreno driver in TWRP,
 * before it goes into a root image. Boot #61: CoreAnimation's sampler2DRect shader did not compile.
 *   1 the boot #61 shader, rewritten: compiles and links; drg_rinv_texture0 is an active uniform
 *   2 a rewritten rectangle sample draws a 50x30 NPOT texture back pixel for pixel, with
 *     drg_rinv from glGetTexLevelParameteriv (as rect_update)
 *   3 the same through an EGLImage-backed texture (as backboardd's IOSurfaces): size and pixels
 *   4 what Adreno says to GL_TEXTURE_RECTANGLE (0x84F5) itself: the error code, for the record
 * Same start-up and bionic use as gles-probe.c; our own code.
 */

typedef unsigned long size_t;
typedef unsigned int u32;

extern void __libc_init(void *raw_args, void (*onexit)(void), int (*slingshot)(int, char **, char **),
			const void *structors);
extern int printf(const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern int fflush(void *);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memmove(void *, const void *, size_t);
extern size_t strlen(const char *);
extern int strncmp(const char *, const char *, size_t);
extern char *strstr(const char *, const char *);
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100

extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
extern void abort(void);

/* as gles-probe.c: Android's log printed (no logd in TWRP); gralloc's newer-libcutils import */
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
	printf("ASSERT %s %s: %s\n", cond ? cond : "-", tag ? tag : "-", fmt ? fmt : "");
	fflush(0);
	abort();
}

__attribute__((visibility("default"))) unsigned long atrace_get_enabled_tags(void)
{
	return 0;
}

#define LOG(...) printf("rewrite: " __VA_ARGS__)
#define host_alloc malloc
#define host_free free
#define RECT_NAMES 4
#include "rect_gen.h"

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLImage;
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#define EGL_GL_TEXTURE_LEVEL_KHR 0x30BC
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_RECTANGLE 0x84F5
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5

static void *gles, *egl;
#define F(ret, name, args) static ret(*name) args;
#define L(lib, name)                                                   \
	do {                                                           \
		*(void **)&name = dlsym(lib, #name);                   \
		if (!name) {                                           \
			printf("FAIL: no symbol %s\n", #name);         \
			return 1;                                      \
		}                                                      \
	} while (0)
F(EGLDisplay, eglGetDisplay, (void *))
F(int, eglInitialize, (EGLDisplay, int *, int *))
F(int, eglChooseConfig, (EGLDisplay, const int *, EGLConfig *, int, int *))
F(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const int *))
F(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const int *))
F(int, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
F(int, eglGetError, (void))
F(int, eglTerminate, (EGLDisplay))
F(void *, eglGetProcAddress, (const char *))
F(EGLImage, eglCreateImageKHR, (EGLDisplay, EGLContext, u32, void *, const int *))
F(void, glEGLImageTargetTexture2DOES, (u32, EGLImage))
F(const char *, glGetString, (u32))
F(void, glClearColor, (float, float, float, float))
F(void, glClear, (u32))
F(void, glFinish, (void))
F(void, glViewport, (int, int, int, int))
F(void, glReadPixels, (int, int, int, int, u32, u32, void *))
F(u32, glGetError, (void))
F(u32, glCreateShader, (u32))
F(void, glShaderSource, (u32, int, const char *const *, const int *))
F(void, glCompileShader, (u32))
F(void, glGetShaderiv, (u32, u32, int *))
F(void, glGetShaderInfoLog, (u32, int, int *, char *))
F(u32, glCreateProgram, (void))
F(void, glAttachShader, (u32, u32))
F(void, glBindAttribLocation, (u32, u32, const char *))
F(void, glLinkProgram, (u32))
F(void, glGetProgramiv, (u32, u32, int *))
F(void, glGetProgramInfoLog, (u32, int, int *, char *))
F(void, glUseProgram, (u32))
F(void, glVertexAttribPointer, (u32, int, u32, unsigned char, int, const void *))
F(void, glEnableVertexAttribArray, (u32))
F(void, glDrawArrays, (u32, int, int))
F(void, glGenTextures, (int, u32 *))
F(void, glBindTexture, (u32, u32))
F(void, glActiveTexture, (u32))
F(void, glTexImage2D, (u32, int, int, int, int, int, u32, u32, const void *))
F(void, glTexParameteri, (u32, u32, int))
F(void, glPixelStorei, (u32, int))
F(void, glGetTexLevelParameteriv, (u32, int, u32, int *))
F(void, glGetActiveUniform, (u32, u32, int, int *, int *, u32 *, char *))
F(int, glGetUniformLocation, (u32, const char *))
F(void, glUniform1i, (int, int))
F(void, glUniform2fv, (int, int, const float *))

#define TW 50
#define TH 30
static unsigned char tex[TW * TH * 4], px[TW * TH * 4];

/* the boot #61 shader as CoreAnimation sent it (logs/boot61/drgeagl.log, without our precision line) */
static const char ca_lut[] =
	"#define NOISE_SIZE 32\n"
	"\n"
	"uniform highp vec4 texmat0, texmat1, texmat2;\n"
	"uniform lowp sampler2DRect texture0;\n"
	"uniform lowp sampler2D texture1;\n"
	"uniform mediump vec4 colorP0[1];\n"
	"varying highp vec2 texcoord0_0;\n"
	"\n"
	"void main(void) {\n"
	"  lowp vec4 t0 = texture2DRect(texture0, texcoord0_0);\n"
	"  lowp vec4 s0 = vec4(t0.rgb, 1.);\n"
	"  {\n"
	"    lowp vec3 c = s0.rgb, t;\n"
	"    c.b = c.b * colorP0[0].z;\n"
	"    lowp float b_mix = fract(c.b);\n"
	"    c.b = floor(c.b) * colorP0[0].w;\n"
	"    mediump vec2 c_st = vec2(c.r, c.g);\n"
	"    c_st.r = c_st.r * colorP0[0].x + colorP0[0].y;\n"
	"    c_st.g = c_st.g * colorP0[0].x * colorP0[0].w + colorP0[0].y * colorP0[0].w + c.b;\n"
	"    lowp vec3 c0 = texture2D(texture1, c_st).rgb;\n"
	"    lowp vec3 c1 = texture2D(texture1, c_st + vec2(0., colorP0[0].w)).rgb;\n"
	"    c = mix(c0, c1, b_mix);\n"
	"    s0 = vec4(c.rgb, 1.);\n"
	"  }\n"
	"  gl_FragData[0] = s0;\n"
	"}\n";

static const char sample_rect[] =
	"uniform lowp sampler2DRect tex;\n"
	"varying highp vec2 tc;\n"
	"void main(void) { gl_FragData[0] = texture2DRect(tex, tc); }\n";

/* texcoord in texels, as CoreAnimation gives rectangle textures: centres 0.5 .. size - 0.5 */
static const char vs_src[] =
	"attribute vec2 p;\n"
	"varying highp vec2 tc;\n"
	"varying highp vec2 texcoord0_0;\n"
	"uniform highp vec2 size;\n"
	"void main() { tc = (p * 0.5 + 0.5) * size; texcoord0_0 = tc; gl_Position = vec4(p, 0.0, 1.0); }\n";

static u32 shader(u32 type, const char *src, const char *what)
{
	char *r = type == GL_FRAGMENT_SHADER ? rect_rewrite(src) : 0;
	static char full[8192];
	/* as t_glShaderSource: a default precision first (the rewrite comes first there too) */
	snprintf(full, sizeof(full), "%s%s", type == GL_FRAGMENT_SHADER ? "precision highp float;\n" : "",
		 r ? r : src);
	if (type == GL_FRAGMENT_SHADER)
		printf("%s: rewritten: %s\n", what, r ? "yes" : "NO");
	free(r);
	u32 s = glCreateShader(type);
	const char *one = full;
	int ok = 0;
	glShaderSource(s, 1, &one, 0);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), 0, log);
		printf("FAIL: %s: compile: %s\n--- source ---\n%s--- end ---\n", what, log, full);
		return 0;
	}
	printf("%s: compiles\n", what);
	return s;
}

static u32 program(const char *fs_src, const char *what, int *rinv, int *samp)
{
	u32 vs = shader(GL_VERTEX_SHADER, vs_src, "vertex"), fs = shader(GL_FRAGMENT_SHADER, fs_src, what);
	if (!vs || !fs)
		return 0;
	u32 p = glCreateProgram();
	int ok = 0, nu = 0;
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetProgramInfoLog(p, sizeof(log), 0, log);
		printf("FAIL: %s: link: %s\n", what, log);
		return 0;
	}
	/* as rect_link: the drg_rinv_ uniforms among the active ones */
	*rinv = *samp = -1;
	glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
	for (int i = 0; i < nu; i++) {
		char name[64];
		int l = 0, size = 0;
		u32 type = 0;
		glGetActiveUniform(p, (u32)i, sizeof(name), &l, &size, &type, name);
		if (l > 9 && !strncmp(name, "drg_rinv_", 9)) {
			*rinv = glGetUniformLocation(p, name);
			*samp = glGetUniformLocation(p, name + 9);
			printf("%s: links; active uniform %s at %d, sampler %s at %d\n", what, name, *rinv, name + 9, *samp);
		}
	}
	if (*rinv < 0 || *samp < 0) {
		printf("FAIL: %s: no drg_rinv_ uniform with its sampler\n", what);
		return 0;
	}
	return p;
}

/* draw texture `t` (bound on unit 0) through the rewritten rectangle sample, compare with tex[] */
static int draw_check(u32 p, int rinv, int samp, u32 t, const char *what)
{
	int w = 0, h = 0;
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, t);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	printf("%s: glGetTexLevelParameteriv size %dx%d (expected %dx%d): %s\n", what, w, h, TW, TH,
	       w == TW && h == TH ? "OK" : "WRONG");
	if (w != TW || h != TH)
		return 1;
	glUseProgram(p);
	glUniform1i(samp, 0);
	float inv[2] = { 1.0f / (float)w, 1.0f / (float)h }, size[2] = { (float)TW, (float)TH };
	glUniform2fv(rinv, 1, inv);
	glUniform2fv(glGetUniformLocation(p, "size"), 1, size);
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glViewport(0, 0, TW, TH);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, px);
	int bad = 0, first = -1;
	for (int i = 0; i < TW * TH * 4; i++)
		if (px[i] != tex[i] && !bad++)
			first = i / 4;
	printf("%s: %d of %d bytes differ from the texture%s", what, bad, TW * TH * 4, bad ? "" : ": OK\n");
	if (bad)
		printf(" (first at pixel %d,%d: got %02x%02x%02x%02x want %02x%02x%02x%02x)\n", first % TW, first / TW,
		       px[first * 4], px[first * 4 + 1], px[first * 4 + 2], px[first * 4 + 3], tex[first * 4],
		       tex[first * 4 + 1], tex[first * 4 + 2], tex[first * 4 + 3]);
	return bad != 0;
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
	L(egl, eglGetDisplay); L(egl, eglInitialize); L(egl, eglChooseConfig); L(egl, eglCreatePbufferSurface);
	L(egl, eglCreateContext); L(egl, eglMakeCurrent); L(egl, eglGetError); L(egl, eglTerminate);
	L(egl, eglGetProcAddress);
	L(gles, glGetString); L(gles, glClearColor); L(gles, glClear); L(gles, glFinish); L(gles, glViewport);
	L(gles, glReadPixels); L(gles, glGetError); L(gles, glCreateShader); L(gles, glShaderSource);
	L(gles, glCompileShader); L(gles, glGetShaderiv); L(gles, glGetShaderInfoLog); L(gles, glCreateProgram);
	L(gles, glAttachShader); L(gles, glBindAttribLocation); L(gles, glLinkProgram); L(gles, glGetProgramiv);
	L(gles, glGetProgramInfoLog); L(gles, glUseProgram); L(gles, glVertexAttribPointer);
	L(gles, glEnableVertexAttribArray); L(gles, glDrawArrays); L(gles, glGenTextures); L(gles, glBindTexture);
	L(gles, glActiveTexture); L(gles, glTexImage2D); L(gles, glTexParameteri); L(gles, glPixelStorei);
	L(gles, glGetTexLevelParameteriv); L(gles, glGetActiveUniform); L(gles, glGetUniformLocation);
	L(gles, glUniform1i); L(gles, glUniform2fv);
	*(void **)&eglCreateImageKHR = eglGetProcAddress("eglCreateImageKHR");
	*(void **)&glEGLImageTargetTexture2DOES = eglGetProcAddress("glEGLImageTargetTexture2DOES");

	EGLDisplay d = eglGetDisplay(0);
	if (!d || !eglInitialize(d, &major, &minor)) {
		printf("FAIL: eglInitialize, error %x\n", eglGetError());
		return 1;
	}
	static const int ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
				  EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
	if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) {
		printf("FAIL: eglChooseConfig, error %x n %d\n", eglGetError(), n);
		return 1;
	}
	static const int sa[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
	static const int xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };	/* backboardd: initWithAPI:3 */
	EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
	EGLContext c = eglCreateContext(d, cfg, 0, xa);
	if (!s || !c || !eglMakeCurrent(d, s, s, c)) {
		printf("FAIL: surface %p context %p, error %x\n", s, c, eglGetError());
		return 1;
	}
	printf("GL \"%s\" \"%s\"\n", glGetString(0x1F01), glGetString(0x1F02));

	/* 1: CoreAnimation's boot #61 shader */
	int rinv = -1, samp = -1;
	fails += !program(ca_lut, "boot61 lut shader", &rinv, &samp);

	/* 2: a 50x30 texture drawn back through a rewritten rectangle sample */
	for (int y = 0; y < TH; y++)
		for (int x = 0; x < TW; x++) {
			unsigned char *q = tex + (y * TW + x) * 4;
			q[0] = (unsigned char)(x * 5), q[1] = (unsigned char)(y * 8), q[2] = (unsigned char)(x ^ y), q[3] = 255;
		}
	u32 t[2];
	glGenTextures(2, t);
	glBindTexture(GL_TEXTURE_2D, t[0]);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
	for (u32 k = 0; k < 2; k++) {
		glBindTexture(GL_TEXTURE_2D, t[k]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	u32 p = program(sample_rect, "rect sample", &rinv, &samp);
	if (!p)
		return fails + 1;
	fails += draw_check(p, rinv, samp, t[0], "plain texture");

	/* 3: the same pixels through an EGLImage (texture 0 as the source, texture 1 its target) */
	if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
		printf("FAIL: no EGLImage functions\n");
		fails++;
	} else {
		static const int ia[] = { EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE };
		EGLImage img = eglCreateImageKHR(d, c, EGL_GL_TEXTURE_2D_KHR, (void *)(unsigned long)t[0], ia);
		glBindTexture(GL_TEXTURE_2D, t[1]);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
		u32 e = glGetError();
		printf("EGLImage %p, glEGLImageTargetTexture2DOES error %x\n", img, e);
		fails += !img || e || draw_check(p, rinv, samp, t[1], "EGLImage texture");
	}

	/* 4: Adreno and the rectangle target itself (what libdrgeagl now maps away) */
	while (glGetError())
		;
	glBindTexture(GL_TEXTURE_RECTANGLE, t[0]);
	printf("glBindTexture(GL_TEXTURE_RECTANGLE) error %x (record only)\n", glGetError());
	printf("GL error at the end %x\n", glGetError());
	eglTerminate(d);
	return fails;
}

static int main_(int argc, char **argv, char **envp)
{
	(void)argc, (void)argv, (void)envp;
	int fails = run();
	printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	fflush(0);
	return fails ? 1 : 0;
}

__attribute__((used)) static void start_c(void *raw_args)
{
	static void *structors[6];
	__libc_init(raw_args, 0, main_, structors);
}
__asm__(".text\n.globl _start\n.type _start,%function\n_start:\n"
	"mov x29, #0\nmov x30, #0\nmov x0, sp\nb start_c\n");
