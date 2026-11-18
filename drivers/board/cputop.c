/*
 * Boot-arg cputop=S: every S seconds, the processes that took the most CPU time since the last time, by name and pid,
 * and how many processes came and went meanwhile. (All cores at 98-100 % for the first minute and
 * more after boot -- the governor's own line says how busy each CPU is, not for whom.)
 *
 * A process's time is its task's (recount_task_times: every thread's, the ones that are gone too), taken from each live
 * process in turn (proc_iterate holds a reference to it meanwhile). A process that came and went between two looks is
 * not seen, only counted: what each process had at the last look is kept by pid.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc_internal.h>
#include <kern/recount.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#define CPUTOP_PROCS            640
#define CPUTOP_SHOWN            12
#define CPUTOP_MAX_SECONDS      3600

extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern task_t proc_task(proc_t);

struct cputop_entry {
	int             pid;
	uint64_t        total;          /* user and system, absolute time */
	uint64_t        took;           /* since the last look */
	char            name[24];
};

static struct cputop_entry cputop_was[CPUTOP_PROCS], cputop_now[CPUTOP_PROCS];
static uint32_t cputop_was_count, cputop_now_count, cputop_new;
static uint32_t cputop_seconds;

static int
cputop_one(proc_t p, void *arg __unused)
{
	struct cputop_entry *e;
	struct recount_times_mach times;
	task_t task = proc_task(p);
	int pid = proc_pid(p);
	uint32_t i;

	if (task == TASK_NULL || cputop_now_count >= CPUTOP_PROCS) {
		return PROC_RETURNED;
	}
	times = recount_task_times(task);
	e = &cputop_now[cputop_now_count++];
	e->pid = pid;
	e->total = times.rtm_user + times.rtm_system;
	e->took = e->total;
	for (i = 0; i < cputop_was_count; i++) {
		if (cputop_was[i].pid == pid) {
			e->took = e->total >= cputop_was[i].total ? e->total - cputop_was[i].total : 0;
			break;
		}
	}
	if (i == cputop_was_count && cputop_was_count != 0) {
		cputop_new++;
	}
	strlcpy(e->name, proc_best_name(p), sizeof(e->name));
	return PROC_RETURNED;
}

static void
cputop_thread(void *arg __unused, wait_result_t w __unused)
{
	for (;;) {
		uint64_t all = 0, ns;
		uint32_t gone = 0;
		char line[640] = "";            /* (said as it is when no process took any time: empty, not whatever was there) */
		int at = 0;

		delay_for_interval(cputop_seconds * 1000, NSEC_PER_MSEC);
		cputop_now_count = 0;
		cputop_new = 0;
		proc_iterate(PROC_ALLPROCLIST, cputop_one, NULL, NULL, NULL);
		for (uint32_t i = 0; i < cputop_was_count; i++) {
			uint32_t j;

			for (j = 0; j < cputop_now_count && cputop_now[j].pid != cputop_was[i].pid; j++) {
			}
			gone += j == cputop_now_count;
		}
		for (uint32_t i = 0; i < cputop_now_count; i++) {
			all += cputop_now[i].took;
		}
		/* the CPUTOP_SHOWN that took most, by selection: a few hundred processes */
		for (uint32_t shown = 0; shown < CPUTOP_SHOWN && shown < cputop_now_count; shown++) {
			uint32_t best = shown;

			for (uint32_t i = shown + 1; i < cputop_now_count; i++) {
				if (cputop_now[i].took > cputop_now[best].took) {
					best = i;
				}
			}
			if (best != shown) {
				struct cputop_entry t = cputop_now[shown];

				cputop_now[shown] = cputop_now[best];
				cputop_now[best] = t;
			}
			if (cputop_now[shown].took == 0) {
				break;
			}
			absolutetime_to_nanoseconds(cputop_now[shown].took, &ns);
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s[%d] %llu", cputop_now[shown].name,
			    cputop_now[shown].pid, ns / NSEC_PER_MSEC);
			if (at >= (int)sizeof(line) - 1) {
				break;
			}
		}
		absolutetime_to_nanoseconds(all, &ns);
		hakios_tl("cputop: %u s: %llu ms of CPU in %u processes (%u new, %u gone); most, ms:%s\n", cputop_seconds,
		    ns / NSEC_PER_MSEC, cputop_now_count, cputop_new, gone, line);
		bcopy(cputop_now, cputop_was, sizeof(cputop_now[0]) * cputop_now_count);
		cputop_was_count = cputop_now_count;
	}
}

void hakios_cputop_start(void);
void
hakios_cputop_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cputop", &cputop_seconds, sizeof(cputop_seconds)) || cputop_seconds == 0) {
		return;
	}
	/* at most an hour: in milliseconds (the wait's unit) more than 49 days would not fit in 32 bits */
	cputop_seconds = cputop_seconds > CPUTOP_MAX_SECONDS ? CPUTOP_MAX_SECONDS : cputop_seconds;
	if (kernel_thread_start(cputop_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
