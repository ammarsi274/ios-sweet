/*
 * DRG (Nokia 6.1 Plus): user-mode LSE/LDAPR emulation, see drg_lse_emul.c.
 */
#ifndef _ARM64_DRG_LSE_EMUL_H_
#define _ARM64_DRG_LSE_EMUL_H_

#include <pexpert/arm64/board_config.h>  /* defines DRG for the Nokia build */

#if defined(DRG)

#include <mach/arm/thread_status.h>

#define DRG_LSE_NOT_EMULATED    0       /* not one of ours: keep the normal SIGILL path */
#define DRG_LSE_EMULATED        1       /* done, PC advanced */
#define DRG_LSE_FAULT_ALIGN     2       /* unaligned: raise an alignment fault at *fault_addr */
#define DRG_LSE_FAULT_ADDR      3       /* bad user address: raise EXC_BAD_ACCESS at *fault_addr */

int drg_emulate_user_lse(arm_saved_state_t *state, uint32_t instr, user_addr_t *fault_addr);

#endif /* DRG */
#endif /* _ARM64_DRG_LSE_EMUL_H_ */
