/*
 * f2fs-resize MOUNTPOINT BLOCKS: ask Android's kernel to shrink the mounted f2fs at MOUNTPOINT to
 * BLOCKS blocks of 4 KB (F2FS_IOC_RESIZE_FS). The kernel moves whatever lies past the new end
 * (here nothing does: see tools/userdata-carve.py) and rewrites the superblocks and the
 * checkpoint; the partition itself stays as it is until its GPT entry is changed.
 * "f2fs-resize MOUNTPOINT probe" only asks whether the kernel has that ioctl at all, by asking
 * for a size it must refuse (one block more than there is): EINVAL means it has, ENOTTY not.
 *
 * Built without any C library (raw system calls), for adb's root shell:
 *   tools/android/build.sh
 */
typedef unsigned long u64;

#define F2FS_IOC_RESIZE_FS 0x4008f510UL         /* _IOW(0xf5, 16, __u64) */
#define AT_FDCWD (-100)
#define O_RDONLY_DIRECTORY 040000               /* O_DIRECTORY | O_RDONLY on arm64 */

static long
sys(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
	__asm__ volatile ("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static void
say(const char *s)
{
	long n = 0;
	while (s[n]) {
		n++;
	}
	sys(64, 1, (long)s, n, 0);              /* write */
}

static void
say_num(u64 v)
{
	char b[24];
	int i = 23;
	b[i] = 0;
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	say(b + i);
}

static int
equal(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int
main(int argc, char **argv)
{
	u64 blocks = 0;
	long fd, r;

	if (argc != 3) {
		say("usage: f2fs-resize MOUNTPOINT BLOCKS|probe\n");
		return 2;
	}
	if (equal(argv[2], "probe")) {
		blocks = (u64)1 << 40;
	} else {
		for (const char *p = argv[2]; *p; p++) {
			if (*p < '0' || *p > '9') {
				say("f2fs-resize: BLOCKS is a number of 4 KB blocks\n");
				return 2;
			}
			blocks = blocks * 10 + (u64)(*p - '0');
		}
	}
	fd = sys(56, AT_FDCWD, (long)argv[1], O_RDONLY_DIRECTORY, 0);   /* openat */
	if (fd < 0) {
		say("f2fs-resize: cannot open ");
		say(argv[1]);
		say(": errno ");
		say_num((u64)-fd);
		say("\n");
		return 1;
	}
	r = sys(29, fd, (long)F2FS_IOC_RESIZE_FS, (long)&blocks, 0);    /* ioctl */
	say("f2fs-resize: F2FS_IOC_RESIZE_FS to ");
	say_num(blocks);
	say(" blocks: ");
	if (r == 0) {
		say("done\n");
	} else {
		say("errno ");
		say_num((u64)-r);
		say(-r == 22 ? " (EINVAL)\n" : -r == 25 ? " (ENOTTY: no such ioctl)\n" : -r == 28 ? " (ENOSPC)\n" :
		    -r == 1 ? " (EPERM)\n" : -r == 30 ? " (EROFS)\n" : -r == 117 ? " (EFSCORRUPTED: fsck first)\n" : "\n");
	}
	sys(57, fd, 0, 0, 0);                   /* close */
	return r == 0 ? 0 : 1;
}

/* the entry point: argc and argv on the stack, as the kernel leaves them */
__attribute__((naked, noreturn)) void
_start(void)
{
	__asm__ volatile (
		"ldr x0, [sp]\n"
		"add x1, sp, #8\n"
		"bl main\n"
		"mov x8, #93\n"         /* exit */
		"svc 0\n");
}
