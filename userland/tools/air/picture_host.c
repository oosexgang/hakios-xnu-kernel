/*
 * The reference for metal_drv_test's --picture, on this machine.
 *
 * userland/src/metal_drv_test.m --picture draws the phone's whole panel with 66 of Apple's
 * own shaders and counts 1 892 224 pixels of it against what they must hold
 * (docs/metal.md 10.33). This is the same picture and the same counts through the same
 * library on softpipe, so that every number that run prints has a reference computed here
 * and not an expectation written down beside it -- the reason tools/air/metalcheck.py
 * exists for the compute path, and the same reason here.
 *
 * It is deliberately NOT in the Mesa build: nothing of it belongs to the library, and a
 * meson target would put it in the way of whoever is editing air_metal.c. Build it by
 * hand against the library that build is already producing:
 *
 *   FD=build/mesa-host/build/src/gallium/drivers/freedreno
 *   tools/air/mk-host.sh                                   # if $FD/libredwoodair.so is not there
 *   gcc -O1 -Wall -I tools/mesa/air -o /tmp/picture_host tools/air/picture_host.c \
 *       -L$FD -l:libredwoodair.so -Wl,-rpath,$PWD/$FD
 *
 * and run it on the modules of the library the device test reads, unpacked from the
 * container (the device gets them from Metal itself, through -[_MTLFunction bitcodeData];
 * here there is no Metal, so tools/metallib.py does what the container reader does there):
 *
 *   python3 tools/metallib.py dump \
 *       ios165/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib /tmp/bc
 *   /tmp/picture_host /tmp/bc /tmp/picture.ppm
 *
 * What it must print, which is what the phone must print too:
 *
 *   picture: mosaic: 64 of 64 shaders drew
 *   picture: mosaic targets: 112 held the colour, 48 stayed at the clear, 96 integer
 *            targets not checked, 0 WRONG (of 256)
 *   picture: mosaic 0 wrong of 1192320, nearest band 0 wrong of 699840, linear texel
 *            centres 0 wrong of 64
 *   picture: at (134, row 40 of each band): linear R=18, nearest R=0
 *
 * The .ppm is the picture itself, at the size of the panel, to be looked at. Nothing of
 * Apple's shaders is written into it or into this file: it reads the system's own
 * .metallib where it lies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "air_metal.h"

enum {
	MTL_TYPE_2D = 2,
	MTL_USAGE_SHADER_READ = 1,
	MTL_USAGE_RENDER_TARGET = 4,
	MTL_BGRA8_UNORM = 80,
	MTL_RGBA32_FLOAT = 125,
	MTL_PRIM_TRIANGLE_STRIP = 4,
};

/* the panel of this phone: 1080x2400, read out of the kernel's own line
 * "display 1125x2436 on a panel of 1080x2400 (4320 bytes per row)" */
enum {
	PANEL_W = 1080, PANEL_H = 2400,
	TILES = 8,                              /* the mosaic is TILES x TILES shaders */
	TILE_W = PANEL_W / TILES,               /* 135 */
	TILE_H = 138,                           /* so that 8 rows of tiles are 1104 */
	MOSAIC_H = TILES * TILE_H,              /* 1104 */
	BAND_H = (PANEL_H - MOSAIC_H) / 2,      /* 648 each */
	LINEAR_Y = MOSAIC_H,                    /* 1104 */
	NEAREST_Y = MOSAIC_H + BAND_H,          /* 1752 */
	TEX = 8,                                /* the gradient texture is TEX x TEX */
	SX = PANEL_W / TEX,                     /* 135, odd: see below */
	SY = BAND_H / TEX,                      /* 81, odd */
};

/* Eight levels of a channel, each an exact byte, so that a texel written as k/255 comes
 * back as exactly k through an 8-bit unorm attachment and the check needs no tolerance. */
static const unsigned char ramp[TILES] = { 0, 36, 73, 109, 146, 182, 219, 255 };

static void
mosaic_colour(unsigned i, unsigned j, unsigned char bgra[4])
{
	bgra[0] = ramp[TILES - 1 - i];  /* B */
	bgra[1] = ramp[j];              /* G */
	bgra[2] = ramp[i];              /* R */
	bgra[3] = 255;
}

