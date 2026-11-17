/*
 * The checks of a processor's firmware image the DSP drivers share (drivers/board/mdt.h), as
 * they are: what is a header file, which segments are loaded, which lie in the processor's memory, and when
 * the header file is nothing but its three parts. The files come from a RAM disk; a count, an address or a
 * size made to be wrong must not get past.
 *
 *   mdt_test SCENARIO
 */
#include "fakekernel.h"
#include "board/mdt.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define MOST            16
#define LARGEST         0x4000
static uint8_t file[LARGEST + 64];

/* a header file of `count' segments and `hashes' bytes of hashes after its table */
static uint64_t
header(uint32_t count, uint32_t hashes)
{
	memset(file, 0, sizeof(file));
	memcpy(file, "\177ELF\1\1\1", 7);
	file[28] = MDT_ELF_HEADER;              /* e_phoff: the table right after the header */
	file[42] = MDT_SEGMENT;                 /* e_phentsize */
	file[44] = (uint8_t)count;
	file[45] = (uint8_t)(count >> 8);
	return MDT_ELF_HEADER + (uint64_t)MDT_SEGMENT * count + hashes;
}

static void
segment(uint32_t i, uint32_t type, uint32_t flags, uint32_t paddr, uint32_t file_size, uint32_t memory_size)
{
	struct elf_segment s = { .type = type, .paddr = paddr, .file_size = file_size, .memory_size = memory_size, .flags = flags };

	memcpy(file + MDT_ELF_HEADER + MDT_SEGMENT * i, &s, sizeof(s));
}

static void
headers(void)
{
	uint64_t size = header(4, 200);

	CHECK(mdt_segment_count(file, size, MOST, LARGEST) == 4, "four segments");
	CHECK(mdt_segment_count(file, header(2, 0), MOST, LARGEST) == 0, "fewer than three: not one");
	CHECK(mdt_segment_count(file, header(17, 0), MOST, LARGEST) == 0, "more than the driver takes: not one");
	size = header(4, 0);
	CHECK(mdt_segment_count(file, size - 1, MOST, LARGEST) == 0, "a table that does not fit the file: not one");
	header(4, 0);
	file[44] = 0xff, file[45] = 0xff;
	CHECK(mdt_segment_count(file, LARGEST, 0xffff, LARGEST) == 0, "65535 segments in a file of 16 KiB: not one");
	size = header(4, 0);
	file[4] = 2;                            /* a 64-bit ELF */
	CHECK(mdt_segment_count(file, size, MOST, LARGEST) == 0, "a 64-bit ELF: not one");
	size = header(4, 0);
	file[0] = 0;
	CHECK(mdt_segment_count(file, size, MOST, LARGEST) == 0, "no ELF at all: not one");
	header(4, 0);
	CHECK(mdt_segment_count(file, LARGEST + 1, MOST, LARGEST) == 0, "larger than what is handed over: not one");
	CHECK(mdt_segment_count(file, 4, MOST, LARGEST) == 0 && mdt_segment_count(NULL, 0, MOST, LARGEST) == 0,
	    "a file of four bytes, or none: not one, and nothing read past it");
	size = header(4, 0);
	file[5] = 2;                            /* big-endian */
	CHECK(mdt_segment_count(file, size, MOST, LARGEST) == 0, "a big-endian ELF: not one");
	size = header(4, 0);
	file[28] = MDT_ELF_HEADER + MDT_SEGMENT;
	CHECK(mdt_segment_count(file, size, MOST, LARGEST) == 0, "a table that is not right after the header: not one");
	size = header(4, 0);
	file[31] = 1;
	CHECK(mdt_segment_count(file, size, MOST, LARGEST) == 0, "a table 16 MiB on: not one");
	size = header(4, 0);
	file[42] = 56;
	CHECK(mdt_segment_count(file, size, MOST, LARGEST) == 0, "entries of 56 bytes (a 64-bit table): not one");
}

static void
segments(void)
{
	struct elf_segment s;

	header(4, 0);
	segment(0, 6, 0, 0, 0x34, 0x34);                        /* the ELF header itself (PT_PHDR-like): not loaded */
	segment(1, MDT_LOAD, 2u << 24, 0x9f000000, 0x1000, 0x1000);     /* the hashes */
	segment(2, MDT_LOAD, 0, 0x9f000000, 0x2000, 0x3000);
	segment(3, MDT_LOAD, 0, 0x9f003000, 0, 0);              /* empty */
	mdt_segment(file, 0, &s);
	CHECK(!mdt_loaded(&s), "a segment of another type is not loaded");
	mdt_segment(file, 1, &s);
	CHECK(MDT_HASH(s.flags) && !mdt_loaded(&s), "the hashes are not loaded");
	mdt_segment(file, 2, &s);
	CHECK(mdt_loaded(&s) && s.paddr == 0x9f000000 && s.file_size == 0x2000 && s.memory_size == 0x3000, "a segment to load, as written");
	mdt_segment(file, 3, &s);
	CHECK(!mdt_loaded(&s), "an empty one is not");
}

