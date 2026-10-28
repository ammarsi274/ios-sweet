/*
 * DRG (Nokia 6.1 Plus) buttons, polled every 20 ms, read only.
 *
 * Measured on the phone with the shim key watch (drg/BOOTLOG.md boots #21,
 * #22), addresses from this phone's device tree, register layout from HMD's
 * DRG kernel (drivers/spmi/spmi-pmic-arb.c, pinctrl-spmi-gpio.c):
 *   - SPMI PMIC arbiter @800f000, version 0x30000000 (v3): core 0x800f000,
 *     observer 0x9400000. Channel (APID) map at core + 0x800 + 4n, PPID in
 *     bits 19:8. Observer channel of EE 0 at obs + 0x8000 * apid: CMD +0,
 *     STATUS +8 (bit 0 done, 1-3 errors), RDATA0 +0x18. Read command =
 *     EXT_READL (1) << 27 | (addr & 0xff) << 4 | (bytes - 1).
 *   - power: PM660 (SID 0) power-on block 0x800, RT_STS 0x810 bit 0 (kpdpwr)
 *   - volume up: same register, bit 1 (resin, linux,code 0x73)
 *   - volume down: PM660L (SID 2) GPIO 7 = peripheral 0xc6, RT_STS 0x10
 *     bit 0, low while pressed (gpio-keys vol_down, active low)
 * The only register write is the read command on an observer channel. No
 * PMIC configuration is changed. Keys go to DrgHID as 0 power, 1 volume up,
 * 2 volume down. Boot-arg drgkeys=0 turns the driver off.
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_DRG)

#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void drg_hid_key(int key, int down);
void drg_keys_init(void);

#define KEYS_CORE_PA            0x0800f000u
#define KEYS_OBS_PA             0x09400000u
#define KEYS_OBS_STRIDE         0x8000u
#define KEYS_APIDS              512u
#define KEYS_NO_APID            0xffffu

#define KEYS_PON_PPID           0x008u          /* PM660 SID 0, peripheral 0x08 */
#define KEYS_PON_RT_STS         0x10u           /* 0x810 */
#define KEYS_VOLDN_PPID         0x2c6u          /* PM660L SID 2, GPIO 7 */
#define KEYS_GPIO_RT_STS        0x10u           /* 0xc610 */

enum { KEY_POWER, KEY_VOLUP, KEY_VOLDN, KEY_COUNT };

static vm_offset_t keys_core, keys_obs_pon, keys_obs_voldn;
static uint32_t keys_last_status;
static bool keys_started;
static int keys_sleep_chan;

static uint32_t
keys_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static uint16_t
keys_find_apid(uint16_t ppid)
{
	for (uint32_t n = 0; n < KEYS_APIDS; n++) {
		if (((keys_rd(keys_core, 0x800u + 4u * n) >> 8) & 0xfffu) == ppid) {
			return (uint16_t)n;
		}
	}
	return KEYS_NO_APID;
}

/* One byte from register addr of the channel's peripheral. */
static bool
keys_read(vm_offset_t ch, uint8_t addr, uint8_t *out)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)ch = (1u << 27) | ((uint32_t)addr << 4);
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		keys_last_status = keys_rd(ch, 0x08u);
		if (keys_last_status & 1u) {
			if (keys_last_status & 0x0eu) {
				return false;
			}
			*out = (uint8_t)keys_rd(ch, 0x18u);
			return true;
		}
		delay(1);
	}
	return false;
}

static void
keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&keys_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
keys_thread(__unused void *arg, __unused wait_result_t result)
{
	int state[KEY_COUNT] = {0, 0, 0};
	unsigned errors = 0;

	for (;;) {
		uint8_t pon = 0, gpio = 1;
		bool pok = keys_read(keys_obs_pon, KEYS_PON_RT_STS, &pon);
		bool gok = keys_read(keys_obs_voldn, KEYS_GPIO_RT_STS, &gpio);
		int now[KEY_COUNT] = {!!(pon & 1u), !!(pon & 2u), !(gpio & 1u)};
		bool ok[KEY_COUNT] = {pok, pok, gok};

		if (!pok || !gok) {
			if ((errors++ % 250u) == 0) {
				printf("DRG KEYS: read error pon-ok=%d voldn-ok=%d status=%#x\n",
				    pok, gok, keys_last_status);
			}
		} else {
			errors = 0;
		}
		for (int k = 0; k < KEY_COUNT; ++k) {
			if (ok[k] && now[k] != state[k]) {
				state[k] = now[k];
				printf("DRG KEYS: key=%d down=%d\n", k, state[k]);
				drg_hid_key(k, state[k]);
			}
		}
		keys_wait();
	}
}

void
drg_keys_init(void)
{
	int enabled = 1;
	thread_t thread;

	if (keys_started) {
		return;
	}
	keys_started = true;
	(void)PE_parse_boot_argn("drgkeys", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("DRG KEYS: disabled (drgkeys=0)\n");
		return;
	}
	keys_core = ml_io_map(KEYS_CORE_PA, 0x1000);
	if (!keys_core) {
		printf("DRG KEYS: cannot map the SPMI arbiter\n");
		return;
	}
	uint32_t version = keys_rd(keys_core, 0);
	if (version < 0x30000000u || version >= 0x50000000u) {
		printf("DRG KEYS: unexpected SPMI arbiter version %#x (measured 0x30000000)\n", version);
		return;
	}
	uint16_t pon = keys_find_apid(KEYS_PON_PPID);
	uint16_t voldn = keys_find_apid(KEYS_VOLDN_PPID);
	printf("DRG KEYS: SPMI v3, pon apid %u, vol- apid %u\n", pon, voldn);
	if (pon == KEYS_NO_APID || voldn == KEYS_NO_APID) {
		return;
	}
	keys_obs_pon = ml_io_map(KEYS_OBS_PA + KEYS_OBS_STRIDE * pon, 0x1000);
	keys_obs_voldn = ml_io_map(KEYS_OBS_PA + KEYS_OBS_STRIDE * voldn, 0x1000);
	if (!keys_obs_pon || !keys_obs_voldn) {
		printf("DRG KEYS: cannot map the observer channels\n");
		return;
	}
	kern_return_t kr = kernel_thread_start(keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("DRG KEYS: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_DRG */
