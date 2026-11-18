/*
 * The sparse image's driver itself (drivers/board/sparse.c), run as a host program. It
 * serves a disk out of a table and some blocks that sit at the end of the RAM disk, on the word of a
 * header there. So the model is that memory, as the build writes it and as it must not be believed:
 * with a table that points outside the image, with sizes whose sums wrap.
 *
 *   sparse_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];
extern const char *boot_args;
void loader_log_putc(char c);
bool logged(const char *text);

/* what the driver says, kept where logged() looks */
static void __printflike(1, 2)
said(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/* ---- the RAM disk -------------------------------------------------------------------------------- */

#define BLOCK           4096u
#define BLOCKS          8u              /* of the container the image stands in for */
#define HERE            3u              /* of them in the image: blocks 0, 5 and 7 */
#define ROOT            8192u           /* the root's own image before it */
#define HEADER          52u
#define TABLE_AT        64u             /* from the header */
#define DATA_AT         128u
#define IMAGE           (DATA_AT + HERE * BLOCK)
#define AFTER           (2 * BLOCK)     /* memory past the RAM disk: never the driver's to read */

static uint8_t memory[ROOT + IMAGE + AFTER];
static uint64_t disk_size = ROOT + IMAGE;
static bool no_disk;

dev_t
mdevlookup(int devid)
{
	return no_disk || devid != 0 ? (dev_t)-1 : makedev(3, 0);
}

int
mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size)
{
	if (no_disk || dev != makedev(3, 0)) {
		return -1;
	}
	*base = (vm_offset_t)memory;
	*size = disk_size;
	return 0;
}

static void
put32(unsigned at, uint32_t value)
{
	memcpy(memory + ROOT + at, &value, sizeof(value));
}

static void
put64(unsigned at, uint64_t value)
{
	memcpy(memory + ROOT + at, &value, sizeof(value));
}

/* the header's fields, where the packed structure has them */
enum { AT_BLOCK_SIZE = 16, AT_BLOCK_COUNT = 20, AT_PRESENT = 28, AT_TABLE = 36, AT_DATA = 44 };

/* The image as tools/apfs/mksparse.py writes it: block n of the image is filled with the byte 0xa0 + n. */
static void
image(void)
{
	static const uint32_t slots[BLOCKS] = { 1, 0, 0, 0, 0, 2, 0, 3 };

	memset(memory, 0x11, ROOT);
	memset(memory + ROOT, 0, IMAGE);
	memset(memory + ROOT + IMAGE, 0xee, AFTER);
	memcpy(memory + ROOT, "HAKIOSSPARSE1\0\0\0", 16);
	put32(AT_BLOCK_SIZE, BLOCK);
	put64(AT_BLOCK_COUNT, BLOCKS);
	put64(AT_PRESENT, HERE);
	put64(AT_TABLE, TABLE_AT);
	put64(AT_DATA, DATA_AT);
	for (unsigned i = 0; i < BLOCKS; i++) {
		put32(TABLE_AT + 4 * i, slots[i]);
	}
	for (unsigned i = 0; i < HERE; i++) {
		memset(memory + ROOT + DATA_AT + i * BLOCK, 0xa0 + (int)i, BLOCK);
	}
	boot_args = "apfs_sparse=8192";
}

#define printf said
#include "board/sparse.c"
#undef printf                           /* (the driver's is its own: what the test prints is the test's) */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* every byte of the piece is that one */
static bool
all(const uint8_t *piece, size_t size, uint8_t byte)
{
	for (size_t i = 0; i < size; i++) {
		if (piece[i] != byte) {
			return false;
		}
	}
	return true;
}

static bool
there(void)
{
	return fake_disk_open("rsp0", FREAD) == 0;
}

/* ---- scenarios ----------------------------------------------------------------------------------- */

