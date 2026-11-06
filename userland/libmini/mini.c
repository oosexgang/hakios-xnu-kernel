/*
 * libmini: system call wrappers, string routines and a small printf.
 */
#include "mini.h"

int errno;
char **environ;

#define SC(n, a, b, c, d, e, f) mini_syscall((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

void
__mini_start(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);

	environ = argv + argc + 1;
	_exit(main(argc, argv, environ));
}

void
_exit(int status)
{
	for (;;) {
		SC(SYS_exit, status, 0, 0, 0, 0, 0);
	}
}

ssize_t
read(int fd, void *buf, size_t count)
{
	return SC(SYS_read, fd, buf, count, 0, 0, 0);
}

ssize_t
write(int fd, const void *buf, size_t count)
{
	return SC(SYS_write, fd, buf, count, 0, 0, 0);
}

int
open(const char *path, int flags, int mode)
{
	return (int)SC(SYS_open, path, flags, mode, 0, 0, 0);
}

int
close(int fd)
{
	return (int)SC(SYS_close, fd, 0, 0, 0, 0, 0);
}

off_t
lseek(int fd, off_t offset, int whence)
{
	return SC(SYS_lseek, fd, offset, whence, 0, 0, 0);
}

int
dup2(int from, int to)
{
	return (int)SC(SYS_dup2, from, to, 0, 0, 0, 0);
}

int
chdir(const char *path)
{
	return (int)SC(SYS_chdir, path, 0, 0, 0, 0, 0);
}

pid_t
getpid(void)
{
	return (pid_t)SC(SYS_getpid, 0, 0, 0, 0, 0, 0);
}

pid_t
getppid(void)
{
	return (pid_t)SC(SYS_getppid, 0, 0, 0, 0, 0, 0);
}

int
getuid(void)
{
	return (int)SC(SYS_getuid, 0, 0, 0, 0, 0, 0);
}

pid_t
wait4(pid_t pid, int *status, int options, void *rusage)
{
	return (pid_t)SC(SYS_wait4, pid, status, options, rusage, 0, 0);
}

int
execve(const char *path, char *const argv[], char *const envp[])
{
	return (int)SC(SYS_execve, path, argv, envp, 0, 0, 0);
}

int
stat(const char *path, struct stat *st)
{
	return (int)SC(SYS_stat64, path, st, 0, 0, 0, 0);
}

int
lstat(const char *path, struct stat *st)
{
	return (int)SC(SYS_lstat64, path, st, 0, 0, 0, 0);
}

int
fstat(int fd, struct stat *st)
{
	return (int)SC(SYS_fstat64, fd, st, 0, 0, 0, 0);
}

ssize_t
readlink(const char *path, char *buf, size_t size)
{
	return SC(SYS_readlink, path, buf, size, 0, 0, 0);
}

ssize_t
getdirentries(int fd, void *buf, size_t size, off_t *position)
{
	return SC(SYS_getdirentries64, fd, buf, size, position, 0, 0);
}

void *
mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return (void *)SC(SYS_mmap, addr, length, prot, flags, fd, offset);
}

int
munmap(void *addr, size_t length)
{
	return (int)SC(SYS_munmap, addr, length, 0, 0, 0, 0);
}

int
gettimeofday(struct timeval *tv, void *tz)
{
	return (int)SC(SYS_gettimeofday, tv, tz, 0, 0, 0, 0);
}

/* Sleep with select(): the kernel has no dedicated sleep system call. */
int
usleep(unsigned microseconds)
{
	struct timeval tv = { .tv_sec = microseconds / 1000000, .tv_usec = (int)(microseconds % 1000000) };

	return (int)SC(SYS_select, 0, 0, 0, 0, &tv, 0);
}

int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen)
{
	return (int)SC(SYS_sysctlbyname, name, strlen(name), oldp, oldlenp, newp, newlen);
}

int
mount(const char *type, const char *dir, int flags, void *data)
{
	return (int)SC(SYS_mount, type, dir, flags, data, 0, 0);
}

int
mkdir(const char *path, int mode)
{
	return (int)SC(SYS_mkdir, path, mode, 0, 0, 0, 0);
}

