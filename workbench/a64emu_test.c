/*
 * The emulator of the arm64e instructions an ARMv8.2 core lacks (arch/arm64/kernel/a64emu.c), compiled as it is and
 * handed a trapped thread's registers as the exception vectors save them. The program's memory is an array here,
 * and copyin()/copyout() are models of the kernel's: under 16 bytes they go a byte at a time, as _bcopyin and
 * _bcopyout do, so another thread can store (or look) between two of the bytes; copy{in,out}_atomic{32,64} are one
 * access, and refuse an address that is not a multiple of their size. Built for aarch64 and run under qemu-aarch64,
 * the emulator is also compared with the processor's own FJCVTZS and LDAPURS*.
 *
 *   a64emu_test SCENARIO            one scenario a process
 */
#include <math.h>

#include "fakekernel.h"
#include "mach/arm/thread_status.h"

typedef struct fake_task *task_t;
static task_t current_task(void) { return NULL; }
static int task_pid(task_t task) { (void)task; return 42; }
#define os_atomic_inc_orig(pointer, order)      __atomic_fetch_add((pointer), 1, __ATOMIC_RELAXED)
#define __builtin_arm_dmb(what)                 ((void)(what), __sync_synchronize())

/* ---- the program's memory ---------------------------------------------------------------------------------------- */

#define USER_BASE       0x100000000ull
static uint8_t user_mem[4096] __attribute__((aligned(16)));

static bool
in_user(user_addr_t address, size_t length)
{
	return address >= USER_BASE && address - USER_BASE <= sizeof(user_mem) && length <= sizeof(user_mem) - (address - USER_BASE);
}

static uint64_t
peek64(user_addr_t address)
{
	uint64_t value;

	memcpy(&value, &user_mem[address - USER_BASE], sizeof(value));
	return value;
}

static void
poke64(user_addr_t address, uint64_t value)
{
	memcpy(&user_mem[address - USER_BASE], &value, sizeof(value));
}

static void
poke32(user_addr_t address, uint32_t value)
{
	memcpy(&user_mem[address - USER_BASE], &value, sizeof(value));
}

/*
 * Another thread of the program. It stores other_value (other_size bytes) at other_at once a copy over that place has
 * gone other_after bytes into it; and it looks at the 8 bytes at watch_at after every byte a copy writes there.
 */
static user_addr_t other_at, watch_at;
static uint64_t other_value;
static unsigned other_size, other_after;
static uint64_t seen[16];
static unsigned nseen, atomic_accesses;

static void
other_thread(user_addr_t from, size_t done, bool writing)
{
	if (other_at != 0 && other_at >= from && other_at < from + 8 && done == other_after) {
		memcpy(&user_mem[other_at - USER_BASE], &other_value, other_size);
		other_at = 0;
	}
	if (writing && watch_at != 0 && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen++] = peek64(watch_at);
	}
}

static int
user_copyin(user_addr_t from, void *to, size_t length)
{
	if (!in_user(from, length)) {
		return EFAULT;
	}
	for (size_t i = 0; i < length; i++) {
		((uint8_t *)to)[i] = user_mem[from - USER_BASE + i];
		other_thread(from, i + 1, false);
	}
	return 0;
}

static int
user_copyout(const void *from, user_addr_t to, size_t length)
{
	if (!in_user(to, length)) {
		return EFAULT;
	}
	for (size_t i = 0; i < length; i++) {
		user_mem[to - USER_BASE + i] = ((const uint8_t *)from)[i];
		other_thread(to, i + 1, true);
	}
	return 0;
}

/* one access: the other thread's store comes after it, whole */
static int
atomic_access(user_addr_t address, void *kernel, unsigned size, bool writing)
{
	if (address & (size - 1)) {
		return EINVAL;
	}
	if (!in_user(address, size)) {
		return EFAULT;
	}
	atomic_accesses++;
	if (writing) {
		memcpy(&user_mem[address - USER_BASE], kernel, size);
	} else {
		memcpy(kernel, &user_mem[address - USER_BASE], size);
	}
	other_thread(address, other_after, writing);
	return 0;
}

