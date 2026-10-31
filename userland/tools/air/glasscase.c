/*
 * glasscase: draw tools/air/glassref's cases again through libredwoodair and say how far each is from what
 * Apple's GPU drew (tools/air/glasscase.h). On this Mac (RWA_SOFTWARE=1) that is softpipe or llvmpipe; on the
 * phone it is freedreno and the Adreno 830 -- the one compiler glassref cannot reach, ir3.
 *
 *   glasscase [--tol T] [--show X,Y] [--verbose] [--env NAME=VALUE]... FILE.case|DIRECTORY ...
 *
 * A directory: every *.case in it, in name order. One line a case, then a total. Exit 0 when all match.
 * Also the drawing half of glassref, which builds this file in with GLASSCASE_NO_MAIN.
 */
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glasscase.h"

/* Metal's numbers, which the library takes as Metal defines them */
enum {
	FMT_R32Uint = 53, FMT_R32Sint = 54, FMT_R32Float = 55,
	FMT_RG32Uint = 103, FMT_RG32Sint = 104, FMT_RG32Float = 105,
	FMT_RGBA16Float = 115, FMT_RGBA32Uint = 123, FMT_RGBA32Sint = 124, FMT_RGBA32Float = 125,
	TEXTURE_2D = 2, USAGE_SHADER_READ = 1, USAGE_RENDER_TARGET = 4,
	ADDRESS_CLAMP_TO_EDGE = 0, FILTER_LINEAR = 1, MIP_NOT_MIPMAPPED = 0, COMPARE_NEVER = 0,
	PRIMITIVE_TRIANGLE = 3,
};

static void
say(char *why, size_t size, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(why, size, format, ap);
	va_end(ap);
}

/* ---- the file ---------------------------------------------------------------------------------- */

