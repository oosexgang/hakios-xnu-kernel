/* Part of the loader (included by loader.c, not compiled alone): the iOS system image as an Android DSU image ("oem dsu"). */
/* ------------------------------------------------------------------------ */
/* The iOS system image as an Android DSU image ("oem dsu")                  */

/*
 * Android's Dynamic System Updates keep an installed image in userdata as a pinned file, written
 * beneath the metadata encryption (libfiemap maps it with dm-linear onto the raw partition and
 * writes it through that), so on the storage it is plaintext. Where it is, libfiemap writes down
 * in /metadata/gsi/dsu/<slot>/lp_metadata: a liblp geometry block (4 KB) followed by a metadata
 * header and tables whose one partition, "system_gsi", lists the extents in 512-byte sectors of
 * userdata. /metadata is not encrypted; this loader has no ext4 reader and does not need one: the
 * file is small, and its blocks are found by their magic in the raw partition.
 *
 * "oem dsu"          say what was found (read only)
 * "oem dsu load N"   read the first N bytes (hex) of system_gsi into the resident area: the image
 *                    the kernel mounts as /dev/md0, as "oem ramdisk" would have had it over USB
 * Nothing here writes to the storage.
 */
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

static efi_block_io *dsu_patch_data;     /* "oem dsu patch": the next download goes into the image */
static uint64_t dsu_patch_offset;

static void
dsu_command(char *cmd)
{
	efi_block_io *data;
	struct payload_info pi;
	uint64_t want, got = 0, t0;
	uint8_t *dest;

	log_len = 0;
	if (!dsu_find(&data)) {
		fb_reply("FAIL", "no DSU image (see oem log)");
		return;
	}
	if (starts_with(cmd, "oem dsu patch ")) {
		/*
		 * "oem dsu patch OFFSET" (hex): the next download (whole blocks, at most the download buffer)
		 * replaces those bytes of the image on the storage -- and in the resident area, if the image
		 * is loaded there.
		 */
		dsu_patch_offset = parse_number(cmd + 14, 16);
		if (dsu_patch_offset >= dsu_bytes || dsu_patch_offset % data->media->block_size) {
			fb_reply("FAIL", "bad offset");
			return;
		}
		dsu_patch_data = data;
		fb_reply("OKAY", "");
		return;
	}
	if (!starts_with(cmd, "oem dsu load ")) {
		fb_reply("OKAY", "");
		return;
	}
	want = parse_number(cmd + 13, 16);
	parse_payload(&pi, 0);
	if (want == 0 || want > dsu_bytes || !choose_window(&pi) ||
	    want > win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE) {
		fb_reply("FAIL", "bad size or no memory window");
		return;
	}
	log_len = 0;
	dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (unsigned x = 0; x < dsu_extents && got < want; x++) {
		uint64_t bs = data->media->block_size, start = dsu_extent[x].sector * 512, left = dsu_extent[x].count * 512;

		if (start % bs) {
			fb_reply("FAIL", "extent not block aligned");
			return;
		}
		while (left > 0 && got < want) {
			uint64_t n = left < (8u << 20) ? left : (8u << 20);

			if (want - got < n) {
				n = (want - got + bs - 1) / bs * bs;
			}
			if (EFI_ERROR(data->read_blocks(data, data->media->media_id, start / bs, n, dest + got))) {
				print("dsu: read failed at sector %x\n", start / 512);
				resident_rd_size = 0;
				fb_reply("FAIL", "read failed");
				return;
			}
			start += n;
			left -= n;
			got += n;
		}
	}
	resident_rd_size = want;
	print("dsu: %d bytes read into the resident area in %d ms\n", want,
	    (READ_SYSREG(CNTVCT_EL0) - t0) / (READ_SYSREG(CNTFRQ_EL0) / 1000));
	fb_reply("OKAY", "");
}
