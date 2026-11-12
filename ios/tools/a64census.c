/*
 * a64census: which instructions in the executable mappings of a dyld shared cache a Cortex-A78/A55
 * (ARMv8.2, as the SM7350 and SM7325 have) cannot execute. A file that is not a cache's is skipped.
 *
 *   a64census FILE...          (every file of a cache: the main file and its subcaches)
 *
 * The A78/A55 of a Snapdragon 780G report: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid
 * asimdrdm lrcpc dcpop asimddp. No pointer authentication: the PAC instructions of the hint space
 * (PACIBSP, AUTIBSP, XPACLRI, ...) are NOPs there, every other one is UNDEFINED. Counted are those,
 * and the later extensions arm64e code built for A12/A13 may use. Words that are data in a text
 * mapping (jump tables, literal pools) are counted too: the numbers are an upper bound.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct class {
	const char *name;
	uint32_t mask, value;
	uint64_t count;
};

static struct class classes[] = {
	/* pointer authentication, not in the hint space: UNDEFINED without FEAT_PAuth */
	{ "RETAA/RETAB",                 0xfffffbff, 0xd65f0bff, 0 },
	{ "BRAA/BRAB (reg modifier)",    0xfffff800, 0xd71f0800, 0 },
	{ "BLRAA/BLRAB (reg modifier)",  0xfffff800, 0xd73f0800, 0 },
	{ "BRAAZ/BRABZ",                 0xfffff81f, 0xd61f081f, 0 },
	{ "BLRAAZ/BLRABZ",               0xfffff81f, 0xd63f081f, 0 },
	{ "LDRAA/LDRAB",                 0xff200400, 0xf8200400, 0 },
	{ "PAC*/AUT* (register forms)",  0xffffc000, 0xdac10000, 0 },
	{ "XPACI/XPACD",                 0xfffffbe0, 0xdac143e0, 0 },
	{ "PACGA",                       0xffe0fc00, 0x9ac03000, 0 },
	{ "ERETAA/ERETAB",               0xfffffbff, 0xd69f0bff, 0 },
	/* hint-space PAC: NOPs on ARMv8.2 (harmless, for the record) */
	{ "hint PAC SP/Z forms (NOP here)", 0xffffff1f, 0xd503231f, 0 },
	/* ARMv8.3+ besides PAuth */
	{ "FJCVTZS (JSCVT)",             0xfffffc00, 0x1e7e0000, 0 },
	{ "FCMLA vector (FCMA)",         0xbf20e400, 0x2e00c400, 0 },
	{ "FCADD (FCMA)",                0xbf20ec00, 0x2e00e400, 0 },
	{ "FCMLA by element (FCMA)",     0xbf009400, 0x2f001000, 0 },
	{ "LDAPUR/STLUR (RCPC2)",        0x3f200c00, 0x19000000, 0 },
	{ "CFINV/XAFLAG/AXFLAG (FlagM)", 0xffffff9f, 0xd500401f, 0 },
	{ "RMIF (FlagM)",                0xffe07c10, 0xba000400, 0 },
	{ "SETF8/SETF16 (FlagM)",        0xffffbc1f, 0x3a00080d, 0 },
	{ "SB",                          0xffffffff, 0xd50330ff, 0 },
	{ "EOR3/BCAX (SHA3)",            0xff808000, 0xce000000, 0 },
	{ "RAX1/SHA512*",                0xffe0f000, 0xce608000, 0 },
	{ "XAR (SHA3)",                  0xffe00000, 0xce800000, 0 },
	{ "SM3/SM4",                     0xffe0c000, 0xce400000, 0 },
	{ "FRINT32/64 scalar (FRINTTS)", 0xffbe7c00, 0x1e284000, 0 },
	{ "DC CVADP",                    0xffffffe0, 0xd50b7d20, 0 },
	{ "SMMLA (I8MM)",                0xffe0fc00, 0x4e80a400, 0 },
	{ "Apple AMX",                   0xfffffc00, 0x00201000, 0 },
};
#define CLASSES (sizeof(classes) / sizeof(classes[0]))

struct mapping {
	uint64_t address, size, offset;
	uint32_t max_prot, init_prot;
};

static uint64_t words;

static void
scan(const uint8_t *code, uint64_t size)
{
	const uint32_t *w = (const uint32_t *)code;

	for (uint64_t i = 0; i < size / 4; i++) {
		uint32_t x = w[i];

		for (unsigned c = 0; c < CLASSES; c++) {
			if ((x & classes[c].mask) == classes[c].value) {
				classes[c].count++;
				break;
			}
		}
	}
	words += size / 4;
}

int
main(int argc, char **argv)
{
	for (int a = 1; a < argc; a++) {
		int fd = open(argv[a], O_RDONLY);
		struct stat st;
		const uint8_t *file;

		if (fd < 0 || fstat(fd, &st) != 0) {
			perror(argv[a]);
			continue;
		}
		file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (file == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		if (st.st_size > 0x20 && memcmp(file, "dyld_v1", 7) == 0) {
			uint32_t mapping_offset, mapping_count;

			memcpy(&mapping_offset, file + 0x10, 4);
			memcpy(&mapping_count, file + 0x14, 4);
			for (uint32_t m = 0; m < mapping_count; m++) {
				struct mapping map;

				memcpy(&map, file + mapping_offset + m * sizeof(map), sizeof(map));
				if ((map.init_prot & 4) && map.offset + map.size <= (uint64_t)st.st_size) {
					fprintf(stderr, "%s: executable mapping at %#llx, %llu MB\n", argv[a],
					    (unsigned long long)map.address, (unsigned long long)(map.size >> 20));
					scan(file + map.offset, map.size);
				}
			}
		} else {
			fprintf(stderr, "%s: not a dyld cache file, skipped\n", argv[a]);
		}
		munmap((void *)file, st.st_size);
		close(fd);
	}
	printf("%llu instruction words scanned\n", (unsigned long long)words);
	for (unsigned c = 0; c < CLASSES; c++) {
		printf("%12llu  %7.4f%%  %s\n", (unsigned long long)classes[c].count,
		    words ? 100.0 * classes[c].count / words : 0.0, classes[c].name);
	}
	return 0;
}
