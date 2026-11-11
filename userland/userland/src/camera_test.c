/*
 * camera_test: the wide camera's frames as a program gets them (cam=5).
 *
 * usage: camera_test raw [FRAMES] [noae] [noawb] [keepraw|full] [ccm M] [sensor N]
 *        camera_test cycle [FRAMES]
 *        camera_test noise [SENSOR|all]
 *   raw: reads FRAMES frames (default 150) from the kernel's /dev/hakios-camera, with src/camera_isp.c's
 *   exposure (unless noae: the kernel's, boot-args cam_shutter and cam_gain) and white balance working
 *   on them, says what came and how fast, and leaves the last one as a picture for the host (xl
 *   camera: logs/, never the repository): one pixel a cell, NV12, or with full every pixel (bilinear
 *   demosaic, at the sensor's size), or with keepraw the frame itself.
 *   With sensor N, another sensor first (0 the macro, 1 the wide, 2 the front, 3 the ultra-wide); with
 *   noawb, no white balance (gains of 1: for colour bars, whose colours are known); with ccm M, the
 *   colour matrix M (nine numbers, row by row, separated by commas: a check of the matrix's path).
 *   cycle: each of the four sensors in turn, FRAMES frames each (default 30), the kernel's exposure:
 *   what came of each, and how long the change from one to the next took.
 *   noise: the sensor's black level and noise in the dark (the lens covered, or a dark room): at the
 *   longest exposure of its usual frame and 1x, 2x, 4x, 8x and its most gain, two frames each: per
 *   site of the 2x2 cell the mean (the black level), the temporal noise (the two frames' difference,
 *   over the square root of 2), the fixed pattern (what is left of the two frames' mean), the rows'
 *   noise (how much whole lines move together); for one sensor (0 to 3) or all four.
 */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "camera_isp.h"

static double
ms_since(uint64_t then)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)(mach_absolute_time() - then) * tb.numer / tb.denom / 1e6;
}

static double
ticks_ms(uint64_t ticks)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)ticks * tb.numer / tb.denom / 1e6;
}

static const char *const sensor_names[4] = { "macro", "wide", "front", "ultra-wide" };

/* HAKIOS_CAMERA_SENSOR: the kernel changes to that sensor (its usual mode) before the next frames */
static bool
choose_sensor(int device, unsigned n)
{
	struct hakios_camera_command c = { .command = HAKIOS_CAMERA_SENSOR, .a = n, .b = ~0u };

	if (write(device, &c, sizeof(c)) != (ssize_t)sizeof(c)) {
		printf("camera_test: the %s camera cannot be chosen: %s\n", n < 4 ? sensor_names[n] : "?", strerror(errno));
		return false;
	}
	return true;
}