static int copyin_atomic32(user_addr_t from, uint32_t *to) { return atomic_access(from, to, 4, false); }
static int copyin_atomic64(user_addr_t from, uint64_t *to) { return atomic_access(from, to, 8, false); }
static int copyout_atomic32(uint32_t value, user_addr_t to) { return atomic_access(to, &value, 4, true); }
static int copyout_atomic64(uint64_t value, user_addr_t to) { return atomic_access(to, &value, 8, true); }

/* (what the emulator logs -- the first few of each kind -- is only counted) */
static unsigned emu_lines;

static void __printflike(1, 2)
emu_log(const char *format, ...)
{
	(void)format;
	emu_lines++;
}

#define copyin  user_copyin
#define copyout user_copyout
#define printf  emu_log
#include "../arch/arm64/kernel/a64emu.c"
#undef printf
#undef copyin
#undef copyout

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- a trapped thread --------------------------------------------------------------------------------------------- */

#define CODE    (USER_BASE + 0x100)     /* where the instruction is */
#define DATA    (USER_BASE + 0x800)     /* x0 points here */

static arm_context_t context;

/* the thread trapped on INSTR at CODE, x0 = DATA, every other register its number times 0x1111; true if emulated */
static void
thread_at(uint32_t instr)
{
	memset(&context, 0, sizeof(context));
	for (unsigned n = 0; n < 31; n++) {
		context.ss.x[n] = n * 0x1111ull;
	}
	context.ss.x[0] = DATA;
	context.ss.sp = DATA + 0x100;
	context.ss.pc = CODE;
	context.ss.cpsr = 0;            /* EL0, AArch64 */
	poke32(CODE, instr);
	other_at = watch_at = 0;
	nseen = atomic_accesses = 0;
}

static bool
trap(void)
{
	return hakios_a64emu(&context);
}

#define X(n)    (context.ss.x[n])
#define PC      (context.ss.pc)

/* ---- scenarios ---------------------------------------------------------------------------------------------------- */

/*
 * LDAPUR X1, [X0, #8] of a value another thread replaces meanwhile: the old one or the new one, never half of each.
 * (copyin() went a byte at a time: the other thread's store between the fourth and the fifth byte made 0x22222222_11111111.)
 * The same of LDAPUR W1 (4 bytes) and LDAPURH W1 (2 bytes, read in the word they are in).
 */
static void
acquire_load_whole(void)
{
	thread_at(0xd9408001);                  /* LDAPUR X1, [X0, #8] */
	poke64(DATA + 8, 0x1111111111111111ull);
	other_at = DATA + 8, other_value = 0x2222222222222222ull, other_size = 8, other_after = 4;
	CHECK(trap() && PC == CODE + 4, "emulated");
	CHECK(X(1) == 0x1111111111111111ull || X(1) == 0x2222222222222222ull, "x1 0x%llx", (unsigned long long)X(1));

	thread_at(0x99404001);                  /* LDAPUR W1, [X0, #4] */
	poke64(DATA, 0x3333333300000000ull);
	other_at = DATA + 4, other_value = 0x44444444, other_size = 4, other_after = 2;
	CHECK(trap() && PC == CODE + 4, "emulated");
	CHECK(X(1) == 0x33333333 || X(1) == 0x44444444, "w1 0x%llx", (unsigned long long)X(1));

	thread_at(0x59402001);                  /* LDAPURH W1, [X0, #2] */
	poke64(DATA, 0x00000000aaaa5555ull);
	other_at = DATA + 2, other_value = 0xbbbb, other_size = 2, other_after = 1;
	CHECK(trap() && PC == CODE + 4, "emulated");
	CHECK(X(1) == 0xaaaa || X(1) == 0xbbbb, "w1 0x%llx", (unsigned long long)X(1));
}

/* LDRAA X1, [X0, #8] the same: an LDR of 64 bits, one access */
static void
ldraa_load_whole(void)
{
	thread_at(0xf8201401);                  /* LDRAA X1, [X0, #8] */
	poke64(DATA + 8, 0x1111111111111111ull);
	other_at = DATA + 8, other_value = 0x2222222222222222ull, other_size = 8, other_after = 4;
	CHECK(trap() && PC == CODE + 4, "emulated");
	CHECK(X(1) == 0x1111111111111111ull || X(1) == 0x2222222222222222ull, "x1 0x%llx", (unsigned long long)X(1));
}

