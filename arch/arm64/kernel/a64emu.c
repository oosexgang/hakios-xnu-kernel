/*
 * The instructions of arm64e user code that a Cortex-A78/A55 (ARMv8.2) does not have, done
 * here when one of them traps (undefined instruction from EL0).
 *
 * Darwin 26 user space is arm64e only. arch/arm64/tools/pacstrip rewrites the code sections of the dyld
 * shared cache and of the programs we ship so that pointer authentication is the identity: a
 * signature is never made and never checked (RETAB -> RET, BLRAA -> BLR, PACIA/AUTIA -> NOP,
 * PACGA -> EOR, ...). What it could not rewrite, or does not know about (code outside the sections
 * it walks, code generated at run time), arrives here, and gets the same meaning:
 *
 *   RETAA/RETAB                        return through x30
 *   BRAA/BRAB/BRAAZ/BRABZ              branch to Xn
 *   BLRAA/BLRAB/BLRAAZ/BLRABZ          call Xn
 *   PAC* / AUT* (register forms), XPACI/XPACD    nothing (the pointer stays as it is)
 *   PACGA Xd, Xn, Xm                   Xd = Xn ^ Xm (register 31 the zero register, as pacstrip's EOR)
 *   LDRAA/LDRAB                        LDR of 64 bits, with the pre-index writeback
 *   LDAPUR, LDAPURS, STLUR (RCPC2)     the access, with a barrier for the acquire or release order
 *   FJCVTZS (JSCVT)                    JavaScript's double -> int32, Z when exact
 *   SB                                 nothing (the exception return is a context synchronization)
 *
 * Anything else stays an undefined instruction (EXC_BAD_INSTRUCTION), as before. The first few
 * emulations of each kind are logged with the process and the address.
 */
#include <mach/mach_types.h>
#include <mach/arm/thread_status.h>
#include <kern/task.h>
#include <kern/misc_protos.h>
#include <arm64/proc_reg.h>
#include <machine/atomic.h>

bool hakios_a64emu(arm_context_t *context);

enum {
	EMU_RET, EMU_BR, EMU_BLR, EMU_PAC, EMU_PACGA, EMU_LDRA, EMU_RCPC2, EMU_FJCVTZS, EMU_SB, EMU_KINDS
};
static const char *const emu_names[EMU_KINDS] = {
	"RETAA/RETAB", "BRAA*", "BLRAA*", "PAC/AUT", "PACGA", "LDRAA/LDRAB", "LDAPUR/STLUR", "FJCVTZS", "SB"
};
static uint64_t emu_count[EMU_KINDS];

static uint64_t
xreg(arm_saved_state_t *ss, unsigned n, bool sp)
{
	if (n == 31) {
		return sp ? get_saved_state_sp(ss) : 0;
	}
	return get_saved_state_reg(ss, n);
}

static void
set_xreg(arm_saved_state_t *ss, unsigned n, uint64_t value, bool sp)
{
	if (n == 31) {
		if (sp) {
			set_saved_state_sp(ss, value);
		}
		return;
	}
	set_saved_state_reg(ss, n, value);
}

/*
 * The emulated loads and stores. An access of 4 or 8 bytes at an address that is a multiple of its size is one
 * access, as the instruction's own would be: copyin() and copyout() go a byte at a time under 16 bytes, and another
 * thread would see, or make, a value half old and half new -- a pointer or a counter of a lock-free structure torn.
 * (A load of 2 bytes reads the word they are in; a store of 2 bytes, or anything not so aligned, is a plain copy.)
 */
static int
user_load(user_addr_t address, uint64_t *value, unsigned size)
{
	uint32_t word;
	int error;

	if (size == 8 && (address & 7) == 0) {
		return copyin_atomic64(address, value);
	}
	if ((size == 4 && (address & 3) == 0) || (size == 2 && (address & 1) == 0)) {
		error = copyin_atomic32(address & ~(user_addr_t)3, &word);
		if (error == 0) {
			*value = size == 4 ? word : (word >> ((address & 2) * 8)) & 0xffff;
		}
		return error;
	}
	return copyin(address, value, size);
}

