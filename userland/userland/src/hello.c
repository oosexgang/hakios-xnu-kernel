/*
 * First user-space program for the Darwin-on-redwood bring-up.
 *
 * A static arm64 Mach-O with no libc and no dyld: it talks to the kernel with
 * raw system calls. The kernel's mockfs presents the ramdisk (this very file)
 * as /sbin/launchd, so this runs as process 1.
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

#define SYS_exit    1
#define SYS_write   4
#define SYS_open    5
#define SYS_getpid  20
#define SYS_getuid  24
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

static void
put_number(int fd, long v)
{
	char buf[24];
	int i = sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put(fd, buf + i);
}

/*
 * The kernel enters a static program with sp at argc, which need not be 16-byte aligned; a
 * processor that checks stack alignment (SCTLR_EL1.SA0, real hardware; QEMU does not) faults
 * on the first push of a C prologue. Align first, as crt1's start and dyld's __dyld_start do.
 */
__asm__(
	"	.text\n"
	"	.globl	_start\n"
	"	.p2align 2\n"
	"_start:\n"
	"	mov	x9, sp\n"
	"	and	sp, x9, #~15\n"
	"	b	_hello_main\n");

void hello_main(void);

void
hello_main(void)
{
	int fd = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);

	put(fd, "\nHello from user space on Darwin!\n");
	put(fd, "I am process ");
	put_number(fd, syscall3(SYS_getpid, 0, 0, 0));
	put(fd, ", uid ");
	put_number(fd, syscall3(SYS_getuid, 0, 0, 0));
	put(fd, ", console fd ");
	put_number(fd, fd);
	put(fd, ".\n");

	/* Process 1 is not supposed to exit; doing so panics the kernel, which ends the test run. */
	syscall3(SYS_exit, 0, 0, 0);
	for (;;) {
	}
}