static void
texel_colour(unsigned x, unsigned y, unsigned char bgra[4])
{
	bgra[0] = ramp[y];                  /* B */
	bgra[1] = ramp[TILES - 1 - y];      /* G */
	bgra[2] = ramp[x];                  /* R */
	bgra[3] = 255;
}

static unsigned char *panel;

static void *
read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc((size_t)n);
	if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
		free(p);
		p = NULL;
	}
	fclose(f);
	if (p)
		*size = (size_t)n;
	return p;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: picture_host BCDIR OUT.ppm\n");
		return 2;
	}
	const char *dir = argv[1];
	char why[512] = "";

	setenv("RWA_SOFTWARE", "1", 0);
	struct rwa_device *dev = rwa_device_open(why, sizeof(why));
	if (!dev) {
		printf("picture: rwa_device_open: %s\n", why);
		return 1;
	}
	printf("picture: device is \"%s\"\n", rwa_device_name(dev));
	panel = calloc(1, (size_t)PANEL_W * PANEL_H * 4);

	/* ---- band 1: the mosaic ------------------------------------------------------- */
	struct rwa_texture *mt[4] = { 0 };
	for (int i = 0; i < 4; i++) {
		struct rwa_texture_shape ts = {
			.pixel_format = MTL_BGRA8_UNORM, .type = MTL_TYPE_2D,
			.width = TILE_W, .height = TILE_H, .depth = 1,
			.array_length = 1, .mip_levels = 1, .sample_count = 1,
			.usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
		};
		mt[i] = rwa_texture_create(dev, &ts, why, sizeof(why));
		if (!mt[i]) {
			printf("picture: mosaic attachment %d: %s\n", i, why);
			return 1;
		}
	}
	struct rwa_buffer *ub = rwa_buffer_create(dev, 48, why, sizeof(why));
	if (!ub) {
		printf("picture: uniform: %s\n", why);
		return 1;
	}
	struct rwa_buffer *ubs[16];
	for (int i = 0; i < 16; i++)
		ubs[i] = ub;

	unsigned drew = 0;
	unsigned char *tile = malloc((size_t)TILE_W * TILE_H * 4);
	unsigned long rt_colour = 0, rt_clear = 0, rt_skipped = 0, rt_wrong = 0;

	for (unsigned k = 0; k < TILES * TILES; k++) {
		char path[512], name[64];
		unsigned nn = 4 * k + 1;

		snprintf(name, sizeof(name), "clearFragment%02X", nn);
		snprintf(path, sizeof(path), "%s/%s.bc", dir, name);
		size_t size = 0;
		void *bc = read_file(path, &size);
		if (!bc) {
			printf("picture: no %s\n", path);
			continue;
		}
		struct rwa_render_shape shape = { .color_count = 4, .sample_count = 1 };
		for (int i = 0; i < 4; i++)
			shape.color_format[i] = MTL_BGRA8_UNORM;
		struct rwa_render_info info = { 0 };
		struct rwa_render *r = rwa_render_create(dev, NULL, 0, NULL, bc, size, NULL,
							 &shape, &info, why, sizeof(why));
		if (!r) {
			printf("picture: %s no pipeline: %s\n", name, why);
			free(bc);
			continue;
		}
		unsigned i = k % TILES, j = k / TILES;
		unsigned char want[4];
		mosaic_colour(i, j, want);
		float *u = rwa_buffer_map(ub);
		memset(u, 0, 48);
		u[0] = want[2] / 255.0f;        /* the shader's float4 is r,g,b,a */
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
		unsigned nbuf = info.fragment_buffers > 16 ? 16 : info.fragment_buffers;
		struct rwa_draw d = {
			.color = mt, .color_count = 4,
			.clear_color = (const float[]){ 0, 0, 0, 1 },
			.fragment = { .buffers = ubs, .buffer_count = nbuf },
			.primitive = MTL_PRIM_TRIANGLE_STRIP,
			.vertex_start = 0, .vertex_count = 4,
		};
		if (rwa_render_draw(r, &d, why, sizeof(why)) != 0) {
			printf("picture: %s no draw: %s\n", name, why);
			rwa_render_destroy(r);
			free(bc);
			continue;
		}
		if (rwa_finish(dev, why, sizeof(why)) != 0)
			printf("picture: finish: %s\n", why);
		const unsigned o[3] = { 0, 0, 0 }, e[3] = { TILE_W, TILE_H, 1 };
		if (rwa_texture_read(mt[0], 0, 0, o, e, tile, 0, why, sizeof(why)) != 0) {
			printf("picture: %s no readback: %s\n", name, why);
			rwa_render_destroy(r);
			free(bc);
			continue;
		}
		for (unsigned y = 0; y < TILE_H; y++)
			memcpy(panel + ((size_t)(j * TILE_H + y) * PANEL_W + i * TILE_W) * 4,
			       tile + (size_t)y * TILE_W * 4, (size_t)TILE_W * 4);
		/* The name says which target gets what: two bits per render target, 0 none,
		 * 1 float4, 2 int4, 3 uint4. So every attachment has a verdict of its own. */
		for (unsigned t = 0; t < 4; t++) {
			unsigned kind = (nn >> (2 * t)) & 3;
			unsigned char expect[4] = { 0, 0, 0, 255 };

			if (kind == 2 || kind == 3) {
				rt_skipped++;
				continue;
			}
			if (kind == 1)
				memcpy(expect, want, 4);
			if (t != 0 &&
			    rwa_texture_read(mt[t], 0, 0, o, e, tile, 0, why, sizeof(why)) != 0) {
				printf("picture: %s attachment %u: %s\n", name, t, why);
				continue;
			}
			unsigned long bad = 0;
			for (size_t q = 0; q < (size_t)TILE_W * TILE_H; q++)
				if (tile[q * 4] != expect[0] || tile[q * 4 + 1] != expect[1] ||
				    tile[q * 4 + 2] != expect[2])
					bad++;
			if (bad) {
				rt_wrong++;
				if (rt_wrong < 6)
					printf("picture: %s target %u (%s): %lu of %u pixels "
					       "wrong\n", name, t,
					       kind ? "its colour" : "untouched", bad,
					       TILE_W * TILE_H);
			} else if (kind) {
				rt_colour++;
			} else {
				rt_clear++;
			}
		}
		/* attachment 0 again, for the panel: the loop above left `tile` on target 3 */
		if (rwa_texture_read(mt[0], 0, 0, o, e, tile, 0, why, sizeof(why)) == 0)
			for (unsigned y = 0; y < TILE_H; y++)
				memcpy(panel + ((size_t)(j * TILE_H + y) * PANEL_W +
						i * TILE_W) * 4,
				       tile + (size_t)y * TILE_W * 4, (size_t)TILE_W * 4);
		drew++;
		rwa_render_destroy(r);
		free(bc);
	}
	printf("picture: mosaic: %u of %u shaders drew\n", drew, TILES * TILES);
	printf("picture: mosaic targets: %lu held the colour, %lu stayed at the clear, "
	       "%lu integer targets not checked, %lu WRONG (of %u)\n", rt_colour, rt_clear,
	       rt_skipped, rt_wrong, 4 * TILES * TILES);

	/* ---- bands 2 and 3: Apple's own textured blit ---------------------------------- */
	size_t vsize = 0, fsize = 0;
	char p1[512], p2[512];
	snprintf(p1, sizeof(p1), "%s/texturedVertex2D.bc", dir);
	snprintf(p2, sizeof(p2), "%s/texturedFragment2DFloat.bc", dir);
	void *vbc = read_file(p1, &vsize), *fbc = read_file(p2, &fsize);
	if (!vbc || !fbc) {
		printf("picture: no textured pair in %s\n", dir);
		return 1;
	}
	struct rwa_texture *bt[4] = { 0 };
	for (int i = 0; i < 4; i++) {
		struct rwa_texture_shape ts = {
			.pixel_format = MTL_BGRA8_UNORM, .type = MTL_TYPE_2D,
			.width = PANEL_W, .height = BAND_H, .depth = 1,
			.array_length = 1, .mip_levels = 1, .sample_count = 1,
			.usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
		};
		bt[i] = rwa_texture_create(dev, &ts, why, sizeof(why));
		if (!bt[i]) {
			printf("picture: band attachment %d: %s\n", i, why);
			return 1;
		}
	}
	struct rwa_texture_shape gs = {
		.pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TYPE_2D,
		.width = TEX, .height = TEX, .depth = 1,
		.array_length = 1, .mip_levels = 1, .sample_count = 1,
		.usage = MTL_USAGE_SHADER_READ,
	};
	struct rwa_texture *grad = rwa_texture_create(dev, &gs, why, sizeof(why));
	if (!grad) {
		printf("picture: gradient texture: %s\n", why);
		return 1;
	}
	static float texels[TEX * TEX * 4];
	for (unsigned y = 0; y < TEX; y++)
		for (unsigned x = 0; x < TEX; x++) {
			unsigned char c[4];
			texel_colour(x, y, c);
			float *t = texels + (y * TEX + x) * 4;
			t[0] = c[2] / 255.0f;
			t[1] = c[1] / 255.0f;
			t[2] = c[0] / 255.0f;
			t[3] = 1.0f;
		}
	const unsigned go[3] = { 0, 0, 0 }, ge[3] = { TEX, TEX, 1 };
	if (rwa_texture_write(grad, 0, 0, go, ge, texels, 0, why, sizeof(why)) != 0) {
		printf("picture: seeding the gradient: %s\n", why);
		return 1;
	}
	struct rwa_buffer *vpos = rwa_buffer_create(dev, 64, why, sizeof(why));
	struct rwa_buffer *vuv = rwa_buffer_create(dev, 64, why, sizeof(why));
	if (!vpos || !vuv) {
		printf("picture: vertex buffers: %s\n", why);
		return 1;
	}
	static const float quad[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
	memcpy(rwa_buffer_map(vpos), quad, sizeof(quad));
	memcpy(rwa_buffer_map(vuv), uv, sizeof(uv));
	struct rwa_buffer *vbufs[2] = { vpos, vuv };

	struct rwa_render_shape shape = { .color_count = 4, .sample_count = 1 };
	for (int i = 0; i < 4; i++)
		shape.color_format[i] = MTL_BGRA8_UNORM;

	/* The ladder the device test runs before Apple's blit, in the same order and the
	 * same four shapes: the same clearFragment01 the mosaic has already drawn 64 times,
	 * with no texture and no supplied vertex stage.
	 *
	 * On the phone it is there to say which axis the stall is on; here it is the
	 * reference for its counts AND one honest half of the question. This program calls
	 * the library directly, so it never goes through redwoodmetal.m -- and the phone
	 * stopped inside -renderCommandEncoderWithDescriptor:, which is exactly the part
	 * this cannot reach. What four passing rungs here DO say is that the library is
	 * willing to make and draw all four shapes; what they cannot say is anything about
	 * the driver's encoder or about freedreno.
	 */
	{
		static const struct { unsigned n, w, h; const char *name; } rung[] = {
			{ 4, TILE_W, TILE_H, "again-small" },
			{ 1, PANEL_W, BAND_H, "one-big" },
			{ 4, PANEL_W / 2, BAND_H / 2, "four-medium" },
			{ 4, PANEL_W, BAND_H, "four-big" },
		};
		const unsigned char want[4] = { 109, 182, 73, 255 };   /* b, g, r, a */
		size_t psize = 0;
		char pp[512];

		snprintf(pp, sizeof(pp), "%s/clearFragment01.bc", dir);
		void *pbc = read_file(pp, &psize);

		for (unsigned k = 0; pbc && k < sizeof(rung) / sizeof(rung[0]); k++) {
			struct rwa_texture *pt[4] = { 0 };
			struct rwa_render_shape ps = { .color_count = rung[k].n,
						       .sample_count = 1 };
			unsigned ok = 1;

			for (unsigned i = 0; i < rung[k].n; i++)
				ps.color_format[i] = MTL_BGRA8_UNORM;
			for (unsigned i = 0; i < rung[k].n && ok; i++) {
				struct rwa_texture_shape ts = {
					.pixel_format = MTL_BGRA8_UNORM, .type = MTL_TYPE_2D,
					.width = rung[k].w, .height = rung[k].h, .depth = 1,
					.array_length = 1, .mip_levels = 1, .sample_count = 1,
					.usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
				};
				pt[i] = rwa_texture_create(dev, &ts, why, sizeof(why));
				if (!pt[i]) {
					printf("picture: probe %s: %s\n", rung[k].name, why);
					ok = 0;
				}
			}
			struct rwa_render_info pinfo = { 0 };
			struct rwa_render *pr = ok ? rwa_render_create(dev, NULL, 0, NULL, pbc,
								      psize, NULL, &ps,
								      &pinfo, why,
								      sizeof(why))
						   : NULL;
			if (ok && !pr)
				printf("picture: probe %s: %s\n", rung[k].name, why);
			if (pr) {
				float *u = rwa_buffer_map(ub);

				memset(u, 0, 48);
				u[0] = want[2] / 255.0f;
				u[1] = want[1] / 255.0f;
				u[2] = want[0] / 255.0f;
				u[3] = 1.0f;
				unsigned pn = pinfo.fragment_buffers > 16 ? 16
									 : pinfo.fragment_buffers;
				struct rwa_draw pd = {
					.color = pt, .color_count = rung[k].n,
					.clear_color = (const float[]){ 0, 0, 0, 1 },
					.fragment = { .buffers = ubs, .buffer_count = pn },
					.primitive = MTL_PRIM_TRIANGLE_STRIP,
					.vertex_start = 0, .vertex_count = 4,
				};
				unsigned char *pb = malloc((size_t)rung[k].w * rung[k].h * 4);
				const unsigned o[3] = { 0, 0, 0 };
				const unsigned e[3] = { rung[k].w, rung[k].h, 1 };
				unsigned long bad = 0;

				if (rwa_render_draw(pr, &pd, why, sizeof(why)) != 0 ||
				    rwa_finish(dev, why, sizeof(why)) != 0 ||
				    rwa_texture_read(pt[0], 0, 0, o, e, pb, 0, why,
						     sizeof(why)) != 0) {
					printf("picture: probe %s: %s\n", rung[k].name, why);
				} else {
					for (size_t q = 0;
					     q < (size_t)rung[k].w * rung[k].h; q++)
						if (pb[q * 4] != want[0] ||
						    pb[q * 4 + 1] != want[1] ||
						    pb[q * 4 + 2] != want[2])
							bad++;
					printf("picture: probe %-12s %u of %ux%u: %lu pixels of "
					       "%u are not that colour -- %s\n", rung[k].name,
					       rung[k].n, rung[k].w, rung[k].h, bad,
					       rung[k].w * rung[k].h,
					       bad == 0 ? "WHOLE" : "WRONG");
				}
				free(pb);
				rwa_render_destroy(pr);
			}
			for (unsigned i = 0; i < rung[k].n; i++)
				if (pt[i])
					rwa_texture_destroy(pt[i]);
		}
		free(pbc);
	}

	struct rwa_render_info info = { 0 };
	struct rwa_render *r = rwa_render_create(dev, vbc, vsize, NULL, fbc, fsize, NULL,
						 &shape, &info, why, sizeof(why));
	if (!r) {
		printf("picture: textured pipeline: %s\n", why);
		return 1;
	}
	printf("picture: textured pipeline: %u vertex buffers, %u fragment textures, "
	       "%u samplers, vertex stage %s\n", info.vertex_buffers, info.fragment_textures,
	       info.fragment_samplers, info.generated_vertex_stage ? "generated" : "Apple's");

	unsigned char *band = malloc((size_t)PANEL_W * BAND_H * 4);
	for (int pass = 0; pass < 2; pass++) {
		struct rwa_sampler_state st = {
			.address = { 0, 0, 0 },
			.min_filter = pass == 0 ? 1u : 0u,
			.mag_filter = pass == 0 ? 1u : 0u,
			.mip_filter = 0, .compare_function = 0, .max_anisotropy = 1,
			.normalized_coordinates = 1, .lod_min = 0, .lod_max = 65504.0f,
		};
		struct rwa_sampler *samp = rwa_sampler_create(dev, &st, why, sizeof(why));
		if (!samp) {
			printf("picture: sampler %d: %s\n", pass, why);
			return 1;
		}
		struct rwa_texture *texs[1] = { grad };
		struct rwa_sampler *samps[1] = { samp };
		struct rwa_draw d = {
			.color = bt, .color_count = 4,
			.clear_color = (const float[]){ 0, 0, 0, 1 },
			.vertex = { .buffers = vbufs, .buffer_count = 2 },
			.fragment = { .textures = texs, .texture_count = 1,
				      .samplers = samps, .sampler_count = 1 },
			.primitive = MTL_PRIM_TRIANGLE_STRIP,
			.vertex_start = 0, .vertex_count = 4,
		};
		if (rwa_render_draw(r, &d, why, sizeof(why)) != 0) {
			printf("picture: band %d draw: %s\n", pass, why);
			return 1;
		}
		if (rwa_finish(dev, why, sizeof(why)) != 0)
			printf("picture: finish: %s\n", why);
		const unsigned o[3] = { 0, 0, 0 }, e[3] = { PANEL_W, BAND_H, 1 };
		if (rwa_texture_read(bt[0], 0, 0, o, e, band, 0, why, sizeof(why)) != 0) {
			printf("picture: band %d readback: %s\n", pass, why);
			return 1;
		}
		unsigned y0 = pass == 0 ? LINEAR_Y : NEAREST_Y;
		memcpy(panel + (size_t)y0 * PANEL_W * 4, band, (size_t)PANEL_W * BAND_H * 4);
		rwa_sampler_destroy(samp);
	}

	/* ---- the count ---------------------------------------------------------------- */
	unsigned long bad_mosaic = 0, bad_nearest = 0, bad_centres = 0;

	for (unsigned j = 0; j < TILES; j++)
		for (unsigned i = 0; i < TILES; i++) {
			unsigned char want[4];
			mosaic_colour(i, j, want);
			for (unsigned y = 0; y < TILE_H; y++)
				for (unsigned x = 0; x < TILE_W; x++) {
					const unsigned char *p = panel +
					    ((size_t)(j * TILE_H + y) * PANEL_W +
					     i * TILE_W + x) * 4;
					if (p[0] != want[0] || p[1] != want[1] ||
					    p[2] != want[2])
						bad_mosaic++;
				}
		}
	/*
	 * THE TEXEL ROW IS COUNTED FROM THE BOTTOM, and that is Metal's orientation rather
	 * than a workaround.
	 *
	 * The viewport now puts clip y = +1 in memory row 0, which is Metal's rule
	 * (docs/metal.md 10.28, landed once 10.32 had fixed the flat varying that was
	 * hiding it). The quad's corner at clip (-1, +1) therefore lands in row 0, and the
	 * texture coordinate that corner carries is v = 1 -- so row 0 of the band shows
	 * texel row TEX-1 and the band is the texture mirrored vertically.
	 *
	 * The uv table is deliberately NOT changed to cancel it. Those four corners are
	 * air_metal_test's own (draw_quad_pos / draw_quad_uv), so that this draw and the
	 * device's are one quad; and compensating in the coordinates would hide the very
	 * thing the viewport fix made visible, which is how a flat varying passed for a
	 * vertical flip for three boots. The picture is supposed to show the orientation,
	 * so the expectation flips and the geometry does not.
	 *
	 * The x axis is untouched: nothing in the viewport's y sign is a horizontal
	 * statement, which is also why the linear/nearest contrast at column 134 below
	 * reads exactly as it did before.
	 */
	for (unsigned q = 0; q < BAND_H; q++)
		for (unsigned p = 0; p < PANEL_W; p++) {
			unsigned char want[4];
			texel_colour(p / SX, TEX - 1 - q / SY, want);
			const unsigned char *g = panel +
			    ((size_t)(NEAREST_Y + q) * PANEL_W + p) * 4;
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2])
				bad_nearest++;
		}
	for (unsigned j = 0; j < TEX; j++)
		for (unsigned i = 0; i < TEX; i++) {
			unsigned char want[4];
			texel_colour(i, j, want);
			/* texel row j is now at band row TEX-1-j */
			unsigned px = SX * i + (SX - 1) / 2;
			unsigned py = SY * (TEX - 1 - j) + (SY - 1) / 2;
			const unsigned char *g = panel +
			    ((size_t)(LINEAR_Y + py) * PANEL_W + px) * 4;
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2]) {
				if (bad_centres < 6)
					printf("picture: linear texel centre (%u,%u) at panel "
					       "(%u,%u) is %u %u %u, must be %u %u %u\n",
					       i, j, px, LINEAR_Y + py, g[2], g[1], g[0],
					       want[2], want[1], want[0]);
				bad_centres++;
			}
		}
	printf("picture: mosaic %lu wrong of %u, nearest band %lu wrong of %u, "
	       "linear texel centres %lu wrong of %u\n",
	       bad_mosaic, TILES * TILES * TILE_W * TILE_H, bad_nearest, PANEL_W * BAND_H,
	       bad_centres, TEX * TEX);

	/* the one place where the two bands must differ: halfway between two texel centres.
	 * Unmoved by the viewport's y sign, and that is the point: this reads the RED
	 * channel, which texel_colour makes a function of x alone, so it is a horizontal
	 * statement and a vertical mirror cannot change it. */
	{
		unsigned x = SX - 1;            /* 134: between texel 0 and texel 1 */
		unsigned y = (SY - 1) / 2;      /* 40: the centre row of a texel row */
		const unsigned char *l = panel + ((size_t)(LINEAR_Y + y) * PANEL_W + x) * 4;
		const unsigned char *n = panel + ((size_t)(NEAREST_Y + y) * PANEL_W + x) * 4;

		printf("picture: at (%u, row %u of each band): linear R=%u, nearest R=%u "
		       "(texel 0 R=%u, texel 1 R=%u)\n", x, y, l[2], n[2], ramp[0], ramp[1]);
	}

	/* And the orientation itself, as two named pixels rather than as a count: with
	 * Metal's rule the top of the band is texel row TEX-1 and the bottom is texel row
	 * 0, so the band is BLUE at the top and GREEN at the bottom. Under the old sign it
	 * was the other way up, and a count of 699840 cannot say which way up anything is. */
	{
		unsigned char top[4], bottom[4];
		const unsigned char *a = panel +
		    ((size_t)(LINEAR_Y + (SY - 1) / 2) * PANEL_W + (SX - 1) / 2) * 4;
		const unsigned char *b = panel +
		    ((size_t)(LINEAR_Y + SY * (TEX - 1) + (SY - 1) / 2) * PANEL_W +
		     (SX - 1) / 2) * 4;

		texel_colour(0, TEX - 1, top);
		texel_colour(0, 0, bottom);
		printf("picture: the band's top left is %u %u %u (texel (0,%u), must be "
		       "%u %u %u) and its bottom left is %u %u %u (texel (0,0), must be "
		       "%u %u %u): clip y = +1 is memory row 0\n", a[2], a[1], a[0], TEX - 1,
		       top[2], top[1], top[0], b[2], b[1], b[0], bottom[2], bottom[1],
		       bottom[0]);
	}

	/* ---- the image ---------------------------------------------------------------- */
	FILE *out = fopen(argv[2], "wb");
	if (!out) {
		perror(argv[2]);
		return 1;
	}
	fprintf(out, "P6\n%d %d\n255\n", PANEL_W, PANEL_H);
	for (size_t i = 0; i < (size_t)PANEL_W * PANEL_H; i++) {
		unsigned char rgb[3] = { panel[i * 4 + 2], panel[i * 4 + 1], panel[i * 4] };
		fwrite(rgb, 1, 3, out);
	}
	fclose(out);
	printf("picture: %s written, %dx%d\n", argv[2], PANEL_W, PANEL_H);
	return 0;
}
