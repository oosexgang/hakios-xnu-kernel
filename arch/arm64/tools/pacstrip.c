/*
 * pacstrip: make arm64e code run on a CPU without pointer authentication (an ARMv8.2 core such as the
 * Cortex-A78/A55 of a Snapdragon 7-series: RCpc, no PAuth) by rewriting every PAC instruction 1:1 into its identity, in a copy.
 *
 *   pacstrip cache OUTDIR FILE...   copy every file of a dyld shared cache (main file + subcaches;
 *                                   .symbols/.atlas and other non-cache files are copied unchanged)
 *                                   into OUTDIR and rewrite the code sections of every image there
 *   pacstrip macho IN OUT           copy a Mach-O (thin arm64e, or fat with an arm64e slice) and
 *                                   rewrite the code sections of its arm64e slice; the page hashes
 *                                   of its code signature, if it has one, are made again
 *   pacstrip census FILE...         read-only: the same walk and classification over a cache (all
 *                                   its files) or one Mach-O; rewritable classes are reported as
 *                                   "still present" (0 after pacstrip), plus the words of executable
 *                                   cache mappings that lie outside every code section
 *   pacstrip word HEX...            classify/rewrite single instruction words (encoding tests)
 *
 * Only sections flagged S_ATTR_PURE_INSTRUCTIONS or S_ATTR_SOME_INSTRUCTIONS are touched, plus
 * flag-less sections of executable segments named "__text*"/"text*" (Metal's __TEXT,text_env holds
 * functions with flags 0; it is reported when rewritten). In a cache
 * the sections are found by walking every image's mach_header (dyld_cache_header imagesOffset/
 * imagesCount, imagesTextOffset/imagesTextCount and dyldInCacheMH), unslid addresses being mapped
 * to file offsets through the mappings of all the cache files; each address range is processed
 * exactly once even when several images (or both image lists) describe it. The cache's TEXT_STUBS
 * mappings (dyld_cache_mapping_and_slide_info flag 8: stub islands for branches between subcaches,
 * adrp/add/ldr/braa x16, x17, owned by no image) are pure code and are rewritten as a whole too.
 *
 * Rewrites (same size, in place):
 *   RETAA/RETAB                      -> RET
 *   BRAA/BRAB Xn,Xm, BRAAZ/BRABZ Xn  -> BR Xn
 *   BLRAA/BLRAB Xn,Xm, BLRAAZ/BLRABZ -> BLR Xn
 *   PACIA..AUTDZB register forms, XPACI/XPACD -> NOP (they modify Xd in place: identity without PAC)
 *   PACGA Xd,Xn,Xm                   -> EOR Xd,Xn,Xm
 *   LDRAA/LDRAB Xt,[Xn,#off]         -> LDR Xt,[Xn,#off] (0..4088) or LDUR (-256..-8)
 *   LDRAA/LDRAB Xt,[Xn,#off]!        -> LDR Xt,[Xn,#off]! (-256..248)
 *   LDAPUR{B,H,,} Rt,[Xn,#0]         -> LDAPR{B,H,,} Rt,[Xn]
 *   STLUR{B,H,,} Rt,[Xn,#0]          -> STLR{B,H,,} Rt,[Xn]
 * Hint-space PAC (PACIBSP, AUTIBSP, XPACLRI, *1716, ...) are NOPs on ARMv8.2 and stay. Everything
 * else such a CPU cannot execute is counted as left over (for the kernel's EL0 emulator), per class and
 * per image. Data words inside code sections that happen to match are rewritten too (upper bound).
 */
#define _GNU_SOURCE /* copy_file_range */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u
#define MH_MAGIC_64              0xfeedfacfu
#define FAT_MAGIC                0xcafebabeu
#define FAT_MAGIC_64             0xcafebabfu
#define LC_SEGMENT_64            0x19u
#define CPU_TYPE_ARM64           0x0100000c
#define CPU_SUBTYPE_ARM64E       2
#define CPU_SUBTYPE_MASK         0xff000000u
#define VM_PROT_EXECUTE          4u

#define NOP 0xd503201fu

/* ---- instruction classes ---- */

enum {
	R_RETA, R_BRA, R_BLRA, R_BRAZ, R_BLRAZ, R_PACAUT, R_XPAC, R_PACGA,
	R_LDRA_LDR, R_LDRA_LDUR, R_LDRA_PRE, R_LDAPUR0, R_STLUR0,
	NREW
};
static const char *const rew_name[NREW] = {
	"RETAA/RETAB -> RET",
	"BRAA/BRAB -> BR",
	"BLRAA/BLRAB -> BLR",
	"BRAAZ/BRABZ -> BR",
	"BLRAAZ/BLRABZ -> BLR",
	"PAC*/AUT* register forms -> NOP",
	"XPACI/XPACD -> NOP",
	"PACGA -> EOR",
	"LDRAA/LDRAB #off>=0 -> LDR (imm12)",
	"LDRAA/LDRAB #-256..-8 -> LDUR",
	"LDRAA/LDRAB ! #-256..248 -> LDR pre-index",
	"LDAPUR{B,H,W,X} #0 -> LDAPR",
	"STLUR{B,H,W,X} #0 -> STLR",
};

