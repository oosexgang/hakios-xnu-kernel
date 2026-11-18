/*
 * readercheck: the AIR reader alone (air_bitcode.c, air_module.c, air_layout.c, air_metallib.c and
 * Mesa's ralloc.c, no NIR), built with AddressSanitizer and UndefinedBehaviorSanitizer by
 * tools/air/readercheck.sh, against bitcode that apps hand to it: the reader runs in their
 * process, and in backboardd's, so a file it was not written for is a crash there.
 *
 *     readercheck GOOD.bc... [-- BASE.bc...]
 *
 * Every GOOD is a good module and must be read, its types laid out. Then the malformed inputs, each
 * of which must be refused: modules made here record by record (the comment above each says what
 * the reader did with it before it bounded what it reads: most crashed, hung or asked for gigabytes;
 * the rest break a rule of LLVM's that the reader now keeps); changes of a few bytes of a GOOD or a
 * BASE that crashed or hung it (one made against bytes that no file given has is skipped, and said
 * so); and .metallib containers. Two odd modules must be read, and laid out sanely. Nothing may
 * take more than five seconds, and the script runs this with every allocation of more than 256 MB
 * an error.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "air_module.h"
#include "nir_air.h"
#include "util/ralloc.h"

/* ralloc.c's printf functions take the length from util/u_printf.c, which brings much of Mesa's
 * util/ with it; the reader calls none of them, so this is all they need to link.
 */
size_t u_printf_length(const char *fmt, va_list args);
size_t u_printf_length(const char *fmt, va_list args) {
	va_list copy;
	va_copy(copy, args);
	int n = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	return n < 0 ? 0 : n;
}

/* -- a bitstream writer ---------------------------------------------------------------------- */

struct w {
	uint8_t *b;
	size_t bits, cap;
	unsigned width, depth;
	struct { unsigned width; size_t at; } open[8];
};

static void bit(struct w *w, unsigned v) {
	if (w->bits / 8 >= w->cap) {
		size_t cap = w->cap ? w->cap * 2 : 256;
		w->b = realloc(w->b, cap);
		memset(w->b + w->cap, 0, cap - w->cap);
		w->cap = cap;
	}
	if (v)
		w->b[w->bits / 8] |= 1u << (w->bits % 8);
	w->bits++;
}
static void fixed(struct w *w, uint64_t v, unsigned n) { for (unsigned i = 0; i < n; i++) bit(w, (v >> i) & 1); }
static void vbr(struct w *w, uint64_t v, unsigned n) {
	uint64_t hi = 1ull << (n - 1);
	do {
		uint64_t chunk = v & (hi - 1);
		v >>= n - 1;
		fixed(w, chunk | (v ? hi : 0), n);
	} while (v);
}
static void align32(struct w *w) { while (w->bits % 32) bit(w, 0); }
static void bytes(struct w *w, const void *p, size_t n) { for (size_t i = 0; i < n; i++) fixed(w, ((const uint8_t *)p)[i], 8); }
static void start(struct w *w) { memset(w, 0, sizeof(*w)); w->width = 2; bytes(w, "BC\xc0\xde", 4); }
static void enter(struct w *w, unsigned id, unsigned width) {
	fixed(w, 1, w->width); vbr(w, id, 8); vbr(w, width, 4); align32(w);
	w->open[w->depth].width = w->width; w->open[w->depth++].at = w->bits;
	fixed(w, 0, 32);
	w->width = width;
}
/* the open block's length in words, without writing its END_BLOCK */
static void close_raw(struct w *w) {
	size_t at = w->open[--w->depth].at, words = (w->bits - at - 32) / 32;
	for (unsigned i = 0; i < 32; i++)
		w->b[(at + i) / 8] = (w->b[(at + i) / 8] & ~(1u << ((at + i) % 8))) | ((words >> i) & 1) << ((at + i) % 8);
	w->width = w->open[w->depth].width;
}
static void end(struct w *w) { fixed(w, 0, w->width); align32(w); close_raw(w); }
static void rec_ops(struct w *w, unsigned code, const uint64_t *ops, unsigned n) {
	fixed(w, 3, w->width); vbr(w, code, 6); vbr(w, n, 6);
	for (unsigned i = 0; i < n; i++)
		vbr(w, ops[i], 6);
}
#define REC(w, code, ...) rec_ops(w, code, (const uint64_t[]){ __VA_ARGS__ }, sizeof((const uint64_t[]){ __VA_ARGS__ }) / 8)
static void rec0(struct w *w, unsigned code) { rec_ops(w, code, NULL, 0); }
static void rec_str(struct w *w, unsigned code, const char *s) {
	uint64_t ops[64];
	unsigned n = strlen(s);
	for (unsigned i = 0; i < n; i++)
		ops[i] = (uint8_t)s[i];
	rec_ops(w, code, ops, n);
}
/* an abbreviation: "l5" literal 5, "f8" fixed 8, "v6" vbr 6, "a" array, "c" char6, "b" blob */
static void abbrev(struct w *w, const char *const *ops, unsigned n) {
	fixed(w, 2, w->width); vbr(w, n, 5);
	for (unsigned i = 0; i < n; i++) {
		char k = ops[i][0];
		if (k == 'l') { fixed(w, 1, 1); vbr(w, strtoull(ops[i] + 1, NULL, 10), 8); continue; }
		fixed(w, 0, 1);
		fixed(w, k == 'f' ? 1 : k == 'v' ? 2 : k == 'a' ? 3 : k == 'c' ? 4 : 5, 3);
		if (k == 'f' || k == 'v')
			vbr(w, strtoull(ops[i] + 1, NULL, 10), 5);
	}
}
#define ABBREV(w, ...) abbrev(w, (const char *const[]){ __VA_ARGS__ }, sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *))

