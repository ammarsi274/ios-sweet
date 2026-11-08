/*
 * DRG (Nokia 6.1 Plus) battery as an iOS power source, read from the PM660's
 * fuel gauge and charger (research/reports/battery.md, from HMD's
 * qpnp-fg-gen3.c, fg-reg.h, smb-reg.h, smb-lib.c and this phone's device
 * tree). Plain register reads over the SPMI arbiter's observer channels (as
 * drg_keys.c: v3 arbiter, APID map at core + 0x800 + 4n, observer of EE 0 at
 * 0x9400000 + 0x8000 * apid); no fuel gauge SRAM, and the only PMIC write is
 * the charger watchdog pet below. PM660 = SPMI SID 0, so a register 0xPPRR is PPID 0x0PP, offset 0xRR.
 *
 *   state of charge  0x4009 (monotonic SOC) and its copy 0x400a, used only
 *                    when equal; 255 -> 100, 0 -> 0, else
 *                    round((raw - 1) * 98 / 253) + 1          (fg-gen3.c 734-787)
 *   voltage          0x41a0..a1, unsigned, 122.070 uV per bit
 *   current          0x41a2..a3, signed, 488.281 uA per bit, + = discharging
 *   temperature      0x4150..51, 11 bits, 0.25 K per bit; valid with 0x4106 bit 0
 *   USB plugged in   0x1310 bit 4 (USBIN plug-in RT_STS)
 *   charger state    0x1006 bits 2:0: 0-4 charging, 5-6 terminated, 7 disabled
 *
 * Polled every 30 s (and the plug-in bit every 5 s). A sample that fails a
 * read or the SOC copy check is dropped: the last good one stays, and until
 * the first good one the old static values (100 %, external) stay with
 * DrgBatteryFallback set. Never publishes 0 % or a critical level from a
 * single sample. The design capacity is this phone's profile in its device
 * tree: "DRG_alt_3000mah_100k" / "DRG_coslight_3000mah_200k", 3000 mAh, 4.4 V.
 *
 * Charger watchdog (patch 0919): boot #53's first start came right after
 * TWRP and ran 87 min with charger state 7 (disabled) on a PC port, 56 % ->
 * 3 %; later cold starts showed state 3. Linux (qpnp-smb2.c smb2_init_hw)
 * leaves WD_CFG 0x1651 = 0x42 (timer on at plug-in, bark interrupt) and
 * 0x1653 = 0x0b, and pets BARK_BITE_WDOG_PET 0x1643 = 1 on each bark
 * (smblib_handle_wdog_bark); the PMIC keeps that across a warm restart. So
 * every 5 s, when 0x1651 bits 1:0 arm the timer, the MISC block (PPID 0x016)
 * gets the same pet, through the arbiter's channel only if EE 0 owns it.
 * That is the only PMIC write. The first sample also logs the charger's
 * enable/status registers, and every charger state change is logged.
 */
#if defined(ARM64_BOARD_CONFIG_DRG)
#include <device/device_types.h> /* io_buf_ptr_t for private OSKext headers */
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <machine/machine_routines.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);   /* as DrgGPU.cpp */

#define DRG_BATTERY_MAH         3000u   /* qcom,nom-batt-capacity-mah */
#define DRG_BATTERY_FULL_MV     4400u   /* qcom,max-voltage-uv */

#define SPMI_CORE_PA            0x0800f000u
#define SPMI_OBS_PA             0x09400000u
#define SPMI_OBS_STRIDE         0x8000u
#define SPMI_CNFG_PA            0x0800a000u
#define SPMI_CHNLS_PA           0x08400000u
#define SPMI_APIDS              512u
#define SPMI_NO_APID            0xffffu

enum { P_SOC, P_BATT, P_CHG, P_USB, P_MISC, P_COUNT };
static const uint16_t drg_bat_ppid[P_COUNT] = { 0x040, 0x041, 0x010, 0x013, 0x016 };

class DrgBattery : public IOPMPowerSource {
	OSDeclareDefaultStructors(DrgBattery);
public:
	bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
private:
	void putNumber(const char *key, UInt32 value);
	void putBool(const char *key, bool value);
	void publishStatic(void);
	void legacy(UInt32 mah, UInt32 mv, SInt32 ma, bool plugged, bool charging);
	bool spmiSetup(void);
	bool rd(unsigned p, uint8_t reg, unsigned bytes, uint32_t *out);
	void sample(bool full);
	void petWatchdog(void);
	static void timerFired(OSObject *owner, IOTimerEventSource *sender);

