/*
 * The reading of the processors' shared memory that the drivers share
 * (kernel/overlay/bsd/dev/hakios_smem.h), against areas built here: one as a boot loader lays it out,
 * and ones whose headers say wrong things, which the reading must not follow outside the area.
 *
 *   smem_test SCENARIO
 */
#include "fakekernel.h"

#define KERNEL 1
#include "hakios_smem.h"
#include "hakios_smp2p.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- an area, built the way smem.c reads one ------------------------------------------------------ */

#define SIZE            0x10000u
#define TABLE           (SIZE - 0x1000u)
static uint8_t bytes[SIZE + 64];                /* (what lies after the area is the canary's) */
static const struct hakios_smem area = { bytes, SIZE };
static unsigned partitions;

static void
empty(void)
{
	struct smem_table table = { .magic = "$TOC", .version = 1 };

	memset(bytes, 0, SIZE);
	memset(bytes + SIZE, 0xee, 64);
	memcpy(bytes + TABLE, &table, sizeof(table));
	partitions = 0;
}

/* a partition of two processors at that offset: its line in the table and its header */
static void
partition(uint32_t offset, uint32_t size, uint16_t one, uint16_t other, uint32_t cacheline)
{
	struct smem_table_entry entry = { .offset = offset, .size = size, .host0 = one, .host1 = other, .cacheline = cacheline };
	struct smem_partition header = { .magic = "$PRT", .host0 = one, .host1 = other, .size = size,
		                         .free_uncached = sizeof(header), .free_cached = size };
	struct smem_table table;

	memcpy(bytes + TABLE + sizeof(table) + partitions * sizeof(entry), &entry, sizeof(entry));
	memcpy(&table, bytes + TABLE, sizeof(table));
	table.count = ++partitions;
	memcpy(bytes + TABLE, &table, sizeof(table));
	memcpy(bytes + offset, &header, sizeof(header));
}

/* an item allocated from the bottom of the partition at `at': an entry, then its bytes; returns where the bytes are */
static uint32_t
from_the_bottom(uint32_t at, uint16_t item, const void *data, uint32_t length, uint16_t padding)
{
	struct smem_partition header;
	struct smem_entry entry = { .canary = SMEM_CANARY, .item = item, .size = length + padding, .padding_data = padding };
	uint32_t where;

	memcpy(&header, bytes + at, sizeof(header));
	where = at + header.free_uncached + sizeof(entry);
	memcpy(bytes + at + header.free_uncached, &entry, sizeof(entry));
	memcpy(bytes + where, data, length);
	header.free_uncached += sizeof(entry) + entry.size;
	memcpy(bytes + at, &header, sizeof(header));
	return where;
}

/* ... and one from the top: its bytes, then its entry behind them, aligned to the cache line */
static uint32_t
from_the_top(uint32_t at, uint16_t item, const void *data, uint32_t length, uint32_t cacheline)
{
	struct smem_partition header;
	struct smem_entry entry = { .canary = SMEM_CANARY, .item = item, .size = (length + cacheline - 1) & ~(cacheline - 1),
		                    .padding_data = (uint16_t)(((length + cacheline - 1) & ~(cacheline - 1)) - length) };
	uint32_t step = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1), entry_at, where;

	memcpy(&header, bytes + at, sizeof(header));
	entry_at = header.free_cached - step;
	where = entry_at - entry.size;
	memcpy(bytes + at + entry_at, &entry, sizeof(entry));
	memcpy(bytes + at + where, data, length);
	header.free_cached = where;
	memcpy(bytes + at, &header, sizeof(header));
	return at + where;
}

#define US      0
#define DSP     2
#define RADIO   13
static uint32_t dsp_at, global_at, first_at, second_at, cached_at;

static void
as_a_boot_loader_lays_it_out(void)
{
	empty();
	partition(0x1000, 0x2000, US, DSP, 64);
	partition(0x4000, 0x3000, SMEM_GLOBAL, SMEM_GLOBAL, 32);
	partition(0x8000, 0x1000, DSP, RADIO, 64);              /* (two others': not ours to look into) */
	dsp_at = 0x1000;
	global_at = 0x4000;
	first_at = from_the_bottom(dsp_at, 443, "their words", 12, 4);
	second_at = from_the_bottom(dsp_at, 429, "ours", 5, 3);
	cached_at = from_the_top(dsp_at, 478, "a queue, aligned", 17, 64);
	from_the_top(dsp_at, 479, "another", 8, 64);
	from_the_bottom(global_at, 137, "the boot loader's", 18, 6);
}

static bool
canary_whole(void)
{
	for (unsigned i = 0; i < 64; i++) {
		if (bytes[SIZE + i] != 0xee) {
			return false;
		}
	}
	return true;
}