/* the string table: one blob holding s */
static void strtab(struct w *w, const char *s) {
	enter(w, 23, 3);
	ABBREV(w, "l1", "b");
	fixed(w, 4, 3); vbr(w, strlen(s), 6); align32(w);
	bytes(w, s, strlen(s)); align32(w);
	end(w);
}

/* A module of version 2: types void and void(), one function of them with a body that body
 * writes, named "main".
 */
static void module_with_fn(struct w *w, void (*body)(struct w *), void (*after)(struct w *)) {
	start(w);
	enter(w, 8, 3);
	REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 2); rec0(w, 2); REC(w, 21, 0, 0); end(w);
	REC(w, 8, 0, 4, 1, 0, 0, 0, 0, 0, 0);
	enter(w, 12, 4); body(w); end(w);
	if (after)
		after(w);
	end(w);
	strtab(w, "main");
}

/* -- the malformed modules --------------------------------------------------------------------- */

/* METADATA_KIND 0xFFFFFFFF: id + 1 wrapped to 0, the table shrank and was written 32 GB past */
static void mdkind(struct w *w) { start(w); enter(w, 8, 3); enter(w, 22, 3); REC(w, 6, 0xFFFFFFFF, 'a'); end(w); end(w); }
/* the same in a METADATA block */
static void mdkind15(struct w *w) { start(w); enter(w, 8, 3); enter(w, 15, 3); REC(w, 6, 0xFFFFFFFF, 'a'); end(w); end(w); }

/* a forward operand of id 0xFFFFFFFF (relative id num_values + 1): r->fwd[0xFFFFFFFF] written */
static void b_fwdref(struct w *w) { REC(w, 1, 1); REC(w, 2, 2, 0, 1, 0); rec0(w, 10); }
static void fwdref(struct w *w) { module_with_fn(w, b_fwdref, NULL); }

/* a phi of no operands: (num_ops - 1) / 2 incoming values, 2^31 of them */
static void b_phi0(struct w *w) { REC(w, 1, 1); rec0(w, 16); rec0(w, 10); }
static void phi0(struct w *w) { module_with_fn(w, b_phi0, NULL); }

/* DECLAREBLOCKS 0xFFFFFFFF: an allocation of 128 GB, written through when it failed */
static void b_blocks(struct w *w) { REC(w, 1, 0xFFFFFFFF); rec0(w, 10); }
static void blocks(struct w *w) { module_with_fn(w, b_blocks, NULL); }

/* NUMENTRY 0xFFFFFFFF: the same for the type table */
static void numentry(struct w *w) { start(w); enter(w, 8, 3); REC(w, 1, 2); enter(w, 17, 4); REC(w, 1, 0xFFFFFFFF); end(w); end(w); }

/* an array of literals, 2^32 of them: no bits read for any, and the operand buffer's capacity
 * doubled until it wrapped to 64 */
static void litarray(struct w *w) {
	start(w); enter(w, 8, 3); enter(w, 22, 4);
	ABBREV(w, "l6", "a", "l0");
	fixed(w, 4, 4); vbr(w, 0xFFFFFFFF, 6);
	end(w); end(w);
}

/* DEFINE_ABBREV of 0xFFFFFFFF operands: an allocation of 64 GB */
static void abbrevops(struct w *w) { start(w); enter(w, 8, 3); enter(w, 22, 4); fixed(w, 2, 4); vbr(w, 0xFFFFFFFF, 5); end(w); end(w); }

/* a blob of 2^61 bytes: len * 8 wrapped to 0, and the strings were read past the stream */
static void blob(struct w *w) {
	start(w); enter(w, 8, 3); enter(w, 15, 4);
	ABBREV(w, "l35", "v6", "v6", "b");
	fixed(w, 4, 4); vbr(w, 1, 6); vbr(w, 2, 6); vbr(w, 1ull << 61, 6); align32(w);
	bytes(w, "\xf0\xf7\xff\xff", 4); /* lengths: 1008; low nibble 0, the END_BLOCK of METADATA */
	close_raw(w);
	bytes(w, "\xf8\xff\xff\xff", 4); /* the END_BLOCK of the module, and bits that are not 0 after it */
	close_raw(w);
}

