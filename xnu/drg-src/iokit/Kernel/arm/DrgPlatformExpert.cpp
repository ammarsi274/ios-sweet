/*
 * Minimal device-tree platform expert for Nokia 6.1 Plus (DRG).
 * Uses XNU's normal platform, power-domain and nub initialization. Hardware
 * interrupt and power-management services are supplied by separate drivers.
 */
#include <pexpert/arm64/board_config.h>

#if defined(DRG)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>

class DrgPlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(DrgPlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(DrgPlatformExpert, IODTPlatformExpert);

/*
 * Power off and restart (patch 0919). Without PE_halt_restart, every iOS
 * shutdown (power-off slider, Restart, the low-battery shutdown) waited 30 s
 * and panicked "Halt/Restart Timed Out" (boot #53, photo).
 *   restart: IMEM restart_reason 0x146bf65c = 0 (ABL's normal start), PSCI
 *     SYSTEM_RESET, PS_HOLD 0x10ac000 = 0 if it returns: the shim's reset,
 *     measured on this phone (BOOTLOG milestone 2).
 *   power off: as HMD msm-poweroff.c do_msm_poweroff + qpnp-power-on.c
 *     qpnp_pon_reset_config: on each PON (PM660 SID 0 primary, PM660L SID 2
 *     secondary, both GEN2: RESET_EN in PS_HOLD_RST_CTL2 0x5b bit 7) clear
 *     RESET_EN, >= 500 us, PS_HOLD_RST_CTL 0x5a[3:0] = 4 (shutdown), set
 *     RESET_EN; then drop PS_HOLD. PSCI SYSTEM_OFF in between (not measured
 *     here; PS_HOLD follows if it returns). TWRP leaves 0x85a/0x85b = 01/80
 *     (primary, warm reset) and 04/00 (secondary).
 * PMIC writes go through the SPMI arbiter's channel only when its ownership
 * table gives the PON to EE 0 (as DrgBacklight.cpp); otherwise PSCI alone.
 * Panics keep their screen (no restart on kPEPanicRestartCPU).
 */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define HR_CORE_PA      0x0800f000u
#define HR_CNFG_PA      0x0800a000u
#define HR_CHNLS_PA     0x08400000u
#define HR_OBS_PA       0x09400000u
#define HR_STRIDE       0x8000u
#define HR_PSHOLD_PA    0x010ac000u
#define HR_IMEM_PA      0x146bf000u     /* restart_reason at +0x65c */
#define HR_PSCI_OFF     0x84000008ul
#define HR_PSCI_RESET   0x84000009ul

struct hr_pon {
	vm_offset_t obs, chan;
	uint32_t ppid;
};
static struct hr_pon hr_pons[2] = { { 0, 0, 0x008 }, { 0, 0, 0x208 } };
static vm_offset_t hr_pshold, hr_imem;

static uint64_t
hr_smc(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
                 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static bool
hr_wait(vm_offset_t ch)
{
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t st = *(volatile uint32_t *)(ch + 0x08u);
		__asm__ volatile ("dmb oshld" ::: "memory");
		if (st & 1u) {
			return !(st & 0x0eu);
		}
		IODelay(1);
	}
	return false;
}

static bool
hr_rd(const struct hr_pon *p, uint8_t reg, uint8_t *v)
{
	*(volatile uint32_t *)p->obs = (1u << 27) | ((uint32_t)reg << 4);
	__asm__ volatile ("dsb osh" ::: "memory");
	if (!hr_wait(p->obs)) {
		return false;
	}
	*v = (uint8_t)*(volatile uint32_t *)(p->obs + 0x18u);
	return true;
}

static bool
hr_wr(const struct hr_pon *p, uint8_t reg, uint8_t v)
{
	*(volatile uint32_t *)(p->chan + 0x10u) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
	*(volatile uint32_t *)p->chan = (uint32_t)reg << 4;
	__asm__ volatile ("dsb osh" ::: "memory");
	return hr_wait(p->chan);
}

static void
hr_setup(void)
{
	vm_offset_t core = ml_io_map(HR_CORE_PA, 0x1000), cnfg = ml_io_map(HR_CNFG_PA, 0x1000);
	hr_pshold = ml_io_map(HR_PSHOLD_PA, 0x1000);
	hr_imem = ml_io_map(HR_IMEM_PA, 0x1000);
	if (!core || !cnfg || *(volatile uint32_t *)core != 0x30000000u) {
		IOLog("DRG POWER: SPMI arbiter not as expected: PSCI only\n");
		return;
	}
	for (unsigned k = 0; k < 2; k++) {
		struct hr_pon *p = &hr_pons[k];
		uint32_t apid = 0xffff;
		for (uint32_t n = 0; n < 512; n++) {
			if (((*(volatile uint32_t *)(core + 0x800u + 4u * n) >> 8) & 0xfffu) == p->ppid) {
				apid = n;
				break;
			}
		}
		uint32_t owner = apid == 0xffff ? 7 : *(volatile uint32_t *)(cnfg + 0x700u + 4u * apid) & 7u;
		if (apid != 0xffff && owner == 0) {
			p->obs = ml_io_map(HR_OBS_PA + HR_STRIDE * apid, 0x1000);
			p->chan = ml_io_map(HR_CHNLS_PA + HR_STRIDE * apid, 0x1000);
		}
		uint8_t ctl = 0xee, ctl2 = 0xee;
		if (p->obs) {
			(void)hr_rd(p, 0x5a, &ctl);
			(void)hr_rd(p, 0x5b, &ctl2);
		}
		IOLog("DRG POWER: PON 0x%03x apid %u owner EE %u, PS_HOLD_RST_CTL %02x/%02x%s\n", p->ppid, apid, owner,
		    ctl, ctl2, p->obs && p->chan ? "" : " (not ours)");
	}
}

static void
hr_pon_type(uint8_t type)
{
	for (unsigned k = 0; k < 2; k++) {
		struct hr_pon *p = &hr_pons[k];
		uint8_t ctl = 0, ctl2 = 0;
		if (!p->obs || !p->chan || !hr_rd(p, 0x5a, &ctl) || !hr_rd(p, 0x5b, &ctl2)) {
			continue;
		}
		(void)hr_wr(p, 0x5b, ctl2 & 0x7f);
		IODelay(500);
		(void)hr_wr(p, 0x5a, (uint8_t)((ctl & 0xf0) | type));
		(void)hr_wr(p, 0x5b, ctl2 | 0x80);
	}
}

static int
drg_halt_restart(unsigned int type)
{
	if (type == kPEHaltCPU) {
		hr_pon_type(4);
		__asm__ volatile ("dsb sy" ::: "memory");
		(void)hr_smc(HR_PSCI_OFF);
	} else if (type == kPERestartCPU) {
		if (hr_imem) {
			*(volatile uint32_t *)(hr_imem + 0x65cu) = 0;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		(void)hr_smc(HR_PSCI_RESET);
	} else {
		return -1;
	}
	if (hr_pshold) {
		*(volatile uint32_t *)hr_pshold = 0;
	}
	for (;;) {
		__asm__ volatile ("wfe");
	}
}

bool
DrgPlatformExpert::start(IOService *provider)
{
	IOLog("DRG: starting device-tree platform expert\n");
	if (!IODTPlatformExpert::start(provider)) {
		return false;
	}
	hr_setup();
	PE_halt_restart = drg_halt_restart;
	registerService();
	IOLog("DRG: platform expert registered\n");
	return true;
}

const char *
DrgPlatformExpert::deleteList(void)
{
	return "()";
}

const char *
DrgPlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'options')";
}

void
DrgPlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/*
	 * The stock implementation requires an Apple NVRAM /options node.
	 * DRG has no NVRAM controller; publish real CPU/device nodes without
	 * inventing one. createNubs adds the topology's logical CPU/cluster IDs.
	 */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != NULL) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, NULL));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));

	/*
	 * No AppleARMIO driver publishes the /arm-io children: do it here so
	 * built-in drivers (the display, AppleCLCD) can match them. The GIC is
	 * driven by the platform code and gets no nub. The nubs hang off this
	 * platform expert: IOPlatformDevice::compareName asks its provider.
	 */
	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}
}

/*
 * hw.target / hw.product come from IODTPlatformExpert: the first root
 * "compatible" string and "model" (D22AP / iPhone10,3 in the ADT), which
 * is what the iPhone userland (MobileGestalt) expects.
 */
#endif /* DRG */
