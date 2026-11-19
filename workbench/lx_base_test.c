/*
 * Host tests of the Wi-Fi driver's layer under ath11k (kernel/lx/src/lx_base.c), compiled as it is: the file is included
 * whole, with what it takes of FreeBSD's LinuxKPI headers (the few types and macros of theirs it uses, as those headers
 * have them) here, and those headers' names empty files (workbench/run.sh makes them, from the file's own #include lines).
 * Under it, the calls of the kernel's that it rests on (sys/board/lx.h): one lock, sleeping under it, threads, the clock,
 * memory -- on the host's threads. What is tested is what the driver counts on of Linux: its work, timers and waits.
 *
 *   lx_base_test                 the scenarios' names
 *   lx_base_test SCENARIO        one scenario a process: the file keeps its state in statics
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "../include/board/lx.h"

/* ---- FreeBSD's, and LinuxKPI's, as far as the file uses them ---------------------------------------------------- */

#define nitems(x)			(sizeof((x)) / sizeof((x)[0]))
#define roundup2(x, y)			(((x) + ((y) - 1)) & (~((y) - 1)))
#define __containerof(p, type, field)	((type *)(void *)((char *)(p) - offsetof(type, field)))
#define __printflike(a, b)		__attribute__((__format__(__printf__, a, b)))
#define MAX_SCHEDULE_TIMEOUT		INT_MAX
#define TASK_RUNNING			0
#define GFP_KERNEL			0
#define NET_SKB_PAD			128
#define DMA_MAPPING_ERROR		(~(dma_addr_t)0)

typedef unsigned char u_char;
typedef uint64_t vm_paddr_t;
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef unsigned gfp_t;
typedef uint64_t dma_addr_t;
typedef int bus_dmasync_op_t;
enum dma_data_direction { DMA_BIDIRECTIONAL, DMA_TO_DEVICE, DMA_FROM_DEVICE, DMA_NONE };

typedef struct { volatile int counter; } atomic_t;
#define atomic_set(v, i)		__atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_read(v)			__atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST)

struct task_struct {
	struct thread *task_thread;
	int pid;
	const char *comm;
	atomic_t usage, state;
};
struct thread {
	int td_tid;
	struct task_struct *td_lkpi_task;
};
struct pcpu {
	int pc_unused;
};
struct lock_object {
	const char *lo_name;
};
struct sx {
	struct lock_object lock_object;
	volatile uintptr_t sx_lock;
};
struct mtx {
	volatile uintptr_t mtx_lock;
};
typedef struct {
	struct mtx m;
} spinlock_t;
void __mtx_lock_flags(volatile uintptr_t *c, int opts, const char *file, int line);
void __mtx_unlock_flags(volatile uintptr_t *c, int opts, const char *file, int line);
#define spin_lock_irq(l)		__mtx_lock_flags(&(l)->m.mtx_lock, 0, __FILE__, __LINE__)
#define spin_unlock_irq(l)		__mtx_unlock_flags(&(l)->m.mtx_lock, 0, __FILE__, __LINE__)

struct completion {
	unsigned int done;
};
typedef struct wait_queue_head {
	int unused;
} wait_queue_head_t;
typedef struct wait_queue {
	int unused;
} wait_queue_t;

struct workqueue_struct {
	int unused;
};
struct work_struct;
typedef void (*work_func_t)(struct work_struct *);
struct work_struct {
	atomic_t state;
	struct workqueue_struct *work_queue;
	work_func_t func;
};
struct delayed_work {
	struct work_struct work;
};
#define INIT_WORK(w, f)	do { (w)->func = (f); (w)->work_queue = NULL; atomic_set(&(w)->state, 0); } while (0)
bool linux_queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work);
bool linux_queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay);
bool linux_cancel_delayed_work(struct delayed_work *dwork);
bool linux_cancel_delayed_work_sync(struct delayed_work *dwork);
bool linux_cancel_work_sync(struct work_struct *work);
void linux_init_delayed_work(struct delayed_work *dwork, work_func_t function);
struct workqueue_struct *linux_create_workqueue_common(const char *name, int cpus);
void linux_destroy_workqueue(struct workqueue_struct *wq);
#define work_pending(w)			(atomic_read(&(w)->state) != 0)

struct callout {
	int c_iflags;
};
struct timer_list {
	struct callout callout;
	void (*function_415)(struct timer_list *);
	unsigned long expires;
};
int mod_timer(struct timer_list *timer, unsigned long expires);
int timer_delete_sync(struct timer_list *timer);

struct device {
	int unused;
};
struct skb_shared_info {
	int nr_frags;
};
struct sk_buff {
	struct sk_buff *next, *prev;
	void *m;
	struct skb_shared_info *shinfo;
	unsigned truesize, len;
	uint8_t *head, *data, *tail, *end;
};
static inline void skb_reset_tail_pointer(struct sk_buff *skb) { skb->tail = skb->data; }
static inline void skb_reserve(struct sk_buff *skb, int length) { skb->data += length; skb->tail += length; }

