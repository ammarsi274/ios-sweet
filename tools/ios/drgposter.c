/*
 * libdrgposter.dylib: PosterBoard's home-screen preview drawn on the CPU when
 * the device has no Metal (research/reports/wallpaper.md).
 *
 * -[PBFHomeScreenPreviewImageBuilder _imageForView:] (PosterBoard.framework,
 * iOS 16.7, 0x1c17cc8fc) renders the view's layer with an offscreen Metal
 * CARenderer into an IOSurface and returns
 * [UIImage imageWithCGImage:[[surface CGImageBuilder] buildCGImage]]
 * (autoreleased; its only caller, the block at 0x1c17cc8b4, hands it to a
 * completion block). MTLCreateSystemDefaultDevice() is nil here (no
 * IOAcceleratorES), so the command queue is nil and building the renderer's
 * options dictionary throws (SIGABRT, boot #53).
 *
 * The replacement, used only while MTLCreateSystemDefaultDevice() is nil
 * (else the original IMP runs): [CATransaction flush], then
 * UIGraphicsImageRenderer at the layer's bounds and contentsScale with
 * [layer renderInContext:], and the same kind of result as the original:
 * an autoreleased UIImage made with +imageWithCGImage: (scale 1, size in
 * pixels = bounds x contentsScale).
 *
 * Loaded into PosterBoard at launch (tools/drg-root-poster.py: a weak load
 * command in its executable; launchd drops DYLD_INSERT_LIBRARIES from Apple
 * jobs). The constructor only swaps one method; Metal is first asked when
 * the method is first called. Log: /private/var/tmp/drgposter.log.
 * Built without an SDK by tools/ios/build-drgposter.sh (declarations below as
 * the SDK has them).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef struct objc_class *Class;
typedef struct objc_object *id;
typedef struct objc_selector *SEL;
typedef struct objc_method *Method;
typedef id (*IMP)(id, SEL, ...);
typedef signed char BOOL;
typedef double CGFloat;
typedef struct { CGFloat x, y, w, h; } CGRect;
typedef struct CGContext *CGContextRef;
typedef struct CGImage *CGImageRef;
struct mach_header;

Class objc_getClass(const char *);
SEL sel_registerName(const char *);
Method class_getInstanceMethod(Class, SEL);
IMP method_getImplementation(Method);
IMP class_replaceMethod(Class, SEL, IMP, const char *);
void objc_msgSend(void);	/* always called through a cast to the exact type */
id MTLCreateSystemDefaultDevice(void);
void _dyld_register_func_for_add_image(void (*)(const struct mach_header *, intptr_t));

int open(const char *, int, ...);
int close(int);
long write(int, const void *, unsigned long);
int getpid(void);
int snprintf(char *, unsigned long, const char *, ...);
int vsnprintf(char *, unsigned long, const char *, va_list);
#define O_WRONLY 0x0001
#define O_APPEND 0x0008
#define O_CREAT 0x0200
#define LOG_FILE "/private/var/tmp/drgposter.log"	/* PosterBoard runs as mobile */

#define CLASS_NAME "PBFHomeScreenPreviewImageBuilder"
#define SEL_NAME "_imageForView:"
#define TYPES "@24@0:8@16"	/* as the call site and the crash symbol give it */

__attribute__((format(printf, 1, 2))) static void logf_(const char *fmt, ...)
{
	char m[192];
	int n = snprintf(m, sizeof(m), "drgposter[%d]: ", getpid());
	if (n < 0 || n >= (int)sizeof(m))
		return;
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(m + n, sizeof(m) - (unsigned long)n, fmt, ap);
	va_end(ap);
	if (k < 0)
		return;
	n += k;
	if (n >= (int)sizeof(m))
		n = (int)sizeof(m) - 1;
	int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		write(fd, m, (unsigned long)n);
		close(fd);
	}
}

#define MSG(ret, ...) ((ret(*)(__VA_ARGS__))objc_msgSend)

static IMP orig_imp;
static int metal = -1;	/* -1 not asked yet, 0 none, 1 a device */
static int logged_calls;