int
rmdir(const char *path)
{
	return (int)SC(SYS_rmdir, path, 0, 0, 0, 0, 0);
}

int
unlink(const char *path)
{
	return (int)SC(SYS_unlink, path, 0, 0, 0, 0, 0);
}

int
rename(const char *from, const char *to)
{
	return (int)SC(SYS_rename, from, to, 0, 0, 0, 0);
}

int
link(const char *from, const char *to)
{
	return (int)SC(SYS_link, from, to, 0, 0, 0, 0);
}

int
symlink(const char *target, const char *path)
{
	return (int)SC(SYS_symlink, target, path, 0, 0, 0, 0);
}

int
chmod(const char *path, int mode)
{
	return (int)SC(SYS_chmod, path, mode, 0, 0, 0, 0);
}

int
chown(const char *path, int uid, int gid)
{
	return (int)SC(SYS_chown, path, uid, gid, 0, 0, 0);
}

int
lchown(const char *path, int uid, int gid)
{
	return (int)SC(SYS_lchown, path, uid, gid, 0, 0, 0);
}

int
chflags(const char *path, int flags)
{
	return (int)SC(SYS_chflags, path, flags, 0, 0, 0, 0);
}

int
utimes(const char *path, const struct timeval times[2])
{
	return (int)SC(SYS_utimes, path, times, 0, 0, 0, 0);
}

ssize_t
listxattr(const char *path, char *names, size_t size, int options)
{
	return SC(SYS_listxattr, path, names, size, options, 0, 0);
}

ssize_t
getxattr(const char *path, const char *name, void *value, size_t size, uint32_t position, int options)
{
	return SC(SYS_getxattr, path, name, value, size, position, options);
}

int
setxattr(const char *path, const char *name, const void *value, size_t size, uint32_t position, int options)
{
	return (int)SC(SYS_setxattr, path, name, value, size, position, options);
}

int
ftruncate(int fd, off_t length)
{
	return (int)SC(SYS_ftruncate, fd, length, 0, 0, 0, 0);
}

int
fsync(int fd)
{
	return (int)SC(SYS_fsync, fd, 0, 0, 0, 0, 0);
}

ssize_t
pread(int fd, void *buf, size_t count, off_t offset)
{
	return SC(SYS_pread, fd, buf, count, offset, 0, 0);
}

ssize_t
pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	return SC(SYS_pwrite, fd, buf, count, offset, 0, 0);
}

int
socket(int domain, int type, int protocol)
{
	return (int)SC(SYS_socket, domain, type, protocol, 0, 0, 0);
}

int
bind(int fd, const void *address, unsigned length)
{
	return (int)SC(SYS_bind, fd, address, length, 0, 0, 0);
}

int
reboot(int howto)
{
	return (int)SC(SYS_reboot, howto, 0, 0, 0, 0, 0);
}

size_t
strlen(const char *s)
{
	size_t n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b || !*a) {
			return (unsigned char)*a - (unsigned char)*b;
		}
	}
	return 0;
}

char *
strcpy(char *dst, const char *src)
{
	char *d = dst;

	while ((*d++ = *src++)) {
	}
	return dst;
}

char *
strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c) {
			return (char *)s;
		}
		if (!*s) {
			return NULL;
		}
	}
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n--) {
		*d++ = (unsigned char)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++) {
		if (*x != *y) {
			return *x - *y;
		}
	}
	return 0;
}

const char *
strerror(int error)
{
	static const char *const names[] = {
		[1] = "Operation not permitted", [2] = "No such file or directory", [3] = "No such process",
		[4] = "Interrupted system call", [5] = "Input/output error", [6] = "Device not configured",
		[7] = "Argument list too long", [8] = "Exec format error", [9] = "Bad file descriptor",
		[10] = "No child processes", [12] = "Cannot allocate memory", [13] = "Permission denied",
		[14] = "Bad address", [16] = "Resource busy", [17] = "File exists", [19] = "Operation not supported by device",
		[20] = "Not a directory", [21] = "Is a directory", [22] = "Invalid argument", [25] = "Inappropriate ioctl for device",
		[28] = "No space left on device", [30] = "Read-only file system", [45] = "Operation not supported",
		[78] = "Function not implemented",
	};

	if (error >= 0 && (size_t)error < sizeof(names) / sizeof(names[0]) && names[error]) {
		return names[error];
	}
	return "Unknown error";
}

