/*
 * The storage's driver itself (kernel/overlay/bsd/dev/hakios_ufs.c), run as a host program: the one
 * place of the kernel from which a write can reach a phone's flash. It asks the loader, which stays on
 * core 0 with UEFI's driver, through a page of memory and a buffer. So the model here is that loader: a
 * thread that answers what the page asks, over a made-up disk, and that notes every request -- above all
 * every write, with whether it is one the port may make: into its own partition, past what the build
 * puts there. The loader on a phone refuses the others of its own accord (loader/src/own_partition.c,
 * tests/loader_own_partition_test.c); this test is that the kernel never asks for one.
 *
 *   ufs_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];
void loader_log_putc(char c);

/* what the driver says, kept where logged() looks */
static void
said(const char *format, ...)
{
	char line[768];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/* (the kernel's printf takes %llu for a uint64_t, which on this host is an unsigned long of the same size) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#define printf said
#include "hakios_board.c"
#include "hakios_ufs.c"
#undef printf                           /* (the driver's is its own: what the test prints is the test's) */
#pragma GCC diagnostic pop

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- the loader: its page, its buffer, the disk behind it ----------------------------------------- */

#define PAGE_PA         0x9f704080ull           /* (not at the start of its page, as the loader's is not) */
#define BUFFER_PA       0x9f800000ull
#define BUFFER          0x10000u                /* 64 KiB: sixteen of darwin's blocks a request */
#define MAP_PAGE        0x4000ul

/* the partitions, as the loader's table has them */
enum { BOOT, UNIT0, DARWIN, SUPER, PARTS };
/* the board's storage { } (tests/devices/phone-video): darwin's place on its unit and where writing begins */
#define DARWIN_FIRST    25374720ull
#define DARWIN_BLOCKS   5242875ull
#define WRITE_FIRST     2042368ull

static _Alignas(16384) uint8_t page_memory[2 * MAP_PAGE + sizeof(struct loader_block)];
static struct loader_block *const page = (struct loader_block *)(void *)(page_memory + (PAGE_PA & (MAP_PAGE - 1)));
static _Alignas(16384) uint8_t buffer[BUFFER];
static unsigned mapped_elsewhere;

vm_offset_t
ml_io_map_unmappable(vm_offset_t phys, vm_size_t size __unused, unsigned int flags __unused)
{
	if (phys == (PAGE_PA & ~(MAP_PAGE - 1))) {
		return (vm_offset_t)page_memory;
	}
	if (phys == BUFFER_PA) {
		return (vm_offset_t)buffer;
	}
	mapped_elsewhere++;
	fprintf(stderr, "MODEL: %#lx mapped, which is neither the loader's page nor its buffer\n", (unsigned long)phys);
	return (vm_offset_t)buffer;
}

/* The disk: every byte of every partition is a function of where it is, until it is written. */
static uint8_t
made_up(uint32_t part, uint64_t offset)
{
	return (uint8_t)((offset >> 12) * 31 + (offset >> 9) * 7 + offset + part * 101);
}

#define KEPT            96                      /* blocks that were written, as they were written */
static struct { uint32_t part; uint64_t lba; uint8_t data[4096]; } kept[KEPT];
static unsigned kept_n;

static const uint8_t *
written(uint32_t part, uint64_t lba)
{
	for (unsigned i = kept_n; i-- > 0;) {
		if (kept[i].part == part && kept[i].lba == lba) {
			return kept[i].data;
		}
	}
	return NULL;
}

/* what was asked of the loader, in order */
#define ASKED           256
static struct { uint32_t op, part; uint64_t lba, count; } asked[ASKED];
static volatile unsigned asked_n;
static volatile unsigned writes_asked, wrong_writes, too_long;
static volatile int64_t answer_status;         /* what the loader answers instead of serving (0: it serves) */
static volatile bool silent;                   /* the loader has stopped answering */
static volatile bool list_in;                  /* this kernel's own transfer list is in the controller */
static unsigned flushes;

/* A write the port may make: into darwin, from the block past what the build puts there, inside the partition. */
static bool
allowed(uint32_t part, uint64_t lba, uint64_t count)
{
	return part == DARWIN && lba >= WRITE_FIRST && count <= DARWIN_BLOCKS && lba <= DARWIN_BLOCKS - count;
}

static void
serve(void)
{
	uint32_t op = page->ask.op, part = page->ask.part;
	uint64_t lba = page->ask.lba, count = page->ask.count, size;

	if (asked_n < ASKED) {
		asked[asked_n].op = op;
		asked[asked_n].part = part;
		asked[asked_n].lba = lba;
		asked[asked_n].count = count;
	}
	asked_n++;
	page->answer.status = 0;
	if (part >= PARTS || count == 0 || count > BUFFER / page->part[part].block_size) {
		too_long++;
		page->answer.status = (int64_t)0x8000000000000002ull;   /* (EFI_INVALID_PARAMETER) */
		return;
	}
	size = page->part[part].block_size;
	if (op == BLOCK_OP_WRITE) {
		writes_asked++;
		if (!allowed(part, lba, count)) {
			wrong_writes++;
			page->answer.status = (int64_t)0x8000000000000008ull;   /* (EFI_WRITE_PROTECTED: the loader's own refusal) */
			return;
		}
	}
	if (answer_status != 0) {
		page->answer.status = answer_status;
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		if (op == BLOCK_OP_WRITE) {
			if (kept_n < KEPT) {
				kept[kept_n].part = part;
				kept[kept_n].lba = lba + i;
				memcpy(kept[kept_n].data, buffer + i * size, size);
				kept_n++;
			}
		} else if (written(part, lba + i) != NULL) {
			memcpy(buffer + i * size, written(part, lba + i), size);
		} else {
			for (uint64_t b = 0; b < size; b++) {
				buffer[i * size + b] = made_up(part, (lba + i) * size + b);
			}
		}
	}
}

/* core 0: it looks at the page, serves what is asked and says so */
static void *
core0(void *unused __unused)
{
	for (;;) {
		uint32_t request;

		__sync_synchronize();
		request = page->ask.request;
		if (silent || request == page->answer.answered) {
			usleep(20);
			continue;
		}
		serve();
		__sync_synchronize();
		page->answer.answered = request;
		__sync_synchronize();
	}
	return NULL;
}

void FlushPoC_DcacheRegion(vm_offset_t va __unused, size_t length __unused) { flushes++; }
void loader_return(void) {}

/* this kernel's own driver of the controller (hakios_ufshc.c) is not part of this: it is there, and not used */
static unsigned switched_out;
bool ufshc_probe(const struct ufshc_loader *loader __unused, bool only __unused) { return false; }
bool ufshc_available(void) { return false; }
void ufshc_set_luns(uint32_t mask __unused) {}
int ufshc_switch_in(bool prove __unused) { return EIO; }
int ufshc_switch_out(const char *why __unused) { switched_out++; return 0; }
bool ufshc_list_in(void) { return list_in; }
int ufshc_read(uint8_t lun __unused, uint64_t offset __unused, uint8_t *dst __unused, uint64_t len __unused) { return EIO; }
void ufshc_report(const char *when __unused) {}

static void
partition(uint32_t index, const char *name, uint32_t block_size, uint64_t blocks, uint64_t start_lba, uint8_t flags)
{
	snprintf(page->part[index].name, sizeof(page->part[index].name), "%s", name);
	page->part[index].block_size = block_size;
	page->part[index].blocks = blocks;
	page->part[index].start_lba = start_lba;
	page->part[index].unit = 0;
	page->part[index].lun = 0xff;
	page->part[index].flags = flags;
}

/* The phone as the loader leaves it for a kernel: its page filled in, core 0 serving it. */
static void
loader(const char *arguments)
{
	static char line[160];
	pthread_t thread;

	board = "phone-video";
	fake_delay_divisor = 1000;              /* the driver gives a silent loader 20 s: tens of milliseconds here */
	memset(page, 0, sizeof(*page));
	page->magic = BLOCK_MAGIC;
	page->version = BLOCK_VERSION;
	page->size = sizeof(*page);
	page->parts = PARTS;
	page->buffer_size = BUFFER;
	page->buffer_pa = BUFFER_PA;
	partition(BOOT, "boot", 512, 64, 2048, 0);
	partition(UNIT0, "unit0", 4096, 30617600, 0, BLOCK_PART_UNIT);
	partition(DARWIN, "darwin", 4096, DARWIN_BLOCKS, DARWIN_FIRST, 0);
	partition(SUPER, "super", 4096, 32, 4096, 0);
	page->write_part_plus_one = DARWIN + 1;         /* what a loader started with ufs_write=1 offers */
	page->write_first = WRITE_FIRST;
	page->answer.answered = 40;                     /* (it has served a kernel before this one) */
	page->ask.request = 40;
	snprintf(line, sizeof(line), "ufs_shared_pa=%#llx %s", PAGE_PA, arguments);
	boot_args = line;
	pthread_create(&thread, NULL, core0, NULL);
}

/* the bytes are the disk's own, from that offset of the partition on */
static bool
is_disk(uint32_t part, uint64_t offset, const uint8_t *data, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (data[i] != made_up(part, offset + i)) {
			return false;
		}
	}
	return true;
}

