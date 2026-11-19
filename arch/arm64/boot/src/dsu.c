/* Part of the loader (included by loader.c, not compiled alone): the Darwin system image as an Android DSU image ("oem dsu"). */
/* ------------------------------------------------------------------------ */
/* The Darwin system image as an Android DSU image ("oem dsu")                  */

/*
 * Android's Dynamic System Updates keep an installed image in userdata as a pinned file, written
 * beneath the metadata encryption (libfiemap maps it with dm-linear onto the raw partition and
 * writes it through that), so on the storage it is plaintext. Where it is, libfiemap writes down
 * in /metadata/gsi/dsu/<slot>/lp_metadata: a liblp geometry block (4 KB) followed by a metadata
 * header and tables whose one partition, "system_gsi", lists the extents in 512-byte sectors of
 * userdata. /metadata is not encrypted; this loader has no ext4 reader and does not need one: the
 * file is small, and its blocks are found by their magic in the raw partition.
 *
 * "oem dsu"            say what was found (read only)
 * "oem dsu load N"     read the first N bytes (hex) of system_gsi into the resident area: the image
 *                      the kernel mounts as /dev/md0, as "oem ramdisk" would have had it over USB
 * "oem dsu patch OFF"  the next download is written into system_gsi at byte OFF (hex), through its
 *                      extents and read back (dsu_write.c): the one thing here that writes to the
 *                      storage, and it writes inside the image only
 */

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
	/*
	 * (the storage only through UEFI's own transfer list, as the block service and the log's writer use it: a read
	 * rings the controller's doorbell too, and after a run whose list could not be put back that slot is the kernel's)
	 */
	if (!hc_list_is_uefis()) {
		hc_print_state("DSU not looked for");
		dsu_patch_data = NULL;
		fb_reply("FAIL", "the storage is not UEFI's now (see oem log)");
		return;
	}
	if (!dsu_find(&data)) {
		dsu_patch_data = NULL;  /* (a patch armed before is not written by a table this look refused) */
		fb_reply("FAIL", "no DSU image (see oem log)");
		return;
	}
	if (starts_with(cmd, "oem dsu patch ")) {
		/*
		 * "oem dsu patch OFFSET" (hex): the next download (whole blocks, at most the download buffer)
		 * replaces those bytes of the image on the storage -- and in the resident area, if the image
		 * is loaded there.
		 */
		const char *c = cmd + 14;

		if (!number_given(&c, 16, &dsu_patch_offset) || *c != 0 || dsu_patch_offset >= dsu_bytes ||
		    dsu_patch_offset % data->media->block_size) {
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
	{
		const char *c = cmd + 13;

		if (!number_given(&c, 16, &want) || *c != 0) {
			want = 0;
		}
	}
	parse_payload(&pi, 0);
	if (want == 0 || want > dsu_bytes || !choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE ||
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
