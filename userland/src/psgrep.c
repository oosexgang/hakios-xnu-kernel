/*
 * psgrep WORD...: the processes whose path has one of the words in it (case ignored), one line each
 * -- pid, parent, state and flags, and path -- and how many processes there are in all. For a job's output kept by
 * tolog: ps -A is longer than the kernel keeps (96 lines), and the job runner has no shell to grep.
 * psgrep -stack WORD...: and each one's threads, by the kernel (kern.redwood.ustack), with the shared cache's slide.
 * psgrep -sysctl NAME [TIMES]: a string sysctl, TIMES times a second apart (iOS has no sysctl command).
 * psgrep -cpu SECONDS: the ten processes that used the most CPU in the next SECONDS, as % of one core.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <unistd.h>

/* libSystem's, without the header, which the iOS SDK leaves out */
int proc_pidpath(int pid, void *buffer, uint32_t size);
int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
#define PROC_PIDPATHINFO_MAXSIZE 4096

/* the shared cache's slide in this process, the same in every one: what to take off the addresses kern.redwood.ustack says */
extern const void *_dyld_get_shared_cache_range(size_t *length);

static struct kinfo_proc *
all_processes(unsigned *count)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0;
	struct kinfo_proc *procs;

	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || (procs = malloc(size += 16 * sizeof(*procs))) == NULL ||
	    sysctl(mib, 3, procs, &size, NULL, 0) != 0) {
		perror("psgrep: kern.proc.all");
		exit(1);
	}
	*count = (unsigned)(size / sizeof(*procs));
	return procs;
}

static uint64_t
cpu_time(pid_t pid)
{
	struct rusage_info_v0 usage;

	if (proc_pid_rusage(pid, RUSAGE_INFO_V0, (rusage_info_t *)&usage) != 0) {
		return 0;
	}
	return usage.ri_user_time + usage.ri_system_time;
}

/* -cpu SECONDS: each process's CPU time before and after, the biggest differences said */
static int
cpu_top(int seconds)
{
	enum { MOST = 512 };
	static pid_t pids[MOST];
	static uint64_t before[MOST], used[MOST];
	static char names[MOST][17];
	unsigned count, n = 0;
	struct kinfo_proc *procs = all_processes(&count);
	mach_timebase_info_data_t base;
	uint64_t total = 0;

	for (unsigned i = 0; i < count && n < MOST; i++, n++) {
		pids[n] = procs[i].kp_proc.p_pid;
		strlcpy(names[n], procs[i].kp_proc.p_comm, sizeof(names[n]));
		before[n] = cpu_time(pids[n]);
	}
	free(procs);
	sleep((unsigned)seconds);
	mach_timebase_info(&base);
	for (unsigned i = 0; i < n; i++) {
		uint64_t after = cpu_time(pids[i]);

		used[i] = after > before[i] ? (after - before[i]) * base.numer / base.denom : 0;      /* ns */
		total += used[i];
	}
	printf("psgrep: CPU in %d s: %.0f%% of one core in all\n", seconds, total / (seconds * 1e7));
	for (int k = 0; k < 10; k++) {
		unsigned best = 0;

		for (unsigned i = 1; i < n; i++) {
			if (used[i] > used[best]) {
				best = i;
			}
		}
		if (used[best] == 0) {
			break;
		}
		printf("  %5.1f%% %s (%d)\n", used[best] / (seconds * 1e7), names[best], pids[best]);
		used[best] = 0;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int stacks = argc > 1 && strcmp(argv[1], "-stack") == 0;       /* -stack: and the threads of each, by the kernel */

	if (argc > 2 && strcmp(argv[1], "-cpu") == 0) {
		return cpu_top(atoi(argv[2]) > 0 ? atoi(argv[2]) : 10);
	}
	if (argc > 2 && strcmp(argv[1], "-sysctl") == 0) {
		/* -sysctl NAME [TIMES]: a string sysctl said TIMES times a second apart (iOS has no sysctl command) */
		int times = argc > 3 ? atoi(argv[3]) : 1;

		for (int i = 0; i < times; i++) {
			char value[1024] = "";
			size_t size = sizeof(value) - 1;

			if (sysctlbyname(argv[2], value, &size, NULL, 0) != 0) {
				perror(argv[2]);
				return 1;
			}
			if (size == sizeof(int)) {
				printf("%s: %#x\n", argv[2], *(int *)value);     /* (an integer one) */
			} else {
				/* (in pieces: a kept line holds about 200 characters) */
				size_t length = strlen(value);

				for (size_t at = 0; at == 0 || at < length; at += 180) {
					printf("%s%s: %.180s\n", argv[2], at ? " (more)" : "", value + at);
				}
			}
			fflush(stdout);
			if (i + 1 < times) {
				sleep(1);
			}
		}
		return 0;
	}

	if (stacks) {
		size_t length = 0;

		printf("psgrep: the shared cache's slide %#lx\n", (unsigned long)((uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL));
		argv++;
		argc--;
	}
	unsigned count, shown = 0;
	struct kinfo_proc *procs = all_processes(&count);

	for (unsigned i = 0; i < count; i++) {
		char path[PROC_PIDPATHINFO_MAXSIZE] = "";
		pid_t pid = procs[i].kp_proc.p_pid;

		if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
			strlcpy(path, procs[i].kp_proc.p_comm, sizeof(path));
		}
		for (int w = 1; w < argc; w++) {
			const char *at;
			size_t n = strlen(argv[w]);

			for (at = path; *at && strncasecmp(at, argv[w], n) != 0; at++) {
			}
			if (*at) {
				/* p_stat: 1 being made, 2 running, 3 asleep, 4 STOPPED, 5 a zombie */
				printf("%d (parent %d, state %d, flags %#x) %s\n", pid, procs[i].kp_eproc.e_ppid,
				    procs[i].kp_proc.p_stat, procs[i].kp_proc.p_flag, path);
				shown++;
				if (stacks) {
					fflush(stdout);
					if (sysctlbyname("kern.redwood.ustack", NULL, NULL, &pid, sizeof(pid)) != 0) {
						perror("psgrep: kern.redwood.ustack");
					}
				}
				break;
			}
		}
	}
	printf("psgrep: %u of %u processes\n", shown, count);
	return 0;
}
