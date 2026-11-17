/* Part of the loader (included by loader.c, not compiled alone): the kernel's memory: the firmware's map and the block chosen from it. */
/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static uint64_t download_ramdisk_patch; /* "oem ramdisk patch": where the next download goes in it */
#define RAMDISK_CRC_CHUNK (256ULL << 20)
static char last_result[64] = "idle";

/* "getvar result": what the last command that runs the kernel came to, WHAT A (decimal) B (hex), at most 63 letters */
static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	format_into(last_result, sizeof(last_result), "%s %d %x", what, a, b);
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

#define LOW_DMA_SIZE        0x8000000ULL    /* 128 MB: the sound (64 KiB), the Wi-Fi chip's firmware, its memory and rings, the driver's buffers (124 MB from 4 MB on; 60 ran out before the first receive buffers: run wifi22) */
static uint64_t low_dma;

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload sits in RAM that UEFI may consider free: stay clear of it */
		if (lo < pay_hi && hi > pay_lo) {
			/* (the larger of what is left below it and above it; either may be nothing, and neither sum wraps) */
			uint64_t below = pay_lo > lo ? pay_lo - lo : 0, above = hi > pay_hi ? hi - pay_hi : 0;

			if (below > above) {           /* (the same: above it, as before) */
				hi = pay_lo;
			} else {
				lo = pay_hi;
			}
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0 && (pi->forced_base & (BASE_ALIGN - 1)) != 0) {
		/* (the kernel maps its memory in blocks of that size from its base: a base between two is none it can take) */
		print("physbase %x is not a multiple of %x: not taken\n", pi->forced_base, (uint64_t)BASE_ALIGN);
	} else if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	if (best_size == 0) {
		/* (said here: what a firmware answers to a claim of no pages at address 0 is its own affair) */
		print("no free range of 32 MB in the firmware's memory map: the kernel has nowhere to run (\"oem memmap\" shows the map)\n");
		return 0;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	/*
	 * Memory below 4 GB for devices that take only 32-bit addresses (the audio DSP's buffers): every
	 * page of the kernel's window is above. Once, kept for the kernel (UEFI stays alive), and handed to
	 * it as boot-arg lowdma=<address> (LOW_DMA_SIZE bytes).
	 */
	if (low_dma == 0) {
		uint64_t at = 0xFFFFFFFFULL;
		if (EFI_ERROR(BS->allocate_pages(AllocateMaxAddress, EfiLoaderData, LOW_DMA_SIZE >> 12, &at))) {
			at = 0;
		}
		if (at != 0 && at < pay_hi && at + LOW_DMA_SIZE > pay_lo) {
			/* (over the payload, which UEFI may count as free: the devices would write into the next run's) */
			BS->free_pages(at, LOW_DMA_SIZE >> 12);
			at = pay_lo - 1;
			if (pay_lo <= LOW_DMA_SIZE ||
			    EFI_ERROR(BS->allocate_pages(AllocateMaxAddress, EfiLoaderData, LOW_DMA_SIZE >> 12, &at))) {
				at = 0;
			}
		}
		low_dma = at;
		print("low DMA memory %x\n", low_dma);
	}
	return 1;
}
