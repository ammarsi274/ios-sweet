/*
 * Host build of xnu/drg-src/osfmk/arm64/drg_lse_emul.s for the test:
 * XNU's frame and copyio-recovery macros become no-ops (Linux user space,
 * the test only touches valid memory), plus a trampoline that runs one
 * real instruction with x1-x7 taken from and returned to an array.
 */
#define EXT(x) x
#define LEXT(x) x:
#define ARM64_STACK_PROLOG
#define PUSH_FRAME
#define POP_FRAME
#define ARM64_STACK_EPILOG ret

.macro COPYIO_RECOVER_RANGE end_addr, recovery_addr=0
.endm

#include "drg_lse_emul.s"

/* void native_call(uint64_t regs[8], void *code): code = { insn; ret } */
	.text
	.align 2
	.globl native_call
native_call:
	stp	x29, x30, [sp, #-32]!
	str	x19, [sp, #16]
	mov	x19, x0
	mov	x16, x1
	ldp	x0, x1, [x19, #0]
	ldp	x2, x3, [x19, #16]
	ldp	x4, x5, [x19, #32]
	ldp	x6, x7, [x19, #48]
	blr	x16
	stp	x0, x1, [x19, #0]
	stp	x2, x3, [x19, #16]
	stp	x4, x5, [x19, #32]
	stp	x6, x7, [x19, #48]
	ldr	x19, [sp, #16]
	ldp	x29, x30, [sp], #32
	ret
