/*
 * Host tests of the cameras' picture processor (components/camera/src/camera_isp.c), as libhakios-media builds it: the
 * frames it takes and the photograph's rendering, which the capture device runs in pieces on dispatch's worker
 * threads. Its allocations are counted (malloc, calloc and free are this file's for it), and one of them can be
 * refused.
 *
 *   camera_isp_test                 the scenarios' names
 *   camera_isp_test SCENARIO        one scenario a process
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static pthread_mutex_t counted_lock = PTHREAD_MUTEX_INITIALIZER;
static long outstanding;        /* the processor's allocations not given back */
static int allocations;         /* made (or refused) since the scenario began */
static int refuse;              /* the allocation (from 1) to refuse; 0: none */

static void *
counted(void *p, bool asked)
{
	pthread_mutex_lock(&counted_lock);
	allocations += asked;
	if (asked && allocations == refuse) {
		free(p);
		p = NULL;
	}
	outstanding += p != NULL;
	pthread_mutex_unlock(&counted_lock);
	return p;
}

static void *
counted_malloc(size_t n)
{
	return counted(malloc(n), true);
}

static void *
counted_calloc(size_t n, size_t size)
{
	return counted(calloc(n, size), true);
}

static void
counted_free(void *p)
{
	if (p != NULL) {
		pthread_mutex_lock(&counted_lock);
		outstanding--;
		pthread_mutex_unlock(&counted_lock);
	}
	free(p);
}

#define malloc(n)       counted_malloc(n)
#define calloc(n, s)    counted_calloc(n, s)
#define free(p)         counted_free(p)
#include "../components/camera/src/camera_isp.c"
#undef malloc
#undef calloc
#undef free

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* a frame as the kernel's header describes one, its RAW10 lines a grey ramp, in a buffer of exactly its size */
struct frame {
	struct hakios_camera_frame f;
	uint8_t *raw;
};

static void
frame_make(struct frame *fr, uint32_t width, uint32_t height, uint32_t stride)
{
	memset(fr, 0, sizeof(*fr));
	fr->f.header_bytes = sizeof(fr->f);
	fr->f.format = 1;
	fr->f.width = width;
	fr->f.height = height;
	fr->f.stride = stride;
	fr->f.black = 64;
	fr->f.gain = fr->f.gain_unity = 256;
	fr->raw = malloc((size_t)stride * height);
	for (size_t i = 0; i < (size_t)stride * height; i++) {
		fr->raw[i] = (uint8_t)(40 + i % 151);
	}
}

/* a picture's planes, each in a buffer of exactly its size, filled with a mark nothing renders over by chance */
struct picture {
	uint32_t width, height;
	size_t y_stride, uv_stride;
	uint8_t *y, *uv;
};

static void
picture_make(struct picture *p, uint32_t width, uint32_t height)
{
	p->width = width;
	p->height = height;
	p->y_stride = width;
	p->uv_stride = ((size_t)width + 1) / 2 * 2;
	p->y = malloc(p->y_stride * height);
	p->uv = malloc(p->uv_stride * ((height + 1) / 2));
	memset(p->y, 0xa5, p->y_stride * height);
	memset(p->uv, 0xa5, p->uv_stride * ((height + 1) / 2));
}

static bool
picture_untouched(const struct picture *p)
{
	for (size_t i = 0; i < p->y_stride * p->height; i++) {
		if (p->y[i] != 0xa5) {
			return false;
		}
	}
	for (size_t i = 0; i < p->uv_stride * ((p->height + 1) / 2); i++) {
		if (p->uv[i] != 0xa5) {
			return false;
		}
	}
	return true;
}

static void
picture_free(struct picture *p)
{
	free(p->y);
	free(p->uv);
}

static struct isp isp;

