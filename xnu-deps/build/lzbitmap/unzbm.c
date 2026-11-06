/*
 * unzbm: finish what 7z leaves undone for files compressed with LZBITMAP.
 *
 * 7z extracts such a file from an APFS image as an empty file plus its two
 * attributes, "NAME:com.apple.decmpfs" (the compression header) and
 * "NAME:com.apple.ResourceFork" (the compressed data, in 64K blocks behind a
 * table of offsets). For every NAME given, this writes the real contents and
 * removes the two attribute files.
 *
 * The decompressor is libzbitmap (Ernesto A. Fernandez, GPL-2.0+ OR MIT),
 * as shipped with the linux-apfs-rw driver.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libzbitmap.h"

#define BLOCK 0x10000u
#define LZBITMAP_RSRC 14

static uint8_t *
slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data;

	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*size = (size_t)ftell(f);
	rewind(f);
	data = malloc(*size + 1);
	if (fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int
expand(const char *name)
{
	char header_path[4096], fork_path[4096];
	size_t header_size, fork_size;
	uint8_t *header, *fork, *out;
	uint64_t size, blocks;
	FILE *f;

	snprintf(header_path, sizeof(header_path), "%s:com.apple.decmpfs", name);
	snprintf(fork_path, sizeof(fork_path), "%s:com.apple.ResourceFork", name);
	header = slurp(header_path, &header_size);
	fork = slurp(fork_path, &fork_size);
	if (!header || !fork || header_size < 16 || memcmp(header, "fpmc", 4) != 0 || le32(header + 4) != LZBITMAP_RSRC) {
		fprintf(stderr, "unzbm: %s: not an LZBITMAP resource-fork file\n", name);
		return 1;
	}
	size = le32(header + 8) | (uint64_t)le32(header + 12) << 32;
	blocks = (size + BLOCK - 1) / BLOCK;
	if ((blocks + 1) * 4 > fork_size) {
		fprintf(stderr, "unzbm: %s: block table does not fit\n", name);
		return 1;
	}
	out = malloc(size ? size : 1);
	for (uint64_t b = 0; b < blocks; b++) {
		uint32_t from = le32(fork + 4 * b), to = le32(fork + 4 * (b + 1));
		size_t want = size - b * BLOCK > BLOCK ? BLOCK : (size_t)(size - b * BLOCK), got = 0;

		/* a block that did not shrink is stored as it is behind one byte, 0xff */
		if (from < to && to <= fork_size && to - from == want + 1 && fork[from] == 0xff) {
			memcpy(out + b * BLOCK, fork + from + 1, want);
			continue;
		}
		if (from > to || to > fork_size || zbm_decompress(out + b * BLOCK, want, fork + from, to - from, &got) != 0 ||
		    got != want) {
			fprintf(stderr, "unzbm: %s: block %llu does not decompress\n", name, (unsigned long long)b);
			return 1;
		}
	}
	f = fopen(name, "wb");
	if (!f || fwrite(out, 1, size, f) != size || fclose(f) != 0) {
		fprintf(stderr, "unzbm: %s: %s\n", name, strerror(errno));
		return 1;
	}
	unlink(header_path);
	unlink(fork_path);
	free(out);
	free(fork);
	free(header);
	return 0;
}

int
main(int argc, char **argv)
{
	int failed = 0;

	for (int i = 1; i < argc; i++) {
		failed += expand(argv[i]);
	}
	return failed != 0;
}
