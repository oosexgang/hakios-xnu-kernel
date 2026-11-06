/*
 * What the driver's files ask of a Linux kernel around them, as far as the files built so far
 * ask it: locks, sleeping and waking, work queues, timers, memory, memory the radio reads and
 * writes, socket buffers, firmware files. The headers that declare all this are FreeBSD's
 * LinuxKPI; FreeBSD's own implementations stand on FreeBSD's kernel and are not used. These
 * stand on the few calls of lx_os.h, which the XNU side gives (bsd/dev/redwood_lx.c), and are
 * written for a driver that runs on threads only: nothing here is called from an interrupt.
 *
 * Everything that sleeps does so with lx_os_sleep() under the one lock of lx_os.h, and wakes
 * every little while to look again, so that a wake-up that came between a look and the sleep
 * costs milliseconds and nothing else.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/callout.h>
#include <sys/kdb.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/pmap.h>

#include <linux/bottom_half.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmi.h>
#include <linux/firmware.h>
#include <linux/idr.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/rhashtable.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <lx_os.h>
#include "lx.h"

#define SLICE_NS	(5 * 1000000ull)	/* how long a sleep lasts before it looks again */

/* ---- saying things ------------------------------------------------------------------------- */

int
printf(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	lx_os_print(line);
	return (0);
}

/* A line that is said whatever the kernel does with the others (lx_os_tell()): outcomes, and what has gone wrong. */
void
lx_tell(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	lx_os_tell(line);
}

/* A function of a part of the driver that is not built yet (out/stubs.s): its name, a few times; the answer is 0. */
long
lx_stub(const char *name)
{
	static struct {
		const char *name;
		unsigned calls;
	} seen[128];

	for (unsigned i = 0; i < nitems(seen); i++) {
		if (seen[i].name == NULL) {
			seen[i].name = name;
		}
		if (seen[i].name == name) {
			if (seen[i].calls++ < 3) {
				printf("lx: %s() is not there yet: answers 0%s\n", name, seen[i].calls == 3 ? " (said for the last time)" : "");
				lx_mark(name);
			}
			break;
		}
	}
	return (0);
}

/*
 * A point the driver's start has reached. With the kernel's option wifi_lx_step=MS it is also
 * waited at, so that whoever follows the log from outside has the line before what comes next:
 * for finding where a run ends that ends without a word.
 */
void
lx_mark(const char *what)
{
	static unsigned wait = ~0u;

	if (wait == ~0u) {
		wait = lx_os_option("wifi_lx_step");
	}
	if (wait != 0) {
		printf("lx: reached: %s\n", what);
		lx_pause_ms(wait);
	}
}

void
linux_dump_stack(void)
{
}

int linuxkpi_warn_dump_stack;

/* ---- threads, and the time ------------------------------------------------------------------- */

/*
 * FreeBSD's code finds "the current thread" and LinuxKPI's hangs its "current task" on it. A
 * thread of the kernel's gets one of these the first time it is seen here, and keeps it.
 */
static struct lx_thread {
	void *volatile self;			/* lx_os_thread_self() of the thread that has it; NULL: free */
	struct thread td;
	struct task_struct task;
	const void *waiting_on;			/* linux_wait_event_common(): what for, and ... */
	unsigned long long deadline;		/* ... until when (0: for good) */
} lx_threads[24];

static struct lx_thread *
lx_thread(void)
{
	void *self = lx_os_thread_self();

	for (unsigned i = 0; i < nitems(lx_threads); i++) {
		if (lx_threads[i].self == self) {
			return (&lx_threads[i]);
		}
	}
	for (unsigned i = 0; i < nitems(lx_threads); i++) {
		if (lx_threads[i].self == NULL && __sync_bool_compare_and_swap(&lx_threads[i].self, NULL, self)) {
			lx_threads[i].td.td_tid = 100 + i;
			return (&lx_threads[i]);
		}
	}
	lx_os_panic("lx: more threads in the Wi-Fi driver than it has room for");
}

struct thread *
lx_curthread(void)
{
	return (&lx_thread()->td);
}

struct pcpu *
lx_pcpu(void)
{
	static struct pcpu one;			/* (who runs where is nobody's business here) */

	return (&one);
}

static int
alloc_current(struct thread *td, int flags)
{
	struct lx_thread *t = __containerof(td, struct lx_thread, td);

	t->task.task_thread = td;
	t->task.pid = td->td_tid;
	t->task.comm = "wifi";
	atomic_set(&t->task.usage, 1);
	atomic_set(&t->task.state, TASK_RUNNING);
	td->td_lkpi_task = &t->task;
	return (0);
}

