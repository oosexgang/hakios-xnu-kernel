/*
 * Where the loader finds the iOS system image on the phone (loader/src/dsu_extents.c, dsu_place.c),
 * compiled as it is into a host program. The image is stored the way Android stores a "DSU" system:
 * as a partition, system_gsi, described by Android's lp_metadata in the metadata partition and lying
 * in pieces (extents) inside userdata. The loader reads that description from the phone -- and with
 * "oem dsu" writes the image through it. So what it takes from the description has to lie inside
 * userdata whatever the description says; this is where that is tried.
 *
 *   loader_dsu_test SCENARIO
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "efi.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

static char said[8192];
static void
print(const char *format, ...)
{
	snprintf(said + strlen(said), sizeof(said) - strlen(said), "%s", format);
}

/* what the parts before these two give them in the loader: a buffer to read into, the partitions by name */
static uint8_t ufs_buffer[256 * 1024];
static efi_block_io *metadata_partition, *userdata_partition;

static efi_block_io *
partition_named(const char *want)
{
	return strcmp(want, "metadata") == 0 ? metadata_partition : strcmp(want, "userdata") == 0 ? userdata_partition : NULL;
}

#include "src/dsu_extents.c"
#include "src/dsu_place.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define USERDATA_SECTORS        (200ULL << 21)          /* 200 GiB */

/* ---- an lp_metadata, as Android's liblp lays one out ----------------------------------------------- */

struct piece { uint64_t sector, count; uint32_t kind; };

static void
put32(uint8_t *at, uint32_t value)
{
	memcpy(at, &value, 4);
}

static void
put64(uint8_t *at, uint64_t value)
{
	memcpy(at, &value, 8);
}

/*
 * A copy at `p': the geometry's block, then the header (its size, the tables' size, where the partitions'
 * and the extents' tables are in them and how large an entry is), then the tables. Two partitions before
 * system_gsi, with an extent each, and one after it. Returns the copy's length.
 */
static uint32_t
metadata(uint8_t *p, const char *name, const struct piece *pieces, uint32_t count)
{
	uint8_t *h = p + 4096, *t = h + 128, *partitions = t, *extents = t + 4 * 52;
	static const char *const others[] = { "userdata_gsi", "scratch", "system_gsi_a" };

	memset(p, 0, 4096 + 128 + 4 * 52 + (count + 3) * 24);
	put32(p, LP_GEOMETRY_MAGIC);
	put32(h, LP_HEADER_MAGIC);
	put32(h + 8, 128);
	put32(h + 44, 4 * 52 + (count + 3) * 24);
	put32(h + 80, 0);
	put32(h + 84, 4);
	put32(h + 88, 52);
	put32(h + 92, 4 * 52);
	put32(h + 96, count + 3);
	put32(h + 100, 24);
	for (unsigned i = 0; i < 4; i++) {
		uint8_t *e = partitions + i * 52;
		unsigned other = i < 2 ? i : 2;

		if (i == 2) {
			strcpy((char *)e, name);
			put32(e + 40, 2);               /* its first extent */
			put32(e + 44, count);
		} else {
			strcpy((char *)e, others[other]);
			put32(e + 40, i < 2 ? i : count + 2);
			put32(e + 44, 1);
		}
	}
	for (unsigned i = 0; i < count + 3; i++) {
		uint8_t *x = extents + i * 24;

		if (i >= 2 && i < count + 2) {
			put64(x, pieces[i - 2].count);
			put32(x + 8, pieces[i - 2].kind);
			put64(x + 12, pieces[i - 2].sector);
		} else {
			put64(x, 2048);
			put64(x + 12, 4096 + i * 4096);
		}
	}
	return 4096 + 128 + 4 * 52 + (count + 3) * 24;
}

static const struct piece two[] = { { 0x100000, 0x4000, 0 }, { 0x900000, 0x1000, 0 } };
static uint8_t copy[65536];

/* ---- scenarios: the description ------------------------------------------------------------------- */