static uint64_t n(const char *name) { return get64(name); }
#define AT(block)       ((off_t)((block) * 4096ull))

/* ---- scenarios ------------------------------------------------------------------------------------ */

/* the partitions become devices, the whole units do not; a disk answers what a program asks of it */
static void
partitions_listed(void)
{
	uint8_t data[4096];
	size_t resid = 9;
	uint32_t size = 0, writable = 9;
	uint64_t count = 0;

	loader("");
	hakios_ufs_init();
	CHECK(logged("ufs: 3 partitions of the phone's storage as /dev/ufs_NAME and /dev/rufs_NAME, read only"), "said: %s", log_text);
	CHECK(fake_disk_open("ufs_boot", FREAD) == 0 && fake_disk_open("ufs_darwin", FREAD) == 0 && fake_disk_open("ufs_super", FREAD) == 0,
	    "the three partitions are block devices");
	CHECK(fake_dev_open("rufs_boot", FREAD) == 0 && fake_dev_open("rufs_darwin", FREAD) == 0, "and character devices");
	CHECK(fake_disk_open("ufs_unit0", FREAD) == -1 && fake_dev_open("rufs_unit0", FREAD) == -1, "a whole unit is no device");
	CHECK(fake_disk_read("ufs_boot", 3, data, 1024, &resid) == 0 && resid == 0 && is_disk(BOOT, 3 * 512, data, 1024), "sectors 3 and 4 of boot");
	CHECK(asked_n == 1 && asked[0].op == BLOCK_OP_READ && asked[0].part == BOOT && asked[0].lba == 3 && asked[0].count == 2,
	    "one request of the loader: blocks %llu, %llu of them", (unsigned long long)asked[0].lba, (unsigned long long)asked[0].count);
	/* what a program asks a disk: darwin is counted in its own blocks until it is told 512 */
	CHECK(fake_disk_ioctl("ufs_darwin", DKIOCGETBLOCKSIZE, &size) == 0 && size == 4096, "blocks of %u", size);
	CHECK(fake_disk_ioctl("ufs_darwin", DKIOCGETBLOCKCOUNT, &count) == 0 && count == DARWIN_BLOCKS, "%llu of them", (unsigned long long)count);
	size = 512;
	CHECK(fake_disk_ioctl("ufs_darwin", DKIOCSETBLOCKSIZE, &size) == 0, "counted in 512");
	CHECK(fake_disk_ioctl("ufs_darwin", DKIOCGETBLOCKCOUNT, &count) == 0 && count == DARWIN_BLOCKS * 8, "then %llu", (unsigned long long)count);
	CHECK(fake_disk_ioctl("ufs_darwin", DKIOCGETPHYSICALBLOCKSIZE, &size) == 0 && size == 4096, "the flash's own blocks stay 4096");
	CHECK(fake_disk_ioctl("ufs_darwin", DKIOCGETMAXBLOCKCOUNTREAD, &count) == 0 && count == BUFFER / 512, "a request is a bufferful at most");
	CHECK(fake_disk_read("ufs_darwin", 8 * 5 + 1, data, 512, &resid) == 0 && is_disk(DARWIN, 5 * 4096 + 512, data, 512), "and a sector is read as one");
	for (uint32_t wrong = 0; wrong < 4; wrong++) {
		static const uint32_t sizes[] = { 256, 8192, 1000, 0 };

		size = sizes[wrong];
		CHECK(fake_disk_ioctl("ufs_darwin", DKIOCSETBLOCKSIZE, &size) == EINVAL, "not counted in %u", size);
	}
	CHECK(fake_disk_ioctl("ufs_darwin", DKIOCISWRITABLE, &writable) == 0 && writable == 0, "the block device is not writable");
	CHECK(fake_disk_ioctl("ufs_darwin", 0x1234, &size) == ENOTTY, "nothing else is answered");
	CHECK(writes_asked == 0 && mapped_elsewhere == 0, "no write was asked for, nothing else was mapped");
}