	vm_offset_t fCore = 0;
	vm_offset_t fObs[P_COUNT] = {};
	vm_offset_t fMiscChan = 0;
	unsigned fChgState = 8;
	unsigned fPets = 0;
	IOWorkLoop *fWL = NULL;
	IOTimerEventSource *fTimer = NULL;
	unsigned fTicks = 0;
	bool fHaveGood = false;
	bool fLastPlugged = true;
	unsigned fPercent = 100;
	bool fLogged = false;
};
OSDefineMetaClassAndStructors(DrgBattery, IOPMPowerSource);

bool
DrgBattery::init(OSDictionary *dictionary)
{
	/* IOCatalogue calls init(dictionary). This XNU's IOPMPowerSource only
	 * overrides init(void): explicitly run it to allocate its PS dictionary
	 * and symbols, then copy the matching personality into the registry. */
	if (!IOPMPowerSource::init()) {
		return false;
	}
	if (dictionary) {
		OSCollectionIterator *it = OSCollectionIterator::withCollection(dictionary);
		if (!it) {
			return false;
		}
		OSObject *object;
		bool ok = true;
		while ((object = it->getNextObject())) {
			const OSSymbol *key = OSDynamicCast(OSSymbol, object);
			if (!key || !setProperty(key, dictionary->getObject(key))) {
				ok = false;
				break;
			}
		}
		it->release();
		if (!ok) {
			return false;
		}
	}
	return true;
}

void
DrgBattery::putNumber(const char *key, UInt32 value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	OSNumber *number = OSNumber::withNumber(value, 32);
	if (symbol && number) {
		setPSProperty(symbol, number);
	}
	OSSafeReleaseNULL(number);
	OSSafeReleaseNULL(symbol);
}

void
DrgBattery::putBool(const char *key, bool value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	if (symbol) {
		setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
		symbol->release();
	}
}

void
DrgBattery::legacy(UInt32 mah, UInt32 mv, SInt32 ma, bool plugged, bool charging)
{
	OSDictionary *d = OSDictionary::withCapacity(7);
	if (!d) {
		return;
	}
	const char *keys[] = {kIOBatteryCurrentChargeKey, kIOBatteryCapacityKey,
	    kIOBatteryVoltageKey, kIOBatteryAmperageKey, kIOBatteryCycleCountKey,
	    kIOBatteryFlagsKey};
	UInt32 flags = kIOBatteryInstalled | (plugged ? kIOBatteryChargerConnect : 0) | (charging ? kIOBatteryCharge : 0);
	UInt32 values[] = {mah, DRG_BATTERY_MAH, mv, (UInt32)ma, 0, flags};
	for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
		OSNumber *number = OSNumber::withNumber(values[i], 32);
		if (number) {
			d->setObject(keys[i], number);
			number->release();
		}
	}
	setLegacyIOBatteryInfo(d);
	d->release();
}

void
DrgBattery::publishStatic(void)
{
	setBatteryInstalled(true);
	setCurrentCapacity(DRG_BATTERY_MAH);
	setMaxCapacity(DRG_BATTERY_MAH);
	setExternalConnected(true);
	setExternalChargeCapable(true);
	setIsCharging(false);
	setAtWarnLevel(false);
	setAtCriticalLevel(false);
	setVoltage(DRG_BATTERY_FULL_MV);
	setAmperage(0);
	setTimeRemaining(-1);   /* unknown; never publish zero minutes */
	setCycleCount(0);
	putBool("FullyCharged", true);
	putBool("AppleRawExternalConnected", true);
	putNumber("AppleRawCurrentCapacity", DRG_BATTERY_MAH);
	putNumber("AppleRawMaxCapacity", DRG_BATTERY_MAH);
	putNumber("DesignCapacity", DRG_BATTERY_MAH);
	putNumber("InstantAmperage", 0);
	putNumber("Temperature", 2500);  /* centidegrees C, not measured */
	putBool("DrgBatteryHardwareSampleValid", false);
	putBool("DrgBatteryFallback", true);
	putNumber("DrgBatteryPercent", 100);
	legacy(DRG_BATTERY_MAH, DRG_BATTERY_FULL_MV, 0, true, false);
	updateStatus();
}