/* METADATA_STRINGS whose second length is 2^64 - 1: off + len wrapped, the string 4 GB long */
static void mdlen(struct w *w) {
	struct w l;
	memset(&l, 0, sizeof(l));
	vbr(&l, 1, 6); vbr(&l, ~0ull, 6);
	size_t lb = (l.bits + 7) / 8, n = lb + 4;
	n += (4 - n % 4) % 4;
	start(w); enter(w, 8, 3); enter(w, 15, 4);
	ABBREV(w, "l35", "v6", "v6", "b");
	fixed(w, 4, 4); vbr(w, 2, 6); vbr(w, lb, 6); vbr(w, n, 6); align32(w);
	bytes(w, l.b, lb);
	for (size_t i = lb; i < n; i++)
		fixed(w, 'A', 8);
	bytes(w, "\xf0\xff\xff\xff", 4);
	close_raw(w);
	bytes(w, "\xf8\xff\xff\xff", 4);
	close_raw(w);
	free(l.b);
}

/* a global named at offset 2^63, size 2^63: off + size wrapped past the string table's check */
static void strtab_wrap(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 2); REC(w, 7, 32); REC(w, 8, 0, 0); end(w);
	REC(w, 7, 1ull << 63, 1ull << 63, 1, 0, 0, 0, 0, 0);
	end(w);
	strtab(w, "name");
}

/* NAMED_NODE in a function's metadata: never resolved, its operand number used as a pointer */
static void b_namedmd(struct w *w) {
	REC(w, 1, 1);
	enter(w, 15, 4); rec_str(w, 4, "air.kernel"); REC(w, 10, 0x41414141414140); end(w);
	rec0(w, 10);
}
static void namedmd(struct w *w) { module_with_fn(w, b_namedmd, NULL); }

/* the same after the module's first function: resolved only when the first body began */
static void b_ret(struct w *w) { REC(w, 1, 1); rec0(w, 10); }
static void a_namedmd(struct w *w) { enter(w, 15, 4); rec_str(w, 4, "air.kernel"); REC(w, 10, 0x41414140); end(w); }
static void namedmd_late(struct w *w) { module_with_fn(w, b_ret, a_namedmd); }

/* [1 x itself] as a global's type: the layout functions recursed until the stack ran out */
static void rectype(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 1); REC(w, 11, 1, 0); end(w);
	REC(w, 7, 0, 1, 0, 2, 0, 0, 0, 0); /* flags bit 1: the type is the value's own */
	end(w);
	strtab(w, "g");
}

/* a named struct holding itself through an array: %s = { [1 x %s] } */
static void recstruct(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 2); REC(w, 11, 1, 1); rec_str(w, 19, "s"); REC(w, 20, 0, 0); end(w);
	REC(w, 7, 0, 1, 1, 2, 0, 0, 0, 0);
	end(w);
	strtab(w, "g");
}

/* an array of a type defined after it that is no named struct (LLVM refuses it as well) */
static void fwdarray(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 2); REC(w, 11, 1, 1); REC(w, 7, 32); end(w);
	end(w);
}

/* 300000 arrays each of the one before: the layout's recursion as deep, past the stack */
static void deeptype(struct w *w) {
	unsigned n = 300000;
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, n + 1); REC(w, 7, 32);
	for (unsigned i = 0; i < n; i++)
		REC(w, 11, 1, i);
	end(w);
	REC(w, 7, 0, 1, n, 2, 0, 0, 0, 0);
	end(w);
	strtab(w, "g");
}

/* 50000 named structs each holding the next, ahead of its definition as a named struct may be */
static void deepstruct(struct w *w) {
	unsigned n = 50000;
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, n + 1);
	for (unsigned i = 0; i < n; i++)
		REC(w, 20, 0, i + 1);
	REC(w, 20, 0);
	end(w);
	REC(w, 7, 0, 1, 0, 2, 0, 0, 0, 0);
	end(w);
	strtab(w, "g");
}

/* a vector of vectors, which air_type_bit_size() walks as deep as it goes */
static void vecvec(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 3); REC(w, 7, 32); REC(w, 12, 2, 0); REC(w, 12, 2, 1); end(w);
	end(w);
}

/* a record at the top level, after the module and before its string table: the read stopped there
 * with no error and returned the module, its functions unnamed and its types unchecked */
static void toprecord(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 2); rec0(w, 2); REC(w, 21, 0, 0); end(w);
	REC(w, 8, 0, 4, 1, 0, 1, 0, 0, 0, 0);
	end(w);
	REC(w, 1, 0);
	align32(w);
	strtab(w, "main");
}

/* "f32:1": an alignment of 1 bit is 0 bytes, and ALIGN_NPOT divided by it */
static void align0(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2); rec_str(w, 3, "f32:1");
	enter(w, 17, 4); REC(w, 1, 2); rec0(w, 3); REC(w, 18, 0, 0); end(w);
	REC(w, 7, 0, 1, 1, 2, 0, 0, 0, 0);
	end(w);
	strtab(w, "g");
}

/* s_k = { s_k-1, s_k-1 } 48 deep: laid out again at every level, 2^48 steps */
static void exptype(struct w *w) {
	unsigned n = 48;
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, n + 1); REC(w, 7, 32);
	for (unsigned i = 0; i < n; i++)
		REC(w, 18, 0, i, i);
	end(w);
	REC(w, 7, 0, 1, n, 2, 0, 0, 0, 0);
	end(w);
	strtab(w, "g");
}

/* BLOCKINFO: a record that counts 2^32 - 1 operands at the end of the stream, each read failing
 * at once and the loop going on to the last */
