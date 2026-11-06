/*
 * QuartzCore's own shaders, as the firmware has them. The iPhone X image carries no default.metallib for
 * QuartzCore, but its default.pipelinelib holds the uber shader already SPECIALIZED for every pipeline
 * CoreAnimation 16.5 builds: `functions` is 451 complete .metallibs in AIR for air64-apple-ios16.5.0 behind an
 * LPPM table, and `pipelines.desc` names each one (com.apple.coreanimation.{vert,frag}.<spec>, function i = the
 * i-th .metallib by its offset) and each pipeline (com.apple.coreanimation.draw.<state>_<spec>, followed by the
 * labels of its vertex and fragment function). A pipeline whose label is there gets those two modules instead of
 * the ones it was described with -- which, with camtl=1, come from a macOS library whose constants and layouts
 * are not this QuartzCore's.
 *
 * Plain arrays sorted for a binary search, labels pointing into the description as read, the functions mapped
 * (the boot partition has no room for std's containers).
 */
#include "rwm.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

constexpr const char *library = "/System/Library/Frameworks/QuartzCore.framework/default.pipelinelib";
constexpr const char *prefix = "com.apple.coreanimation.";
constexpr size_t draw_prefix = 29;      /* "com.apple.coreanimation.draw." */

struct module {
	const uint8_t *at;      /* nullptr: no bitcode there */
	size_t size;
};
struct named {                  /* a function's label and its module's index */
	const char *label;
	unsigned index;
};
struct pipeline {               /* a draw label (or its shader part) and its two modules, in the file's order */
	const char *label;
	unsigned vertex, fragment, order;
};

const uint8_t *functions;
size_t functions_size;
char *desc;
size_t desc_size;
module *modules;
unsigned nmodules;
pipeline *draws, *specs;
unsigned ndraws, nspecs;

pthread_once_t once = PTHREAD_ONCE_INIT;

int
by_label(const void *a, const void *b)
{
	int c = strcmp(((const named *)a)->label, ((const named *)b)->label);

	return c;
}

int
by_pipeline(const void *a, const void *b)
{
	const pipeline *x = (const pipeline *)a, *y = (const pipeline *)b;
	int c = strcmp(x->label, y->label);

	return c != 0 ? c : (x->order > y->order) - (x->order < y->order);
}

int
by_offset(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/* sorted by label, the first in the file's order kept of each */
unsigned
sort_unique(pipeline *list, unsigned n)
{
	unsigned kept = 0;

	qsort(list, n, sizeof(*list), by_pipeline);
	for (unsigned i = 0; i < n; i++) {
		if (kept == 0 || strcmp(list[kept - 1].label, list[i].label) != 0) {
			list[kept++] = list[i];
		}
	}
	return kept;
}

const pipeline *
find(const pipeline *list, unsigned n, const char *label)
{
	unsigned lo = 0, hi = n;

	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		int c = strcmp(list[mid].label, label);

		if (c == 0) {
			return &list[mid];
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return nullptr;
}

bool
has_prefix(const char *label, const char *start)
{
	return strncmp(label, start, strlen(start)) == 0;
}

void *
read_whole(const char *path, size_t *size)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	char *at = nullptr;

	if (fd < 0) {
		return nullptr;
	}
	if (fstat(fd, &st) == 0 && st.st_size > 0 && (at = (char *)malloc((size_t)st.st_size + 1)) != nullptr) {
		ssize_t got = read(fd, at, (size_t)st.st_size);

		if (got != st.st_size) {
			free(at);
			at = nullptr;
		} else {
			at[st.st_size] = '\0';
			*size = (size_t)st.st_size;
		}
	}
	close(fd);
	return at;
}

const uint8_t *
map_whole(const char *path, size_t *size)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	void *at = MAP_FAILED;

	if (fd < 0) {
		return nullptr;
	}
	if (fstat(fd, &st) == 0 && st.st_size > 0) {
		at = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	}
	close(fd);
	if (at == MAP_FAILED) {
		return nullptr;
	}
	*size = (size_t)st.st_size;
	return (const uint8_t *)at;
}

uint64_t
word64(const uint8_t *at)
{
	uint64_t w;

	memcpy(&w, at, sizeof(w));
	return w;
}

