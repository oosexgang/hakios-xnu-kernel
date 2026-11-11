/*
 * Copyright (c) 2026 The redwood bring-up. All rights reserved.
 *
 * Standard ARMv8.3+/ARMv9 pointer authentication, as opposed to Apple's own
 * implementation (osfmk/arm64/pac_asm.h, HAS_APPLE_PAC && !HAS_STD_PAC).
 *
 * The architectural differences from Apple silicon that shape this file:
 *
 *  - The signing instructions are the standard ones (PACIA, PACIB, PACDA, PACDB, PACGA and their
 *    AUT variants), which take their keys from APIAKey_EL1, APIBKey_EL1, APDAKey_EL1, APDBKey_EL1
 *    and APGAKey_EL1.
 *  - Those five registers are one set shared by EL0 and EL1.  Apple's hardware keeps a separate
 *    EL0 state, so its kernel never has to look at them on an exception.  Here the kernel must:
 *    EL1 always runs with the kernel's keys, and the moment control returns to EL0 the key
 *    registers have to hold that process's keys instead.  locore.s does the switching, using the
 *    macros below.
 *  - The keys are not held in hardware-only registers: the kernel's five keys live in
 *    pac_std_keys, written once at boot, and a process's keys are derived from its thread's
 *    jop_pid / rop_pid and those secrets (PAC_STD_DERIVE_KEY), so nothing has to be stored per
 *    task and two processes that share a jop_pid get the same A keys, as a shared region needs.
 *
 * The GA key is not swapped: pacga is what ml_sign_thread_state() and ml_check_signed_state() hash
 * the saved thread state with, and those run at EL1 on both the entry and the exit path.
 *
 * Requires EXT() from <arm64/asm.h>, TH_JOP_PID and TH_ROP_PID from assym.s.
 */

#ifndef _ARM64_PAC_STD_ASM_H_
#define _ARM64_PAC_STD_ASM_H_

#ifndef __ASSEMBLER__
#error "This header should only be used in .s files"
#endif

/*
 * pac_std_keys (osfmk/arm64/machine_routines_asm.s) holds five 128-bit keys in this order, each as
 * a little-endian pair of 64-bit words.
 */
#define PAC_STD_KEY_IA  0
#define PAC_STD_KEY_IB  1
#define PAC_STD_KEY_DA  2
#define PAC_STD_KEY_DB  3
#define PAC_STD_KEY_GA  4
#define PAC_STD_KEY_WORDS 10

/**
 * Loads one 128-bit key out of the block at \base into \lo and \hi.
 *
 *   base - address of pac_std_keys
 */