/* the blocks that are here are served at their own addresses; one that is not reads as zeros and is counted */
static void
served(void)
{
	uint8_t data[2 * BLOCK];
	size_t resid = 99;
	uint32_t size = 0, writable = 9;
	uint64_t count = 0;

	image();
	hakios_sparse_init();
	CHECK(there(), "the disk is there: %s", log_text);
	CHECK(logged("/dev/rsp0 stands in for 8 blocks of 4096, 3 of them here"), "said: %s", log_text);
	CHECK(fake_disk_read("rsp0", 0, data, BLOCK, &resid) == 0 && resid == 0 && all(data, BLOCK, 0xa0), "block 0: %#x", data[0]);
	CHECK(fake_disk_read("rsp0", 5 * 8, data, BLOCK, &resid) == 0 && all(data, BLOCK, 0xa1), "block 5: %#x", data[0]);
	CHECK(fake_disk_read("rsp0", 7 * 8, data, BLOCK, &resid) == 0 && all(data, BLOCK, 0xa2), "block 7: %#x", data[0]);
	memset(data, 0x55, sizeof(data));
	CHECK(fake_disk_read("rsp0", 1 * 8, data, BLOCK, &resid) == 0 && resid == 0 && all(data, BLOCK, 0), "block 1, which is not here: zeros");
	hakios_sparse_report();
	CHECK(logged("sparse: 3 blocks served, 1 asked for that are not in the image"), "counted: %s", log_text);
	CHECK(!logged("damaged"), "and nothing is said of damage: %s", log_text);
	/* what a program asks a disk */
	CHECK(fake_disk_ioctl("rsp0", DKIOCGETBLOCKSIZE, &size) == 0 && size == 512, "sectors of 512: %u", size);
	CHECK(fake_disk_ioctl("rsp0", DKIOCGETBLOCKCOUNT, &count) == 0 && count == BLOCKS * 8, "%llu sectors", (unsigned long long)count);
	CHECK(fake_disk_ioctl("rsp0", DKIOCISWRITABLE, &writable) == 0 && writable == 0, "not writable");
	size = 4096;
	CHECK(fake_disk_ioctl("rsp0", DKIOCSETBLOCKSIZE, &size) == EINVAL, "no other sector size");
	CHECK(fake_disk_ioctl("rsp0", 0x1234, &size) == ENOTTY, "nothing else is answered");
}

/* a request need not be one block: from the middle of a sector run, across blocks that are and are not here */
static void
across_blocks(void)
{
	uint8_t data[3 * BLOCK];
	size_t resid = 99;

	image();
	put32(TABLE_AT + 4 * BLOCKS, 1);        /* (what follows the table looks like an entry: it is none, and is never read) */
	hakios_sparse_init();
	memset(data, 0x55, sizeof(data));
	/* from sector 3 of block 4 (not here) over all of block 5 into block 6 (not here) */
	CHECK(fake_disk_read("rsp0", 4 * 8 + 3, data, 5 * 512 + BLOCK + 512, &resid) == 0 && resid == 0, "read");
	CHECK(all(data, 5 * 512, 0) && all(data + 5 * 512, BLOCK, 0xa1) && all(data + 5 * 512 + BLOCK, 512, 0),
	    "zeros, block 5, zeros: %#x %#x %#x", data[0], data[5 * 512], data[5 * 512 + BLOCK]);
	CHECK(all(data + 5 * 512 + BLOCK + 512, 512, 0x55), "and not a byte more than was asked for");
	/* the last sector of the last block, and what follows it */
	CHECK(fake_disk_read("rsp0", BLOCKS * 8 - 1, data, 512, &resid) == 0 && resid == 0 && all(data, 512, 0xa2), "the last sector");
	CHECK(fake_disk_read("rsp0", BLOCKS * 8, data, 512, &resid) == 0 && resid == 512, "at the end: nothing, and no error");
	CHECK(fake_disk_read("rsp0", BLOCKS * 8 + 1, data, 512, &resid) == EINVAL && resid == 512, "past it: refused");
	memset(data, 0x55, sizeof(data));
	CHECK(fake_disk_read("rsp0", BLOCKS * 8 - 1, data, 1024, &resid) == 0 && all(data, 512, 0xa2) && all(data + 512, 512, 0),
	    "over the end: what is there, then zeros: %#x %#x", data[0], data[512]);
}

/* the smallest blocks a header may have, a sector each, and every one of them here: the image is the disk */
static void
every_block_here(void)
{
	uint8_t data[BLOCKS * 512];
	size_t resid = 99;

	image();
	put32(AT_BLOCK_SIZE, 512);
	put64(AT_PRESENT, BLOCKS);
	for (unsigned i = 0; i < BLOCKS; i++) {
		put32(TABLE_AT + 4 * i, i + 1);
		memset(memory + ROOT + DATA_AT + i * 512, 0xb0 + (int)i, 512);
	}
	hakios_sparse_init();
	CHECK(there() && logged("/dev/rsp0 stands in for 8 blocks of 512, 8 of them here"), "the disk is there: %s", log_text);
	CHECK(fake_disk_read("rsp0", 0, data, sizeof(data), &resid) == 0 && resid == 0, "read whole");
	for (unsigned i = 0; i < BLOCKS; i++) {
		CHECK(all(data + i * 512, 512, (uint8_t)(0xb0 + i)), "sector %u: %#x", i, data[i * 512]);
	}
	hakios_sparse_report();
	CHECK(logged("sparse: 8 blocks served, 0 asked for that are not in the image"), "counted: %s", log_text);
}

