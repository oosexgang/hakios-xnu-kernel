/*
 * Just enough of XNU's kernel interfaces for a driver's own code to run as a host program
 * (workbench/backlight_driver_test.c): threads are pthreads, waits are condition variables, the
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
#define MAX(a, b)               ((a) > (b) ? (a) : (b))

typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *event_t;
#define THREAD_AWAKENED         0
#define THREAD_TIMED_OUT        1
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
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline);  /* now + interval x scale */
void delay(int usec);
void delay_for_interval(uint32_t interval, uint32_t scale);     /* (scale: nanoseconds a unit) */
typedef unsigned long clock_sec_t;
typedef unsigned int clock_usec_t;
void clock_get_calendar_microtime(clock_sec_t *seconds, clock_usec_t *micro);

/* locks (kern/locks.h): one a declaration, a mutex of the host's behind each */
struct fake_lock { pthread_mutex_t mutex; };
typedef struct fake_lock lck_mtx_t;
#define LCK_GRP_DECLARE(group, name)    int group __attribute__((unused))
#define LCK_SPIN_DECLARE(lock, group)   struct fake_lock lock = { PTHREAD_MUTEX_INITIALIZER }
#define lck_spin_lock(lock)             pthread_mutex_lock(&(lock)->mutex)
#define lck_spin_unlock(lock)           pthread_mutex_unlock(&(lock)->mutex)
#define LCK_MTX_DECLARE(lock, group)    struct fake_lock lock = { PTHREAD_MUTEX_INITIALIZER }
#define lck_mtx_lock(lock)              pthread_mutex_lock(&(lock)->mutex)
#define lck_mtx_unlock(lock)            pthread_mutex_unlock(&(lock)->mutex)
#define lck_mtx_try_lock(lock)          (pthread_mutex_trylock(&(lock)->mutex) == 0)
/* sleeping on a channel with a mutex given up meanwhile, and its wake-up: a short sleep here, for whoever asks looks again */
#define PRIBIO                          16
int msleep(void *channel, struct fake_lock *lock, int priority, const char *why, void *time);
void wakeup_one(void *channel);
/* the same, as kern/locks.h names it (every caller looks at its condition again when it returns) */
#define LCK_SLEEP_DEFAULT               0
wait_result_t lck_mtx_sleep(struct fake_lock *lock, int flags, event_t event, int interruptible);
/* (a sleep with a deadline: no one is woken here, so it is a short sleep, as lck_mtx_sleep's) */
wait_result_t lck_mtx_sleep_deadline(struct fake_lock *lock, int flags, event_t event, int interruptible, uint64_t deadline);
void wakeup(void *channel);

/*
 * A call to be made later on a thread of its own (kern/thread_call.h): entered with a deadline, it runs then
 * unless it was cancelled or entered again meanwhile. The deadline is shortened by fake_delay_divisor, as
 * a driver's waits are.
 */
typedef void *thread_call_param_t;
typedef struct fake_thread_call *thread_call_t;
typedef void (*thread_call_func_t)(thread_call_param_t p0, thread_call_param_t p1);
#define PZERO                           22
#define PCATCH                          0x100
thread_call_t thread_call_allocate(thread_call_func_t func, thread_call_param_t p0);
bool thread_call_enter_delayed(thread_call_t call, uint64_t deadline);
bool thread_call_cancel(thread_call_t call);

/* memory (kern/kalloc.h): the host's, zeroed */
#define Z_WAITOK                        0
#define kalloc_data(size, flags)        calloc(1, (size))
#define kfree_data(pointer, size)       free(pointer)
#define Z_ZERO                          0
#define kalloc_type(type, flags)        ((type *)calloc(1, sizeof(type)))
#define kfree_type(type, pointer)       free(pointer)
#define __DECONST(type, var)            ((type)(uintptr_t)(const void *)(var))
#define os_atomic_add(pointer, value, order)    __sync_add_and_fetch((pointer), (value))
#define os_atomic_inc(pointer, order)           __sync_add_and_fetch((pointer), 1)
#define os_atomic_cmpxchg(pointer, old, new, order)     __sync_bool_compare_and_swap((pointer), (old), (new))
typedef int boolean_t;
#ifndef TRUE
#define TRUE                            1
#define FALSE                           0
#endif
/* memory of the kernel's map in one piece (vm/vm_kern.h): the host's, aligned; the map and the flags are not looked at */
#define PAGE_SIZE                       16384
#define PAGE_MASK                       (PAGE_SIZE - 1)
#define round_page(x)                   (((x) + PAGE_MASK) & ~(vm_offset_t)PAGE_MASK)
#define KMA_ZERO                        0x1
#define KMA_KOBJECT                     0x2
#define KMA_DATA                        0x4
#define VM_KERN_MEMORY_IOKIT            1
#define VM_KERN_MEMORY_DIAG             2
extern void *kernel_map;
int kmem_alloc_contig(void *map, vm_offset_t *address, vm_size_t size, vm_offset_t mask, int max_pnum, int pnum_mask,
    int flags, int tag);