/* STLUR X1, [X0] and STLUR W1, [X0]: another thread looking at the place sees the old value or the new, never a mix */
static void
release_store_whole(void)
{
	thread_at(0xd9000001);                  /* STLUR X1, [X0] */
	X(1) = 0x2222222222222222ull;
	poke64(DATA, 0x1111111111111111ull);
	watch_at = DATA;
	CHECK(trap() && PC == CODE + 4, "emulated");
	CHECK(peek64(DATA) == 0x2222222222222222ull, "stored 0x%llx", (unsigned long long)peek64(DATA));
	for (unsigned i = 0; i < nseen; i++) {
		CHECK(seen[i] == 0x1111111111111111ull || seen[i] == 0x2222222222222222ull, "seen 0x%llx", (unsigned long long)seen[i]);
	}

	thread_at(0x99000001);                  /* STLUR W1, [X0] */
	X(1) = 0xffffffff22222222ull;
	poke64(DATA, 0x5555555511111111ull);
	watch_at = DATA;
	CHECK(trap() && PC == CODE + 4, "emulated");
	CHECK(peek64(DATA) == 0x5555555522222222ull, "stored 0x%llx", (unsigned long long)peek64(DATA));
	for (unsigned i = 0; i < nseen; i++) {
		CHECK(seen[i] == 0x5555555511111111ull || seen[i] == 0x5555555522222222ull, "seen 0x%llx", (unsigned long long)seen[i]);
	}
}

/* unaligned, the copy is still made (a plain one), and a bad address is the undefined instruction, nothing changed */
static void
unaligned_and_bad(void)
{
	thread_at(0xd9401001);                  /* LDAPUR X1, [X0, #1] */
	poke64(DATA + 1, 0x0123456789abcdefull);
	CHECK(trap() && X(1) == 0x0123456789abcdefull && atomic_accesses == 0, "x1 0x%llx", (unsigned long long)X(1));

	thread_at(0xd9000001);                  /* STLUR X1, [X0] */
	X(0) = 0x1000;
	CHECK(!trap() && PC == CODE && X(0) == 0x1000, "not emulated");

	thread_at(0xf8201c01);                  /* LDRAA X1, [X0, #8]! */
	X(0) = USER_BASE + sizeof(user_mem);
	CHECK(!trap() && PC == CODE && X(0) == USER_BASE + sizeof(user_mem) && X(1) == 0x1111, "not emulated, no writeback");
}

/*
 * PACIZA..AUTDZB (opcode 8 to 15) exist only with Rn 31; with another register they are unallocated, and stay the
 * undefined instruction (pacstrip leaves them so too). The forms with a modifier, and XPACI/XPACD, are nothing.
 */
static void
pac_unallocated_left(void)
{
	thread_at(0xdac12020);                  /* PACIZA X0 with Rn 1: unallocated */
	CHECK(!trap() && PC == CODE, "PACIZA with Rn 1 emulated");
	thread_at(0xdac13c20);                  /* AUTDZB X0 with Rn 1: unallocated */
	CHECK(!trap() && PC == CODE, "AUTDZB with Rn 1 emulated");
	thread_at(0xdac123e0);                  /* PACIZA X0 */
	CHECK(trap() && PC == CODE + 4 && X(0) == DATA, "PACIZA");
	thread_at(0xdac10020);                  /* PACIA X0, X1 */
	CHECK(trap() && PC == CODE + 4 && X(0) == DATA, "PACIA");
	thread_at(0xdac11c20);                  /* AUTDB X0, X1 */
	CHECK(trap() && PC == CODE + 4 && X(0) == DATA, "AUTDB");
	thread_at(0xdac143e0);                  /* XPACI X0 */
	CHECK(trap() && PC == CODE + 4 && X(0) == DATA, "XPACI");
}

