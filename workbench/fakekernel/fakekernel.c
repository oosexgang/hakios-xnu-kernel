/*
 * The fake kernel of the driver tests (fakekernel.h): pthreads for threads, condition variables for waits,
 * the host's monotonic clock, a table of sysctls, boot-args and the board's name set by the test.
 */
#include <pthread.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>

#include "fakekernel.h"

/* ---- the fake kernel ---------------------------------------------------------------------------- */

const char *boot_args = "";
const char *board = "";
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t woken;
static uint64_t wake_generation;
static __thread void *wait_event;
static __thread uint64_t wait_until_ns, wait_generation;
char log_text[1 << 16];
static size_t log_length;

/* the waits' deadlines are by the monotonic clock, as mach_absolute_time() below */
__attribute__((constructor)) static void
fake_kernel_init(void)
{
	pthread_condattr_t attributes;

	pthread_condattr_init(&attributes);
	pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	pthread_cond_init(&woken, &attributes);
}

uint64_t
mach_absolute_time(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * NSEC_PER_SEC + (uint64_t)t.tv_nsec;
}

/* (absolute time is nanoseconds here) */
void
clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
	*deadline = mach_absolute_time() + (uint64_t)interval * scale;
}

void
absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result)
{
	*result = abstime;
}

void
delay(int usec)
{
	usleep((useconds_t)(usec > 0 ? usec : 0));
}

void (*fake_before_wait)(void);

void
assert_wait(void *event, int interruptible __unused)
{
	if (fake_before_wait != NULL) {
		void (*now)(void) = fake_before_wait;

		fake_before_wait = NULL;        /* (once) */
		now();
	}
	pthread_mutex_lock(&big);
	wait_event = event;
	wait_until_ns = 0;
	wait_generation = wake_generation;
	pthread_mutex_unlock(&big);
}

void
assert_wait_timeout(void *event, int interruptible __unused, uint32_t interval, uint64_t scale)
{
	pthread_mutex_lock(&big);
	wait_event = event;
	wait_until_ns = mach_absolute_time() + interval * scale;
	wait_generation = wake_generation;
	pthread_mutex_unlock(&big);
}

wait_result_t
thread_block(thread_continue_t continuation __unused)
{
	pthread_mutex_lock(&big);
	while (wake_generation == wait_generation) {
		if (wait_until_ns != 0) {
			struct timespec t = { (time_t)(wait_until_ns / NSEC_PER_SEC), (long)(wait_until_ns % NSEC_PER_SEC) };

			if (mach_absolute_time() >= wait_until_ns) {
				break;
			}
			pthread_cond_timedwait(&woken, &big, &t);
		} else {
			pthread_cond_wait(&woken, &big);
		}
	}
	wait_event = NULL;
	pthread_mutex_unlock(&big);
	return 0;
}

/* (a wait is only declared until thread_block(): taking it back is to do nothing) */
kern_return_t
clear_wait(thread_t thread __unused, wait_result_t result __unused)
{
	return KERN_SUCCESS;
}

void
thread_wakeup(void *event __unused)
{
	pthread_mutex_lock(&big);
	wake_generation++;
	pthread_cond_broadcast(&woken);
	pthread_mutex_unlock(&big);
}

struct start { thread_continue_t fn; void *arg; };

/* where thread_terminate() takes a driver's thread: back to its start, which returns it to pthreads. Not
 * pthread_exit(): that unwinds the stack, and an unwind at the moment the test's main thread called exit()
 * aborted the program now and then (qemu-aarch64, a loaded machine: torch_driver_test battery_hot) */
static __thread jmp_buf *thread_start;

static void *
thread_main(void *p)
{
	struct start s = *(struct start *)p;
	jmp_buf start;

	free(p);
	thread_start = &start;
	if (setjmp(start) == 0) {
		s.fn(s.arg, 0);
	}
	return NULL;
}

