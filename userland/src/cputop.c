/*
 * Which processes take the processors: every few seconds, the processor time each process was given since the
 * last look (user and system together), the most first, as one line for the run's log:
 *
 *   cputop: 25 s: 412% in all: backboardd[13] 96% SpringBoard[41] 88% WeatherWidget[131] 40% ...
 *
 * (100% is one core's whole time.) tools/frames.py says when the cores were all busy ("cores busy") while the
 * window server's frames came late; this says with whom it shared them. A process born since the last look counts
 * with all the time it has had.
 *
 *   usage: cputop [SECONDS [LOOKS]]     every SECONDS (default 5), LOOKS times (default: until the run ends)
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libproc's, which the iPhoneOS SDK does not declare (XNU's bsd/sys/proc_info.h, libsyscall/wrappers/libproc) */
#define PROC_ALL_PIDS           1
#define PROC_PIDTASKINFO        4
struct proc_taskinfo {
	uint64_t pti_virtual_size;
	uint64_t pti_resident_size;
	uint64_t pti_total_user;        /* in mach_absolute_time's units */
	uint64_t pti_total_system;
	uint64_t pti_threads_user;
	uint64_t pti_threads_system;
	int32_t pti_policy;
	int32_t pti_faults;
	int32_t pti_pageins;
	int32_t pti_cow_faults;
	int32_t pti_messages_sent;
	int32_t pti_messages_received;
	int32_t pti_syscalls_mach;
	int32_t pti_syscalls_unix;
	int32_t pti_csw;
	int32_t pti_threadnum;
	int32_t pti_numrunning;
	int32_t pti_priority;
};
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

#define MAX_PROCS       2048
#define SHOWN           10

struct sample {
	int pid;
	uint64_t time;                  /* user + system, mach_absolute_time's units */
};

static struct sample last[MAX_PROCS], now[MAX_PROCS];
static int last_count;

struct share {
	int pid;
	uint64_t time;
};

static int
by_time(const void *a, const void *b)
{
	const struct share *x = a, *y = b;

	return x->time < y->time ? 1 : x->time > y->time ? -1 : 0;
}

static uint64_t
time_before(int pid, int *found)
{
	for (int i = 0; i < last_count; i++) {
		if (last[i].pid == pid) {
			*found = 1;
			return last[i].time;
		}
	}
	*found = 0;
	return 0;
}

/*
 * "vmem:" beside each "cputop:" line: the page faults and page-ins SpringBoard and backboardd had since the look before
 * (a page of the shared cache or of a binary that is not in memory costs a read from the flash through APFS: a stall in code that
 * has nothing slow in it), and the whole system's memory: free pages, and what was paged in, out, compressed and decompressed.
 */
static int watched_pid[2];
static int32_t watched_faults[2], watched_pageins[2];
static vm_statistics64_data_t vm_last;

static void
vmem_line(uint64_t seconds_since_start)
{
	static const char *const names[2] = { "SpringBoard", "backboardd" };
	static int pids[MAX_PROCS];
	int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids));
	vm_statistics64_data_t vm;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

	printf("vmem: %llu s:", (unsigned long long)seconds_since_start);
	for (int i = 0; i < bytes / (int)sizeof(int); i++) {
		char name[64] = "";

		if (pids[i] <= 0 || proc_name(pids[i], name, sizeof(name)) <= 0) {
			continue;
		}
		for (int w = 0; w < 2; w++) {
			struct proc_taskinfo info;

			if (strcmp(name, names[w]) != 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &info, sizeof(info)) != sizeof(info)) {
				continue;
			}
			if (watched_pid[w] == pids[i]) {
				printf(" %s faults +%d pageins +%d rss %llu MB;", names[w], info.pti_faults - watched_faults[w],
				    info.pti_pageins - watched_pageins[w], (unsigned long long)(info.pti_resident_size >> 20));
			}
			watched_pid[w] = pids[i];
			watched_faults[w] = info.pti_faults;
			watched_pageins[w] = info.pti_pageins;
		}
	}
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
		if (vm_last.free_count != 0) {
			printf(" free %llu MB, pageins +%llu pageouts +%llu compressions +%llu decompressions +%llu purges +%llu",
			    (unsigned long long)vm.free_count * vm_page_size >> 20, vm.pageins - vm_last.pageins,
			    vm.pageouts - vm_last.pageouts, vm.compressions - vm_last.compressions,
			    vm.decompressions - vm_last.decompressions, vm.purges - vm_last.purges);
		}
		vm_last = vm;
	}
	printf("\n");
}

