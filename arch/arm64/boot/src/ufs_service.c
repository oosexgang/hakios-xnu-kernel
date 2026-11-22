/* Part of the loader (included by loader.c, not compiled alone): ufs=2, the service itself: the storage's units and partitions listed, the kernel's requests answered. */
/* Qualcomm's UEFI sometimes exposes raw Block I/O but omits EFI_PARTITION_INFO_PROTOCOL.
 * Read GPT directly in that case; this is read-only and keeps the same partition bounds. */
static uint32_t
block_gpt_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t crc = ~0u;

	for (uint64_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (unsigned b = 0; b < 8; b++) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

static uint64_t
block_gpt_fallback(efi_block_io *bio, uint64_t unit, uint8_t lun)
{
	static efi_partition_info info[BLOCK_MAX_PARTS];
	uint64_t bs = bio->media->block_size, count, entry_size, entries_lba, bytes, read, parts = 0;
	uint8_t header[512];
	uint32_t header_size, saved_crc, entries_crc;

	if (bs < 512 || bs > BLOCK_BUFFER_SIZE || bio->media->last_block < 1 ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, block_buffer)) ||
	    memcmp(block_buffer, "EFI PART", 8) != 0) {
		return 0;
	}
	memcpy(header, block_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	saved_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	entry_size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header) || count == 0 || count > BLOCK_MAX_PARTS ||
	    entry_size < 128 || entry_size > 4096 || count > UINT64_MAX / entry_size) {
		return 0;
	}
	*(uint32_t *)(header + 16) = 0;
	if (block_gpt_crc32(header, header_size) != saved_crc) {
		print("ufs: unit GPT header CRC failed; refusing its partitions\n");
		return 0;
	}
	bytes = count * entry_size;
	if (bytes > BLOCK_BUFFER_SIZE || entries_lba > bio->media->last_block ||
	    bytes > (bio->media->last_block + 1 - entries_lba) * bs) {
		return 0;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, block_buffer)) ||
	    block_gpt_crc32(block_buffer, bytes) != entries_crc) {
		print("ufs: unit GPT table CRC failed; refusing its partitions\n");
		return 0;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + parts + 1 + BLOCK_MAX_UNITS < BLOCK_MAX_PARTS; i++) {
		const uint8_t *entry = block_buffer + i * entry_size;
		efi_partition_info *p = &info[parts];
		uint8_t any = 0;

		for (unsigned j = 0; j < 16; j++) any |= entry[j];
		if (any == 0) continue;
		memset(p, 0, sizeof(*p));
		p->revision = 0x00010000;
		p->type = 2;
		memcpy(&p->type_guid, entry, 16);
		memcpy(&p->unique_guid, entry + 16, 16);
		p->start_lba = *(const uint64_t *)(entry + 32);
		p->end_lba = *(const uint64_t *)(entry + 40);
		p->attributes = *(const uint64_t *)(entry + 48);
		memcpy(p->name, entry + 56, sizeof(p->name));
		if (p->start_lba > p->end_lba || p->end_lba > bio->media->last_block) continue;
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		ufs_name(p, bp->name, sizeof(bp->name));
		bp->block_size = (uint32_t)bs;
		bp->blocks = p->end_lba - p->start_lba + 1;
		bp->start_lba = p->start_lba;
		bp->unit = (uint16_t)unit;
		bp->lun = lun;
		bp->flags = BLOCK_PART_RAW_GPT;
		block_bios[block_shared.parts++] = bio;
		block_keep_last();
		parts++;
	}
	print("ufs: GPT fallback: %d partitions from raw Block I/O\n", parts);
	return parts;
}

