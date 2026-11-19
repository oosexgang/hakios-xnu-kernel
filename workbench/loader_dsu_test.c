/*
 * Where the loader finds the Darwin system image on the phone (arch/arm64/boot/src/dsu_extents.c, dsu_place.c),
 * compiled as it is into a host program. The image is stored the way Android stores a "DSU" system:
 * as a partition, system_gsi, described by Android's lp_metadata in the metadata partition and lying
 * in pieces (extents) inside userdata. The loader reads that description from the phone -- and with
 * "oem dsu patch" writes into the image through it (arch/arm64/boot/src/dsu_write.c). So what it takes from the
 * description has to lie inside userdata whatever the description says, and what it writes inside the
 * image's extents whatever it is asked; this is where both are tried.
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

/* ... and whether UEFI's transfer list is the one in the UFS controller (ufs_block.c: the kernel may have put its own in) */
static bool kernels_list_in;

static int
hc_list_is_uefis(void)
{
	return !kernels_list_in;
}

static void
hc_print_state(const char *when)
{
}

#include "src/dsu_extents.c"
#include "src/dsu_place.c"
#include "src/dsu_write.c"

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

/* ... but what is written into it is kept, so that it reads back: up to STORED blocks, wherever they are */
#define STORED          4608
static struct { uint64_t lba; bool used; } stored[STORED];
static uint8_t stored_bytes[STORED][4096];
static struct { uint64_t lba, size; } writes[64];
static unsigned write_count, flushes, written_outside;
static bool forgetful;                          /* a storage that takes a write and does not keep it */
static efi_status write_status, flush_status;

static efi_status EFIAPI
userdata_write(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buffer)
{
	if (write_count < 64) {
		writes[write_count].lba = lba;
		writes[write_count].size = size;
	}
	write_count++;                          /* (every call is counted, also one the storage refuses) */
	if (media_id != 9 || size % 4096 || lba + size / 4096 > USERDATA_SECTORS / 8) {
		return EFI_ERROR_BIT | 2;
	}
	if (EFI_ERROR(write_status)) {
		return write_status;
	}
	for (uint64_t i = 0; i < size / 4096; i++) {
		bool inside = false;
		unsigned slot = 0;

		for (unsigned x = 0; x < dsu_extents; x++) {
			inside |= (lba + i) * 8 >= dsu_extent[x].sector && (lba + i) * 8 + 8 <= dsu_extent[x].sector + dsu_extent[x].count;
		}
		written_outside += !inside;
		if (forgetful) {
			continue;
		}
		while (slot < STORED && stored[slot].used && stored[slot].lba != lba + i) {
			slot++;
		}
		if (slot == STORED) {
			return EFI_ERROR_BIT | 9;
		}
		stored[slot].used = true;
		stored[slot].lba = lba + i;
		memcpy(stored_bytes[slot], (uint8_t *)buffer + i * 4096, 4096);
	}
	return 0;
}

static efi_status EFIAPI
userdata_read_back(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buffer)
{
	efi_status status = userdata_read(self, media_id, lba, size, buffer);

	for (uint64_t i = 0; !EFI_ERROR(status) && i < size / 4096; i++) {
		for (unsigned slot = 0; slot < STORED && stored[slot].used; slot++) {
			if (stored[slot].lba == lba + i) {
				memcpy((uint8_t *)buffer + i * 4096, stored_bytes[slot], 4096);
			}
		}
	}
	return status;
}

static efi_status EFIAPI
userdata_flush(struct efi_block_io *self)
{
	flushes++;
	return flush_status;
}