/* what each instruction does, as before */
static void
each_instruction(void)
{
	thread_at(0xd65f0bff);                  /* RETAA */
	CHECK(trap() && PC == 30 * 0x1111ull, "RETAA: pc 0x%llx", (unsigned long long)PC);
	thread_at(0xd71f0843);                  /* BRAA X2, X3 */
	CHECK(trap() && PC == 2 * 0x1111ull && X(30) == 30 * 0x1111ull, "BRAA");
	thread_at(0xd73f0843);                  /* BLRAA X2, X3 */
	CHECK(trap() && PC == 2 * 0x1111ull && X(30) == CODE + 4, "BLRAA");
	thread_at(0xd63f0bdf);                  /* BLRAAZ X30: the target read before the link is written */
	CHECK(trap() && PC == 30 * 0x1111ull && X(30) == CODE + 4, "BLRAAZ X30");
	thread_at(0x9ac23020);                  /* PACGA X0, X1, X2 */
	CHECK(trap() && X(0) == (0x1111ull ^ 0x2222ull) && PC == CODE + 4, "PACGA");
	thread_at(0x9adf33e3);                  /* PACGA X3, XZR, XZR (register 31 the zero register, as pacstrip's EOR) */
	CHECK(trap() && X(3) == 0, "PACGA x31");

	thread_at(0xf87ffc01);                  /* LDRAA X1, [X0, #-8]! */
	poke64(DATA - 8, 0xfeedfacecafebeefull);
	CHECK(trap() && X(1) == 0xfeedfacecafebeefull && X(0) == DATA - 8 && PC == CODE + 4, "LDRAA pre-index");
	thread_at(0xf8a017e1);                  /* LDRAB X1, [SP, #8] */
	poke64(DATA + 0x108, 0x77);
	CHECK(trap() && X(1) == 0x77 && context.ss.sp == DATA + 0x100, "LDRAB from sp");

	poke64(DATA, 0x00000000000080f0ull);
	thread_at(0x19800001);                  /* LDAPURSB X1, [X0] */
	CHECK(trap() && X(1) == 0xfffffffffffffff0ull, "LDAPURSB X: 0x%llx", (unsigned long long)X(1));
	thread_at(0x19c00001);                  /* LDAPURSB W1, [X0] */
	CHECK(trap() && X(1) == 0xfffffff0ull, "LDAPURSB W: 0x%llx", (unsigned long long)X(1));
	thread_at(0x59800001);                  /* LDAPURSH X1, [X0] */
	CHECK(trap() && X(1) == 0xffffffffffff80f0ull, "LDAPURSH X: 0x%llx", (unsigned long long)X(1));
	poke64(DATA, 0x0000000080000000ull);
	thread_at(0x99800001);                  /* LDAPURSW X1, [X0] */
	CHECK(trap() && X(1) == 0xffffffff80000000ull, "LDAPURSW: 0x%llx", (unsigned long long)X(1));
	thread_at(0x99c00001);                  /* LDAPURSW to a W register: unallocated */
	CHECK(!trap() && PC == CODE, "unallocated");
	thread_at(0x190003e1);                  /* STLURB W1, [SP] */
	poke64(DATA + 0x100, 0);
	CHECK(trap() && peek64(DATA + 0x100) == 0x11, "STLURB");

	thread_at(0x1e7e0020);                  /* FJCVTZS W0, D1 */
	double d = -3.75;
	memcpy(&context.ns.ns_64.v.q[1], &d, sizeof(d));
	context.ss.cpsr = 0xf0000000;
	CHECK(trap() && X(0) == 0xfffffffdull && context.ss.cpsr == 0, "FJCVTZS -3.75: 0x%llx, cpsr 0x%x", (unsigned long long)X(0),
	    context.ss.cpsr);
	thread_at(0x1e7e0020);
	d = 7.0;
	memcpy(&context.ns.ns_64.v.q[1], &d, sizeof(d));
	CHECK(trap() && X(0) == 7 && context.ss.cpsr == 0x40000000, "FJCVTZS 7.0: Z");

	thread_at(0xd50330ff);                  /* SB */
	CHECK(trap() && PC == CODE + 4, "SB");
	thread_at(0xd503201f);                  /* NOP: not ours */
	CHECK(!trap() && PC == CODE, "NOP left");
	thread_at(0xd65f0bff);
	context.ss.cpsr = PSR64_MODE_RW_32;     /* an AArch32 thread */
	CHECK(!trap(), "AArch32 left");

	/* the first three of a kind are logged, then the thousandth: not one line an instruction */
	emu_lines = 0;
	memset(emu_count, 0, sizeof(emu_count));
	for (unsigned i = 0; i < 2000; i++) {
		thread_at(0xd50330ff);
		trap();
	}
	CHECK(emu_lines == 4, "%u lines for 2000 SB", emu_lines);
}

