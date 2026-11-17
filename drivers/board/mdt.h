/*
 * A processor's firmware as Qualcomm splits it (Linux: drivers/soc/qcom/mdt_loader.c): a 32-bit ELF whose
 * header, table of segments and block of hashes and certificates are the file NAME.mdt, and whose loadable
 * segments are a file each, NAME.bNN for segment NN. The checks that a header is one, that a segment lies
 * in the memory reserved for the processor, and that the header file is nothing but those three parts:
 * once, for the drivers that hand an image to TrustZone (the audio DSP's, the Wi-Fi DSP's). The files come
 * from a RAM disk: nothing here trusts a count, an address or a size in them.
 */
#ifndef HAKIOS_MDT_H
#define HAKIOS_MDT_H

#include <libkern/libkern.h>
#include <mach/mach_types.h>

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};
#define MDT_ELF_HEADER          52                              /* the ELF header; the table of segments follows it */
#define MDT_SEGMENT             32                              /* one entry of the table */
#define MDT_LOAD                1                               /* a segment's type: one that is loaded */
#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)      /* the segment that is the block of hashes */

/*
 * How many segments the header file describes, when it is one: a 32-bit little-endian ELF whose table of
 * segments is where this file reads it (right after the ELF header, entries of 32 bytes: what TrustZone
 * reads is then what was checked here), at least three segments (the header, the hashes, something to load)
 * and at most `most', its table inside the file, the file no larger than `largest' (what the driver hands
 * TrustZone in one piece). 0 when it is not.
 */
static inline uint32_t
mdt_segment_count(const uint8_t *header, uint64_t size, uint32_t most, uint64_t largest)
{
	uint32_t count;

	if (header == NULL || size < MDT_ELF_HEADER + 3 * MDT_SEGMENT || size > largest ||
	    memcmp(header, "\177ELF\1\1", 6) != 0) {
		return 0;
	}
	if ((header[28] | header[29] << 8 | header[30] << 16 | (uint32_t)header[31] << 24) != MDT_ELF_HEADER ||
	    (header[42] | header[43] << 8) != MDT_SEGMENT) {
		return 0;                       /* (e_phoff, e_phentsize: a table elsewhere, or of other entries) */
	}
	count = header[44] | header[45] << 8;
	if (count < 3 || count > most || MDT_ELF_HEADER + (uint64_t)MDT_SEGMENT * count > size) {
		return 0;
	}
	return count;
}

/* Segment i of the table (i below the count above). */
static inline void
mdt_segment(const uint8_t *header, uint32_t i, struct elf_segment *segment)
{
	memcpy(segment, header + MDT_ELF_HEADER + MDT_SEGMENT * i, sizeof(*segment));
}

/* Is this a segment that is loaded (and not the block of hashes)? */
static inline bool
mdt_loaded(const struct elf_segment *segment)
{
	return segment->type == MDT_LOAD && !MDT_HASH(segment->flags) && segment->memory_size != 0;
}

/* Does a loaded segment lie wholly in the memory [base, base + size), with no more in its file than in memory? */
static inline bool
mdt_within(const struct elf_segment *segment, uint64_t base, uint64_t size)
{
	return segment->paddr >= base && segment->file_size <= segment->memory_size &&
	       (uint64_t)segment->paddr - base <= size && segment->memory_size <= size - ((uint64_t)segment->paddr - base);
}

/* Do two stretches of memory [a, a + a_size) and [b, b + b_size) not overlap? (one segment not loaded over another) */
static inline bool
mdt_apart(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size)
{
	return (uint64_t)a + a_size <= b || (uint64_t)b + b_size <= a;
}

/*
 * Is the header file the ELF header, the table and the block of hashes, and nothing else: one segment of hashes
 * and only one, of some bytes, whose place in the file is right after the table, and the file ending with it?
 * `hashes': its size, or 0.
 */
static inline bool
mdt_header_whole(const uint8_t *header, uint32_t count, uint64_t size, uint32_t *hashes)
{
	struct elf_segment segment, found = { 0 };
	uint32_t seen = 0;

	for (uint32_t i = 0; i < count; i++) {
		mdt_segment(header, i, &segment);
		if (MDT_HASH(segment.flags)) {
			found = segment;
			seen++;
		}
	}
	*hashes = seen == 1 ? found.file_size : 0;
	return seen == 1 && found.file_size != 0 && found.offset == MDT_ELF_HEADER + (uint64_t)MDT_SEGMENT * count &&
	       MDT_ELF_HEADER + (uint64_t)MDT_SEGMENT * count + found.file_size == size;
}

#endif /* HAKIOS_MDT_H */