static void blockinfo_count(struct w *w) {
	start(w); enter(w, 0, 2);
	fixed(w, 3, 2); vbr(w, 1, 6); vbr(w, 0xFFFFFFFF, 6);
	align32(w); close_raw(w);
}

/* BLOCKINFO for 65 block ids: each block entered looked through them all */
static void blockinfo_ids(struct w *w) {
	start(w); enter(w, 0, 2);
	for (unsigned i = 0; i < 65; i++)
		REC(w, 1, 100 + i);
	end(w);
	enter(w, 8, 3); end(w);
}

/* a record that counts 2^32 - 1 operands */
static void opcount(struct w *w) { start(w); enter(w, 8, 3); fixed(w, 3, 3); vbr(w, 1, 6); vbr(w, 0xFFFFFFFF, 6); end(w); }

/* a global aligned to 2^39: 1u << 39 */
static void bigalign(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 2); REC(w, 7, 32); REC(w, 8, 0, 0); end(w);
	REC(w, 7, 0, 1, 1, 0, 0, 0, 40, 0);
	end(w);
	strtab(w, "g");
}

/* a function aligned to 2^39 */
static void fnalign(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 2); rec0(w, 2); REC(w, 21, 0, 0); end(w);
	REC(w, 8, 0, 4, 1, 0, 1, 0, 0, 40, 0);
	end(w);
	strtab(w, "main");
}

/* CE_GEP_WITH_INRANGE_INDEX of no operands: num_ops - 2 wrapped, 2^31 operands */
static void cegep(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 1); REC(w, 7, 32); end(w);
	enter(w, 11, 4); REC(w, 1, 0); rec0(w, 24); end(w);
	end(w);
}

/* an integer of 0 bits (LLVM allows 1 to 2^23) */
static void int0(struct w *w) { start(w); enter(w, 8, 3); REC(w, 1, 2); enter(w, 17, 4); REC(w, 1, 1); REC(w, 7, 0); end(w); end(w); }

/* a second type table, which would redefine the types values already have */
static void twotables(struct w *w) {
	start(w); enter(w, 8, 3); REC(w, 1, 2);
	enter(w, 17, 4); REC(w, 1, 1); REC(w, 7, 32); end(w);
	enter(w, 17, 4); REC(w, 7, 8); end(w);
	end(w);
}

static const struct {
	const char *name;
	void (*make)(struct w *);
} bad_modules[] = {
	{ "METADATA_KIND 0xFFFFFFFF", mdkind },
	{ "METADATA_KIND 0xFFFFFFFF in a METADATA block", mdkind15 },
	{ "forward operand 0xFFFFFFFF", fwdref },
	{ "phi of no operands", phi0 },
	{ "DECLAREBLOCKS 0xFFFFFFFF", blocks },
	{ "NUMENTRY 0xFFFFFFFF", numentry },
	{ "array of 2^32 literals", litarray },
	{ "abbreviation of 2^32 operands", abbrevops },
	{ "blob of 2^61 bytes", blob },
	{ "metadata string of 2^64 - 1 bytes", mdlen },
	{ "string table offset 2^63", strtab_wrap },
	{ "named metadata in a function", namedmd },
	{ "named metadata after a function", namedmd_late },
	{ "array of itself", rectype },
	{ "named struct holding itself", recstruct },
	{ "array of a later integer", fwdarray },
	{ "types nested 300000 deep", deeptype },
	{ "named structs nested 50000 deep", deepstruct },
	{ "vector of vectors", vecvec },
	{ "record at the top level", toprecord },
	{ "BLOCKINFO record of 2^32 operands", blockinfo_count },
	{ "BLOCKINFO for 65 block ids", blockinfo_ids },
	{ "record of 2^32 operands", opcount },
	{ "global aligned to 2^39", bigalign },
	{ "function aligned to 2^39", fnalign },
	{ "CE_GEP_WITH_INRANGE_INDEX of no operands", cegep },
	{ "integer of 0 bits", int0 },
	{ "two type tables", twotables },
};

/* read, but laid out as they must be */
static const struct {
	const char *name;
	void (*make)(struct w *);
} odd_modules[] = {
	{ "datalayout f32:1", align0 },
	{ "struct of two of itself, 48 deep", exptype },
};

/* -- reading ----------------------------------------------------------------------------------- */

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int failures;

static void failed(const char *name, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	printf("FAIL     %s: ", name);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	failures++;
}

/* reads a module and touches all of it that a translation would; true when it was read */
static bool read_module(const char *name, const void *data, size_t size, const char **why) {
	void *ctx = ralloc_context(NULL);
	double t0 = now();
	struct air_module *m = air_module_read(ctx, data, size, why);
	if (m) {
		for (unsigned i = 0; i < m->num_types; i++) {
			if (!air_type_abi_align(m, &m->types[i]))
				failed(name, "a type of alignment 0");
			(void)air_type_alloc_size(m, &m->types[i]);
		}
		for (unsigned i = 0; i < m->num_globals; i++)
			(void)air_type_alloc_size(m, m->globals[i]->value_type);
		for (unsigned i = 0; i < m->num_md; i++)
			(void)air_md_string(m->md[i]);
		for (unsigned i = 0; i < m->num_named_md; i++)
			for (unsigned k = 0; k < m->named_md[i].num_ops; k++)
				if (m->named_md[i].ops[k] && m->named_md[i].ops[k]->kind > AIR_MD_OTHER)
					failed(name, "named metadata whose operand is no node");
	}
	if (now() - t0 > 5)
		failed(name, "took %.1f seconds", now() - t0);
	/* (the reason outlives the module: it is a copy under ctx, so it is copied again) */
	static char reason[256];
	snprintf(reason, sizeof(reason), "%s", *why ? *why : "");
	*why = reason;
	ralloc_free(ctx);
	return m != NULL;
}