static int
put(FILE *f, const void *p, size_t n)
{
	return n == 0 || fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int
put_blob(FILE *f, const struct gc_blob *b)
{
	return put(f, &b->size, 4) || put(f, b->bytes, b->size);
}

int
gc_write(const char *path, const struct gc_case *c)
{
	FILE *f = fopen(path, "wb");
	uint32_t magic = GC_MAGIC, shape_size = sizeof(c->shape), constant_size = sizeof(c->constants[0]);
	int bad = f == NULL;

	if (bad) {
		return -1;
	}
	bad |= put(f, &magic, 4) || put(f, &shape_size, 4) || put(f, &constant_size, 4);
	bad |= put(f, &c->w, 4) || put(f, &c->h, 4) || put(f, &c->target_format, 4) || put(f, &c->tolerance, 4);
	bad |= put(f, c->vertex_name, sizeof(c->vertex_name)) || put(f, c->fragment_name, sizeof(c->fragment_name));
	bad |= put_blob(f, &c->vertex_air) || put_blob(f, &c->fragment_air);
	bad |= put(f, &c->vertex_constants, 4) || put(f, &c->fragment_constants, 4) || put(f, &c->constant_count, 4);
	bad |= put(f, c->constants, c->constant_count * sizeof(c->constants[0]));
	bad |= put(f, &c->shape, sizeof(c->shape)) || put(f, c->vertices, sizeof(c->vertices));
	for (int i = 0; i < GC_SLOTS; i++) {
		bad |= put_blob(f, &c->vertex_buffer[i]) || put_blob(f, &c->fragment_buffer[i]);
		bad |= put(f, &c->fragment_sampler[i], 4) || put(f, &c->texture_format[i], 4) ||
		       put(f, c->texture_kind[i], sizeof(c->texture_kind[i]));
	}
	bad |= put_blob(f, &c->apple);
	bad |= fclose(f) != 0;
	return bad ? -1 : 0;
}

static int
get(FILE *f, void *p, size_t n)
{
	return n == 0 || fread(p, 1, n, f) == n ? 0 : -1;
}

static int
get_blob(FILE *f, struct gc_blob *b)
{
	if (get(f, &b->size, 4) || b->size > (256u << 20)) {
		return -1;
	}
	b->bytes = b->size ? malloc(b->size) : NULL;
	return b->size && b->bytes == NULL ? -1 : get(f, b->bytes, b->size);
}

int
gc_read(const char *path, struct gc_case *c, char *why, size_t why_size)
{
	FILE *f = fopen(path, "rb");
	uint32_t magic = 0, shape_size = 0, constant_size = 0;
	int bad;

	memset(c, 0, sizeof(*c));
	if (f == NULL) {
		say(why, why_size, "cannot open it");
		return -1;
	}
	if (get(f, &magic, 4) || get(f, &shape_size, 4) || get(f, &constant_size, 4) || magic != GC_MAGIC ||
	    shape_size != sizeof(c->shape) || constant_size != sizeof(c->constants[0])) {
		say(why, why_size, "not a case of this build (magic %#x, shape %u of %zu bytes)", magic, shape_size,
		    sizeof(c->shape));
		fclose(f);
		return -1;
	}
	bad = get(f, &c->w, 4) || get(f, &c->h, 4) || get(f, &c->target_format, 4) || get(f, &c->tolerance, 4);
	bad = bad || get(f, c->vertex_name, sizeof(c->vertex_name)) || get(f, c->fragment_name, sizeof(c->fragment_name));
	bad = bad || get_blob(f, &c->vertex_air) || get_blob(f, &c->fragment_air);
	bad = bad || get(f, &c->vertex_constants, 4) || get(f, &c->fragment_constants, 4) || get(f, &c->constant_count, 4);
	bad = bad || c->constant_count > 64 || get(f, c->constants, c->constant_count * sizeof(c->constants[0]));
	bad = bad || get(f, &c->shape, sizeof(c->shape)) || get(f, c->vertices, sizeof(c->vertices));
	for (int i = 0; i < GC_SLOTS && !bad; i++) {
		bad = get_blob(f, &c->vertex_buffer[i]) || get_blob(f, &c->fragment_buffer[i]) ||
		      get(f, &c->fragment_sampler[i], 4) || get(f, &c->texture_format[i], 4) ||
		      get(f, c->texture_kind[i], sizeof(c->texture_kind[i]));
		c->texture_kind[i][sizeof(c->texture_kind[i]) - 1] = 0;
	}
	bad = bad || get_blob(f, &c->apple) || c->apple.size != c->w * c->h * 16;
	c->vertex_name[sizeof(c->vertex_name) - 1] = c->fragment_name[sizeof(c->fragment_name) - 1] = 0;
	fclose(f);
	if (bad) {
		say(why, why_size, "the file is cut short or damaged");
		gc_free(c);
		return -1;
	}
	return 0;
}

void
gc_free(struct gc_case *c)
{
	free(c->vertex_air.bytes);
	free(c->fragment_air.bytes);
	for (int i = 0; i < GC_SLOTS; i++) {
		free(c->vertex_buffer[i].bytes);
		free(c->fragment_buffer[i].bytes);
	}
	free(c->apple.bytes);
	memset(c, 0, sizeof(*c));
}

/* ---- texels ------------------------------------------------------------------------------------ */

void
gc_pattern(const char *kind, unsigned w, unsigned h, void *texels)
{
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			float *t = (float *)texels + 4 * (y * w + x);
			float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			if (strcmp(kind, "seeds") == 0) {
				uint32_t *s = (uint32_t *)t;
				s[0] = (x / 8) * 8 + 4, s[1] = (y / 8) * 8 + 4, s[2] = (x * 7 + y * 3) % 16, s[3] = 1;
			} else if (strcmp(kind, "checker") == 0) {
				float k = ((x / 8 + y / 8) & 1) ? 1.0f : 0.25f;
				t[0] = k, t[1] = 1.0f - k, t[2] = 0.5f, t[3] = 1.0f;
			} else if (strcmp(kind, "sdf") == 0) {
				float dx = fabsf(x + 0.5f - w / 2.0f) - w / 4.0f + 6, dy = fabsf(y + 0.5f - h / 2.0f) - h / 4.0f + 6;
				float out = sqrtf(fmaxf(dx, 0) * fmaxf(dx, 0) + fmaxf(dy, 0) * fmaxf(dy, 0)) +
					    fminf(fmaxf(dx, dy), 0) - 6;
				t[0] = out, t[1] = dx < dy ? 0.0f : 1.0f, t[2] = dx < dy ? 1.0f : 0.0f, t[3] = 1.0f;
			} else if (strncmp(kind, "solid:", 6) == 0) {
				const char *p = kind + 6;
				for (int k = 0; k < 4; k++) {
					t[k] = p != NULL && *p ? strtof(p, NULL) : 1.0f;
					p = p != NULL ? strchr(p, ',') : NULL;
					p = p != NULL ? p + 1 : NULL;
				}
			} else {        /* "grad" */
				t[0] = u, t[1] = v, t[2] = 0.5f + 0.5f * sinf(6.2831853f * (u + v));
				/* never constant: a flat alpha makes CA's edge filters divide 0 by 0, a test of NaN, not of
				 * translation */
				t[3] = 0.2f + 0.7f * (0.5f + 0.5f * cosf(6.2831853f * (2 * u - v)));
				t[0] *= t[3], t[1] *= t[3], t[2] *= t[3];       /* premultiplied, as CA's surfaces are */
			}
		}
	}
}

