/* tools/qemu-init: threads of its own (bsdthread_create), timed, and taking turns under one lock (ulock_wait,
 * ulock_wake): threads, locks */
#include "init.h"

#define SYS_bsdthread_create    360
#define SYS_bsdthread_terminate 361
#define SYS_bsdthread_register  366
#define PTHREAD_START_CUSTOM    0x01000000      /* the stack and the thread's memory are the caller's */
#define THREADS_MAX             8
#define THREAD_STACK            (64 * 1024)

/*
 * Where the kernel starts a thread (bsdthread_register): x0 its memory, x1 its port, x2 the function,
 * x3 the function's argument, sp its stack. The function runs, then the thread ends, giving back the
 * port's right (bsdthread_terminate).
 */
__asm__(".globl _thread_entry\n_thread_entry:\n"
        "\tmov x19, x1\n\tmov x0, x3\n\tblr x2\n"
        "\tmov x0, #0\n\tmov x1, #0\n\tmov x2, x19\n\tmov x3, #0\n\tmov x16, #361\n\tsvc #0x80\n\tbrk #1\n");

void thread_entry(void);

static volatile uint64_t threads_total;

static volatile uint32_t threads_done;

static uint32_t thread_rounds;

/* a share of the work: count, then say so */
static void
thread_work(void *argument)
{
	uint64_t mine = 0;
	(void)argument;

	for (uint32_t i = 0; i < thread_rounds; i++) {
		mine += i & 1;
		__asm__ volatile ("" : "+r"(mine));     /* (not to be folded into one sum) */
	}
	__atomic_fetch_add(&threads_total, mine, __ATOMIC_RELAXED);
	__atomic_fetch_add(&threads_done, 1, __ATOMIC_RELEASE);
}

/* whether this process may start threads: its entry told to the kernel, once (a second time is refused) */
static int
threads_ready(void)
{
	static int registered;

	if (!registered && syscall6(SYS_bsdthread_register, (long)thread_entry, 0, 0, 0, 0, 0) >= 0) {
		registered = 1;         /* (what the kernel answers is its features) */
	}
	return registered;
}

/* COUNT threads doing WORK of ROUNDS each; how long it took; -1 if they did not all end */
static int64_t
threads_run(unsigned count, uint32_t rounds, void (*work)(void *))
{
	static uint8_t stacks[THREADS_MAX][THREAD_STACK] __attribute__((aligned(16)));
	static uint8_t memory[THREADS_MAX][4096] __attribute__((aligned(16)));
	int64_t start = now_us();

	threads_total = 0;
	threads_done = 0;
	thread_rounds = rounds;
	for (unsigned i = 0; i < count; i++) {
		if (syscall6(SYS_bsdthread_create, (long)work, 0, (long)(stacks[i] + THREAD_STACK), (long)memory[i],
		    PTHREAD_START_CUSTOM, 0) < 0) {
			say("threads: a thread does not start\n");
			return -1;
		}
	}
	for (int waited = 0; __atomic_load_n(&threads_done, __ATOMIC_ACQUIRE) < count; waited++) {
		if (waited == 6000) {           /* a minute */
			return -1;
		}
		sleep_us(10000);
	}
	return now_us() - start;
}

/* [COUNT]: COUNT threads (as many as processors, if not said) share some counting; how long it took them */
void
command_threads(int argc, char **argv)
{
	uint32_t processors = 1;
	size_t size = sizeof(processors);
	const uint32_t work = 1u << 28;     /* (half a second for one thread, in QEMU: well above the noise) */

	syscall6(SYS_sysctlbyname, (long)"hw.ncpu", 7, (long)&processors, (long)&size, 0, 0);
	unsigned count = argc > 1 ? parse_number(argv[1]) : processors;
	if (count < 1 || count > THREADS_MAX) {
		say("threads [COUNT]: 1 to 8\n");
		return;
	}
	if (!threads_ready()) {
		say("threads: the kernel takes no threads from this process\n");
		return;
	}
	int64_t alone = threads_run(1, work, thread_work), shared = threads_run(count, work / count, thread_work);
	if (alone < 0 || shared < 0 || threads_total != (uint64_t)(work / count / 2) * count) {
		say("threads: the work was not all done\n");
		return;
	}
	say("threads: ");
	say_number(count);
	say(count == 1 ? " thread on " : " threads on ");
	say_number(processors);
	say(processors == 1 ? " processor did in " : " processors did in ");
	say_number((uint64_t)shared / 1000);
	say(" ms what one does in ");
	say_number((uint64_t)alone / 1000);
	say(" ms\n");
}