/* left over: what such a CPU cannot execute and pacstrip cannot rewrite */
struct lclass {
	const char *name;
	uint32_t mask, value; /* 0, 0: decided in classify() */
};
enum { L_LDRA_NOWB, L_LDRA_WB, L_RCPC2_OFF, L_LDAPURS, L_TABLE };
static const struct lclass left_class[] = {
	{ "LDRAA/LDRAB #-4096..-264 (no LDR form)", 0, 0 },
	{ "LDRAA/LDRAB ! #off outside -256..248",   0, 0 },
	{ "LDAPUR/STLUR #off != 0 (RCPC2)",         0, 0 },
	{ "LDAPURS* sign-extending (RCPC2)",        0, 0 },
	{ "ERETAA/ERETAB",                 0xfffffbff, 0xd69f0bff },
	{ "FJCVTZS (JSCVT)",               0xfffffc00, 0x1e7e0000 },
	{ "FMLAL/FMLSL vector (FHM)",      0xbf60fc00, 0x0e20ec00 },
	{ "FMLAL2/FMLSL2 vector (FHM)",    0xbf60fc00, 0x2e20cc00 },
	{ "FMLAL/FMLSL by element (FHM)",  0xbfc0b400, 0x0f800000 },
	{ "FMLAL2/FMLSL2 by element (FHM)", 0xbfc0b400, 0x2f808000 },
	{ "FCMLA vector (FCMA)",           0xbf20e400, 0x2e00c400 },
	{ "FCADD (FCMA)",                  0xbf20ec00, 0x2e00e400 },
	{ "FCMLA by element (FCMA)",       0xbf009400, 0x2f001000 },
	{ "CFINV/XAFLAG/AXFLAG (FlagM)",   0xffffff9f, 0xd500401f },
	{ "RMIF (FlagM)",                  0xffe07c10, 0xba000400 },
	{ "SETF8/SETF16 (FlagM)",          0xffffbc1f, 0x3a00080d },
	{ "SB",                            0xffffffff, 0xd50330ff },
	{ "EOR3/BCAX (SHA3)",              0xffc08000, 0xce000000 },
	{ "RAX1/SHA512H/H2/SU1",           0xffe0f000, 0xce608000 },
	{ "SHA512SU0/SM4E",                0xfffff800, 0xcec08000 },
	{ "XAR (SHA3)",                    0xffe00000, 0xce800000 },
	{ "SM3SS1/SM3TT* (SM3)",           0xffe00000, 0xce400000 },
	{ "SM3PARTW1/2, SM4EKEY",          0xffe0f000, 0xce60c000 },
	{ "FRINT32/64 scalar (FRINTTS)",   0xffbe7c00, 0x1e284000 },
	{ "DC CVADP",                      0xffffffe0, 0xd50b7d20 },
	{ "SMMLA (I8MM)",                  0xffe0fc00, 0x4e80a400 },
	{ "Apple AMX",                     0xfffffc00, 0x00201000 },
};
#define NLEFT ((int)(sizeof(left_class) / sizeof(left_class[0])))

/* classify() results: 0..NREW-1 rewritable, LEFT0+i left over i, C_HINT kept, C_NONE nothing */
#define LEFT0  64
#define C_HINT 128
#define C_NONE (-1)

static int
classify(uint32_t x, uint32_t *out)
{
	uint32_t base = x & 0x3ffu; /* Rn:Rt (Rn at 9..5, Rt/Rd at 4..0) */
	uint32_t rn = (x >> 5) & 31;

	if ((x & 0xfffffbffu) == 0xd65f0bffu) {
		*out = 0xd65f03c0u;
		return R_RETA;
	}
	if ((x & 0xfffff800u) == 0xd71f0800u) {
		*out = 0xd61f0000u | rn << 5;
		return R_BRA;
	}
	if ((x & 0xfffff800u) == 0xd73f0800u) {
		*out = 0xd63f0000u | rn << 5;
		return R_BLRA;
	}
	if ((x & 0xfffff81fu) == 0xd61f081fu) {
		*out = 0xd61f0000u | rn << 5;
		return R_BRAZ;
	}
	if ((x & 0xfffff81fu) == 0xd63f081fu) {
		*out = 0xd63f0000u | rn << 5;
		return R_BLRAZ;
	}
	if ((x & 0xffffc000u) == 0xdac10000u) {
		/* opcode 0..7: PACIA..AUTDB Xd, Xn|SP; 8..15: PACIZA..AUTDZB Xd (Rn must be 11111) */
		if (((x >> 10) & 15) < 8 || rn == 31) {
			*out = NOP;
			return R_PACAUT;
		}
		return C_NONE;
	}
	if ((x & 0xfffffbe0u) == 0xdac143e0u) {
		*out = NOP;
		return R_XPAC;
	}
	if ((x & 0xffe0fc00u) == 0x9ac03000u) {
		*out = 0xca000000u | (x & 0x001f03ffu);
		return R_PACGA;
	}
	if ((x & 0xff200400u) == 0xf8200400u) {
		/* LDRAA/LDRAB: M at 23, S at 22, imm9 at 20..12, W at 11; offset = SExt(S:imm9) * 8 */
		int32_t imm = (int32_t)(((x >> 12) & 0x1ff) | ((x >> 22) & 1) << 9);
		int32_t off;

		if (imm & 0x200)
			imm -= 0x400;
		off = imm * 8;
		if (!(x & (1u << 11))) {
			if (off >= 0) {
				*out = 0xf9400000u | (uint32_t)(off / 8) << 10 | base;
				return R_LDRA_LDR;
			}
			if (off >= -256) {
				*out = 0xf8400000u | ((uint32_t)off & 0x1ff) << 12 | base;
				return R_LDRA_LDUR;
			}
			return LEFT0 + L_LDRA_NOWB;
		}
		if (off >= -256 && off <= 255) {
			*out = 0xf8400c00u | ((uint32_t)off & 0x1ff) << 12 | base;
			return R_LDRA_PRE;
		}
		return LEFT0 + L_LDRA_WB;
	}
	if ((x & 0x3f200c00u) == 0x19000000u) {
		/* RCPC2: size at 31..30, opc at 23..22, imm9 at 20..12 */
		uint32_t size = x >> 30, opc = (x >> 22) & 3, imm9 = (x >> 12) & 0x1ff;

		switch (opc) {
		case 0: /* STLURB/STLURH/STLUR W/X */
			if (imm9 != 0)
				return LEFT0 + L_RCPC2_OFF;
			*out = size << 30 | 0x089ffc00u | base;
			return R_STLUR0;
		case 1: /* LDAPURB/LDAPURH/LDAPUR W/X */
			if (imm9 != 0)
				return LEFT0 + L_RCPC2_OFF;
			*out = size << 30 | 0x38bfc000u | base;
			return R_LDAPUR0;
		case 2: /* LDAPURSB/LDAPURSH/LDAPURSW Xt; size 3 unallocated */
			return size < 3 ? LEFT0 + L_LDAPURS : C_NONE;
		default: /* LDAPURSB/LDAPURSH Wt; size 2/3 unallocated */
			return size < 2 ? LEFT0 + L_LDAPURS : C_NONE;
		}
	}
	if ((x & 0xffffff1fu) == 0xd503231fu || (x & 0xffffff1fu) == 0xd503211fu || x == 0xd50320ffu)
		return C_HINT;
	for (int l = L_TABLE; l < NLEFT; l++)
		if ((x & left_class[l].mask) == left_class[l].value)
			return LEFT0 + l;
	return C_NONE;
}

