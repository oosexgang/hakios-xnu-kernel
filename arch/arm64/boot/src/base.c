/* Part of the loader (included by loader.c, not compiled alone): the loader's globals, memory and strings. */
/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if (d < s) {
		while (n--) {
			*d++ = *s++;
		}
	} else {
		while (n--) {
			d[n] = s[n];
		}
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * The loader's printf subset -- %s, and %x and %d of 64-bit values, %% -- each character handed to PUT with TO: print()
 * puts them on the screen and into the log, format_into() into a string of its own.
 */
static void
format_with(void (*put)(char, void *), void *to, const char *fmt, __builtin_va_list ap)
{
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put(*fmt, to);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put(*s++, to);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put(tmp[--i], to);
			}
		} else if (*fmt == 0) {
			break;                  /* (a '%' at the end: nothing after it to read) */
		} else {
			put(*fmt, to);
		}
	}
}

struct text_room {
	char *at;
	uint32_t left;                  /* bytes, the end's 0 among them */
};

static void
text_put(char c, void *to)
{
	struct text_room *room = to;

	if (room->left > 1) {
		*room->at++ = c;
		room->left--;
	}
}

/*
 * FMT formatted into the SIZE bytes at TEXT, cut short to fit, and always ended. For a line kept apart from the log --
 * "getvar result", "getvar screen" -- which a kernel's run leaves full: what print() puts there after it is lost.
 */
static void
format_into(char *text, uint32_t size, const char *fmt, ...)
{
	struct text_room room = { text, size };
	__builtin_va_list ap;

	if (size == 0) {
		return;
	}
	__builtin_va_start(ap, fmt);
	format_with(text_put, &room, fmt, ap);
	__builtin_va_end(ap);
	*room.at = 0;
}
