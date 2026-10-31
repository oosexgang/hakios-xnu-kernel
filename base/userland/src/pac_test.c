/*
 * arm64e bring-up check: are return addresses really signed?
 *
 * A static arm64e Mach-O with no libc, like hello.c, that runs as process 1 through the kernel's
 * mockfs.  It prints a line, then signs its own return address with pacibsp, bends it by 8 and
 * retabs to it.  With pointer authentication enforced the retab raises the PAC failure exception
 * and the kernel kills the process (SIGBUS at the retab); if the line after the call is printed
 * instead, return-address signing is not in effect.
 *
 * The corruption is written in assembly because -Os drops the frame pointer, so reaching for a
 * saved x30 through x29 would touch whatever the compiler left in that register.
 *
 * Build: xnu-clang -arch arm64e -Os -static -nostdlib -ffreestanding -fno-stack-protector \
 *          -fno-builtin -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o pac_test src/pac_test.c
 * Run:   xnuloader/mkpayload.py -o qemu/payload.bin kernel=... ramdisk=userland/pac_test
 */
typedef unsigned long u64;

/* XNU arm64 system call: number in x16, arguments in x0.., "svc #0x80"; carry set on error */
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

static long
syscall1(long num, long a)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16) : "memory", "cc");
	return x0;
}

#define SYS_exit    1
#define SYS_write   4
#define SYS_open    5
#define SYS_getpid  20
#define O_RDWR      2

static unsigned
length(const char *s)
{
	unsigned n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static void
put(int fd, const char *s)
{
	syscall3(SYS_write, fd, (long)s, length(s));
}

/*
 * Signs its own return address, bends the signature, and returns through the bent value: the
 * retab must fault.  There is no C frame here -- the whole function is the sequence below.
 */
__attribute__((naked, noinline))
static void
bent_return(void)
{
	__asm__ volatile(
		".arch_extension pauth\n\t"
		"pacibsp\n\t"                   /* x30 = the signed return address */
		"stp x29, x30, [sp, #-16]!\n\t"
		"mov x29, sp\n\t"
		"ldr x9, [sp, #8]\n\t"
		"add x9, x9, #8\n\t"            /* the corruption */
		"str x9, [sp, #8]\n\t"
		"ldp x29, x30, [sp], #16\n\t"
		"retab\n\t"                     /* authenticates x30: must fault here */
		);
}

void
start(void)
{
	int fd = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);
	char pid[16];
	int i = sizeof(pid);
	long v = syscall1(SYS_getpid, 0);

	put(fd, "\narm64e PAC check, process ");
	pid[--i] = 0;
	do {
		pid[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put(fd, pid + i);
	put(fd, ": sign a return address, bend it, retab\n");
	put(fd, "calling bent_return()\n");

	bent_return();

	put(fd, "NOT ENFORCED: retab returned\n");
	syscall1(SYS_exit, 0);
	for (;;) {
	}
}