static void
still(const struct frame *fr, struct picture *p, uint32_t first, uint32_t end)
{
	isp_render_still_nv12(&isp, &fr->f, fr->raw, p->y, p->y_stride, p->uv, p->uv_stride, p->width, p->height, first, end, false);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/*
 * The frames the processor takes: RAW10 only, at least 4x4, and lines of at least five bytes for every four pixels,
 * the last group's whole. Everything a stride one byte shorter would have read past is refused, the widths one
 * more than a multiple of 4 among them, which need a group more than half the width's cells do.
 */
static void
scenario_frames_taken(void)
{
	struct hakios_camera_frame f = { .format = 1, .width = 4000, .height = 3000, .stride = 5008 };

	CHECK(isp_frame_readable(&f), "the kernel's 4000x3000 frame, 16-byte lines");
	f.format = 0;
	CHECK(!isp_frame_readable(&f), "format 0");
	f.format = 2;
	CHECK(!isp_frame_readable(&f), "format 2");
	f.format = 1;
	for (uint32_t width = 1; width <= 4003; width += width < 40 ? 1 : 997) {
		uint32_t least = (width + 3) / 4 * 5;

		f.width = width;
		f.stride = least;
		CHECK(isp_frame_readable(&f) == (width >= 4), "width %u, stride %u", width, least);
		f.stride = least - 1;
		CHECK(!isp_frame_readable(&f), "width %u, stride %u (one short)", width, least - 1);
	}
	f.width = 4001;
	f.stride = ((4001 / 2 - 1) / 2) * 5 + 5;    /* (what cell() alone reads: unpack_line() reads one group further) */
	CHECK(!isp_frame_readable(&f), "width 4001, stride %u", f.stride);
	f.width = 4000;
	f.stride = 5000;
	for (uint32_t height = 0; height < 6; height++) {
		f.height = height;
		CHECK(isp_frame_readable(&f) == (height >= 4), "height %u", height);
	}
	f.height = 3000;
	f.width = 0;
	CHECK(!isp_frame_readable(&f), "no width");
	f.width = 0xffffffffu;
	f.stride = 0xffffffffu;
	CHECK(!isp_frame_readable(&f), "a width whose lines would not fit 32 bits");
}

/*
 * A refused frame is not read and nothing is drawn of it (the buffers are exactly the frame's and the picture's:
 * under the sanitizers a read past them fails the scenario); the smallest of every shape the processor takes,
 * odd widths and heights among them, at the least stride, is drawn whole, still and full, and every allocation
 * is given back.
 */
static void
scenario_odd_frames(void)
{
	static const uint32_t refused[][3] = {
		{ 4000, 3000, 4000 }, { 4001, 3000, 5000 }, { 4000, 1, 5008 }, { 4000, 3, 5008 }, { 3, 8, 5 }, { 64, 64, 79 },
	};
	struct frame fr;
	struct picture p;

	isp_init(&isp);
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		frame_make(&fr, refused[i][0], refused[i][1], refused[i][2]);
		picture_make(&p, 64, 48);
		still(&fr, &p, 0, p.height);
		CHECK(picture_untouched(&p), "%ux%u, stride %u: drawn", refused[i][0], refused[i][1], refused[i][2]);
		picture_free(&p);
		picture_make(&p, refused[i][0] < 64 ? refused[i][0] : 64, refused[i][1] < 48 ? refused[i][1] : 48);
		isp_render_full_nv12(&isp, &fr.f, fr.raw, p.y, p.y_stride, p.uv, p.uv_stride, 0, p.height, false);
		CHECK(picture_untouched(&p), "%ux%u, stride %u: drawn in full", refused[i][0], refused[i][1], refused[i][2]);
		picture_free(&p);
		free(fr.raw);
	}
	for (uint32_t width = 4; width <= 13; width++) {
		for (uint32_t height = 4; height <= 7; height++) {
			frame_make(&fr, width, height, (width + 3) / 4 * 5);
			picture_make(&p, 2 * width + 1, 2 * height + 1);
			still(&fr, &p, 0, p.height);
			CHECK(!picture_untouched(&p), "%ux%u: not drawn", width, height);
			picture_free(&p);
			picture_make(&p, width, height);
			isp_render_full_nv12(&isp, &fr.f, fr.raw, p.y, p.y_stride, p.uv, p.uv_stride, 0, height, false);
			CHECK(!picture_untouched(&p), "%ux%u: not drawn in full", width, height);
			picture_free(&p);
			free(fr.raw);
		}
	}
	CHECK(outstanding == 0, "%ld allocations not given back", outstanding);
}

