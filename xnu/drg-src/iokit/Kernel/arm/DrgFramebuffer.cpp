/*
 * Built-in IOMobileFramebuffer for DRG: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again.
 *      The panel is an LCD (CTC OTM1911A): black pixels do not switch the
 *      backlight off, so off/on also switch the PM660L WLED module
 *      (DrgBacklight.cpp, drg_backlight_display; patch 0919).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg drgfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 */
#include "DrgSurface.h"

extern "C" int drg_console_muted;     /* osfmk/console/serial_console.c */
extern "C" void drg_perf_fb(uint64_t abs);      /* bsd/kern/drg_perf.c */
extern "C" void drg_backlight_display(int on);  /* DrgBacklight.cpp */

#if defined(DRG)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

enum {
	kDrgFBGetLayerDefaultSurface  = 3,
	kDrgFBSwapBegin               = 4,
	kDrgFBSwapEnd                 = 5,
	kDrgFBSwapWait                = 6,
	kDrgFBGetID                   = 7,
	kDrgFBGetDisplaySize          = 8,
	kDrgFBRequestPowerChange      = 12,
	kDrgFBIsMainDisplay           = 18,
	kDrgFBGetDotPitch             = 28,
	kDrgFBNotifications           = 72,
};

#define DRG_FB_SWAP_REC_SIZE          0x46c
#define DRG_FB_LAYERS                 4
#define DRG_FB_NOTIFY_TYPES           8
#define DRG_FB_NOTIFY_VSYNC           5
#define DRG_FB_VSYNC_US               16667
#define DRG_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * whole frame is scaled onto the Nokia's 1080x2280 panel (OTM1911A): every
 * panel pixel takes the iPhone X pixel at its centre (fMapX/fMapY). Boot
 * #30 showed the earlier centred crop cutting 78 lines off the top and the
 * bottom (status bar, home indicator). Boot-arg drgfb_native=1 presents the
 * real panel size instead.
 */
