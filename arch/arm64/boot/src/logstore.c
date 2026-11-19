/* Part of the loader (included by loader.c, not compiled alone): the log kept across runs: a raw write to a partition (phone only). */
/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			BS->free_pool(handles);
			return bio;
		}
	}
	BS->free_pool(handles);
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
static char out_buf[2 * 1024 * 1024];
/* the storage's reads for the kernel land here (block_poll); also where the log is read back */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;
/*
 * Where the last run's kernel log is in out_buf, from its separator to its end marker. It stays after the
 * loader's own log until the next run begins: a log composed again ("oem log", a write of the partition)
 * used to be the loader's alone, and a host that asked for the log just after the kernel came back got
 * that and not what the kernel said.
 */
static uint64_t kernel_part_at, kernel_part_len;

/* what compose_logs() leaves for the loader's log beside the kernel's: all of log_buf, the two lines around, a block */
#define KERNEL_PART_ROOM        (sizeof(log_buf) + 2 * 32 + 512)

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	if (klog == NULL && kernel_part_len != 0 && n + kernel_part_len <= sizeof(out_buf)) {
		memmove(out_buf + n, out_buf + kernel_part_at, kernel_part_len);     /* (the loader's log may have grown) */
		kernel_part_at = n;
		memcpy(out_buf, log_buf, log_len);
		n += kernel_part_len;
		memset(out_buf + n, 0, sizeof(out_buf) - n);
		staged_len = n;
		return;
	}
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	kernel_part_len = 0;
	if (klog != NULL) {
		/*
		 * (room left for the loader's log to grow to its whole size, and the two lines around the kernel's: else a later
		 * compose_logs(NULL) would find no room for the kernel's part and leave it out for good)
		 */
		if (klog_len > sizeof(out_buf) - n - (KERNEL_PART_ROOM)) {
			klog_len = sizeof(out_buf) - n - (KERNEL_PART_ROOM);
		}
		kernel_part_at = n;
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
		kernel_part_len = n - kernel_part_at;
	}
	staged_len = n;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

static void
write_logs(const char *klog, uint64_t klog_len)
{
	efi_block_io *bio = find_log_partition();


	compose_logs(klog, klog_len);
	if (bio == NULL) {
		/* No log partition (QEMU): echo the returned kernel log on the console instead. */
		if (klog != NULL) {
			print("\n== KERNEL LOG ==\n");
			for (uint64_t i = 0; i < klog_len && i < sizeof(out_buf); i++) {      /* (as much as is staged, no more) */
				put_char(klog[i]);
			}
			print("\n== END OF LOG ==\n");
		}
		return;
	}
	uint64_t bs = bio->media->block_size, len;
	efi_status ws, fs = 0, rs = 0;
	int same = 0;

	/*
	 * Nothing past the partition's end: the log is written from its first block, and a logdump smaller than the
	 * log (or a medium that says its blocks are of no size) is not written at all rather than written beyond.
	 */
	if (bs == 0 || bio->media->last_block >= ~0ULL / bs ||
	    (len = (staged_len + 1 + bs - 1) / bs * bs) > (bio->media->last_block + 1) * bs) {
		print("log: NOT WRITTEN to logdump: %d bytes, the partition has %d blocks of %d\n", staged_len + 1,
		    bio->media->last_block + 1, bs);
		compose_logs(klog, klog_len);
		return;
	}
	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to logdump: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf);

	if (!EFI_ERROR(ws)) {
		/*
		 * (out of the device's own cache first, and its answer taken: a flush that failed and a read
		 * back from that cache would have said "the same" of blocks that are not on the flash)
		 */
		fs = bio->flush_blocks(bio);
	}
	if (!EFI_ERROR(ws) && !EFI_ERROR(fs)) {
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(block_buffer)) {
			rs = bio->read_blocks(bio, bio->media->media_id, 0, len, block_buffer);
			same = !EFI_ERROR(rs) && memcmp(block_buffer, out_buf, len) == 0;
		}
	}
	print("log: %d bytes written to logdump (%x), flushed (%x), read back %s (%x)\n", len, ws, fs,
	    EFI_ERROR(ws) ? "not tried" : EFI_ERROR(fs) ? "not tried: the flush FAILED" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}