static efi_block_io metadata_io = { .media = &metadata_media, .read_blocks = metadata_read };
static efi_block_io userdata_io = { .media = &userdata_media, .read_blocks = userdata_read_back, .write_blocks = userdata_write,
	                            .flush_blocks = userdata_flush };

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
	{
		/* as many bytes, from the same first sector, in one extent where the other has two: they differ */
		static const struct piece one[] = { { 0x100000, 0x5000, 0 } };

		phone();
		metadata(metadata_bytes + 0x3000, "system_gsi", two, 2);
		metadata(metadata_bytes + 0x80000, "system_gsi", one, 1);
		CHECK(dsu_find(&data) == 0 && strstr(said, "two different copies") != NULL, "the same size, other extents: %s", said);
	}
	{
		/* the same size, the same first extent, the same number of them: the second elsewhere -- they differ */
		static const struct piece moved[] = { { 0x100000, 0x4000, 0 }, { 0xa00000, 0x1000, 0 } };

		phone();
		metadata(metadata_bytes + 0x3000, "system_gsi", two, 2);
		metadata(metadata_bytes + 0x80000, "system_gsi", moved, 2);
		CHECK(dsu_find(&data) == 0 && strstr(said, "two different copies") != NULL, "only the second extent elsewhere: %s", said);
	}
	{
		/* a good copy, then one refused at its second extent (outside userdata): the first's table, whole */
		static const struct piece bad[] = { { 0x300000, 0x4000, 0 }, { ~0ULL - 8, 0x1000, 0 } };

		phone();
		metadata(metadata_bytes + 0x3000, "system_gsi", two, 2);
		metadata(metadata_bytes + 0x80000, "system_gsi", bad, 2);
		CHECK(dsu_find(&data) == 1, "a copy that is none does not count: %s", said);
		CHECK(dsu_extents == 2 && dsu_extent[0].sector == 0x100000 && dsu_extent[0].count == 0x4000 && dsu_extent[1].sector == 0x900000,
		    "and leaves the good one's extents as they were: %#llx", (unsigned long long)dsu_extent[0].sector);
	}
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
	CHECK(dsu_read(&userdata_io, 0xfffffffffffff000ULL, 2 * 4096, into) == (EFI_ERROR_BIT | 2), "an offset so large that offset + length wraps around");
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

/* ---- writing into the image ------------------------------------------------------------------------- */

static uint8_t patch[9u << 20];

/* an image of two extents, 3 and 2 blocks, the second lower on the storage than the first */
static void
image(void)
{
	phone();
	dsu_extent[0].sector = 80000, dsu_extent[0].count = 24;
	dsu_extent[1].sector = 16000, dsu_extent[1].count = 16;
	dsu_extents = 2;
	dsu_bytes = 40 * 512;
	for (unsigned i = 0; i < sizeof(patch); i++) {
		patch[i] = (uint8_t)(i * 31 + (i >> 12));
	}
}

/* what is written goes where the extents say that part of the image lies, is flushed and read back */
static void
written_through_extents(void)
{
	static uint8_t back[5 * 4096];

	image();
	CHECK(dsu_write(&userdata_io, 4096, patch, 4 * 4096) == 1, "the image's last four blocks: %s", said);
	CHECK(write_count == 2 && writes[0].lba == 10001 && writes[0].size == 2 * 4096 && writes[1].lba == 2000 && writes[1].size == 2 * 4096,
	    "two writes, each inside one extent: %u, %llu + %llu, %llu + %llu", write_count, (unsigned long long)writes[0].lba,
	    (unsigned long long)writes[0].size, (unsigned long long)writes[1].lba, (unsigned long long)writes[1].size);
	CHECK(written_outside == 0, "%u blocks were written outside the image's extents", written_outside);
	CHECK(flushes == 2, "each write is flushed before it is read back: %u", flushes);
	CHECK(dsu_read(&userdata_io, 4096, 4 * 4096, back) == 0 && memcmp(back, patch, 4 * 4096) == 0, "the image reads back as written");
	/* the first block was not written: it is as it was */
	memset(back, 0, sizeof(back));
	CHECK(dsu_read(&userdata_io, 0, 4096, back) == 0 && back[8] == 0 && *(uint64_t *)back == 10000, "what was not written is untouched");
	/* a piece that lies in the second extent only */
	write_count = 0;
	CHECK(dsu_write(&userdata_io, 4 * 4096, patch + 4096, 4096) == 1 && write_count == 1 && writes[0].lba == 2001 && writes[0].size == 4096,
	    "the image's last block alone: %u writes, at %llu", write_count, (unsigned long long)writes[0].lba);
	CHECK(written_outside == 0, "%u blocks were written outside the image's extents", written_outside);
}