static int
user_store(user_addr_t address, uint64_t value, unsigned size)
{
	if (size == 8 && (address & 7) == 0) {
		return copyout_atomic64(value, address);
	}
	if (size == 4 && (address & 3) == 0) {
		return copyout_atomic32((uint32_t)value, address);
	}
	return copyout(&value, address, size);
}

/* JavaScript's ToInt32 of a double, as FJCVTZS: the result modulo 2^32, and whether it was exact */
static uint32_t
js_int32(uint64_t bits, bool *exact)
{
	bool negative = (bits >> 63) != 0;
	int exponent = (int)((bits >> 52) & 0x7ff);
	uint64_t fraction = bits & ((1ULL << 52) - 1);
	uint64_t mantissa = fraction | (1ULL << 52), integer;
	uint32_t result;
	bool inexact, overflow;

	if (exponent == 0x7ff) {                        /* infinity, NaN */
		*exact = false;
		return 0;
	}
	if (exponent == 0) {                            /* zero, subnormal */
		*exact = fraction == 0 && !negative;    /* (-0 is not exact) */
		return 0;
	}
	int shift = exponent - 1075;
	if (shift >= 0) {                               /* |value| >= 2^52: an integer, out of range */
		result = shift < 32 ? (uint32_t)(mantissa << shift) : 0;
		*exact = false;
		return negative ? (uint32_t)-result : result;
	}
	if (-shift >= 64) {
		integer = 0;
		inexact = true;
	} else {
		integer = mantissa >> -shift;
		inexact = (mantissa & ((1ULL << -shift) - 1)) != 0;
	}
	overflow = integer > (negative ? 0x80000000ULL : 0x7fffffffULL);
	result = (uint32_t)integer;
	*exact = !inexact && !overflow && !(negative && integer == 0);
	return negative ? (uint32_t)-result : result;
}