static unsigned
channels_of(uint32_t format)
{
	switch (format) {
	case FMT_R32Uint: case FMT_R32Sint: case FMT_R32Float: return 1;
	case FMT_RG32Uint: case FMT_RG32Sint: case FMT_RG32Float: return 2;
	default: return 4;
	}
}

static int
is_int(uint32_t format)
{
	return format == FMT_R32Uint || format == FMT_RG32Uint || format == FMT_RGBA32Uint ? 1 :
	       format == FMT_R32Sint || format == FMT_RG32Sint || format == FMT_RGBA32Sint ? -1 : 0;
}

unsigned
gc_texel_bytes(uint32_t format)
{
	return format == FMT_RGBA16Float ? 8 : 4 * channels_of(format);
}

static float
half_value(uint16_t h)
{
	unsigned e = (h >> 10) & 31, m = h & 1023;
	float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024), (int)e - 25);

	return h & 0x8000 ? -v : v;
}

void
gc_as_floats(float *texels, uint32_t format, unsigned w, unsigned h)
{
	unsigned n = channels_of(format);
	float *out = calloc((size_t)w * h, 16);

	if (format == FMT_RGBA16Float) {
		const uint16_t *half = (const uint16_t *)texels;
		for (unsigned i = 0; i < w * h * 4; i++) {
			out[i] = half_value(half[i]);
		}
	} else {
		for (unsigned i = 0; i < w * h; i++) {
			for (unsigned k = 0; k < 4; k++) {
				uint32_t word;
				if (k >= n) {
					out[4 * i + k] = k == 3 ? 1.0f : 0.0f;
					continue;
				}
				memcpy(&word, &texels[n * i + k], 4);
				out[4 * i + k] = is_int(format) > 0 ? (float)word : is_int(format) < 0 ? (float)(int32_t)word
												      : texels[n * i + k];
			}
		}
	}
	memcpy(texels, out, (size_t)w * h * 16);
	free(out);
}

/* ---- the draw ---------------------------------------------------------------------------------- */