struct piece {
	const struct frame *fr;
	struct picture *p;
	uint32_t first, end;
};

static void *
piece(void *context)
{
	struct piece *pc = context;

	still(pc->fr, pc->p, pc->first, pc->end);
	return NULL;
}

/*
 * A photograph drawn in eight pieces, each on a thread of its own that then ends, as dispatch's workers come and
 * go: the same picture as drawn in one piece, and nothing of the processor's left allocated once each piece is
 * done -- the lines it works in were the thread's, kept when the thread ended, some 96 KB a thread at 4032.
 */
static void
scenario_still_in_pieces(void)
{
	struct frame fr;
	struct picture whole, pieces;
	pthread_t threads[8];
	struct piece pc[8];

	isp_init(&isp);
	isp.zoom = 1.5f;
	isp.turned = true;
	frame_make(&fr, 4032, 64, 5040);
	picture_make(&whole, 4032, 48);
	picture_make(&pieces, 4032, 48);
	still(&fr, &whole, 0, whole.height);
	CHECK(outstanding == 0, "%ld allocations not given back by a picture drawn whole", outstanding);
	for (int i = 0; i < 8; i++) {
		pc[i] = (struct piece){ &fr, &pieces, (uint32_t)i * 6, (uint32_t)i * 6 + 6 };
		CHECK(pthread_create(&threads[i], NULL, piece, &pc[i]) == 0, "thread %d", i);
	}
	for (int i = 0; i < 8; i++) {
		pthread_join(threads[i], NULL);
	}
	CHECK(outstanding == 0, "%ld allocations not given back once the pieces' threads ended", outstanding);
	CHECK(memcmp(whole.y, pieces.y, whole.y_stride * whole.height) == 0, "the pieces' luma is not the whole's");
	CHECK(memcmp(whole.uv, pieces.uv, whole.uv_stride * (whole.height / 2)) == 0, "the pieces' chroma is not the whole's");
	picture_free(&whole);
	picture_free(&pieces);
	free(fr.raw);
}

/* each of a render's allocations refused in turn: nothing drawn, nothing written through a null, nothing left allocated */
static void
scenario_no_memory(void)
{
	struct frame fr;
	struct picture p;
	int made;

	isp_init(&isp);
	frame_make(&fr, 64, 32, 80);
	picture_make(&p, 48, 32);
	allocations = 0;
	still(&fr, &p, 0, p.height);
	made = allocations;
	CHECK(made >= 1 && outstanding == 0, "a still: %d allocations, %ld not given back", made, outstanding);
	picture_free(&p);
	for (refuse = 1; refuse <= made; refuse++) {
		picture_make(&p, 48, 32);
		allocations = 0;
		still(&fr, &p, 0, p.height);
		CHECK(picture_untouched(&p), "a still, its allocation %d refused: drawn", refuse);
		CHECK(outstanding == 0, "a still, its allocation %d refused: %ld not given back", refuse, outstanding);
		picture_free(&p);
	}
	refuse = 0;
	picture_make(&p, 64, 32);
	allocations = 0;
	isp_render_full_nv12(&isp, &fr.f, fr.raw, p.y, p.y_stride, p.uv, p.uv_stride, 0, p.height, false);
	made = allocations;
	CHECK(made >= 1 && outstanding == 0, "in full: %d allocations, %ld not given back", made, outstanding);
	picture_free(&p);
	for (refuse = 1; refuse <= made; refuse++) {
		picture_make(&p, 64, 32);
		allocations = 0;
		isp_render_full_nv12(&isp, &fr.f, fr.raw, p.y, p.y_stride, p.uv, p.uv_stride, 0, p.height, false);
		CHECK(picture_untouched(&p), "in full, its allocation %d refused: drawn", refuse);
		CHECK(outstanding == 0, "in full, its allocation %d refused: %ld not given back", refuse, outstanding);
		picture_free(&p);
	}
	refuse = 0;
	free(fr.raw);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "frames_taken", scenario_frames_taken },
		{ "odd_frames", scenario_odd_frames },
		{ "still_in_pieces", scenario_still_in_pieces },
		{ "no_memory", scenario_no_memory },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("camera_isp_test %s: %d checks, %d failed\n", argv[1], checks, failures);
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