static void
block_start(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_parts_kept = 0;
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp = NULL;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = NULL;
			units[nunits].path_bytes = 0;
			units[nunits].lun = 0xff;
			if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				units[nunits].path = dp;
				units[nunits].path_bytes = device_path_bytes(dp);
				units[nunits].lun = device_path_print(nunits, dp);
			} else {
				print("ufs: unit %d: no EFI device path; scanning its GPT directly\n", nunits);
			}
			nunits++;
		}
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		/* Keep storage usable on firmware that publishes only whole-device Block I/O handles. */
		for (uint64_t u = 0; u < nunits && block_shared.parts + nunits < BLOCK_MAX_PARTS; u++) {
			block_gpt_fallback(units[u].bio, u, units[u].lun);
		}
		if (block_shared.parts == 0) {
			print("ufs: no partition information and GPT fallback found no partitions; block service is off\n");
			return;
		}
		goto partitions_done;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path != NULL && units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
		if (block_write_requested && block_is_darwin(info, bio, bp)) {
			block_write_part = (int)block_shared.parts;
		}
		block_bios[block_shared.parts++] = bio;
		block_keep_last();
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	partitions_done:
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
		block_keep_last();
	}
	/*
	 * The Darwin system image installed as an Android DSU image, as one more partition: "dsu", its
	 * blocks those of the image, wherever on userdata its extents put them (block_poll). Read only.
	 */
	{
		efi_block_io *data;
		uint64_t saved = log_len;

		block_dsu_part = -1;
		if (block_shared.parts < BLOCK_MAX_PARTS && dsu_find(&data) && data->media->block_size == 4096) {
			struct loader_block_part *bp = &block_shared.part[block_shared.parts];

			memset(bp, 0, sizeof(*bp));
			bp->name[0] = 'd', bp->name[1] = 's', bp->name[2] = 'u';
			bp->block_size = 4096;
			bp->blocks = dsu_bytes / 4096;
			bp->unit = 0xffff;
			bp->lun = 0xff;
			bp->flags = BLOCK_PART_DSU;
			block_dsu_part = (int)block_shared.parts;
			block_bios[block_shared.parts++] = data;
			block_keep_last();
			print("ufs: dsu: the DSU image as partition %d, %d blocks of 4096 in %d extents\n",
			    (uint64_t)block_dsu_part, bp->blocks, (uint64_t)dsu_extents);
		} else {
			log_len = saved;        /* (no DSU image: nothing to say about it) */
		}
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = BLOCK_WRITE_FIRST;
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits, nunits, (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	/* (bounded by what this core listed, not by what the shared page says now) */
	if (part < block_parts_kept && block_part_kept[part].block_size != 0 && count != 0 &&
	    count <= BLOCK_BUFFER_SIZE / block_part_kept[part].block_size &&
	    lba < block_part_kept[part].blocks && count <= block_part_kept[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_part_kept[part].block_size;
		if (!hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if ((int)part == block_dsu_part) {
			if (op != BLOCK_OP_READ) {
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED: the image is read only here */
				bytes = 0;
			} else {
				s = dsu_read(bio, lba * 4096, bytes, block_buffer);
				flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
			}
		} else if (op == BLOCK_OP_WRITE) {
			if ((block_part_kept[part].flags & BLOCK_PART_RAW_GPT) || block_write_part < 0 ||
			    part != (uint32_t)block_write_part || lba < BLOCK_WRITE_FIRST) {
				/* not darwin, or not past what the build writes from the computer: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of partition %d: REFUSED\n", count, lba, part);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					/*
					 * (out of the device's own cache first, and its answer taken: a flush that failed and a read
					 * back from that cache would have said "written" of blocks that are not on the flash)
					 */
					s = bio->flush_blocks(bio);
				}
				if (!EFI_ERROR(s)) {
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of darwin read back DIFFERENT\n",
						    count, lba);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			uint64_t io_lba = lba;

			if (block_part_kept[part].flags & BLOCK_PART_RAW_GPT) {
				if (io_lba > UINT64_MAX - block_part_kept[part].start_lba) {
					io_lba = UINT64_MAX;
				} else {
					io_lba += block_part_kept[part].start_lba;
				}
			}
			if (io_lba > bio->media->last_block || count > bio->media->last_block + 1 - io_lba) {
				s = 0x8000000000000002ULL;
				bytes = 0;
			} else {
				s = bio->read_blocks(bio, bio->media->media_id, io_lba, bytes, block_buffer);
				/* what the controller wrote is in memory; drop whatever this core held of the buffer */
				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
			}
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy" ::: "memory");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}