/* ---- statistics ---- */

struct image {
	uint64_t addr;
	const char *path;
	uint64_t left[NLEFT];
	uint64_t lefttotal;
};

static struct image *images;
static uint32_t nimages;
static uint64_t rew[NREW], left[NLEFT], hint, words, sections, shared_bytes;
static uint32_t stubmaps;
static uint64_t gap_rew[NREW], gap_left[NLEFT], gap_hint, gap_words;
static int dowrite;

static void
process(uint32_t *w, uint64_t n, struct image *im)
{
	for (uint64_t i = 0; i < n; i++) {
		uint32_t out;
		int c = classify(w[i], &out);

		if (c == C_NONE)
			continue;
		if (c < NREW) {
			rew[c]++;
			if (dowrite)
				w[i] = out;
		} else if (c == C_HINT) {
			hint++;
		} else {
			left[c - LEFT0]++;
			im->left[c - LEFT0]++;
			im->lefttotal++;
		}
	}
	words += n;
}

/* addr: unslid address of w[0]; PACSTRIP_GAPS=1 lists the PAC branch/register-form words found */
static void
process_gap(const uint32_t *w, uint64_t n, uint64_t addr)
{
	static int list = -1;

	if (list < 0)
		list = getenv("PACSTRIP_GAPS") != NULL;
	for (uint64_t i = 0; i < n; i++) {
		uint32_t out;
		int c = classify(w[i], &out);

		if (c == C_NONE)
			continue;
		if (c < NREW) {
			gap_rew[c]++;
			if (list && c <= R_PACGA)
				fprintf(stderr, "gap %#llx: %08x %s\n", (unsigned long long)(addr + 4 * i), w[i],
				    rew_name[c]);
		} else if (c == C_HINT) {
			gap_hint++;
		} else {
			gap_left[c - LEFT0]++;
		}
	}
	gap_words += n;
}

static int
cmp_lefttotal(const void *a, const void *b)
{
	const struct image *x = *(struct image *const *)a, *y = *(struct image *const *)b;

	return x->lefttotal < y->lefttotal ? 1 : x->lefttotal > y->lefttotal ? -1 : 0;
}

static void
report(int census, int cache)
{
	uint64_t total_rew = 0, total_left = 0;

	printf("\n%llu code sections", (unsigned long long)sections);
	if (cache)
		printf(" + %u TEXT_STUBS mappings", stubmaps);
	printf(", %llu instruction words (%llu MB)", (unsigned long long)words,
	    (unsigned long long)(words >> 18));
	if (cache)
		printf(", %llu bytes skipped as already processed (shared ranges)",
		    (unsigned long long)shared_bytes);
	printf("\n\n%s:\n", census ? "PAC instructions still present (rewritable)" : "rewritten");
	for (int c = 0; c < NREW; c++) {
		printf("%12llu  %s\n", (unsigned long long)rew[c], rew_name[c]);
		total_rew += rew[c];
	}
	printf("%12llu  total\n\nkept (hint space, NOP on ARMv8.2):\n%12llu  PACI*SP/Z, AUTI*SP/Z, *1716, "
	    "XPACLRI\n\nleft over (UNDEFINED on ARMv8.2, for the EL0 emulator):\n",
	    (unsigned long long)total_rew, (unsigned long long)hint);
	for (int l = 0; l < NLEFT; l++) {
		printf("%12llu  %s\n", (unsigned long long)left[l], left_class[l].name);
		total_left += left[l];
	}
	printf("%12llu  total\n", (unsigned long long)total_left);

	if (cache) {
		uint64_t g = 0;

		printf("\nexecutable mapping words outside every code section (never touched): %llu\n",
		    (unsigned long long)gap_words);
		for (int c = 0; c < NREW; c++)
			if (gap_rew[c]) {
				printf("%12llu  %s (not rewritten)\n", (unsigned long long)gap_rew[c], rew_name[c]);
				g += gap_rew[c];
			}
		for (int l = 0; l < NLEFT; l++)
			if (gap_left[l]) {
				printf("%12llu  %s\n", (unsigned long long)gap_left[l], left_class[l].name);
				g += gap_left[l];
			}
		if (gap_hint)
			printf("%12llu  hint-space PAC\n", (unsigned long long)gap_hint);
		if (!g && !gap_hint)
			printf("%12s  no PAC or post-v8.2 pattern there\n", "");
	}

	if (nimages > 0 && total_left > 0) {
		struct image **order = malloc(nimages * sizeof(*order));
		uint32_t n = 0;

		for (uint32_t i = 0; i < nimages; i++)
			if (images[i].lefttotal)
				order[n++] = &images[i];
		qsort(order, n, sizeof(*order), cmp_lefttotal);
		printf("\nleft over per image (%u images have some; top 30):\n", n);
		for (uint32_t i = 0; i < n && i < 30; i++) {
			printf("%10llu  %s:", (unsigned long long)order[i]->lefttotal, order[i]->path);
			for (int l = 0; l < NLEFT; l++)
				if (order[i]->left[l])
					printf(" %s=%llu", left_class[l].name, (unsigned long long)order[i]->left[l]);
			printf("\n");
		}
		free(order);
	}
}

