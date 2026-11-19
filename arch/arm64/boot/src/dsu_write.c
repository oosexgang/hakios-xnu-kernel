/* Part of the loader (included by loader.c, not compiled alone): the system image written through its extents, and nowhere else. */
/*
 * Write `n` bytes (whole blocks) at byte `offset` of system_gsi, and nowhere else: each piece is put
 * where the extents say that part of the image lives, and read back. 1 if all went in and came
 * back the same.
 */
static uint8_t dsu_check[8u << 20] __attribute__((aligned(4096)));

static int
dsu_write(efi_block_io *data, uint64_t offset, const uint8_t *src, uint64_t n)
{
	uint64_t bs = data->media->block_size, image_at = 0;

	if (bs == 0 || offset % bs || n % bs || offset > dsu_bytes || n > dsu_bytes - offset) {
		print("dsu: patch at %x of %d bytes: not whole blocks inside the image\n", offset, n);
		return 0;
	}
	/*
	 * Only through UEFI's own transfer list (see hc_take_back): with the kernel's still in the controller,
	 * UEFI's driver rings for a slot of that list, and the controller does what the kernel left there.
	 */
	if (!hc_list_is_uefis()) {
		hc_print_state("patch not written");
		print("dsu: patch: NOT WRITTEN: UEFI's transfer list is not the one in the controller\n");
		return 0;
	}
	/*
	 * Every piece the patch is cut into begins and ends on a block of the storage, looked at before anything
	 * is written: found at the second extent, the first piece would have been written already, and the patch
	 * left half in.
	 */
	for (unsigned x = 0; x < dsu_extents && image_at < offset + n; x++) {
		uint64_t len = dsu_extent[x].count * 512, start = dsu_extent[x].sector * 512;
		uint64_t from = offset > image_at ? offset : image_at, to = offset + n < image_at + len ? offset + n : image_at + len;

		if (from < to && ((start + (from - image_at)) % bs || (start + (to - image_at)) % bs)) {
			print("dsu: patch: extent %d (sector %x, %d sectors) is not in whole blocks: nothing written\n",
			    (uint64_t)x, dsu_extent[x].sector, dsu_extent[x].count);
			return 0;
		}
		image_at += len;
	}
	image_at = 0;
	for (unsigned x = 0; x < dsu_extents && n > 0; x++) {
		uint64_t len = dsu_extent[x].count * 512, start = dsu_extent[x].sector * 512;

		if (offset >= image_at + len) {
			image_at += len;
			continue;
		}
		while (n > 0 && offset < image_at + len) {
			uint64_t at = start + (offset - image_at);
			uint64_t piece = image_at + len - offset;

			if (piece > n) {
				piece = n;
			}
			if (piece > sizeof(dsu_check)) {
				piece = sizeof(dsu_check);
			}
			if (at % bs || EFI_ERROR(data->write_blocks(data, data->media->media_id, at / bs, piece, (void *)src)) ||
			    EFI_ERROR(data->flush_blocks(data)) ||
			    EFI_ERROR(data->read_blocks(data, data->media->media_id, at / bs, piece, dsu_check)) ||
			    memcmp(dsu_check, src, piece) != 0) {
				print("dsu: patch: writing %d bytes at sector %x failed or did not read back\n", piece, at / 512);
				return 0;
			}
			src += piece;
			offset += piece;
			n -= piece;
		}
		image_at += len;
	}
	return n == 0;
}
