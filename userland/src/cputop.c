/*
 * cputop: which processes the processors' time went to, into the kernel's log -- for a run nobody
 * watches, where a frame of the window server's waits for SpringBoard or an application and the
 * log says only that the cores were busy.
 *
 * usage: cputop SECONDS COUNT
 * Goes into the background at once (a job of com.redwood.demo runs its lines one after another),
 * then every SECONDS for COUNT times: "cputop: NAME[PID] P% ..." for the six that used the most,
 * in percent of one core over the interval.
 */
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* libproc's, which the iOS SDK has no header for */
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);
#define PROC_ALL_PIDS 1
#define PROC_PIDTASKINFO 4

/* sys/proc_info.h's, likewise */
struct proc_taskinfo {
	uint64_t pti_virtual_size, pti_resident_size, pti_total_user, pti_total_system, pti_threads_user,
	    pti_threads_system;
	int32_t pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent, pti_messages_received,
	    pti_syscalls_mach, pti_syscalls_unix, pti_csw, pti_threadnum, pti_numrunning, pti_priority;
};

#define MAX_PROCS 1024

struct sample {
	int pid;
	uint64_t ns;
	char name[2 * MAXCOMLEN + 1];
};

static mach_timebase_info_data_t tb;

static int
take(struct sample *out)
{
	int pids[MAX_PROCS];
	int n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)) / (int)sizeof(int), k = 0;

	for (int i = 0; i < n && k < MAX_PROCS; i++) {
		struct proc_taskinfo task;

		if (pids[i] <= 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &task, sizeof(task)) != sizeof(task)) {
			continue;
		}
		out[k].pid = pids[i];
		/* (Mach ticks on arm64, not nanoseconds) */
		out[k].ns = (task.pti_total_user + task.pti_total_system) * tb.numer / tb.denom;
		if (proc_name(pids[i], out[k].name, sizeof(out[k].name)) <= 0) {
			strcpy(out[k].name, "?");
		}
		k++;
	}
	return k;
}

static void
say(const char *line)
{
	sysctlbyname("kern.redwood.say", NULL, NULL, (void *)line, strlen(line));
}

int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 2, count = argc > 2 ? (unsigned)atoi(argv[2]) : 100;
	static struct sample before[MAX_PROCS], after[MAX_PROCS];
	int nb, na;

	mach_timebase_info(&tb);
	if (fork() != 0) {
		return 0;               /* the job goes on to its next line */
	}
	nb = take(before);
	for (unsigned round = 0; round < count; round++) {
		struct { int index; uint64_t used; } top[6];
		char line[400];
		int length;

		for (int t = 0; t < 6; t++) {
			top[t].index = -1;
			top[t].used = 0;
		}

		sleep(seconds);
		na = take(after);
		for (int i = 0; i < na; i++) {
			uint64_t used = after[i].ns;

			for (int j = 0; j < nb; j++) {
				if (before[j].pid == after[i].pid) {
					used = after[i].ns > before[j].ns ? after[i].ns - before[j].ns : 0;
					break;
				}
			}
			for (int t = 0; t < 6; t++) {
				if (top[t].index < 0 || used > top[t].used) {
					memmove(&top[t + 1], &top[t], sizeof(top[0]) * (5 - t));
					top[t].index = i;
					top[t].used = used;
					break;
				}
			}
		}
		length = snprintf(line, sizeof(line), "cputop:");
		for (int t = 0; t < 6 && top[t].index >= 0; t++) {
			length += snprintf(line + length, sizeof(line) - length, " %s[%d] %llu%%", after[top[t].index].name,
			                   after[top[t].index].pid,
			                   (unsigned long long)(top[t].used / 10000000ull / seconds));
		}
		say(line);
		memcpy(before, after, sizeof(after[0]) * (size_t)na);
		nb = na;
	}
	return 0;
}