kern_return_t
kernel_thread_start(thread_continue_t fn, void *arg, thread_t *thread)
{
	pthread_t t;
	struct start *s = malloc(sizeof(*s));

	s->fn = fn;
	s->arg = arg;
	*thread = (thread_t)(uintptr_t)0x7e57;  /* (a reference the caller is given, and must let go of) */
	if (pthread_create(&t, NULL, thread_main, s) != 0) {
		*thread = NULL;
		return KERN_FAILURE;
	}
	__atomic_fetch_add(&fake_threads_started, 1, __ATOMIC_RELAXED);
	return KERN_SUCCESS;
}

/* threads a driver started, and the references to them it let go of: the two counts should end equal */
unsigned fake_threads_started, fake_threads_released;

void
thread_deallocate(thread_t thread)
{
	if (thread != NULL) {
		__atomic_fetch_add(&fake_threads_released, 1, __ATOMIC_RELAXED);
	}
}
void thread_set_thread_name(thread_t thread __unused, const char *name __unused) {}

/* ---- devices: the nodes drivers make, by name ---- */

/*
 * A driver adds its switch and gets a major number (character devices 0 to 7, block devices 8 to 11), then
 * makes nodes of it, one for each minor number. A test reaches a node by its name, as a program would. A
 * switch taken back (cdevsw_remove(), bdevsw_remove()) leaves its major free for the next to be added, and
 * its nodes reach nothing.
 */
#define CHAR_MAJORS             8
#define BLOCK_MAJORS            4
#define NODES                   512
static const struct cdevsw *added[CHAR_MAJORS];
static unsigned added_n;
static const struct bdevsw *disks[BLOCK_MAJORS];
static unsigned disks_n;
static struct { dev_t dev; char name[48]; } nodes[NODES];
static unsigned nodes_n;

int
cdevsw_add(int index __unused, const struct cdevsw *devsw)
{
	for (unsigned i = 0; i < added_n; i++) {
		if (added[i] == NULL) {
			added[i] = devsw;
			return (int)i;
		}
	}
	if (added_n >= CHAR_MAJORS) {
		return -1;
	}
	added[added_n] = devsw;
	return (int)added_n++;
}

int
bdevsw_add(int index __unused, const struct bdevsw *devsw)
{
	for (unsigned i = 0; i < disks_n; i++) {
		if (disks[i] == NULL) {
			disks[i] = devsw;
			return CHAR_MAJORS + (int)i;
		}
	}
	if (disks_n >= BLOCK_MAJORS) {
		return -1;
	}
	disks[disks_n] = devsw;
	return CHAR_MAJORS + (int)disks_n++;
}

/* (as the kernel's: the major back, if that switch is the one there; -1 if not) */
int
cdevsw_remove(int index, const struct cdevsw *devsw)
{
	if (index < 0 || (unsigned)index >= added_n || added[index] != devsw) {
		return -1;
	}
	added[index] = NULL;
	return index;
}

int
bdevsw_remove(int index, const struct bdevsw *devsw)
{
	if (index < CHAR_MAJORS || (unsigned)(index - CHAR_MAJORS) >= disks_n || disks[index - CHAR_MAJORS] != devsw) {
		return -1;
	}
	disks[index - CHAR_MAJORS] = NULL;
	return index;
}

int
cdevsw_add_with_bdev(int index, const struct cdevsw *devsw, int bdev __unused)
{
	return cdevsw_add(index, devsw);
}

/* (the name is a format with its arguments, as devfs takes it; a name that is there already is refused) */
void *
devfs_make_node(dev_t dev, int type __unused, int uid __unused, int gid __unused, int perms __unused, const char *name, ...)
{
	unsigned index = major(dev);
	char text[48];
	va_list arguments;

	va_start(arguments, name);
	vsnprintf(text, sizeof(text), name, arguments);
	va_end(arguments);
	if (nodes_n >= NODES || (index < CHAR_MAJORS ? index >= added_n || added[index] == NULL :
	    index - CHAR_MAJORS >= disks_n || disks[index - CHAR_MAJORS] == NULL)) {
		return NULL;
	}
	for (unsigned i = 0; i < nodes_n; i++) {
		if (strcmp(nodes[i].name, text) == 0) {
			return NULL;
		}
	}
	nodes[nodes_n].dev = dev;
	snprintf(nodes[nodes_n].name, sizeof(nodes[nodes_n].name), "%s", text);
	return &nodes[nodes_n++];
}