static const struct smem_item *
item_of(const struct smem_item *found, unsigned count, uint32_t item)
{
	for (unsigned i = 0; i < count; i++) {
		if (found[i].item == item) {
			return &found[i];
		}
	}
	return NULL;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* the partition of two processors, whichever of them is named first; the global one; none for strangers */
static void
partitions_found(void)
{
	uint32_t cacheline = 0, size = 0;

	as_a_boot_loader_lays_it_out();
	CHECK(hakios_smem_partition(&area, US, DSP, &cacheline, &size) == 0x1000 && cacheline == 64 && size == 0x2000,
	    "ours with the DSP: cache line %u, %#x bytes", cacheline, size);
	CHECK(hakios_smem_partition(&area, DSP, US, &cacheline, &size) == 0x1000, "... named the other way round");
	CHECK(hakios_smem_partition(&area, SMEM_GLOBAL, SMEM_GLOBAL, &cacheline, &size) == 0x4000 && cacheline == 32, "the global one");
	CHECK(hakios_smem_partition(&area, US, RADIO, &cacheline, &size) == 0, "none with a processor we share nothing with");
	CHECK(hakios_smem_partition(&area, US, US, &cacheline, &size) == 0, "nor with ourselves");
}

/* items from both ends of a partition, each where its bytes are and as long as they are */
static void
items_found(void)
{
	struct smem_item found[16];
	const struct smem_item *item;
	unsigned count;

	as_a_boot_loader_lays_it_out();
	count = hakios_smem_items(&area, dsp_at, 64, found, 16);
	CHECK(count == 4, "four items: two from the bottom, two from the top (%u)", count);
	item = item_of(found, count, 443);
	CHECK(item != NULL && item->offset == first_at && item->size == 12 && memcmp(bytes + item->offset, "their words", 12) == 0,
	    "the first from the bottom, without its padding");
	item = item_of(found, count, 429);
	CHECK(item != NULL && item->offset == second_at && item->size == 5, "the second, behind the first's padding");
	item = item_of(found, count, 478);
	CHECK(item != NULL && item->offset == cached_at && item->size == 17 && memcmp(bytes + item->offset, "a queue, aligned", 17) == 0,
	    "one from the top: its bytes are in front of its entry");
	CHECK(item_of(found, count, 479) != NULL && item_of(found, count, 137) == NULL, "the other from the top; not the global partition's");
	count = hakios_smem_items(&area, global_at, 32, found, 16);
	CHECK(count == 1 && found[0].item == 137 && found[0].size == 18, "the global partition's one item");
}

/* an item of two processors: in their partition, or else in the global one */
static void
an_item_of_two(void)
{
	uint32_t size = 0;

	as_a_boot_loader_lays_it_out();
	CHECK(hakios_smem_item(&area, US, DSP, 429, &size) == second_at && size == 5, "in the partition of the two: %u bytes", size);
	CHECK(hakios_smem_item(&area, US, DSP, 478, &size) == cached_at && size == 17, "one from its top");
	CHECK(hakios_smem_item(&area, US, DSP, 137, &size) != 0 && size == 18, "one that is only in the global partition");
	CHECK(hakios_smem_item(&area, US, RADIO, 137, &size) != 0, "... which a processor without a partition of its own with us finds too");
	CHECK(hakios_smem_item(&area, US, RADIO, 429, &size) == 0, "but not another pair's item");
	CHECK(hakios_smem_item(&area, US, DSP, 999, &size) == 0, "an item nobody made");
}

/* an SMP2P item's words by name */
static void
smp2p_words(void)
{
	struct smp2p words = { .magic = SMP2P_MAGIC, .version = SMP2P_VERSION, .total = 16, .valid = 3 };

	memcpy(words.entry[0].name, "master-kernel", 14);
	memcpy(words.entry[1].name, "sixteen letters!", 16);    /* (no zero after it) */
	memcpy(words.entry[2].name, "sleepstate", 11);
	memcpy(words.entry[3].name, "not valid yet", 14);
	CHECK(smp2p_entry(&words, "master-kernel") == 0 && smp2p_entry(&words, "sleepstate") == 2, "by name");
	CHECK(smp2p_entry(&words, "sixteen letters!") == 1, "a name that fills its sixteen bytes");
	CHECK(smp2p_entry(&words, "master") == -1 && smp2p_entry(&words, "") == -1, "the whole name");
	CHECK(smp2p_entry(&words, "not valid yet") == -1, "only among the words that are valid");
	words.valid = 60000;                                    /* (as the other side might write) */
	CHECK(smp2p_entry(&words, "not valid yet") == 3 && smp2p_entry(&words, "nobody") == -1, "a count beyond sixteen is sixteen");
	CHECK(sizeof(words) == 20 + 16 * 20 && SMP2P_AT_NAME(1) == 40 && SMP2P_AT_VALUE(0) == 36, "the item's shape: %zu bytes", sizeof(words));
}

/* more items than the caller has room for: counted, not written */
static void
more_than_room(void)
{
	struct smem_item found[3] = { { 0 }, { 0 }, { 0xdead, 0xdead, 0xdead } };

	as_a_boot_loader_lays_it_out();
	CHECK(hakios_smem_items(&area, dsp_at, 64, found, 2) == 4, "all four are counted");
	CHECK(found[0].item == 443 && found[1].item == 429 && found[2].item == 0xdead, "two are written, the third slot is not");
	CHECK(hakios_smem_items(&area, 0, 64, found, 2) == 0, "no partition, no items");
}

/* a cache line that is no power of two (or none): the items from the top cannot be walked, those from the bottom can */
static void
odd_cacheline(void)
{
	struct smem_item found[16];

	as_a_boot_loader_lays_it_out();
	CHECK(hakios_smem_items(&area, dsp_at, 0, found, 16) == 2 && hakios_smem_items(&area, dsp_at, 48, found, 16) == 2,
	    "the two from the bottom");
}

/* what is in memory is no shared memory area */
static void
not_an_area(void)
{
	uint32_t cacheline, size;
	struct hakios_smem none = { NULL, SIZE }, small = { bytes, 0x800 };
	uint8_t byte = 7;

	as_a_boot_loader_lays_it_out();
	bytes[TABLE] = 'X';
	CHECK(hakios_smem_partition(&area, US, DSP, &cacheline, &size) == 0, "a table without its magic");
	as_a_boot_loader_lays_it_out();
	bytes[TABLE + 4] = 2;
	CHECK(hakios_smem_partition(&area, US, DSP, &cacheline, &size) == 0, "a table of another version");
	as_a_boot_loader_lays_it_out();
	bytes[0x1000] = 'X';
	CHECK(hakios_smem_partition(&area, US, DSP, &cacheline, &size) == 0, "a partition without its magic");
	CHECK(!hakios_smem_peek(&none, 0, &byte, 1) && byte == 7 && hakios_smem_partition(&none, US, DSP, &cacheline, &size) == 0,
	    "nothing mapped");
	CHECK(hakios_smem_partition(&small, US, DSP, &cacheline, &size) == 0, "an area smaller than its own table");
}

/* headers that say wrong things of themselves are not followed outside the area */
static void
lies_about_itself(void)
{
	struct smem_table_entry entry;
	struct smem_partition header;
	struct smem_entry first;
	struct smem_item found[16];
	uint32_t cacheline, size;
	uint8_t out[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	as_a_boot_loader_lays_it_out();
	memcpy(&entry, bytes + TABLE + sizeof(struct smem_table), sizeof(entry));
	entry.offset = SIZE - 0x800;                    /* a partition inside the table of partitions */
	memcpy(bytes + TABLE + sizeof(struct smem_table), &entry, sizeof(entry));
	CHECK(hakios_smem_partition(&area, US, DSP, &cacheline, &size) == 0, "a partition that would overlap the table");
	as_a_boot_loader_lays_it_out();
	memcpy(&header, bytes + dsp_at, sizeof(header));
	header.size = 0x900000;                         /* not what the table says */
	memcpy(bytes + dsp_at, &header, sizeof(header));
	CHECK(hakios_smem_partition(&area, US, DSP, &cacheline, &size) == 0, "a header and a table that disagree");
	as_a_boot_loader_lays_it_out();
	memcpy(&header, bytes + dsp_at, sizeof(header));
	header.free_uncached = 0xffffff00;
	memcpy(bytes + dsp_at, &header, sizeof(header));
	CHECK(hakios_smem_items(&area, dsp_at, 64, found, 16) == 0, "free space beyond the partition's end");
	as_a_boot_loader_lays_it_out();
	memcpy(&first, bytes + dsp_at + sizeof(header), sizeof(first));
	first.size = 0xfffffff0;                        /* the next entry would be far outside */
	memcpy(bytes + dsp_at + sizeof(header), &first, sizeof(first));
	CHECK(hakios_smem_items(&area, dsp_at, 64, found, 16) <= 2, "an item longer than its partition ends the walk from the bottom");
	as_a_boot_loader_lays_it_out();
	bytes[dsp_at + sizeof(header)] = 0;             /* the first entry's canary */
	CHECK(hakios_smem_items(&area, dsp_at, 64, found, 16) == 2, "an entry without its canary ends that walk: the two from the top remain");
	CHECK(!hakios_smem_peek(&area, SIZE - 4, out, 8) && out[0] == 1 && out[7] == 8, "a piece that is half outside is not copied at all");
	CHECK(!hakios_smem_peek(&area, 0xffffffffffull, out, 8), "nor one far outside");
	CHECK(canary_whole(), "nothing behind the area was written");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "partitions_found", partitions_found }, { "items_found", items_found }, { "an_item_of_two", an_item_of_two },
		{ "smp2p_words", smp2p_words }, { "more_than_room", more_than_room },
		{ "odd_cacheline", odd_cacheline }, { "not_an_area", not_an_area }, { "lies_about_itself", lies_about_itself },
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
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