static int
cycle(int frames)
{
	int device = open(HAKIOS_CAMERA_DEVICE, O_RDWR), failures = 0;
	uint8_t *buffer = NULL;
	size_t size = 0;

	if (device < 0) {
		perror("camera_test: " HAKIOS_CAMERA_DEVICE);
		return 1;
	}
	for (unsigned n = 0; n < 4; n++) {
		struct hakios_camera_frame head, *f = &head;
		uint64_t asked = mach_absolute_time(), first = 0, last_seq = 0, first_seq = 0;
		int got = 0;
		ssize_t r;

		if (!choose_sensor(device, n)) {
			failures++;
			continue;
		}
		for (int tries = 0; (r = read(device, &head, sizeof(head))) < 0 && errno == EAGAIN && tries < 3; tries++) {
		}
		if (r != (ssize_t)sizeof(head) || memcmp(head.magic, HAKIOS_CAMERA_MAGIC, 8) != 0 || head.sensor != n) {
			printf("camera_test: the %s camera: no frames (%s, header of sensor %u)\n", sensor_names[n],
			    r < 0 ? strerror(errno) : "short", r == (ssize_t)sizeof(head) ? head.sensor : 99);
			failures++;
			if (r < 0 && errno == EIO) {
				break;
			}
			continue;
		}
		size_t want = sizeof(head) + (size_t)head.stride * head.height;

		if (want > size) {
			free(buffer);
			buffer = malloc(want);
			size = want;
		}
		double changed_ms = ms_since(asked);
		struct isp_stats s;

		for (int i = 0; i < frames; i++) {
			r = read(device, buffer, want);
			if (r != (ssize_t)want) {
				printf("camera_test: the %s camera: a read gave %zd of %zu bytes (%s)\n", sensor_names[n], r, want,
				    r < 0 ? strerror(errno) : "short");
				break;
			}
			f = (struct hakios_camera_frame *)buffer;
			got++;
			first = first ? first : f->time;
			first_seq = first_seq ? first_seq : f->sequence;
			last_seq = f->sequence;
		}
		if (got == 0) {
			failures++;
			continue;
		}
		isp_measure(NULL, f, buffer + f->header_bytes, &s);
		printf("camera_test: the %s camera (sensor %u): %ux%u, stride %u, Bayer %u; the first frame %.0f ms after it was asked "
		    "for; %d frames of %llu in %.2f s (%.1f a second); R %.1f G %.1f B %.1f above black\n", sensor_names[n], f->sensor,
		    f->width, f->height, f->stride, f->bayer, changed_ms, got, last_seq - first_seq + 1, ticks_ms(f->time - first) / 1000,
		    got > 1 ? (got - 1) * 1000.0 / ticks_ms(f->time - first) : 0.0, s.mean[0], s.mean[1], s.mean[2]);
	}
	free(buffer);
	close(device);
	return failures == 0 ? 0 : 1;
}

/* the value of pixel x of line y of a RAW10 frame */
static inline int
raw10(const struct hakios_camera_frame *f, const uint8_t *raw, uint32_t x, uint32_t y)
{
	const uint8_t *g = raw + (size_t)y * f->stride + (size_t)(x >> 2) * 5;

	return g[x & 3] << 2 | (g[4] >> ((x & 3) * 2) & 3);
}

/* the next frame whose exposure is the one asked for (up to 12 tries) */
static bool
read_settled(int device, uint8_t *buffer, size_t bytes, uint32_t lines, uint32_t gain)
{
	for (int tries = 0; tries < 12; tries++) {
		const struct hakios_camera_frame *f = (const void *)buffer;

		if (read(device, buffer, bytes) != (ssize_t)bytes) {
			return false;
		}
		if (f->exposure_lines == lines && f->gain == gain) {
			return true;
		}
	}
	return false;
}