/* ---- files ---- */

static void
die(const char *what, const char *arg)
{
	fprintf(stderr, "pacstrip: %s%s%s: %s\n", what, arg ? " " : "", arg ? arg : "",
	    errno ? strerror(errno) : "failed");
	exit(1);
}

/* copy in -> out (refusing to overwrite in), return 0 */
static void
copy_file(const char *in, const char *out)
{
	struct stat si, so;
	int fi, fo;
	off_t left_bytes;

	fi = open(in, O_RDONLY);
	if (fi < 0 || fstat(fi, &si) != 0)
		die("open", in);
	if (stat(out, &so) == 0 && so.st_dev == si.st_dev && so.st_ino == si.st_ino) {
		errno = EINVAL;
		die("output is the input file", out);
	}
	fo = open(out, O_WRONLY | O_CREAT | O_TRUNC, (si.st_mode & 0777) | 0200);     /* (rewritten in place below) */
	if (fo < 0)
		die("create", out);
	left_bytes = si.st_size;
	while (left_bytes > 0) {
		ssize_t n;

#if defined(__APPLE__)
		/* macOS has no copy_file_range; use the portable read/write path below. */
		errno = ENOSYS;
		n = -1;
#else
		n = copy_file_range(fi, NULL, fo, NULL, (size_t)(left_bytes > (1 << 30) ? (1 << 30) : left_bytes), 0);
#endif

		if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL)) {
			static char buf[1 << 20];
			ssize_t r;

			while ((r = read(fi, buf, sizeof(buf))) > 0)
				if (write(fo, buf, (size_t)r) != r)
					die("write", out);
			if (r < 0)
				die("read", in);
			break;
		}
		if (n <= 0)
			die("copy_file_range", out);
		left_bytes -= n;
	}
	if (close(fo) != 0)
		die("close", out);
	close(fi);
}

static uint8_t *
map_file(const char *path, int rw, uint64_t *size)
{
	struct stat st;
	int fd = open(path, rw ? O_RDWR : O_RDONLY);
	void *p;

	if (fd < 0 || fstat(fd, &st) != 0)
		die("open", path);
	*size = (uint64_t)st.st_size;
	if (st.st_size == 0) {
		close(fd);
		return NULL;
	}
	p = mmap(NULL, (size_t)st.st_size, rw ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		die("mmap", path);
	close(fd);
	return p;
}

static void
unmap_file(uint8_t *p, uint64_t size)
{
	if (!p)
		return;
	if (dowrite && msync(p, size, MS_SYNC) != 0)
		die("msync", NULL);
	munmap(p, size);
}

static uint32_t
rd32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

static uint64_t
rd64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, 8);
	return v;
}

/*
 * 1: section_64 flagged S_ATTR_PURE_INSTRUCTIONS or S_ATTR_SOME_INSTRUCTIONS; 2: a regular section
 * (type 0, no attributes) of an executable segment named like code ("__text*", "text*"): Metal's
 * __TEXT,text_env holds real functions (PACIBSP ... RETAB, calls through __auth_stubs) with flags 0.
 */
static int
is_code_section(const uint8_t *sec, uint32_t seg_initprot)
{
	uint32_t flags = rd32(sec + 64);

	if (flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))
		return 1;
	if (flags != 0 || !(seg_initprot & VM_PROT_EXECUTE))
		return 0;
	return strncmp((const char *)sec, "__text", 6) == 0 || strncmp((const char *)sec, "text", 4) == 0 ? 2 : 0;
}

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ---- dyld shared cache ---- */

#define DYLD_CACHE_MAPPING_TEXT_STUBS 8u

struct cmap {
	uint64_t addr, size, off, flags; /* flags: dyld_cache_mapping_and_slide_info */
	uint32_t initprot;
	uint8_t *base; /* file mapping */
	uint64_t filesize;
	const char *path;
};

struct range {
	uint64_t addr, size;
	uint32_t image;
};

static struct cmap *cmaps;
static uint32_t ncmaps;

static int
cmp_cmap(const void *a, const void *b)
{
	const struct cmap *x = a, *y = b;

	return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static int
cmp_range(const void *a, const void *b)
{
	const struct range *x = a, *y = b;

	if (x->addr != y->addr)
		return x->addr < y->addr ? -1 : 1;
	return x->size > y->size ? -1 : x->size < y->size; /* longest first */
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static const struct cmap *
find_map(uint64_t addr)
{
	uint32_t lo = 0, hi = ncmaps;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;

		if (addr < cmaps[mid].addr)
			hi = mid;
		else if (addr >= cmaps[mid].addr + cmaps[mid].size)
			lo = mid + 1;
		else
			return &cmaps[mid];
	}
	return NULL;
}

/* pointer to n bytes at unslid address addr, or NULL */
static uint8_t *
resolve(uint64_t addr, uint64_t n, const struct cmap **mp)
{
	const struct cmap *m = find_map(addr);

	if (!m || addr + n > m->addr + m->size || m->off + (addr - m->addr) + n > m->filesize)
		return NULL;
	if (mp)
		*mp = m;
	return m->base + m->off + (addr - m->addr);
}

static int
has_suffix(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);

	return a >= b && strcmp(s + a - b, suf) == 0;
}

/*
 * The ad hoc code signature made right again for the rewritten code: every CodeDirectory of the
 * SuperBlob at sigoff gets the hashes of the pages as they are now (with the hash type it names).
 * The special slots (entitlements, requirements, Info.plist) are left alone: those blobs did not
 * change. An ad hoc signature has no certificate, so the CodeDirectory is all there is to it; the
 * kernel checks a Mach-O's first page against it at exec (a mismatch clears CS_VALID, and with it the
 * platform-binary status) and every page it brings in later.
 */
