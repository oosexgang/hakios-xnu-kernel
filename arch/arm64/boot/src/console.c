/* Part of the loader (included by loader.c, not compiled alone): what the loader says, on the screen and into its log; the caches. */
/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;

enum {
	EFI_TEXT_LIGHTGRAY = 7,
	EFI_TEXT_LIGHTCYAN = 11,
	EFI_TEXT_YELLOW = 14,
	EFI_TEXT_BLACK = 0,
};

#define EFI_TEXT_ATTRIBUTE(fg, bg) ((fg) | ((bg) << 4))

static void print(const char *fmt, ...);

static void
console_attribute(uint64_t attribute)
{
	if (ST->con_out != NULL && ST->con_out->set_attribute != NULL) {
		ST->con_out->set_attribute(ST->con_out, attribute);
	}
}

static void
console_banner(void)
{
	if (ST->con_out != NULL && ST->con_out->clear_screen != NULL) {
		ST->con_out->clear_screen(ST->con_out);
	}
	console_attribute(EFI_TEXT_ATTRIBUTE(EFI_TEXT_LIGHTCYAN, EFI_TEXT_BLACK));
	print("+--------------------------------------------------+\n");
	console_attribute(EFI_TEXT_ATTRIBUTE(EFI_TEXT_YELLOW, EFI_TEXT_BLACK));
	print("|  HackiOS                                         |\n");
	console_attribute(EFI_TEXT_ATTRIBUTE(EFI_TEXT_LIGHTCYAN, EFI_TEXT_BLACK));
	print("+--------------------------------------------------+\n");
	console_attribute(EFI_TEXT_ATTRIBUTE(EFI_TEXT_LIGHTGRAY, EFI_TEXT_BLACK));
	print("  Open XNU boot environment  /  POCO X5 Pro\n\n");
}

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

static void
console_put(char c, void *unused)
{
	(void)unused;
	put_char(c);
}

/* onto the screen and into the log: the subset of printf format_with() knows */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	format_with(console_put, NULL, fmt, ap);
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}