int (*lkpi_alloc_current)(struct thread *, int) = alloc_current;

/* What FreeBSD's and LinuxKPI's inline code looks at, and finds at rest. */
u_char kdb_active;
bool scheduler_stopped;
_Bool lse_supported;				/* (atomics the plain way) */
uintptr_t *__start_set_pcpu, *__stop_set_pcpu;	/* (the bounds of a section of per-processor data: there is none) */

void
local_bh_disable(void)
{
}

void
local_bh_enable(void)
{
}

/*
 * RCU, as far as the driver needs it: readers say when they begin and end to look at something
 * shared, and whoever is about to free such a thing waits until nobody is looking any more
 * (which is more than Linux asks: it waits only for those who were looking already).
 */
static volatile int rcu_readers;

void
linux_rcu_read_lock(unsigned type)
{
	__sync_fetch_and_add(&rcu_readers, 1);
}

void
linux_rcu_read_unlock(unsigned type)
{
	__sync_fetch_and_sub(&rcu_readers, 1);
}

void
linux_synchronize_rcu(unsigned type)
{
	for (unsigned ms = 0; rcu_readers != 0 && ms < 1000; ms++) {
		lx_pause_ms(1);
	}
}

/* A thousand ticks a second, counted by a thread that also runs the timers that have come due. */
int hz = 1000;
unsigned long jiffies;
uint64_t lkpi_msec2hz_rem = 1, lkpi_msec2hz_div = 1, lkpi_msec2hz_max = 0x7fffffff;

static unsigned long long
ticks_to_ns(unsigned long ticks)
{
	return ((unsigned long long)ticks * 1000000ull);
}

static struct timer_list *armed[16];
static struct {
	struct delayed_work *work;		/* to be queued on ... */
	struct workqueue_struct *queue;		/* ... when the ticks have reached ... */
	unsigned long when;
} later[16];

static void
ticker(void *argument)
{
	unsigned long long began = lx_os_nanoseconds();

	for (;;) {
		struct timer_list *due = NULL;
		struct delayed_work *work = NULL;
		struct workqueue_struct *queue = NULL;

		lx_os_lock();
		lx_os_sleep(&jiffies, 2 * 1000000ull);
		jiffies = (unsigned long)((lx_os_nanoseconds() - began) / 1000000ull);
		for (unsigned i = 0; i < nitems(armed) && due == NULL; i++) {
			if (armed[i] != NULL && (long)(jiffies - armed[i]->expires) >= 0) {
				due = armed[i];
				armed[i] = NULL;
			}
		}
		for (unsigned i = 0; i < nitems(later) && work == NULL; i++) {
			if (later[i].work != NULL && (long)(jiffies - later[i].when) >= 0) {
				work = later[i].work;
				queue = later[i].queue;
				later[i].work = NULL;
			}
		}
		lx_os_unlock();
		/* (a timer that a part of the driver not built yet would have given its function has none) */
		if (due != NULL && due->function_415 != NULL) {
			due->function_415(due);
		}
		if (work != NULL) {
			linux_queue_work_on(0, queue, &work->work);
		}
	}
}

/* Work that is to be queued when some time has passed: the thread above does that. */
void
linux_init_delayed_work(struct delayed_work *dwork, work_func_t function)
{
	bzero(dwork, sizeof(*dwork));
	INIT_WORK(&dwork->work, function);
}

bool
linux_queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
	int slot = -1;

	if (delay == 0) {
		return (linux_queue_work_on(cpu, wq, &dwork->work));
	}
	lx_os_lock();
	for (unsigned i = 0; i < nitems(later); i++) {
		if (later[i].work == dwork) {
			lx_os_unlock();
			return (false);			/* (it waits already) */
		}
		if (later[i].work == NULL && slot < 0) {
			slot = (int)i;
		}
	}
	if (slot >= 0) {
		later[slot].work = dwork;
		later[slot].queue = wq;
		later[slot].when = jiffies + delay;
	}
	lx_os_unlock();
	if (slot < 0) {
		printf("lx: no room for more work that waits for its time: one piece will not be done\n");
	}
	return (slot >= 0);
}