/* nothing writes it */
static void
read_only(void)
{
	uint8_t data[BLOCK];
	size_t resid = 0;

	image();
	hakios_sparse_init();
	memset(data, 0x77, sizeof(data));
	CHECK(fake_disk_open("rsp0", FREAD | FWRITE) == EROFS, "not opened for writing");
	CHECK(fake_disk_write("rsp0", 0, data, BLOCK, &resid) == EROFS, "a write is refused");
	CHECK(all(memory + ROOT + DATA_AT, BLOCK, 0xa0) && all(memory, ROOT, 0x11), "and the RAM disk is as it was");
}

/* a sector whose offset in bytes wraps 64 bits (2^55 x 512 is 0): refused, not served as sector 0 */
static void
sector_that_wraps(void)
{
	uint8_t data[BLOCK];
	size_t resid = 0;

	image();
	hakios_sparse_init();
	memset(data, 0x55, sizeof(data));
	CHECK(fake_disk_read("rsp0", 1ull << 55, data, BLOCK, &resid) == EINVAL && resid == BLOCK, "refused: resid %zu", resid);
	CHECK(all(data, BLOCK, 0x55), "and block 0's 0xa0 not given for it: %#x", data[0]);
}

/* an entry of the table that points past the image's blocks: nothing is read from there */
static void
table_points_outside(void)
{
	uint8_t data[BLOCK];
	size_t resid = 99;

	image();
	put32(TABLE_AT + 4 * 2, HERE + 1);      /* block 2: the first block after the image, which is the memory after the RAM disk */
	put32(TABLE_AT + 4 * 3, 0xffffffffu);   /* block 3: gigabytes away */
	hakios_sparse_init();
	CHECK(there(), "the disk is there: %s", log_text);
	memset(data, 0x55, sizeof(data));
	CHECK(fake_disk_read("rsp0", 2 * 8, data, BLOCK, &resid) == 0 && all(data, BLOCK, 0), "block 2 reads as zeros, not as what follows the disk: %#x", data[0]);
	memset(data, 0x55, sizeof(data));
	CHECK(fake_disk_read("rsp0", 3 * 8, data, BLOCK, &resid) == 0 && all(data, BLOCK, 0), "block 3 too: %#x", data[0]);
	CHECK(fake_disk_read("rsp0", 7 * 8, data, BLOCK, &resid) == 0 && all(data, BLOCK, 0xa2), "the last one that is here is still served");
	hakios_sparse_report();
	CHECK(logged("2 of those have an entry that points past the image's blocks: the image is damaged"), "and it is said: %s", log_text);
}