static void
within(void)
{
	struct elf_segment s = { .type = MDT_LOAD, .paddr = 0x9f000000, .file_size = 0x1000, .memory_size = 0x2000 };
	const uint64_t base = 0x9f000000, size = 0x100000;

	CHECK(mdt_within(&s, base, size), "at the start");
	s.paddr = base + size - 0x2000;
	CHECK(mdt_within(&s, base, size), "ending at the end");
	s.paddr = base + size - 0x1000;
	CHECK(!mdt_within(&s, base, size), "over the end");
	s.paddr = base - 0x1000;
	CHECK(!mdt_within(&s, base, size), "before the start");
	s.paddr = base;
	s.file_size = 0x3000;
	CHECK(!mdt_within(&s, base, size), "more in its file than in memory");
	/* sizes and addresses whose sums would wrap */
	s.file_size = 0;
	s.memory_size = 0xffffffffu;
	s.paddr = 0x9f001000;
	CHECK(!mdt_within(&s, base, size), "a memory size of nearly 4 GiB");
	s.memory_size = 0x1000;
	s.paddr = 0xfffff000u;
	CHECK(!mdt_within(&s, 0xfff00000ull, 0x1000), "at the top of 32 bits, past a small region");
	CHECK(mdt_within(&s, 0xfff00000ull, 0x100000), "and inside a region that reaches it");
}

/* exactly at each limit: the least a header can be, the most the driver is handed, three segments and the most it
 * takes; a segment with as much in its file as in memory, and an empty one at the memory's very end */
static void
boundaries(void)
{
	struct elf_segment s = { .type = MDT_LOAD, .paddr = 0x9f000000, .file_size = 0x2000, .memory_size = 0x2000 };
	uint64_t size = header(3, 0);

	CHECK(size == MDT_ELF_HEADER + 3 * MDT_SEGMENT && mdt_segment_count(file, size, MOST, LARGEST) == 3,
	    "three segments and nothing more: a header of the least size");
	header(MOST, 0);
	CHECK(mdt_segment_count(file, LARGEST, MOST, LARGEST) == MOST, "as many segments as the driver takes, in a file as large as it is handed");
	CHECK(mdt_segment_count(NULL, LARGEST, MOST, LARGEST) == 0, "no header, whatever its size: not one, and nothing read");
	CHECK(mdt_within(&s, 0x9f000000, 0x100000), "as much in its file as in memory");
	s.paddr = 0x9f100000;
	s.file_size = s.memory_size = 0;
	CHECK(mdt_within(&s, 0x9f000000, 0x100000), "nothing, at the memory's very end");
}

#define TABLE_END(n)    (MDT_ELF_HEADER + MDT_SEGMENT * (n))
#define HASHES          (2u << 24)              /* the flags of the block of hashes */

static void
hashes_at(uint32_t i, uint32_t offset, uint32_t size)
{
	struct elf_segment s = { .type = 0, .offset = offset, .file_size = size, .flags = HASHES };

	memcpy(file + MDT_ELF_HEADER + MDT_SEGMENT * i, &s, sizeof(s));
}

static void
whole(void)
{
	uint32_t hashes = 7;
	uint64_t size = header(4, 200);

	hashes_at(1, TABLE_END(4), 200);
	CHECK(mdt_header_whole(file, 4, size, &hashes) && hashes == 200, "the three parts and nothing else (%u)", hashes);
	CHECK(!mdt_header_whole(file, 4, size + 1, &hashes), "a byte more");
	hashes_at(1, TABLE_END(4), 0xffffffffu);
	CHECK(!mdt_header_whole(file, 4, (TABLE_END(4) + 0xffffffffull) & 0xffffffffu, &hashes),
	    "hashes that would wrap a 32-bit sum to the file's size");
	size = header(4, 0);
	CHECK(!mdt_header_whole(file, 4, size, &hashes) && hashes == 0, "no block of hashes, and nothing after the table");
	size = header(4, 200);
	hashes_at(1, TABLE_END(4) + 8, 200);
	CHECK(!mdt_header_whole(file, 4, size, &hashes), "hashes said to be elsewhere than after the table");
	hashes_at(1, TABLE_END(4), 200);
	hashes_at(2, TABLE_END(4), 200);
	CHECK(!mdt_header_whole(file, 4, size, &hashes), "two blocks of hashes");
	size = header(4, 0);
	hashes_at(1, TABLE_END(4), 0);
	CHECK(!mdt_header_whole(file, 4, size, &hashes), "a block of hashes of no bytes");
}

static void
apart(void)
{
	CHECK(mdt_apart(0, 0x1000, 0x1000, 0x1000) && mdt_apart(0x1000, 0x1000, 0, 0x1000), "end to end, either way round");
	CHECK(!mdt_apart(0, 0x1001, 0x1000, 0x1000) && !mdt_apart(0x1000, 0x1000, 0xfff, 0x10), "a byte over, either way round");
	CHECK(!mdt_apart(0x2000, 0x10, 0x1000, 0x2000), "one inside the other");
	CHECK(mdt_apart(0xfffff000u, 0x1000, 0, 0x10), "at the top of 32 bits, and one at 0");
	CHECK(!mdt_apart(0x1000, 0xffffffffu, 0x2000, 0x10), "a size that would wrap a 32-bit sum below the other");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "headers", headers }, { "segments", segments }, { "within", within }, { "whole", whole }, { "boundaries", boundaries },
		{ "apart", apart },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-10s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
