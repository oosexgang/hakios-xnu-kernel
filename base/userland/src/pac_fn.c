/*
 * arm64e user-space PAC features, run as process 1: signed function pointers and pacga.
 * Positive checks that print PASS; a real PAC failure faults (SIGBUS) instead, so reaching
 * "all checks passed" is the proof.  (Return-address signing is pac_test.c.)
 *
 * The function-pointer work is all in one asm block: we take the raw address of target with
 * adrp/add (the C operator & would hand back a compiler-signed arm64e function pointer and fight
 * our own signing), sign it with pacia + our discriminator, authenticate it back, and finally
 * call it with an authenticated branch (blraa).
 */
typedef unsigned long u64;
static long syscall3(long n,long a,long b,long c){register long x16 __asm__("x16")=n;register long x0 __asm__("x0")=a;register long x1 __asm__("x1")=b;register long x2 __asm__("x2")=c;__asm__ volatile("svc #0x80":"+r"(x0):"r"(x16),"r"(x1),"r"(x2):"memory","cc");return x0;}
static long syscall1(long n,long a){register long x16 __asm__("x16")=n;register long x0 __asm__("x0")=a;__asm__ volatile("svc #0x80":"+r"(x0):"r"(x16):"memory","cc");return x0;}
#define SYS_exit 1
#define SYS_write 4
#define SYS_open 5
#define O_RDWR 2
static unsigned slen(const char*s){unsigned n=0;while(s[n])n++;return n;}
static void put(int fd,const char*s){syscall3(SYS_write,fd,(long)s,slen(s));}
static void puthex(int fd,u64 v){char b[19];b[0]='0';b[1]='x';for(int i=0;i<16;i++)b[2+i]="0123456789abcdef"[(v>>((15-i)*4))&0xf];b[18]=0;put(fd,b);}

static volatile int target_called;
static void __attribute__((noinline, used)) target(void){ target_called++; }

int
checks(int fd)
{
	/* sign the raw address of target with IA + our discriminator, authenticate it back */
	u64 roundtrip_ok, changed;
	__asm__ volatile(
		".arch_extension pauth\n\t"
		"adrp x9, _target@PAGE\n\t"
		"add  x9, x9, _target@PAGEOFF\n\t"   /* x9 = raw &target */
		"mov  x10, x9\n\t"                   /* x10 = raw */
		"mov  x11, #0x1234\n\t"
		"pacia x9, x11\n\t"                  /* x9 = signed */
		"cmp   x9, x10\n\t"
		"cset  %w1, ne\n\t"                  /* changed = signed != raw */
		"autia x9, x11\n\t"                  /* x9 = authenticated (must fault if bad) */
		"cmp   x9, x10\n\t"
		"cset  %w0, eq\n\t"                  /* roundtrip_ok = authed == raw */
		: "=r"(roundtrip_ok), "=r"(changed) : : "x9","x10","x11","cc");
	if (!changed)      { put(fd, "FAIL: pacia did not change the pointer (PAC off?)\n"); return 1; }
	if (!roundtrip_ok) { put(fd, "FAIL: autia did not recover the address\n"); return 1; }
	put(fd, "PASS: signed function pointer round-trips (pacia/autia)\n");

	/* authenticated indirect call: sign the raw address, blraa to it */
	target_called = 0;
	__asm__ volatile(
		".arch_extension pauth\n\t"
		"adrp x9, _target@PAGE\n\t"
		"add  x9, x9, _target@PAGEOFF\n\t"
		"mov  x11, #0x1234\n\t"
		"pacia x9, x11\n\t"
		"blraa x9, x11\n\t"                  /* authenticate x9 with x11 and call */
		: : : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12",
		      "x13","x14","x15","x16","x17","x18","x30","memory","cc");
	if (target_called != 1) { put(fd, "FAIL: authenticated call (blraa) did not reach target\n"); return 1; }
	put(fd, "PASS: authenticated indirect call (pacia + blraa) reached target\n");

	/* pacga: 32-bit generic MAC in the high half, deterministic and data-dependent */
	u64 m1, m2, m3;
	__asm__ volatile(".arch_extension pauth\n\tpacga %0, %1, %2" : "=r"(m1) : "r"(0x1111UL), "r"(0UL));
	__asm__ volatile(".arch_extension pauth\n\tpacga %0, %1, %2" : "=r"(m2) : "r"(0x2222UL), "r"(0UL));
	__asm__ volatile(".arch_extension pauth\n\tpacga %0, %1, %2" : "=r"(m3) : "r"(0x1111UL), "r"(0UL));
	if ((m1 >> 32) == 0) { put(fd, "FAIL: pacga produced a zero MAC\n"); return 1; }
	if (m1 != m3)        { put(fd, "FAIL: pacga not deterministic\n"); return 1; }
	if (m1 == m2)        { put(fd, "FAIL: pacga not data-dependent\n"); return 1; }
	put(fd, "PASS: pacga MAC "); puthex(fd, m1 >> 32); put(fd, " vs "); puthex(fd, m2 >> 32); put(fd, "\n");
	return 0;
}

void
start(void)
{
	int fd = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);
	put(fd, "\narm64e PAC features, process 1\n");
	if (checks(fd) == 0) {
		put(fd, "all checks passed\n");
	}
	syscall1(SYS_exit, 0);
	for (;;) {}
}