#include <openssl/sha.h>
static uint64_t resigned_pages, resigned_dirs;

static void
resign(uint8_t *base, uint64_t size, uint64_t sigoff, uint64_t sigsize)
{
	uint8_t *sb = base + sigoff;

	if (sigsize < 12 || sigoff + sigsize > size || be32(sb) != 0xfade0cc0)
		return;
	uint32_t count = be32(sb + 8);
	for (uint32_t k = 0; k < count && 12 + 8 * (uint64_t)(k + 1) <= sigsize; k++) {
		uint32_t off = be32(sb + 16 + 8 * k);
		uint8_t *cd = sb + off;

		if (off + 44 > sigsize || be32(cd) != 0xfade0c02)
			continue;
		uint32_t version = be32(cd + 8), hashoff = be32(cd + 16), nslots = be32(cd + 28);
		uint64_t limit = be32(cd + 32);
		uint8_t hsize = cd[36], htype = cd[37], pshift = cd[39];
		if (version >= 0x20300 && off + 64 <= sigsize) {
			uint64_t l64 = (uint64_t)be32(cd + 56) << 32 | be32(cd + 60);
			if (l64)
				limit = l64;
		}
		uint64_t ps = pshift ? 1ULL << pshift : limit;
		if (limit > sigoff || off + hashoff + (uint64_t)nslots * hsize > sigsize)
			continue;
		for (uint32_t i = 0; i < nslots; i++) {
			uint64_t start = (uint64_t)i * ps, end = start + ps > limit ? limit : start + ps;
			uint8_t digest[SHA384_DIGEST_LENGTH];

			if (start >= limit)
				break;
			switch (htype) {
			case 1:
				SHA1(base + start, end - start, digest);
				break;
			case 2:
			case 3:
				SHA256(base + start, end - start, digest);
				break;
			case 4:
				SHA384(base + start, end - start, digest);
				break;
			default:
				fprintf(stderr, "resign: hash type %u unknown, signature left as it was\n", htype);
				return;
			}
			memcpy(cd + hashoff + (uint64_t)i * hsize, digest, hsize);
			resigned_pages++;
		}
		resigned_dirs++;
	}
}