struct linuxkpi_firmware {
	size_t size;
	const uint8_t *data;
};
struct idr_layer;
struct idr {
	struct idr_layer *top;
};
struct dmi_system_id {
	int unused;
};
#include "../kernel/lx/include/linux/rhashtable.h"

/*
 * (the file's own printf() and log() are not the C library's: theirs stand under other names here; and it is compiled for
 * the phone with no warnings asked for -- what it leaves unused, and the kernel's calls that take no volatile, are no error)
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#define printf		lx_base_printf
#define log		lx_base_log
#define explicit_bzero	lx_base_explicit_bzero
#define arc4random_buf	lx_base_arc4random_buf
#include "../kernel/lx/src/lx_base.c"
#undef printf
#undef log
#undef explicit_bzero
#undef arc4random_buf
#pragma GCC diagnostic pop

/* ---- the kernel's side (sys/board/lx.h), on the host's threads ----------------------------------------------- */

static pthread_mutex_t os_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t os_woken = PTHREAD_COND_INITIALIZER;	/* (one for every address: a sleeper looks again anyway) */
static bool verbose;

void *lx_os_alloc(unsigned long size) { return calloc(1, size); }
void lx_os_free(void *memory, unsigned long size __attribute__((unused))) { free(memory); }

void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	void *memory = aligned_alloc(4096, size);

	if (memory != NULL) {
		memset(memory, 0, size);
		*address = 0x80000000ull;
	}
	return memory;
}

void *lx_os_map_registers(unsigned long long physical __attribute__((unused)), unsigned long size __attribute__((unused)))
{
	return NULL;
}

void lx_os_print(const char *text) { if (verbose) fputs(text, stderr); }
void lx_os_tell(const char *text) { if (verbose) fputs(text, stderr); }
void lx_os_panic(const char *text) { fprintf(stderr, "panic: %s\n", text); abort(); }

unsigned long long
lx_os_nanoseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec;
}

void *lx_os_thread_self(void) { return (void *)pthread_self(); }

struct os_start {
	void (*function)(void *);
	void *argument;
};

static void *
os_thread_runs(void *parameter)
{
	struct os_start start = *(struct os_start *)parameter;

	free(parameter);
	start.function(start.argument);
	return NULL;
}

int
lx_os_thread(const char *name __attribute__((unused)), void (*function)(void *), void *argument)
{
	struct os_start *start = malloc(sizeof(*start));
	pthread_t thread;

	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (pthread_create(&thread, NULL, os_thread_runs, start) != 0) {
		free(start);
		return EAGAIN;
	}
	pthread_detach(thread);
	return 0;
}

void lx_os_lock(void) { pthread_mutex_lock(&os_lock); }
void lx_os_unlock(void) { pthread_mutex_unlock(&os_lock); }

int
lx_os_sleep(const void *address __attribute__((unused)), unsigned long long nanoseconds)
{
	struct timespec until;
	unsigned long long at;

	if (nanoseconds == 0) {
		pthread_cond_wait(&os_woken, &os_lock);
		return 0;
	}
	clock_gettime(CLOCK_REALTIME, &until);
	at = (unsigned long long)until.tv_nsec + nanoseconds;
	until.tv_sec += (time_t)(at / 1000000000ull);
	until.tv_nsec = (long)(at % 1000000000ull);
	return pthread_cond_timedwait(&os_woken, &os_lock, &until) == ETIMEDOUT;
}

void lx_os_wakeup(const void *address __attribute__((unused))) { pthread_cond_broadcast(&os_woken); }
void lx_os_random(void *buffer, unsigned long size) { memset(buffer, 0x5a, size); }
unsigned lx_os_option(const char *name __attribute__((unused))) { return 0; }

int
lx_os_firmware(const char *name __attribute__((unused)), const void **data __attribute__((unused)),
    unsigned long *size __attribute__((unused)))
{
	return ENOENT;
}

/* ---- the scenarios ----------------------------------------------------------------------------------------- */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void
ms_sleep(unsigned ms)
{
	usleep(ms * 1000u);
}

/* work that counts how often it has run */
static struct counted {
	struct delayed_work dwork;
	volatile int runs;
} counted;

static void
counted_runs(struct work_struct *work)
{
	struct counted *c = __containerof(work, struct counted, dwork.work);

	__atomic_add_fetch(&c->runs, 1, __ATOMIC_SEQ_CST);
}

/* a piece of work that runs for as long as it is told to: the queue's thread is busy with it meanwhile */
static struct work_struct blocker;
static volatile bool blocking, blocked;

static void
blocker_runs(struct work_struct *work __attribute__((unused)))
{
	blocked = true;
	while (blocking) {
		ms_sleep(1);
	}
	blocked = false;
}

/* its first run, and time for a second, if one were to come */
static void
runs_settle(void)
{
	for (int ms = 0; counted.runs == 0 && ms < 2000; ms++) {
		ms_sleep(1);
	}
	ms_sleep(100);
}