.macro PAC_STD_LOAD_KEY base, idx, lo, hi
	ldp	\lo, \hi, [\base, #(16 * (\idx))]
.endm

/**
 * Programs all five key registers from the block at \base (the kernel's keys).
 *
 *   base - address of pac_std_keys, virtual: the MMU is on
 *   lo, hi - scratch registers
 */
.macro PAC_STD_LOAD_KEYS base, lo, hi
	PAC_STD_LOAD_KEY \base, PAC_STD_KEY_IA, \lo, \hi
	msr	APIAKeyLo_EL1, \lo
	msr	APIAKeyHi_EL1, \hi
	PAC_STD_LOAD_KEY \base, PAC_STD_KEY_IB, \lo, \hi
	msr	APIBKeyLo_EL1, \lo
	msr	APIBKeyHi_EL1, \hi
	PAC_STD_LOAD_KEY \base, PAC_STD_KEY_DA, \lo, \hi
	msr	APDAKeyLo_EL1, \lo
	msr	APDAKeyHi_EL1, \hi
	PAC_STD_LOAD_KEY \base, PAC_STD_KEY_DB, \lo, \hi
	msr	APDBKeyLo_EL1, \lo
	msr	APDBKeyHi_EL1, \hi
	PAC_STD_LOAD_KEY \base, PAC_STD_KEY_GA, \lo, \hi
	msr	APGAKeyLo_EL1, \lo
	msr	APGAKeyHi_EL1, \hi
.endm

/**
 * PAC_INIT_KEY_STATE
 *
 * Loads the five kernel keys into the key registers.  Called from common_start once the MMU is on,
 * SCTLR_EL1 has enabled the signing instructions and before the first C function, so that the first
 * signed pointer the kernel makes is signed with the kernel's keys.  Every CPU runs it: on a
 * secondary CPU this is what gives it the boot CPU's keys.
 *
 * The keys themselves are made and written to pac_std_keys by the boot loader, which needs them
 * earlier than this: it signs the kernel image's ptrauth-carrying pointers while it loads them
 * (osfmk/arm64/../../../xnuloader/loader.c).  Until the loader has filled the section the keys are
 * zero, which is self-consistent -- PAC with an all-zero key verifies -- just not secret.
 *
 * Clobbers x2, x0 and x1.
 */
.macro PAC_INIT_KEY_STATE tmp, tmp2
	adrp	x2, EXT(pac_std_keys)@page
	add	x2, x2, EXT(pac_std_keys)@pageoff
	PAC_STD_LOAD_KEYS x2, x0, x1
.endm

/**
 * Derives one 128-bit user key: the kernel's secret for the key \idx, mixed with a per-process id.
 *
 *   base - address of pac_std_keys
 *   idx - PAC_STD_KEY_IA or PAC_STD_KEY_IB (the secret the key is derived from)
 *   id - jop_pid or rop_pid
 *   out_lo, out_hi - the key
 *   t1-t4 - scratch registers, distinct from id and from out_lo/out_hi
 */
.macro PAC_STD_DERIVE_KEY base, idx, id, out_lo, out_hi, t1, t2, t3, t4
	PAC_STD_LOAD_KEY \base, \idx, \out_lo, \out_hi
	eor	\out_lo, \out_lo, \id
	eor	\out_hi, \out_hi, \id
	MOV64	\t1, 0xbf58476d1ce4e5b9
	MOV64	\t2, 0x94d049bb133111eb
	eor	\t3, \out_lo, \out_lo, lsr #30
	mul	\t3, \t3, \t1
	eor	\t3, \t3, \t3, lsr #27
	mul	\t3, \t3, \t2
	eor	\out_lo, \t3, \t3, lsr #31
	eor	\t4, \out_hi, \out_hi, lsr #30
	mul	\t4, \t4, \t1
	eor	\t4, \t4, \t4, lsr #27
	mul	\t4, \t4, \t2
	eor	\out_hi, \t4, \t4, lsr #31
.endm

/**
 * Programs the A keys (instruction and data) for a process with the id \id.
 */
.macro PAC_STD_LOAD_A_KEYS base, id, lo, hi, t1, t2, t3, t4
	PAC_STD_DERIVE_KEY \base, PAC_STD_KEY_IA, \id, \lo, \hi, \t1, \t2, \t3, \t4
	msr	APIAKeyLo_EL1, \lo
	msr	APIAKeyHi_EL1, \hi
	msr	APDAKeyLo_EL1, \lo
	msr	APDAKeyHi_EL1, \hi
.endm

/**
 * Programs the B keys (instruction and data) for a process with the id \id.
 */
.macro PAC_STD_LOAD_B_KEYS base, id, lo, hi, t1, t2, t3, t4
	PAC_STD_DERIVE_KEY \base, PAC_STD_KEY_IB, \id, \lo, \hi, \t1, \t2, \t3, \t4
	msr	APIBKeyLo_EL1, \lo
	msr	APIBKeyHi_EL1, \hi
	msr	APDBKeyLo_EL1, \lo
	msr	APDBKeyHi_EL1, \hi
.endm

/**
 * Programs the keys of the process that owns \thread: the A keys from its jop_pid, the B keys from
 * its rop_pid.  Called on the way back to EL0 and by ml_enable_user_jop_key().
 *
 *   base - address of pac_std_keys
 *   thread - struct thread *, e.g. from TPIDR_EL1
 *   id, lo, hi, t1-t4 - scratch registers, all distinct
 */
.macro PAC_STD_LOAD_USER_KEYS base, thread, id, lo, hi, t1, t2, t3, t4
	ldr	\id, [\thread, TH_JOP_PID]
	PAC_STD_LOAD_A_KEYS \base, \id, \lo, \hi, \t1, \t2, \t3, \t4
	ldr	\id, [\thread, TH_ROP_PID]
	PAC_STD_LOAD_B_KEYS \base, \id, \lo, \hi, \t1, \t2, \t3, \t4
.endm

#endif /* _ARM64_PAC_STD_ASM_H_ */

/* vim: set ts=4 ft=asm: */