void
devfs_remove(void *handle)
{
	if (handle != NULL) {
		((__typeof__(nodes[0]) *)handle)->name[0] = '\0';
	}
}

/* the node of that name and of that kind, of a switch that is still there: its number, or false */
static bool
node_named(const char *name, bool disk, dev_t *dev)
{
	for (unsigned i = 0; i < nodes_n; i++) {
		if (strcmp(nodes[i].name, name) == 0 && (major(nodes[i].dev) >= CHAR_MAJORS) == disk) {
			if (disk ? disks[major(nodes[i].dev) - CHAR_MAJORS] == NULL : added[major(nodes[i].dev)] == NULL) {
				return false;
			}
			*dev = nodes[i].dev;
			return true;
		}
	}
	return false;
}

/* ---- block devices ---- */

static int
disk_request(const char *name, int flags, uint64_t sector, void *data, size_t size, size_t *resid)
{
	struct buf request = { .flags = flags, .blkno = (int64_t)sector, .count = (uint32_t)size, .resid = (uint32_t)size, .data = data };
	const struct bdevsw *devsw;

	if (!node_named(name, true, &request.dev)) {
		return -1;
	}
	devsw = disks[major(request.dev) - CHAR_MAJORS];
	if (devsw->d_strategy == NULL) {
		return -1;
	}
	devsw->d_strategy(&request);
	if (resid != NULL) {
		*resid = request.resid;
	}
	return request.done ? request.error : -2;
}

int
fake_disk_read(const char *name, uint64_t sector, void *data, size_t size, size_t *resid)
{
	return disk_request(name, B_READ, sector, data, size, resid);
}

int
fake_disk_read_async(const char *name, uint64_t sector, void *data, size_t size, struct buf *request)
{
	const struct bdevsw *devsw;

	*request = (struct buf){ .flags = B_READ | B_ASYNC, .blkno = (int64_t)sector, .count = (uint32_t)size, .resid = (uint32_t)size,
		                 .data = data };
	if (!node_named(name, true, &request->dev)) {
		return -1;
	}
	devsw = disks[major(request->dev) - CHAR_MAJORS];
	if (devsw->d_strategy == NULL) {
		return -1;
	}
	devsw->d_strategy(request);
	return 0;
}

int
fake_disk_write(const char *name, uint64_t sector, const void *data, size_t size, size_t *resid)
{
	return disk_request(name, 0, sector, __DEVOLATILE(void *, data), size, resid);
}

int
fake_disk_open(const char *name, int flags)
{
	dev_t dev;

	if (!node_named(name, true, &dev) || disks[major(dev) - CHAR_MAJORS]->d_open == NULL) {
		return -1;
	}
	return disks[major(dev) - CHAR_MAJORS]->d_open(dev, flags, 0, NULL);
}

int
fake_disk_ioctl(const char *name, unsigned long cmd, void *data)
{
	dev_t dev;

	if (!node_named(name, true, &dev) || disks[major(dev) - CHAR_MAJORS]->d_ioctl == NULL) {
		return -1;
	}
	return disks[major(dev) - CHAR_MAJORS]->d_ioctl(dev, cmd, data, 0, NULL);
}

/* ---- character devices ---- */

user_ssize_t uio_resid(struct uio *uio) { return (user_ssize_t)uio->resid; }
off_t uio_offset(struct uio *uio) { return uio->offset; }
void uio_setoffset(struct uio *uio, off_t offset) { uio->offset = offset; }