/* Not to be queued any more, if it waits for its time; true if it did. */
bool
linux_cancel_delayed_work(struct delayed_work *dwork)
{
	bool waited = false;

	lx_os_lock();
	for (unsigned i = 0; i < nitems(later); i++) {
		if (later[i].work == dwork) {
			later[i].work = NULL;
			waited = true;
		}
	}
	lx_os_unlock();
	return (waited);
}

/* (A timer's function that is running on the ticker's thread at this moment is not waited for.) */
int
timer_delete_sync(struct timer_list *timer)
{
	int was = 0;

	lx_os_lock();
	for (unsigned i = 0; i < nitems(armed); i++) {
		if (armed[i] == timer) {
			armed[i] = NULL;
			was = 1;
		}
	}
	lx_os_unlock();
	return (was);
}

void
lx_pause_ms(unsigned ms)
{
	static char nothing;			/* (an address nobody wakes) */

	lx_os_lock();
	lx_os_sleep(&nothing, ms * 1000000ull);
	lx_os_unlock();
}

void
callout_init(struct callout *c, int mpsafe)
{
	bzero(c, sizeof(*c));
}

int
mod_timer(struct timer_list *timer, unsigned long expires)
{
	int was = 0, slot = -1;

	lx_os_lock();
	for (unsigned i = 0; i < nitems(armed); i++) {
		if (armed[i] == timer) {
			was = 1;
			slot = (int)i;
		} else if (armed[i] == NULL && slot < 0) {
			slot = (int)i;
		}
	}
	timer->expires = expires;
	if (slot >= 0) {
		armed[slot] = timer;
	}
	lx_os_unlock();
	if (slot < 0) {
		printf("lx: no room for another timer: one will not fire\n");
	}
	return (was);
}

/* ---- locks ------------------------------------------------------------------------------------ */

/*
 * LinuxKPI makes a Linux mutex of FreeBSD's "sx" and a spinlock of FreeBSD's "mtx". Both are a
 * word here that holds who has the lock; whoever finds it taken sleeps until it is given back.
 * (A spinlock that sleeps is no harm where nothing runs in an interrupt.)
 */
static void
word_lock(volatile uintptr_t *word)
{
	uintptr_t self = (uintptr_t)lx_os_thread_self();

	lx_os_lock();
	while (*word != 0) {
		if (*word == self) {
			lx_os_unlock();
			lx_os_panic("lx: a lock taken twice by the thread that has it");
		}
		lx_os_sleep(word, SLICE_NS);
	}
	*word = self;
	lx_os_unlock();
}

static void
word_unlock(volatile uintptr_t *word)
{
	lx_os_lock();
	*word = 0;
	lx_os_wakeup(word);
	lx_os_unlock();
}

void
sx_init_flags(struct sx *sx, const char *description, int opts)
{
	bzero(sx, sizeof(*sx));
	sx->lock_object.lo_name = description;
}

int
_sx_xlock(struct sx *sx, int opts, const char *file, int line)
{
	word_lock(&sx->sx_lock);
	return (0);
}

void
_sx_xunlock(struct sx *sx, const char *file, int line)
{
	word_unlock(&sx->sx_lock);
}

void
_mtx_init(volatile uintptr_t *c, const char *name, const char *type, int opts)
{
	*c = 0;
}

void
__mtx_lock_flags(volatile uintptr_t *c, int opts, const char *file, int line)
{
	word_lock(c);
}

void
__mtx_unlock_flags(volatile uintptr_t *c, int opts, const char *file, int line)
{
	word_unlock(c);
}

/* ---- waiting for something ---------------------------------------------------------------------- */

void
linux_complete_common(struct completion *c, int all)
{
	lx_os_lock();
	if (all) {
		c->done = UINT_MAX;
	} else if (c->done != UINT_MAX) {
		c->done++;
	}
	lx_os_wakeup(c);
	lx_os_unlock();
}

int
linux_completion_done(struct completion *c)
{
	return (c->done != 0);
}

/* What is left of the time when the completion comes (at least 1), or 0 if it does not. */
unsigned long
linux_wait_for_timeout_common(struct completion *c, unsigned long timeout, int flags)
{
	unsigned long long deadline = lx_os_nanoseconds() + ticks_to_ns(timeout), now;
	unsigned long left = 0;

	lx_os_lock();
	for (;;) {
		now = lx_os_nanoseconds();
		if (c->done != 0) {
			if (c->done != UINT_MAX) {
				c->done--;
			}
			left = now < deadline ? (unsigned long)((deadline - now) / 1000000ull) : 0;
			left = left == 0 ? 1 : left;
			break;
		}
		if (now >= deadline) {
			break;
		}
		lx_os_sleep(c, deadline - now < SLICE_NS ? deadline - now : SLICE_NS);
	}
	lx_os_unlock();
	return (left);
}

