/*
 * The lookup in RPMh's command database that the drivers share (kernel/overlay/bsd/dev/hakios_cmd_db.h),
 * against databases built here: one as a boot loader leaves it, and ones that say wrong things of
 * themselves, which the lookup must not follow outside the memory it was given.
 *
 *   cmd_db_test SCENARIO
 */
#include "fakekernel.h"

#define KERNEL 1
#include "hakios_cmd_db.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- a database, built the way cmd-db.c reads one ------------------------------------------------ */

#define SIZE            0x2000u
static union {
	struct cmd_db_header header;
	uint8_t bytes[SIZE + 64];               /* (what lies after the mapping is the canary's) */
} image;
#define DB              (&image.header)
static unsigned entries_at, data_at;            /* from `data': where the next slave's entries and data go */

static void
empty(void)
{
	memset(&image, 0, sizeof(image));
	memset(image.bytes + SIZE, 0xee, 64);
	DB->magic[0] = 0xdb;
	DB->magic[1] = 0x30;
	DB->magic[2] = 0x03;
	DB->magic[3] = 0x0c;
	entries_at = 0;
	data_at = 0x800;
}

/* a slave of that kind with these resources: name, address, and the bytes of its own data */
struct resource {
	const char *name;
	uint32_t address;
	const void *data;
	uint16_t length;
};

static void
slave(unsigned index, unsigned kind, const struct resource *list, unsigned count)
{
	struct cmd_db_slave *s = &DB->slave[index];
	struct cmd_db_entry *entry = (struct cmd_db_entry *)(DB->data + entries_at);
	unsigned offset = 0;

	s->id = (uint16_t)kind;
	s->header_offset = (uint16_t)entries_at;
	s->data_offset = (uint16_t)data_at;
	s->count = (uint16_t)count;
	for (unsigned i = 0; i < count; i++) {
		memset(&entry[i], 0, sizeof(entry[i]));
		memcpy(entry[i].id, list[i].name, strlen(list[i].name) < 8 ? strlen(list[i].name) : 8);
		entry[i].address = list[i].address;
		entry[i].length = list[i].length;
		entry[i].offset = (uint16_t)offset;
		if (list[i].length) {
			memcpy(DB->data + data_at + offset, list[i].data, list[i].length);
		}
		offset += list[i].length;
	}
	entries_at += count * sizeof(struct cmd_db_entry);
	data_at += offset;
}

static const uint16_t cx_levels[] = { 0, 48, 64, 128, 256, 384, 416 };
static const struct { uint32_t unit; uint16_t width; uint8_t vcd, reserved; } mc0 = { 1000, 14, 0, 0 };

static void
as_a_boot_loader_leaves_it(void)
{
	static const struct resource rails[] = {
		{ "cx.lvl", 0x30000, cx_levels, sizeof(cx_levels) }, { "mx.lvl", 0x30010, NULL, 0 }, { "xo.lvl", 0x30080, NULL, 0 },
	};
	static const struct resource regulators[] = {
		{ "smpb1", 0x40000, NULL, 0 }, { "ldoc2", 0x41100, NULL, 0 }, { "eightlet", 0x41200, NULL, 0 },
	};
	static const struct resource buses[] = {
		{ "MC0", 0x50000, &mc0, sizeof(mc0) }, { "SH0", 0x50004, NULL, 0 }, { "ACV", 0x50008, NULL, 0 },
		{ "cx.lvl", 0x5ffff, NULL, 0 },         /* (a name that another kind has too) */
	};

	empty();
	slave(0, CMD_DB_ARC, rails, 3);
	slave(1, CMD_DB_VRM, regulators, 3);
	slave(3, CMD_DB_BCM, buses, 4);         /* (slave 2 is unused, as real databases have gaps) */
}

