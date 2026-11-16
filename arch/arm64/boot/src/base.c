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
