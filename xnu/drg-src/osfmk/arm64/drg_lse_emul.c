/*
 * DRG (Nokia 6.1 Plus): emulate ARMv8.1 LSE atomics and ARMv8.3 LDAPR for
 * 64-bit user code.
 *
 * The Kryo 260 cores of the SDM636 are ARMv8.0 (ID_AA64ISAR0.Atomic = 0,
 * ID_AA64ISAR1.LRCPC = 0, measured on the phone). The iPhone X userland
 * was built for an ARMv8.2 CPU and contains these instructions, so each one
 * raises an "unknown reason" synchronous exception. handle_uncategorized()
 * in sleh.c hands user-mode ones to drg_emulate_user_lse(), which decodes
 * the instruction, performs it on user memory with an exclusive load/store
 * loop (drg_lse_emul.s, under copyio fault recovery), writes the result
 * registers and steps over it. Anything not recognised stays SIGILL.
 *
 * Encodings (Arm Architecture Reference Manual, A64 load/store):
 *   LD<op>/ST<op>/SWP/LDAPR  size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt
 *       mask 0x3f200c00 == 0x38200000
 *       o3=0: opc = ADD CLR EOR SET SMAX SMIN UMAX UMIN (ST<op> = Rt XZR)
 *       o3=1 opc=000: SWP;  o3=1 opc=100 A=1 R=0 Rs=11111: LDAPR
 *   CAS     size 001000 1 L 1 Rs o0 11111 Rn Rt
 *       mask 0x3fa07c00 == 0x08a07c00
 *   CASP    0 sz 001000 0 L 1 Rs o0 11111 Rn Rt   (Rs, Rt even)
 *       mask 0xbfa07c00 == 0x08207c00
 *
 * Built with DRG_LSE_TEST, only the decoder and drg_lse_run() are compiled
 * (tools/test-drg-lse checks them against real LSE instructions).
 */

#if !defined(DRG_LSE_TEST)
#include <pexpert/arm64/board_config.h>  /* defines DRG for the Nokia build */
#endif

#if defined(DRG) || defined(DRG_LSE_TEST)

#include <stdbool.h>
#include <stdint.h>
#include <sys/errno.h>

enum drg_lse_kind {
	DRG_LSE_KIND_NONE,
	DRG_LSE_KIND_RMW,       /* LD<op>, ST<op>, SWP */
	DRG_LSE_KIND_CAS,
	DRG_LSE_KIND_CASP,
	DRG_LSE_KIND_LDAPR,
};

/* op values: opc of LD<op>, then SWP and CAS. Keep in sync with drg_lse_emul.s */
#define DRG_LSE_OP_ADD  0
#define DRG_LSE_OP_CLR  1
#define DRG_LSE_OP_EOR  2
#define DRG_LSE_OP_SET  3
#define DRG_LSE_OP_SMAX 4
#define DRG_LSE_OP_SMIN 5
#define DRG_LSE_OP_UMAX 6
#define DRG_LSE_OP_UMIN 7
#define DRG_LSE_OP_SWP  8
#define DRG_LSE_OP_CAS  9

struct drg_lse_insn {
	enum drg_lse_kind kind;
	unsigned size;          /* log2 of the access size; CASP: of one element */
	unsigned op;
	unsigned rs, rt, rn;
};

/* x0-x30, then sp. Register 31 reads as XZR in Rs/Rt and as SP in Rn. */
struct drg_lse_regs {
	uint64_t x[31];
	uint64_t sp;
};

int drg_lse_rmw8(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t *);
int drg_lse_rmw16(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t *);
int drg_lse_rmw32(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t *);
int drg_lse_rmw64(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t *);
int drg_lse_casp32(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);
int drg_lse_casp64(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);
int drg_lse_ldar8(uint64_t, uint64_t *);
int drg_lse_ldar16(uint64_t, uint64_t *);
int drg_lse_ldar32(uint64_t, uint64_t *);
int drg_lse_ldar64(uint64_t, uint64_t *);

bool drg_lse_decode(uint32_t insn, struct drg_lse_insn *d);
uint64_t drg_lse_bytes(const struct drg_lse_insn *d);
uint64_t drg_lse_base(const struct drg_lse_insn *d, const struct drg_lse_regs *r);
int drg_lse_run(const struct drg_lse_insn *d, struct drg_lse_regs *r, uint64_t addr);

