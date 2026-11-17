/* Part of the loader (included by loader.c, not compiled alone): the payload: kernel, images and boot arguments, found and taken apart. */
/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	const uint8_t *identity;        /* which iPhone Darwin is told it runs on (identity_given.c), when the payload says */
	uint32_t identity_size;
	int usb_mode;
};

/* the value of the digit C, or 16 when it is none: 0-9, a-f and A-F */
static unsigned
digit_of(char c)
{
	return c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
	       c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
}

/* a number in BASE (16 may begin with 0x), as far as its digits go: for the payload's entries, which the packer writes */
static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;

	if (base == 16 && c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) {
		c += 2;
	}
	for (; *c && digit_of(*c) < base; c++) {
		v = v * base + digit_of(*c);
	}
	return v;
}

/*
 * A number of a command from the host at *TEXT, in BASE (16 may begin with 0x), into *VALUE: 1, with *TEXT past it and
 * the spaces after it; or 0 when it is none -- no digit, a letter after its digits (an "8000A000" read as 8 would put a
 * download at the wrong place on the storage), more than 64 bits -- and nothing taken.
 */
static int
number_given(const char **text, unsigned base, uint64_t *value)
{
	const char *c = *text;
	uint64_t v = 0;
	unsigned digits = 0;

	if (base == 16 && c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) {
		c += 2;
	}
	for (; *c && digit_of(*c) < base; c++, digits++) {
		if (v > (~0ULL - digit_of(*c)) / base) {
			return 0;
		}
		v = v * base + digit_of(*c);
	}
	if (digits == 0 || (*c != 0 && *c != ' ')) {
		return 0;
	}
	while (*c == ' ') {
		c++;
	}
	*value = v;
	*text = c;
	return 1;
}

/* An entry's text holds its end within its SIZE bytes (else whoever reads it would read on past it) */
static int
payload_text(const char *text, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++) {
		if (text[i] == 0) {
			return 1;
		}
	}
	return 0;
}

/* A payload that is none after all: nothing of it taken, and the reason said */
static int
payload_damaged(struct payload_info *pi)
{
	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	return 0;
}

/*
 * Every size the payload gives is held to what is there before anything is taken from it -- the header's to the room
 * reserved for a payload, each entry's to what is left after it, the text of the boot arguments and the numbers to an
 * end inside their entry -- and the padding is reckoned in 64 bits: a payload cut short or damaged is none, said so,
 * rather than read past (a kernel copied from beyond it, a gigabyte of identity, a line of arguments without an end).
 */
static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
	if ((uint64_t)ph->size > PAYLOAD_MAX_SIZE - sizeof(*ph)) {
		print("the payload says it has %x bytes, more than the %x reserved for one: damaged\n", (uint64_t)ph->size,
		    (uint64_t)PAYLOAD_MAX_SIZE);
		return payload_damaged(pi);
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while ((uint64_t)(pend - p) >= sizeof(struct payload_entry)) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		uint64_t left = (uint64_t)(pend - p) - sizeof(*e), step;
		char name[sizeof(e->name) + 1];

		if (e->name[0] == 0) {
			break;
		}
		memcpy(name, e->name, sizeof(e->name));
		name[sizeof(e->name)] = 0;              /* (a name of all sixteen letters has no end of its own) */
		if (verbose) {
			print("payload entry '%s' size %x\n", name, (uint64_t)e->size);
		}
		if (e->size > left) {
			print("payload entry '%s' says %x bytes, %x are left: the payload is damaged\n", name, (uint64_t)e->size, left);
			return payload_damaged(pi);
		}
		if ((str_eq(name, "cmdline") || str_eq(name, "physbase") || str_eq(name, "rdres") || str_eq(name, "checkpoint")) &&
		    !payload_text(text, e->size)) {
			print("payload entry '%s' has no end to its text: the payload is damaged\n", name);
			return payload_damaged(pi);
		}
		if (str_eq(name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(name, "kernel")) {
			pi->kernel = p + sizeof(*e);
		} else if (str_eq(name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(name, "identity")) {
			pi->identity = p + sizeof(*e);           /* made from the IPSW of whoever boots it: identity_given.c */
			pi->identity_size = e->size;
		} else if (str_eq(name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		step = sizeof(*e) + (((uint64_t)e->size + 15) & ~15ULL);
		if (step >= (uint64_t)(pend - p)) {
			break;                          /* (the last entry: its padding may go past the size given) */
		}
		p += step;
	}
	return 1;
}
