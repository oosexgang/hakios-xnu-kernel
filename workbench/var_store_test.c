/*
 * Host tests of the data volume's store (init/var_store.c), as it is: what `var_store erase` leaves in the partition,
 * and whether the keychain is packed.
 *
 * The program is compiled against libmini's header, as for the phone, and the host's C library answers the calls
 * it declares: what Linux has not got (sysctlbyname, chflags, the extended stat) is stood in for here, sysctlbyname
 * with a boot line of the test's choosing. The partition is a sparse file, with the slots at their real offsets.
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wincompatible-library-redeclaration"  /* (libmini's strerror gives a const string) */
#endif
#define main var_store_main
#define errno var_store_errno           /* (the host's errno is not a plain int) */
#include "../init/var_store.c"
#undef main
#undef errno

int var_store_errno;
char *getenv(const char *name);

#define HOST_O_CREAT    0x40            /* Linux's, not Darwin's: the file is opened by the host's C library */

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *boot_line = "";

int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen)
{
	(void)newp;
	(void)newlen;
	if (strcmp(name, "kern.bootargs") == 0 && oldp != NULL && oldlenp != NULL) {
		size_t n = strlen(boot_line);

		if (n > *oldlenp) {
			n = *oldlenp;
		}
		memcpy(oldp, boot_line, n);
		*oldlenp = n;
		return 0;
	}
	return -1;
}

int
chflags(const char *p, int flags)
{
	(void)p;
	(void)flags;
	return 0;
}

int
lstat_extended(const char *p, struct stat *st, void *sec, size_t *size)
{
	(void)p;
	(void)st;
	(void)sec;
	(void)size;
	return -1;
}

int
chmod_extended(const char *p, uint32_t uid, uint32_t gid, int mode, const void *sec)
{
	(void)p;
	(void)uid;
	(void)gid;
	(void)mode;
	(void)sec;
	return -1;
}

static uint8_t test_buffer[BUFFER];

/* A slot as save() leaves it: BYTES of archive, every byte FILL, and a header that checks out. */
static void
put_slot(int slot, uint64_t bytes, uint8_t fill, uint64_t generation)
{
	static uint8_t block[BLOCK];
	struct head h;

	memset(block, fill, sizeof(block));
	for (uint64_t done = 0; done < bytes; done += BLOCK) {
		CHECK(device_write(slot_at(slot) + BLOCK + (off_t)done, block, BLOCK), "slot %d: archive written", slot);
	}
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, HEAD_MAGIC, sizeof(h.magic));
	h.version = HEAD_VERSION;
	h.slot = (uint32_t)slot;
	h.generation = generation;
	h.bytes = bytes;
	h.entries = 1;
	h.head_crc = head_checksum(&h);
	memset(block, 0, sizeof(block));
	memcpy(block, &h, sizeof(h));
	CHECK(device_write(slot_at(slot), block, BLOCK), "slot %d: header written", slot);
}

/* How many bytes from WHERE on are not zero. */
static uint64_t
not_zero(off_t where, uint64_t n)
{
	static uint8_t block[BLOCK];
	uint64_t count = 0;

	while (n > 0) {
		uint64_t piece = n > BLOCK ? BLOCK : n;

		if (!device_read(where, block, piece)) {
			return n;
		}
		for (uint64_t i = 0; i < piece; i++) {
			count += block[i] != 0;
		}
		where += (off_t)piece;
		n -= piece;
	}
	return count;
}