static id cpu_imageForView(id self, SEL cmd, id view)
{
	if (metal < 0) {
		metal = MTLCreateSystemDefaultDevice() != 0;	/* +1 if any; kept: it is the process's device */
		logf_("%s\n", metal ? "Metal device present: Apple's renderer kept" : "no Metal device: CPU rendering");
	}
	if (metal && orig_imp)
		return ((id(*)(id, SEL, id))orig_imp)(self, cmd, view);

	id layer = view ? MSG(id, id, SEL)(view, sel_registerName("layer")) : 0;
	if (!layer) {
		logf_("no view or layer: nil image\n");
		return 0;
	}
	CGRect b = MSG(CGRect, id, SEL)(layer, sel_registerName("bounds"));
	CGFloat scale = MSG(CGFloat, id, SEL)(layer, sel_registerName("contentsScale"));
	if (!(scale > 0))
		scale = 1;
	if (!(b.w >= 1))	/* the original would ask for a 0-pixel IOSurface; draw one point instead */
		b.w = 1;
	if (!(b.h >= 1))
		b.h = 1;

	MSG(void, Class, SEL)(objc_getClass("CATransaction"), sel_registerName("flush"));

	Class fcls = objc_getClass("UIGraphicsImageRendererFormat");
	Class rcls = objc_getClass("UIGraphicsImageRenderer");
	Class icls = objc_getClass("UIImage");
	if (!fcls || !rcls || !icls) {
		logf_("UIKit classes missing: nil image\n");
		return 0;
	}
	id fmt = MSG(id, Class, SEL)(fcls, sel_registerName("preferredFormat"));	/* autoreleased */
	MSG(void, id, SEL, CGFloat)(fmt, sel_registerName("setScale:"), scale);
	MSG(void, id, SEL, BOOL)(fmt, sel_registerName("setOpaque:"), 0);
	id r = MSG(id, Class, SEL)(rcls, sel_registerName("alloc"));
	r = MSG(id, id, SEL, CGRect, id)(r, sel_registerName("initWithBounds:format:"), b, fmt);	/* +1 */
	if (!r) {
		logf_("renderer for %.0fx%.0f @%.1f failed: nil image\n", b.w, b.h, scale);
		return 0;
	}
	SEL s_cg = sel_registerName("CGContext"), s_render = sel_registerName("renderInContext:");
	id drawn = MSG(id, id, SEL, void *)(r, sel_registerName("imageWithActions:"), (void *)^(id ctx) {
		CGContextRef c = MSG(CGContextRef, id, SEL)(ctx, s_cg);
		MSG(void, id, SEL, CGContextRef)(layer, s_render, c);
	});	/* autoreleased */
	MSG(void, id, SEL)(r, sel_registerName("release"));
	CGImageRef cg = drawn ? MSG(CGImageRef, id, SEL)(drawn, sel_registerName("CGImage")) : 0;
	id out = cg ? MSG(id, Class, SEL, CGImageRef)(icls, sel_registerName("imageWithCGImage:"), cg) : 0;	/* autoreleased */
	if (logged_calls < 20) {
		logged_calls++;
		logf_(out ? "CPU preview %.0fx%.0f pt @%.1f\n" : "CPU preview %.0fx%.0f pt @%.1f FAILED (nil image)\n",
		      b.w, b.h, scale);
	}
	return out;
}

static int replaced;

static void try_replace(void)
{
	if (replaced)
		return;
	Class k = objc_getClass(CLASS_NAME);
	if (!k)
		return;
	SEL s = sel_registerName(SEL_NAME);
	Method m = class_getInstanceMethod(k, s);
	if (!m) {
		replaced = 1;
		logf_("-[" CLASS_NAME " " SEL_NAME "] not found: nothing replaced\n");
		return;
	}
	orig_imp = method_getImplementation(m);
	class_replaceMethod(k, s, (IMP)cpu_imageForView, TYPES);
	replaced = 1;
	logf_("-[" CLASS_NAME " " SEL_NAME "] replaced (CPU when no Metal device)\n");
}

static void on_add_image(const struct mach_header *mh, intptr_t slide)
{
	(void)mh;
	(void)slide;
	try_replace();
}

__attribute__((constructor)) static void drgposter_init(void)
{
	logf_("loaded\n");
	try_replace();
	if (!replaced)	/* PosterBoard.framework not loaded yet: retry as images arrive (also called for each loaded one) */
		_dyld_register_func_for_add_image(on_add_image);
}