int
gc_draw(struct rwa_device *device, const struct gc_case *c, float *ours, int verbose, char *why, size_t why_size)
{
	struct rwa_buffer *vb[GC_SLOTS] = { NULL }, *fb[GC_SLOTS] = { NULL };
	struct rwa_texture *ft[GC_SLOTS] = { NULL };
	struct rwa_sampler *fs[GC_SLOTS] = { NULL };
	struct rwa_buffer_bind vtab[GC_SLOTS], ftab[GC_SLOTS];
	struct rwa_vertex_stream streams[GC_SLOTS];
	struct rwa_render_info info;
	struct rwa_render_shape shape = c->shape;
	struct rwa_buffer *stream = NULL;
	struct rwa_texture *target = NULL;
	struct rwa_render *render = NULL;
	struct rwa_draw d;
	void *vcopy = NULL, *fcopy = NULL;
	static float zero[1][4] = { { 0, 0, 0, 0 } };
	static const struct rwa_sampler_state linear_clamp = {
		{ ADDRESS_CLAMP_TO_EDGE, ADDRESS_CLAMP_TO_EDGE, ADDRESS_CLAMP_TO_EDGE },
		FILTER_LINEAR, FILTER_LINEAR, MIP_NOT_MIPMAPPED, COMPARE_NEVER, 1, 1, 0.0f, 1000.0f };
	int result = -1;

	memset(vtab, 0, sizeof(vtab));
	memset(ftab, 0, sizeof(ftab));
	memset(streams, 0, sizeof(streams));
	memset(&info, 0, sizeof(info));

	/* our own copies of the bitcode, registered with the constants as the driver does it (redwoodmetal.m) */
	vcopy = malloc(c->vertex_air.size);
	fcopy = malloc(c->fragment_air.size);
	if (vcopy == NULL || fcopy == NULL) {
		say(why, why_size, "out of memory");
		goto out;
	}
	memcpy(vcopy, c->vertex_air.bytes, c->vertex_air.size);
	memcpy(fcopy, c->fragment_air.bytes, c->fragment_air.size);
	if (c->constant_count && c->vertex_constants) {
		rwa_constants_register(vcopy, c->vertex_air.size, c->constants, c->constant_count);
	}
	if (c->constant_count && c->fragment_constants) {
		rwa_constants_register(fcopy, c->fragment_air.size, c->constants, c->constant_count);
	}
	render = rwa_render_create(device, vcopy, c->vertex_air.size, c->vertex_name, fcopy, c->fragment_air.size,
				   c->fragment_name, &shape, &info, why, why_size);
	if (render == NULL) {
		goto out;
	}
	for (int i = 0; i < GC_SLOTS; i++) {
		if (c->vertex_buffer[i].size) {
			if ((vb[i] = rwa_buffer_create(device, c->vertex_buffer[i].size, why, why_size)) == NULL) {
				goto out;
			}
			memcpy(rwa_buffer_map(vb[i]), c->vertex_buffer[i].bytes, c->vertex_buffer[i].size);
			vtab[i].buffer = vb[i];
		}
		if (c->fragment_buffer[i].size) {
			if ((fb[i] = rwa_buffer_create(device, c->fragment_buffer[i].size, why, why_size)) == NULL) {
				goto out;
			}
			memcpy(rwa_buffer_map(fb[i]), c->fragment_buffer[i].bytes, c->fragment_buffer[i].size);
			ftab[i].buffer = fb[i];
		}
		if (c->fragment_sampler[i] && (fs[i] = rwa_sampler_create(device, &linear_clamp, why, why_size)) == NULL) {
			goto out;
		}
		if (c->texture_format[i]) {
			struct rwa_texture_shape ts = { c->texture_format[i], TEXTURE_2D, c->w, c->h, 1, 1, 1, 1, USAGE_SHADER_READ };
			unsigned origin[3] = { 0, 0, 0 }, size[3] = { c->w, c->h, 1 };
			void *texels = malloc((size_t)c->w * c->h * 16);
			int bad;
			if (texels == NULL) {
				say(why, why_size, "out of memory");
				goto out;
			}
			gc_pattern(c->texture_kind[i], c->w, c->h, texels);
			bad = (ft[i] = rwa_texture_create(device, &ts, why, why_size)) == NULL ||
			      rwa_texture_write(ft[i], 0, 0, origin, size, texels, c->w * 16, why, why_size);
			free(texels);
			if (bad) {
				goto out;
			}
		}
	}
	if ((stream = rwa_buffer_create(device, sizeof(c->vertices), why, why_size)) == NULL) {
		goto out;
	}
	memcpy(rwa_buffer_map(stream), c->vertices, sizeof(c->vertices));
	streams[GC_STREAM].buffer = stream;
	{
		struct rwa_texture_shape ts = { c->target_format, TEXTURE_2D, c->w, c->h, 1, 1, 1, 1,
						USAGE_RENDER_TARGET | USAGE_SHADER_READ };
		if ((target = rwa_texture_create(device, &ts, why, why_size)) == NULL) {
			goto out;
		}
	}
	if (verbose) {
		printf("  vertex buffers %#x, fragment buffers %#x textures %#x samplers %#x, attributes %u, streams %#x\n",
		       info.vertex_buffers, info.fragment_buffers, info.fragment_textures, info.fragment_samplers,
		       info.vertex_attribute_count, info.vertex_stream_mask);
	}
	memset(&d, 0, sizeof(d));
	d.color = &target;
	d.color_count = 1;
	d.clear_colors = zero;
	d.clear_mask = 1;
	d.vertex.buffers = vb;
	d.vertex.buffer_count = GC_SLOTS;
	d.fragment.buffers = fb;
	d.fragment.buffer_count = GC_SLOTS;
	d.fragment.textures = ft;
	d.fragment.texture_count = GC_SLOTS;
	d.fragment.samplers = fs;
	d.fragment.sampler_count = GC_SLOTS;
	d.primitive = PRIMITIVE_TRIANGLE;
	d.vertex_start = 0;
	d.vertex_count = GC_VERTICES;
	d.vertex_streams = streams;
	d.vertex_stream_count = GC_SLOTS;
	d.vertex_buffer_table = vtab;
	d.vertex_buffer_table_count = GC_SLOTS;
	d.fragment_buffer_table = ftab;
	d.fragment_buffer_table_count = GC_SLOTS;
	if (rwa_render_draw(render, &d, why, why_size) || rwa_finish(device, why, why_size)) {
		goto out;
	}
	{
		unsigned origin[3] = { 0, 0, 0 }, size[3] = { c->w, c->h, 1 };
		if (rwa_texture_read(target, 0, 0, origin, size, ours, c->w * gc_texel_bytes(c->target_format), why, why_size)) {
			goto out;
		}
	}
	gc_as_floats(ours, c->target_format, c->w, c->h);
	result = 0;
out:
	/* (the render, buffers and textures are the library's to free; a short-lived program leaves them) */
	return result;
}