/* a read is of bytes: from any offset, of any length, over as many of the loader's buffers as it takes */
static void
reads_any_alignment(void)
{
	static uint8_t data[3 * BUFFER];
	size_t done = 0, resid = 0;
	uint64_t end = 32 * 4096;

	loader("");
	hakios_ufs_init();
	CHECK(fake_dev_pread("rufs_darwin", data, 5000, AT(7) + 100, &done) == 0 && done == 5000 && is_disk(DARWIN, 7 * 4096 + 100, data, 5000),
	    "5000 bytes from the middle of block 7");
	CHECK(asked_n == 1 && asked[0].lba == 7 && asked[0].count == 2, "the two blocks that hold them: %llu, %llu",
	    (unsigned long long)asked[0].lba, (unsigned long long)asked[0].count);
	asked_n = 0;
	CHECK(fake_dev_pread("rufs_darwin", data, 2 * BUFFER + 4096, AT(100), &done) == 0 && done == 2 * BUFFER + 4096 &&
	    is_disk(DARWIN, 100 * 4096, data, 2 * BUFFER + 4096), "more than two buffers");
	CHECK(asked_n == 3 && asked[0].count == 16 && asked[1].lba == 116 && asked[1].count == 16 && asked[2].lba == 132 && asked[2].count == 1,
	    "in three requests, none longer than the buffer: %u", asked_n);
	CHECK(too_long == 0, "the loader was never asked for more than its buffer holds");
	/* super is 32 blocks: a read that reaches its end gets what there is */
	CHECK(fake_dev_pread("rufs_super", data, 8192, (off_t)end - 4096 - 10, &done) == 0 && done == 4106 &&
	    is_disk(SUPER, end - 4106, data, 4106), "up to the end: %zu bytes", done);
	CHECK(fake_dev_pread("rufs_super", data, 4096, (off_t)end, &done) == 0 && done == 0, "at the end: none");
	CHECK(fake_dev_pread("rufs_super", data, 4096, -4096, &done) == EINVAL, "before the start: refused");
	asked_n = 0;
	CHECK(fake_disk_read("ufs_super", 31, data, 8192, &resid) == 0 && resid == 4096 && is_disk(SUPER, 31 * 4096, data, 4096),
	    "a block request over the end is cut there: %zu left", resid);
	CHECK(fake_disk_read("ufs_super", 32, data, 4096, &resid) == 0 && resid == 4096, "at the end: nothing, and no error");
	CHECK(fake_disk_read("ufs_super", 33, data, 4096, &resid) == EINVAL, "past it: refused");
	CHECK(asked_n == 1, "and only the first of those three reached the loader: %u", asked_n);
	CHECK(n("ufs_errors") == 0 && n("ufs_requests") == 6, "counted: %llu requests", (unsigned long long)n("ufs_requests"));
}

