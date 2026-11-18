/* tools/qemu-init: the console, numbers and words, the time */
#include "init.h"

int console = 1;

size_t
length(const char *s)
{
	size_t n = 0;

	while (s[n] != '\0') {
		n++;
	}
	return n;
}

int
same(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

void
say(const char *s)
{
	syscall6(SYS_write, console, (long)s, (long)length(s), 0, 0, 0);
}

void
say_number(uint64_t n)
{
	char digits[24];

	append_number(digits, 0, sizeof(digits), n);
	say(digits);
}

/* text, then a number, put at the end of what LINE (SIZE bytes) holds from AT; where it now ends */
size_t
append(char *line, size_t at, size_t size, const char *text)
{
	while (*text != '\0' && at + 1 < size) {
		line[at++] = *text++;
	}
	line[at] = '\0';
	return at;
}

size_t
append_number(char *line, size_t at, size_t size, uint64_t n)
{
	char digits[24];
	int i = (int)sizeof(digits) - 1;

	digits[i] = '\0';
	do {
		digits[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i > 0);
	return append(line, at, size, &digits[i]);
}

/* the line split at spaces, in place; how many words */
int
split(char *text, char **words)
{
	int n = 0;

	while (*text != '\0' && n < WORDS) {
		while (*text == ' ' || *text == '\t') {
			*text++ = '\0';
		}
		if (*text == '\0') {
			break;
		}
		words[n++] = text;
		while (*text != '\0' && *text != ' ' && *text != '\t') {
			text++;
		}
	}
	return n;
}

void
say_padded(uint64_t n, int width)
{
	char digits[24];
	int i = (int)sizeof(digits) - 1;

	digits[i] = '\0';
	do {
		digits[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i > 0);
	for (int pad = width - ((int)sizeof(digits) - 1 - i); pad > 0; pad--) {
		say(" ");
	}
	say(&digits[i]);
}

unsigned
parse_number(const char *text)
{
	unsigned n = 0;

	while (*text >= '0' && *text <= '9') {
		n = n * 10 + (unsigned)(*text++ - '0');
	}
	return *text == '\0' ? n : 0;
}

/* the same, for numbers past 4 GB (a file's size) */
uint64_t
parse_number64(const char *text)
{
	uint64_t n = 0;

	while (*text >= '0' && *text <= '9') {
		n = n * 10 + (uint64_t)(*text++ - '0');
	}
	return *text == '\0' ? n : 0;
}

#define SYS_select              93

/* microseconds since 1970 */
int64_t
now_us(void)
{
	int64_t tv[2] = { 0, 0 };       /* struct timeval: seconds, then microseconds in 32 bits */

	syscall6(SYS_gettimeofday, (long)tv, 0, 0, 0, 0, 0);
	return tv[0] * 1000000 + (int32_t)tv[1];
}

void
sleep_us(int64_t us)
{
	int64_t tv[2] = { us / 1000000, us % 1000000 };

	syscall6(SYS_select, 0, 0, 0, 0, (long)tv, 0);
}

/* N bytes from FROM to TO (no libc here; volatile, or the compiler makes the loop a call to the memcpy there is not) */
void
copy_bytes(void *to, const void *from, size_t n)
{
	volatile uint8_t *t = to;
	const uint8_t *f = from;

	for (size_t i = 0; i < n; i++) {
		t[i] = f[i];
	}
}

/* "error N (ENAME)": a system call's -errno, by its number and, if it is a common one, its name (sys/errno.h) */
void
say_error(long error)
{
	static const struct { int number; const char *name; } names[] = {
		{ 1, "EPERM" }, { 2, "ENOENT" }, { 3, "ESRCH" }, { 4, "EINTR" }, { 5, "EIO" }, { 7, "E2BIG" },
		{ 8, "ENOEXEC" }, { 9, "EBADF" }, { 10, "ECHILD" }, { 12, "ENOMEM" }, { 13, "EACCES" },
		{ 14, "EFAULT" }, { 16, "EBUSY" }, { 17, "EEXIST" }, { 18, "EXDEV" }, { 20, "ENOTDIR" },
		{ 21, "EISDIR" }, { 22, "EINVAL" }, { 24, "EMFILE" }, { 27, "EFBIG" }, { 28, "ENOSPC" }, { 30, "EROFS" },
		{ 31, "EMLINK" }, { 32, "EPIPE" }, { 35, "EAGAIN" }, { 36, "EINPROGRESS" }, { 45, "ENOTSUP" },
		{ 48, "EADDRINUSE" }, { 49, "EADDRNOTAVAIL" }, { 50, "ENETDOWN" }, { 51, "ENETUNREACH" },
		{ 54, "ECONNRESET" }, { 60, "ETIMEDOUT" }, { 61, "ECONNREFUSED" }, { 62, "ELOOP" },
		{ 63, "ENAMETOOLONG" }, { 65, "EHOSTUNREACH" }, { 66, "ENOTEMPTY" }, { 78, "ENOSYS" },
		{ 86, "EBADARCH" }, { 88, "EBADMACHO" }, { 93, "ENOATTR" },
	};
	uint64_t number = (uint64_t)(error < 0 ? -error : error);

	say("error ");
	say_number(number);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((uint64_t)names[i].number == number) {
			say(" (");
			say(names[i].name);
			say(")");
		}
	}
}

/* the n bytes at a are those at b */
int
bytes_same(const void *a, const void *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (((const uint8_t *)a)[i] != ((const uint8_t *)b)[i]) {
			return 0;
		}
	}
	return 1;
}

/* FNV-1a, over n bytes, from where it got to (FNV1A_START for none yet) */
uint64_t
fnv1a(uint64_t hash, const void *bytes, long n)
{
	for (long i = 0; i < n; i++) {
		hash = (hash ^ ((const uint8_t *)bytes)[i]) * 0x100000001b3ull;
	}
	return hash;
}

/* "WHAT: PATH: error N (ENAME)" */
void
say_failed(const char *what, const char *path, long error)
{
	say(what);
	say(": ");
	say(path);
	say(": ");
	say_error(error);
	say("\n");
}