/* (what was not moved after all is the program's again: the bytes are where they were) */
void
uio_setresid(struct uio *uio, user_ssize_t resid)
{
	uio->base -= (size_t)resid - uio->resid;
	uio->resid = (size_t)resid;
}

/* uiomove()s made so far; the one numbered fake_uiomove_fault_at (from 1) meets a bad user address */
unsigned fake_uiomove_calls, fake_uiomove_fault_at;

int
uiomove(caddr_t kernel, int n, struct uio *uio)
{
	size_t count = (size_t)n < uio->resid ? (size_t)n : uio->resid;

	if (++fake_uiomove_calls == fake_uiomove_fault_at) {
		return EFAULT;
	}

	if (uio->to_kernel) {
		memcpy(kernel, uio->base, count);
	} else {
		memcpy(uio->base, kernel, count);
	}
	uio->base += count;
	uio->resid -= count;
	uio->offset += (off_t)count;
	return 0;
}

/* a caller's buffer is a host pointer; none at all is a fault, as in the kernel */
size_t fake_copied_out;

int
copyin(user_addr_t from, void *to, size_t length)
{
	if (from == USER_ADDR_NULL) {
		return EFAULT;
	}
	memcpy(to, (const void *)(uintptr_t)from, length);
	return 0;
}

int
copyout(const void *from, user_addr_t to, size_t length)
{
	if (to == USER_ADDR_NULL) {
		return EFAULT;
	}
	memcpy((void *)(uintptr_t)to, from, length);
	fake_copied_out = length;
	return 0;
}

void
proc_selfname(char *buffer, int size)
{
	snprintf(buffer, (size_t)size, "wifid");
}

int
memset_s(void *to, size_t room, int value, size_t length)
{
	memset(to, value, length < room ? length : room);
	return 0;
}

struct kev_msg fake_last_event;
unsigned fake_events;
uint8_t fake_last_event_data[512];
uint32_t fake_last_event_length, fake_event_codes[16];

int
kev_post_msg(struct kev_msg *message)
{
	fake_last_event = *message;
	fake_event_codes[fake_events % 16] = message->event_code;
	fake_events++;
	fake_last_event_length = 0;
	for (unsigned i = 0; i < 5 && message->dv[i].data_length != 0; i++) {
		uint32_t n = message->dv[i].data_length;

		if (n > sizeof(fake_last_event_data) - fake_last_event_length) {
			n = (uint32_t)(sizeof(fake_last_event_data) - fake_last_event_length);
		}
		memcpy(fake_last_event_data + fake_last_event_length, message->dv[i].data_ptr, n);
		fake_last_event_length += n;
	}
	return 0;
}

int
fake_dev_pwrite(const char *name, const void *data, size_t size, off_t offset, size_t *done)
{
	struct uio uio = { __DEVOLATILE(char *, data), size, true, offset };
	dev_t dev;
	int error;

	if (!node_named(name, false, &dev) || added[major(dev)]->d_write == NULL) {
		return -1;
	}
	error = added[major(dev)]->d_write(dev, &uio, 0);
	if (done != NULL) {
		*done = size - uio.resid;
	}
	return error;
}

int
fake_dev_pread(const char *name, void *data, size_t size, off_t offset, size_t *done)
{
	struct uio uio = { data, size, false, offset };
	dev_t dev;
	int error;

	if (!node_named(name, false, &dev) || added[major(dev)]->d_read == NULL) {
		return -1;
	}
	error = added[major(dev)]->d_read(dev, &uio, 0);
	if (done != NULL) {
		*done = size - uio.resid;
	}
	return error;
}

int fake_dev_write(const char *name, const void *data, size_t size) { return fake_dev_pwrite(name, data, size, 0, NULL); }
int fake_dev_read(const char *name, void *data, size_t size) { return fake_dev_pread(name, data, size, 0, NULL); }

int
fake_dev_open(const char *name, int flags)
{
	dev_t dev;

	if (!node_named(name, false, &dev) || added[major(dev)]->d_open == NULL) {
		return -1;
	}
	return added[major(dev)]->d_open(dev, flags, 0, NULL);
}