/* without ufs_write=1 nothing of the storage takes a write, whoever asks and wherever */
static void
read_only_by_default(void)
{
	static uint8_t data[4096];
	size_t done = 9, resid = 0;

	loader("");
	hakios_ufs_init();
	CHECK(try_as_root("var_busy", 1) == 0, "(the writer has said it is the one)");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EROFS && done == 0, "darwin, where a write would go: refused");
	CHECK(fake_dev_pwrite("rufs_boot", data, 512, 0, &done) == EROFS, "boot: refused");
	CHECK(fake_dev_open("rufs_darwin", FREAD | FWRITE) == EROFS && fake_disk_open("ufs_darwin", FREAD | FWRITE) == EROFS, "not opened for writing");
	CHECK(fake_disk_write("ufs_darwin", WRITE_FIRST * 8, data, 4096, &resid) == EROFS, "the block device: refused");
	CHECK(asked_n == 0 && writes_asked == 0, "and the loader was asked nothing: %u", asked_n);
	CHECK(!logged("takes writes"), "nothing is said of writing: %s", log_text);
}

/* with it, the port's own partition takes whole blocks from the block past what the build puts there, and nothing else does */
static void
writes_only_past_the_build(void)
{
	static uint8_t data[3 * 4096], back[3 * 4096];
	size_t done = 9, resid = 0;

	loader("ufs_write=1");
	hakios_ufs_init();
	CHECK(logged("ufs: /dev/rufs_darwin takes writes, whole blocks from 2042368 on"), "said: %s", log_text);
	for (unsigned i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(i * 13 + 5);
	}
	CHECK(try_as_root("var_busy", 1) == 0, "the writer says it is the one");
	CHECK(fake_dev_open("rufs_darwin", FREAD | FWRITE) == 0, "darwin's character device opens for writing");
	/* the first block that may be written, and the two after it */
	CHECK(fake_dev_pwrite("rufs_darwin", data, sizeof(data), AT(WRITE_FIRST), &done) == 0 && done == sizeof(data), "three blocks from the first one allowed");
	CHECK(writes_asked == 1 && asked[0].op == BLOCK_OP_WRITE && asked[0].part == DARWIN && asked[0].lba == WRITE_FIRST && asked[0].count == 3,
	    "one write asked of the loader: blocks %llu, %llu of them", (unsigned long long)asked[0].lba, (unsigned long long)asked[0].count);
	CHECK(kept_n == 3 && memcmp(kept[0].data, data, 4096) == 0 && memcmp(kept[2].data, data + 8192, 4096) == 0, "with the writer's bytes");
	CHECK(flushes >= 1, "which left this core's cache before the loader was asked");
	CHECK(fake_dev_pread("rufs_darwin", back, sizeof(back), AT(WRITE_FIRST), &done) == 0 && memcmp(back, data, sizeof(data)) == 0, "and they read back");
	/* the last block of the partition */
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(DARWIN_BLOCKS - 1), &done) == 0 && done == 4096, "the last block");
	/* the block before the first one allowed is the build's: the iOS volume and the cryptexes */
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST - 1), &done) == EROFS && done == 0, "the block before: refused");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 8192, AT(WRITE_FIRST - 1), &done) == EROFS && done == 0, "nor a write that begins there and goes on into what is allowed");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, 0, &done) == EROFS, "the partition's first block: refused");
	/* past the partition's end */
	CHECK(fake_dev_pwrite("rufs_darwin", data, 8192, AT(DARWIN_BLOCKS - 1), &done) == EROFS && done == 0, "over the end: refused whole");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(DARWIN_BLOCKS), &done) == EROFS, "at the end: refused");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(DARWIN_BLOCKS + 100), &done) == EROFS, "past it: refused");
	/* whole blocks only: nothing is read before a write */
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST) + 512, &done) == EINVAL, "from the middle of a block: refused");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 1000, AT(WRITE_FIRST), &done) == EINVAL, "part of a block: refused");
	/* every other partition, and darwin's block device */
	CHECK(fake_dev_pwrite("rufs_super", data, 4096, 0, &done) == EROFS && fake_dev_pwrite("rufs_boot", data, 512, 0, &done) == EROFS, "another partition: refused");
	CHECK(fake_dev_open("rufs_super", FREAD | FWRITE) == EROFS, "which does not open for writing either");
	CHECK(fake_disk_write("ufs_darwin", WRITE_FIRST * 8, data, 4096, &resid) == EROFS && fake_disk_open("ufs_darwin", FWRITE) == EROFS,
	    "darwin's block device, where a file system would be: refused");
	CHECK(writes_asked == 2 && wrong_writes == 0, "of all that, the loader was asked for the two writes that are the port's: %u, %u of them wrong",
	    writes_asked, wrong_writes);
	CHECK(n("ufs_writes") == 2 && n("ufs_write_bytes") == 4 * 4096 && n("ufs_write_refused") >= 6 && n("ufs_write_errors") == 0,
	    "counted: %llu writes, %llu bytes, %llu refused here", (unsigned long long)n("ufs_writes"), (unsigned long long)n("ufs_write_bytes"),
	    (unsigned long long)n("ufs_write_refused"));
	CHECK(fake_dev_close("rufs_darwin") == 0, "closed");
}