static void must_refuse(const char *name, const void *data, size_t size) {
	const char *why;
	if (read_module(name, data, size, &why))
		failed(name, "read, and it should not have been");
	else
		printf("refused  %-46s %s\n", name, why);
}

/* -- byte changes of good modules ---------------------------------------------------------------- */

static uint32_t crc32(const uint8_t *p, size_t n) {
	uint32_t c = ~0u;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = c >> 1 ^ (0xEDB88320u & -(c & 1));
	}
	return ~c;
}

/* Changes a fuzzer made to the good modules (the one readercheck.sh describes): the module, what
 * the reader did with the change before it had its bounds, the module's size and CRC-32, and the
 * bytes changed. The ones of "(llvm-as 18)" were made against plugin/tools/kernels/NAME.ll through
 * LLVM 18's llvm-as, the others against the fixtures. */
static const struct {
	const char *of, *was;
	size_t size;
	uint32_t crc;
	unsigned num;
	uint32_t at[32];
	uint8_t byte[32];
} mutations[] = {
	{ "clz_id (llvm-as 18)", "an operand id far ahead: written gigabytes past a table", 2336, 0x30379076, 4,
	  { 2053, 2054, 2055, 2056 },
	  { 4, 74, 72, 95 } },
	{ "tex_col16 (llvm-as 18)", "an operand id far ahead: written gigabytes past a table", 3664, 0xfa38f36f, 9,
	  { 2235, 2236, 2237, 2238, 2402, 2403, 2404, 2405, 3102 },
	  { 187, 56, 195, 42, 255, 255, 255, 255, 131 } },
	{ "bytes_id (llvm-as 18)", "an operand id far ahead: written gigabytes past a table", 2432, 0x1005dd20, 1,
	  { 2085 },
	  { 30 } },
	{ "tex_copy (llvm-as 18)", "METADATA_KIND id in the billions: a table of gigabytes", 2432, 0x2e0c26ac, 11,
	  { 1371, 1372, 1373, 1374, 1427, 1703, 1901, 1941, 1942, 1943, 1944 },
	  { 255, 255, 255, 255, 56, 134, 210, 142, 20, 61, 126 } },
	{ "fill_id (llvm-as 18)", "METADATA_KIND id in the billions: a table of gigabytes", 2352, 0xa57d23bd, 5,
	  { 330, 669, 670, 671, 672 },
	  { 162, 255, 255, 255, 255 } },
	{ "null_fs (llvm-as 18)", "METADATA_KIND id in the billions: a table of gigabytes", 2016, 0x4cd75f5c, 10,
	  { 290, 501, 977, 978, 979, 980, 1418, 1472, 1791, 1849 },
	  { 255, 161, 255, 255, 255, 255, 32, 255, 182, 129 } },
	{ "dual_fs (llvm-as 18)", "METADATA_KIND 0xFFFFFFFF: written past its table", 2256, 0x1323c350, 9,
	  { 658, 659, 660, 661, 746, 747, 748, 749, 842 },
	  { 255, 255, 255, 255, 45, 34, 128, 18, 145 } },
	{ "pack_id (llvm-as 18)", "METADATA_KIND 0xFFFFFFFF: written past its table", 2480, 0xf7745bcb, 7,
	  { 970, 971, 972, 973, 1503, 1792, 1864 },
	  { 255, 255, 255, 255, 255, 0, 255 } },
	{ "tex_col16 (llvm-as 18)", "METADATA_KIND 0xFFFFFFFF: written past its table", 3664, 0xfa38f36f, 5,
	  { 698, 699, 700, 701, 1300 },
	  { 255, 255, 255, 255, 110 } },
	{ "undef_phi_id (llvm-as 18)", "DEFINE_ABBREV count in the billions: written through a failed allocation", 2400, 0x6c349748, 9,
	  { 107, 108, 109, 110, 762, 763, 764, 765, 965 },
	  { 255, 255, 255, 255, 140, 56, 26, 88, 34 } },
	{ "tri_attr_vs (llvm-as 18)", "DEFINE_ABBREV count in the billions: written through a failed allocation", 2320, 0xc160d256, 12,
	  { 133, 134, 135, 136, 240, 476, 1507, 1541, 1723, 1724, 1725, 1726 },
	  { 255, 255, 255, 255, 255, 119, 50, 0, 255, 255, 255, 255 } },
	{ "tri_color_fs (llvm-as 18)", "DEFINE_ABBREV count in the billions: written through a failed allocation", 2192, 0x6fe88345, 14,
	  { 133, 134, 135, 136, 170, 348, 1131, 1217, 1357, 1468, 1684, 1685, 1686, 1687 },
	  { 255, 255, 255, 255, 186, 3, 255, 85, 118, 144, 255, 255, 255, 255 } },
	{ "clz_id (llvm-as 18)", "not done in 25 seconds: billions of failing reads", 2336, 0x30379076, 11,
	  { 110, 111, 112, 113, 161, 467, 768, 769, 770, 771, 1169 },
	  { 255, 255, 255, 255, 255, 105, 255, 255, 255, 255, 231 } },
	{ "tex_fill_h (llvm-as 18)", "not done in 25 seconds: billions of failing reads", 2416, 0xdd6b517f, 12,
	  { 110, 111, 112, 113, 391, 467, 744, 1624, 1853, 1854, 1855, 1856 },
	  { 255, 255, 255, 255, 119, 255, 140, 255, 255, 255, 255, 255 } },
	{ "argbuf_res (llvm-as 18)", "not done in 25 seconds: billions of failing reads", 3152, 0xe9be4737, 14,
	  { 110, 111, 112, 113, 467, 586, 740, 1430, 1431, 1432, 1433, 2204, 2699, 2882 },
	  { 255, 255, 255, 255, 255, 71, 111, 255, 255, 255, 255, 255, 210, 97 } },
	{ "tri_tex_fs (llvm-as 18)", "CE_GEP_WITH_INRANGE_INDEX of no operands: written through a failed allocation", 2480, 0x52fc65e5, 1,
	  { 2125 },
	  { 131 } },
	{ "tri_dpos_vs", "an operand id far ahead: written gigabytes past a table", 2560, 0x2eb84776, 3,
	  { 1413, 1799, 2280 },
	  { 247, 0, 255 } },
	{ "fill_id", "an operand id far ahead: written gigabytes past a table", 2560, 0x83ccbfc5, 4,
	  { 2288, 2289, 2290, 2291 },
	  { 42, 86, 171, 112 } },
	{ "tri_shadow_fs", "an operand id far ahead: written gigabytes past a table", 2848, 0x04d3a3ea, 5,
	  { 178, 515, 902, 2194, 2497 },
	  { 152, 0, 0, 146, 246 } },
	{ "tri_mrt_fs", "DEFINE_ABBREV count in the billions: written through a failed allocation", 2384, 0x584d1446, 13,
	  { 46, 118, 119, 120, 121, 208, 209, 210, 211, 328, 1365, 2134, 2335 },
	  { 144, 255, 255, 255, 255, 191, 75, 19, 79, 80, 66, 0, 0 } },
	{ "tri_dbuf_fs", "DEFINE_ABBREV count in the billions: written through a failed allocation", 2464, 0x33484003, 17,
	  { 118, 119, 120, 121, 198, 517, 780, 781, 782, 783, 1751, 1922, 2172, 2173, 2174, 2175, 2334 },
	  { 255, 255, 255, 255, 69, 86, 228, 250, 201, 10, 54, 0, 255, 255, 255, 255, 255 } },
	{ "tri_flat_fs", "DEFINE_ABBREV count in the billions: written through a failed allocation", 2336, 0x4f55dfd4, 5,
	  { 111, 112, 113, 114, 1929 },
	  { 255, 255, 255, 255, 159 } },
	{ "ballot_id", "function alignment past 2^31: an undefined shift", 2624, 0xfcb551eb, 6,
	  { 645, 646, 647, 648, 1029, 1340 },
	  { 135, 70, 229, 93, 53, 168 } },
	{ "clz_id", "function alignment past 2^31: an undefined shift", 2544, 0xdc743b6c, 10,
	  { 380, 660, 1015, 1016, 1017, 1018, 1312, 1526, 2050, 2066 },
	  { 112, 255, 255, 255, 255, 255, 0, 61, 46, 255 } },
	{ "tri_mrt_fs", "function alignment past 2^31: an undefined shift", 2384, 0x584d1446, 6,
	  { 641, 642, 643, 644, 1662, 2353 },
	  { 29, 240, 191, 67, 117, 57 } },
	{ "tri_color_vs", "METADATA_KIND id in the billions: a table of gigabytes", 2672, 0x89357587, 7,
	  { 1054, 1326, 1327, 1328, 1329, 1428, 2670 },
	  { 23, 255, 255, 255, 255, 151, 164 } },
	{ "tri_upos_vs", "METADATA_KIND id in the billions: a table of gigabytes", 2560, 0x6eaabd7f, 4,
	  { 1542, 1543, 1544, 1545 },
	  { 255, 255, 255, 255 } },
	{ "tri_fc_fs", "METADATA_KIND id in the billions: a table of gigabytes", 2880, 0x23c34f81, 9,
	  { 1046, 1047, 1048, 1049, 1453, 1454, 1455, 1456, 2861 },
	  { 255, 255, 255, 255, 255, 255, 255, 255, 125 } },
	{ "tri_ctex_fs", "METADATA_KIND 0xFFFFFFFF: written past its table", 2816, 0xb0d53bc8, 5,
	  { 603, 1297, 1298, 1299, 1300 },
	  { 199, 255, 255, 255, 255 } },
	{ "tex_col16", "METADATA_KIND 0xFFFFFFFF: written past its table", 3888, 0xd065abab, 6,
	  { 1245, 1246, 1247, 1248, 2937, 3173 },
	  { 255, 255, 255, 255, 219, 223 } },
	{ "tri_flat_vs", "METADATA_KIND 0xFFFFFFFF: written past its table", 2512, 0xef2480c0, 8,
	  { 1304, 1305, 1306, 1307, 1530, 1531, 1532, 1533 },
	  { 255, 255, 255, 255, 220, 173, 115, 81 } },
	{ "clz_id", "killed for want of memory", 2544, 0xdc743b6c, 10,
	  { 671, 672, 673, 674, 1103, 1104, 1105, 1106, 1255, 1897 },
	  { 255, 255, 255, 255, 33, 181, 219, 112, 245, 255 } },
	{ "tri_color_vs", "not done in 25 seconds: billions of failing reads", 2672, 0x89357587, 20,
	  { 110, 111, 112, 113, 268, 765, 766, 767, 768, 1117, 1348, 1349, 1350, 1351, 1394, 1395, 1396, 1397, 1478, 1802 },
	  { 255, 255, 255, 255, 110, 255, 255, 255, 255, 0, 255, 255, 255, 255, 171, 213, 171, 20, 120, 0 } },
	{ "tri_dpos_vs", "not done in 25 seconds: billions of failing reads", 2560, 0x2eb84776, 16,
	  { 110, 111, 112, 113, 366, 383, 384, 385, 386, 752, 982, 1418, 2273, 2274, 2275, 2276 },
	  { 255, 255, 255, 255, 0, 119, 52, 85, 57, 64, 145, 148, 178, 81, 249, 17 } },
	{ "undef_phi_id", "not done in 25 seconds: billions of failing reads", 2608, 0xd8dda178, 2,
	  { 201, 2277 },
	  { 162, 0 } },
	{ "tri_fc_fs", "global alignment past 2^31: an undefined shift", 2880, 0x23c34f81, 6,
	  { 716, 958, 1030, 1159, 1560, 1745 },
	  { 118, 212, 245, 255, 59, 2 } },
};