static void
extents_are_taken(void)
{
	uint32_t length = metadata(copy, "system_gsi", two, 2);

	CHECK(dsu_parse(copy, length, USERDATA_SECTORS) == 1, "a description is taken");
	CHECK(dsu_extents == 2 && dsu_extent[0].sector == 0x100000 && dsu_extent[0].count == 0x4000 && dsu_extent[1].sector == 0x900000 &&
	    dsu_extent[1].count == 0x1000, "system_gsi's two extents, not its neighbours': %u, %#llx", dsu_extents, (unsigned long long)dsu_extent[0].sector);
	CHECK(dsu_bytes == 0x5000ULL * 512, "the image's size is theirs together: %llu", (unsigned long long)dsu_bytes);
	/* an image that ends with userdata's last sector is still inside */
	{
		const struct piece last[] = { { USERDATA_SECTORS - 8, 8, 0 } };

		CHECK(dsu_parse(copy, metadata(copy, "system_gsi", last, 1), USERDATA_SECTORS) == 1 && dsu_extents == 1, "up to the last sector");
	}
}

/* only the partition of exactly that name */
static void
another_name(void)
{
	CHECK(dsu_parse(copy, metadata(copy, "system_gsi_b", two, 2), USERDATA_SECTORS) == 0, "system_gsi_b is not system_gsi");
	CHECK(dsu_parse(copy, metadata(copy, "system_gs", two, 2), USERDATA_SECTORS) == 0, "nor is system_gs");
	CHECK(dsu_parse(copy, metadata(copy, "xsystem_gsi", two, 2), USERDATA_SECTORS) == 0, "nor xsystem_gsi");
	CHECK(dsu_extents == 0 && dsu_bytes == 0, "nothing is kept of them");
}

/* an extent that does not lie in userdata, however it is written: nothing of the description is used */
static void
outside_userdata(void)
{
	const struct piece past[] = { { 0x100000, 0x4000, 0 }, { USERDATA_SECTORS - 8, 9, 0 } };
	const struct piece beyond[] = { { USERDATA_SECTORS, 8, 0 } };
	const struct piece wraps[] = { { 0xffffffffffffff00ULL, 0x200, 0 } };       /* (start and length add up to a small number) */
	const struct piece huge[] = { { 0x1000, 0xfffffffffffff800ULL, 0 } };
	const struct piece not_linear[] = { { 0x100000, 0x4000, 1 } };

	CHECK(dsu_parse(copy, metadata(copy, "system_gsi", past, 2), USERDATA_SECTORS) == 0, "one sector past userdata's end");
	CHECK(dsu_parse(copy, metadata(copy, "system_gsi", beyond, 1), USERDATA_SECTORS) == 0, "starting at its end");
	CHECK(dsu_parse(copy, metadata(copy, "system_gsi", wraps, 1), USERDATA_SECTORS) == 0, "a start so large that start + length wraps around");
	CHECK(dsu_parse(copy, metadata(copy, "system_gsi", huge, 1), USERDATA_SECTORS) == 0, "a length so large that it does");
	CHECK(dsu_parse(copy, metadata(copy, "system_gsi", not_linear, 1), USERDATA_SECTORS) == 0, "an extent that is not plain sectors");
	CHECK(dsu_extents == 0 && dsu_bytes == 0, "nothing is kept of them: %u extents, %llu bytes", dsu_extents, (unsigned long long)dsu_bytes);
}

/* a description that is not whole or not of the shape known is none */
static void
not_of_the_shape_known(void)
{
	uint32_t length = metadata(copy, "system_gsi", two, 2);
	uint8_t *h = copy + 4096;

	CHECK(dsu_parse(copy, length, USERDATA_SECTORS) == 1, "(it is taken as it is)");
	dsu_extents = 0;
	CHECK(dsu_parse(copy, length - 1, USERDATA_SECTORS) == 0, "tables that do not fit into what was read");
	CHECK(dsu_parse(copy, 4096 + 100, USERDATA_SECTORS) == 0, "a header that does not");
#define BROKEN(offset, value, why) do { uint32_t was; memcpy(&was, h + (offset), 4); put32(h + (offset), (value)); \
	CHECK(dsu_parse(copy, length, USERDATA_SECTORS) == 0, why); put32(h + (offset), was); } while (0)
	BROKEN(0, 0x12345678, "another magic");
	BROKEN(8, 64, "a header too short");
	BROKEN(8, 4096, "a header too long");
	BROKEN(44, 0xfffffff0u, "tables larger than anything");
	BROKEN(84, 1000, "more partitions than the tables hold");
	BROKEN(88, 40, "partition entries too short to have their fields");
	BROKEN(96, 3, "fewer extents than system_gsi says it has");
	BROKEN(96, 0x10000000, "more extents than the tables hold");
	BROKEN(100, 16, "extent entries too short to have their fields");
	BROKEN(92, 0xffffff00u, "an extents' table that starts outside the tables");
	BROKEN(80, 0xffffff00u, "a partitions' table that does");
	CHECK(dsu_extents == 0, "nothing is kept of any");
	CHECK(dsu_parse(copy, length, USERDATA_SECTORS) == 1 && dsu_extents == 2, "(and whole again it is taken)");
	/* system_gsi with no extent, or with more than the loader keeps */
	put32(h + 128 + 2 * 52 + 44, 0);
	CHECK(dsu_parse(copy, length, USERDATA_SECTORS) == 0, "an image of no extents");
	put32(h + 128 + 2 * 52 + 44, DSU_MAX_EXTENTS + 1);
	put32(h + 96, DSU_MAX_EXTENTS + 100);
	CHECK(dsu_parse(copy, sizeof(copy), USERDATA_SECTORS) == 0, "more extents than are kept");
}