/*
 * "sysbench:" once, at the 56th and 58th looks (the start-up work is over by then, the gestures not begun): what the kernel's basic operations
 * cost here. A system call, a page fault (fresh memory touched, one page of 16 KB a time), the round trip of a mach message between
 * two threads (what XPC is made of; SpringBoard's main thread is mostly that), and memory copied: a kernel whose faults or messages
 * cost ten times what they should makes every piece of code slow alike.
 */
struct ping_message {
	mach_msg_header_t header;
	mach_msg_trailer_t trailer;
};

static void *
pong_thread(void *argument)
{
	mach_port_t *ports = argument;
	struct ping_message in;

	for (;;) {
		mach_msg_header_t out = { .msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(out),
			.msgh_remote_port = ports[1], .msgh_id = 1 };

		if (mach_msg(&in.header, MACH_RCV_MSG, 0, sizeof(in), ports[0], MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS ||
		    in.header.msgh_id == 2) {
			return NULL;
		}
		mach_msg(&out, MACH_SEND_MSG, sizeof(out), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	}
}

static double
micros_since(uint64_t began, mach_timebase_info_data_t *timebase)
{
	return (double)(mach_absolute_time() - began) * timebase->numer / timebase->denom / 1000.0;
}

/*
 * "cachebench:" what a load costs at the sizes of the caches: a pointer chased through a random ring of cache lines (nanoseconds a
 * load: ~1-2 in the first level, 4-10 in the second, 100 or more in memory) and bytes copied inside a buffer of that size (GB/s).
 * Memory the processor cannot cache shows as the same slow figure at every size.
 */
static void
cachebench(mach_timebase_info_data_t *timebase)
{
	static const size_t sizes[] = { 16 << 10, 128 << 10, 1 << 20, 8 << 20, 64 << 20 };
	char line[512];
	int at = snprintf(line, sizeof(line), "cachebench: load");

	for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		size_t lines = sizes[k] / 64;
		uintptr_t **ring = mmap(NULL, sizes[k], PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		uint64_t random = 88172645463325252ull, began;
		uintptr_t **here;
		const unsigned steps = 2000000;

		if (ring == MAP_FAILED) {
			continue;
		}
		/* Sattolo's shuffle: one ring through all the lines (each line's first word points to the next) */
		uint32_t *order = malloc(lines * sizeof(uint32_t));

		for (size_t i = 0; i < lines; i++) {
			order[i] = (uint32_t)i;
		}
		for (size_t i = lines - 1; i > 0; i--) {
			random ^= random << 13;
			random ^= random >> 7;
			random ^= random << 17;
			size_t j = random % i;
			uint32_t t = order[i];

			order[i] = order[j];
			order[j] = t;
		}
		for (size_t i = 0; i < lines; i++) {
			ring[order[i] * 8] = (uintptr_t *)&ring[order[(i + 1) % lines] * 8];
		}
		free(order);
		here = (uintptr_t **)ring[0];
		began = mach_absolute_time();
		for (unsigned i = 0; i < steps; i++) {
			here = (uintptr_t **)*here;
		}
		double ns = micros_since(began, timebase) * 1000.0 / steps;

		/* (the pointer is used, or the loop goes) */
		if (here == NULL) {
			ns = -1;
		}
		/* copy: half of the buffer to the other half, enough times to move 256 MB */
		size_t half = sizes[k] / 2, repeat = (256u << 20) / half;

		began = mach_absolute_time();
		for (size_t i = 0; i < repeat; i++) {
			memcpy((char *)ring + half, ring, half);
		}
		double gbps = (double)half * repeat / (micros_since(began, timebase) * 1000.0);

		at += snprintf(line + at, sizeof(line) - (size_t)at, " | %zu KB: %.1f ns, copy %.2f GB/s", sizes[k] >> 10, ns, gbps);
		munmap(ring, sizes[k]);
	}
	printf("%s\n", line);
}

/*
 * "membw:" the same memory three ways, in GB/s, with a buffer that fits the second-level cache and one that does not: summed
 * (plain loads), written (plain stores), and memcpy'd (the C library's, which uses non-temporal pair instructions for big sizes).
 * The kernel's own benchmark gets 12 GB/s from one core; a user program that gets a tenth of that has either memory mapped the
 * kernel's way not (cache attributes), or a memcpy that does not suit these cores.
 */
static void
membw(mach_timebase_info_data_t *timebase)
{
	static const size_t sizes[] = { 256 << 10, 64 << 20 };
	char line[400];
	int at = snprintf(line, sizeof(line), "membw:");

	for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		size_t bytes = sizes[k], words = bytes / 8, repeat = (512u << 20) / bytes;
		uint64_t *buffer = mmap(NULL, 2 * bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		uint64_t began, sum = 0;
		double read_gbps, write_gbps, copy_gbps;

		if (buffer == MAP_FAILED) {
			continue;
		}
		memset(buffer, 1, 2 * bytes);
		began = mach_absolute_time();
		for (size_t r = 0; r < repeat; r++) {
			uint64_t a = 0, b = 0, c = 0, d = 0;

			for (size_t i = 0; i < words; i += 4) {
				a += buffer[i];
				b += buffer[i + 1];
				c += buffer[i + 2];
				d += buffer[i + 3];
			}
			sum += a + b + c + d;
		}
		read_gbps = (double)bytes * repeat / (micros_since(began, timebase) * 1000.0);
		began = mach_absolute_time();
		for (size_t r = 0; r < repeat; r++) {
			for (size_t i = 0; i < words; i += 4) {
				buffer[i] = r;
				buffer[i + 1] = r;
				buffer[i + 2] = r;
				buffer[i + 3] = r;
			}
		}
		write_gbps = (double)bytes * repeat / (micros_since(began, timebase) * 1000.0);
		began = mach_absolute_time();
		for (size_t r = 0; r < repeat; r++) {
			memcpy(buffer + words, buffer, bytes);
		}
		copy_gbps = (double)bytes * repeat / (micros_since(began, timebase) * 1000.0);
		at += snprintf(line + at, sizeof(line) - (size_t)at, " | %zu KB: read %.2f write %.2f memcpy %.2f%s", bytes >> 10, read_gbps,
		    write_gbps, copy_gbps, sum == 12345 ? "!" : "");
		munmap(buffer, 2 * bytes);
	}
	printf("%s\n", line);
}

static void
sysbench(mach_timebase_info_data_t *timebase)
{
	enum { CALLS = 100000, TRIPS = 5000, FAULT_BYTES = 64 << 20, COPY_BYTES = 32 << 20 };
	double syscall_us, fault_us, trip_us, copy_ms;
	uint64_t began;
	size_t page = (size_t)vm_page_size;

	began = mach_absolute_time();
	for (int i = 0; i < CALLS; i++) {
		(void)getppid();
	}
	syscall_us = micros_since(began, timebase) / CALLS;

	char *memory = mmap(NULL, FAULT_BYTES, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	began = mach_absolute_time();
	for (size_t at = 0; memory != MAP_FAILED && at < FAULT_BYTES; at += page) {
		memory[at] = 1;
	}
	fault_us = micros_since(began, timebase) / (FAULT_BYTES / page);
	if (memory != MAP_FAILED) {
		char *copy = mmap(NULL, COPY_BYTES, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

		memset(copy, 2, COPY_BYTES);
		began = mach_absolute_time();
		memcpy(memory, copy, COPY_BYTES);
		copy_ms = micros_since(began, timebase) / 1000.0;
		munmap(copy, COPY_BYTES);
		munmap(memory, FAULT_BYTES);
	} else {
		copy_ms = -1;
	}

	mach_port_t ports[2];
	pthread_t thread;

	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &ports[0]);
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &ports[1]);
	mach_port_insert_right(mach_task_self(), ports[0], ports[0], MACH_MSG_TYPE_MAKE_SEND);
	mach_port_insert_right(mach_task_self(), ports[1], ports[1], MACH_MSG_TYPE_MAKE_SEND);
	trip_us = -1;
	if (pthread_create(&thread, NULL, pong_thread, ports) == 0) {
		struct ping_message in;
		mach_msg_header_t out = { .msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(out),
			.msgh_remote_port = ports[0], .msgh_id = 1 };

		began = mach_absolute_time();
		for (int i = 0; i < TRIPS; i++) {
			mach_msg(&out, MACH_SEND_MSG, sizeof(out), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
			mach_msg(&in.header, MACH_RCV_MSG, 0, sizeof(in), ports[1], MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		}
		trip_us = micros_since(began, timebase) / TRIPS;
		out.msgh_id = 2;
		mach_msg(&out, MACH_SEND_MSG, sizeof(out), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		pthread_join(thread, NULL);
	}
	printf("sysbench: system call %.2f us, page fault (%zu KB page) %.1f us, mach message round trip %.1f us, "
	    "%d MB copied in %.1f ms\n", syscall_us, page >> 10, fault_us, trip_us, COPY_BYTES >> 20, copy_ms);
	cachebench(timebase);
	membw(timebase);
}

int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 5, looks = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
	static int pids[MAX_PROCS];
	static struct share shares[MAX_PROCS];
	mach_timebase_info_data_t timebase;
	uint64_t start = mach_absolute_time(), before = start;

	if (seconds == 0) {
		seconds = 5;
	}
	mach_timebase_info(&timebase);
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (unsigned look = 0; looks == 0 || look <= looks; look++) {
		int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)), count = 0, shown = 0;
		uint64_t at = mach_absolute_time(), all = 0;

		for (int i = 0; i < bytes / (int)sizeof(int) && count < MAX_PROCS; i++) {
			struct proc_taskinfo info;

			if (pids[i] <= 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &info, sizeof(info)) != sizeof(info)) {
				continue;
			}
			now[count].pid = pids[i];
			now[count].time = info.pti_total_user + info.pti_total_system;
			count++;
		}
		if (look > 0) {
			int n = 0;

			for (int i = 0; i < count; i++) {
				int found;
				uint64_t then = time_before(now[i].pid, &found);

				if (now[i].time > then) {
					shares[n].pid = now[i].pid;
					shares[n].time = now[i].time - then;
					all += shares[n].time;
					n++;
				}
			}
			qsort(shares, (size_t)n, sizeof(shares[0]), by_time);
			/* the shares as percent of one core over the time between the looks (both in mach_absolute_time's units) */
			double span = (double)(at - before);
			printf("cputop: %llu s: %.0f%% in all:", (unsigned long long)((at - start) * timebase.numer / timebase.denom / 1000000000ull),
			    span > 0 ? 100.0 * (double)all / span : 0);
			for (int i = 0; i < n && shown < SHOWN; i++) {
				char name[64] = "?";
				double percent = span > 0 ? 100.0 * (double)shares[i].time / span : 0;

				if (percent < 1) {
					break;
				}
				proc_name(shares[i].pid, name, sizeof(name));
				printf(" %s[%d] %.0f%%", name, shares[i].pid, percent);
				shown++;
			}
			printf("\n");
		}
		if (look == 56 || look == 58) {
			sysbench(&timebase);
		}
		if (look > 0) {
			vmem_line((at - start) * timebase.numer / timebase.denom / 1000000000ull);
		}
		memcpy(last, now, sizeof(now[0]) * (size_t)count);
		last_count = count;
		before = at;
		if (looks != 0 && look == looks) {
			break;
		}
		sleep(seconds);
	}
	return 0;
}