int kmem_alloc(void *map, vm_offset_t *address, vm_size_t size, int flags, int tag);
void kmem_free(void *map, vm_offset_t address, vm_size_t size);
#include <sched.h>
#define __builtin_arm_yield()           sched_yield()
void IOSleep(unsigned int milliseconds);
#define VM_WIMG_DEFAULT                 0       /* how memory is mapped (vm/memory_types.h): the host maps nothing */
#define VM_WIMG_WCOMB                   1
uint32_t crc32(uint32_t crc, const void *data, size_t size);    /* libkern/crc.h: zlib's */

/* processes (sys/proc.h): the calling one is fake_pid, and the processes alive are it, fake_other_pid (0: none) and the
 * kernel's, pid 0, which proc_find() finds as XNU's does */
extern int fake_pid, fake_other_pid;
int proc_selfpid(void);
struct proc *proc_find(int pid);
void proc_rele(struct proc *p);
/* ... and its credentials (sys/kauth.h): its user ID is fake_uid, 0 -- root -- unless a test says otherwise */
extern int fake_uid;
typedef struct fake_cred *kauth_cred_t;
kauth_cred_t kauth_cred_get(void);
int suser(kauth_cred_t cred, unsigned short *acflag);   /* 0 for root, else EPERM */

void assert_wait(void *event, int interruptible);
void assert_wait_timeout(void *event, int interruptible, uint32_t interval, uint64_t scale);
wait_result_t thread_block(thread_continue_t continuation);
void thread_wakeup(void *event);
kern_return_t kernel_thread_start(thread_continue_t fn, void *arg, thread_t *thread);
void thread_deallocate(thread_t thread);
thread_t current_thread(void);
kern_return_t clear_wait(thread_t thread, wait_result_t result);   /* a wait that was declared is taken back */
/*
 * Called, if a test sets it, when a thread is about to declare a wait without a time: the moment between
 * what the thread last read and its sleep, where a wake-up from elsewhere wakes nobody.
 */
extern void (*fake_before_wait)(void);
kern_return_t thread_terminate(thread_t thread);        /* (the calling thread's end) */

bool ml_get_interrupts_enabled(void);
bool PE_parse_boot_argn(const char *name, void *value, int size);
bool PE_parse_boot_arg_str(const char *name, char *value, int size);

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
#define CTLTYPE_QUAD 0
#define FAKE_OID(parent, nm, k, a1, a2, h, fl) \
	static struct sysctl_oid fake_oid_##parent##_##nm = { #parent "." #nm, k, a1, a2, h, fl }; \
	__attribute__((constructor)) static void fake_reg_##parent##_##nm(void) { fake_sysctl_register(&fake_oid_##parent##_##nm); }
#define SYSCTL_NODE(parent, nbr, name, access, handler, descr) extern int fake_unused_node_##name
#define SYSCTL_PROC(parent, nbr, name, access, ptr, arg, handler, fmt, descr) FAKE_OID(parent, name, 0, ptr, arg, handler, access)
#define SYSCTL_UINT(parent, nbr, name, access, ptr, val, descr) FAKE_OID(parent, name, 1, (void *)(ptr), val, NULL, access)
#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr) FAKE_OID(parent, name, 2, (void *)(ptr), val, NULL, access)
#define SYSCTL_QUAD(parent, nbr, name, access, ptr, descr) FAKE_OID(parent, name, 3, (void *)(ptr), 0, NULL, access)
#define SYSCTL_DECL(name) extern int fake_unused_decl_##name

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
	off_t offset;                   /* where in the device the next byte is */
};
user_ssize_t uio_resid(struct uio *uio);
off_t uio_offset(struct uio *uio);
void uio_setoffset(struct uio *uio, off_t offset);
void uio_setresid(struct uio *uio, user_ssize_t resid);
int uiomove(caddr_t kernel, int n, struct uio *uio);
extern unsigned fake_uiomove_calls, fake_uiomove_fault_at;     /* (the call numbered fault_at, from 1, faults) */
extern unsigned fake_threads_started, fake_threads_released;    /* (kernel_thread_start's references, and thread_deallocate's) */
struct cdevsw {
	int (*d_open)(dev_t dev, int flags, int type, struct proc *p);
	int (*d_close)(dev_t dev, int flags, int type, struct proc *p);
	int (*d_read)(dev_t dev, struct uio *uio, int ioflag);
	int (*d_write)(dev_t dev, struct uio *uio, int ioflag);
	void *d_ioctl, *d_stop, *d_reset, *d_ttys, *d_select, *d_mmap, *d_strategy, *d_reserved_1, *d_reserved_2;
	int d_type;
};
#define eno_rdwrt               NULL            /* (a node that cannot be read, or written) */
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
int cdevsw_remove(int index, const struct cdevsw *devsw);
void *devfs_make_node(dev_t dev, int type, int uid, int gid, int perms, const char *name, ...);
void devfs_remove(void *handle);        /* (the node of that name reaches nothing from then on) */
/* for the tests: 0, an errno of the driver's, or -1 when no driver has made a node of that name */
int fake_dev_write(const char *name, const void *data, size_t size);
int fake_dev_read(const char *name, void *data, size_t size);
/* ... the same at an offset of the device (pread(), pwrite()), with how many bytes went in *done; and its open and close */
int fake_dev_pwrite(const char *name, const void *data, size_t size, off_t offset, size_t *done);
int fake_dev_pread(const char *name, void *data, size_t size, off_t offset, size_t *done);
int fake_dev_open(const char *name, int flags);
int fake_dev_ioctl(const char *name, unsigned long cmd, void *data);     /* -1 when there is no such node or it takes none */
int fake_dev_close(const char *name);