#if defined(__aarch64__)
/* the processor's FJCVTZS W0, D1 (qemu-aarch64's has JSCVT): the result and Z */
static uint32_t
fjcvtzs(double value, bool *z)
{
	register double in __asm__("d1") = value;
	register uint64_t out __asm__("x0");
	uint64_t nzcv;

	__asm__ volatile (".inst 0x1e7e0020\n\tmrs %1, nzcv" : "=r" (out), "=&r" (nzcv) : "w" (in) : "cc");
	*z = (nzcv & 0x40000000) != 0;
	return (uint32_t)out;
}

/* the emulator's FJCVTZS and LDAPURS* against the processor's, value by value */
static void
against_the_processor(void)
{
	static const double some[] = {
		0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5, 3.0, -3.75, 2147483647.0, 2147483647.5, 2147483648.0, -2147483648.0,
		-2147483648.5, -2147483649.0, 4294967296.0, 4294967301.0, -4294967301.0, 1e20, -1e20, 4503599627370497.0,
		9007199254740993.0, 1.8446744073709552e19, 1e300, -1e300, 5e-324, -5e-324, 2.2250738585072014e-308,
	};
	uint64_t state = 0x9e3779b97f4a7c15ull;

	for (unsigned i = 0; i < sizeof(some) / sizeof(some[0]) + 4000; i++) {
		double value;
		uint64_t bits;
		bool z;

		if (i < sizeof(some) / sizeof(some[0])) {
			value = some[i];
		} else if (i < sizeof(some) / sizeof(some[0]) + 4) {
			value = (double[]){ INFINITY, -INFINITY, NAN, -NAN }[i - sizeof(some) / sizeof(some[0])];
		} else {
			state = state * 6364136223846793005ull + 1442695040888963407ull;
			/* exponents around the int32 range mostly, and every bit of the fraction */
			bits = (state & 0x800fffffffffffffull) | (uint64_t)(1023 + (int)((state >> 52) % 90) - 20) << 52;
			memcpy(&value, &bits, sizeof(value));
		}
		uint32_t expected = fjcvtzs(value, &z);

		thread_at(0x1e7e0020);
		memcpy(&context.ns.ns_64.v.q[1], &value, sizeof(value));
		CHECK(trap() && X(0) == expected && ((context.ss.cpsr & 0x40000000) != 0) == z, "FJCVTZS %a: 0x%llx Z %d, the processor's 0x%x Z %d",
		    value, (unsigned long long)X(0), (context.ss.cpsr & 0x40000000) != 0, expected, z);
	}
	for (unsigned byte = 0; byte < 256; byte++) {
		static const uint32_t forms[] = { 0x19800001, 0x19c00001, 0x59800001, 0x59c00001, 0x99800001 };
		uint64_t word = 0x0102030405060700ull | byte | (uint64_t)byte << 24;

		for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
			uint64_t expected;
			register uint64_t base __asm__("x0") = (uint64_t)(uintptr_t)&word;
			register uint64_t out __asm__("x1");

			/* the same instruction on the processor: base in x0, result in x1 */
			switch (f) {
			case 0: __asm__ volatile (".inst 0x19800001" : "=r" (out) : "r" (base), "m" (word)); break;
			case 1: __asm__ volatile (".inst 0x19c00001" : "=r" (out) : "r" (base), "m" (word)); break;
			case 2: __asm__ volatile (".inst 0x59800001" : "=r" (out) : "r" (base), "m" (word)); break;
			case 3: __asm__ volatile (".inst 0x59c00001" : "=r" (out) : "r" (base), "m" (word)); break;
			default: __asm__ volatile (".inst 0x99800001" : "=r" (out) : "r" (base), "m" (word)); break;
			}
			expected = out;
			thread_at(forms[f]);
			poke64(DATA, word);
			CHECK(trap() && X(1) == expected, "0x%08x of 0x%llx: 0x%llx, the processor's 0x%llx", forms[f], (unsigned long long)word,
			    (unsigned long long)X(1), (unsigned long long)expected);
		}
	}
}
#endif

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "acquire_load_whole", acquire_load_whole }, { "ldraa_load_whole", ldraa_load_whole },
		{ "release_store_whole", release_store_whole }, { "unaligned_and_bad", unaligned_and_bad },
		{ "pac_unallocated_left", pac_unallocated_left }, { "each_instruction", each_instruction },
#if defined(__aarch64__)
		{ "against_the_processor", against_the_processor },
#endif
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
