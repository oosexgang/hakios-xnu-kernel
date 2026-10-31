/*
 * One draw of tools/air/glassref, kept as a file: everything our side needs to draw it again (the two
 * functions' bitcode, the function constants, the vertices, every buffer and texture) and what Apple's GPU
 * drew. glassref writes one with --case FILE; tools/air/glasscase.c draws it again through libredwoodair --
 * softpipe or llvmpipe on this Mac, freedreno on the phone's Adreno 830 -- and says how far it is from Apple's.
 *
 * The file is the fields below in order, each variable-length one as a uint32 length and the bytes, all
 * little-endian, the structs as air_metal.h lays them out (both ends are LP64 and build from that header).
 */
#ifndef GLASSCASE_H
#define GLASSCASE_H

#include <stdint.h>
#include <stdio.h>
#include "air_metal.h"

#define GC_MAGIC 0x34435247u    /* "GRC4" */
#define GC_SLOTS 31             /* buffer, texture and sampler indices kept */
#define GC_STREAM 20            /* the vertex buffer's index */
#define GC_STRIDE 128           /* one vertex: up to eight attributes of four floats */
#define GC_VERTICES 6

struct gc_blob {
	uint32_t size;
	void *bytes;
};

struct gc_case {
	uint32_t w, h;
	uint32_t target_format;         /* MTLPixelFormat: RGBA32Float, RGBA16Float, or a 32-bit integer one */
	float tolerance;
	char vertex_name[128], fragment_name[128];
	struct gc_blob vertex_air, fragment_air;
	uint32_t vertex_constants, fragment_constants;  /* 1: the function declares function constants */
	uint32_t constant_count;
	struct rwa_constant constants[64];
	struct rwa_render_shape shape;
	float vertices[GC_VERTICES][GC_STRIDE / 4];
	struct gc_blob vertex_buffer[GC_SLOTS], fragment_buffer[GC_SLOTS];
	uint32_t fragment_sampler[GC_SLOTS];            /* 1: an API sampler (linear, clamp to edge) at this index */
	uint32_t texture_format[GC_SLOTS];              /* MTLPixelFormat, 0: none */
	char texture_kind[GC_SLOTS][48];                /* its texels, made by gc_pattern (not kept: the file is small) */
	struct gc_blob apple;                           /* w x h RGBA floats */
};

int gc_write(const char *path, const struct gc_case *c);
int gc_read(const char *path, struct gc_case *c, char *why, size_t why_size);
void gc_free(struct gc_case *c);

/*
 * A texture's w x h texels (16 bytes each) made from its kind: "grad" (the default: premultiplied, alpha never
 * flat), "checker", "sdf" (a rounded rectangle's signed distance in pixels, negative inside), "solid:R,G,B,A",
 * "seeds" (a jump flood's field of coordinates, for an integer format: each texel names a seed nearby).
 */
void gc_pattern(const char *kind, unsigned w, unsigned h, void *texels);

/* bytes a texel of a target takes, and a read-back target's texels as RGBA floats (in place, w*h*16 bytes) */
unsigned gc_texel_bytes(uint32_t format);
void gc_as_floats(float *texels, uint32_t format, unsigned w, unsigned h);

/*
 * Draw the case on `device`; `ours` gets w*h RGBA floats. 0, or nonzero with `why` said. `verbose` prints
 * what the library made of the pipeline.
 */
int gc_draw(struct rwa_device *device, const struct gc_case *c, float *ours, int verbose, char *why, size_t why_size);

/* Compare and say it on one line, prefixed by `name`; the number of pixels off by more than the tolerance. */
unsigned gc_compare(const char *name, const struct gc_case *c, const float *ours, int show_x, int show_y, FILE *out);

#endif
