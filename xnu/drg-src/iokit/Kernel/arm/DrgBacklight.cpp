/*
 * DRG (Nokia 6.1 Plus) screen brightness for iOS (research/reports/backlight.md).
 *
 * iOS side (CoreBrightness in backboardd, 20H392): -[BLControl findBacklight]
 * matches {IOPropertyMatch: {backlight-control: true}}; the brightness
 * modules are built only if the service conforms to "AppleARMBacklight";
 * _DisplayCreateWithID reads IODisplayParameters -> brightness -> value/min/
 * max (SInt32); without "new-backlight-architecture" the legacy path sets
 * IORegistryEntrySetCFProperties(svc, {brightness: int32, FadePeriod: int32})
 * with min + (max - min) * x. No user client.
 *
 * Hardware: the panel (CTC OTM1911A) uses the PM660L WLED (bl_ctrl_wled,
 * 1..0xfff). PM660L = SPMI SID 3, sink block 0xd900 (PPID 0x3d9): per string
 * 0..2 brightness LSB 0xd957/67/77 = L & 0xff, MSB 0xd958/68/78 = L >> 8,
 * then SYNC 0xd947 = 0x0f, >= 1000 us, 0x00 (HMD leds-qpnp-wled.c). The
 * module setup ABL did (enable, sinks, current) is left alone.
 *
 * SPMI arbiter v3 (as drg_keys.c and the shim's drg_l6_enable): APID map at
 * core 0x800f000 + 0x800 + 4n, observer reads at 0x9400000 + 0x8000 * apid,
 * writes through the channel at 0x8400000 + 0x8000 * apid (WDATA0 +0x10,
 * CMD +0 = addr << 4, STATUS +8), only if the arbiter's ownership table
 * (0x800a000 + 0x700 + 4 * apid, bits 2:0) gives the channel to EE 0. Not
 * measured on this phone yet: logged at start; if not ours, the service is
 * still published (so CoreBrightness runs) but writes are skipped.
 */
#if defined(ARM64_BOARD_CONFIG_DRG)
#include <device/device_types.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);   /* as DrgGPU.cpp */

#define BL_CORE_PA      0x0800f000u
#define BL_CNFG_PA      0x0800a000u
#define BL_CHNLS_PA     0x08400000u
#define BL_OBS_PA       0x09400000u
#define BL_STRIDE       0x8000u
#define BL_SINK_PPID    0x3d9u          /* PM660L SID 3, peripheral 0xd9 */
#define BL_MIN          16
#define BL_MAX          4095

class AppleARMBacklight : public IOService {
	OSDeclareDefaultStructors(AppleARMBacklight);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
private:
	bool spmiSetup(void);
	bool rd(uint8_t reg, uint8_t *v);
	bool wr(uint8_t reg, uint8_t v);
	void publish(SInt32 value);
	void apply(SInt32 level);

	vm_offset_t fObs = 0, fChan = 0;
	bool fWritable = false;
	SInt32 fValue = BL_MAX;
};
OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

bool
AppleARMBacklight::spmiSetup(void)
{
	vm_offset_t core = ml_io_map(BL_CORE_PA, 0x1000), cnfg = ml_io_map(BL_CNFG_PA, 0x1000);
	if (!core || !cnfg || *(volatile uint32_t *)core != 0x30000000u) {
		return false;
	}
	uint32_t apid = 0xffff;
	for (uint32_t n = 0; n < 512; n++) {
		if (((*(volatile uint32_t *)(core + 0x800u + 4u * n) >> 8) & 0xfffu) == BL_SINK_PPID) {
			apid = n;
			break;
		}
	}
	if (apid == 0xffff) {
		IOLog("DRG BACKLIGHT: WLED sink (PPID 0x3d9) not in the arbiter's map\n");
		return false;
	}
	uint32_t owner = *(volatile uint32_t *)(cnfg + 0x700u + 4u * apid) & 7u;
	fObs = ml_io_map(BL_OBS_PA + BL_STRIDE * apid, 0x1000);
	fChan = owner == 0 ? ml_io_map(BL_CHNLS_PA + BL_STRIDE * apid, 0x1000) : 0;
	fWritable = fChan != 0;
	IOLog("DRG BACKLIGHT: WLED sink apid %u, owner EE %u: %s\n", apid, owner,
	    fWritable ? "brightness control on" : "not ours, read only");
	return fObs != 0;
}