/*
 * wait_event...() is a loop in LinuxKPI's header: look at the condition, call this, look again.
 * It gives the whole time allowed each time round, so when the first round began is kept here.
 */
void
linux_prepare_to_wait(wait_queue_head_t *wqh, wait_queue_t *wq, int state)
{
}

void
linux_finish_wait(wait_queue_head_t *wqh, wait_queue_t *wq)
{
	lx_thread()->waiting_on = NULL;
}

int
linux_wait_event_common(wait_queue_head_t *wqh, wait_queue_t *wq, long timeout, unsigned int state, spinlock_t *lock)
{
	struct lx_thread *t = lx_thread();
	unsigned long long now = lx_os_nanoseconds(), slice = SLICE_NS;

	if (t->waiting_on != wqh) {
		t->waiting_on = wqh;
		t->deadline = timeout == MAX_SCHEDULE_TIMEOUT ? 0 : now + ticks_to_ns((unsigned long)timeout);
	}
	if (t->deadline != 0) {
		if (now >= t->deadline) {
			return (-EWOULDBLOCK);
		}
		slice = t->deadline - now < slice ? t->deadline - now : slice;
	}
	if (lock != NULL) {
		spin_unlock_irq(lock);
	}
	lx_os_lock();
	lx_os_sleep(wqh, slice);
	lx_os_unlock();
	if (lock != NULL) {
		spin_lock_irq(lock);
	}
	return (0);
}

void
linux_wake_up(wait_queue_head_t *wqh, unsigned int state, int nr, bool locked)
{
	lx_os_lock();
	lx_os_wakeup(wqh);
	lx_os_unlock();
}

int
autoremove_wake_function(wait_queue_t *wq, unsigned int state, int flags, void *key)
{
	return (1);
}

/* ---- work queues ------------------------------------------------------------------------------ */

/*
 * A queue is a thread and a list of the work waiting for it: work runs one piece at a time in
 * the order it was queued, which is what an "ordered" queue promises and more than the other
 * kinds do. A piece of work that is queued while it waits is not queued twice; one that is
 * queued while it runs runs again.
 */
enum { WORK_IDLE = 0, WORK_QUEUED = 2 };

struct lx_workqueue {
	struct workqueue_struct shown;		/* what LinuxKPI's headers know of a queue (nothing of it is used) */
	struct work_struct *waiting[64];
	unsigned first, count;
	struct work_struct *running;
	void *thread;
	char name[24];
};

static void
worker(void *argument)
{
	struct lx_workqueue *queue = argument;
	struct work_struct *work;

	queue->thread = lx_os_thread_self();
	for (;;) {
		lx_os_lock();
		queue->running = NULL;
		lx_os_wakeup(&queue->running);
		while (queue->count == 0) {
			lx_os_sleep(queue, SLICE_NS);
		}
		work = queue->waiting[queue->first];
		queue->first = (queue->first + 1) % nitems(queue->waiting);
		queue->count--;
		queue->running = work;
		atomic_set(&work->state, WORK_IDLE);
		lx_os_unlock();
		work->func(work);
	}
}

struct workqueue_struct *
linux_create_workqueue_common(const char *name, int cpus)
{
	struct lx_workqueue *queue = lx_os_alloc(sizeof(*queue));

	if (queue == NULL) {
		return (NULL);
	}
	strlcpy(queue->name, name, sizeof(queue->name));
	if (lx_os_thread(queue->name, worker, queue) != 0) {
		lx_os_free(queue, sizeof(*queue));
		return (NULL);
	}
	return (&queue->shown);
}

void
linux_destroy_workqueue(struct workqueue_struct *wq)
{
	/* (its thread stays, asleep: queues are made once here and never given up while the kernel runs) */
}

bool
linux_queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
	struct lx_workqueue *queue = __containerof(wq, struct lx_workqueue, shown);
	bool queued = false;

	lx_os_lock();
	if (atomic_read(&work->state) == WORK_IDLE && queue->count < nitems(queue->waiting)) {
		atomic_set(&work->state, WORK_QUEUED);
		work->work_queue = wq;
		queue->waiting[(queue->first + queue->count++) % nitems(queue->waiting)] = work;
		lx_os_wakeup(queue);
		queued = true;
	}
	lx_os_unlock();
	return (queued);
}

