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
	int i = (int)sizeof(digits) - 1;

	digits[i] = '\0';
	do {
		digits[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0 && i > 0);
	say(&digits[i]);
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