/* printf: %c %s %d %i %u %x %X %p %%, flags '-' and '0', a width, length modifiers l, ll and z. */

struct out {
	int fd;
	int total;
	size_t used;
	char buf[256];
};

static void
out_flush(struct out *o)
{
	size_t done = 0;

	while (done < o->used) {
		ssize_t n = write(o->fd, o->buf + done, o->used - done);

		if (n <= 0) {
			break;
		}
		done += (size_t)n;
	}
	o->used = 0;
}

static void
out_char(struct out *o, char c)
{
	if (o->used == sizeof(o->buf)) {
		out_flush(o);
	}
	o->buf[o->used++] = c;
	o->total++;
}

static void
out_field(struct out *o, const char *s, size_t len, int width, int left, char pad)
{
	int fill = width > (int)len ? width - (int)len : 0;

	while (!left && fill-- > 0) {
		out_char(o, pad);
	}
	while (len--) {
		out_char(o, *s++);
	}
	while (left && fill-- > 0) {
		out_char(o, ' ');
	}
}

int
vdprintf(int fd, const char *format, va_list ap)
{
	struct out o = { .fd = fd };

	for (const char *p = format; *p; p++) {
		char digits[24], pad = ' ';
		int left = 0, width = 0, longs = 0, base = 10, negative = 0, upper = 0;
		unsigned long long value;
		size_t len;

		if (*p != '%') {
			out_char(&o, *p);
			continue;
		}
		for (p++; *p == '-' || *p == '0'; p++) {
			if (*p == '-') {
				left = 1;
			} else {
				pad = '0';
			}
		}
		for (; *p >= '0' && *p <= '9'; p++) {
			width = width * 10 + (*p - '0');
		}
		for (; *p == 'l' || *p == 'z'; p++) {
			longs++;
		}

		switch (*p) {
		case '%':
			out_char(&o, '%');
			continue;
		case 'c':
			digits[0] = (char)va_arg(ap, int);
			out_field(&o, digits, 1, width, left, ' ');
			continue;
		case 's': {
			const char *s = va_arg(ap, const char *);

			if (!s) {
				s = "(null)";
			}
			out_field(&o, s, strlen(s), width, left, ' ');
			continue;
		}
		case 'd':
		case 'i': {
			long long v = longs ? va_arg(ap, long long) : va_arg(ap, int);

			negative = v < 0;
			value = negative ? 0ULL - (unsigned long long)v : (unsigned long long)v;
			break;
		}
		case 'p':
			longs = 1;
			base = 16;
			value = va_arg(ap, unsigned long long);
			break;
		case 'X':
			upper = 1;
			/* fall through */
		case 'x':
			base = 16;
			/* fall through */
		case 'u':
			value = longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
			break;
		default:
			/* unknown conversion: show it as written */
			out_char(&o, '%');
			if (*p) {
				out_char(&o, *p);
			} else {
				p--;
			}
			continue;
		}

		len = sizeof(digits);
		do {
			unsigned digit = (unsigned)(value % (unsigned)base);

			digits[--len] = (char)(digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
			value /= (unsigned)base;
		} while (value);
		if (negative) {
			if (pad == '0' && !left) {
				/* the sign goes before the zero padding */
				out_char(&o, '-');
				width = width > 0 ? width - 1 : 0;
			} else {
				digits[--len] = '-';
			}
		}
		out_field(&o, digits + len, sizeof(digits) - len, width, left, left ? ' ' : pad);
	}
	out_flush(&o);
	return o.total;
}

int
dprintf(int fd, const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vdprintf(fd, format, ap);
	va_end(ap);
	return n;
}

int
printf(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vdprintf(1, format, ap);
	va_end(ap);
	return n;
}

int
puts(const char *s)
{
	return printf("%s\n", s);
}
