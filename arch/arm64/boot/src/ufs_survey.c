/* Part of the loader (included by loader.c, not compiled alone): ufs=1: what the phone's storage holds, read only. */
/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */
static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
		const char *a = ufs_references[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / READ_SYSREG(CNTFRQ_EL0));
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, ufs_references[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}
