/*
 * DRG (Nokia 6.1 Plus): performance counters (drg/EFFICIENCY-PLAN.md, Phase 0).
 *
 * Measures, does not change behaviour. Every 60 s (boot-arg drg_perf=N
 * seconds, 0 = off) the kernel log gets one "DRG PERF:" report:
 *   - the clock of the core the report runs on: PMU cycle counter
 *     (PMCCNTR_EL0) against the generic timer over 10 ms
 *   - user LSE/LDAPR emulations (osfmk/arm64/drg_lse_emul.c): total, and
 *     the busiest user PCs with the last process that hit each
 *   - the processes that used the most CPU in the period (user, system)
 *   - eMMC traffic and time spent in the driver (bsd/dev/drg_emmc.c)
 *   - frames copied to the panel and their cost (DrgFramebuffer.cpp)
 * Counters are per period: each report starts them from zero.
 */
#include <pexpert/arm64/board_config.h>

#if defined(DRG)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/bsdtask_info.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/cpu_number.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>

#define DRG_PERF_PCS            512     /* power of two */
#define DRG_PERF_PROCS          1024    /* power of two */
#define DRG_PERF_PROBE          16
#define DRG_PERF_TOP            8

void drg_perf_lse_hit(uint64_t pc);
void drg_perf_emmc(int write, uint64_t bytes, uint64_t abs);
void drg_perf_fb(uint64_t abs);

static struct {
	uint64_t pc;
	uint32_t count;
	int32_t pid;
} lse_pcs[DRG_PERF_PCS];
static uint64_t lse_total, lse_untracked;
static uint64_t emmc_bytes[2], emmc_abs[2], emmc_ops[2];
static uint64_t fb_frames, fb_abs, fb_max_abs;

static struct {
	int32_t pid;
	uint64_t user, sys;
} proc_seen[DRG_PERF_PROCS];

struct drg_perf_top {
	int32_t pid;
	uint64_t user, sys;     /* mach time used in the period */
	char name[20];
};
static struct drg_perf_top proc_top[DRG_PERF_TOP];

static thread_call_t drg_perf_call;
static uint32_t drg_perf_period = 60;

/* sleh.c, after an emulated instruction; pc is the instruction's address. */
void
drg_perf_lse_hit(uint64_t pc)
{
	uint32_t h = (uint32_t)((pc >> 2) * 0x9e3779b1u);

	os_atomic_inc(&lse_total, relaxed);
	for (uint32_t i = 0; i < DRG_PERF_PROBE; i++) {
		uint32_t k = (h + i) & (DRG_PERF_PCS - 1);
		uint64_t cur = os_atomic_load(&lse_pcs[k].pc, relaxed);
		if (cur == 0 && os_atomic_cmpxchg(&lse_pcs[k].pc, 0, pc, relaxed)) {
			cur = pc;
		} else if (cur == 0) {
			cur = os_atomic_load(&lse_pcs[k].pc, relaxed);
		}
		if (cur == pc) {
			os_atomic_inc(&lse_pcs[k].count, relaxed);
			lse_pcs[k].pid = proc_selfpid();
			return;
		}
	}
	os_atomic_inc(&lse_untracked, relaxed);
}

void
drg_perf_emmc(int write, uint64_t bytes, uint64_t abs)
{
	int w = write ? 1 : 0;
	os_atomic_add(&emmc_bytes[w], bytes, relaxed);
	os_atomic_add(&emmc_abs[w], abs, relaxed);
	os_atomic_inc(&emmc_ops[w], relaxed);
}

void
drg_perf_fb(uint64_t abs)
{
	os_atomic_inc(&fb_frames, relaxed);
	os_atomic_add(&fb_abs, abs, relaxed);
	if (abs > fb_max_abs) {
		fb_max_abs = abs;
	}
}

static uint64_t
abs_to_us(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_USEC;
}

/* MHz of the current core: PMU cycles over 10 ms of the generic timer. */
static uint32_t
core_mhz(uint32_t *cpu, uint64_t *mpidr)
{
	uint64_t span, t0, t1, c0, c1;
	boolean_t istate = ml_set_interrupts_enabled(FALSE);

	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &span);
	*cpu = (uint32_t)cpu_number();
	*mpidr = __builtin_arm_rsr64("MPIDR_EL1") & 0xffffff;
	/* PMCR.E on, PMCR.D off (count every cycle), cycle counter enabled */
	__builtin_arm_wsr64("PMCR_EL0", (__builtin_arm_rsr64("PMCR_EL0") | 1) & ~(1ULL << 3));
	__builtin_arm_wsr64("PMCNTENSET_EL0", 1ULL << 31);
	__builtin_arm_isb(ISB_SY);
	t0 = ml_get_timebase();
	c0 = __builtin_arm_rsr64("PMCCNTR_EL0");
	do {
		t1 = ml_get_timebase();
	} while (t1 - t0 < span);
	c1 = __builtin_arm_rsr64("PMCCNTR_EL0");
	ml_set_interrupts_enabled(istate);
	uint64_t us = abs_to_us(t1 - t0);
	return us ? (uint32_t)((c1 - c0) / us) : 0;
}