/* Not to run any more if it waits; and if it runs, until it has ended (unless it is the caller that runs it). */
bool
linux_cancel_work_sync(struct work_struct *work)
{
	struct lx_workqueue *queue;
	bool waited = false;

	if (work->work_queue == NULL) {
		return (false);
	}
	queue = __containerof(work->work_queue, struct lx_workqueue, shown);
	lx_os_lock();
	for (unsigned i = 0; i < queue->count; i++) {
		unsigned at = (queue->first + i) % nitems(queue->waiting);

		if (queue->waiting[at] == work) {
			for (; i + 1 < queue->count; i++) {
				queue->waiting[(queue->first + i) % nitems(queue->waiting)] =
				    queue->waiting[(queue->first + i + 1) % nitems(queue->waiting)];
			}
			queue->count--;
			atomic_set(&work->state, WORK_IDLE);
			waited = true;
			break;
		}
	}
	while (queue->running == work && queue->thread != lx_os_thread_self()) {
		lx_os_sleep(&queue->running, SLICE_NS);
		waited = true;
	}
	lx_os_unlock();
	return (waited);
}

bool
linux_cancel_delayed_work_sync(struct delayed_work *dwork)
{
	bool waited = linux_cancel_delayed_work(dwork);

	return (linux_cancel_work_sync(&dwork->work) || waited);
}

void
linux_work_fn(void *context, int pending)
{
	/* (the function FreeBSD's task queues would call; LinuxKPI's headers want its address) */
}

/* ---- memory ------------------------------------------------------------------------------------- */

/* kmalloc(): the size goes in front, because kfree() is not told it. */
static volatile long allocations, allocated;	/* pieces out now, and their bytes: for lx_memory_say() */

void *
lkpi_kmalloc(size_t size, gfp_t flags)
{
	size_t *block = lx_os_alloc(size + 16);

	if (block == NULL) {
		return (NULL);
	}
	block[0] = size + 16;
	__sync_fetch_and_add(&allocations, 1);
	__sync_fetch_and_add(&allocated, (long)(size + 16));
	return (block + 2);
}

void
lkpi_kfree(const void *memory)
{
	size_t *block = (size_t *)(uintptr_t)memory - 2;

	if (memory != NULL) {
		__sync_fetch_and_sub(&allocations, 1);
		__sync_fetch_and_sub(&allocated, (long)block[0]);
		lx_os_free(block, block[0]);
	}
}

void *
_ioremap_attr(vm_paddr_t physical, unsigned long size, int attributes)
{
	return (lx_os_map_registers(physical, size));
}

void
iounmap(void *address)
{
}

/*
 * Memory the radio itself reads and writes: its rings, and the buffers it fills. The radio
 * reaches memory through the SoC's SMMU, which lets it see what it has been shown and nothing
 * else; so all such memory comes out of one piece, taken once, which the kernel's side shows
 * the radio and says the radio's address of (lx_os_radio_memory()). That piece is not behind
 * the processor's cache: there is nothing to do between our writing and the radio's reading,
 * or the other way round. The first part of it is handed out for good, a stretch at a time
 * (rings); the rest is slots of one size, taken and given back (buffers).
 */
#define POOL_SIZE	(44u << 20)
#define POOL_RINGS	(12u << 20)		/* (the data path's rings and link descriptors come to about 6 MB) */
#define POOL_SLOT	4096u
#define POOL_SLOTS	((POOL_SIZE - POOL_RINGS) / POOL_SLOT)

static struct {
	uint8_t *memory;
	unsigned long long address;		/* the radio's, of the first byte */
	uint32_t rings_used;
	uint32_t free_slots[POOL_SLOTS], free_count;
} pool;

int
lx_pool_init(void)
{
	if (pool.memory != NULL) {
		return (0);
	}
	pool.memory = lx_os_radio_memory(POOL_SIZE, &pool.address);
	if (pool.memory == NULL) {
		return (ENOMEM);
	}
	for (uint32_t i = 0; i < POOL_SLOTS; i++) {
		pool.free_slots[i] = POOL_SLOTS - 1 - i;
	}
	pool.free_count = POOL_SLOTS;
	printf("lx: %u MB for the radio to read and write; it knows them from %#llx on\n", POOL_SIZE >> 20, pool.address);
	return (0);
}