int
fake_dev_ioctl(const char *name, unsigned long cmd, void *data)
{
	int (*ioctl)(dev_t, unsigned long, caddr_t, int, struct proc *);
	dev_t dev;

	if (!node_named(name, false, &dev) || added[major(dev)]->d_ioctl == NULL) {
		return -1;
	}
	ioctl = (int (*)(dev_t, unsigned long, caddr_t, int, struct proc *))added[major(dev)]->d_ioctl;
	return ioctl(dev, cmd, (caddr_t)data, 0, NULL);
}

int
fake_dev_close(const char *name)
{
	dev_t dev;

	if (!node_named(name, false, &dev) || added[major(dev)]->d_close == NULL) {
		return -1;
	}
	return added[major(dev)]->d_close(dev, 0, 0, NULL);
}

/* ---- processes, sleeps on a channel, and what else of the kernel a storage driver uses ---- */

int fake_pid = 100, fake_other_pid;

int proc_selfpid(void) { return fake_pid; }
int proc_pid(proc_t p __unused) { return fake_pid; }
/* (pid 0 is the kernel's own process, as XNU's proc_find() finds it: alive always -- so 0 is no "nobody" to look up) */
struct proc *proc_find(int pid) { return pid == 0 || pid == fake_pid || pid == fake_other_pid ? (struct proc *)&fake_pid : NULL; }
void proc_rele(struct proc *p __unused) {}

int fake_uid;

kauth_cred_t kauth_cred_get(void) { return (kauth_cred_t)&fake_uid; }
int suser(kauth_cred_t cred, unsigned short *acflag __unused) { return *(const int *)cred == 0 ? 0 : EPERM; }

int
msleep(void *channel __unused, struct fake_lock *lock, int priority __unused, const char *why __unused, void *time __unused)
{
	if (lock != NULL) {
		pthread_mutex_unlock(&lock->mutex);
	}
	usleep(200);
	if (lock != NULL) {
		pthread_mutex_lock(&lock->mutex);
	}
	return 0;
}

void wakeup(void *channel __unused) {}
void wakeup_one(void *channel __unused) {}

wait_result_t
lck_mtx_sleep(struct fake_lock *lock, int flags __unused, event_t event, int interruptible __unused)
{
	msleep(event, lock, 0, NULL, NULL);
	return THREAD_AWAKENED;
}

wait_result_t
lck_mtx_sleep_deadline(struct fake_lock *lock, int flags __unused, event_t event, int interruptible __unused,
    uint64_t deadline __unused)
{
	msleep(event, lock, 0, NULL, NULL);
	return THREAD_TIMED_OUT;
}

/* the cores the scheduler is told to prefer (kern/sched_prim.c), as the CPU frequency driver parks the small ones */
uint32_t fake_recommended_cores = ~0u;
void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores) { fake_recommended_cores = recommended_cores; }

/*
 * How long a core has been idle (the kernel's own counter, patches/0002): not kept unless a test says
 * (fake_idle_kept), and then each core is fake_busy_pct[] busy, its idle time growing by the rest of each
 * moment since it was last asked.
 */
bool fake_idle_kept;
volatile unsigned fake_busy_pct[MAX_CPUS];

bool
ml_cpu_idle_time(int cpu_id, uint64_t *idle)
{
	static uint64_t idle_of[MAX_CPUS], asked_at[MAX_CPUS];
	uint64_t now = mach_absolute_time();

	if (!fake_idle_kept || cpu_id < 0 || cpu_id >= MAX_CPUS) {
		return false;
	}
	if (asked_at[cpu_id] != 0) {
		idle_of[cpu_id] += (now - asked_at[cpu_id]) * (100 - (fake_busy_pct[cpu_id] > 100 ? 100 : fake_busy_pct[cpu_id])) / 100;
	}
	asked_at[cpu_id] = now;
	*idle = idle_of[cpu_id];
	return true;
}