int
main(void)
{
	static char name[256];
	const char *tmp = getenv("TMPDIR");
	struct head h;
	size_t n;

	crc_init();
	buffer = test_buffer;
	n = strlen(tmp != NULL && *tmp != 0 ? tmp : "/tmp");
	memcpy(name, tmp != NULL && *tmp != 0 ? tmp : "/tmp", n);
	memcpy(name + n, "/var_store_test.", sizeof("/var_store_test."));
	n += sizeof("/var_store_test.") - 1;
	for (unsigned pid = (unsigned)getpid(), div = 1000000000u; div > 0; div /= 10) {
		name[n++] = (char)('0' + pid / div % 10);       /* (one file a run: runs side by side do not meet) */
	}
	name[n] = 0;
	unlink(name);
	fd = open(name, O_RDWR | HOST_O_CREAT, 0600);
	if (fd < 0) {
		printf("var_store_test: %s cannot be made\n", name);
		return 1;
	}

	/* erase: an archive that does not end on a block (10000 bytes), one of whole blocks, and what lies past each */
	put_slot(0, 10000, 0xa5, 7);
	put_slot(1, 3 * BLOCK, 0x5a, 8);
	{
		static uint8_t past[BLOCK];

		memset(past, 0x77, sizeof(past));
		CHECK(device_write(slot_at(0) + BLOCK + 3 * BLOCK, past, BLOCK), "past slot 0's archive");
		CHECK(device_write(slot_at(1) + BLOCK + 3 * BLOCK, past, BLOCK), "past slot 1's archive");
	}
	CHECK(read_head(0, &h) && read_head(1, &h), "both slots check out before the erase");
	CHECK(not_zero(slot_at(0) + BLOCK, 10000) == 10000, "slot 0 holds its archive");
	CHECK(erase(), "the erase succeeds");
	CHECK(!read_head(0, &h) && !read_head(1, &h), "neither slot checks out after it");
	CHECK(not_zero(slot_at(0), BLOCK) == 0 && not_zero(slot_at(1), BLOCK) == 0, "both headers are zero");
	CHECK(not_zero(slot_at(0) + BLOCK, 3 * BLOCK) == 0, "slot 0's archive is zero, its last part block too: %llu bytes are not",
	    (unsigned long long)not_zero(slot_at(0) + BLOCK, 3 * BLOCK));
	CHECK(not_zero(slot_at(1) + BLOCK, 3 * BLOCK) == 0, "slot 1's archive is zero: %llu bytes are not",
	    (unsigned long long)not_zero(slot_at(1) + BLOCK, 3 * BLOCK));
	CHECK(not_zero(slot_at(0) + 4 * BLOCK, BLOCK) == BLOCK && not_zero(slot_at(1) + 4 * BLOCK, BLOCK) == BLOCK,
	    "nothing past what the header said is written");
	CHECK(erase(), "an erase of an erased store succeeds, and writes the headers alone");

	/* a header that says more than a slot holds does not check out, and nothing is written for it */
	put_slot(0, BLOCK, 0x11, 9);
	{
		static uint8_t block[BLOCK];
		struct head *bad = (struct head *)block;

		CHECK(device_read(slot_at(0), block, BLOCK), "header read");
		bad->bytes = (uint64_t)SLOT_BYTES;
		bad->head_crc = head_checksum(bad);
		CHECK(device_write(slot_at(0), block, BLOCK), "header written");
		CHECK(!read_head(0, &h), "a header that says more than the slot holds is not one");
		CHECK(erase(), "the erase succeeds");
		CHECK(not_zero(slot_at(0) + BLOCK, BLOCK) == BLOCK, "and goes no further than the header");
	}

	/* the keychain: left out unless the boot line asks for it, and only that name */
	boot_line = "var_store=1 ufs_write=1";
	keychain_wanted = -1;
	CHECK(is_skipped("Keychains"), "the keychain is left out by default");
	CHECK(!is_skipped("Keychains2") && !is_skipped("mobile/Keychains"), "and nothing else by that name");
	boot_line = "var_store=1 var_store_keychain=10";
	keychain_wanted = -1;
	CHECK(is_skipped("Keychains"), "var_store_keychain=10 is not var_store_keychain=1");
	boot_line = "var_store=1 var_store_keychain=1 ufs_write=1";
	keychain_wanted = -1;
	CHECK(!is_skipped("Keychains"), "var_store_keychain=1 keeps it");
	CHECK(is_skipped("tmp"), "the rest of SKIP stays left out");

	close(fd);
	unlink(name);
	printf("var_store_test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