/* ---- locks: threads that take turns, as os_unfair_lock makes them, through ulock_wait and ulock_wake ---- */

#define SYS_ulock_wait          515
#define SYS_ulock_wake          516
#define UL_COMPARE_AND_WAIT     1

static volatile uint32_t lock_word;     /* 0 free, 1 held, 2 held and somebody waits */
static uint64_t locked_count;           /* only ever changed with the lock held: not atomic */
static volatile uint32_t lock_waits;

static void
lock_take(void)
{
	uint32_t was = 0;

	if (__atomic_compare_exchange_n(&lock_word, &was, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return;
	}
	while (__atomic_exchange_n(&lock_word, 2, __ATOMIC_ACQUIRE) != 0) {
		/* asleep in the kernel for as long as the word is 2 (what it is when the holder lets go is woken) */
		syscall6(SYS_ulock_wait, UL_COMPARE_AND_WAIT, (long)&lock_word, 2, 0, 0, 0);
		__atomic_fetch_add(&lock_waits, 1, __ATOMIC_RELAXED);
	}
}

static void
lock_give(void)
{
	if (__atomic_exchange_n(&lock_word, 0, __ATOMIC_RELEASE) == 2) {
		syscall6(SYS_ulock_wake, UL_COMPARE_AND_WAIT, (long)&lock_word, 0, 0, 0, 0);
	}
}

static void
locked_work(void *argument)
{
	(void)argument;
	for (uint32_t i = 0; i < thread_rounds; i++) {
		lock_take();
		uint64_t now = locked_count;
		__asm__ volatile ("" ::: "memory");    /* (a window for another thread, were the lock not one) */
		locked_count = now + 1;
		lock_give();
	}
	__atomic_fetch_add(&threads_done, 1, __ATOMIC_RELEASE);
}

/* [COUNT]: COUNT threads (as many as processors, if not said) add to one count under one lock; none lost */
void
command_locks(int argc, char **argv)
{
	uint32_t processors = 1;
	size_t size = sizeof(processors);
	const uint32_t rounds = 200000;

	syscall6(SYS_sysctlbyname, (long)"hw.ncpu", 7, (long)&processors, (long)&size, 0, 0);
	unsigned count = argc > 1 ? parse_number(argv[1]) : (processors > 1 ? processors : 2);
	if (count < 2 || count > THREADS_MAX) {
		say("locks [COUNT]: 2 to 8 threads\n");
		return;
	}
	if (!threads_ready()) {
		say("locks: the kernel takes no threads from this process\n");
		return;
	}
	locked_count = 0;
	lock_waits = 0;
	int64_t took = threads_run(count, rounds, locked_work);
	char line[160];
	size_t at = append(line, 0, sizeof(line), "locks: ");
	at = append_number(line, at, sizeof(line), count);
	at = append_number(line, append(line, at, sizeof(line), " threads added "), sizeof(line), locked_count);
	at = append_number(line, append(line, at, sizeof(line), " of "), sizeof(line), (uint64_t)count * rounds);
	at = append_number(line, append(line, at, sizeof(line), " under one lock, waited in the kernel "), sizeof(line), lock_waits);
	at = append_number(line, append(line, at, sizeof(line), " times, in "), sizeof(line), took < 0 ? 0 : (uint64_t)took / 1000);
	append(line, at, sizeof(line), took < 0 ? " ms: THEY DID NOT ALL END\n" : " ms\n");
	say(line);
}