/* ---- a phone's two partitions ----------------------------------------------------------------------- */

static uint8_t metadata_bytes[1024 * 1024];
static efi_block_media metadata_media = { .media_id = 3, .block_size = 4096, .last_block = sizeof(metadata_bytes) / 4096 - 1 };
static efi_block_media userdata_media = { .media_id = 9, .block_size = 4096, .last_block = USERDATA_SECTORS / 8 - 1 };
static struct { uint64_t lba, size; } reads[64];
static unsigned read_count;
static efi_status read_status;

static efi_status EFIAPI
metadata_read(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buffer)
{
	if (media_id != 3 || size % 4096 || (lba + size / 4096) * 4096 > sizeof(metadata_bytes) || EFI_ERROR(read_status)) {
		return EFI_ERROR(read_status) ? read_status : (EFI_ERROR_BIT | 2);
	}
	memcpy(buffer, metadata_bytes + lba * 4096, size);
	return 0;
}

/* (userdata has no bytes here: a read is written down, and a block's number put where its data would be) */
static efi_status EFIAPI
userdata_read(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buffer)
{
	if (media_id != 9 || size % 4096 || lba + size / 4096 > USERDATA_SECTORS / 8) {
		return EFI_ERROR_BIT | 2;
	}
	if (read_count < 64) {
		reads[read_count].lba = lba;
		reads[read_count].size = size;
	}
	read_count++;
	if (EFI_ERROR(read_status)) {
		return read_status;
	}
	for (uint64_t i = 0; i < size / 4096; i++) {
		put64((uint8_t *)buffer + i * 4096, lba + i);
	}
	return 0;
}

static efi_block_io metadata_io = { .media = &metadata_media, .read_blocks = metadata_read };
static efi_block_io userdata_io = { .media = &userdata_media, .read_blocks = userdata_read };

static void
phone(void)
{
	metadata_partition = &metadata_io;
	userdata_partition = &userdata_io;
	memset(metadata_bytes, 0, sizeof(metadata_bytes));
	said[0] = 0;
}

/* the description is found wherever in the metadata partition it lies, once */
static void
found_on_the_phone(void)
{
	efi_block_io *data = NULL;

	phone();
	metadata(metadata_bytes + 0x3000, "system_gsi", two, 2);
	CHECK(dsu_find(&data) == 1 && data == &userdata_io, "found: %s", said);
	CHECK(dsu_extents == 2 && dsu_bytes == 0x5000ULL * 512, "with its extents");

	/* where the loader's reads of the partition overlap, a copy is still one copy: near each window's end, and at the very end */
	for (uint64_t at = 0x2f000; at <= 0x33000; at += 0x1000) {
		phone();
		metadata(metadata_bytes + at, "system_gsi", two, 2);
		CHECK(dsu_find(&data) == 1, "a copy at %#llx: %s", (unsigned long long)at, said);
		CHECK(strstr(said, "lp_metadata at") != NULL && strstr(strstr(said, "lp_metadata at") + 1, "lp_metadata at") == NULL,
		    "... is seen once: %s", said);
	}
	phone();
	metadata(metadata_bytes + sizeof(metadata_bytes) - 0x2000, "system_gsi", two, 2);
	CHECK(dsu_find(&data) == 1, "a copy in the partition's last blocks: %s", said);
}