/* ---- the comparison ---------------------------------------------------------------------------- */

unsigned
gc_compare(const char *name, const struct gc_case *c, const float *ours, int show_x, int show_y, FILE *out)
{
	const float *apple = c->apple.bytes;
	unsigned n = c->w * c->h, bad = 0, nan_apple = 0, nan_ours = 0;
	float worst = 0;
	int wx = 0, wy = 0;
	double sum_a[4] = { 0 }, sum_o[4] = { 0 };

	for (unsigned i = 0; i < n; i++) {
		float m = 0;
		for (int k = 0; k < 4; k++) {
			float x = apple[4 * i + k], y = ours[4 * i + k];
			nan_apple += isnan(x), nan_ours += isnan(y);
			if (!isnan(x)) sum_a[k] += x;
			if (!isnan(y)) sum_o[k] += y;
			float dlt = (isnan(x) != isnan(y)) ? INFINITY : (isnan(x) ? 0 : fabsf(x - y) / fmaxf(1.0f, fabsf(x)));
			m = fmaxf(m, dlt);
		}
		if (m > c->tolerance) {
			bad++;
		}
		if (m > worst) {
			worst = m, wx = (int)(i % c->w), wy = (int)(i / c->w);
		}
	}
	fprintf(out, "%s: %u of %u pixels differ (> %g), worst %g at (%d,%d); mean apple %.3f %.3f %.3f %.3f, ours %.3f %.3f %.3f %.3f%s%s\n",
		name, bad, n, c->tolerance, worst, wx, wy, sum_a[0] / n, sum_a[1] / n, sum_a[2] / n, sum_a[3] / n,
		sum_o[0] / n, sum_o[1] / n, sum_o[2] / n, sum_o[3] / n,
		nan_apple ? " (NaN in Apple's)" : "", nan_ours ? " (NaN in ours)" : "");
	int px[2][2] = { { wx, wy }, { show_x, show_y } };
	for (int k = 0; k < 2; k++) {
		if (px[k][0] < 0 || px[k][0] >= (int)c->w || px[k][1] >= (int)c->h ||
		    (k == 1 && px[k][0] == wx && px[k][1] == wy) || (k == 0 && bad == 0)) {
			continue;
		}
		unsigned i = (unsigned)(px[k][1] * (int)c->w + px[k][0]);
		fprintf(out, "  (%d,%d) apple %.4f %.4f %.4f %.4f   ours %.4f %.4f %.4f %.4f\n", px[k][0], px[k][1],
			apple[4 * i], apple[4 * i + 1], apple[4 * i + 2], apple[4 * i + 3],
			ours[4 * i], ours[4 * i + 1], ours[4 * i + 2], ours[4 * i + 3]);
	}
	return bad;
}

