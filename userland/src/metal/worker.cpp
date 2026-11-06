/*
 * The driver's own thread, as a GPU driver has one (boot-arg rwm_async=1 only: with it frames drew parts of other
 * frames, the "breakcore" of runs 236, 240, 244, and without it they are clean and no slower). -commit hands the buffer
 * over and returns, and the window server builds its next frame while this one's draws are made. A stack of 16 MB,
 * for the library's recursion; the window server's own rendering class, so that the scheduler puts it on a big core.
 * What the thread runs is the plug-in's (a command buffer, inside its autorelease pool).
 */
#include "rwm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t more = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;

struct job {
	rwm_job_fn run;
	void *context;
};

constexpr unsigned ring_room = 16;      /* rwm_queue is 8 at most */
job ring[ring_room];
unsigned head, queued;
pthread_t thread;
bool started;
bool busy;                              /* a job taken off the queue and not yet done */

void
say(const char *line)
{
	sysctlbyname("kern.redwood.say", NULL, NULL, (void *)line, strlen(line));
}

void *
main_loop(void *)
{
	pthread_setname_np("rwm worker");       /* (redwood_drm tells its waits apart by this name) */
	rwm_bind_big(2);
	for (;;) {
		job j;

		pthread_mutex_lock(&lock);
		while (queued == 0) {
			pthread_cond_wait(&more, &lock);
		}
		j = ring[head];
		head = (head + 1) % ring_room;
		queued--;
		busy = true;
		pthread_mutex_unlock(&lock);

		const uint64_t from = rwm_ns(), from_cpu = rwm_thread_cpu_ns();

		if (rwm_trace()) {
			rwm_trace_line("b", j.context);
		}
		j.run(j.context);
		if (rwm_trace()) {
			rwm_trace_line("e", j.context);
		}
		rwm_perf.worker_cbs++;
		rwm_perf.worker_ns += rwm_ns() - from;
		rwm_perf.worker_cpu += rwm_thread_cpu_ns() - from_cpu;
		pthread_mutex_lock(&lock);
		busy = false;
		pthread_cond_broadcast(&done);
		pthread_mutex_unlock(&lock);
	}
	return nullptr;
}

void
start()
{
	pthread_attr_t attr;
	pthread_t t;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 16 << 20);
	pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
	if (pthread_create(&t, &attr, main_loop, nullptr) == 0) {
		thread = t;
		started = true;
	}
	pthread_attr_destroy(&attr);
}

pthread_once_t once = PTHREAD_ONCE_INIT;

} // namespace

extern "C" bool
rwm_worker_on(void)
{
	return started && pthread_equal(pthread_self(), thread);
}

/*
 * The window server's two busiest threads on the two Cortex-A75s (kern.redwood.bind_cpu: logical processors 1 and 2):
 * CoreAnimation's render thread, the one that commits, on 1, and this worker on 2 -- with boot-arg bigcore=1 only:
 * since 26 October a thread bound to one core waited for that core more than it ran (runs 194, 195).
 */
extern "C" void
rwm_bind_big(int cpu)
{
	static int wanted = -1;

	if (wanted < 0) {
		wanted = strcmp(getprogname(), "backboardd") == 0 && rwm_boot_arg("bigcore=1");
	}
	if (wanted) {
		(void)sysctlbyname("kern.redwood.bind_cpu", NULL, NULL, &cpu, sizeof(cpu));
	}
}

/* boot-arg mali_trace=1 (or rwm_trace=1): the worker's buffers on the same time line as the kernel's "tl:" lines */
extern "C" bool
rwm_trace(void)
{
	static int on = -1;

	if (on < 0) {
		on = (rwm_boot_arg("mali_trace=1") || rwm_boot_arg("rwm_trace=1")) && strcmp(getprogname(), "backboardd") == 0;
	}
	return on;
}

extern "C" void
rwm_trace_line(const char *what, void *object)
{
	char line[96];

	snprintf(line, sizeof(line), "tl: w %s %lx at %llu", what, (unsigned long)((uintptr_t)object >> 4 & 0xffffff),
		 (unsigned long long)(rwm_ns() / 1000));
	say(line);
}

