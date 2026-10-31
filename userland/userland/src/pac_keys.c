/*
 * arm64e bring-up check for a hypervisor that may not keep EL1's key writes: do a process's pointer
 * authentication keys stay the same for its whole life? Runs as process 1 (static, no libc, like
 * hello.c). Signs fixed values with the IA, IB, DA and GA keys once, then signs them again after
 * system calls and after long stretches of plain computation (timer interrupts, the hypervisor's
 * own exits) and counts every result that differs from the first. All zeros: the keys are stable.
 *
 * Build: xnu-clang -arch arm64e -Os -static -nostdlib -ffreestanding -fno-stack-protector \
 *          -fno-builtin -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o pac_keys src/pac_keys.c
 */
typedef unsigned long u64;

static long
syscall3(long num, long a, long b, long c)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2) : "memory", "cc");
	return x0;
}

#define SYS_exit    1
#define SYS_write   4
#define SYS_open    5
#define SYS_getpid  20
#define O_RDWR      2

static int console;

static void
put(const char *s)
{
	unsigned n = 0;
	while (s[n]) {
		n++;
	}
	syscall3(SYS_write, console, (long)s, n);
}

static void
put_hex(u64 v)
{
	char buf[19];
	buf[0] = '0';
	buf[1] = 'x';
	for (int i = 0; i < 16; i++) {
		buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	}
	buf[18] = 0;
	put(buf);
}

static void
put_number(u64 v)
{
	char buf[24];
	int i = sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put(buf + i);
}

struct sig { u64 ia, ib, da, ga; };

static void
sign(struct sig *s)
{
	u64 p = 0x0000000100004000UL, m = 0x1234;
	u64 ia = p, ib = p, da = p, ga;
	__asm__ volatile ("pacia %0, %1" : "+r" (ia) : "r" (m));
	__asm__ volatile ("pacib %0, %1" : "+r" (ib) : "r" (m));
	__asm__ volatile ("pacda %0, %1" : "+r" (da) : "r" (m));
	__asm__ volatile ("pacga %0, %1, %2" : "=r" (ga) : "r" (p), "r" (m));
	s->ia = ia; s->ib = ib; s->da = da; s->ga = ga;
}

__asm__(
	"	.text\n"
	"	.globl	_start\n"
	"	.p2align 2\n"
	"_start:\n"
	"	mov	x9, sp\n"
	"	and	sp, x9, #~15\n"
	"	b	_pac_keys_main\n");

void pac_keys_main(void);

void
pac_keys_main(void)
{
	struct sig first, now;
	u64 bad_ia = 0, bad_ib = 0, bad_da = 0, bad_ga = 0, rounds = 0;
	volatile u64 sink = 0;

	console = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);
	put("\npac_keys: process ");
	put_number(syscall3(SYS_getpid, 0, 0, 0));
	put(" checking its own PAC keys\n");
	sign(&first);
	put("pac_keys: first  ia "); put_hex(first.ia); put(" ib "); put_hex(first.ib);
	put(" da "); put_hex(first.da); put(" ga "); put_hex(first.ga); put("\n");

	for (int phase = 0; phase < 6; phase++) {
		/* half the rounds right after a system call, half after a long stretch of computation */
		for (int i = 0; i < 20000; i++) {
			syscall3(SYS_getpid, 0, 0, 0);
			sign(&now);
			bad_ia += now.ia != first.ia; bad_ib += now.ib != first.ib;
			bad_da += now.da != first.da; bad_ga += now.ga != first.ga;
			rounds++;
		}
		for (int i = 0; i < 200; i++) {
			for (u64 k = 0; k < 200000; k++) {
				sink += k;
			}
			sign(&now);
			bad_ia += now.ia != first.ia; bad_ib += now.ib != first.ib;
			bad_da += now.da != first.da; bad_ga += now.ga != first.ga;
			rounds++;
		}
		put("pac_keys: phase "); put_number(phase); put(": rounds "); put_number(rounds);
		put(", differing ia "); put_number(bad_ia); put(" ib "); put_number(bad_ib);
		put(" da "); put_number(bad_da); put(" ga "); put_number(bad_ga);
		put(bad_ia | bad_ib | bad_da | bad_ga ? "  KEYS CHANGED\n" : "  stable\n");
		if (bad_ia | bad_ib | bad_da | bad_ga) {
			put("pac_keys: now    ia "); put_hex(now.ia); put(" ib "); put_hex(now.ib);
			put(" da "); put_hex(now.da); put(" ga "); put_hex(now.ga); put("\n");
		}
	}
	put("pac_keys: done\n");
	syscall3(SYS_exit, 0, 0, 0);
	for (;;) {
	}
}
