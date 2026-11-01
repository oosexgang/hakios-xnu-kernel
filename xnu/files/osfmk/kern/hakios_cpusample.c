/*
 * HakiOS: what the processors spend their time on, for finding a slow frame's cause (boot-arg cpu_sample=1).
 *
 * A thread of its own looks once a millisecond at every processor's running thread and counts it (by its
 * address only: nothing of a thread is touched while sampling, a thread may be gone a moment later). Every
 * five seconds the counts are matched against the live threads, under the lock that keeps them alive, and the
 * processes and threads that had the most samples are logged, as a share of all the processors' time:
 *
 *   cpu: 5 s, 7 cpus: idle 61%; backboardd 14% (com.apple.coreanimation.render-server 11%), SpringBoard 9% ...
 *
 * Started by hakios_cpusample_start() (the Xiaomi 12S: hakios.adreno7.c, once the GPU is kept).
 */
#include <mach/mach_types.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/queue.h>
#include <kern/misc_protos.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern char *proc_name_address(void *p);
extern void bsd_getthreadname(void *uth, char *buffer);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);

#define SAMPLE_SLOTS    512             /* threads seen in five seconds, at most (more are counted as "others") */
#define SAMPLE_TOP      8

static struct {
	thread_t        thread;
	uint32_t        count;
} slots[SAMPLE_SLOTS];
static uint32_t idle_samples, all_samples, other_samples;

static void
count(thread_t thread)
{
	uint32_t h = (uint32_t)(((uintptr_t)thread >> 4) * 2654435761u) % SAMPLE_SLOTS;

	for (uint32_t i = 0; i < 16; i++, h = (h + 1) % SAMPLE_SLOTS) {
		if (slots[h].thread == thread) {
			slots[h].count++;
			return;
		}
		if (slots[h].thread == THREAD_NULL) {
			slots[h].thread = thread;
			slots[h].count = 1;
			return;
		}
	}
	other_samples++;
}

struct named {
	char            proc[32];
	char            thread[64];
	uint32_t        count;
};

static void
report(uint32_t cpus)
{
	static struct named procs[SAMPLE_TOP * 4], busy[SAMPLE_TOP];
	uint32_t nprocs = 0, nthreads = 0;
	char line[1024];
	size_t at = 0;
	thread_t thread;

	bzero(procs, sizeof(procs));
	bzero(busy, sizeof(busy));
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&threads, thread, thread_t, threads) {
		uint32_t n = 0;
		uint32_t h = (uint32_t)(((uintptr_t)thread >> 4) * 2654435761u) % SAMPLE_SLOTS;
		const char *pname = "?";
		char tname[64] = "";
		task_t task;
		void *proc;

		for (uint32_t i = 0; i < 16; i++, h = (h + 1) % SAMPLE_SLOTS) {
			if (slots[h].thread == thread) {
				n = slots[h].count;
				break;
			}
			if (slots[h].thread == THREAD_NULL) {
				break;
			}
		}
		if (n == 0) {
			continue;
		}
		task = get_threadtask(thread);
		proc = task == TASK_NULL ? NULL : get_bsdtask_info(task);
		if (task == kernel_task) {
			pname = "kernel";
		} else if (proc != NULL) {
			pname = proc_name_address(proc);
		}
		if (get_bsdthread_info(thread) != NULL) {
			bsd_getthreadname(get_bsdthread_info(thread), tname);
		}
		/* by process */
		uint32_t p;
		for (p = 0; p < nprocs && strncmp(procs[p].proc, pname, sizeof(procs[p].proc)) != 0; p++) {
		}
		if (p == nprocs && nprocs < sizeof(procs) / sizeof(procs[0])) {
			strlcpy(procs[p].proc, pname, sizeof(procs[p].proc));
			nprocs++;
		}
		if (p < nprocs) {
			procs[p].count += n;
		}
		/* the busiest threads */
		uint32_t least = 0;
		for (uint32_t t = 1; t < SAMPLE_TOP; t++) {
			least = busy[t].count < busy[least].count ? t : least;
		}
		if (n > busy[least].count) {
			strlcpy(busy[least].proc, pname, sizeof(busy[least].proc));
			strlcpy(busy[least].thread, tname[0] ? tname : "(no name)", sizeof(busy[least].thread));
			busy[least].count = n;
			nthreads = nthreads < SAMPLE_TOP ? nthreads + 1 : nthreads;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);

	if (all_samples == 0) {
		return;
	}
	at += (size_t)snprintf(line + at, sizeof(line) - at, "cpu: 5 s, %u cpus: idle %u%%;", cpus, idle_samples * 100 / all_samples);
	for (uint32_t k = 0; k < SAMPLE_TOP && at < sizeof(line) - 64; k++) {
		uint32_t best = 0;

		for (uint32_t p = 1; p < nprocs; p++) {
			best = procs[p].count > procs[best].count ? p : best;
		}
		if (nprocs == 0 || procs[best].count * 200 < all_samples) {
			break;          /* (under half a percent) */
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %u.%u%%", procs[best].proc,
		    procs[best].count * 100 / all_samples, procs[best].count * 1000 / all_samples % 10);
		procs[best].count = 0;
	}
	printf("%s\n", line);
	at = 0;
	at += (size_t)snprintf(line + at, sizeof(line) - at, "cpu: busiest threads:");
	for (uint32_t k = 0; k < SAMPLE_TOP && at < sizeof(line) - 96; k++) {
		uint32_t best = 0;

		for (uint32_t t = 1; t < SAMPLE_TOP; t++) {
			best = busy[t].count > busy[best].count ? t : best;
		}
		if (busy[best].count == 0) {
			break;
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %s/%s %u.%u%%;", busy[best].proc, busy[best].thread,
		    busy[best].count * 100 / all_samples, busy[best].count * 1000 / all_samples % 10);
		busy[best].count = 0;
	}
	printf("%s\n", line);
}

static void
sampler(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t cpus = machine_info.logical_cpu_max;
	uint64_t since = mach_absolute_time(), five;

	nanoseconds_to_absolutetime(5ULL * NSEC_PER_SEC, &five);
	cpus = cpus > MAX_SCHED_CPUS ? MAX_SCHED_CPUS : cpus;
	for (;;) {
		for (uint32_t i = 0; i < cpus; i++) {
			processor_t processor = processor_array[i];
			thread_t thread;

			if (processor == PROCESSOR_NULL) {
				continue;
			}
			thread = processor->active_thread;
			all_samples++;
			if (thread == THREAD_NULL || thread == processor->idle_thread) {
				idle_samples++;
			} else if (thread != current_thread()) {
				count(thread);
			} else {
				idle_samples++;          /* (this sampler: about nothing) */
			}
		}
		if (mach_absolute_time() - since >= five) {
			report(cpus);
			bzero(slots, sizeof(slots));
			idle_samples = all_samples = other_samples = 0;
			since = mach_absolute_time();
		}
		delay(1000);
	}
}

void hakios_cpusample_start(void);

void
hakios_cpusample_start(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("cpu_sample", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start_priority(sampler, NULL, 95, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "cpu sampler");
		thread_deallocate(thread);
		printf("cpu: the sampler runs (cpu_sample=1): what the processors run, every 5 s\n");
	}
}
