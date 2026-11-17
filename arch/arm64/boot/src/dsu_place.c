/* Part of the loader (included by loader.c, not compiled alone): the DSU image found on the phone, and read through its extents. */
/* Find system_gsi's extents; 1 if there is exactly one plausible copy of the metadata (older ones a rename left are ignored when they agree, extent by extent). */
static int
dsu_find(efi_block_io **userdata_out)
{
	efi_block_io *meta = partition_named("metadata"), *data = partition_named("userdata");
	uint64_t bs, meta_bytes, chunk = sizeof(ufs_buffer), found = 0;
	uint64_t first_bytes = 0, first_extents = 0;
	static uint8_t first_table[sizeof(dsu_extent)];         /* (the first copy's extents, each later one compared with them) */

	if (meta == NULL || data == NULL) {
		print("dsu: no %s partition\n", meta == NULL ? "metadata" : "userdata");
		return 0;
	}
	bs = meta->media->block_size;
	meta_bytes = (meta->media->last_block + 1) * bs;
	for (uint64_t at = 0; at < meta_bytes; at += chunk - 64 * 1024) {
		uint64_t n = meta_bytes - at < chunk ? meta_bytes - at : chunk;

		n -= n % bs;
		if (EFI_ERROR(meta->read_blocks(meta, meta->media->media_id, at / bs, n, ufs_buffer))) {
			print("dsu: metadata read failed at %x\n", at);
			return 0;
		}
		/* only blocks in the first part of the window start a copy: the last 64 KB are read again next time */
		for (uint64_t b = 0; b + 4096 <= n && (b < chunk - 64 * 1024 || at + n == meta_bytes); b += 4096) {
			if (*(const uint32_t *)(ufs_buffer + b) != LP_GEOMETRY_MAGIC ||
			    !dsu_parse(ufs_buffer + b, n - b, (data->media->last_block + 1) * data->media->block_size / 512)) {
				continue;
			}
			print("dsu: lp_metadata at %x of metadata: system_gsi %d bytes in %d extents, the first at sector %x\n",
			    at + b, dsu_bytes, (uint64_t)dsu_extents, dsu_extent[0].sector);
			if (found++ == 0) {
				first_bytes = dsu_bytes, first_extents = dsu_extents;
				memcpy(first_table, dsu_extent, dsu_extents * sizeof(dsu_extent[0]));
			} else if (dsu_bytes != first_bytes || dsu_extents != first_extents ||
			    memcmp(first_table, dsu_extent, dsu_extents * sizeof(dsu_extent[0])) != 0) {
				print("dsu: two different copies of the metadata: not choosing\n");
				return 0;
			}
		}
	}
	if (found == 0) {
		print("dsu: no lp_metadata with a system_gsi in the metadata partition\n");
		return 0;
	}
	*userdata_out = data;
	return 1;
}

/* `n` bytes of the DSU image from byte `offset` (both whole blocks), through its extents */
static efi_status
dsu_read(efi_block_io *data, uint64_t offset, uint64_t n, uint8_t *dest)
{
	uint64_t bs = data->media->block_size, image_at = 0;

	if (offset > dsu_bytes || n > dsu_bytes - offset || offset % bs || n % bs) {
		return 0x8000000000000002ULL;   /* EFI_INVALID_PARAMETER */
	}
	for (unsigned x = 0; x < dsu_extents && n > 0; x++) {
		uint64_t len = dsu_extent[x].count * 512;

		while (n > 0 && offset < image_at + len) {
			uint64_t at = dsu_extent[x].sector * 512 + (offset - image_at), piece = image_at + len - offset;
			efi_status st;

			if (piece > n) {
				piece = n;
			}
			if (at % bs) {
				return 0x8000000000000002ULL;
			}
			st = data->read_blocks(data, data->media->media_id, at / bs, piece, dest);
			if (EFI_ERROR(st)) {
				return st;
			}
			dest += piece;
			offset += piece;
			n -= piece;
		}
		image_at += len;
	}
	return n == 0 ? 0 : 0x8000000000000002ULL;
}
