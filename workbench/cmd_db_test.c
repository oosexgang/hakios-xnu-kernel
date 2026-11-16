/*
 * The lookup in RPMh's command database that the drivers share (drivers/board/cmd_db.h),
 * against databases built here: one as a boot loader leaves it, and ones that say wrong things of
 * themselves, which the lookup must not follow outside the memory it was given.
 *
 *   cmd_db_test SCENARIO
 */
#include "fakekernel.h"

#define KERNEL 1
#include "cmd_db_image.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* (a rail's list is of a fixed length in a real database, filled up with zeros after its last level) */
static const uint16_t cx_levels[] = { 0, 48, 64, 128, 256, 384, 416, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
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

/* a rail's levels: what a request names a level by */
static void
levels_of_a_rail(void)
{
	uint16_t levels[16] = { 0 };
	unsigned count = 99;

	as_a_boot_loader_leaves_it();
	CHECK(hakios_cmd_db_levels(DB, SIZE, "cx.lvl", levels, 16, &count) == 0x30000 && count == 7, "its address and seven levels: %u", count);
	CHECK(levels[0] == 0 && levels[1] == 48 && levels[6] == 416, "the first may be zero; the list ends at the next zero, not at its sixteen places: %u %u %u",
	    levels[0], levels[1], levels[6]);
	CHECK(hakios_cmd_db_levels(DB, SIZE, "cx.lvl", levels, 3, &count) == 0x30000 && count == 3, "no more than asked for: %u", count);
	CHECK(hakios_cmd_db_levels(DB, SIZE, "mx.lvl", levels, 16, &count) == 0x30010 && count == 0, "a rail without a list: its address, no level");
	CHECK(hakios_cmd_db_levels(DB, SIZE, "none", levels, 16, &count) == 0 && count == 0, "what is not there");
	CHECK(hakios_cmd_db_levels(DB, SIZE, "cx.lvl", NULL, 0, NULL) == 0x30000, "the address alone");
	DB->slave[0].data_offset = (uint16_t)(SIZE - sizeof(*DB) - 4);
	CHECK(hakios_cmd_db_levels(DB, SIZE, "cx.lvl", levels, 16, &count) == 0x30000 && count == 0 && canary_whole(),
	    "levels that would lie outside the mapping are not read");
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
		{ "found_by_name", found_by_name }, { "found_by_kind", found_by_kind }, { "own_data", own_data }, { "levels_of_a_rail", levels_of_a_rail },
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
