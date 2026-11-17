/* Part of the loader (included by loader.c, not compiled alone): the commands taken over USB. */
static void
fb_command(char *cmd)
{
	int busy = running_live != NULL;

	host_last_cmd = READ_SYSREG(CNTVCT_EL0);

	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "board")) {
				/* the board it took itself for (its framebuffer; /chosen/darwin-board), for the host to compare */
				fb_reply("OKAY", PLAT ? PLAT->name : "");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "max-download-size")) {
				/* what the next download may be, where the commands before it send it (fastboot: 32 bits at most) */
				uint64_t before, room;
				char size[24];

				if (download_room(download_to_ramdisk, download_ramdisk_append, download_ramdisk_patch, resident_rd_size, win_size,
				    &before, &room) != NULL) {
					room = 0;
				}
				format_into(size, sizeof(size), "0x%x", room < 0xffffffffULL ? room : 0xffffffffULL);
				fb_reply("OKAY", size);
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				char geom[64];

				format_into(geom, sizeof(geom), "%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height,
				    (uint64_t)SCREEN.stride);
				fb_reply("OKAY", geom);
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem read ") && !busy) {
			/*
			 * "oem read NAME OFFSET LENGTH" (hex; whole blocks, at most 2 MB): that part of a GPT
			 * partition into the staged buffer, for "fastboot get_staged". Read only -- for taking a
			 * look at what Android has (its vendor modules in super, say) without root.
			 */
			char name[40];
			unsigned k = 0;
			const char *c = cmd + 9;
			uint64_t off, len;
			efi_block_io *bio;

			while (*c != 0 && *c != ' ' && k + 1 < sizeof(name)) {
				name[k++] = *c++;
			}
			name[k] = 0;
			while (*c == ' ') {
				c++;
			}
			if (!number_given(&c, 16, &off) || !number_given(&c, 16, &len) || *c != 0) {
				fb_reply("FAIL", "oem read NAME OFFSET LENGTH, the two in hex");
				return;
			}
			bio = partition_named(name);
			/* (inside the partition without adding the two: an offset near 2^64 would make the sum small) */
			/* (into the block buffer: out_buf holds the last run's kernel log, which "oem log" puts after the loader's) */
			if (bio == NULL || len == 0 || len > 2 * 1024 * 1024 || off % bio->media->block_size ||
			    len % bio->media->block_size || off > (bio->media->last_block + 1) * bio->media->block_size ||
			    len > (bio->media->last_block + 1) * bio->media->block_size - off ||
			    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, off / bio->media->block_size, len, block_buffer))) {
				fb_reply("FAIL", "no such partition, or out of it");
				return;
			}
			staged_data = (const char *)block_buffer;
			staged_len = len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem dsu") && !busy) {
			dsu_command(cmd);
		} else if (starts_with(cmd, "oem ramdisk crc") && !busy) {
			/*
			 * "oem ramdisk crc": a CRC-32 of each 256 MB of the resident image into the log (then "oem
			 * log"): whether a run left it as it was uploaded, and which parts it changed.
			 */
			static uint32_t table[256];
			const uint8_t *base = (const uint8_t *)(win_base + RESIDENT_RD_OFFSET);

			if (table[1] == 0) {
				for (uint32_t i = 0; i < 256; i++) {
					uint32_t c = i;

					for (int k = 0; k < 8; k++) {
						c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
					}
					table[i] = c;
				}
			}
			log_len = 0;            /* (the log of a run fills it: these lines would be dropped) */
			print("ramdisk crc: %d bytes at %x\n", resident_rd_size, win_base + RESIDENT_RD_OFFSET);
			for (uint64_t at = 0; at < resident_rd_size; at += RAMDISK_CRC_CHUNK) {
				uint64_t n = resident_rd_size - at < RAMDISK_CRC_CHUNK ? resident_rd_size - at : RAMDISK_CRC_CHUNK;
				uint32_t c = 0xffffffffu;

				for (uint64_t i = 0; i < n; i++) {
					c = table[(c ^ base[at + i]) & 0xff] ^ (c >> 8);
				}
				print("ramdisk crc %d: %x\n", at / RAMDISK_CRC_CHUNK, (uint64_t)~c);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem ramdisk patch ") && !busy) {
			/*
			 * "oem ramdisk patch OFFSET" (hex): the next download overwrites the resident image from
			 * OFFSET on -- an image changed in a few places is sent as those places only.
			 */
			const char *c = cmd + 18;
			uint64_t at;

			if (!number_given(&c, 16, &at) || *c != 0) {
				fb_reply("FAIL", "oem ramdisk patch OFFSET, in hex");
				return;
			}
			if (resident_rd_size == 0 || at >= resident_rd_size) {
				fb_reply("FAIL", "no resident image there");
				return;
			}
			download_ramdisk_patch = at;
			download_to_ramdisk = 2;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				download_to_ramdisk = 0;        /* a patch or an append asked for before has nothing to go into */
				download_ramdisk_append = 0;
				fb_reply("OKAY", "");
				return;
			}
			/*
			 * "oem ramdisk append": the next download goes after what the resident area holds already.
			 * A fastboot download is at most 4 GB (its size is 32 bits), and a Darwin system image may be
			 * sent in pieces.
			 */
			download_ramdisk_append = starts_with(cmd, "oem ramdisk append");
			if (!download_ramdisk_append) {
				parse_payload(&pi, 0);
				log_len = 0;
			}
			if ((!download_ramdisk_append && !choose_window(&pi)) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			const char *size_text = cmd + 9;
			uint64_t total = 0, got = 0, before, limit;
			uint8_t *dest = (uint8_t *)PAYLOAD_BASE;
			int to_ramdisk = download_to_ramdisk;
			efi_block_io *patch_data = to_ramdisk ? NULL : dsu_patch_data;
			const char *no_room = download_room(to_ramdisk, download_ramdisk_append, download_ramdisk_patch, resident_rd_size,
			    win_size, &before, &limit);

			/* what the commands before it asked for is for this download only, whatever comes of it */
			download_to_ramdisk = 0;
			download_ramdisk_append = 0;
			dsu_patch_data = NULL;
			if (no_room != NULL) {
				fb_reply("FAIL", no_room);
				return;
			}
			if (!number_given(&size_text, 16, &total) || *size_text != 0 || total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");       /* (nothing received: the resident image is as it was) */
				return;
			}
			if (to_ramdisk) {
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET + before);
			}
			if (to_ramdisk == 1) {
				resident_rd_size = before;      /* (what was after it is being written over) */
			}
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				got += (uint64_t)r;
				host_last_cmd = READ_SYSREG(CNTVCT_EL0);        /* (a host sending is a host there: the watchdog is petted) */
			}
			if (!to_ramdisk && (got != total || patch_data != NULL)) {
				/* the payload's area holds no payload now -- part of one, or the patch's bytes: "oem run" says so */
				((struct payload_header *)PAYLOAD_BASE)->magic = 0;
			}
			if (got == total && patch_data != NULL) {
				efi_block_io *data = patch_data;

				if (!dsu_write(data, dsu_patch_offset, dest, total)) {
					fb_reply("FAIL", "patch not written (oem log)");
					return;
				}
				if (resident_rd_size >= dsu_patch_offset + total) {
					memcpy((uint8_t *)(win_base + RESIDENT_RD_OFFSET + dsu_patch_offset), dest, total);
				}
				fb_reply("OKAY", "");
				return;
			}
			if (got == total) {
				if (to_ramdisk == 1) {
					resident_rd_size = before + total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
				host_last_cmd = READ_SYSREG(CNTVCT_EL0);
			}
			if (sent == staged_len) {
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "upload cut short");
			}
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			/* what the kernel drew went to memory, past our cache: drop the lines we still hold of it */
			for (uint64_t at = SCREEN.base & ~63ULL; at < SCREEN.base + staged_len; at += 64) {
				__asm__ volatile ("dc civac, %0" : : "r" (at) : "memory");
			}
			__asm__ volatile ("dsb sy" : : : "memory");
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || *arg != 0;
			uint64_t cp = CHECKPOINT_SELFTEST;

			if (!selftest && have_cp && (!number_given(&arg, 10, &cp) || *arg != 0)) {
				fb_reply("FAIL", "oem run [CHECKPOINT], a decimal number");
				return;
			}

			set_result("running", cp, 0);
			download_to_ramdisk = 0;        /* (a download asked for and never sent is not for the next one) */
			download_ramdisk_append = 0;
			dsu_patch_data = NULL;
			fb_reply("OKAY", "");
			log_len = 0;
			kernel_part_len = 0;            /* the last run's kernel log is not this one's */
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem ufs") && !busy) {
			/* read only: the storage's units, the GPT partitions and super's liblp metadata into the
			   log (then "oem log" and fetch it) -- for choosing where the Darwin system image can live */
			log_len = 0;
			ufs_survey();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}