bool
drg_lse_decode(uint32_t insn, struct drg_lse_insn *d)
{
	d->rs = (insn >> 16) & 31;
	d->rn = (insn >> 5) & 31;
	d->rt = insn & 31;

	if ((insn & 0x3f200c00) == 0x38200000) {
		unsigned o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
		unsigned a = (insn >> 23) & 1, r = (insn >> 22) & 1;

		d->size = insn >> 30;
		if (o3 == 0) {
			d->kind = DRG_LSE_KIND_RMW;
			d->op = opc;
			return true;
		}
		if (opc == 0) {
			d->kind = DRG_LSE_KIND_RMW;
			d->op = DRG_LSE_OP_SWP;
			return true;
		}
		if (opc == 4 && a == 1 && r == 0 && d->rs == 31) {
			d->kind = DRG_LSE_KIND_LDAPR;
			return true;
		}
		return false;   /* other o3=1 encodings are not ARMv8.1-8.3 */
	}
	if ((insn & 0x3fa07c00) == 0x08a07c00) {
		d->kind = DRG_LSE_KIND_CAS;
		d->size = insn >> 30;
		d->op = DRG_LSE_OP_CAS;
		return true;
	}
	if ((insn & 0xbfa07c00) == 0x08207c00) {
		if ((d->rs & 1) || (d->rt & 1)) {
			return false;   /* odd register pair: UNDEFINED, leave as SIGILL */
		}
		d->kind = DRG_LSE_KIND_CASP;
		d->size = 2 + ((insn >> 30) & 1);
		return true;
	}
	return false;
}

uint64_t
drg_lse_bytes(const struct drg_lse_insn *d)
{
	return (d->kind == DRG_LSE_KIND_CASP ? 2ULL : 1ULL) << d->size;
}

uint64_t
drg_lse_base(const struct drg_lse_insn *d, const struct drg_lse_regs *r)
{
	return d->rn == 31 ? r->sp : r->x[d->rn];
}

static uint64_t
reg_get(const struct drg_lse_regs *r, unsigned n)
{
	return n == 31 ? 0 : r->x[n];
}

static void
reg_set(struct drg_lse_regs *r, unsigned n, uint64_t v)
{
	if (n != 31) {
		r->x[n] = v;
	}
}

/*
 * Perform the decoded instruction at addr (stripped, range-checked and
 * aligned by the caller) and update r. Returns 0 or EFAULT; on EFAULT
 * nothing in r has changed.
 */
int
drg_lse_run(const struct drg_lse_insn *d, struct drg_lse_regs *r, uint64_t addr)
{
	uint64_t old = 0, pair[2] = {0, 0};
	int err;

	switch (d->kind) {
	case DRG_LSE_KIND_RMW:
	case DRG_LSE_KIND_CAS: {
		/* LD<op>/SWP: operand Rs, old -> Rt.  CAS: compare Rs, new Rt, old -> Rs. */
		uint64_t operand = d->kind == DRG_LSE_KIND_CAS ? reg_get(r, d->rt) : reg_get(r, d->rs);
		uint64_t compare = d->kind == DRG_LSE_KIND_CAS ? reg_get(r, d->rs) : 0;
		static int (*const rmw[4])(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t *) = {
			drg_lse_rmw8, drg_lse_rmw16, drg_lse_rmw32, drg_lse_rmw64,
		};
		err = rmw[d->size](addr, operand, compare, d->op, &old);
		if (err) {
			return err;
		}
		reg_set(r, d->kind == DRG_LSE_KIND_CAS ? d->rs : d->rt, old);
		return 0;
	}
	case DRG_LSE_KIND_CASP:
		err = (d->size == 2 ? drg_lse_casp32 : drg_lse_casp64)(addr,
		    reg_get(r, d->rt), reg_get(r, d->rt + 1),
		    reg_get(r, d->rs), reg_get(r, d->rs + 1), pair);
		if (err) {
			return err;
		}
		reg_set(r, d->rs, pair[0]);
		reg_set(r, d->rs + 1, pair[1]);
		return 0;
	case DRG_LSE_KIND_LDAPR: {
		static int (*const ldar[4])(uint64_t, uint64_t *) = {
			drg_lse_ldar8, drg_lse_ldar16, drg_lse_ldar32, drg_lse_ldar64,
		};
		err = ldar[d->size](addr, &old);
		if (err) {
			return err;
		}
		reg_set(r, d->rt, old);
		return 0;
	}
	default:
		return EINVAL;
	}
}

