/* Part of the loader (included by loader.c, not compiled alone): the NVRAM image the kernel is given. */
/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}