/* asked for more than the image, or for less than whole blocks: nothing is written */
static void
only_inside_the_image(void)
{
	image();
	CHECK(dsu_write(&userdata_io, 4 * 4096, patch, 2 * 4096) == 0, "one block past the image's end");
	CHECK(dsu_write(&userdata_io, 5 * 4096, patch, 4096) == 0, "from its end");
	CHECK(dsu_write(&userdata_io, 0, patch, 6 * 4096) == 0, "more than all of it");
	CHECK(dsu_write(&userdata_io, 512, patch, 4096) == 0 && dsu_write(&userdata_io, 0, patch, 512) == 0, "not in whole blocks");
	/* (an offset off the image's blocks, also where an extent off the storage's would put it on one of the storage's) */
	dsu_extent[0].sector = 79999;
	CHECK(dsu_write(&userdata_io, 512, patch, 4096) == 0, "an offset that is no block of the image");
	dsu_extent[0].sector = 80000;
	said[0] = 0;
	CHECK(dsu_write(&userdata_io, 0xfffffffffffff000ULL, patch, 2 * 4096) == 0 && strstr(said, "not whole blocks inside the image") != NULL,
	    "an offset so large that offset + length wraps around is refused as what it is: %s", said);
	CHECK(write_count == 0 && written_outside == 0, "none of them writes anything: %u writes", write_count);
	CHECK(strstr(said, "not whole blocks inside the image") != NULL, "and why is said: %s", said);
	/* an extent that does not start on a block of the storage cannot be written in blocks */
	dsu_extent[0].sector = 80001;
	CHECK(dsu_write(&userdata_io, 0, patch, 4096) == 0 && write_count == 0, "an extent off the storage's blocks");
	/* a storage whose blocks are of no size */
	userdata_media.block_size = 0;
	CHECK(dsu_write(&userdata_io, 0, patch, 4096) == 0 && write_count == 0, "a storage that gives no block size");
	userdata_media.block_size = 4096;
	/* nothing to write at the image's very end is nothing written, and done */
	image();
	write_count = 0;
	CHECK(dsu_write(&userdata_io, 5 * 4096, patch, 0) == 1 && write_count == 0, "an empty patch at the end: %u writes", write_count);
	/* an image said to be larger than its extents: what lies past them is not written, and not said to be */
	dsu_bytes = 48 * 512;
	CHECK(dsu_write(&userdata_io, 5 * 4096, patch, 4096) == 0 && write_count == 0 && written_outside == 0,
	    "a block past the extents: %u writes", write_count);
	CHECK(dsu_write(&userdata_io, 4 * 4096, patch, 4096 + 512) == 0 && write_count == 0,
	    "a patch of a block and a half, the half past the extents: refused before anything is written: %u writes", write_count);
}

/* a write that fails, or that does not read back as written: said, and nothing more is written */
static void
not_written_is_said(void)
{
	image();
	forgetful = true;
	CHECK(dsu_write(&userdata_io, 0, patch, 5 * 4096) == 0, "a storage that does not keep what it is given");
	CHECK(write_count == 1 && strstr(said, "failed or did not read back") != NULL, "said after the first piece, the second is not tried: %u, %s",
	    write_count, said);
	forgetful = false;
	write_count = 0;
	write_status = EFI_ERROR_BIT | 7;
	CHECK(dsu_write(&userdata_io, 0, patch, 5 * 4096) == 0 && write_count == 1, "a write that fails");
	write_status = 0;
	write_count = 0;
	read_status = EFI_ERROR_BIT | 7;
	CHECK(dsu_write(&userdata_io, 0, patch, 5 * 4096) == 0 && write_count == 1, "a read back that fails");
	read_status = 0;
	/* a flush that fails: what reads back may be the device's cache, not the flash -- not taken for written */
	write_count = 0;
	flush_status = EFI_ERROR_BIT | 7;
	CHECK(dsu_write(&userdata_io, 0, patch, 5 * 4096) == 0 && write_count == 1, "a flush that fails: %u writes", write_count);
	flush_status = 0;
}

/*
 * An extent the patch reaches that does not begin or end on a block of the storage: nothing at all is written --
 * not the pieces of the extents before it either, which would leave the image half patched.
 */