/* two copies that agree are one description; two that differ are none; none is none */
static void
copies(void)
{
	static const struct piece other[] = { { 0x200000, 0x4000, 0 }, { 0x900000, 0x1000, 0 } };
	efi_block_io *data = NULL;

	phone();
	metadata(metadata_bytes + 0x3000, "system_gsi", two, 2);
	metadata(metadata_bytes + 0x80000, "system_gsi", two, 2);
	CHECK(dsu_find(&data) == 1, "two copies that agree: %s", said);
	phone();
	metadata(metadata_bytes + 0x3000, "system_gsi", two, 2);
	metadata(metadata_bytes + 0x80000, "system_gsi", other, 2);
	CHECK(dsu_find(&data) == 0 && strstr(said, "two different copies") != NULL, "two that differ: %s", said);
	phone();
	metadata(metadata_bytes + 0x3000, "system_gsi_b", two, 2);
	CHECK(dsu_find(&data) == 0 && strstr(said, "no lp_metadata with a system_gsi") != NULL, "none: %s", said);
	phone();
	metadata(metadata_bytes + 0x3000, "system_gsi", two, 2);
	read_status = EFI_ERROR_BIT | 7;
	CHECK(dsu_find(&data) == 0 && strstr(said, "metadata read failed") != NULL, "a partition that cannot be read: %s", said);
	read_status = 0;
	metadata_partition = NULL;
	CHECK(dsu_find(&data) == 0 && strstr(said, "dsu: no %s partition") != NULL, "a phone without the partition: %s", said);
}

/* the image is read through its extents: each read lies in one extent, in whole blocks */
static void
read_through_extents(void)
{
	static uint8_t into[16 * 4096];
	uint64_t first;

	phone();
	/* two extents of 3 and 2 blocks, the second lower on the storage than the first */
	dsu_extent[0].sector = 80000, dsu_extent[0].count = 24;
	dsu_extent[1].sector = 16000, dsu_extent[1].count = 16;
	dsu_extents = 2;
	dsu_bytes = 40 * 512;
	CHECK(dsu_read(&userdata_io, 0, 5 * 4096, into) == 0, "the whole image");
	CHECK(read_count == 2 && reads[0].lba == 10000 && reads[0].size == 3 * 4096 && reads[1].lba == 2000 && reads[1].size == 2 * 4096,
	    "two reads, one for each extent: %u, %llu + %llu, %llu + %llu", read_count, (unsigned long long)reads[0].lba, (unsigned long long)reads[0].size,
	    (unsigned long long)reads[1].lba, (unsigned long long)reads[1].size);
	memcpy(&first, into + 3 * 4096, 8);
	CHECK(first == 2000, "the second extent's blocks follow the first's in the buffer");
	read_count = 0;
	CHECK(dsu_read(&userdata_io, 2 * 4096, 2 * 4096, into) == 0 && read_count == 2 && reads[0].lba == 10002 && reads[0].size == 4096 &&
	    reads[1].lba == 2000 && reads[1].size == 4096, "a piece across the two");
	read_count = 0;
	CHECK(dsu_read(&userdata_io, 4 * 4096, 4096, into) == 0 && read_count == 1 && reads[0].lba == 2001, "a piece of the second only");
	read_count = 0;
	CHECK(EFI_ERROR(dsu_read(&userdata_io, 4 * 4096, 2 * 4096, into)), "past the image's end");
	CHECK(EFI_ERROR(dsu_read(&userdata_io, 5 * 4096, 4096, into)), "from its end");
	CHECK(EFI_ERROR(dsu_read(&userdata_io, 512, 4096, into)) && EFI_ERROR(dsu_read(&userdata_io, 0, 512, into)), "not in whole blocks");
	CHECK(read_count == 0, "none of them reads anything");
	read_status = EFI_ERROR_BIT | 7;
	CHECK(dsu_read(&userdata_io, 0, 4096, into) == (EFI_ERROR_BIT | 7), "a read that fails is passed on");
	read_status = 0;
	/* an extent that does not start on a block of the storage cannot be read in blocks */
	dsu_extent[0].sector = 80001;
	read_count = 0;
	CHECK(EFI_ERROR(dsu_read(&userdata_io, 0, 4096, into)) && read_count == 0, "an extent off the storage's blocks");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "extents_are_taken", extents_are_taken }, { "another_name", another_name }, { "outside_userdata", outside_userdata },
		{ "not_of_the_shape_known", not_of_the_shape_known }, { "found_on_the_phone", found_on_the_phone }, { "copies", copies },
		{ "read_through_extents", read_through_extents },
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
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