bool
DrgBattery::spmiSetup(void)
{
	fCore = ml_io_map(SPMI_CORE_PA, 0x1000);
	if (!fCore) {
		return false;
	}
	uint32_t version = *(volatile uint32_t *)fCore;
	if (version != 0x30000000u) {   /* measured (drg_keys.c) */
		IOLog("DRG BATTERY: unexpected SPMI arbiter version %#x\n", version);
		return false;
	}
	for (unsigned p = 0; p < P_COUNT; p++) {
		uint32_t apid = SPMI_NO_APID;
		for (uint32_t n = 0; n < SPMI_APIDS; n++) {
			if (((*(volatile uint32_t *)(fCore + 0x800u + 4u * n) >> 8) & 0xfffu) == drg_bat_ppid[p]) {
				apid = n;
				break;
			}
		}
		if (apid == SPMI_NO_APID) {
			IOLog("DRG BATTERY: PPID %#x not in the arbiter's map\n", drg_bat_ppid[p]);
			if (p == P_MISC) {
				continue;       /* only the watchdog pet and its log need it */
			}
			return false;
		}
		fObs[p] = ml_io_map(SPMI_OBS_PA + SPMI_OBS_STRIDE * apid, 0x1000);
		if (!fObs[p]) {
			return false;
		}
		if (p == P_MISC) {
			vm_offset_t cnfg = ml_io_map(SPMI_CNFG_PA, 0x1000);
			uint32_t owner = cnfg ? *(volatile uint32_t *)(cnfg + 0x700u + 4u * apid) & 7u : 7u;
			if (owner == 0) {
				fMiscChan = ml_io_map(SPMI_CHNLS_PA + SPMI_OBS_STRIDE * apid, 0x1000);
			}
			IOLog("DRG BATTERY: charger MISC apid %u, owner EE %u: %s\n", apid, owner,
			    fMiscChan ? "watchdog pet on" : "not ours, no pet");
		}
	}
	return true;
}

void
DrgBattery::petWatchdog(void)
{
	uint32_t cfg = 0;
	if (!fMiscChan || !rd(P_MISC, 0x51, 1, &cfg) || !(cfg & 3u)) {
		return;
	}
	*(volatile uint32_t *)(fMiscChan + 0x10u) = 1;      /* BARK_BITE_WDOG_PET bit 0 */
	__asm__ volatile("dsb osh" ::: "memory");
	*(volatile uint32_t *)fMiscChan = 0x43u << 4;       /* EXT_WRITEL, one byte */
	__asm__ volatile("dsb osh" ::: "memory");
	bool ok = false;
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t st = *(volatile uint32_t *)(fMiscChan + 0x08u);
		__asm__ volatile("dmb oshld" ::: "memory");
		if (st & 1u) {
			ok = !(st & 0x0eu);
			break;
		}
		IODelay(1);
	}
	if (fPets++ == 0 || !ok) {
		IOLog("DRG BATTERY: charger watchdog pet (WD_CFG %#x)%s\n", cfg, ok ? "" : " failed");
	}
}

/* bytes (1..4) from reg of peripheral p through its observer channel, little endian */
bool
DrgBattery::rd(unsigned p, uint8_t reg, unsigned bytes, uint32_t *out)
{
	vm_offset_t ch = fObs[p];
	if (!ch) {
		return false;
	}
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)ch = (1u << 27) | ((uint32_t)reg << 4) | (bytes - 1);
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t st = *(volatile uint32_t *)(ch + 0x08u);
		__asm__ volatile("dmb oshld" ::: "memory");
		if (st & 1u) {
			if (st & 0x0eu) {
				return false;
			}
			uint32_t v = *(volatile uint32_t *)(ch + 0x18u);
			*out = bytes == 4 ? v : v & ((1u << (8 * bytes)) - 1);
			return true;
		}
		IODelay(1);
	}
	return false;
}