static int
drg_perf_proc(proc_t p, __unused void *arg)
{
	struct proc_taskinfo_internal pti;
	task_t task = proc_task(p);
	int32_t pid = proc_pid(p);
	uint32_t slot = DRG_PERF_PROCS;

	if (task == TASK_NULL) {
		return PROC_RETURNED;
	}
	fill_taskprocinfo(task, &pti);
	for (uint32_t i = 0; i < DRG_PERF_PROBE; i++) {
		uint32_t k = ((uint32_t)pid + i) & (DRG_PERF_PROCS - 1);
		if (proc_seen[k].pid == pid || proc_seen[k].pid == 0) {
			slot = k;
			break;
		}
	}
	if (slot == DRG_PERF_PROCS) {
		slot = (uint32_t)pid & (DRG_PERF_PROCS - 1);   /* reuse a stale slot */
	}
	uint64_t du = pti.pti_total_user, ds = pti.pti_total_system;
	if (proc_seen[slot].pid == pid && du >= proc_seen[slot].user && ds >= proc_seen[slot].sys) {
		du -= proc_seen[slot].user;
		ds -= proc_seen[slot].sys;
	}
	proc_seen[slot].pid = pid;
	proc_seen[slot].user = pti.pti_total_user;
	proc_seen[slot].sys = pti.pti_total_system;

	/* keep the DRG_PERF_TOP largest by user + system, sorted */
	int at = DRG_PERF_TOP;
	while (at > 0 && du + ds > proc_top[at - 1].user + proc_top[at - 1].sys) {
		at--;
	}
	if (at < DRG_PERF_TOP) {
		memmove(&proc_top[at + 1], &proc_top[at], (size_t)(DRG_PERF_TOP - 1 - at) * sizeof(proc_top[0]));
		proc_top[at].pid = pid;
		proc_top[at].user = du;
		proc_top[at].sys = ds;
		strlcpy(proc_top[at].name, proc_best_name(p), sizeof(proc_top[at].name));
	}
	return PROC_RETURNED;
}

static void
drg_perf_arm(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(drg_perf_period, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(drg_perf_call, deadline);
}

static void
drg_perf_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint32_t cpu, mhz;
	uint64_t mpidr;
	char name[20];

	mhz = core_mhz(&cpu, &mpidr);
	uint64_t total = os_atomic_xchg(&lse_total, 0, relaxed);
	uint64_t untracked = os_atomic_xchg(&lse_untracked, 0, relaxed);
	printf("DRG PERF: --- %us: cpu%u mpidr 0x%llx %u MHz; lse %llu (%llu/s, %llu untracked)\n",
	    drg_perf_period, cpu, mpidr, mhz, total, total / drg_perf_period, untracked);

	/* busiest emulated PCs this period */
	for (int n = 0; n < DRG_PERF_TOP; n++) {
		uint32_t best = 0, bk = 0;
		for (uint32_t k = 0; k < DRG_PERF_PCS; k++) {
			if (lse_pcs[k].count > best) {
				best = lse_pcs[k].count;
				bk = k;
			}
		}
		if (best == 0) {
			break;
		}
		proc_name(lse_pcs[bk].pid, name, sizeof(name));
		printf("DRG PERF: lse pc 0x%llx x%u last %s[%d]\n", lse_pcs[bk].pc, best, name, lse_pcs[bk].pid);
		lse_pcs[bk].count = 0;          /* taken: next best */
	}
	for (uint32_t k = 0; k < DRG_PERF_PCS; k++) {
		os_atomic_store(&lse_pcs[k].count, 0, relaxed);
	}

	if (initproc != PROC_NULL) {
		bzero(proc_top, sizeof(proc_top));
		proc_iterate(PROC_ALLPROCLIST, drg_perf_proc, NULL, NULL, NULL);
		for (int n = 0; n < DRG_PERF_TOP && proc_top[n].pid != 0; n++) {
			printf("DRG PERF: cpu %s[%d] user %llu ms sys %llu ms\n", proc_top[n].name, proc_top[n].pid,
			    abs_to_us(proc_top[n].user) / 1000, abs_to_us(proc_top[n].sys) / 1000);
		}
	}

	for (int w = 0; w < 2; w++) {
		uint64_t bytes = os_atomic_xchg(&emmc_bytes[w], 0, relaxed);
		uint64_t us = abs_to_us(os_atomic_xchg(&emmc_abs[w], 0, relaxed));
		uint64_t ops = os_atomic_xchg(&emmc_ops[w], 0, relaxed);
		if (ops) {
			printf("DRG PERF: emmc %s %llu KiB in %llu ops, %llu ms busy, %llu KiB/s while busy\n",
			    w ? "write" : "read", bytes >> 10, ops, us / 1000, us ? (bytes >> 10) * 1000000 / us : 0);
		}
	}
	uint64_t frames = os_atomic_xchg(&fb_frames, 0, relaxed);
	uint64_t fus = abs_to_us(os_atomic_xchg(&fb_abs, 0, relaxed));
	uint64_t fmax = abs_to_us(fb_max_abs);
	fb_max_abs = 0;
	if (frames) {
		printf("DRG PERF: fb %llu frames, %llu us each, max %llu us\n", frames, fus / frames, fmax);
	}
	drg_perf_arm();
}

static void
drg_perf_init(void)
{
	PE_parse_boot_argn("drg_perf", &drg_perf_period, sizeof(drg_perf_period));
	if (drg_perf_period == 0) {
		return;
	}
	drg_perf_call = thread_call_allocate_with_options(drg_perf_report, NULL,
	    THREAD_CALL_PRIORITY_LOW, 0);
	drg_perf_arm();
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, drg_perf_init);

#endif /* DRG */
