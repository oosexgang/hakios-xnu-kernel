/*
 * A 64-bit user thread's registers as the exception vectors save them (mach/arm/thread_status.h): x0 to x30, sp,
 * pc and cpsr, and the SIMD registers, with the accessors the kernel's code reads and writes them through.
 */
#include "../../fakekernel.h"
#ifndef FAKEKERNEL_ARM_THREAD_STATUS_H
#define FAKEKERNEL_ARM_THREAD_STATUS_H
typedef struct arm_saved_state {
	uint64_t x[31];                 /* x0 to x28, then fp (x29) and lr (x30), as the kernel's are laid out */
	uint64_t sp, pc;
	uint32_t cpsr;
} arm_saved_state_t;
typedef struct arm_neon_saved_state {
	struct {
		union {
			__uint128_t q[32];
		} v;
		uint32_t fpsr, fpcr;
	} ns_64;
} arm_neon_saved_state_t;
typedef struct arm_context {
	arm_saved_state_t ss;
	arm_neon_saved_state_t ns;
} arm_context_t;

static inline bool is_saved_state64(const arm_saved_state_t *ss) { (void)ss; return true; }
static inline uint64_t get_saved_state_reg(const arm_saved_state_t *ss, unsigned n) { return n < 31 ? ss->x[n] : 0; }
static inline void set_saved_state_reg(arm_saved_state_t *ss, unsigned n, uint64_t value) { if (n < 31) ss->x[n] = value; }
static inline uint64_t get_saved_state_fp(const arm_saved_state_t *ss) { return ss->x[29]; }
static inline uint64_t get_saved_state_lr(const arm_saved_state_t *ss) { return ss->x[30]; }
static inline void set_saved_state_lr(arm_saved_state_t *ss, uint64_t value) { ss->x[30] = value; }
static inline uint64_t get_saved_state_sp(const arm_saved_state_t *ss) { return ss->sp; }
static inline void set_saved_state_sp(arm_saved_state_t *ss, uint64_t value) { ss->sp = value; }
static inline uint64_t get_saved_state_pc(const arm_saved_state_t *ss) { return ss->pc; }
static inline void set_saved_state_pc(arm_saved_state_t *ss, uint64_t value) { ss->pc = value; }
static inline uint32_t get_saved_state_cpsr(const arm_saved_state_t *ss) { return ss->cpsr; }
static inline void set_saved_state_cpsr(arm_saved_state_t *ss, uint32_t value) { ss->cpsr = value; }

/* the mode in cpsr (arm64/proc_reg.h): EL0 is 0 in bits 3..2, and bit 4 says AArch32 */
#define PSR64_MODE_EL_MASK      0xcu
#define PSR64_MODE_RW_32        0x10u
#define PSR64_IS_USER(x)        (((x) & PSR64_MODE_EL_MASK) == 0)
#define PSR64_IS_USER64(x)      (PSR64_IS_USER(x) && !((x) & PSR64_MODE_RW_32))
#endif