/* -- .metallib containers ------------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> 8 * i; }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> 8 * i; }
static void putbe32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (24 - 8 * i); }

/* an MTLB of one function, "f", whose module is the n bytes of bc; its size in *size */
static uint8_t *mtlb(const uint8_t *bc, size_t n, size_t *size) {
	uint8_t entry[4 + 8 + 6 + 1 + 6 + 24 + 6 + 8 + 4], *e = entry + 4;
	memcpy(e, "NAME", 4); e[4] = 2; e[5] = 0; memcpy(e + 6, "f", 2); e += 8;
	memcpy(e, "TYPE", 4); e[4] = 1; e[5] = 0; e[6] = 2; e += 7;
	memcpy(e, "OFFT", 4); e[4] = 24; e[5] = 0; memset(e + 6, 0, 24); e += 30;
	memcpy(e, "MDSZ", 4); e[4] = 8; e[5] = 0; put64(e + 6, n); e += 14;
	memcpy(e, "ENDT", 4); e += 4;
	put32(entry, e - entry);
	size_t list = 0x58, mods = list + 4 + sizeof(entry);
	*size = mods + n;
	uint8_t *b = calloc(1, *size);
	memcpy(b, "MTLB", 4);
	put64(b + 0x10, *size); put64(b + 0x18, list); put64(b + 0x48, mods); put64(b + 0x50, n);
	put32(b + list, 1);
	memcpy(b + list + 4, entry, sizeof(entry));
	memcpy(b + mods, bc, n);
	return b;
}