static int
noise_one(int device, unsigned n)
{
	struct hakios_camera_frame head;
	uint8_t *a, *b;
	size_t bytes;
	ssize_t r;

	if (!choose_sensor(device, n)) {
		return 1;
	}
	for (int tries = 0; (r = read(device, &head, sizeof(head))) < 0 && errno == EAGAIN && tries < 3; tries++) {
	}
	if (r != (ssize_t)sizeof(head) || head.sensor != n) {
		printf("camera_test: noise: no frames from the %s camera\n", sensor_names[n]);
		return 1;
	}
	bytes = sizeof(head) + (size_t)head.stride * head.height;
	a = malloc(bytes);
	b = malloc(bytes);
	uint32_t lines = head.frame_lines_min - head.margin, gains[5] = { 1, 2, 4, 8, 0 };

	printf("camera_test: noise of the %s camera: %ux%u, black %u, exposure %.1f ms (%u lines); per site of the cell (R, G1, "
	    "G2, B in its order) the mean, temporal noise, fixed pattern, and the rows' noise, in 10-bit steps\n", sensor_names[n],
	    head.width, head.height, head.black, lines * (double)head.line_ns / 1e6, lines);
	for (int k = 0; k < 5; k++) {
		uint32_t gain = gains[k] ? gains[k] * head.gain_unity : head.gain_max;
		struct hakios_camera_command c = { .command = HAKIOS_CAMERA_EXPOSURE, .a = lines, .b = gain, .c = 0 };

		if (gain > head.gain_max) {
			continue;
		}
		if (write(device, &c, sizeof(c)) != (ssize_t)sizeof(c) || !read_settled(device, a, bytes, lines, gain) ||
		    read(device, b, bytes) != (ssize_t)bytes) {
			printf("camera_test: noise: the %s camera at gain %u: no frames with that exposure\n", sensor_names[n], gain);
			continue;
		}
		const struct hakios_camera_frame *f = (const void *)a;
		const uint8_t *ra = a + f->header_bytes, *rb = b + f->header_bytes;
		double sum[4] = { 0 }, sumd2[4] = { 0 }, sumd[4] = { 0 }, summ2[4] = { 0 }, cnt[4] = { 0 };
		double rows = 0, rows2 = 0, nrows = 0;

		for (uint32_t y = 0; y + 1 < f->height; y += 2) {
			double row = 0, rn = 0;

			for (uint32_t x = 0; x + 1 < f->width; x += 8) {
				for (int site = 0; site < 4; site++) {
					uint32_t xx = x + (site & 1), yy = y + (site >> 1);
					int va = raw10(f, ra, xx, yy), vb = raw10(f, rb, xx, yy);
					double m = (va + vb) / 2.0, d = va - vb;

					sum[site] += m;
					summ2[site] += m * m;
					sumd[site] += d;
					sumd2[site] += d * d;
					cnt[site]++;
					row += m;
					rn++;
				}
			}
			row /= rn;
			rows += row;
			rows2 += row * row;
			nrows++;
		}
		char line[300];
		int at = snprintf(line, sizeof(line), "camera_test:   gain %5.2fx:", (double)gain / f->gain_unity);

		for (int site = 0; site < 4; site++) {
			double mean = sum[site] / cnt[site], dmean = sumd[site] / cnt[site];
			double temporal = sqrt(sumd2[site] / cnt[site] - dmean * dmean) / sqrt(2.0);
			double spread = summ2[site] / cnt[site] - mean * mean;      /* the two frames' mean: fixed pattern and temporal / 2 */
			double fixed = spread > temporal * temporal / 2 ? sqrt(spread - temporal * temporal / 2) : 0;

			at += snprintf(line + at, sizeof(line) - (size_t)at, " %.2f/%.2f/%.2f", mean, temporal, fixed);
		}
		double rmean = rows / nrows;

		snprintf(line + at, sizeof(line) - (size_t)at, "; rows %.3f", sqrt(rows2 / nrows - rmean * rmean));
		printf("%s\n", line);
	}
	free(a);
	free(b);
	return 0;
}

static int
noise(const char *which)
{
	int device = open(HAKIOS_CAMERA_DEVICE, O_RDWR), failures = 0;

	if (device < 0) {
		perror("camera_test: " HAKIOS_CAMERA_DEVICE);
		return 1;
	}
	for (unsigned n = 0; n < 4; n++) {
		if (strcmp(which, "all") == 0 || (unsigned)atoi(which) == n) {
			failures += noise_one(device, n);
		}
	}
	close(device);
	return failures ? 1 : 0;
}

/* a piece of a picture of every pixel (one of 8 bands of lines, each an even number of them) */
struct full_job {
	const struct isp *isp;
	const struct hakios_camera_frame *f;
	const uint8_t *raw;
	uint8_t *y, *uv;
	uint32_t width;
};

static void
full_piece(void *context, size_t i)
{
	struct full_job *j = context;
	uint32_t rows = (j->f->height / 8 + 1) & ~1u, first = (uint32_t)i * rows, end = i == 7 ? j->f->height : first + rows;

	isp_render_full_nv12(j->isp, j->f, j->raw, j->y, j->width, j->uv, j->width, first, end, false);
}