/* a long write goes a bufferful at a time, and arrives whole */
static void
long_write(void)
{
	static uint8_t data[2 * BUFFER + 8192];
	size_t done = 0;
	bool same = true;

	loader("ufs_write=1");
	hakios_ufs_init();
	for (unsigned i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(i ^ (i >> 11));
	}
	CHECK(try_as_root("var_busy", 1) == 0, "(the writer has said it is the one)");
	CHECK(fake_dev_pwrite("rufs_darwin", data, sizeof(data), AT(WRITE_FIRST + 1000), &done) == 0 && done == sizeof(data), "34 blocks: %zu bytes went", done);
	CHECK(writes_asked == 3 && asked[0].count == 16 && asked[1].lba == WRITE_FIRST + 1016 && asked[1].count == 16 && asked[2].lba == WRITE_FIRST + 1032 &&
	    asked[2].count == 2, "in three requests, none longer than the buffer: %u", writes_asked);
	for (unsigned i = 0; i < 34; i++) {
		same = same && written(DARWIN, WRITE_FIRST + 1000 + i) != NULL && memcmp(written(DARWIN, WRITE_FIRST + 1000 + i), data + i * 4096, 4096) == 0;
	}
	CHECK(same && kept_n == 34, "every block has the writer's bytes: %u blocks", kept_n);
	CHECK(too_long == 0 && wrong_writes == 0, "and nothing else was asked for");
}