static void metallib(const char *name, const uint8_t *data, size_t size, const uint8_t *want, size_t want_size) {
	void *ctx = ralloc_context(NULL);
	const char *why;
	struct air_metallib lib;
	bool ok = air_metallib_parse(ctx, data, size, &lib, &why);
	if (!want && ok)
		failed(name, "read, and it should not have been");
	else if (!want)
		printf("refused  %-46s %s\n", name, why);
	else if (!ok)
		failed(name, "refused: %s", why);
	else if (lib.num_functions != 1 || strcmp(lib.functions[0].name, "f") || lib.functions[0].bitcode_size != want_size ||
		 memcmp(lib.functions[0].bitcode, want, want_size))
		failed(name, "read, but not as it was written");
	else if (!read_module(name, lib.functions[0].bitcode, lib.functions[0].bitcode_size, &why))
		failed(name, "its module refused: %s", why);
	else
		printf("read     %s\n", name);
	ralloc_free(ctx);
}

static void metallibs(const uint8_t *good, size_t good_size) {
	size_t n;
	uint8_t *b = mtlb(good, good_size, &n);
	metallib("metallib of one function", b, n, good, good_size);

	/* a fat file whose AIR slice is the whole file: it recursed until the stack ran out */
	uint8_t fat[28];
	putbe32(fat, 0xcafebabe); putbe32(fat + 4, 1);
	putbe32(fat + 8, 0x01000017); putbe32(fat + 12, 0); putbe32(fat + 16, 0); putbe32(fat + 20, 28); putbe32(fat + 24, 0);
	metallib("fat file of itself", fat, sizeof(fat), NULL, 0);

#ifdef READERCHECK_FAT
	/* a fat file of Apple's GPU code and the AIR slice, which is the library */
	size_t fn = 8 + 40 + 16 + n;
	uint8_t *f = calloc(1, fn);
	putbe32(f, 0xcafebabe); putbe32(f + 4, 2);
	putbe32(f + 8, 0x01000013); putbe32(f + 16, 48); putbe32(f + 20, 16);
	putbe32(f + 28, 0x01000017); putbe32(f + 36, 64); putbe32(f + 40, n);
	memcpy(f + 64, b, n);
	metallib("fat file of an AIR slice", f, fn, good, good_size);
	free(f);
#endif

	/* the function list at 2^64 - 2: funcs + 4 wrapped past the check */
	uint8_t h[0x58 + 4 + 64 + 16] = { 'M', 'T', 'L', 'B' };
	put64(h + 0x10, 0x58); put64(h + 0x18, ~0ull - 1);
	metallib("function list at 2^64 - 2", h, 0x58, NULL, 0);

	/* a module at OFFT 2^63, of MDSZ 2^63: the sum wrapped inside the section */
	uint8_t body[56] = "NAME\2\0f";
	memcpy(body + 8, "OFFT\x18", 6); put64(body + 30, 1ull << 63);
	memcpy(body + 38, "MDSZ\x08", 6); put64(body + 44, 1ull << 63);
	memcpy(body + 52, "ENDT", 4);
	size_t blen = sizeof(body);
	memset(h, 0, sizeof(h)); memcpy(h, "MTLB", 4);
	put64(h + 0x18, 0x58); put32(h + 0x58, 1); put32(h + 0x5c, 4 + blen); memcpy(h + 0x60, body, blen);
	put64(h + 0x48, 0x60 + blen); put64(h + 0x50, 16);
	metallib("module offset 2^63", h, 0x60 + blen + 16, NULL, 0);

	/* the same with a count of 0xFFFFFFFF functions: an array of 128 GB */
	put32(h + 0x58, 0xFFFFFFFF);
	metallib("0xFFFFFFFF functions", h, 0x60 + blen + 16, NULL, 0);

	/* an entry whose length runs past the file */
	put32(h + 0x58, 1); put32(h + 0x5c, 0xFFFFFFF0);
	metallib("function entry of 4 GB", h, 0x60 + blen + 16, NULL, 0);
	free(b);
}

