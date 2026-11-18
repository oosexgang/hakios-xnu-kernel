/*
 * What the kernel says of a user thread's exception, and of a program's abort() (arch/arm64/kernel/user_exception_trace.c),
 * compiled as it is. The thread's registers are a saved state the test fills, the program's memory an array that
 * copyin() and copyinstr() read, the clock one the test moves, and the console a buffer the test reads.
 *
 *   user_exception_trace_test SCENARIO      one scenario a process
 */
#include "fakekernel.h"
#include "mach/arm/thread_status.h"

typedef int exception_type_t;
typedef int64_t *mach_exception_data_t;
typedef unsigned int mach_msg_type_number_t;
#define EXC_BAD_ACCESS          1
#define EXC_BAD_INSTRUCTION     2
#define EXC_ARITHMETIC          3
#define EXC_SOFTWARE            5
#define EXC_BREAKPOINT          6
#define MACH_VM_MAX_ADDRESS     0x0000000fc0000000ull
#define TUNABLE(type, var, name, value)         type var = value
#define STARTUP(phase, rank, fn)                __attribute__((unused)) static void (*const startup_##fn)(void) = fn

/* tasks and threads: one of each, the one that trapped, and an empty list of all tasks */
typedef struct queue_entry {
	struct queue_entry *next, *prev;
} queue_head_t, queue_chain_t;
#define queue_iterate(head, elt, type, field) \
	for ((elt) = (type)(void *)(head)->next; (void *)(elt) != (void *)(head); (elt) = (type)(void *)(elt)->field.next)
typedef struct fake_map *vm_map_t;
#define VM_MAP_NULL             ((vm_map_t)NULL)
typedef void *vm_map_switch_context_t;
struct fake_task {
	queue_head_t threads;
	queue_chain_t tasks;
	vm_map_t map;
};
typedef struct fake_task *task_t;
struct fake_thread {
	queue_chain_t task_threads;
	int state;
};
#define TH_RUN                  0x04
static struct fake_task the_task, kernel_task_itself;
static struct fake_thread the_thread;
static arm_saved_state_t the_state;
static char the_proc;
static task_t kernel_task = &kernel_task_itself;
#define current_thread()        (&the_thread)
static task_t get_threadtask(thread_t thread) { return thread == &the_thread ? &the_task : NULL; }
static const arm_saved_state_t *get_user_regs(thread_t thread) { return thread == &the_thread ? &the_state : NULL; }
static struct proc *get_bsdtask_info(task_t task) { return task == &the_task ? (struct proc *)(void *)&the_proc : NULL; }
char *proc_name_address(struct proc *p) { (void)p; return "crashy"; }
static void task_lock(task_t task) { (void)task; }
static void task_unlock(task_t task) { (void)task; }
static void task_reference(task_t task) { (void)task; }
static void task_deallocate(task_t task) { (void)task; }
static uint64_t thread_tid(thread_t thread) { (void)thread; return 1; }
static void *get_bsdthread_info(thread_t thread) { (void)thread; return NULL; }
void bsd_getthreadname(void *uth, char *buffer) { (void)uth; buffer[0] = 0; }
static void vm_map_reference(vm_map_t map) { (void)map; }
static void vm_map_deallocate(vm_map_t map) { (void)map; }
static vm_map_switch_context_t vm_map_switch_to(vm_map_t map) { (void)map; return NULL; }
static void vm_map_switch_back(vm_map_switch_context_t old) { (void)old; }
typedef struct vm_shared_region {
	uint32_t sr_slide;
} *vm_shared_region_t;
static struct vm_shared_region the_region = { 0x1234000 };
static vm_shared_region_t vm_shared_region_get(task_t task) { return task == &the_task ? &the_region : NULL; }
static void vm_shared_region_deallocate(vm_shared_region_t region) { (void)region; }
struct recount_times_mach {
	uint64_t rtm_user, rtm_system;
};
static struct recount_times_mach recount_task_times(task_t task) { (void)task; return (struct recount_times_mach){ 0, 0 }; }
queue_head_t tasks = { &tasks, &tasks };
lck_mtx_t tasks_threads_lock = { PTHREAD_MUTEX_INITIALIZER };
static void thread_call_enter(thread_call_t call) { (void)call; }
static void clock_get_system_microtime(clock_sec_t *secs, clock_usec_t *usecs) { *secs = 0, *usecs = 0; }