static int
do_cache(int census, const char *outdir, int nfiles, char **files)
{
	struct cfile {
		const char *path;
		uint8_t *map;
		uint64_t size;
	} *cf = calloc((size_t)nfiles, sizeof(*cf));
	uint64_t *haddr = NULL;
	uint64_t nhaddr = 0, caphaddr = 0;
	const char **hpath = NULL;
	struct range *ranges = NULL;
	uint64_t nranges = 0, capranges = 0;
	struct range *done = NULL; /* processed pieces, ascending */
	uint64_t ndone = 0, capdone = 0;
	uint64_t cursor = 0, nonexec = 0, unresolved = 0;

	dowrite = !census;
	for (int i = 0; i < nfiles; i++) {
		const char *src = files[i], *bn = strrchr(src, '/');
		char *dst;
		uint8_t hdr[16] = { 0 };
		int fd;

		bn = bn ? bn + 1 : src;
		fd = open(src, O_RDONLY);
		if (fd < 0)
			die("open", src);
		if (read(fd, hdr, sizeof(hdr)) < 0)
			die("read", src);
		close(fd);
		if (census) {
			cf[i].path = src;
		} else {
			dst = malloc(strlen(outdir) + strlen(bn) + 2);
			sprintf(dst, "%s/%s", outdir, bn);
			fprintf(stderr, "copy %s -> %s\n", src, dst);
			copy_file(src, dst);
			cf[i].path = dst;
		}
		if (memcmp(hdr, "dyld_v1", 7) != 0 || has_suffix(bn, ".symbols") || has_suffix(bn, ".atlas")) {
			fprintf(stderr, "%s: not rewritten (%s)\n", bn,
			    memcmp(hdr, "dyld_v1", 7) ? "not a dyld cache file" : "no code");
			cf[i].path = NULL;
			continue;
		}
		cf[i].map = map_file(cf[i].path, !census, &cf[i].size);
	}

	/* all mappings of all files */
	for (int i = 0; i < nfiles; i++) {
		const uint8_t *h = cf[i].map;
		uint32_t moff, mcnt, mwsoff = 0, mwscnt = 0;

		if (!h || cf[i].size < 0x200)
			continue;
		moff = rd32(h + 0x10);
		mcnt = rd32(h + 0x14);
		if ((uint64_t)moff + (uint64_t)mcnt * 32 > cf[i].size) {
			fprintf(stderr, "%s: bad mapping table\n", cf[i].path);
			continue;
		}
		if (moff >= 0x140) {
			mwsoff = rd32(h + 0x138);
			mwscnt = rd32(h + 0x13c);
			if (mwscnt != mcnt || (uint64_t)mwsoff + (uint64_t)mwscnt * 56 > cf[i].size)
				mwscnt = 0;
		}
		cmaps = realloc(cmaps, (ncmaps + mcnt) * sizeof(*cmaps));
		for (uint32_t m = 0; m < mcnt; m++) {
			const uint8_t *e = h + moff + m * 32;
			struct cmap *c = &cmaps[ncmaps++];

			c->addr = rd64(e);
			c->size = rd64(e + 8);
			c->off = rd64(e + 16);
			c->initprot = rd32(e + 28);
			c->flags = mwscnt ? rd64(h + mwsoff + m * 56 + 40) : 0;
			c->base = cf[i].map;
			c->filesize = cf[i].size;
			c->path = cf[i].path;
		}
	}
	qsort(cmaps, ncmaps, sizeof(*cmaps), cmp_cmap);
	for (uint32_t m = 1; m < ncmaps; m++)
		if (cmaps[m].addr < cmaps[m - 1].addr + cmaps[m - 1].size)
			fprintf(stderr, "warning: mappings overlap at %#llx\n", (unsigned long long)cmaps[m].addr);
	fprintf(stderr, "%u mappings\n", ncmaps);

	/* every image header: imagesOffset/Count (or the old fields), imagesText, dyldInCacheMH */
	for (int i = 0; i < nfiles; i++) {
		const uint8_t *h = cf[i].map;
		uint32_t moff, ioff, icnt;
		uint64_t toff, tcnt, dyldmh;

		if (!h || cf[i].size < 0x200)
			continue;
		moff = rd32(h + 0x10);
		if (moff >= 0x1c8) {
			ioff = rd32(h + 0x1c0);
			icnt = rd32(h + 0x1c4);
		} else {
			ioff = rd32(h + 0x18);
			icnt = rd32(h + 0x1c);
		}
		toff = moff >= 0x98 ? rd64(h + 0x88) : 0;
		tcnt = moff >= 0x98 ? rd64(h + 0x90) : 0;
		dyldmh = moff >= 0x88 ? rd64(h + 0x78) : 0;
		if ((uint64_t)ioff + (uint64_t)icnt * 32 > cf[i].size)
			icnt = 0;
		if (toff + tcnt * 32 > cf[i].size)
			tcnt = 0;
		if (icnt || tcnt)
			fprintf(stderr, "%s: %u images, %llu image text infos, dyld in cache at %#llx\n",
			    cf[i].path, icnt, (unsigned long long)tcnt, (unsigned long long)dyldmh);
		if (nhaddr + icnt + tcnt + 1 > caphaddr) {
			caphaddr = (nhaddr + icnt + tcnt + 1) * 2;
			haddr = realloc(haddr, caphaddr * sizeof(*haddr));
			hpath = realloc(hpath, caphaddr * sizeof(*hpath));
		}
		for (uint32_t k = 0; k < icnt; k++) {
			const uint8_t *e = h + ioff + (uint64_t)k * 32;
			uint32_t po = rd32(e + 24);

			haddr[nhaddr] = rd64(e);
			hpath[nhaddr++] = po < cf[i].size ? (const char *)h + po : "?";
		}
		for (uint64_t k = 0; k < tcnt; k++) {
			const uint8_t *e = h + toff + k * 32;
			uint32_t po = rd32(e + 28);

			haddr[nhaddr] = rd64(e + 16);
			hpath[nhaddr++] = po < cf[i].size ? (const char *)h + po : "?";
		}
		if (dyldmh) {
			haddr[nhaddr] = dyldmh;
			hpath[nhaddr++] = "dyld (dyldInCacheMH)";
		}
	}

	/* dedupe header addresses (keep the first path seen) */
	{
		uint64_t *idx = malloc(nhaddr * 2 * sizeof(uint64_t));

		for (uint64_t k = 0; k < nhaddr; k++) {
			idx[2 * k] = haddr[k];
			idx[2 * k + 1] = k;
		}
		qsort(idx, nhaddr, 2 * sizeof(uint64_t), cmp_u64);
		images = calloc(nhaddr + ncmaps + 1, sizeof(*images));
		for (uint64_t k = 0; k < nhaddr; k++) {
			if (nimages && images[nimages - 1].addr == idx[2 * k])
				continue;
			images[nimages].addr = idx[2 * k];
			images[nimages].path = hpath[idx[2 * k + 1]];
			nimages++;
		}
		free(idx);
	}
	fprintf(stderr, "%u distinct image headers\n", nimages);

	/* code sections of every image */
	for (uint32_t k = 0; k < nimages; k++) {
		const uint8_t *mh = resolve(images[k].addr, 32, NULL);
		uint32_t ncmds, sizeofcmds;
		const uint8_t *lc;

		if (!mh || rd32(mh) != MH_MAGIC_64) {
			fprintf(stderr, "warning: no mach_header_64 at %#llx (%s)\n",
			    (unsigned long long)images[k].addr, images[k].path);
			continue;
		}
		ncmds = rd32(mh + 16);
		sizeofcmds = rd32(mh + 20);
		if (!resolve(images[k].addr, 32 + (uint64_t)sizeofcmds, NULL)) {
			fprintf(stderr, "warning: load commands of %s not mapped\n", images[k].path);
			continue;
		}
		lc = mh + 32;
		for (uint32_t c = 0; c < ncmds; c++) {
			uint32_t cmd = rd32(lc), cmdsize = rd32(lc + 4);

			if (cmdsize < 8 || lc + cmdsize > mh + 32 + sizeofcmds)
				break;
			if (cmd == LC_SEGMENT_64 && cmdsize >= 72) {
				uint32_t nsects = rd32(lc + 64);

				for (uint32_t s = 0; s < nsects && 72 + (s + 1) * 80 <= cmdsize; s++) {
					const uint8_t *sec = lc + 72 + s * 80;
					uint64_t addr = rd64(sec + 32), size = rd64(sec + 40);
					int code = is_code_section(sec, rd32(lc + 60));

					if (!code || size == 0)
						continue;
					if (code == 2)
						fprintf(stderr, "%.16s,%.16s of %s: flags 0 but named as code, rewritten\n",
						    sec + 16, sec, images[k].path);
					if (nranges == capranges) {
						capranges = capranges ? capranges * 2 : 4096;
						ranges = realloc(ranges, capranges * sizeof(*ranges));
					}
					ranges[nranges].addr = addr & ~3ull;
					ranges[nranges].size = (addr + size - (addr & ~3ull)) & ~3ull;
					ranges[nranges].image = k;
					nranges++;
				}
			}
			lc += cmdsize;
		}
	}
	/*
	 * TEXT_STUBS mappings: stub islands the cache builder emits for branches between subcaches
	 * (adrp/add/ldr/braa x16, x17). They belong to no image's load commands but are pure code.
	 */
	for (uint32_t m = 0; m < ncmaps; m++) {
		const char *bn;
		char *name;

		if (!(cmaps[m].flags & DYLD_CACHE_MAPPING_TEXT_STUBS) || !(cmaps[m].initprot & VM_PROT_EXECUTE))
			continue;
		bn = strrchr(cmaps[m].path, '/');
		bn = bn ? bn + 1 : cmaps[m].path;
		name = malloc(strlen(bn) + 32);
		sprintf(name, "stub islands (%s)", bn);
		images[nimages].addr = cmaps[m].addr;
		images[nimages].path = name;
		if (nranges == capranges) {
			capranges = capranges ? capranges * 2 : 4096;
			ranges = realloc(ranges, capranges * sizeof(*ranges));
		}
		ranges[nranges].addr = cmaps[m].addr;
		ranges[nranges].size = cmaps[m].size & ~3ull;
		ranges[nranges].image = nimages++;
		nranges++;
		stubmaps++;
	}
	fprintf(stderr, "%llu code sections + %u TEXT_STUBS mappings\n",
	    (unsigned long long)(nranges - stubmaps), stubmaps);
	sections = nranges - stubmaps;
	qsort(ranges, nranges, sizeof(*ranges), cmp_range);

	/* process each address once */
	for (uint64_t r = 0; r < nranges; r++) {
		uint64_t s = ranges[r].addr, e = ranges[r].addr + ranges[r].size;

		if (s < cursor) {
			shared_bytes += (e < cursor ? e : cursor) - s;
			s = cursor;
		}
		if (e <= s)
			continue;
		cursor = e;
		while (s < e) {
			const struct cmap *m = find_map(s);
			uint64_t chunk;
			uint8_t *p;

			if (!m) {
				unresolved += e - s;
				fprintf(stderr, "warning: %s: code %#llx..%#llx not in any mapping\n",
				    images[ranges[r].image].path, (unsigned long long)s, (unsigned long long)e);
				break;
			}
			chunk = (m->addr + m->size < e ? m->addr + m->size : e) - s;
			p = resolve(s, chunk, NULL);
			if (!p) {
				unresolved += chunk;
				fprintf(stderr, "warning: %s: code %#llx not backed by the file\n",
				    images[ranges[r].image].path, (unsigned long long)s);
			} else if (!(m->initprot & VM_PROT_EXECUTE)) {
				nonexec += chunk;
			} else {
				process((uint32_t *)p, chunk / 4, &images[ranges[r].image]);
				if (ndone && done[ndone - 1].addr + done[ndone - 1].size == s) {
					done[ndone - 1].size += chunk;
				} else {
					if (ndone == capdone) {
						capdone = capdone ? capdone * 2 : 4096;
						done = realloc(done, capdone * sizeof(*done));
					}
					done[ndone].addr = s;
					done[ndone].size = chunk;
					ndone++;
				}
			}
			s += chunk;
		}
	}
	if (nonexec || unresolved)
		fprintf(stderr, "code section bytes skipped: %llu in non-executable mappings, %llu unmapped\n",
		    (unsigned long long)nonexec, (unsigned long long)unresolved);

	/* executable mapping words outside every code section */
	{
		uint64_t d = 0;

		for (uint32_t m = 0; m < ncmaps; m++) {
			uint64_t a = cmaps[m].addr, end = cmaps[m].addr + cmaps[m].size;

			if (!(cmaps[m].initprot & VM_PROT_EXECUTE))
				continue;
			while (d < ndone && done[d].addr + done[d].size <= a)
				d++;
			while (a < end) {
				uint64_t gend = end;
				uint8_t *p;

				if (d < ndone && done[d].addr < end)
					gend = done[d].addr > a ? done[d].addr : a;
				if (gend > a && (p = resolve(a, gend - a, NULL)))
					process_gap((const uint32_t *)p, (gend - a) / 4, a);
				if (d < ndone && done[d].addr < end) {
					a = done[d].addr + done[d].size;
					d++;
				} else {
					a = end;
				}
			}
		}
	}

	report(census, 1);
	for (int i = 0; i < nfiles; i++) {
		if (!census && cf[i].size > 0x38 && memcmp(cf[i].map, "dyld_v1", 7) == 0)
			resign(cf[i].map, cf[i].size, rd64(cf[i].map + 0x28), rd64(cf[i].map + 0x30));
		unmap_file(cf[i].map, cf[i].size);
	}
	if (!census)
		printf("code signature: %llu CodeDirectories, %llu page hashes made again\n",
		    (unsigned long long)resigned_dirs, (unsigned long long)resigned_pages);
	return 0;
}