int main(int argc, char **argv) {
	struct { const char *path; uint8_t *data; size_t size; uint32_t crc; } *good = calloc(argc, sizeof(*good));
	int num_good = 0, num_read = 0;
	bool bases = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			bases = true;
			continue;
		}
		FILE *f = fopen(argv[i], "rb");
		if (!f) { perror(argv[i]); return 2; }
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		uint8_t *b = malloc(n ? n : 1);
		if (fread(b, 1, n, f) != (size_t)n) { perror(argv[i]); return 2; }
		fclose(f);
		const char *why;
		if (!bases && !read_module(argv[i], b, n, &why)) {
			failed(argv[i], "refused: %s", why);
			free(b);
			continue;
		}
		num_read += !bases;
		good[num_good].path = argv[i];
		good[num_good].data = b;
		good[num_good].size = n;
		good[num_good++].crc = crc32(b, n);
	}
	printf("read     %d good modules\n", num_read);

	for (unsigned i = 0; i < sizeof(bad_modules) / sizeof(bad_modules[0]); i++) {
		struct w w;
		bad_modules[i].make(&w);
		must_refuse(bad_modules[i].name, w.b, (w.bits + 31) / 32 * 4);
		free(w.b);
	}
	for (unsigned i = 0; i < sizeof(odd_modules) / sizeof(odd_modules[0]); i++) {
		struct w w;
		const char *why;
		odd_modules[i].make(&w);
		if (!read_module(odd_modules[i].name, w.b, (w.bits + 31) / 32 * 4, &why))
			failed(odd_modules[i].name, "refused: %s", why);
		else
			printf("read     %s\n", odd_modules[i].name);
		free(w.b);
	}

	unsigned changed = 0, skipped = 0;
	for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		int g = -1;
		for (int k = 0; k < num_good && g < 0; k++)
			if (good[k].size == mutations[i].size && good[k].crc == mutations[i].crc)
				g = k;
		if (g < 0) {
			skipped++;
			continue;
		}
		uint8_t *b = malloc(good[g].size);
		memcpy(b, good[g].data, good[g].size);
		for (unsigned k = 0; k < mutations[i].num; k++)
			b[mutations[i].at[k]] = mutations[i].byte[k];
		char name[256];
		snprintf(name, sizeof(name), "%s, changed (%s)", mutations[i].of, mutations[i].was);
		must_refuse(name, b, good[g].size);
		free(b);
		changed++;
	}
	printf("refused  %u changed modules", changed);
	if (skipped)
		printf("; %u skipped, made from bytes that none of the files given is", skipped);
	printf("\n");

	if (num_good)
		metallibs(good[0].data, good[0].size);

	for (int i = 0; i < num_good; i++)
		free(good[i].data);
	free(good);
	printf("%s\n", failures ? "readercheck: FAILED" : "readercheck: ok");
	return failures != 0;
}