void
load()
{
	char path[256];

	snprintf(path, sizeof(path), "%s/pipelines.desc", library);
	desc = (char *)read_whole(path, &desc_size);
	snprintf(path, sizeof(path), "%s/functions", library);
	functions = map_whole(path, &functions_size);
	if (desc == nullptr || functions == nullptr || functions_size < 0x30 || memcmp(functions, "LPPM", 4) != 0) {
		rwm_print("no pipeline library to take QuartzCore's shaders from\n");
		return;
	}

	const uint8_t *f = functions;
	const size_t fl = functions_size, dl = desc_size;
	/* the modules, in the order of their offsets */
	const uint64_t count = word64(f + 0x28);
	uint64_t *offsets = (uint64_t *)calloc(count < 4096 ? count : 4096, sizeof(*offsets));
	unsigned noffsets = 0;

	for (uint64_t i = 0; offsets != nullptr && i < count && i < 4096 && 0x30 + (i + 1) * 48 <= fl; i++) {
		offsets[noffsets++] = word64(f + 0x30 + i * 48 + 32);
	}
	qsort(offsets, noffsets, sizeof(*offsets), by_offset);
	modules = (module *)calloc(noffsets + 1, sizeof(*modules));

	const uint64_t data = word64(f + 0x20);   /* the table's offsets count from here */

	for (unsigned i = 0; modules != nullptr && i < noffsets; i++) {
		const uint64_t off = data + offsets[i];
		uint64_t bc_off = 0, bc_len = 0;

		if (off + 0x58 <= fl && memcmp(f + off, "MTLB", 4) == 0) {
			bc_off = word64(f + off + 0x48);
			bc_len = word64(f + off + 0x50);
		}
		modules[nmodules++] = bc_len == 0 || off + bc_off + bc_len > fl ? module{ nullptr, 0 }
										 : module{ f + off + bc_off, (size_t)bc_len };
	}
	free(offsets);

	/* the function labels: {2, 0xc, 0x10, index} before each; and the pipelines: a draw label, then its
	 * vertex and fragment labels (labels end at a NUL, so they are used where they lie) */
	const size_t plen = strlen(prefix);
	size_t room = dl / 24 + 1;    /* (a label is 30 characters and more, and binary lies between them) */
	named *names = (named *)calloc(room, sizeof(*names));
	unsigned nnames = 0;

	for (size_t at = 16; names != nullptr && at + plen < dl; at++) {
		if (memcmp(desc + at, prefix, plen) != 0) {
			continue;
		}

		const size_t end = at + strlen(desc + at);
		uint32_t w[4];

		memcpy(w, desc + at - 16, sizeof(w));
		if (((uintptr_t)(desc + at - 16) & 3) == 0 && w[0] == 2 && w[1] == 0xc && w[2] == 0x10 &&
		    (has_prefix(desc + at, "com.apple.coreanimation.frag.") ||
		     has_prefix(desc + at, "com.apple.coreanimation.vert.")) && nnames < room) {
			names[nnames++] = named{ desc + at, w[3] };
		}
		at = end;
	}
	qsort(names, nnames, sizeof(*names), by_label);

	auto index_of = [&](const char *label, unsigned *index) {
		named key = { label, 0 };
		const named *hit = (const named *)bsearch(&key, names, nnames, sizeof(*names), by_label);

		if (hit != nullptr) {
			*index = hit->index;
		}
		return hit != nullptr;
	};

	draws = (pipeline *)calloc(room, sizeof(*draws));

	const char *draw = nullptr, *vert = nullptr, *frag = nullptr;

	for (size_t at = 0; draws != nullptr && at + plen < dl; at++) {
		if (memcmp(desc + at, prefix, plen) != 0) {
			continue;
		}

		const char *label = desc + at;
		const size_t end = at + strlen(label);
		unsigned vi, fi;

		if (has_prefix(label, "com.apple.coreanimation.draw.")) {
			draw = label;
			vert = frag = nullptr;
		} else if (draw != nullptr && has_prefix(label, "com.apple.coreanimation.vert.")) {
			vert = label;
		} else if (draw != nullptr && has_prefix(label, "com.apple.coreanimation.frag.")) {
			frag = label;
		}
		if (draw != nullptr && vert != nullptr && frag != nullptr && index_of(vert, &vi) && index_of(frag, &fi) &&
		    ndraws < room) {
			draws[ndraws] = pipeline{ draw, vi, fi, ndraws };
			ndraws++;
			draw = nullptr;
		}
		at = end;
	}
	free(names);
	/* and by the shader part alone (after the first '_' of the state part): the part before it is the
	 * pipeline's attachment format and framebuffer blend, which no module depends on */
	specs = (pipeline *)calloc(ndraws + 1, sizeof(*specs));
	for (unsigned i = 0; specs != nullptr && i < ndraws; i++) {
		const char *u = strlen(draws[i].label) > draw_prefix ? strchr(draws[i].label + draw_prefix, '_') : nullptr;

		if (u != nullptr) {
			specs[nspecs++] = pipeline{ u + 1, draws[i].vertex, draws[i].fragment, draws[i].order };
		}
	}
	nspecs = specs != nullptr ? sort_unique(specs, nspecs) : 0;
	ndraws = draws != nullptr ? sort_unique(draws, ndraws) : 0;
	rwm_print("QuartzCore's pipeline library: %u modules, %u functions named, %u pipelines\n", nmodules, nnames, ndraws);
}

