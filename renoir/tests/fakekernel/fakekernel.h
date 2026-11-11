/*
 * Just enough of XNU's kernel interfaces for a driver's own code to run as a host program
 * (renoir/tests/backlight_driver_test.c): threads are pthreads, waits are condition variables, the
 * clock is the host's, sysctls go into a table the test reads and writes, boot-args are a string the
 * test sets. Every header of this directory includes this one.
 */
#ifndef FAKEKERNEL_H
#define FAKEKERNEL_H
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define __printflike(a, b)      __attribute__((format(printf, a, b)))
#define __unused                __attribute__((unused))
#define __DEVOLATILE(type, var) ((type)(uintptr_t)(volatile void *)(var))
#define MIN(a, b)               ((a) < (b) ? (a) : (b))

typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef struct fake_thread *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define KERN_SUCCESS            0
#define KERN_FAILURE            5
#define THREAD_UNINT            0
#define THREAD_CONTINUE_NULL    ((thread_continue_t)0)
#define NSEC_PER_USEC           1000ull
#define NSEC_PER_MSEC           1000000ull
#define NSEC_PER_SEC            1000000000ull

uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
void delay(int usec);

void assert_wait(void *event, int interruptible);
void assert_wait_timeout(void *event, int interruptible, uint32_t interval, uint64_t scale);
wait_result_t thread_block(thread_continue_t continuation);
void thread_wakeup(void *event);
kern_return_t kernel_thread_start(thread_continue_t fn, void *arg, thread_t *thread);
void thread_deallocate(thread_t thread);

bool ml_get_interrupts_enabled(void);
bool PE_parse_boot_argn(const char *name, void *value, int size);

/* sysctl */
struct sysctl_oid;
struct sysctl_req {
	const void *newptr;
	size_t newlen;
	void *oldptr;
	size_t oldlen, oldidx;
};
#define SYSCTL_HANDLER_ARGS     (struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
typedef int (*sysctl_handler_t) SYSCTL_HANDLER_ARGS;
struct sysctl_oid {
	const char *name;
	int kind;               /* 0 handler, 1 unsigned, 2 signed */
	void *arg1;
	int arg2;
	sysctl_handler_t handler;
	int flags;
};
void fake_sysctl_register(struct sysctl_oid *oid);
int sysctl_io_number(struct sysctl_req *req, long long value, size_t size, void *pointer, int *changed);
int fake_sysctl_out(struct sysctl_req *req, const void *data, size_t size);
#define SYSCTL_OUT(req, p, l)   fake_sysctl_out(req, p, l)
#define OID_AUTO (-1)
#define CTLFLAG_RD 1
#define CTLFLAG_WR 2
#define CTLFLAG_RW 3
#define CTLFLAG_ANYBODY 0x10000000
#define CTLFLAG_LOCKED 0
#define CTLTYPE_INT 0
#define CTLTYPE_STRING 0
#define FAKE_OID(parent, nm, k, a1, a2, h, fl) \
	static struct sysctl_oid fake_oid_##parent##_##nm = { #parent "." #nm, k, a1, a2, h, fl }; \
	__attribute__((constructor)) static void fake_reg_##parent##_##nm(void) { fake_sysctl_register(&fake_oid_##parent##_##nm); }
#define SYSCTL_NODE(parent, nbr, name, access, handler, descr) extern int fake_unused_node_##name
#define SYSCTL_PROC(parent, nbr, name, access, ptr, arg, handler, fmt, descr) FAKE_OID(parent, name, 0, ptr, arg, handler, access)
#define SYSCTL_UINT(parent, nbr, name, access, ptr, val, descr) FAKE_OID(parent, name, 1, (void *)(ptr), val, NULL, access)
#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr) FAKE_OID(parent, name, 2, (void *)(ptr), val, NULL, access)

/* for the tests (fakekernel.c) */
extern const char *boot_args;
extern const char *board;
bool logged(const char *text);
uint32_t get(const char *name);
const char *get_string(const char *name);
void set(const char *name, uint32_t value);
void set_as_root(const char *name, int32_t value);
#endif