static float ccm[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static int
raw(int frames, bool ae, bool awb, bool keep_raw, bool full, int sensor)
{
	struct hakios_camera_frame head, *f;
	struct isp isp;
	struct isp_stats s;
	uint64_t started = mach_absolute_time(), previous = 0, first_seq = 0;
	size_t bytes;
	int device = open(HAKIOS_CAMERA_DEVICE, O_RDWR), got = 0, failures = 0;
	ssize_t n;

	if (device < 0) {
		perror("camera_test: " HAKIOS_CAMERA_DEVICE);
		return 1;
	}
	if (sensor >= 0 && !choose_sensor(device, (unsigned)sensor)) {
		return 1;
	}
	for (int tries = 0; (n = read(device, &head, sizeof(head))) < 0 && errno == EAGAIN && tries < 3; tries++) {
	}
	if (n != (ssize_t)sizeof(head) || memcmp(head.magic, HAKIOS_CAMERA_MAGIC, 8) != 0) {
		perror("camera_test: the first frame's header");
		return 1;
	}
	printf("camera_test: the first frame after %.0f ms: %ux%u, stride %u, Bayer %u, black %u, %u lines of %u ns a frame "
	    "(%.2f frames/s at the shortest), exposure %u lines, gain %u (%u is 1x)\n", ms_since(started), head.width, head.height,
	    head.stride, head.bayer, head.black, head.frame_lines_min, head.line_ns, 1e9 / ((double)head.frame_lines_min * head.line_ns),
	    head.exposure_lines, head.gain, head.gain_unity);
	bytes = sizeof(head) + (size_t)head.stride * head.height;
	f = malloc(bytes);
	isp_init(&isp);
	isp_set_ccm(&isp, ccm);
	while (got < frames && failures < 5) {
		struct hakios_camera_command c;
		uint64_t t0 = mach_absolute_time();

		n = read(device, f, bytes);
		if (n != (ssize_t)bytes) {
			printf("camera_test: a read gave %zd of %zu bytes (%s)\n", n, bytes, n < 0 ? strerror(errno) : "short");
			failures++;
			continue;
		}
		double read_ms = ms_since(t0);
		const uint8_t *raw = (const uint8_t *)f + f->header_bytes;

		got++;
		first_seq = first_seq ? first_seq : f->sequence;
		isp_measure(&isp, f, raw, &s);
		if (awb) {
			isp_awb(&isp, &s);
		}
		if (ae && isp_ae(&isp, f, &s, &c) && write(device, &c, sizeof(c)) != (ssize_t)sizeof(c)) {
			perror("camera_test: the exposure");
		}
		if (got <= 3 || got % 15 == 0) {
			printf("camera_test: frame %llu (%d read): %.1f ms after the last, read in %.1f ms; exposure %.2f ms (%u lines), gain "
			    "%.2fx, digital %.2fx, frame %.1f ms; R %.1f G %.1f B %.1f above black, 1 %% above %.0f; wb %.2f %.2f\n",
			    f->sequence, got, previous ? ticks_ms(f->time - previous) : 0.0, read_ms,
			    f->exposure_lines * (double)f->line_ns / 1e6, f->exposure_lines, (double)f->gain / f->gain_unity, isp.digital,
			    f->frame_lines * (double)f->line_ns / 1e6, s.mean[0], s.mean[1], s.mean[2], s.bright, isp.wb[0], isp.wb[2]);
		}
		previous = f->time;
	}
	if (got == 0) {
		return 1;
	}
	printf("camera_test: %d frames of %llu (%llu skipped) in %.1f s\n", got, f->sequence - first_seq + 1,
	    f->sequence - first_seq + 1 - got, ms_since(started) / 1000);

	if (keep_raw) {
		struct hakios_camera_command *k = (struct hakios_camera_command *)((uint8_t *)f + f->header_bytes) - 1;
		uint32_t frame_bytes = f->stride * f->height, w = f->width, h = f->height, stride = f->stride;

		/* (the command goes over the end of the header, just before the frame's bytes) */
		memset(k, 0, sizeof(*k));
		k->command = HAKIOS_CAMERA_KEEP;
		k->bytes = frame_bytes;
		k->a = w;
		k->b = h;
		k->c = stride;
		k->d = 1;
		if (write(device, k, sizeof(*k) + frame_bytes) != (ssize_t)(sizeof(*k) + frame_bytes)) {
			perror("camera_test: the frame for the host");
		} else {
			printf("camera_test: the frame (RAW10) is the host's (xl camera)\n");
		}
		close(device);
		return 0;
	}
	/* the last one, as a picture for the host: one pixel per cell, or every pixel; NV12 */
	uint32_t w = full ? f->width : f->width / 2, h = full ? f->height : f->height / 2;
	size_t picture = (size_t)w * h * 3 / 2;
	struct hakios_camera_command *keep = malloc(sizeof(*keep) + picture);
	uint8_t *y = (uint8_t *)(keep + 1);
	uint64_t t0 = mach_absolute_time();

	if (full) {
		struct full_job job = { &isp, f, (const uint8_t *)f + f->header_bytes, y, y + (size_t)w * h, w };

		dispatch_apply_f(8, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &job, full_piece);
	} else {
		isp_render_nv12(&isp, f, (const uint8_t *)f + f->header_bytes, y, w, y + (size_t)w * h, w, w, h, 0, h, false);
	}
	printf("camera_test: the picture (%ux%u NV12%s) made in %.1f ms\n", w, h, full ? ", every pixel" : "", ms_since(t0));
	memset(keep, 0, sizeof(*keep));
	keep->command = HAKIOS_CAMERA_KEEP;
	keep->bytes = (uint32_t)picture;
	keep->a = w;
	keep->b = h;
	keep->c = w;
	keep->d = 2;
	if (write(device, keep, sizeof(*keep) + picture) != (ssize_t)(sizeof(*keep) + picture)) {
		perror("camera_test: the picture for the host");
	} else {
		printf("camera_test: the picture is the host's (xl camera)\n");
	}
	close(device);
	return 0;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 2 && strcmp(argv[1], "raw") == 0) {
		bool ae = true, awb = true, keep_raw = false, full = false;
		int sensor = -1;

		for (int i = 3; i < argc; i++) {
			ae = ae && strcmp(argv[i], "noae") != 0;
			awb = awb && strcmp(argv[i], "noawb") != 0;
			keep_raw = keep_raw || strcmp(argv[i], "keepraw") == 0;
			full = full || strcmp(argv[i], "full") == 0;
			if (strcmp(argv[i], "ccm") == 0 && i + 1 < argc) {
				const char *at = argv[++i];

				for (int k = 0; k < 9 && at != NULL; k++) {
					ccm[k] = strtof(at, NULL);
					at = strchr(at, ',');
					at = at != NULL ? at + 1 : NULL;
				}
				printf("camera_test: the colour matrix %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f\n", ccm[0], ccm[1],
				    ccm[2], ccm[3], ccm[4], ccm[5], ccm[6], ccm[7], ccm[8]);
			}
			if (strcmp(argv[i], "sensor") == 0 && i + 1 < argc) {
				sensor = atoi(argv[++i]);
			}
		}
		return raw(argc >= 3 ? atoi(argv[2]) : 150, ae, awb, keep_raw, full, sensor);
	}
	if (argc >= 2 && strcmp(argv[1], "noise") == 0) {
		return noise(argc >= 3 ? argv[2] : "all");
	}
	if (argc >= 2 && strcmp(argv[1], "cycle") == 0) {
		return cycle(argc >= 3 ? atoi(argv[2]) : 30);
	}
	fprintf(stderr, "usage: camera_test raw [FRAMES] [noae] [noawb] [keepraw|full] [ccm M] [sensor N]\n       camera_test cycle [FRAMES]\n       camera_test noise [SENSOR|all]\n");
	return 2;
}