/* the tail with every blend-mode segment taken out: "B" and three or four lowercase letters */
void
without_blends(const char *tail, char *out, size_t room)
{
	size_t used = 0;

	for (const char *at = tail; *at != '\0' && used + 1 < room;) {
		if (at[0] == 'B' && at[1] >= 'a' && at[1] <= 'z' && at[2] >= 'a' && at[2] <= 'z' && at[3] >= 'a' &&
		    at[3] <= 'z') {
			at += at[4] >= 'a' && at[4] <= 'z' ? 5 : 4;
			continue;
		}
		out[used++] = *at++;
	}
	out[used] = '\0';
}

/* how much of `theirs` begins as `ours` does, part by part ('_'): each part's common beginning, and on to the
 * next part only while the whole of ours matched */
uint64_t
score(const char *ours, const char *theirs)
{
	uint64_t total = 0;

	while (*ours != '\0' && *theirs != '\0') {
		size_t a = strcspn(ours, "_"), b = strcspn(theirs, "_"), common = 0;

		while (common < a && common < b && ours[common] == theirs[common]) {
			common++;
		}
		total = total * 64 + common;
		if (common != a || ours[a] == '\0' || theirs[b] == '\0') {
			break;
		}
		ours += a + 1;
		theirs += b + 1;
	}
	return total;
}

void
say(const char *line)
{
	sysctlbyname("kern.redwood.say", NULL, NULL, (void *)line, strlen(line));
}

bool
again(unsigned long &times)
{
	return (++times & (times - 1)) == 0;
}

} // namespace

extern "C" bool
rwm_plib_modules_for(const char *label, bool allow_near, const void **vertex, size_t *vertex_size,
		     const void **fragment, size_t *fragment_size)
{
	const pipeline *pair;

	if (label == nullptr || strncmp(label, "com.apple.coreanimation.draw.", draw_prefix) != 0) {
		return false;
	}
	pthread_once(&once, load);
	pair = find(draws, ndraws, label);

	const char *underscore = pair == nullptr && strlen(label) > draw_prefix ? strchr(label + draw_prefix, '_') : nullptr;

	if (pair == nullptr && underscore != nullptr) {
		/* Not among the pipelines the firmware specialized: the same shaders under another attachment
		 * format or framebuffer blend; then (once QuartzCore's own shader with the pipeline's constants has
		 * failed to build) the nearest one whose fragment part differs only by a blend mode (TimgBdsoA2Xhfn_Isrc
		 * -> TimgA2Xhfn_Isrc), which then composites with source-over instead of its blend mode */
		const char *tail = underscore + 1;
		char plain[256], near[320];

		pair = find(specs, nspecs, tail);
		if (pair == nullptr && !allow_near) {
			return false;
		}
		if (pair == nullptr) {
			without_blends(tail, plain, sizeof(plain));
			snprintf(near, sizeof(near), "%.*s%s", (int)(tail - label), label, plain);
			pair = find(draws, ndraws, near);
			if (pair == nullptr) {
				pair = find(specs, nspecs, plain);
			}
			if (pair != nullptr) {
				static unsigned long times;

				if (again(times)) {
					rwm_print("%s: not in the pipeline library; %s instead (its blend mode lost)\n", label, near);
				}
			}
		}
		if (pair == nullptr) {
			/* Nor that: the specialized shaders whose label shares the longest beginning with this one's,
			 * part by part (TmuaA2Xhf_IsrcCcl_Isrg, opening an application, 24 October). Not this layer's
			 * shader, but a window server that ends itself over one pipeline takes SpringBoard and every
			 * application with it. */
			uint64_t best_score = 0;

			for (unsigned i = 0; i < nspecs; i++) {
				const uint64_t s = score(plain, specs[i].label);

				if (s > best_score) {
					best_score = s;
					pair = &specs[i];
				}
			}
			if (pair != nullptr) {
				static unsigned long times;
				char line[200];

				if (again(times)) {
					rwm_print("%s: not in the pipeline library; the shaders of %s instead\n", label, pair->label);
				}
				snprintf(line, sizeof(line), "!rwm %s: the shaders of %s instead", label, pair->label);
				say(line);
			}
		}
	}
	if (pair == nullptr || pair->vertex >= nmodules || pair->fragment >= nmodules ||
	    modules[pair->vertex].at == nullptr || modules[pair->fragment].at == nullptr) {
		return false;
	}
	*vertex = modules[pair->vertex].at;
	*vertex_size = modules[pair->vertex].size;
	*fragment = modules[pair->fragment].at;
	*fragment_size = modules[pair->fragment].size;
	return true;
}