static bool
canary_whole(void)
{
	for (unsigned i = 0; i < 64; i++) {
		if (image.bytes[SIZE + i] != 0xee) {
			return false;
		}
	}
	return true;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

static void
found_by_name(void)
{
	as_a_boot_loader_leaves_it();
	CHECK(hakios_cmd_db_valid(DB), "a database");
	CHECK(hakios_cmd_db_address(DB, SIZE, "cx.lvl") == 0x30000, "a rail: %#x", hakios_cmd_db_address(DB, SIZE, "cx.lvl"));
	CHECK(hakios_cmd_db_address(DB, SIZE, "ldoc2") == 0x41100, "a regulator");
	CHECK(hakios_cmd_db_address(DB, SIZE, "ACV") == 0x50008, "a bus clock manager, behind an unused slave");
	CHECK(hakios_cmd_db_address(DB, SIZE, "ldoc9") == 0, "what is not there is 0");
	CHECK(hakios_cmd_db_address(DB, SIZE, "ldoc") == 0 && hakios_cmd_db_address(DB, SIZE, "ldoc22") == 0, "a name is the whole name");
	CHECK(hakios_cmd_db_address(DB, SIZE, "") == 0, "no name is no resource");
	CHECK(hakios_cmd_db_address(DB, SIZE, "eightlet") == 0x41200, "a name of eight letters, which has no zero after it");
	CHECK(hakios_cmd_db_address(DB, SIZE, "eightletters") == 0x41200, "... as the database holds it: by its first eight");
}

static void
found_by_kind(void)
{
	unsigned where = 99;

	as_a_boot_loader_leaves_it();
	CHECK(hakios_cmd_db_find(DB, SIZE, "cx.lvl", CMD_DB_ANY, &where) != NULL && where == 0, "of any kind: the first, in slave %u", where);
	CHECK(hakios_cmd_db_find(DB, SIZE, "cx.lvl", CMD_DB_BCM, &where) != NULL && where == 3 &&
	    hakios_cmd_db_find(DB, SIZE, "cx.lvl", CMD_DB_BCM, NULL)->address == 0x5ffff, "among the bus clock managers: slave %u", where);
	CHECK(hakios_cmd_db_find(DB, SIZE, "MC0", CMD_DB_ARC, NULL) == NULL, "a bus clock manager is not a rail");
	CHECK(hakios_cmd_db_find(DB, SIZE, "smpb1", CMD_DB_VRM, NULL) != NULL, "a regulator among the regulators");
}

static void
own_data(void)
{
	const struct cmd_db_entry *entry;
	const uint8_t *data;
	unsigned where = 0, length = 99;

	as_a_boot_loader_leaves_it();
	entry = hakios_cmd_db_find(DB, SIZE, "cx.lvl", CMD_DB_ARC, &where);
	data = entry ? hakios_cmd_db_data(DB, SIZE, where, entry, &length) : NULL;
	CHECK(data != NULL && length == sizeof(cx_levels) && memcmp(data, cx_levels, sizeof(cx_levels)) == 0, "a rail's levels: %u bytes", length);
	entry = hakios_cmd_db_find(DB, SIZE, "MC0", CMD_DB_BCM, &where);
	data = entry ? hakios_cmd_db_data(DB, SIZE, where, entry, &length) : NULL;
	CHECK(data != NULL && length == sizeof(mc0) && memcmp(data, &mc0, sizeof(mc0)) == 0, "a bus clock manager's unit and width");
	entry = hakios_cmd_db_find(DB, SIZE, "SH0", CMD_DB_BCM, &where);
	CHECK(entry != NULL && hakios_cmd_db_data(DB, SIZE, where, entry, &length) == NULL && length == 0, "one without data has none");
}

static void
every_entry(void)
{
	unsigned count = 0;

	as_a_boot_loader_leaves_it();
	for (unsigned s = 0; s < CMD_DB_SLAVES; s++) {
		for (unsigned i = 0; hakios_cmd_db_entry(DB, SIZE, s, i) != NULL; i++) {
			count++;
		}
	}
	CHECK(count == 10, "ten resources in all: %u", count);
	CHECK(hakios_cmd_db_entry(DB, SIZE, 2, 0) == NULL, "an unused slave has no entry");
	CHECK(hakios_cmd_db_entry(DB, SIZE, 0, 3) == NULL && hakios_cmd_db_entry(DB, SIZE, 0, 2) != NULL, "a slave ends at its count");
	CHECK(hakios_cmd_db_entry(DB, SIZE, CMD_DB_SLAVES, 0) == NULL, "there are eight slaves");
}

/* what is in memory is not a database: nothing is found, nothing is followed */
static void
not_a_database(void)
{
	as_a_boot_loader_leaves_it();
	DB->magic[2] = 0;
	CHECK(!hakios_cmd_db_valid(DB) && hakios_cmd_db_address(DB, SIZE, "cx.lvl") == 0 && hakios_cmd_db_entry(DB, SIZE, 0, 0) == NULL,
	    "a wrong magic");
	CHECK(!hakios_cmd_db_valid(NULL) && hakios_cmd_db_address(NULL, SIZE, "cx.lvl") == 0, "no mapping at all");
	memset(&image, 0xff, sizeof(image));
	CHECK(hakios_cmd_db_address(DB, SIZE, "cx.lvl") == 0, "memory of ones");
}

/* a database that says wrong things of itself is not followed outside what was mapped */
static void
lies_about_itself(void)
{
	const struct cmd_db_entry *entry;
	unsigned where = 0, length = 99;

	as_a_boot_loader_leaves_it();
	DB->slave[1].header_offset = 0xfff0;            /* its entries far behind the mapping */
	CHECK(hakios_cmd_db_address(DB, SIZE, "ldoc2") == 0, "entries outside the mapping are not read");
	CHECK(hakios_cmd_db_address(DB, SIZE, "ACV") == 0x50008, "... and the slaves after it still are");
	as_a_boot_loader_leaves_it();
	DB->slave[0].count = 60000;
	CHECK(hakios_cmd_db_address(DB, SIZE, "mx.lvl") == 0 && hakios_cmd_db_address(DB, SIZE, "smpb1") == 0x40000, "a count nobody believes");
	as_a_boot_loader_leaves_it();
	DB->slave[0].count = 400;                       /* believable, but 400 entries do not fit behind its offset */
	DB->slave[0].header_offset = (uint16_t)(SIZE - sizeof(*DB) - 100 * sizeof(struct cmd_db_entry));
	CHECK(hakios_cmd_db_entry(DB, SIZE, 0, 0) == NULL, "entries that would run past the end");
	as_a_boot_loader_leaves_it();
	entry = hakios_cmd_db_find(DB, SIZE, "cx.lvl", CMD_DB_ARC, &where);
	DB->slave[0].data_offset = (uint16_t)(SIZE - sizeof(*DB) - 4);
	CHECK(entry != NULL && hakios_cmd_db_data(DB, SIZE, where, entry, &length) == NULL && length == 0, "data that would run past the end");
	CHECK(hakios_cmd_db_entry(DB, (uint32_t)sizeof(*DB) - 1, 0, 0) == NULL, "a mapping too small for a header");
	CHECK(canary_whole(), "nothing behind the mapping was written");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "found_by_name", found_by_name }, { "found_by_kind", found_by_kind }, { "own_data", own_data },
		{ "every_entry", every_entry }, { "not_a_database", not_a_database }, { "lies_about_itself", lies_about_itself },
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
