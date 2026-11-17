/*
 * Calls to TrustZone ("SCM", Qualcomm's secure channel manager), as Linux's qcom_scm makes them: an SMC
 * of the SMC Calling Convention's SiP range, with the arguments' kinds in x1. A call that TrustZone
 * preempts says so, and is made again with the session it gave back until it is answered; one it is too
 * busy for is made again a little later. The calls the drivers make are the peripheral authentication
 * service's (PAS: an image of a DSP or the GPU's zap shader, authenticated and started by TrustZone).
 */
#ifndef _HAKIOS_SCM_H
#define _HAKIOS_SCM_H

#include <stdint.h>
#include <kern/clock.h>

#define SCM_FAST                (1ull << 31)            /* answered at once; without it a call may be preempted */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)            /* a call of the SoC maker's, not one of the Arm standard */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_ONE_VALUE           1                       /* the word describing the arguments: one, a plain number */
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)            /* two: a number, and the address of memory it may read and write */
#define SCM_INTERRUPTED         1                       /* a call that was preempted says so, and is made again so */
#define SCM_GAVE_UP             (-1000)                 /* hakios_scm_call(): never answered (see below) */
#define SCM_BUSY                (-12)                   /* TrustZone busy with another caller's call: asked again later */
#define SCM_RESUMES_MOST        (1u << 20)              /* resumes of one call: a long hash is preempted by every interrupt */
#define SCM_BUSY_TRIES          20                      /* as Linux's qcom_scm: 20 tries ... */
#define SCM_BUSY_WAIT_MS        30                      /* ... 30 ms apart */

/* One SMC, made again with the session TrustZone gave back for as long as it says it was preempted. */
static inline int
hakios_scm_once(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
#if defined(__arm64__) || defined(__aarch64__)
	uint64_t session = 0;

	/*
	 * (Linux resumes for as long as it takes: a session left half done would be TrustZone's for the next call. The
	 * bound is only against a TrustZone that never finishes.)
	 */
	for (uint32_t i = 0; i < SCM_RESUMES_MOST; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			*answer = x1;
			return (int32_t)x0;
		}
	}
#else
	(void)function, (void)arguments, (void)first, (void)second, (void)third, (void)answer;
#endif
	return SCM_GAVE_UP;
}

/*
 * One call: TrustZone's status (0: done), its answer in *answer; SCM_GAVE_UP when it never answered. A call TrustZone
 * says it is too busy for is made again, as Linux makes it, SCM_BUSY_TRIES times SCM_BUSY_WAIT_MS apart.
 */
static inline int
hakios_scm_call(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	int status = SCM_BUSY;

	for (int tries = 0; tries < SCM_BUSY_TRIES && status == SCM_BUSY; tries++) {
		if (tries != 0) {
			delay_for_interval(SCM_BUSY_WAIT_MS, NSEC_PER_MSEC);
		}
		status = hakios_scm_once(function, arguments, first, second, third, answer);
	}
	return status;
}

#endif /* _HAKIOS_SCM_H */
