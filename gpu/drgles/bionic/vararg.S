/*
 * Entry points the Nokia's driver calls with AAPCS64 variadic arguments
 * (in x0-x7/q0-q7 and then on the stack). Darwin's arm64 convention would
 * look for the variadic part on the stack, so every such call lands here:
 * the registers are saved, an AAPCS64 va_list (struct va64) is built over
 * them, and the C implementation gets the named arguments plus a pointer to
 * that va_list. The same code runs on the Linux bench.
 *
 * Also setjmp/longjmp: bionic's jmp_buf is 32 longs; we use x19-x30, sp and
 * d8-d15 (callee-saved registers of AAPCS64, the same on Darwin).
 */
#ifdef __APPLE__
#define SYM(x) _##x
#define FUNC(x) .globl SYM(x) %% .p2align 2 %% SYM(x):
#else
#define SYM(x) x
#define FUNC(x) .globl x; .type x, %function; .p2align 2; x:
#endif

/* frame: [sp,0..63] x0-x7, [64..191] q0-q7, [192..223] va64, [224] fp/lr */
.macro VSTUB name, impl, named
FUNC(\name)
	sub	sp, sp, #240
	stp	x29, x30, [sp, #224]
	add	x29, sp, #224
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	q0, q1, [sp, #64]
	stp	q2, q3, [sp, #96]
	stp	q4, q5, [sp, #128]
	stp	q6, q7, [sp, #160]
	add	x9, sp, #240		/* __stack: the caller's stack arguments */
	str	x9, [sp, #192]
	add	x9, sp, #64		/* __gr_top */
	str	x9, [sp, #200]
	add	x9, sp, #192		/* __vr_top */
	str	x9, [sp, #208]
	mov	w9, #(-(8 - \named) * 8)	/* __gr_offs: named ones are used */
	str	w9, [sp, #216]
	mov	w9, #-128		/* __vr_offs: no named FP arguments */
	str	w9, [sp, #220]
	add	x\named, sp, #192	/* va64 * after the named arguments */
	bl	SYM(\impl)
	ldp	x29, x30, [sp, #224]
	add	sp, sp, #240
	ret
.endm

.text
VSTUB drgv_printf, b_vprintf, 1
VSTUB drgv_fprintf, b_vfprintf, 2
VSTUB drgv_sscanf, b_vsscanf, 2
VSTUB drgv_open, b_vopen, 2
VSTUB drgv_ioctl, b_vioctl, 2
VSTUB drgv_fcntl, b_vfcntl, 2
VSTUB drgv_prctl, b_vprctl, 1
VSTUB drgv_android_log_print, b_vlog_print, 3
VSTUB drgv_android_log_assert, b_vlog_assert, 3
VSTUB drgv_snprintf, b_vsnprintf3, 3
VSTUB drgv_sprintf, b_vsprintf2, 2

FUNC(drg_setjmp)
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x9, sp
	str	x9, [x0, #96]
	stp	d8, d9, [x0, #104]
	stp	d10, d11, [x0, #120]
	stp	d12, d13, [x0, #136]
	stp	d14, d15, [x0, #152]
	mov	w0, #0
	ret

FUNC(drg_longjmp)
	ldp	x19, x20, [x0, #0]
	ldp	x21, x22, [x0, #16]
	ldp	x23, x24, [x0, #32]
	ldp	x25, x26, [x0, #48]
	ldp	x27, x28, [x0, #64]
	ldp	x29, x30, [x0, #80]
	ldr	x9, [x0, #96]
	mov	sp, x9
	ldp	d8, d9, [x0, #104]
	ldp	d10, d11, [x0, #120]
	ldp	d12, d13, [x0, #136]
	ldp	d14, d15, [x0, #152]
	cmp	w1, #0
	csinc	w0, w1, wzr, ne		/* longjmp(env, 0) returns 1 */
	ret