static size_t
test_strlcpy(char *to, const char *from, size_t size)
{
	size_t length = strlen(from);

	if (size != 0) {
		size_t n = length < size - 1 ? length : size - 1;

		memcpy(to, from, n);
		to[n] = 0;
	}
	return length;
}
#define strlcpy test_strlcpy

static char *
strnstr(const char *haystack, const char *needle, size_t length)
{
	size_t n = strlen(needle);

	for (size_t i = 0; i + n <= length && haystack[i] != 0; i++) {
		if (strncmp(haystack + i, needle, n) == 0) {
			return (char *)(uintptr_t)(haystack + i);
		}
	}
	return NULL;
}

/* the clock (absolute time is nanoseconds here) */
static uint64_t clock_now = 5 * NSEC_PER_SEC;
#define mach_absolute_time()    (clock_now)

/* the program's memory */
#define USER_BASE               0x100000000ull
static uint8_t user_mem[4096];

static bool
in_user(user_addr_t address, size_t length)
{
	return address >= USER_BASE && address - USER_BASE <= sizeof(user_mem) && length <= sizeof(user_mem) - (address - USER_BASE);
}

static int
user_copyin(user_addr_t from, void *to, size_t length)
{
	if (!in_user(from, length)) {
		return EFAULT;
	}
	memcpy(to, &user_mem[from - USER_BASE], length);
	return 0;
}

static int
copyinstr(user_addr_t from, char *to, vm_size_t max, vm_size_t *done)
{
	for (vm_size_t i = 0; i < max; i++) {
		if (!in_user(from + i, 1)) {
			*done = i;
			return EFAULT;
		}
		to[i] = (char)user_mem[from + i - USER_BASE];
		if (to[i] == 0) {
			*done = i + 1;
			return 0;
		}
	}
	*done = max;
	return ENAMETOOLONG;
}

/* the console */
static char console[1 << 20];
static size_t console_length;

static void __printflike(1, 2)
console_printf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	int n = vsnprintf(console + console_length, sizeof(console) - console_length, format, args);
	va_end(args);
	if (n > 0) {
		console_length += (size_t)n < sizeof(console) - console_length ? (size_t)n : sizeof(console) - console_length - 1;
	}
}

#define copyin  user_copyin
#define printf  console_printf
/* (a uint64_t is an unsigned long long in the kernel, which prints it so, and an unsigned long here) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#include "../arch/arm64/kernel/user_exception_trace.c"
#pragma GCC diagnostic pop
#undef printf
#undef copyin

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* how many lines of the console begin with PREFIX */
static unsigned
lines(const char *prefix)
{
	unsigned count = 0;

	for (const char *line = console; *line != 0;) {
		const char *end = strchr(line, '\n');

		if (strncmp(line, prefix, strlen(prefix)) == 0) {
			count++;
		}
		if (end == NULL) {
			break;
		}
		line = end + 1;
	}
	return count;
}

/* a thread at pc 0x100004000 with three frames above it, x0 pointing at TEXT (if any), and systrace=1 */
static void
thread_with(const char *text)
{
	static const uint64_t frames[] = { USER_BASE + 0x210, 0x10000a000, USER_BASE + 0x220, 0x10000b000, 0, 0x10000c000 };

	memset(&the_state, 0, sizeof(the_state));
	memcpy(&user_mem[0x200], frames, sizeof(frames));
	the_state.pc = 0x100004000;
	the_state.x[30] = 0x100004100;
	the_state.sp = USER_BASE + 0x1f0;
	the_state.x[29] = USER_BASE + 0x200;
	if (text != NULL) {
		strcpy((char *)&user_mem[0x400], text);
		the_state.x[0] = USER_BASE + 0x400;
	}
	user_exception_trace_enabled = 1;
	fake_pid = 321;
	console_length = 0;
	console[0] = 0;
}

static void
exception(exception_type_t type)
{
	int64_t code[2] = { 1, 0x10 };

	user_exception_trace(type, code, 2, &the_thread);
}