static bool
emulate(arm_context_t *context, uint32_t instr, unsigned *kind)
{
	arm_saved_state_t *ss = &context->ss;
	uint64_t pc = get_saved_state_pc(ss);
	unsigned rd = instr & 31, rn = (instr >> 5) & 31, rm = (instr >> 16) & 31;

	if ((instr & 0xfffffbffU) == 0xd65f0bffU) {                     /* RETAA, RETAB */
		*kind = EMU_RET;
		set_saved_state_pc(ss, get_saved_state_reg(ss, 30));
		return true;
	}
	if ((instr & 0xfffff800U) == 0xd71f0800U || (instr & 0xfffff81fU) == 0xd61f081fU) {     /* BRAA/BRAB, BRAAZ/BRABZ */
		*kind = EMU_BR;
		set_saved_state_pc(ss, xreg(ss, rn, false));
		return true;
	}
	if ((instr & 0xfffff800U) == 0xd73f0800U || (instr & 0xfffff81fU) == 0xd63f081fU) {     /* BLRAA/BLRAB, BLRAAZ/BLRABZ */
		uint64_t target = xreg(ss, rn, false);

		*kind = EMU_BLR;
		set_saved_state_lr(ss, pc + 4);
		set_saved_state_pc(ss, target);
		return true;
	}
	/* (PACIZA..AUTDZB, opcode 8 to 15, only with Rn 31: with another register they are unallocated, as pacstrip has them) */
	if (((instr & 0xffffc000U) == 0xdac10000U && ((instr & (1U << 13)) == 0 || rn == 31)) ||
	    (instr & 0xfffffbe0U) == 0xdac143e0U) {                     /* PAC*, AUT*, XPACI/XPACD */
		*kind = EMU_PAC;
		set_saved_state_pc(ss, pc + 4);
		return true;
	}
	if ((instr & 0xffe0fc00U) == 0x9ac03000U) {                     /* PACGA */
		*kind = EMU_PACGA;
		set_xreg(ss, rd, xreg(ss, rn, false) ^ xreg(ss, rm, false), false);
		set_saved_state_pc(ss, pc + 4);
		return true;
	}
	if ((instr & 0xff200400U) == 0xf8200400U) {                     /* LDRAA, LDRAB */
		int64_t offset = (int64_t)((uint64_t)(((instr >> 12) & 0x1ff) | ((instr >> 13) & 0x200)) << 54) >> 51;
		uint64_t address = xreg(ss, rn, true) + (uint64_t)offset, value;

		*kind = EMU_LDRA;
		if (user_load((user_addr_t)address, &value, sizeof(value)) != 0) {
			return false;                   /* (a bad address: the undefined instruction stands) */
		}
		if (instr & (1U << 11)) {
			set_xreg(ss, rn, address, true);
		}
		set_xreg(ss, rd, value, false);
		set_saved_state_pc(ss, pc + 4);
		return true;
	}
	if ((instr & 0x3f200c00U) == 0x19000000U) {                     /* LDAPUR*, STLUR* */
		unsigned size = 1U << (instr >> 30), opc = (instr >> 22) & 3;
		int64_t offset = (int64_t)((uint64_t)((instr >> 12) & 0x1ff) << 55) >> 55;
		uint64_t address = xreg(ss, rn, true) + (uint64_t)offset, value = 0;

		*kind = EMU_RCPC2;
		if (opc == 0) {                                         /* STLUR: release, then the store */
			value = xreg(ss, rd, false);
			__builtin_arm_dmb(0xb);                         /* DMB ISH */
			if (user_store((user_addr_t)address, value, size) != 0) {
				return false;
			}
		} else {
			if ((size == 8 && opc != 1) || (size == 4 && opc == 3)) {
				return false;                           /* (unallocated) */
			}
			if (user_load((user_addr_t)address, &value, size) != 0) {
				return false;
			}
			__builtin_arm_dmb(0x9);                         /* DMB ISHLD: acquire */
			if (opc >= 2 && size < 8) {                     /* LDAPURS*: sign-extended */
				unsigned bits = size * 8;

				value = (uint64_t)((int64_t)(value << (64 - bits)) >> (64 - bits));
				if (opc == 3) {
					value &= 0xffffffffULL;         /* (to a W register) */
				}
			}
			set_xreg(ss, rd, value, false);
		}
		set_saved_state_pc(ss, pc + 4);
		return true;
	}
	if ((instr & 0xfffffc00U) == 0x1e7e0000U) {                     /* FJCVTZS Wd, Dn */
		uint64_t bits;
		bool exact;
		uint32_t result;

		*kind = EMU_FJCVTZS;
		__builtin_memcpy(&bits, &context->ns.ns_64.v.q[rn], sizeof(bits));
		result = js_int32(bits, &exact);
		set_xreg(ss, rd, result, false);
		set_saved_state_cpsr(ss, (get_saved_state_cpsr(ss) & ~0xf0000000U) | (exact ? 0x40000000U : 0));
		set_saved_state_pc(ss, pc + 4);
		return true;
	}
	if (instr == 0xd50330ffU) {                                     /* SB */
		*kind = EMU_SB;
		set_saved_state_pc(ss, pc + 4);
		return true;
	}
	return false;
}

bool
hakios_a64emu(arm_context_t *context)
{
	arm_saved_state_t *ss = &context->ss;
	uint64_t pc;
	uint32_t instr;
	unsigned kind = EMU_KINDS;

	if (!PSR64_IS_USER64(get_saved_state_cpsr(ss))) {
		return false;
	}
	pc = get_saved_state_pc(ss);
	if (copyin((user_addr_t)pc, &instr, sizeof(instr)) != 0 || !emulate(context, instr, &kind)) {
		return false;
	}
	if (kind < EMU_KINDS) {
		uint64_t n = os_atomic_inc_orig(&emu_count[kind], relaxed);

		if (n < 3 || n == 1000 || n == 1000000) {
			printf("a64emu: pid %d: %s (%#x) at %#llx, %llu so far\n", task_pid(current_task()), emu_names[kind], instr,
			    (unsigned long long)pc, (unsigned long long)n + 1);
		}
	}
	return true;
}