/*
 * Block devices (sys/buf.h, sys/disk.h): a driver's disk is kept by its node's name too, and a test reads
 * it as a file system would, a request of whole sectors at a time (fake_disk_read()), and asks it what a
 * program asks a disk (fake_disk_ioctl()).
 */
#include <errno.h>
typedef struct proc *proc_t;
typedef struct buf {
	int flags, error;
	dev_t dev;                      /* the node the request is for */
	int64_t blkno;                  /* in 512-byte sectors */
	uint32_t count, resid;
	void *data;
	volatile bool done;
} *buf_t;
#define B_READ                  0x00000001
#define B_ASYNC                 0x00000002      /* (a request whose asker does not wait: done from another thread, maybe) */
int proc_pid(proc_t p);                 /* (fake_pid, whichever process it is) */
#define buf_flags(bp)           ((bp)->flags)
#define buf_device(bp)          ((bp)->dev)
#define buf_blkno(bp)           ((bp)->blkno)
#define buf_count(bp)           ((bp)->count)
#define buf_dataptr(bp)         ((uintptr_t)(bp)->data)
#define buf_seterror(bp, e)     ((bp)->error = (e))
#define buf_setresid(bp, n)     ((bp)->resid = (uint32_t)(n))
#define buf_biodone(bp)         ((bp)->done = true)
struct bdevsw {
	int (*d_open)(dev_t dev, int flags, int type, proc_t p);
	int (*d_close)(dev_t dev, int flags, int type, proc_t p);
	void (*d_strategy)(buf_t bp);
	int (*d_ioctl)(dev_t dev, u_long cmd, caddr_t data, int fflag, proc_t p);
	void *d_dump;
	int (*d_psize)(dev_t dev);
	int d_type;
};
#define eno_dump                NULL
#define eno_getc                NULL
#define eno_putc                NULL
#define D_DISK                  2
#define DEVFS_BLOCK             1
#define GID_OPERATOR            5
#define FREAD                   0x0001
#define FWRITE                  0x0002
#define DKIOCGETBLOCKSIZE       0x40046418ul
#define DKIOCSETBLOCKSIZE       0x80046418ul
#define DKIOCGETBLOCKCOUNT      0x40086419ul
#define DKIOCISWRITABLE         0x4004641dul
#define DKIOCGETMAXBLOCKCOUNTREAD 0x40086440ul
#define DKIOCGETPHYSICALBLOCKSIZE 0x4004644dul
int bdevsw_add(int index, const struct bdevsw *devsw);
int bdevsw_remove(int index, const struct bdevsw *devsw);
int cdevsw_add_with_bdev(int index, const struct cdevsw *devsw, int bdev);
/*
 * For the tests. A request of `size` bytes from sector `sector` on, read or written: 0 or the driver's
 * errno, with what was not transferred in *resid; -1 when no driver has made a disk of that name, -2
 * when the driver did not finish the request.
 */
int fake_disk_read(const char *name, uint64_t sector, void *data, size_t size, size_t *resid);
/* the same, as a file system's read-ahead asks: B_ASYNC, not waited for -- REQUEST's done says when it is (-1: no such disk) */
int fake_disk_read_async(const char *name, uint64_t sector, void *data, size_t size, struct buf *request);
int fake_disk_write(const char *name, uint64_t sector, const void *data, size_t size, size_t *resid);
int fake_disk_open(const char *name, int flags);
int fake_disk_ioctl(const char *name, unsigned long cmd, void *data);

