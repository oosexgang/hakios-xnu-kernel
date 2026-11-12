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

	if (bs == 0 || offset % bs || n % bs || offset + n > dsu_bytes) {
		print("dsu: patch at %x of %d bytes: not whole blocks inside the image\n", offset, n);
		return 0;
	}
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
