/*
 * DRG (Nokia 6.1 Plus, Kryo 260 = ARMv8.0): user-memory primitives for
 * emulating the ARMv8.1 LSE atomics and ARMv8.3 LDAPR that the iPhone X
 * userland contains. See drg_lse_emul.c for the decoder and the callers.
 *
 * #included at the end of machine_routines_asm.s, before
 * copyio_recover_table_end, so that every user access below is covered by
 * a COPYIO_RECOVER_RANGE: page faults are resolved by vm_fault and retried,
 * and a bad address ends in copyio_error (returns EFAULT).
 *
 * Every primitive runs an exclusive load/store loop with acquire and
 * release semantics plus a trailing DMB ISH, which is at least as strong
 * as any ordering variant (none, A, L, AL) of the emulated instruction.
 * The callers have already checked the address range and alignment.
 */

#if defined(DRG)

/* DRG_LSE_OP_*: keep in sync with drg_lse_emul.c */
#define DRG_LSE_OP_SMAX	4
#define DRG_LSE_OP_SMIN	5
#define DRG_LSE_OP_UMAX	6
#define DRG_LSE_OP_UMIN	7
#define DRG_LSE_OP_SWP	8
#define DRG_LSE_OP_CAS	9

/*
 * int drg_lse_rmwN(user_addr_t uaddr, uint64_t operand, uint64_t compare,
 *                  uint32_t op, uint64_t *old)
 *
 * x0 user address, x1 operand (Rs of LD<op>/SWP, Rt of CAS), x2 compare
 * value (Rs of CAS), w3 op: 0 ADD, 1 CLR, 2 EOR, 3 SET, 4 SMAX, 5 SMIN,
 * 6 UMAX, 7 UMIN, 8 SWP, 9 CAS. *old receives the previous memory value,
 * zero-extended. Returns 0, or EFAULT through copyio_error.
 */
.macro DRG_LSE_RMW name, bits, ldx, stx, r
	.text
	.align 2
	.globl EXT(\name)
LEXT(\name)
	ARM64_STACK_PROLOG
	PUSH_FRAME
	/* operand and compare value as this access size sees them */
.if \bits == 8
	and		x1, x1, #0xff
	and		x2, x2, #0xff
.elseif \bits == 16
	and		x1, x1, #0xffff
	and		x2, x2, #0xffff
.elseif \bits == 32
	mov		w1, w1
	mov		w2, w2
.endif
	COPYIO_RECOVER_RANGE 9f
1:
	\ldx	\r\()5, [x0]			// x5 = old (zero-extended)
	cmp		w3, #DRG_LSE_OP_CAS
	b.eq	5f
	cmp		w3, #DRG_LSE_OP_SWP
	b.eq	4f
	cmp		w3, #DRG_LSE_OP_SMAX
	b.hs	3f
	cmp		w3, #1
	b.lo	20f
	b.eq	21f
	cmp		w3, #2
	b.eq	22f
	orr		x6, x5, x1			// 3 SET
	b		6f
20:	add		x6, x5, x1			// 0 ADD
	b		6f
21:	bic		x6, x5, x1			// 1 CLR
	b		6f
22:	eor		x6, x5, x1			// 2 EOR
	b		6f
3:	/* min/max: signed compares on sign-extended copies */
.if \bits == 8
	sxtb	x7, w5
	sxtb	x8, w1
.elseif \bits == 16
	sxth	x7, w5
	sxth	x8, w1
.elseif \bits == 32
	sxtw	x7, w5
	sxtw	x8, w1
.else
	mov		x7, x5
	mov		x8, x1
.endif
	cmp		w3, #DRG_LSE_OP_SMIN
	b.lo	30f
	b.eq	31f
	cmp		w3, #DRG_LSE_OP_UMAX
	b.eq	32f
	cmp		x5, x1				// 7 UMIN
	csel	x6, x5, x1, lo
	b		6f
30:	cmp		x7, x8				// 4 SMAX
	csel	x6, x5, x1, gt
	b		6f
31:	cmp		x7, x8				// 5 SMIN
	csel	x6, x5, x1, lt
	b		6f
32:	cmp		x5, x1				// 6 UMAX
	csel	x6, x5, x1, hi
	b		6f
4:	mov		x6, x1				// 8 SWP
	b		6f
5:	cmp		x5, x2				// 9 CAS: store only if equal
	b.eq	50f
	clrex
	b		9f
50:	mov		x6, x1
6:
	\stx	w9, \r\()6, [x0]
	cbnz	w9, 1b
9:
	str		x5, [x4]
	dmb		ish
	mov		x0, #0
	POP_FRAME
	ARM64_STACK_EPILOG
.endmacro

	DRG_LSE_RMW drg_lse_rmw8,  8,  ldaxrb, stlxrb, w
	DRG_LSE_RMW drg_lse_rmw16, 16, ldaxrh, stlxrh, w
	DRG_LSE_RMW drg_lse_rmw32, 32, ldaxr,  stlxr,  w
	DRG_LSE_RMW drg_lse_rmw64, 64, ldaxr,  stlxr,  x

/*
 * int drg_lse_caspN(user_addr_t uaddr, uint64_t new_lo, uint64_t new_hi,
 *                   uint64_t cmp_lo, uint64_t cmp_hi, uint64_t old[2])
 *
 * CASP: compare the pair at uaddr (lo = lower address) with cmp_lo/cmp_hi
 * and store new_lo/new_hi if both match. old[] receives the previous pair.
 */
.macro DRG_LSE_CASP name, ldxp, stxp, r
	.text
	.align 2
	.globl EXT(\name)
LEXT(\name)
	ARM64_STACK_PROLOG
	PUSH_FRAME
	COPYIO_RECOVER_RANGE 9f
1:
	\ldxp	\r\()6, \r\()7, [x0]
	cmp		\r\()6, \r\()3
	ccmp	\r\()7, \r\()4, #0, eq
	b.eq	2f
	clrex
	b		9f
2:
	\stxp	w9, \r\()1, \r\()2, [x0]
	cbnz	w9, 1b
9:
	stp		x6, x7, [x5]
	dmb		ish
	mov		x0, #0
	POP_FRAME
	ARM64_STACK_EPILOG
.endmacro

	DRG_LSE_CASP drg_lse_casp32, ldaxp, stlxp, w
	DRG_LSE_CASP drg_lse_casp64, ldaxp, stlxp, x

/*
 * int drg_lse_ldarN(user_addr_t uaddr, uint64_t *out)
 *
 * LDAPR (RCpc acquire): emulated with LDAR (RCsc acquire), which is
 * stronger, so every correct program stays correct.
 */
.macro DRG_LSE_LDAR name, ldar, r
	.text
	.align 2
	.globl EXT(\name)
LEXT(\name)
	ARM64_STACK_PROLOG
	PUSH_FRAME
	COPYIO_RECOVER_RANGE 9f
	\ldar	\r\()5, [x0]
9:
	str		x5, [x1]
	mov		x0, #0
	POP_FRAME
	ARM64_STACK_EPILOG
.endmacro

	DRG_LSE_LDAR drg_lse_ldar8,  ldarb, w
	DRG_LSE_LDAR drg_lse_ldar16, ldarh, w
	DRG_LSE_LDAR drg_lse_ldar32, ldar,  w
	DRG_LSE_LDAR drg_lse_ldar64, ldar,  x

#endif /* DRG */