/* the device tree, of which a driver asks only what the loader named the board (pexpert/device_tree.h) */
typedef void *DTEntry;
enum { kError = -1, kSuccess = 0 };
int SecureDTLookupEntry(const DTEntry search, const char *path, DTEntry *entry);
int SecureDTGetProperty(const DTEntry entry, const char *name, void const **value, unsigned int *size);

/*
 * The cores (machine/machine_routines.h): as many as the test says, numbered in their MPIDR as the
 * phones' are (core n is n << 8). A driver that asks which core it runs on is told fake_core.
 */
#define MAX_CPUS                8
typedef struct {
	uint32_t phys_id;
	uint32_t l2_access_penalty;     /* 0 on the big cores, as the kernel's own tables say of them */
} ml_topology_cpu_t;
typedef struct {
	unsigned int num_cpus;
	ml_topology_cpu_t cpus[MAX_CPUS];
} ml_topology_info_t;
const ml_topology_info_t *ml_get_topology_info(void);
bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern unsigned fake_cores;             /* how many cores the kernel has (1) */
extern unsigned fake_core;              /* the core "this" code runs on (0) */
extern unsigned fake_small_cores;       /* the cores with an L2 access penalty, the small ones, by number (none) */
extern bool fake_idle_kept;             /* ml_cpu_idle_time() answers (false: it does not, as no core is said idle) */
extern volatile unsigned fake_busy_pct[MAX_CPUS];  /* ... and each core is that busy from the moment it is set */
extern uint32_t fake_recommended_cores; /* the cores the scheduler was last told to prefer (all) */
#define __builtin_arm_rsr64(name)       ((uint64_t)fake_core << 8)

/*
 * What an interface's ioctl reaches (sys/kern_event.h, sys/kpi_mbuf.h): a request's buffer in the caller's memory
 * is a host pointer here, copied as it is; a kernel event is kept, the last of them, for the test to read.
 */
typedef int errno_t;
typedef struct fake_mbuf *mbuf_t;
/* (an mbuf is the test's own: these, where a driver walks one, are the test's to define) */
mbuf_t mbuf_next(mbuf_t m);
size_t mbuf_len(mbuf_t m);
void *mbuf_data(mbuf_t m);
void mbuf_freem(mbuf_t m);
typedef uint64_t user_addr_t;
#define USER_ADDR_NULL                  ((user_addr_t)0)
#ifndef EAUTH
#define EAUTH                           80
#endif
int copyin(user_addr_t from, void *to, size_t length);
int copyout(const void *from, user_addr_t to, size_t length);
extern size_t fake_copied_out;          /* how many bytes the last copyout() wrote */
void proc_selfname(char *buffer, int size);
int memset_s(void *to, size_t room, int value, size_t length);
#define KEV_VENDOR_APPLE                1
#define KEV_IEEE80211_CLASS             6
struct kev_d_vectors {
	uint32_t data_length;
	void *data_ptr;
};
struct kev_msg {
	uint32_t vendor_code, kev_class, kev_subclass, event_code;
	struct kev_d_vectors dv[5];
};
int kev_post_msg(struct kev_msg *message);
extern struct kev_msg fake_last_event;  /* (its data pointers are the poster's, gone once it returns) */
extern unsigned fake_events;            /* how many were posted */
/* what the last one carried, copied: its vectors one after another (at most 512 bytes), and the codes of the last 16 */
extern uint8_t fake_last_event_data[512];
extern uint32_t fake_last_event_length, fake_event_codes[16];

/* for the tests (fakekernel.c) */
/*
 * For a test: wait (up to `ms') for a condition and then check that very condition. A driver's thread
 * writes its values one after another; a test that waits for the first and compares the second fails
 * once in a thousand runs. With this the two cannot differ. (CHECK and WAIT_FOR are the test's own.)
 */
#define EVENTUALLY(cond, ms, ...) do { WAIT_FOR(cond, ms); CHECK(cond, __VA_ARGS__); } while (0)

extern unsigned fake_delay_divisor;     /* a driver's waits are this many times shorter (1: as long as it asks) */
extern const char *boot_args;
extern const char *board;               /* /chosen/darwin-board; NULL: the loader named none */
bool logged(const char *text);
uint32_t get(const char *name);
const char *get_string(const char *name);
void set(const char *name, uint32_t value);
void set_as_root(const char *name, int32_t value);
int try_as_root(const char *name, int32_t value);       /* the same, with what the sysctl's handler answered (an errno) */
uint64_t get64(const char *name);                       /* a 64-bit counter (SYSCTL_QUAD) */
#endif