void
IOSleep(unsigned int milliseconds)
{
	usleep(milliseconds * 1000 / fake_delay_divisor);
}

uint32_t
crc32(uint32_t crc, const void *data, size_t size)
{
	const uint8_t *byte = data;

	crc = ~crc;
	while (size-- > 0) {
		crc ^= *byte++;
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

/* ---- calls made later ---- */

struct fake_thread_call {
	thread_call_func_t func;
	thread_call_param_t p0;
	uint64_t deadline;
	unsigned generation;            /* entered again or cancelled: a waiting one runs only if it is still this */
	pthread_mutex_t lock;
};

thread_call_t
thread_call_allocate(thread_call_func_t func, thread_call_param_t p0)
{
	thread_call_t call = calloc(1, sizeof(*call));

	call->func = func;
	call->p0 = p0;
	pthread_mutex_init(&call->lock, NULL);
	return call;
}

struct fake_call_wait {
	thread_call_t call;
	unsigned generation;
};

static void *
thread_call_main(void *p)
{
	struct fake_call_wait wait = *(struct fake_call_wait *)p;
	bool still;

	free(p);
	for (;;) {
		uint64_t now = mach_absolute_time();

		pthread_mutex_lock(&wait.call->lock);
		still = wait.call->generation == wait.generation;
		pthread_mutex_unlock(&wait.call->lock);
		if (!still || now >= wait.call->deadline) {
			break;
		}
		usleep(200);
	}
	if (still) {
		wait.call->func(wait.call->p0, NULL);
	}
	return NULL;
}

bool
thread_call_enter_delayed(thread_call_t call, uint64_t deadline)
{
	struct fake_call_wait *wait = malloc(sizeof(*wait));
	uint64_t now = mach_absolute_time();
	pthread_t thread;

	pthread_mutex_lock(&call->lock);
	call->generation++;
	call->deadline = now + (deadline > now ? (deadline - now) / fake_delay_divisor : 0);
	wait->call = call;
	wait->generation = call->generation;
	pthread_mutex_unlock(&call->lock);
	pthread_create(&thread, NULL, thread_call_main, wait);
	pthread_detach(thread);
	return false;
}

bool
thread_call_cancel(thread_call_t call)
{
	pthread_mutex_lock(&call->lock);
	call->generation++;
	pthread_mutex_unlock(&call->lock);
	return true;
}

void *kernel_map;

int
kmem_alloc_contig(void *map __unused, vm_offset_t *address, vm_size_t size, vm_offset_t mask, int max_pnum __unused,
    int pnum_mask __unused, int flags __unused, int tag __unused)
{
	void *memory = NULL;

	if (posix_memalign(&memory, mask + 1 > sizeof(void *) ? mask + 1 : sizeof(void *), size) != 0) {
		return KERN_FAILURE;
	}
	memset(memory, 0, size);
	*address = (vm_offset_t)memory;
	return KERN_SUCCESS;
}

/* (weak: a test that counts or refuses the allocations has its own) */
__attribute__((weak)) int
kmem_alloc(void *map, vm_offset_t *address, vm_size_t size, int flags, int tag)
{
	return kmem_alloc_contig(map, address, size, PAGE_MASK, 0, 0, flags, tag);
}

__attribute__((weak)) void kmem_free(void *map __unused, vm_offset_t address, vm_size_t size __unused) { free((void *)address); }

thread_t current_thread(void) { return NULL; }

kern_return_t
thread_terminate(thread_t thread __unused)
{
	if (thread_start != NULL) {
		longjmp(*thread_start, 1);
	}
	pthread_exit(NULL);             /* (a thread not started by kernel_thread_start) */
}

void
nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result)
{
	*result = nanoseconds;          /* (the fake clock counts nanoseconds) */
}

unsigned fake_delay_divisor = 1;

/* ---- the cores ---------------------------------------------------------------------------------- */

unsigned fake_cores = 1, fake_core, fake_small_cores;

const ml_topology_info_t *
ml_get_topology_info(void)
{
	static ml_topology_info_t topology;

	topology.num_cpus = fake_cores > MAX_CPUS ? MAX_CPUS : fake_cores;
	for (unsigned i = 0; i < topology.num_cpus; i++) {
		topology.cpus[i].phys_id = i << 8;
		topology.cpus[i].l2_access_penalty = (fake_small_cores >> i) & 1;
	}
	return &topology;
}

void
delay_for_interval(uint32_t interval, uint32_t scale)
{
	uint64_t nanoseconds = (uint64_t)interval * scale / (fake_delay_divisor ? fake_delay_divisor : 1);
	struct timespec t = { (time_t)(nanoseconds / 1000000000ull), (long)(nanoseconds % 1000000000ull) };

	nanosleep(&t, NULL);
}

void
clock_get_calendar_microtime(clock_sec_t *seconds, clock_usec_t *micro)
{
	struct timespec t;

	clock_gettime(CLOCK_REALTIME, &t);
	*seconds = (clock_sec_t)t.tv_sec;
	*micro = (clock_usec_t)(t.tv_nsec / 1000);
}
bool ml_get_interrupts_enabled(void) { return false; }      /* (everything to the loader's log only) */

/* a line at once, as the kernel's loader_log_line does it under its lock */
void
loader_log_line(const char *line)
{
	pthread_mutex_lock(&big);
	for (const char *c = line; *c != 0; c++) {
		if (log_length + 1 < sizeof(log_text)) {
			log_text[log_length++] = *c;
			log_text[log_length] = 0;
		}
	}
	pthread_mutex_unlock(&big);
	if (getenv("BL_VERBOSE")) {
		fputs(line, stderr);
	}
}

void
loader_log_putc(char c)
{
	pthread_mutex_lock(&big);
	if (log_length + 1 < sizeof(log_text)) {
		log_text[log_length++] = c;
		log_text[log_length] = 0;
	}
	pthread_mutex_unlock(&big);
	if (getenv("BL_VERBOSE")) {
		fputc(c, stderr);
	}
}

bool
logged(const char *text)
{
	bool found;

	pthread_mutex_lock(&big);
	found = strstr(log_text, text) != NULL;
	pthread_mutex_unlock(&big);
	return found;
}

/* The device tree as the loader leaves it: /chosen with darwin-board, the test's `board` with its end.
 * A test that sets `board` to NULL has a loader that named none. */
int
SecureDTLookupEntry(const DTEntry search, const char *path, DTEntry *entry)
{
	(void)search;
	if (strcmp(path, "/chosen") != 0) {
		return kError;
	}
	*entry = (DTEntry)&board;
	return kSuccess;
}

int
SecureDTGetProperty(const DTEntry entry, const char *name, void const **value, unsigned int *size)
{
	if (entry != (DTEntry)&board || strcmp(name, "darwin-board") != 0 || board == NULL) {
		return kError;
	}
	*value = board;
	*size = (unsigned int)strlen(board) + 1;
	return kSuccess;
}

/* "name=value" among the boot-args: a number (decimal or 0x hex) or a string */
bool
PE_parse_boot_argn(const char *name, void *value, int size)
{
	size_t n = strlen(name);

	for (const char *a = boot_args; *a != 0;) {
		const char *end = strchr(a, ' ');
		size_t length = end ? (size_t)(end - a) : strlen(a);

		if (length > n && strncmp(a, name, n) == 0 && a[n] == '=') {
			char text[64];
			size_t vlen = length - n - 1;

			memcpy(text, a + n + 1, vlen < sizeof(text) - 1 ? vlen : sizeof(text) - 1);
			text[vlen < sizeof(text) - 1 ? vlen : sizeof(text) - 1] = 0;
			if ((text[0] >= '0' && text[0] <= '9') || (text[0] == '-' && text[1] >= '0' && text[1] <= '9')) {
				uint64_t number = strtoull(text, NULL, 0);

				memcpy(value, &number, (size_t)size < sizeof(number) ? (size_t)size : sizeof(number));
			} else {
				strncpy(value, text, (size_t)size);
				((char *)value)[size - 1] = 0;
			}
			return true;
		}
		a += length;
		while (*a == ' ') {
			a++;
		}
	}
	return false;
}

/* (the kernel's own: a string argument, as PE_parse_boot_argn() takes one) */
bool
PE_parse_boot_arg_str(const char *name, char *value, int size)
{
	return PE_parse_boot_argn(name, value, size);
}

/* sysctl: a table by name ("_hw_backlight.level") */
static struct sysctl_oid *oids[64];
static int oid_count;

void
fake_sysctl_register(struct sysctl_oid *oid)
{
	oids[oid_count++] = oid;
}

static struct sysctl_oid *
oid_named(const char *name)
{
	for (int i = 0; i < oid_count; i++) {
		const char *dot = strrchr(oids[i]->name, '.');

		if (dot != NULL && strcmp(dot + 1, name) == 0) {
			return oids[i];
		}
	}
	fprintf(stderr, "no sysctl %s\n", name);
	exit(2);
}

int
sysctl_io_number(struct sysctl_req *req, long long value, size_t size, void *pointer, int *changed)
{
	if (req->oldptr != NULL) {
		memcpy(req->oldptr, &value, size);      /* (little-endian host: the low bytes) */
		req->oldidx = size;
	}
	if (req->newptr != NULL) {
		memcpy(pointer, req->newptr, size);
		if (changed != NULL) {
			*changed = 1;
		}
	}
	return 0;
}

int
fake_sysctl_out(struct sysctl_req *req, const void *data, size_t size)
{
	if (req->oldptr != NULL && size <= req->oldlen) {
		memcpy(req->oldptr, data, size);
	}
	req->oldidx = size;
	return 0;
}

uint32_t
get(const char *name)
{
	struct sysctl_oid *oid = oid_named(name);
	uint32_t value = 0;
	struct sysctl_req req = { NULL, 0, &value, sizeof(value), 0 };

	if (oid->handler != NULL) {
		oid->handler(oid, oid->arg1, oid->arg2, &req);
	} else {
		value = *(volatile uint32_t *)oid->arg1;
	}
	return value;
}

const char *
get_string(const char *name)
{
	static char text[256];
	struct sysctl_oid *oid = oid_named(name);
	struct sysctl_req req = { NULL, 0, text, sizeof(text), 0 };

	oid->handler(oid, oid->arg1, oid->arg2, &req);
	return text;
}

void
set(const char *name, uint32_t value)
{
	struct sysctl_oid *oid = oid_named(name);
	struct sysctl_req req = { &value, sizeof(value), NULL, 0, 0 };

	if (!(oid->flags & CTLFLAG_ANYBODY)) {
		fprintf(stderr, "sysctl %s is not writable by Darwin's processes\n", name);
		exit(2);
	}
	oid->handler(oid, oid->arg1, oid->arg2, &req);
}


uint64_t
get64(const char *name)
{
	return *(volatile uint64_t *)oid_named(name)->arg1;
}

int
try_as_root(const char *name, int32_t value)
{
	struct sysctl_oid *oid = oid_named(name);
	struct sysctl_req req = { &value, sizeof(value), NULL, 0, 0 };

	return oid->handler(oid, oid->arg1, oid->arg2, &req);
}

/* a sysctl root may write (no CTLFLAG_ANYBODY): a plain number */
void
set_as_root(const char *name, int32_t value)
{
	struct sysctl_oid *oid = oid_named(name);

	if (oid->handler != NULL) {
		struct sysctl_req req = { &value, sizeof(value), NULL, 0, 0 };

		oid->handler(oid, oid->arg1, oid->arg2, &req);
	} else {
		*(volatile int32_t *)oid->arg1 = value;
	}
}
