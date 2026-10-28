/*
 * DRG (Nokia 6.1 Plus) battery as an iOS power source.
 *
 * Static until the Nokia's fuel gauge (PM660 FG) is ported: 100%, external
 * power, not charging. Every published value says it was not measured
 * (DrgBatteryFallback, DrgBatteryHardwareSampleValid = false). The design
 * capacity is this phone's battery profile from its device tree:
 * qcom,battery-type "DRG_alt_3000mah_100k", qcom,nom-batt-capacity-mah 3000,
 * qcom,max-voltage-uv 4400000. No PMIC register is read or written.
 */
#if defined(ARM64_BOARD_CONFIG_DRG)
#include <device/device_types.h> /* io_buf_ptr_t for private OSKext headers */
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSCollectionIterator.h>

#define DRG_BATTERY_MAH         3000u   /* qcom,nom-batt-capacity-mah */
#define DRG_BATTERY_FULL_MV     4400u   /* qcom,max-voltage-uv */

class DrgBattery : public IOPMPowerSource {
	OSDeclareDefaultStructors(DrgBattery);
public:
	bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
private:
	void putNumber(const char *key, UInt32 value);
	void putBool(const char *key, bool value);
	void publish(void);
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
DrgBattery::publish(void)
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

	/* IOPMCopyBatteryInfo's per-source compatibility dictionary. */
	OSDictionary *legacy = OSDictionary::withCapacity(7);
	if (legacy) {
		const char *keys[] = {kIOBatteryCurrentChargeKey, kIOBatteryCapacityKey,
		    kIOBatteryVoltageKey, kIOBatteryAmperageKey, kIOBatteryCycleCountKey,
		    kIOBatteryFlagsKey};
		UInt32 values[] = {DRG_BATTERY_MAH, DRG_BATTERY_MAH, DRG_BATTERY_FULL_MV, 0, 0,
		    (UInt32)(kIOBatteryInstalled | kIOBatteryChargerConnect)};
		for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
			OSNumber *number = OSNumber::withNumber(values[i], 32);
			if (number) {
				legacy->setObject(keys[i], number);
				number->release();
			}
		}
		setLegacyIOBatteryInfo(legacy);
		legacy->release();
	}
	updateStatus();
	IOLog("DRG BATTERY: static (Nokia %u mAh profile, fuel gauge not ported), 100%% external\n",
	    DRG_BATTERY_MAH);
}

bool
DrgBattery::start(IOService *provider)
{
	if (!IOPMPowerSource::start(provider)) {
		return false;
	}
	/* Publish a complete source BEFORE registerService wakes powerd. */
	publish();
	registerService();
	return true;
}
#endif /* ARM64_BOARD_CONFIG_DRG */