/* how many command buffers may wait for the worker: boot-arg rwm_queue=N (1 to 8), 3 by default (two until 26 October):
 * the window server builds one frame while the driver draws the one before, and does not run ahead of the screen */
extern "C" unsigned
rwm_queue_depth(void)
{
	static int depth = -1;

	if (depth < 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_queue=");

		depth = at != nullptr && atoi(at + 10) >= 1 && atoi(at + 10) <= 8 ? atoi(at + 10) : 3;
	}
	return (unsigned)depth;
}

extern "C" void
rwm_worker_submit(rwm_job_fn run, void *context)
{
	pthread_once(&once, start);
	pthread_mutex_lock(&lock);
	if (queued >= rwm_queue_depth()) {
		const uint64_t from = rwm_ns();

		while (queued >= rwm_queue_depth()) {
			pthread_cond_wait(&done, &lock);
		}
		rwm_perf.queue_waits++;
		rwm_perf.queue_ns += rwm_ns() - from;
	}
	if (rwm_trace()) {
		rwm_trace_line("c", context);
	}
	ring[(head + queued) % ring_room] = job{ run, context };
	queued++;
	pthread_cond_signal(&more);
	pthread_mutex_unlock(&lock);
}

/*
 * What touches a texture's memory at once, outside any command buffer (-replaceRegion:, -getBytes:, a depth read),
 * waits first for the buffers committed before it to have been drawn: in Metal's order they come first, but on the
 * worker they are recorded later, and a draw recorded after the write samples what was written after it (the
 * "breakcore" of 27 October). Boot-arg rwm_drain=0: not waited for.
 */
extern "C" void
rwm_worker_drain(const char *what)
{
	static int wanted = -1;
	static unsigned told;

	if (wanted < 0) {
		wanted = !rwm_boot_arg("rwm_drain=0");
	}
	if (!wanted || !started || rwm_worker_on()) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (queued != 0 || busy) {
		const uint64_t from = rwm_ns();
		const unsigned waiting = queued;

		while (queued != 0 || busy) {
			pthread_cond_wait(&done, &lock);
		}
		rwm_perf.drains++;
		rwm_perf.drain_ns += rwm_ns() - from;
		if (++told <= 6) {
			char line[160];

			snprintf(line, sizeof(line), "!rwm %s: %s waited %llu us for the worker (%u buffers queued)", getprogname(),
				 what, (unsigned long long)((rwm_ns() - from) / 1000), waiting);
			say(line);
		}
	}
	pthread_mutex_unlock(&lock);
}

extern "C" void
rwm_worker_mark(volatile bool *flag)
{
	pthread_mutex_lock(&lock);
	*flag = true;
	pthread_cond_broadcast(&done);
	pthread_mutex_unlock(&lock);
}

extern "C" void
rwm_worker_wait(const volatile bool *flag)
{
	const uint64_t from = rwm_ns();

	pthread_mutex_lock(&lock);
	while (!*flag) {
		pthread_cond_wait(&done, &lock);
	}
	rwm_perf.processed_waits++;
	rwm_perf.processed_ns += rwm_ns() - from;
	pthread_mutex_unlock(&lock);
}

/*
 * The compiler library on a stack of its own. Translating a module (AIR -> NIR -> panfrost) goes deep, and the window
 * server builds its pipelines on dispatch's worker threads, whose 512 KB a shader of CoreAnimation's runs off the end
 * of: a fault on the guard page, where no handler can run, and backboardd gone every twelve seconds without a word. So
 * the call runs on a thread of 16 MB and the caller waits for it (on its own thread when none can be made).
 */
namespace {

struct big_stack_call {
	void (*fn)(void *);
	void *context;
};

void *
big_stack_main(void *argument)
{
	const big_stack_call *call = (const big_stack_call *)argument;

	call->fn(call->context);
	return nullptr;
}

} // namespace

extern "C" void
rwm_on_big_stack(void (*fn)(void *context), void *context)
{
	pthread_attr_t attr;
	pthread_t thread;
	big_stack_call call = { fn, context };

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 16 << 20);
	if (pthread_create(&thread, &attr, big_stack_main, &call) != 0) {
		pthread_attr_destroy(&attr);
		fn(context);
		return;
	}
	pthread_attr_destroy(&attr);
	pthread_join(thread, nullptr);
}