/* ---- standalone Mach-O ---- */

static int
do_macho(int census, const char *in, const char *out)
{
	uint8_t *f;
	uint64_t fsize, soff = 0, ssize = 0;
	uint32_t magic, ncmds, sizeofcmds;
	const uint8_t *lc;
	struct image im = { 0, in, { 0 }, 0 };

	dowrite = !census;
	if (!census)
		copy_file(in, out);
	f = map_file(census ? in : out, !census, &fsize);
	if (!f || fsize < 32) {
		fprintf(stderr, "%s: too small\n", in);
		return 1;
	}
	magic = be32(f);
	if (magic == FAT_MAGIC || magic == FAT_MAGIC_64) {
		uint32_t n = be32(f + 4), esz = magic == FAT_MAGIC ? 20 : 32;

		for (uint32_t a = 0; a < n && 8 + (uint64_t)(a + 1) * esz <= fsize; a++) {
			const uint8_t *e = f + 8 + a * esz;

			if (be32(e) == CPU_TYPE_ARM64 && (be32(e + 4) & ~CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E) {
				if (magic == FAT_MAGIC) {
					soff = be32(e + 8);
					ssize = be32(e + 12);
				} else {
					soff = (uint64_t)be32(e + 8) << 32 | be32(e + 12);
					ssize = (uint64_t)be32(e + 16) << 32 | be32(e + 20);
				}
				break;
			}
		}
		if (!ssize) {
			fprintf(stderr, "%s: fat file without an arm64e slice\n", in);
			return 1;
		}
	} else {
		ssize = fsize;
	}
	if (soff + ssize > fsize || ssize < 32 || rd32(f + soff) != MH_MAGIC_64 ||
	    rd32(f + soff + 4) != CPU_TYPE_ARM64 ||
	    (rd32(f + soff + 8) & ~CPU_SUBTYPE_MASK) != CPU_SUBTYPE_ARM64E) {
		fprintf(stderr, "%s: not an arm64e Mach-O (slice)\n", in);
		return 1;
	}
	printf("%s: arm64e slice at file offset %#llx, %llu bytes\n", in, (unsigned long long)soff,
	    (unsigned long long)ssize);
	ncmds = rd32(f + soff + 16);
	sizeofcmds = rd32(f + soff + 20);
	if (32 + (uint64_t)sizeofcmds > ssize)
		return 1;
	lc = f + soff + 32;
	uint64_t sigoff = 0, sigsize = 0;
	for (uint32_t c = 0; c < ncmds; c++) {
		uint32_t cmd = rd32(lc), cmdsize = rd32(lc + 4);

		if (cmdsize < 8 || lc + cmdsize > f + soff + 32 + sizeofcmds)
			break;
		if (cmd == 0x1d /* LC_CODE_SIGNATURE */ && cmdsize >= 16) {
			sigoff = rd32(lc + 8);
			sigsize = rd32(lc + 12);
		}
		if (cmd == LC_SEGMENT_64 && cmdsize >= 72) {
			uint32_t nsects = rd32(lc + 64);

			for (uint32_t s = 0; s < nsects && 72 + (s + 1) * 80 <= cmdsize; s++) {
				const uint8_t *sec = lc + 72 + s * 80;
				uint32_t type = rd32(sec + 64) & 0xff, off = rd32(sec + 48);
				uint64_t size = rd64(sec + 40);
				int code = is_code_section(sec, rd32(lc + 60));

				if (!code || size == 0 || type == 1 /* S_ZEROFILL */ || off == 0)
					continue;
				if (off + size > ssize || (off & 3)) {
					fprintf(stderr, "%.16s,%.16s: outside the slice, skipped\n", sec + 16, sec);
					continue;
				}
				printf("  %.16s,%.16s  offset %#x size %#llx%s\n", sec + 16, sec, off,
				    (unsigned long long)size, code == 2 ? " (flags 0, named as code)" : "");
				process((uint32_t *)(f + soff + off), size / 4, &im);
				sections++;
			}
		}
		lc += cmdsize;
	}
	if (!census && sigsize)
		resign(f + soff, ssize, sigoff, sigsize);
	images = &im;
	nimages = 1;
	report(census, 0);
	if (!census)
		printf("code signature: %llu CodeDirectories, %llu page hashes made again\n",
		    (unsigned long long)resigned_dirs, (unsigned long long)resigned_pages);
	unmap_file(f, fsize);
	nimages = 0;
	images = NULL;
	return 0;
}

static int
usage(void)
{
	fprintf(stderr,
	    "usage: pacstrip cache OUTDIR FILE...   rewrite a copy of a dyld shared cache\n"
	    "       pacstrip macho IN OUT           rewrite a copy of an arm64e Mach-O\n"
	    "       pacstrip census FILE...         count only (cache files or one Mach-O)\n"
	    "       pacstrip resign FILE...         page hashes made again, in place (cache files)\n"
	    "       pacstrip word HEX...            classify/rewrite instruction words\n");
	return 2;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 4 && strcmp(argv[1], "cache") == 0)
		return do_cache(0, argv[2], argc - 3, argv + 3);
	if (argc == 4 && strcmp(argv[1], "macho") == 0)
		return do_macho(0, argv[2], argv[3]);
	if (argc >= 3 && strcmp(argv[1], "census") == 0) {
		uint8_t hdr[8] = { 0 };
		int fd = open(argv[2], O_RDONLY);

		if (fd < 0 || read(fd, hdr, sizeof(hdr)) < 0)
			die("open", argv[2]);
		close(fd);
		if (memcmp(hdr, "dyld_v1", 7) == 0)
			return do_cache(1, NULL, argc - 2, argv + 2);
		return do_macho(1, argv[2], NULL);
	}
	/* after a later edit of a rewritten cache file (darwin/tools/fips26.py): its signature again */
	if (argc >= 3 && strcmp(argv[1], "resign") == 0) {
		for (int a = 2; a < argc; a++) {
			uint64_t size;
			uint8_t *m = map_file(argv[a], 1, &size);

			if (!m || size <= 0x38 || memcmp(m, "dyld_v1", 7) != 0) {
				fprintf(stderr, "%s: not a dyld shared cache file\n", argv[a]);
				return 1;
			}
			resign(m, size, rd64(m + 0x28), rd64(m + 0x30));
			unmap_file(m, size);
		}
		printf("code signature: %llu CodeDirectories, %llu page hashes made again\n",
		    (unsigned long long)resigned_dirs, (unsigned long long)resigned_pages);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "word") == 0) {
		for (int a = 2; a < argc; a++) {
			uint32_t x = (uint32_t)strtoul(argv[a], NULL, 16), out = x;
			int c = classify(x, &out);

			if (c == C_NONE)
				printf("%08x  -\n", x);
			else if (c < NREW)
				printf("%08x  %08x  %s\n", x, out, rew_name[c]);
			else if (c == C_HINT)
				printf("%08x  %08x  hint PAC (kept)\n", x, x);
			else
				printf("%08x  left  %s\n", x, left_class[c - LEFT0].name);
		}
		return 0;
	}
	return usage();
}