/* one writer at a time, and it says so first; a claim left by a process that is gone can be taken */
static void
writer_claims_first(void)
{
	static uint8_t data[4096];
	size_t done = 0;

	loader("ufs_write=1");
	hakios_ufs_init();
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EPERM && writes_asked == 0, "without the claim: refused");
	CHECK(try_as_root("var_busy", 7) == EINVAL, "the claim is 1 or 0");
	fake_pid = 200;
	fake_other_pid = 200;
	CHECK(try_as_root("var_busy", 1) == 0 && get("var_busy") == 200, "another process takes it: held by %u", get("var_busy"));
	fake_pid = 100;                 /* this one, while the other is alive */
	CHECK(try_as_root("var_busy", 1) == EBUSY, "then it is not to be had");
	CHECK(try_as_root("var_busy", 0) == EPERM, "nor given back by one that does not hold it");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EPERM && writes_asked == 0, "and a write without it is refused");
	fake_other_pid = 0;             /* the holder was killed */
	CHECK(try_as_root("var_busy", 1) == 0 && get("var_busy") == 100, "left by a process that is gone, it is taken");
	CHECK(logged("var_store: the claim on the store was left by process 200, which is gone"), "and that is said: %s", log_text);
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == 0 && writes_asked == 1, "and its holder writes");
	CHECK(try_as_root("var_busy", 0) == 0 && get("var_busy") == 0, "given back");
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EPERM && writes_asked == 1, "after which a write is refused again");
}

/*
 * ufs_write=1 is not enough: the loader has to offer the same partition from the same block, and that partition has to be the
 * port's own in everything the table says of it. Otherwise nothing takes a write.
 */