static bool
in_pool(const void *memory, size_t size)
{
	return ((const uint8_t *)memory >= pool.memory && (const uint8_t *)memory + size <= pool.memory + POOL_SIZE);
}

void *
linux_dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag)
{
	uint32_t at;

	size = roundup2(size, 64);
	lx_os_lock();
	at = pool.rings_used;
	if (pool.memory == NULL || at + size > POOL_RINGS) {
		lx_os_unlock();
		printf("lx: no room for %zu more bytes of rings\n", size);
		return (NULL);
	}
	pool.rings_used += size;
	lx_os_unlock();
	bzero(pool.memory + at, size);
	*dma_handle = pool.address + at;
	return (pool.memory + at);
}

void
kmem_free(void *address, vm_size_t size)
{
	/* (dma_free_coherent() ends here: rings are not given back) */
}

/*
 * Linux's memory for a device that need not be coherent: the same piece. (The data path takes a
 * few kilobytes of it for every traffic class of a station it meets, and gives them back when
 * the station goes: here they stay taken, like the rings.)
 */
void *
dma_alloc_noncoherent(struct device *dev, size_t size, dma_addr_t *handle, enum dma_data_direction direction, gfp_t gfp)
{
	return (linux_dma_alloc_coherent(dev, size, handle, gfp));
}

void
dma_free_noncoherent(struct device *dev, size_t size, void *memory, dma_addr_t handle, enum dma_data_direction direction)
{
}

/*
 * FreeBSD's question for the physical address of memory, which LinuxKPI asks before it hands
 * memory to a device. Here the answer is the radio's address, for memory of the pool; no other
 * memory has one (0, which lkpi_dma_map_phys() below then refuses).
 */
vm_paddr_t
pmap_kextract(vm_offset_t va)
{
	return (in_pool((void *)va, 1) ? pool.address + ((uint8_t *)va - pool.memory) : 0);
}

/* A buffer handed to the radio: it must be the pool's. */
dma_addr_t
lkpi_dma_map_phys(struct device *dev, vm_paddr_t address, size_t length, enum dma_data_direction direction, unsigned long attributes)
{
	if (address < pool.address || address + length > pool.address + POOL_SIZE) {
		printf("lx: memory is given to the radio that it cannot reach (%zu bytes)\n", length);
		return (DMA_MAPPING_ERROR);
	}
	return (address);
}

void
lkpi_dma_unmap(struct device *dev, dma_addr_t address, size_t length, enum dma_data_direction direction, unsigned long attributes)
{
}

void
linuxkpi_dma_sync(struct device *dev, dma_addr_t address, size_t length, bus_dmasync_op_t op)
{
}

/* ---- socket buffers --------------------------------------------------------------------------- */

/* The buffer's description and what LinuxKPI keeps behind it come from kmalloc(); its data is a slot of the pool. */
struct sk_buff *
linuxkpi_dev_alloc_skb(size_t size, gfp_t gfp)
{
	size_t all = roundup2(sizeof(struct sk_buff), 64) + sizeof(struct skb_shared_info) + 64;
	struct sk_buff *skb;
	uint8_t *raw;
	uint32_t slot;

	if (size + NET_SKB_PAD > POOL_SLOT) {
		printf("lx: a buffer of %zu bytes is asked for; the slots are %u\n", size, POOL_SLOT);
		return (NULL);
	}
	raw = lkpi_kmalloc(all, gfp);
	if (raw == NULL) {
		return (NULL);
	}
	lx_os_lock();
	if (pool.free_count == 0) {
		lx_os_unlock();
		lkpi_kfree(raw);
		printf("lx: no buffer left\n");
		return (NULL);
	}
	slot = pool.free_slots[--pool.free_count];
	lx_os_unlock();
	skb = (struct sk_buff *)roundup2((uintptr_t)raw, 64);
	skb->m = raw;				/* (the field FreeBSD keeps for what is behind a buffer) */
	skb->prev = skb->next = skb;
	skb->shinfo = (struct skb_shared_info *)((uint8_t *)skb + roundup2(sizeof(struct sk_buff), 64));
	skb->truesize = size + NET_SKB_PAD;
	skb->head = skb->data = pool.memory + POOL_RINGS + (size_t)slot * POOL_SLOT;
	skb_reset_tail_pointer(skb);
	skb->end = skb->head + size + NET_SKB_PAD;
	skb_reserve(skb, NET_SKB_PAD);
	return (skb);
}