static struct workqueue_struct *
start(void)
{
	struct workqueue_struct *wq;

	CHECK(lx_base_init() == 0, "the ticker runs");
	wq = linux_create_workqueue_common("test", 1);
	CHECK(wq != NULL, "a queue is made");
	INIT_WORK(&blocker, blocker_runs);
	linux_init_delayed_work(&counted.dwork, counted_runs);
	return wq;
}

static void
wait_for(volatile bool *flag, bool value)
{
	for (int ms = 0; *flag != value && ms < 2000; ms++) {
		ms_sleep(1);
	}
}

/*
 * A delayed work whose time has come is on its queue, behind work that runs (ath11k: the connection_loss_work behind
 * the radio's other work on mac80211's queue). Queued again meanwhile, it is pending: Linux's queue_delayed_work() says
 * false and does nothing -- the work runs once.
 */
static void
delayed_pending_on_queue(void)
{
	struct workqueue_struct *wq = start();

	blocking = true;
	CHECK(linux_queue_work_on(0, wq, &blocker), "the blocker is queued");
	wait_for(&blocked, true);
	CHECK(linux_queue_delayed_work_on(0, wq, &counted.dwork, 5), "queued with a delay");
	/* its time comes: it waits on the queue, behind the blocker */
	for (int ms = 0; !work_pending(&counted.dwork.work) && ms < 2000; ms++) {
		ms_sleep(1);
	}
	CHECK(work_pending(&counted.dwork.work), "on the queue once its time has come");
	CHECK(!linux_queue_delayed_work_on(0, wq, &counted.dwork, 5), "queued again while pending: false, as Linux's");
	blocking = false;
	runs_settle();
	CHECK(counted.runs == 1, "it ran once, not %d times", counted.runs);
}

/* Waiting for its time, queued again without a delay: pending, as Linux has it -- false, and it runs once, at its time. */
static void
delayed_pending_waiting(void)
{
	struct workqueue_struct *wq = start();

	CHECK(linux_queue_delayed_work_on(0, wq, &counted.dwork, 50), "queued with a delay");
	CHECK(!linux_queue_delayed_work_on(0, wq, &counted.dwork, 0), "queued again at once while it waits: false, as Linux's");
	runs_settle();
	CHECK(counted.runs == 1, "it ran once, not %d times", counted.runs);
}

/*
 * kfree(NULL), which the driver does all the time (on every path out of a function that may not have allocated yet), is
 * nothing -- and finds no size in front of NULL: that subtraction is undefined, which the sanitizers here say.
 */
static void
kfree_null(void)
{
	long before = allocations;
	void *memory = lkpi_kmalloc(100, GFP_KERNEL);

	CHECK(memory != NULL && allocations == before + 1, "a piece is out");
	lkpi_kfree(NULL);
	CHECK(allocations == before + 1, "kfree(NULL) gave back nothing (%ld out)", allocations - before);
	lkpi_kfree(memory);
	CHECK(allocations == before, "the piece is back");
}

/*
 * A size so large that the layer's own additions -- the rounding of a ring's size to 64 bytes, the room a socket buffer
 * keeps in front -- would come round past 0: none (NULL), as Linux's, and not a few bytes the caller then writes past.
 */
static void
sizes_past_the_largest(void)
{
	dma_addr_t handle = 0;
	uint32_t used;
	void *ring;
	struct sk_buff *skb;

	CHECK(lx_pool_init() == 0, "the radio's memory is there");
	used = pool.rings_used;
	ring = linux_dma_alloc_coherent(NULL, (size_t)-10, &handle, GFP_KERNEL);
	CHECK(ring == NULL, "dma_alloc_coherent() of %zu bytes: %p", (size_t)-10, ring);
	CHECK(pool.rings_used == used, "nothing of the rings' room taken (%u bytes)", pool.rings_used - used);
	skb = linuxkpi_dev_alloc_skb((size_t)-64, GFP_KERNEL);
	CHECK(skb == NULL, "dev_alloc_skb() of %zu bytes: %p", (size_t)-64, (void *)skb);
	/* (and what fits is still had) */
	ring = linux_dma_alloc_coherent(NULL, 100, &handle, GFP_KERNEL);
	CHECK(ring != NULL && handle != 0, "a ring of 100 bytes");
	skb = linuxkpi_dev_alloc_skb(2048, GFP_KERNEL);
	CHECK(skb != NULL && skb->end - skb->data >= 2048, "a buffer of 2048 bytes");
	linuxkpi_kfree_skb(skb);
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "delayed_pending_on_queue", delayed_pending_on_queue },
	{ "delayed_pending_waiting", delayed_pending_waiting },
	{ "kfree_null", kfree_null },
	{ "sizes_past_the_largest", sizes_past_the_largest },
};

int
main(int argc, char **argv)
{
	if (argc < 2) {
		for (unsigned i = 0; i < nitems(scenarios); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	verbose = getenv("LX_BASE_TEST_VERBOSE") != NULL;
	for (unsigned i = 0; i < nitems(scenarios); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-32s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
