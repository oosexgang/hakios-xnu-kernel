/*
 * The fake kernel of the driver tests (fakekernel.h): pthreads for threads, condition variables for waits,
 * the host's monotonic clock, a table of sysctls, boot-args and the board's name set by the test.
 */
#include <pthread.h>
#include <time.h>
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

void
assert_wait(void *event, int interruptible __unused)
{
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

void
thread_wakeup(void *event __unused)
{
	pthread_mutex_lock(&big);
	wake_generation++;
	pthread_cond_broadcast(&woken);
	pthread_mutex_unlock(&big);
}

struct start { thread_continue_t fn; void *arg; };

static void *
thread_main(void *p)
{
	struct start s = *(struct start *)p;

	free(p);
	s.fn(s.arg, 0);
	return NULL;
}

kern_return_t
kernel_thread_start(thread_continue_t fn, void *arg, thread_t *thread)
{
	pthread_t t;
	struct start *s = malloc(sizeof(*s));

	s->fn = fn;
	s->arg = arg;
	*thread = NULL;
	return pthread_create(&t, NULL, thread_main, s) == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

void thread_deallocate(thread_t thread __unused) {}
void thread_set_thread_name(thread_t thread __unused, const char *name __unused) {}

thread_t current_thread(void) { return NULL; }

kern_return_t
thread_terminate(thread_t thread __unused)
{
	pthread_exit(NULL);
}

void
nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result)
{
	*result = nanoseconds;          /* (the fake clock counts nanoseconds) */
}

void
delay_for_interval(uint32_t interval, uint32_t scale)
{
	uint64_t nanoseconds = (uint64_t)interval * scale;
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
			if (text[0] >= '0' && text[0] <= '9') {
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

/* sysctl: a table by name ("_hw_backlight.level") */
static struct sysctl_oid *oids[32];
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
		*changed = 1;
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
		fprintf(stderr, "sysctl %s is not writable by iOS's processes\n", name);
		exit(2);
	}
	oid->handler(oid, oid->arg1, oid->arg2, &req);
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