static void
partition_must_be_the_ports(void)
{
	static const char *const cases[] = {
		"the loader offers no partition to write into",         /* 0: a loader without ufs_write=1 */
		"the loader offers another partition",                  /* 1 */
		"the loader offers no partition to write into",         /* 2: from another block than the board's */
		"it does not begin where it did",                       /* 3 */
		"it is not the size it was",                            /* 4 */
		"its blocks are not 4096 bytes",                        /* 5 */
		"it is not on the first logical unit",                  /* 6 */
		"darwin is not among the partitions",                   /* 7 */
		"the loader offers no partition to write into",         /* 8: an offer past the table */
		"this board's file does not say which partition is the port's own",     /* 9 */
	};
	const char *which = getenv("UFS_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;
	static uint8_t data[4096];
	size_t done = 0;

	loader("ufs_write=1");
	switch (i) {
	case 0: page->write_part_plus_one = 0; break;
	case 1: page->write_part_plus_one = SUPER + 1; break;
	case 2: page->write_first = WRITE_FIRST - 1; break;
	case 3: page->part[DARWIN].start_lba = DARWIN_FIRST + 8; break;
	case 4: page->part[DARWIN].blocks = DARWIN_BLOCKS + 1; break;
	case 5: page->part[DARWIN].block_size = 512; break;
	case 6: page->part[DARWIN].unit = 1; break;
	case 7: snprintf(page->part[DARWIN].name, sizeof(page->part[DARWIN].name), "darwin2"); break;
	case 8: page->write_part_plus_one = PARTS + 1; break;
	default: board = "phone-cmd"; break;            /* (a board with no storage { }) */
	}
	hakios_ufs_init();
	CHECK(logged(cases[i]), "case %u: said why: %s", i, log_text);
	CHECK(!logged("takes writes, whole blocks"), "case %u: and not that anything takes writes: %s", i, log_text);
	CHECK(try_as_root("var_busy", 1) == 0, "(the writer has said it is the one)");
	CHECK(fake_dev_pwrite(i == 7 ? "rufs_darwin2" : "rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EROFS && done == 0, "case %u: a write is refused", i);
	CHECK(fake_dev_pwrite("rufs_super", data, 4096, 0, &done) == EROFS, "case %u: on the partition the loader named too", i);
	CHECK(asked_n == 0, "case %u: the loader was asked nothing: %u", i, asked_n);
	CHECK(fake_dev_pread("rufs_super", data, 4096, 0, &done) == 0 && is_disk(SUPER, 0, data, 4096), "case %u: reading goes on as it did", i);
}

/* what the loader refuses or fails is an error to whoever asked, with what did not go */
static void
loader_fails(void)
{
	static uint8_t data[BUFFER + 4096];
	size_t done = 9;

	loader("ufs_write=1");
	hakios_ufs_init();
	CHECK(try_as_root("var_busy", 1) == 0, "(the writer has said it is the one)");
	answer_status = (int64_t)0x8000000000000007ull;         /* (EFI_DEVICE_ERROR) */
	CHECK(fake_dev_pread("rufs_super", data, 4096, 0, &done) == EIO && done == 0, "a read the loader fails: an error, no bytes");
	CHECK(logged("ufs: super: blocks 0..0: the loader's read failed (status 8000000000000007)"), "said: %s", log_text);
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EIO && done == 0, "a write it fails: an error, nothing counted as written");
	CHECK(logged("the loader's write failed"), "said: %s", log_text);
	CHECK(n("ufs_errors") == 2 && n("ufs_write_errors") == 1 && n("ufs_writes") == 0, "counted: %llu errors, %llu of writing",
	    (unsigned long long)n("ufs_errors"), (unsigned long long)n("ufs_write_errors"));
	answer_status = 0;
	CHECK(fake_dev_pread("rufs_super", data, 4096, 0, &done) == 0 && done == 4096, "and the next request is asked as any other");
}

/* a loader that stops answering is given up on once, and asked nothing more */
static void
loader_goes_silent(void)
{
	static uint8_t data[4096];
	size_t done = 9;
	unsigned before;

	loader("ufs_write=1");
	hakios_ufs_init();
	CHECK(fake_dev_pread("rufs_super", data, 4096, 0, &done) == 0, "(a read while it answers)");
	silent = true;
	CHECK(fake_dev_pread("rufs_super", data, 4096, AT(1), &done) == EIO && done == 0, "no answer: an error in the end");
	CHECK(logged("ufs: no answer from the loader to request 42 in 20 s: the storage is off from now on"), "said: %s", log_text);
	before = page->ask.request;
	CHECK(try_as_root("var_busy", 1) == 0, "(the writer has said it is the one)");
	CHECK(fake_dev_pread("rufs_super", data, 4096, AT(2), &done) == EIO && fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EIO,
	    "after that every request is an error at once");
	CHECK(page->ask.request == before, "and nothing more is put into the page: %u, %u", page->ask.request, before);
}

/* while this kernel's own transfer list is in the controller, UEFI's driver must not be asked to ring: neither read nor write */
static void
not_while_the_list_is_in(void)
{
	static uint8_t data[4096];
	size_t done = 9;

	loader("ufs_write=1");
	hakios_ufs_init();
	CHECK(try_as_root("var_busy", 1) == 0, "(the writer has said it is the one)");
	list_in = true;
	CHECK(fake_dev_pread("rufs_super", data, 4096, 0, &done) == EIO && done == 0, "a read: an error");
	CHECK(logged("ufs: super: not asked of the loader while this kernel's own transfer list is in"), "said: %s", log_text);
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EIO && done == 0, "a write: an error");
	CHECK(asked_n == 0, "and the loader was asked nothing: %u", asked_n);
	list_in = false;
	CHECK(fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == 0 && writes_asked == 1, "with the list out, the write goes");
}

/* a page that is not the loader's, or that says things no request could be made with: no devices */
static void
page_not_usable(void)
{
	static const char *const cases[] = {
		"is not one this kernel knows", "is not one this kernel knows", "is not one this kernel knows", "is not one this kernel knows",
		"its buffer is not whole blocks of 4096 bytes", "its buffer is not whole blocks of 4096 bytes", "its buffer is not whole blocks of 4096 bytes",
		"a partition's block size is not a power of two", "a partition's block size is not a power of two", "a partition's block size is not a power of two",
		"a partition's size in bytes is more than 64 bits hold", "a partition's name has no end",
	};
	const char *which = getenv("UFS_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;

	loader("ufs_write=1");
	switch (i) {
	case 0: page->magic ^= 1; break;
	case 1: page->version = BLOCK_VERSION + 1; break;
	case 2: page->size = sizeof(*page) - 128; break;
	case 3: page->parts = BLOCK_MAX_PARTS + 1; break;
	case 4: page->buffer_size = 0; break;
	case 5: page->buffer_size = 1000; break;
	case 6: page->buffer_pa = 0; break;
	case 7: page->part[BOOT].block_size = 0; break;
	case 8: page->part[SUPER].block_size = 3000; break;
	case 9: page->part[SUPER].block_size = 2 * BUFFER; break;
	case 10: page->part[SUPER].blocks = 1ull << 60; break;
	default: memset(page->part[BOOT].name, 'b', sizeof(page->part[BOOT].name)); break;
	}
	hakios_ufs_init();
	CHECK(logged(cases[i]), "case %u: said: %s", i, log_text);
	CHECK(fake_dev_open("rufs_darwin", FREAD) == -1 && fake_disk_open("ufs_super", FREAD) == -1 && fake_disk_open("ufs_boot", FREAD) == -1,
	    "case %u: no devices", i);
	CHECK(asked_n == 0, "case %u: the loader was asked nothing", i);
}

/* the run is ending: nothing more is asked of the loader */
static void
closing(void)
{
	static uint8_t data[4096];
	size_t done = 9;

	loader("ufs_write=1");
	hakios_ufs_init();
	CHECK(try_as_root("var_busy", 1) == 0, "(the writer has said it is the one)");
	CHECK(fake_dev_pread("rufs_super", data, 4096, 0, &done) == 0, "(a read before)");
	hakios_ufs_close();
	CHECK(fake_dev_pread("rufs_super", data, 4096, AT(1), &done) == EIO && fake_dev_pwrite("rufs_darwin", data, 4096, AT(WRITE_FIRST), &done) == EIO,
	    "after the close: errors");
	CHECK(asked_n == 1, "and the loader saw the one request from before: %u", asked_n);
}

/* the data volume's saver is asked on the way out, and waited for no longer than said */
static void *
saver(void *unused __unused)
{
	while (get("var_save") != VAR_SAVE_WANTED) {
		usleep(200);
	}
	(void)try_as_root("var_save", VAR_SAVE_DONE);
	return NULL;
}

static void
saver_is_asked(void)
{
	pthread_t thread;

	loader("ufs_write=1 var_save_ms=400");
	hakios_ufs_init();
	hakios_var_save_on_return();
	CHECK(!logged("var_store:"), "no saver registered: nothing is asked, nothing waited for: %s", log_text);
	CHECK(try_as_root("var_save", VAR_SAVE_DONE) == EINVAL, "done is an answer, not a thing to say unasked");
	CHECK(try_as_root("var_save", VAR_SAVE_IDLE) == 0 && get("var_save") == VAR_SAVE_IDLE, "a saver registers");
	pthread_create(&thread, NULL, saver, NULL);
	hakios_var_save_on_return();
	pthread_join(thread, NULL);
	CHECK(logged("var_store: the run is ending: the data volume is asked for") && logged("var_store: the saver has finished after"),
	    "asked, and its answer taken: %s", log_text);
	/* one that registered and then does not answer */
	CHECK(try_as_root("var_save", VAR_SAVE_NONE) == 0 && try_as_root("var_save", VAR_SAVE_IDLE) == 0, "(registered again)");
	hakios_var_save_on_return();
	CHECK(logged("var_store: the saver did not answer in 400 ms: the store keeps what it had"), "not waited for longer than said: %s", log_text);
}

/* a loader that was not asked for the storage: nothing */
static void
off_without_the_loader(void)
{
	loader("");
	boot_args = "ufs_write=1";
	hakios_ufs_init();
	CHECK(log_text[0] == 0 && fake_dev_open("rufs_darwin", FREAD) == -1, "no devices, nothing said: %s", log_text);
	hakios_ufs_close();
	hakios_var_save_on_return();
	CHECK(log_text[0] == 0 && asked_n == 0, "nor on the way out: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); unsigned cases; } scenarios[] = {
		{ "partitions_listed", partitions_listed, 1 }, { "reads_any_alignment", reads_any_alignment, 1 },
		{ "read_only_by_default", read_only_by_default, 1 }, { "writes_only_past_the_build", writes_only_past_the_build, 1 },
		{ "long_write", long_write, 1 }, { "writer_claims_first", writer_claims_first, 1 },
		{ "partition_must_be_the_ports", partition_must_be_the_ports, 10 }, { "loader_fails", loader_fails, 1 },
		{ "loader_goes_silent", loader_goes_silent, 1 }, { "not_while_the_list_is_in", not_while_the_list_is_in, 1 },
		{ "page_not_usable", page_not_usable, 12 }, { "closing", closing, 1 }, { "saver_is_asked", saver_is_asked, 1 },
		{ "off_without_the_loader", off_without_the_loader, 1 },
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
				setenv("UFS_CASE", argv[1] + length + 1, 1);
			}
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			/* whatever the scenario, the loader was never asked for a write that is not the port's */
			CHECK(wrong_writes == 0 && too_long == 0 && mapped_elsewhere == 0, "%u writes outside what the port may write, %u requests no loader could serve",
			    wrong_writes, too_long);
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