#define DRG_FB_LOGICAL_W              1125
#define DRG_FB_LOGICAL_H              2436
#define DRG_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	uint16_t     *fMapX, *fMapY;            /* panel pixel -> presented pixel */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	DrgSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[DRG_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
	bool          fPowerOff;      /* RequestPowerChange(0) */
	uint32_t      fFadeStep;      /* fade-out steps done while off */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[DRG_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[DRG_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("DRG FB: no boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("drgfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	if (!native && fWidth <= DRG_FB_LOGICAL_W && fHeight <= DRG_FB_LOGICAL_H) {
		fLogicalW = DRG_FB_LOGICAL_W;
		fLogicalH = DRG_FB_LOGICAL_H;
	}
	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	fMapX = IONew(uint16_t, fWidth);
	fMapY = IONew(uint16_t, fHeight);
	if (fMapX == NULL || fMapY == NULL) {
		IOLog("DRG FB: no scaling tables\n");
		return false;
	}
	for (uint32_t x = 0; x < fWidth; x++) {
		fMapX[x] = (uint16_t)(((2 * x + 1) * fLogicalW) / (2 * fWidth));
	}
	for (uint32_t y = 0; y < fHeight; y++) {
		fMapY[y] = (uint16_t)(((2 * y + 1) * fLogicalH) / (2 * fHeight));
	}
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = drg_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, DRG_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("DRG FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("DRG FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 10;
	PE_parse_boot_argn("drgfb_hz", &hz, sizeof(hz));
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
	fVsync->setTimeoutUS(DRG_FB_VSYNC_US);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("DRG FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);
	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(DRG_FB_NOTIFY_VSYNC, values, 6);
	}
	if (fb->fPowerOff) {
		/* Fade out: halve every channel each vsync, then black, then idle. */
		uint32_t step = fb->fFadeStep;
		if (step <= 6) {
			fb->fFadeStep = step + 1;
		}
		IOLockUnlock(fb->fLock);
		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
		}
		sender->setTimeoutUS(DRG_FB_VSYNC_US);
		return;
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
	uint8_t *rec = NULL;
	if (flush) {
		rec = (uint8_t *)IOMallocData(DRG_FB_SWAP_REC_SIZE);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, DRG_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		uint64_t t0 = mach_absolute_time();
		fb->present(rec);
		drg_perf_fb(mach_absolute_time() - t0);
		IOFreeData(rec, DRG_FB_SWAP_REC_SIZE);
	}
	sender->setTimeoutUS(DRG_FB_VSYNC_US);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t drg_fb_swaps_logged;
static uint64_t drg_fb_swap_count;

/* Copy each enabled layer's surface into the panel memory (no scaling). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * shows the latest frame every fFlushTicks ticks (boot-arg drgfb_hz,
 * default 10 Hz).
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
	IOLockLock(fLock);
	memcpy(fPending, rec, DRG_FB_SWAP_REC_SIZE);
	fDirty = true;
	IOLockUnlock(fLock);
	if ((++drg_fb_swap_count % 1000) == 0) {
		IOLog("DRG FB: %llu swaps\n", drg_fb_swap_count);
	}
}

/*
 * Put one layer's rectangle on the panel (x8r8g8b8, little endian).
 * The display hardware of an iPhone blends its layers; this one did a plain
 * copy per layer, so the lock screen's mostly transparent top layer replaced
 * the wallpaper below it with black (boots #16-#20). Now:
 *   - 'BGRA' (premultiplied, the CoreAnimation output): the bottom layer is
 *     copied, every later layer is blended source-over by its alpha
 *   - 'w30r' (10-bit RGB, wide colour, what PosterBoard renders posters into):
 *     converted to 8 bits per channel, opaque. Assumed layout R 29:20,
 *     G 19:10, B 9:0, extended range 384..894 = 0..1.
 */
#define DRG_FMT_BGRA            0x42475241u     /* 'BGRA' */
#define DRG_FMT_W30R            0x77333072u     /* 'w30r' */

static inline uint32_t
drg_fb_xr10(uint32_t c)
{
	int32_t v = ((int32_t)c - 384) * 255 / 510;
	return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void
drg_fb_blit(uint8_t *dst, uint32_t dst_bpr, const uint8_t *src, uint32_t src_bpr,
    int32_t w, int32_t h, uint32_t fmt, bool blend)
{
	for (int32_t y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)(void *)(dst + (uint64_t)y * dst_bpr);
		const uint32_t *s = (const uint32_t *)(const void *)(src + (uint64_t)y * src_bpr);
		if (fmt == DRG_FMT_W30R) {
			for (int32_t x = 0; x < w; x++) {
				uint32_t v = s[x];
				d[x] = 0xff000000u | drg_fb_xr10((v >> 20) & 0x3ff) << 16 |
				    drg_fb_xr10((v >> 10) & 0x3ff) << 8 | drg_fb_xr10(v & 0x3ff);
			}
		} else if (!blend) {
			memcpy(d, s, (size_t)w * 4);
		} else {
			for (int32_t x = 0; x < w; x++) {
				uint32_t v = s[x], a = v >> 24;
				if (a == 0xff) {
					d[x] = v;
				} else if (a != 0) {
					uint32_t o = d[x], k = 255 - a;
					uint32_t rb = ((o & 0x00ff00ffu) * k + 0x00800080u) >> 8 & 0x00ff00ffu;
					uint32_t g = ((o & 0x0000ff00u) * k + 0x00008000u) >> 8 & 0x0000ff00u;
					d[x] = v + (rb | g);
				}
			}
		}
	}
}

/*
 * One layer onto the panel, scaled: panel pixel (px, py) shows presented
 * pixel (mapx[px], mapy[py]); the layer covers presented [dx, dx + w) x
 * [dy, dy + h) and its pixel there comes from src at (sx - dx, sy - dy) on.
 */
static void
drg_fb_blit_scaled(uintptr_t panel, uint32_t panel_bpr, uint32_t pw, uint32_t ph,
    const uint16_t *mapx, const uint16_t *mapy, const uint8_t *src, uint32_t src_bpr,
    int32_t sx, int32_t sy, int32_t dx, int32_t dy, int32_t w, int32_t h, uint32_t fmt, bool blend)
{
	uint32_t px0 = 0, px1 = 0;
	while (px0 < pw && mapx[px0] < dx) {
		px0++;
	}
	px1 = px0;
	while (px1 < pw && mapx[px1] < dx + w) {
		px1++;
	}
	for (uint32_t py = 0; py < ph; py++) {
		int32_t ly = mapy[py];
		if (ly < dy || ly >= dy + h) {
			continue;
		}
		uint32_t *d = (uint32_t *)(void *)(panel + (uint64_t)py * panel_bpr);
		const uint32_t *s = (const uint32_t *)(const void *)(src + (uint64_t)(sy + ly - dy) * src_bpr);
		for (uint32_t px = px0; px < px1; px++) {
			uint32_t v = s[sx + mapx[px] - dx];
			if (fmt == DRG_FMT_W30R) {
				d[px] = 0xff000000u | drg_fb_xr10((v >> 20) & 0x3ff) << 16 |
				    drg_fb_xr10((v >> 10) & 0x3ff) << 8 | drg_fb_xr10(v & 0x3ff);
			} else if (!blend) {
				d[px] = v;
			} else {
				uint32_t a = v >> 24;
				if (a == 0xff) {
					d[px] = v;
				} else if (a != 0) {
					uint32_t o = d[px], k = 255 - a;
					uint32_t rb = ((o & 0x00ff00ffu) * k + 0x00800080u) >> 8 & 0x00ff00ffu;
					uint32_t g = ((o & 0x0000ff00u) * k + 0x00008000u) >> 8 & 0x0000ff00u;
					d[px] = v + (rb | g);
				}
			}
		}
	}
}

void
AppleCLCD::present(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);


	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("drgfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			drg_console_muted = 1;
		}
	}

	uint32_t drawn = 0;
	for (uint32_t layer = 0; layer < DRG_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		DrgSurface *s = drg_surface_copy(id);
		if (drg_fb_swaps_logged < 16) {
			drg_fb_swaps_logged++;
			IOLog("DRG FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Clip in presented coordinates; the blit scales to the panel. */
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fLogicalW - dx) {
			w = (int32_t)fLogicalW - dx;
		}
		if (h > (int32_t)fLogicalH - dy) {
			h = (int32_t)fLogicalH - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
		if (src != 0 && bpe == 4) {
			drg_fb_blit_scaled(fPixels, fRowBytes, fWidth, fHeight, fMapX, fMapY,
			    (const uint8_t *)(src + s->offset), s->bytesPerRow, sx, sy, dx, dy, w, h,
			    s->pixelFormat, drawn > 0);
			drawn++;
		}
		s->release();
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= DRG_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t drg_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kDrgFBGetLayerDefaultSurface:
		out[0] = drg_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kDrgFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kDrgFBSwapEnd: {
		uint8_t rec[DRG_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < DRG_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kDrgFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kDrgFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kDrgFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kDrgFBGetDotPitch:
		out[0] = DRG_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kDrgFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < DRG_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("DRG FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kDrgFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			IOLockLock(fFB->fLock);
			bool changed = off != fFB->fPowerOff;
			if (changed) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			IOLockUnlock(fFB->fLock);
			IOLog("DRG FB: display %s\n", off ? "off" : "on");
			if (changed) {
				drg_backlight_display(!off);
			}
		}
		break;
	case kDrgFBSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(drg_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			drg_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("DRG FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* DRG */