#if !defined(DRG_LSE_TEST)

#include <arm64/proc_reg.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <machine/atomic.h>
#include <mach/arm/thread_status.h>
#include <os/overflow.h>
#include <vm/vm_map.h>
#include <arm64/drg_lse_emul.h>

static const char *const drg_lse_names[] = {
	"LDADD", "LDCLR", "LDEOR", "LDSET", "LDSMAX", "LDSMIN", "LDUMAX", "LDUMIN", "SWP", "CAS",
};

static uint32_t drg_lse_count;

static void
state_to_regs(const arm_saved_state_t *state, struct drg_lse_regs *r)
{
	for (unsigned i = 0; i < 29; i++) {
		r->x[i] = get_saved_state_reg(state, i);
	}
	r->x[29] = get_saved_state_fp(state);
	r->x[30] = get_saved_state_lr(state);
	r->sp = get_saved_state_sp(state);
}

static void
reg_to_state(arm_saved_state_t *state, const struct drg_lse_regs *r, unsigned n)
{
	if (n < 29) {
		set_saved_state_reg(state, n, r->x[n]);
	} else if (n == 29) {
		set_saved_state_fp(state, r->x[29]);
	} else if (n == 30) {
		set_saved_state_lr(state, r->x[30]);
	}
	/* 31 is XZR here: nothing to write */
}

int
drg_emulate_user_lse(arm_saved_state_t *state, uint32_t instr, user_addr_t *fault_addr)
{
	struct drg_lse_insn d;
	struct drg_lse_regs r;

	if (!PSR64_IS_USER64(get_saved_state_cpsr(state)) || !drg_lse_decode(instr, &d)) {
		return DRG_LSE_NOT_EMULATED;
	}
	state_to_regs(state, &r);

	uint64_t addr = drg_lse_base(&d, &r);
	if (__builtin_arm_rsr64("TCR_EL1") & TCR_TBI0_TOPBYTE_IGNORED) {
		addr &= ~(0xffULL << 56);       /* the hardware would ignore the tag */
	}
	uint64_t bytes = drg_lse_bytes(&d), last;
	*fault_addr = addr;

	/* The checks copy_validate() makes for copyin/copyout, plus alignment
	 * (LSE and exclusives fault on unaligned addresses). */
	if (addr & (bytes - 1)) {
		return DRG_LSE_FAULT_ALIGN;
	}
	vm_map_t map = current_thread()->map;
	if (addr < vm_map_min(map) || os_add_overflow(addr, bytes, &last) || last > vm_map_max(map)) {
		return DRG_LSE_FAULT_ADDR;
	}
	if (drg_lse_run(&d, &r, addr) != 0) {
		return DRG_LSE_FAULT_ADDR;
	}

	switch (d.kind) {
	case DRG_LSE_KIND_CAS:
		reg_to_state(state, &r, d.rs);
		break;
	case DRG_LSE_KIND_CASP:
		reg_to_state(state, &r, d.rs);
		reg_to_state(state, &r, d.rs + 1);
		break;
	default:
		reg_to_state(state, &r, d.rt);
		break;
	}
	add_saved_state_pc(state, 4);

	uint32_t n = os_atomic_inc_orig(&drg_lse_count, relaxed);
	if (n < 16) {
		const char *name = d.kind == DRG_LSE_KIND_CASP ? "CASP" :
		    d.kind == DRG_LSE_KIND_LDAPR ? "LDAPR" : drg_lse_names[d.op];
		kprintf("DRG LSE: emulated %s (size %u) pc=0x%llx insn=0x%08x%s\n", name, d.size,
		    (unsigned long long)get_saved_state_pc(state) - 4, instr,
		    n == 15 ? " (further ones not logged)" : "");
	}
	return DRG_LSE_EMULATED;
}

#endif /* !DRG_LSE_TEST */
#endif /* DRG || DRG_LSE_TEST */
