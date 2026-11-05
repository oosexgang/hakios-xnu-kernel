/*
 * A .metallib taken apart: the MTLB container Apple's shader compiler writes, its list of functions, and where each
 * function's module (LLVM bitcode, "AIR") is. The same reading as tools/metallib.py, in C, for the Metal plug-in
 * (userland/src/HakiosMetal.m: -newLibraryWithData:, -newLibraryWithURL:) and for tools/metallib-test.sh, which checks
 * the two against each other on every .metallib of the system.
 *
 * The file is not trusted: a program hands the device whatever bytes it likes. Every offset and length is checked
 * against the size before it is used, and a file that does not hold together is refused with the reason.
 */
#ifndef HAKIOS_METALLIB_H
#define HAKIOS_METALLIB_H

#include <stddef.h>
#include <stdint.h>

/* the TYPE tag (tools/metallib.py, checked against the metadata of the system's 456 modules) */
enum hakios_metallib_type {
	HAKIOS_METALLIB_VERTEX = 0,
	HAKIOS_METALLIB_FRAGMENT = 1,
	HAKIOS_METALLIB_KERNEL = 2,
	HAKIOS_METALLIB_UNQUALIFIED = 3,
	HAKIOS_METALLIB_VISIBLE = 4,
	HAKIOS_METALLIB_EXTERN = 5,
	HAKIOS_METALLIB_INTERSECTION = 6,
	HAKIOS_METALLIB_NO_TYPE = 255,          /* the entry has no TYPE tag */
};

struct hakios_metallib_function {
	const char *name;                       /* into the file's bytes, NAME_LENGTH long; not NUL-terminated of itself */
	uint32_t name_length;
	uint8_t type;                           /* enum hakios_metallib_type, or the number as the file has it */
	uint16_t air[4];                        /* VERS: the AIR version (2.8.4.0) */
	uint64_t module_offset;                 /* the module's first byte, from the start of the file */
	uint64_t module_size;
};

struct hakios_metallib {
	uint16_t version[2];                    /* the container's (32769.2 on Darwin userland 16.5) */
	uint64_t file_size;                     /* what the header says the file is */
	uint32_t count;                         /* the functions in the file (more than were kept, if the caller's array was short) */
};

/*
 * Reads DATA (SIZE bytes). Up to MAX functions go into OUT (which may be NULL when MAX is 0, to count them first).
 * 0 on success; -1 on a file that is not a metallib or does not hold together, with *WHY saying where.
 */
int hakios_metallib_parse(const uint8_t *data, size_t size, struct hakios_metallib *info,
    struct hakios_metallib_function *out, uint32_t max, const char **why);

/*
 * The AIR of a universal metallib: a fat container (0xCAFEBABE, big endian, 20-byte entries as Mach-O's) of an AIR64 metallib and
 * a binary for Apple's own GPUs, the way the frameworks of newer systems ship their libraries (QuartzCore's default.metallib on
 * iPadOS 17: UniXNU's AdrenoMetal found it, and takes the AIR64 slice as Metal itself does). A thin metallib is returned as it is.
 * 0 with *THIN and *THIN_SIZE, or -1 (with *WHY) for a fat file with no AIR64 slice or one that is not inside it.
 */
int hakios_metallib_thin(const uint8_t *data, size_t size, const uint8_t **thin, size_t *thin_size, const char **why);

/* the TYPE number as tools/metallib.py names it: "vertex", "fragment", "kernel", ... */
const char *hakios_metallib_type_name(uint8_t type);

/*
 * The function's module without the LLVM bitcode wrapper (0x0B17C0DE: the header that says where the bitcode is in it),
 * if it has one: *BITCODE and *LENGTH, into DATA. 0, or -1 if the wrapper points outside the module.
 */
int hakios_metallib_bitcode(const uint8_t *data, size_t size, const struct hakios_metallib_function *function,
    const uint8_t **bitcode, uint64_t *length);

#endif