void
DrgBattery::sample(bool full)
{
	uint32_t usb = 0;
	if (!rd(P_USB, 0x10, 1, &usb)) {
		return;
	}
	bool plugged = (usb >> 4) & 1;
	if (!full && plugged == fLastPlugged) {
		return;
	}
	fLastPlugged = plugged;

	uint32_t soc = 0, vraw = 0, iraw = 0, traw = 0, tok = 0, chg = 0;
	bool ok = rd(P_SOC, 0x09, 2, &soc) && rd(P_BATT, 0xa0, 2, &vraw) && rd(P_BATT, 0xa2, 2, &iraw) &&
	    rd(P_BATT, 0x50, 2, &traw) && rd(P_BATT, 0x06, 1, &tok) && rd(P_CHG, 0x06, 1, &chg);
	if (!fLogged && ok) {
		uint32_t a = 0, b = 0, c = 0, d = 0;
		(void)rd(P_BATT, 0x5c, 3, &a);   /* Linux writes a3 50 ff (thermistor coefficients) */
		(void)rd(P_BATT, 0x73, 1, &b);   /* Linux writes 1 (external sense resistor) */
		(void)rd(P_SOC, 0x46, 1, &c);    /* bit 7: profile loaded */
		(void)rd(P_BATT, 0x10, 1, &d);   /* bit 3: battery missing */
		IOLog("DRG BATTERY: first sample raw soc %#x v %#x i %#x t %#x tok %#x chg %#x usb %#x; "
		    "therm %#x sense %#x prof %#x missing %#x\n", soc, vraw, iraw, traw, tok, chg, usb, a, b, c, d);
		uint32_t st2 = 0, en = 0, cfg2 = 0, wst = 0, wcfg = 0, wbite = 0, mrt = 0;
		(void)rd(P_CHG, 0x07, 1, &st2);   /* BATTERY_CHARGER_STATUS_2: errors, temperature */
		(void)rd(P_CHG, 0x42, 1, &en);    /* CHARGING_ENABLE_CMD bit 0 */
		(void)rd(P_CHG, 0x51, 1, &cfg2);  /* CHGR_CFG2: enable source/polarity */
		(void)rd(P_MISC, 0x0c, 1, &wst);  /* WDOG_STATUS */
		(void)rd(P_MISC, 0x51, 1, &wcfg); /* WD_CFG */
		(void)rd(P_MISC, 0x53, 1, &wbite);/* SNARL_BARK_BITE_WD_CFG: bit 7 bite disables charging */
		(void)rd(P_MISC, 0x10, 1, &mrt);  /* MISC RT_STS: bit 1 bark */
		IOLog("DRG BATTERY: charger status2 %#x enable %#x cfg2 %#x; watchdog status %#x cfg %#x bite %#x rt %#x\n",
		    st2, en, cfg2, wst, wcfg, wbite, mrt);
		fLogged = true;
	}
	if (!ok || (soc & 0xff) != (soc >> 8)) {
		return;                          /* keep the last good sample */
	}
	uint32_t raw = soc & 0xff;
	unsigned pct = raw == 255 ? 100 : raw == 0 ? 0 : (unsigned)(((raw - 1) * 98 + 126) / 253 + 1);
	if (pct == 0 && fHaveGood && fPercent > 5) {
		return;                          /* a jump to 0: not from one sample */
	}
	unsigned mv = (unsigned)((uint64_t)vraw * 122070 / 1000000);
	int32_t ma = (int32_t)((int64_t)(int16_t)iraw * 488281 / 1000000);
	bool tvalid = tok & 1;
	int32_t centic = tvalid ? (int32_t)(traw & 0x7ff) * 25 - 27315 : 2500;
	unsigned state = chg & 7;
	bool charging = plugged && state <= 4;
	bool full_ = plugged && (state == 5 || state == 6 || pct == 100);
	UInt32 mah = DRG_BATTERY_MAH * pct / 100;

	setCurrentCapacity(mah);
	setMaxCapacity(DRG_BATTERY_MAH);
	setExternalConnected(plugged);
	setExternalChargeCapable(plugged);
	setIsCharging(charging && !full_);
	setAtWarnLevel(pct <= 10);
	setAtCriticalLevel(pct <= 3 && fHaveGood);
	setVoltage(mv);
	setAmperage(-ma);                        /* iOS: positive while charging */
	putBool("FullyCharged", full_);
	putBool("AppleRawExternalConnected", plugged);
	putNumber("AppleRawCurrentCapacity", mah);
	putNumber("AppleRawMaxCapacity", DRG_BATTERY_MAH);
	putNumber("InstantAmperage", (UInt32)(-ma));
	putNumber("Temperature", (UInt32)centic);
	putBool("DrgBatteryHardwareSampleValid", true);
	putBool("DrgBatteryTemperatureValid", tvalid);
	putBool("DrgBatteryFallback", false);
	putNumber("DrgBatteryPercent", pct);
	legacy(mah, mv, -ma, plugged, charging && !full_);
	updateStatus();
	if (!fHaveGood || pct != fPercent || state != fChgState) {
		IOLog("DRG BATTERY: %u%% %u mV %d mA %d.%02d C usb %d charger state %u\n", pct, mv, ma,
		    centic / 100, (centic < 0 ? -centic : centic) % 100, plugged, state);
	}
	fChgState = state;
	fHaveGood = true;
	fPercent = pct;
}

void
DrgBattery::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	DrgBattery *me = OSDynamicCast(DrgBattery, owner);
	if (!me) {
		return;
	}
	me->petWatchdog();
	me->sample(me->fTicks++ % 6 == 0);       /* full every 30 s, plug-in bit every 5 s */
	sender->setTimeoutMS(5000);
}

bool
DrgBattery::start(IOService *provider)
{
	if (!IOPMPowerSource::start(provider)) {
		return false;
	}
	/* Publish a complete source BEFORE registerService wakes powerd. */
	publishStatic();
	if (spmiSetup()) {
		sample(true);
		fWL = IOWorkLoop::workLoop();
		IOWorkLoop *wl = fWL;
		fTimer = IOTimerEventSource::timerEventSource(this, &DrgBattery::timerFired);
		if (wl && fTimer && wl->addEventSource(fTimer) == kIOReturnSuccess) {
			fTimer->setTimeoutMS(5000);
		}
	} else {
		IOLog("DRG BATTERY: fuel gauge not reachable, static values kept\n");
	}
	registerService();
	return true;
}
#endif /* ARM64_BOARD_CONFIG_DRG */
