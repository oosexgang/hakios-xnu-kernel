/* Part of the loader (included by loader.c, not compiled alone): where the DSU image lies in userdata: Android's lp_metadata, read. */
/* (the DSU image's place on the storage: see "oem dsu" below) */
static efi_status dsu_read(efi_block_io *data, uint64_t offset, uint64_t n, uint8_t *dest);
#define DSU_MAX_EXTENTS 4096
#define LP_GEOMETRY_MAGIC 0x616c4467u
#define LP_HEADER_MAGIC   0x414c5030u

static struct {
	uint64_t sector, count;         /* in userdata, 512-byte sectors */
} dsu_extent[DSU_MAX_EXTENTS];
static unsigned dsu_extents;
static uint64_t dsu_bytes;

/*
 * A 64-bit number of the metadata, a byte at a time: an extent is 24 bytes packed, so its sector (at 12)
 * lies on a 4-byte boundary only, and an unaligned load is no load to make (it faults in Device memory).
 */
static uint64_t
dsu_u64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--) {
		v = v << 8 | p[i];
	}
	return v;
}

/* The extents of system_gsi from one lp_metadata found at `at` in buffer `p` (geometry first); 0 = none. */
static int
dsu_parse(const uint8_t *p, uint64_t room, uint64_t userdata_sectors)
{
	const uint8_t *h = p + 4096;
	uint32_t header_size, tables_size, po, pn, ps, eo, en, es;
	const uint8_t *t;

	if (room < 4096 + 256 || *(const uint32_t *)h != LP_HEADER_MAGIC) {
		return 0;
	}
	header_size = *(const uint32_t *)(h + 8);
	tables_size = *(const uint32_t *)(h + 44);
	if (header_size < 128 || header_size > 256 || tables_size > room - 4096 - header_size) {
		return 0;
	}
	t = h + header_size;
	po = *(const uint32_t *)(h + 80), pn = *(const uint32_t *)(h + 84), ps = *(const uint32_t *)(h + 88);
	eo = *(const uint32_t *)(h + 92), en = *(const uint32_t *)(h + 96), es = *(const uint32_t *)(h + 100);
	if (ps < 52 || es < 24 || (uint64_t)po + (uint64_t)pn * ps > tables_size || (uint64_t)eo + (uint64_t)en * es > tables_size) {
		return 0;
	}
	for (uint32_t i = 0; i < pn; i++) {
		const uint8_t *e = t + po + (uint64_t)i * ps;
		static const char want[] = "system_gsi";
		uint32_t first, n;
		uint64_t bytes = 0;
		unsigned k = 0;

		while (k < sizeof(want) && e[k] == (uint8_t)want[k]) {
			k++;
		}
		if (k != sizeof(want)) {
			continue;
		}
		first = *(const uint32_t *)(e + 40);
		n = *(const uint32_t *)(e + 44);
		if (n == 0 || n > DSU_MAX_EXTENTS || (uint64_t)first + n > en) {
			return 0;
		}
		/* every extent looked at before one is taken: a copy refused half way leaves the table of the one before it whole */
		for (uint32_t x = 0; x < n; x++) {
			const uint8_t *ex = t + eo + (uint64_t)(first + x) * es;
			uint64_t count = dsu_u64(ex), sector = dsu_u64(ex + 12);

			/* (not "sector + count > userdata_sectors": a start or a length near 2^64 makes that sum small) */
			if (*(const uint32_t *)(ex + 8) != 0 /* linear */ || count > userdata_sectors || sector > userdata_sectors - count) {
				return 0;
			}
		}
		for (uint32_t x = 0; x < n; x++) {
			const uint8_t *ex = t + eo + (uint64_t)(first + x) * es;

			dsu_extent[x].sector = dsu_u64(ex + 12);
			dsu_extent[x].count = dsu_u64(ex);
			bytes += dsu_extent[x].count * 512;
		}
		dsu_extents = n;
		dsu_bytes = bytes;
		return 1;
	}
	return 0;
}