/* ---- the program ------------------------------------------------------------------------------- */

#ifndef GLASSCASE_NO_MAIN

static struct rwa_device *device;
static int verbose, show_x = -1, show_y = -1;
static float tolerance = -1;
static unsigned cases, matched, differ, failed;

static void
run(const char *path)
{
	struct gc_case c;
	char why[512] = "";
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

	if (gc_read(path, &c, why, sizeof(why))) {
		printf("glasscase: %s: %s\n", name, why);
		cases++, failed++;
		return;
	}
	if (tolerance >= 0) {
		c.tolerance = tolerance;
	}
	cases++;
	float *ours = calloc((size_t)c.w * c.h, 16);
	if (gc_draw(device, &c, ours, verbose, why, sizeof(why))) {
		printf("glasscase: %s: OURS FAILED: %s\n", name, why);
		failed++;
	} else if (gc_compare(name, &c, ours, show_x, show_y, stdout) == 0) {
		matched++;
	} else {
		differ++;
	}
	fflush(stdout);
	free(ours);
	gc_free(&c);
}

static int
by_name(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

int
main(int argc, char **argv)
{
	char why[512] = "";
	int a = 1;

	for (; a < argc && argv[a][0] == '-'; a++) {
		if (!strcmp(argv[a], "--tol") && a + 1 < argc) {
			tolerance = strtof(argv[++a], NULL);
		} else if (!strcmp(argv[a], "--show") && a + 1 < argc) {
			sscanf(argv[++a], "%d,%d", &show_x, &show_y);
		} else if (!strcmp(argv[a], "--verbose")) {
			verbose = 1;
		} else if (!strcmp(argv[a], "--env") && a + 1 < argc && strchr(argv[a + 1], '=')) {
			/* (init runs job lines without a shell: the driver's debug switches, IR3_SHADER_DEBUG and
			 * the like, come in this way) */
			char *eq = strchr(argv[++a], '=');
			*eq = 0;
			setenv(argv[a], eq + 1, 1);
			printf("glasscase: %s=%s\n", argv[a], eq + 1);
		} else {
			fprintf(stderr, "usage: glasscase [--tol T] [--show X,Y] [--verbose] [--env NAME=VALUE] FILE.case|DIRECTORY ...\n");
			return 2;
		}
	}
	if ((device = rwa_device_open(why, sizeof(why))) == NULL) {
		printf("glasscase: no device: %s\n", why);
		return 2;
	}
	for (; a < argc; a++) {
		struct dirent **list = NULL;
		int n = scandir(argv[a], &list, NULL, by_name);
		if (n < 0) {
			run(argv[a]);
			continue;
		}
		for (int i = 0; i < n; i++) {
			size_t len = strlen(list[i]->d_name);
			if (len > 5 && strcmp(list[i]->d_name + len - 5, ".case") == 0) {
				char path[1024];
				snprintf(path, sizeof(path), "%s/%s", argv[a], list[i]->d_name);
				run(path);
			}
			free(list[i]);
		}
		free(list);
	}
	printf("glasscase: %u cases: %u match, %u differ, %u could not be drawn\n", cases, matched, differ, failed);
	return matched == cases ? 0 : 1;
}

#endif