static bool
bl_wait(vm_offset_t ch)
{
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t st = *(volatile uint32_t *)(ch + 0x08u);
		__asm__ volatile("dmb oshld" ::: "memory");
		if (st & 1u) {
			return !(st & 0x0eu);
		}
		IODelay(1);
	}
	return false;
}

bool
AppleARMBacklight::rd(uint8_t reg, uint8_t *v)
{
	*(volatile uint32_t *)fObs = (1u << 27) | ((uint32_t)reg << 4);
	__asm__ volatile("dsb osh" ::: "memory");
	if (!bl_wait(fObs)) {
		return false;
	}
	*v = (uint8_t)*(volatile uint32_t *)(fObs + 0x18u);
	return true;
}

bool
AppleARMBacklight::wr(uint8_t reg, uint8_t v)
{
	*(volatile uint32_t *)(fChan + 0x10u) = v;
	__asm__ volatile("dsb osh" ::: "memory");
	*(volatile uint32_t *)fChan = (uint32_t)reg << 4;      /* EXT_WRITEL (0), one byte */
	__asm__ volatile("dsb osh" ::: "memory");
	return bl_wait(fChan);
}

void
AppleARMBacklight::apply(SInt32 level)
{
	if (!fWritable) {
		return;
	}
	uint8_t lo = (uint8_t)(level & 0xff), hi = (uint8_t)((level >> 8) & 0xf);
	bool ok = true;
	for (uint8_t s = 0; s < 3; s++) {
		ok = wr((uint8_t)(0x57 + 0x10 * s), lo) && ok;
		ok = wr((uint8_t)(0x58 + 0x10 * s), hi) && ok;
	}
	ok = wr(0x47, 0x0f) && ok;
	IODelay(1000);
	ok = wr(0x47, 0x00) && ok;
	if (!ok) {
		IOLog("DRG BACKLIGHT: WLED write failed at level %d\n", (int)level);
	}
}

void
AppleARMBacklight::publish(SInt32 value)
{
	OSDictionary *params = OSDictionary::withCapacity(1);
	OSDictionary *b = OSDictionary::withCapacity(3);
	OSNumber *vmin = OSNumber::withNumber((unsigned long long)BL_MIN, 32);
	OSNumber *vmax = OSNumber::withNumber((unsigned long long)BL_MAX, 32);
	OSNumber *vcur = OSNumber::withNumber((unsigned long long)(UInt32)value, 32);
	if (params && b && vmin && vmax && vcur) {
		b->setObject("min", vmin);
		b->setObject("max", vmax);
		b->setObject("value", vcur);
		params->setObject("brightness", b);
		setProperty("IODisplayParameters", params);
	}
	OSSafeReleaseNULL(vcur);
	OSSafeReleaseNULL(vmax);
	OSSafeReleaseNULL(vmin);
	OSSafeReleaseNULL(b);
	OSSafeReleaseNULL(params);
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject("brightness")) : NULL;
	if (!n) {
		return kIOReturnSuccess;        /* FadePeriod etc.: accepted, nothing to do */
	}
	SInt32 level = (SInt32)n->unsigned32BitValue();
	if (level < BL_MIN) {
		level = BL_MIN;
	}
	if (level > BL_MAX) {
		level = BL_MAX;
	}
	if (level != fValue) {
		apply(level);
		fValue = level;
		publish(level);
	}
	return kIOReturnSuccess;
}

bool
AppleARMBacklight::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	if (spmiSetup()) {
		uint8_t lo = 0, hi = 0;
		if (rd(0x57, &lo) && rd(0x58, &hi)) {
			fValue = (SInt32)(lo | (hi & 0xf) << 8);
			if (fValue < BL_MIN) {
				fValue = BL_MIN;
			}
		}
		IOLog("DRG BACKLIGHT: string 0 level %d (as ABL left it)\n", (int)fValue);
	} else {
		IOLog("DRG BACKLIGHT: SPMI not reachable; published without control\n");
	}
	setProperty("backlight-control", kOSBooleanTrue);
	publish(fValue);
	registerService();
	return true;
}
#endif /* ARM64_BOARD_CONFIG_DRG */