void
linuxkpi_kfree_skb(struct sk_buff *skb)
{
	if (skb == NULL) {
		return;
	}
	if (in_pool(skb->head, 1) && skb->head >= pool.memory + POOL_RINGS) {
		lx_os_lock();
		pool.free_slots[pool.free_count++] = (uint32_t)((skb->head - (pool.memory + POOL_RINGS)) / POOL_SLOT);
		lx_os_unlock();
	}
	lkpi_kfree(skb->m);
}

/* ---- firmware files ---------------------------------------------------------------------------- */

int
linuxkpi_firmware_request_nowarn(const struct linuxkpi_firmware **out, const char *name, struct device *dev)
{
	struct linuxkpi_firmware *fw;
	const void *data = NULL;
	unsigned long size = 0;
	int error = lx_os_firmware(name, &data, &size);

	printf("lx: the driver asks for the file \"%s\": %s\n", name, error == 0 ? "there" : "not there");
	if (error != 0) {
		return (-ENOENT);
	}
	fw = lkpi_kmalloc(sizeof(*fw), GFP_KERNEL);
	if (fw == NULL) {
		return (-ENOMEM);
	}
	fw->size = size;
	fw->data = data;
	*out = fw;
	return (0);
}

void
linuxkpi_release_firmware(const struct linuxkpi_firmware *fw)
{
	lkpi_kfree(fw);
}

/* ---- what the files built so far name and do not get to --------------------------------------------- */

/*
 * Numbered things (Linux's "idr": the driver numbers the frames it has handed to the firmware
 * and finds them again by number). Here a table that grows, hung where FreeBSD's keeps its tree.
 */
struct lx_ids {
	int size;
	void *things[];
};

void
idr_init(struct idr *idr)
{
	bzero(idr, sizeof(*idr));
}

void
idr_destroy(struct idr *idr)
{
	struct lx_ids *ids = (struct lx_ids *)idr->top;

	if (ids != NULL) {
		lx_os_free(ids, sizeof(*ids) + ids->size * sizeof(void *));
	}
	idr->top = NULL;
}

/* The lowest free number from `start` up to but not `end` (0 or less: no limit), or -ENOSPC. */
int
idr_alloc(struct idr *idr, void *thing, int start, int end, gfp_t gfp)
{
	struct lx_ids *ids, *more;
	int id, size;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	for (id = start < 0 ? 0 : start; ids != NULL && id < ids->size && ids->things[id] != NULL; id++) {
	}
	if (end > 0 && id >= end) {
		lx_os_unlock();
		return (-ENOSPC);
	}
	if (ids == NULL || id >= ids->size) {
		size = id + 64;
		lx_os_unlock();
		more = lx_os_alloc(sizeof(*more) + size * sizeof(void *));
		if (more == NULL) {
			return (-ENOMEM);
		}
		lx_os_lock();
		ids = (struct lx_ids *)idr->top;
		if (ids != NULL && ids->size >= size) {		/* (somebody else has made it grow meanwhile) */
			lx_os_unlock();
			lx_os_free(more, sizeof(*more) + size * sizeof(void *));
			return (idr_alloc(idr, thing, start, end, gfp));
		}
		more->size = size;
		if (ids != NULL) {
			memcpy(more->things, ids->things, ids->size * sizeof(void *));
		}
		idr->top = (struct idr_layer *)more;
		lx_os_unlock();
		if (ids != NULL) {
			lx_os_free(ids, sizeof(*ids) + ids->size * sizeof(void *));
		}
		return (idr_alloc(idr, thing, start, end, gfp));
	}
	ids->things[id] = thing;
	lx_os_unlock();
	return (id);
}

void *
idr_find(struct idr *idr, int id)
{
	struct lx_ids *ids;
	void *thing;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	thing = ids != NULL && id >= 0 && id < ids->size ? ids->things[id] : NULL;
	lx_os_unlock();
	return (thing);
}

void *
idr_remove(struct idr *idr, int id)
{
	struct lx_ids *ids;
	void *thing = NULL;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	if (ids != NULL && id >= 0 && id < ids->size) {
		thing = ids->things[id];
		ids->things[id] = NULL;
	}
	lx_os_unlock();
	return (thing);
}