static void
whole_blocks_before_writing(void)
{
	image();
	dsu_extent[1].sector = 16001;           /* the second extent off the storage's blocks */
	CHECK(dsu_write(&userdata_io, 0, patch, 5 * 4096) == 0, "a patch across an extent off the storage's blocks");
	CHECK(write_count == 0, "nothing is written, not even the first extent's piece: %u writes", write_count);
	CHECK(strstr(said, "not in whole blocks") != NULL, "and why is said: %s", said);
	/* the first extent 2.5 blocks long: its piece would end in the middle of a block, and the next start there */
	image();
	dsu_extent[0].count = 20;
	dsu_bytes = 36 * 512;
	CHECK(dsu_write(&userdata_io, 0, patch, 4 * 4096) == 0 && write_count == 0, "an extent whose end is off the storage's blocks: %u writes",
	    write_count);
	/* ... which a patch inside the first two blocks of it does not reach: that one goes in */
	CHECK(dsu_write(&userdata_io, 4096, patch, 4096) == 1 && write_count == 1 && writes[0].lba == 10001, "a patch that does not reach it: %s",
	    said);
	/*
	 * an extent that begins off the storage's blocks and ends on one (and the next the other way about): the piece of it
	 * would begin in the middle of a block -- nothing written, not the first extent's piece either
	 */
	image();
	dsu_extent[1].sector = 16001, dsu_extent[1].count = 15;
	dsu_extent[2].sector = 40007, dsu_extent[2].count = 9;
	dsu_extents = 3;
	dsu_bytes = 48 * 512;
	write_count = 0;
	CHECK(dsu_write(&userdata_io, 0, patch, 6 * 4096) == 0 && write_count == 0, "an extent whose start is off the storage's blocks: %u writes",
	    write_count);
	/* an extent off the storage's blocks that ends where a patch begins: the patch does not reach it, and goes in */
	image();
	dsu_extent[0].sector = 80001;
	write_count = 0;
	CHECK(dsu_write(&userdata_io, 3 * 4096, patch, 2 * 4096) == 1 && write_count == 1 && writes[0].lba == 2000,
	    "a patch of the second extent alone: %u writes: %s", write_count, said);
}

/* the kernel's own transfer list still in the controller (a run that did not give it back): nothing is written */
static void
only_through_uefis_list(void)
{
	image();
	kernels_list_in = true;
	CHECK(dsu_write(&userdata_io, 0, patch, 4096) == 0, "a patch while the kernel's list is in the controller");
	CHECK(write_count == 0 && read_count == 0, "nothing is written or read: %u writes, %u reads", write_count, read_count);
	CHECK(strstr(said, "transfer list") != NULL, "and why is said: %s", said);
	kernels_list_in = false;
	CHECK(dsu_write(&userdata_io, 0, patch, 4096) == 1 && write_count == 1, "with UEFI's back, it goes in: %s", said);
}

/* a long piece goes in pieces no larger than the buffer it is read back into */
static void
long_patch(void)
{
	phone();
	dsu_extent[0].sector = 800000, dsu_extent[0].count = (17u << 20) / 512;
	dsu_extents = 1;
	dsu_bytes = 17u << 20;
	for (unsigned i = 0; i < sizeof(patch); i++) {
		patch[i] = (uint8_t)(i * 13 + (i >> 11));
	}
	CHECK(dsu_write(&userdata_io, 4u << 20, patch, sizeof(patch)) == 1, "9 MB at 4 MB of the image: %s", said);
	CHECK(write_count == 2 && writes[0].lba == 100000 + 1024 && writes[0].size == 8u << 20 && writes[1].lba == 100000 + 1024 + 2048 &&
	    writes[1].size == 1u << 20, "8 MB, then the rest behind it: %u writes, %llu + %llu", write_count, (unsigned long long)writes[0].lba,
	    (unsigned long long)writes[0].size);
	CHECK(written_outside == 0, "%u blocks were written outside the image's extent", written_outside);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "extents_are_taken", extents_are_taken }, { "another_name", another_name }, { "outside_userdata", outside_userdata },
		{ "not_of_the_shape_known", not_of_the_shape_known }, { "found_on_the_phone", found_on_the_phone }, { "copies", copies },
		{ "read_through_extents", read_through_extents }, { "written_through_extents", written_through_extents },
		{ "only_inside_the_image", only_inside_the_image }, { "not_written_is_said", not_written_is_said }, { "long_patch", long_patch },
		{ "whole_blocks_before_writing", whole_blocks_before_writing }, { "only_through_uefis_list", only_through_uefis_list },
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
