/*
 * The core's waits on the XNU clock path (drgpu_os.h under KERNEL), run on
 * the phone's own CPUs from TWRP (arm64 Linux, no reboot). Build and run:
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -DKERNEL -static -pthread -I.. \
 *       clock_test.c ../drgpu_pt.c ../drgpu_rpm.c -o clock_test
 *   adb push clock_test /tmp/ && adb shell /tmp/clock_test
 * drgpu.c is included, so its static branch_on_named is the real one. The
 * clock is CLOCK_MONOTONIC at 19.2 MHz (the phone's timer, fdt.dts
 * clock-frequency 0x124f800 = the ADT timebase XNU uses). A "register" is a
 * word in RAM that a second thread changes after a set delay, as a clock
 * branch clears CLK_OFF once it runs. Cases come from boot #35's log.
 * A success must come before the 20 ms timeout; how much before depends on
 * TWRP scheduling the second thread, not on the core.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../drgpu.c"

#define TB_HZ 19200000ull

static uint64_t ns_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

uint64_t drgpu_os_ticks(void) { return ns_now() * (TB_HZ / 100000) / 10000; }
uint64_t drgpu_os_freq(void) { return TB_HZ; }

static char last[128];
static void tlog(void *ctx, const char *msg) { (void)ctx; printf("    log: %s\n", msg); snprintf(last, sizeof(last), "%s", msg); }

static volatile uint32_t reg __attribute__((aligned(4)));
static uint32_t on_after_us;	/* UINT32_MAX: never */

static void *hw(void *arg)
{
	(void)arg;
	if (on_after_us == UINT32_MAX)
		return NULL;
	uint64_t end = ns_now() + on_after_us * 1000ull;
	while (ns_now() < end)
		;
	__atomic_and_fetch(&reg, ~CBCR_CLK_OFF, __ATOMIC_SEQ_CST);	/* the branch runs */
	return NULL;
}

static int fails;

static void check(const char *what, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	fails += !ok;
}

/* branch_on_named on a register that starts at `start` and runs after `after_us` */
static void branch(const char *what, uint32_t start, uint32_t after_us, int want_rc, uint64_t min_us, uint64_t max_us)
{
	struct drgpu g;
	pthread_t t;
	memset(&g, 0, sizeof(g));
	g.os.log = tlog;
	reg = start;
	on_after_us = after_us;
	pthread_create(&t, NULL, hw, NULL);
	uint64_t t0 = ns_now();
	int rc = branch_on_named(&g, (uintptr_t)&reg, "drgpu: test branch did not start, CBCR before");
	uint64_t us = (ns_now() - t0) / 1000;
	pthread_join(t, NULL);
	printf("  %s: rc %d in %llu us, reg 0x%x\n", what, rc, (unsigned long long)us, reg);
	check(what, rc == want_rc && us >= min_us && us <= max_us);
}

int main(void)
{
	printf("clock %llu Hz, 1 ms = %llu ticks\n", (unsigned long long)drgpu_freq(),
	    (unsigned long long)(drgpu_freq() / 1000));

	/* #35 gcc_gpu_cfg_ahb: 0x20008001, already running -> pass at once */
	branch("running branch (cfg_ahb 0x20008001)", 0x20008001, 0, 0, 0, 20000);
	/* #35 gcc_bimc_gfx: 0x80000000, running right after enable -> pass */
	branch("off branch, on after 50 us (bimc_gfx)", 0x80000000, 50, 0, 40, 20000);
	/* slower than the old 2 ms branch_on budget, inside the 20 ms one */
	branch("off branch, on after 8 ms", 0x80000000, 8000, 0, 7900, 20000);
	/* a branch that never starts: fails only after the full 20 ms */
	branch("branch never starts: 20 ms timeout", 0x80000000, UINT32_MAX, -1, 19900, 40000);
	check("failure log names the branch", strstr(last, "CBCR after 20 ms") != NULL);

	/* drgpu_poll: a long success is the time taken, never negative */
	reg = 0x80000000;
	on_after_us = 300000;
	pthread_t t;
	pthread_create(&t, NULL, hw, NULL);
	int r = drgpu_poll((uintptr_t)&reg, CBCR_CLK_OFF, 0, 2000000);
	pthread_join(t, NULL);
	printf("  poll after 300 ms: %d us\n", r);
	check("poll success after 300 ms returns ~300000", r >= 299000 && r <= 330000);

	/* udelay and the RPM ack budget (drgpu_rpm.c: freq / 2) */
	uint64_t t0 = ns_now();
	drgpu_udelay(2000);
	uint64_t us = (ns_now() - t0) / 1000;
	printf("  udelay(2000): %llu us\n", (unsigned long long)us);
	check("udelay(2000) waits 2 ms", us >= 1990 && us <= 4000);
	t0 = ns_now();
	uint64_t end = drgpu_ticks() + drgpu_freq() / 2;
	while (drgpu_ticks() < end)
		;
	us = (ns_now() - t0) / 1000;
	printf("  RPM budget freq/2: %llu us\n", (unsigned long long)us);
	check("RPM ack budget is 0.5 s", us >= 499000 && us <= 510000);

	printf("%s: %d failure(s)\n", fails ? "RESULT FAIL" : "RESULT PASS", fails);
	return fails != 0;
}