int
idr_for_each(struct idr *idr, int (*function)(int id, void *p, void *data), void *data)
{
	int answer = 0;

	for (int id = 0; answer == 0; id++) {
		struct lx_ids *ids;
		void *thing;

		lx_os_lock();
		ids = (struct lx_ids *)idr->top;
		if (ids == NULL || id >= ids->size) {
			lx_os_unlock();
			break;
		}
		thing = ids->things[id];
		lx_os_unlock();
		if (thing != NULL) {
			answer = function(id, thing, data);
		}
	}
	return (answer);
}

/*
 * Things found by a key inside them (linux/rhashtable.h, ours). The chains are changed and walked
 * under the layer's lock; that a thing found stays there while it is used is the caller's
 * business, as it is under Linux.
 */
static struct rhash_head **
rht_chain(struct rhashtable *table, const void *key, unsigned length)
{
	uint32_t hash = 2166136261u;

	for (unsigned i = 0; i < length; i++) {
		hash = (hash ^ ((const uint8_t *)key)[i]) * 16777619u;
	}
	return (&table->chains[hash % nitems(table->chains)]);
}

int
rhashtable_init(struct rhashtable *table, const struct rhashtable_params *params)
{
	bzero(table, sizeof(*table));
	return (0);
}

void
rhashtable_destroy(struct rhashtable *table)
{
}

static void *
rht_find(struct rhashtable *table, const void *key, const struct rhashtable_params *params)
{
	for (struct rhash_head *head = *rht_chain(table, key, params->key_len); head != NULL; head = head->next) {
		uint8_t *thing = (uint8_t *)head - params->head_offset;

		if (memcmp(thing + params->key_offset, key, params->key_len) == 0) {
			return (thing);
		}
	}
	return (NULL);
}

void *
rhashtable_lookup_fast(struct rhashtable *table, const void *key, const struct rhashtable_params params)
{
	void *thing;

	lx_os_lock();
	thing = rht_find(table, key, &params);
	lx_os_unlock();
	return (thing);
}

void *
rhashtable_lookup_get_insert_fast(struct rhashtable *table, struct rhash_head *head, const struct rhashtable_params params)
{
	const uint8_t *key = (uint8_t *)head - params.head_offset + params.key_offset;
	struct rhash_head **chain;
	void *there;

	lx_os_lock();
	there = rht_find(table, key, &params);
	if (there == NULL) {
		chain = rht_chain(table, key, params.key_len);
		head->next = *chain;
		*chain = head;
	}
	lx_os_unlock();
	return (there);
}

int
rhashtable_remove_fast(struct rhashtable *table, struct rhash_head *head, const struct rhashtable_params params)
{
	const uint8_t *key = (uint8_t *)head - params.head_offset + params.key_offset;
	int error = -ENOENT;

	lx_os_lock();
	for (struct rhash_head **link = rht_chain(table, key, params.key_len); *link != NULL; link = &(*link)->next) {
		if (*link == head) {
			*link = head->next;
			error = 0;
			break;
		}
	}
	lx_os_unlock();
	return (error);
}

/* The numbered thing with the lowest number from *id on, and that number; or NULL. */
void *
idr_get_next(struct idr *idr, int *id)
{
	struct lx_ids *ids;
	void *thing = NULL;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	for (int i = *id < 0 ? 0 : *id; ids != NULL && i < ids->size; i++) {
		if (ids->things[i] != NULL) {
			thing = ids->things[i];
			*id = i;
			break;
		}
	}
	lx_os_unlock();
	return (thing);
}

void
explicit_bzero(void *memory, size_t size)
{
	bzero(memory, size);
}

/* FreeBSD's kernel log, which LinuxKPI's rate-limited messages go to. */
void
log(int level, const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	lx_os_print(line);
}

void
arc4random_buf(void *buffer, size_t size)
{
	lx_os_random(buffer, size);
}

const struct dmi_system_id *
linux_dmi_first_match(const struct dmi_system_id *list)
{
	return (NULL);				/* (no table of the machine's maker here) */
}

/* What the layer holds of the kernel's memory right now: a number that keeps growing is a leak. */
void
lx_memory_say(void)
{
	lx_tell("lx: memory: %ld pieces of the kernel's out, %ld KB; of the radio's %u MB: %u KB of rings, %u of %u buffers out\n",
	    allocations, allocated >> 10, POOL_SIZE >> 20, pool.rings_used >> 10, POOL_SLOTS - pool.free_count, POOL_SLOTS);
}

int
lx_base_init(void)
{
	static bool begun;

	if (begun) {
		return (0);
	}
	begun = true;
	return (lx_os_thread("wifi-ticks", ticker, NULL));
}