/* a header whose sizes do not fit the RAM disk, or fit it only because their sum wraps: no disk */
static void
does_not_fit(void)
{
	static const struct { unsigned at; uint64_t value; const char *what; } wrong[] = {
		{ AT_PRESENT, HERE + 3, "more blocks than the disk has room for" },
		{ AT_BLOCK_COUNT, (ROOT + IMAGE) / 4, "a table longer than the disk" },
		{ AT_DATA, IMAGE, "blocks that begin at the disk's end" },
		{ AT_TABLE, ~0ull - ROOT - 16, "a table whose offset wraps to inside the disk" },
		{ AT_DATA, ~0ull - ROOT + 1 + DATA_AT, "blocks whose offset wraps to where they are" },
		{ AT_BLOCK_COUNT, 1ull << 62, "a table whose length in bytes wraps to nothing" },
		{ AT_BLOCK_COUNT, (1ull << 52) + BLOCKS, "a disk whose size in bytes wraps" },
	};
	const char *which = getenv("SPARSE_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;

	image();
	put64(wrong[i].at, wrong[i].value);
	if (wrong[i].at == AT_BLOCK_COUNT && wrong[i].value > BLOCKS) {
		put64(AT_PRESENT, HERE);
	}
	hakios_sparse_init();
	CHECK(!there(), "%s: no disk: %s", wrong[i].what, log_text);
	CHECK(logged("sparse: the image does not fit in md0") || logged("sparse: the header makes no sense"), "%s: said: %s", wrong[i].what, log_text);
}

/* a header that says nothing sensible */
static void
header_makes_no_sense(void)
{
	static const struct { unsigned at; uint64_t value; } wrong[] = {
		{ AT_BLOCK_SIZE, 100 }, { AT_BLOCK_SIZE, 0 }, { AT_BLOCK_SIZE, 4097 }, { AT_BLOCK_COUNT, 0 }, { AT_PRESENT, 0 },
		{ AT_PRESENT, BLOCKS + 1 },
	};
	const char *which = getenv("SPARSE_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;

	image();
	if (wrong[i].at == AT_BLOCK_SIZE) {
		put32(wrong[i].at, (uint32_t)wrong[i].value);
	} else {
		put64(wrong[i].at, wrong[i].value);
	}
	hakios_sparse_init();
	CHECK(!there() && logged("sparse: the header makes no sense"), "case %u: no disk: %s", i, log_text);
}

/* not an image where the boot argument says one is */
static void
no_image_there(void)
{
	image();
	memory[ROOT + 13] = '2';
	hakios_sparse_init();
	CHECK(!there() && logged("sparse: no sparse image at offset 8192 in md0"), "no disk: %s", log_text);
}

/* an offset at the RAM disk's end, past it, and one that wraps with the header's size */
static void
offset_past_the_disk(void)
{
	static const char *const offsets[] = { "apfs_sparse=24576", "apfs_sparse=20560", "apfs_sparse=0xfffffffffffffff0" };
	const char *which = getenv("SPARSE_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;

	image();
	boot_args = offsets[i];
	hakios_sparse_init();
	CHECK(!there() && logged("is past the end of md0"), "%s: no disk: %s", offsets[i], log_text);
}

/* no RAM disk to read it from */
static void
no_ram_disk(void)
{
	image();
	no_disk = true;
	hakios_sparse_init();
	CHECK(!there() && logged("sparse: md0 is not a memory disk this can be read from"), "no disk: %s", log_text);
}

/* switches of other drivers, to take majors and names with */
static const struct cdevsw other_cdevsw;
static const struct bdevsw other_bdevsw;

/* the major the next block (or character) switch would get: taken to see, and given back */
static int
next_bmajor(void)
{
	int major = bdevsw_add(-1, &other_bdevsw);

	(void)bdevsw_remove(major, &other_bdevsw);
	return major;
}

static int
next_cmajor(void)
{
	int major = cdevsw_add(-1, &other_cdevsw);

	(void)cdevsw_remove(major, &other_cdevsw);
	return major;
}

/* no character major left for its switch: no disk, and the block major it took is free again */
static void
no_char_major(void)
{
	int bmajor;

	image();
	for (unsigned i = 0; i < 64; i++) {
		if (cdevsw_add(-1, &other_cdevsw) < 0) {
			break;          /* every one taken */
		}
	}
	bmajor = next_bmajor();
	hakios_sparse_init();
	CHECK(!there() && logged("sparse: cdevsw_add failed"), "no disk: %s", log_text);
	CHECK(next_bmajor() == bmajor, "the block major %d is given back: the next is %d", bmajor, next_bmajor());
}

/* the node's name already taken: no disk, and both majors it took are free again */
static void
name_taken(void)
{
	int bmajor, cmajor;

	image();
	CHECK(devfs_make_node(makedev(cdevsw_add(-1, &other_cdevsw), 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "rsp0") != NULL,
	    "the name taken");
	bmajor = next_bmajor();
	cmajor = next_cmajor();
	hakios_sparse_init();
	CHECK(!there() && logged("sparse: devfs_make_node failed"), "no disk: %s", log_text);
	CHECK(next_bmajor() == bmajor, "the block major %d is given back: the next is %d", bmajor, next_bmajor());
	CHECK(next_cmajor() == cmajor, "the character major %d is given back: the next is %d", cmajor, next_cmajor());
}

/* only when asked for */
static void
off_by_default(void)
{
	image();
	boot_args = "";
	hakios_sparse_init();
	CHECK(!there() && log_text[0] == 0, "without the boot argument: nothing, and nothing said: %s", log_text);
	boot_args = "apfs_sparse=0";
	hakios_sparse_init();
	CHECK(!there() && log_text[0] == 0, "nor with 0: %s", log_text);
	hakios_sparse_report();
	CHECK(log_text[0] == 0, "and nothing to report: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); unsigned cases; } scenarios[] = {
		{ "served", served, 1 }, { "across_blocks", across_blocks, 1 }, { "every_block_here", every_block_here, 1 }, { "read_only", read_only, 1 },
		{ "sector_that_wraps", sector_that_wraps, 1 },
		{ "table_points_outside", table_points_outside, 1 }, { "does_not_fit", does_not_fit, 7 },
		{ "header_makes_no_sense", header_makes_no_sense, 6 }, { "no_image_there", no_image_there, 1 },
		{ "offset_past_the_disk", offset_past_the_disk, 3 }, { "no_ram_disk", no_ram_disk, 1 }, { "off_by_default", off_by_default, 1 },
		{ "no_char_major", no_char_major, 1 }, { "name_taken", name_taken, 1 },
	};

	if (argc < 2) {
		/* a scenario with several cases is listed once for each: NAME:N (the driver's state is a process's) */
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			for (unsigned c = 0; c < scenarios[i].cases; c++) {
				if (scenarios[i].cases == 1) {
					printf("%s\n", scenarios[i].name);
				} else {
					printf("%s:%u\n", scenarios[i].name, c);
				}
			}
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		size_t length = strlen(scenarios[i].name);

		if (strncmp(argv[1], scenarios[i].name, length) == 0 && (argv[1][length] == 0 || argv[1][length] == ':')) {
			if (argv[1][length] == ':') {
				setenv("SPARSE_CASE", argv[1] + length + 1, 1);
			}
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