/* an exception described as before: who, what, the registers, the slide, the text x0 points at, the frames */
static void
described(void)
{
	thread_with("Assertion failed: (x != 0)");
	exception(EXC_BAD_ACCESS);
	CHECK(strstr(console, "usertrap: crashy[321] exception 1 code 0x1 0x10: pc 0x100004000 lr 0x100004100 sp 0x1000001f0, "
	    "shared cache slide 0x1234000\n") != NULL, "%s", console);
	CHECK(strstr(console, "usertrap:   x0 -> \"Assertion failed: (x != 0)\"\n") != NULL, "%s", console);
	CHECK(lines("usertrap:   called from 0x10000") == 3, "%s", console);

	/* not a fatal-looking one, or systrace not set, or another thread's: nothing */
	thread_with(NULL);
	exception(EXC_SOFTWARE);
	user_exception_trace(EXC_BAD_ACCESS, NULL, 0, NULL);
	user_exception_trace_enabled = 0;
	exception(EXC_BAD_ACCESS);
	CHECK(console_length == 0, "%s", console);
}

/*
 * The text a register points at is the program's: a newline in it would make a line of the console's that looks like
 * the kernel's, an escape sequence would work the terminal. It is cut before the first character that is not printable.
 */
static void
text_cut_at_control(void)
{
	thread_with("Assertion failed\nusertrap:   called from 0x4141414141\033[2J");
	exception(EXC_BREAKPOINT);
	CHECK(strstr(console, "usertrap:   x0 -> \"Assertion failed\"\n") != NULL, "%s", console);
	CHECK(strstr(console, "0x4141414141") == NULL && strchr(console, '\033') == NULL, "%s", console);

	thread_with("All is well\033]0;owned\007");
	exception(EXC_BREAKPOINT);
	CHECK(strstr(console, "usertrap:   x0 -> \"All is well\"\n") != NULL && strchr(console, '\033') == NULL, "%s", console);

	thread_with("coffee\xc3\xa9 au lait");
	exception(EXC_BREAKPOINT);
	CHECK(strstr(console, "usertrap:   x0 -> \"coffee\"\n") != NULL, "%s", console);
}

/*
 * A program that takes exceptions in a loop (a SIGTRAP handler that returns, here a thousand in a second): ten
 * described, and after ten seconds the next one is, with how many were not.
 */
static void
exceptions_limited(void)
{
	thread_with(NULL);
	for (unsigned i = 0; i < 1000; i++) {
		exception(EXC_BREAKPOINT);
		clock_now += NSEC_PER_MSEC;
	}
	CHECK(lines("usertrap: crashy[321]") == 10, "%u described", lines("usertrap: crashy[321]"));
	CHECK(console_length < 4096, "%zu bytes on the console", console_length);
	clock_now += 10 * NSEC_PER_SEC;
	exception(EXC_BAD_ACCESS);
	CHECK(lines("usertrap: crashy[321]") == 11, "%u described", lines("usertrap: crashy[321]"));
	CHECK(strstr(console, "usertrap: 990 more exceptions and aborts were not described") != NULL, "%s", console + console_length / 2);
}

/* abort() with SIGABRT ignored, in a loop: the same, and it does not need systrace */
static void
aborts_limited(void)
{
	thread_with(NULL);
	user_exception_trace_enabled = 0;
	user_abort_trace();
	CHECK(strstr(console, "userabort: crashy[321] aborts, shared cache slide 0x1234000; called from: 0x10000a000 0x10000b000 "
	    "0x10000c000\n") != NULL, "%s", console);
	for (unsigned i = 0; i < 5000; i++) {
		user_abort_trace();
		clock_now += NSEC_PER_MSEC / 2;
	}
	CHECK(lines("userabort:") == 10, "%u described", lines("userabort:"));
	clock_now += 10 * NSEC_PER_SEC;
	user_abort_trace();
	CHECK(lines("userabort:") == 11, "%u described", lines("userabort:"));
	CHECK(strstr(console, "usertrap: 4991 more exceptions and aborts were not described") != NULL, "%s", console);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "described", described }, { "text_cut_at_control", text_cut_at_control },
		{ "exceptions_limited", exceptions_limited }, { "aborts_limited", aborts_limited },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
