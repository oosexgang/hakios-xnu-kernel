/*
 * Just enough of XNU's kernel interfaces for a driver's own code to run as a host program
 * (tests/backlight_driver_test.c): threads are pthreads, waits are condition variables, the
 * clock is the host's, sysctls go into a table the test reads and writes, boot-args are a string the
 * test sets. Every header of this directory includes this one.
 */
#ifndef FAKEKERNEL_H
#define FAKEKERNEL_H
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

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
void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
void delay(int usec);
void delay_for_interval(uint32_t interval, uint32_t scale);     /* (scale: nanoseconds a unit) */
typedef unsigned long clock_sec_t;
typedef unsigned int clock_usec_t;
void clock_get_calendar_microtime(clock_sec_t *seconds, clock_usec_t *micro);

/* locks (kern/locks.h): one a declaration, a mutex of the host's behind each */
struct fake_lock { pthread_mutex_t mutex; };
#define LCK_GRP_DECLARE(group, name)    int group __attribute__((unused))
#define LCK_SPIN_DECLARE(lock, group)   struct fake_lock lock = { PTHREAD_MUTEX_INITIALIZER }
#define lck_spin_lock(lock)             pthread_mutex_lock(&(lock)->mutex)
#define lck_spin_unlock(lock)           pthread_mutex_unlock(&(lock)->mutex)

void assert_wait(void *event, int interruptible);
void assert_wait_timeout(void *event, int interruptible, uint32_t interval, uint64_t scale);
wait_result_t thread_block(thread_continue_t continuation);
void thread_wakeup(void *event);
kern_return_t kernel_thread_start(thread_continue_t fn, void *arg, thread_t *thread);
void thread_deallocate(thread_t thread);
thread_t current_thread(void);
kern_return_t thread_terminate(thread_t thread);        /* (the calling thread's end) */

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

/*
 * Character devices (sys/conf.h, sys/uio.h, miscfs/devfs/devfs.h): a driver's /dev node is kept by its
 * name, and a test reads and writes it as a program would (fake_dev_read(), fake_dev_write()).
 */
typedef long user_ssize_t;
struct proc;
struct uio {
	char *base;
	size_t resid;
	bool to_kernel;                 /* a write(): the bytes go from the program to the driver */
};
user_ssize_t uio_resid(struct uio *uio);
int uiomove(caddr_t kernel, int n, struct uio *uio);
struct cdevsw {
	int (*d_open)(dev_t dev, int flags, int type, struct proc *p);
	int (*d_close)(dev_t dev, int flags, int type, struct proc *p);
	int (*d_read)(dev_t dev, struct uio *uio, int ioflag);
	int (*d_write)(dev_t dev, struct uio *uio, int ioflag);
	void *d_ioctl, *d_stop, *d_reset, *d_select, *d_mmap, *d_strategy;
	int d_type;
};
#define eno_ioctl               NULL
#define eno_stop                NULL
#define eno_reset               NULL
#define eno_select              NULL
#define eno_mmap                NULL
#define eno_strat               NULL
#define DEVFS_CHAR              0
#define UID_ROOT                0
#define GID_WHEEL               0
int cdevsw_add(int index, const struct cdevsw *devsw);
void *devfs_make_node(dev_t dev, int type, int uid, int gid, int perms, const char *name, ...);
/* for the tests: 0, an errno of the driver's, or -1 when no driver has made a node of that name */
int fake_dev_write(const char *name, const void *data, size_t size);
int fake_dev_read(const char *name, void *data, size_t size);

/* the device tree, of which a driver asks only what the loader named the board (pexpert/device_tree.h) */
typedef void *DTEntry;
enum { kError = -1, kSuccess = 0 };
int SecureDTLookupEntry(const DTEntry search, const char *path, DTEntry *entry);
int SecureDTGetProperty(const DTEntry entry, const char *name, void const **value, unsigned int *size);

/* for the tests (fakekernel.c) */
extern const char *boot_args;
extern const char *board;               /* /chosen/darwin-board; NULL: the loader named none */
bool logged(const char *text);
uint32_t get(const char *name);
const char *get_string(const char *name);
void set(const char *name, uint32_t value);
void set_as_root(const char *name, int32_t value);
#endif
