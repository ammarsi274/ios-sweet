#!/usr/bin/env python3
"""Add the patch 0906 hooks to an xnu tree that has 0900-0905 applied (run in the tree)."""
import shutil, sys


def rep(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


rep('bsd/conf/files', 'bsd/dev/drg_emmc.c\t\t\tstandard\n',
    'bsd/dev/drg_emmc.c\t\t\tstandard\nbsd/kern/drg_perf.c\t\t\tstandard\n')

rep('osfmk/arm64/sleh.c', '''		case DRG_LSE_EMULATED:
			return;''', '''		case DRG_LSE_EMULATED:
			drg_perf_lse_hit(get_saved_state_pc(state) - 4);
			return;''')
rep('osfmk/arm64/drg_lse_emul.h',
    'int drg_emulate_user_lse(arm_saved_state_t *state, uint32_t instr, user_addr_t *fault_addr);\n',
    'int drg_emulate_user_lse(arm_saved_state_t *state, uint32_t instr, user_addr_t *fault_addr);\n'
    'void drg_perf_lse_hit(uint64_t pc);     /* bsd/kern/drg_perf.c */\n')

p = 'bsd/dev/drg_emmc.c'
rep(p, 'static LCK_GRP_DECLARE(drg_emmc_grp, "drg_emmc");',
    'void drg_perf_emmc(int write, uint64_t bytes, uint64_t abs);   /* bsd/kern/drg_perf.c */\n\n'
    'static LCK_GRP_DECLARE(drg_emmc_grp, "drg_emmc");')
rep(p, '''	lck_mtx_lock(&drg_emmc_lock);
	for (uint32_t done = 0; done < count;) {''', '''	uint64_t t0 = mach_absolute_time();
	lck_mtx_lock(&drg_emmc_lock);
	for (uint32_t done = 0; done < count;) {''')
rep(p, '''	lck_mtx_unlock(&drg_emmc_lock);
	buf_unmap(bp);''', '''	lck_mtx_unlock(&drg_emmc_lock);
	drg_perf_emmc(!is_read, count - buf_resid(bp), mach_absolute_time() - t0);
	buf_unmap(bp);''')

p = 'iokit/Kernel/arm/DrgFramebuffer.cpp'
rep(p, 'extern "C" int drg_console_muted;     /* osfmk/console/serial_console.c */',
    'extern "C" int drg_console_muted;     /* osfmk/console/serial_console.c */\n'
    'extern "C" void drg_perf_fb(uint64_t abs);      /* bsd/kern/drg_perf.c */')
rep(p, '\t\tfb->present(rec);\n', '''\t\tuint64_t t0 = mach_absolute_time();
\t\tfb->present(rec);
\t\tdrg_perf_fb(mach_absolute_time() - t0);
''')

p = 'iokit/Kernel/arm/DrgKeyStore.cpp'
rep(p, '''	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("DRG AKS: %s sel %u''', '''	/*
	 * Log every failure, but each succeeding selector only twice: every
	 * call used to be printed and drawn on the console (efficiency plan,
	 * Phase 1; triald alone made hundreds).
	 */
	static uint8_t drg_aks_ok_logged[128];
	if (ret == kIOReturnSuccess && selector < 128) {
		if (drg_aks_ok_logged[selector] >= 2) {
			return ret;
		}
		drg_aks_ok_logged[selector]++;
	}

	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("DRG AKS: %s sel %u''')

shutil.copy(sys.argv[1], 'bsd/kern/drg_perf.c')
print('hooks added')
