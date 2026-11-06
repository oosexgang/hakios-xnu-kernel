/*
 * A texture: the compiler library's (struct rwa_texture), its shape, and the two roads between it and
 * an IOSurface the Mali cannot hand over as GPU memory -- copied IN from a surface another process
 * draws, copied BACK into a surface the window server shows. Which thread does the copying, and what
 * is said when it fails, is the plug-in's (RedwoodMetalTexture in src/redwoodmetal.m).
 */
#include "rwm.h"

#include <IOKit/IOReturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <new>

struct rwm_texture {
	struct rwa_texture *texture = nullptr;
	struct rwa_texture_shape shape = {};
	IOSurfaceRef surface = nullptr;   /* the copy-back road's (rwm_texture_sync_surface) */
	IOSurfaceRef source = nullptr;    /* the copy-in road's (rwm_texture_copy_in) */
	uint32_t source_seed = 0;         /* the source's seed when it was last copied in */
	bool source_two_planes = false;   /* ... and it is YCbCr 4:2:0 in two planes, packed into one */
	uint32_t in_place = 0;            /* the surface it lies over, drawn in place (src/metal/import.cpp) */

	~rwm_texture()
	{
		if (texture != nullptr) {
			rwm_air()->texture_destroy(texture);
		}
		if (surface != nullptr) {
			CFRelease(surface);
		}
		if (source != nullptr) {
			CFRelease(source);
		}
	}
};

namespace {

struct free_deleter {
	void operator()(void *p) const { free(p); }
};
using block = std::unique_ptr<uint8_t, free_deleter>;

/* a lock held for as long as the copy reads the surface */
class surface_lock {
public:
	surface_lock(IOSurfaceRef surface, IOSurfaceLockOptions options)
		: surface_(surface), options_(options),
		  held_(IOSurfaceLock(surface, options, nullptr) == kIOReturnSuccess) {}
	~surface_lock()
	{
		if (held_) {
			IOSurfaceUnlock(surface_, options_, nullptr);
		}
	}
	bool held() const { return held_; }

private:
	IOSurfaceRef surface_;
	IOSurfaceLockOptions options_;
	bool held_;
};

/*
 * A plane of a surface as a copy reads it. The surface is another process's, and its size is its own:
 * the texture's shape says how much is read, and nothing here reads before it is shown that the plane
 * and the surface's allocation hold that much (the automated review's finding, 31 October: the copy
 * trusted the shape). A surface without planes answers the plane calls with zero, so it is read
 * through the whole-surface calls, as the copy-back does.
 */
struct plane {
	const uint8_t *base = nullptr;
	size_t row = 0, width = 0, height = 0, element = 0;
};

plane
plane_of(IOSurfaceRef surface, size_t index)
{
	const size_t planes = IOSurfaceGetPlaneCount(surface);

	if (planes == 0) {
		if (index != 0) {
			return plane{};
		}
		return plane{ (const uint8_t *)IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface),
			      IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
			      IOSurfaceGetBytesPerElement(surface) };
	}
	if (index >= planes) {
		return plane{};
	}
	return plane{ (const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, index),
		      IOSurfaceGetBytesPerRowOfPlane(surface, index), IOSurfaceGetWidthOfPlane(surface, index),
		      IOSurfaceGetHeightOfPlane(surface, index), IOSurfaceGetBytesPerElementOfPlane(surface, index) };
}

/* `rows` rows of `bytes` bytes, a row apart from the plane's start: inside the plane and the allocation */
bool
covers(IOSurfaceRef surface, const plane &p, size_t bytes, size_t rows)
{
	const uint8_t *start = (const uint8_t *)IOSurfaceGetBaseAddress(surface);
	const size_t alloc = IOSurfaceGetAllocSize(surface);
	size_t end;

	if (p.base == nullptr || start == nullptr || p.base < start || bytes == 0 || rows == 0 ||
	    bytes > p.row || rows > p.height) {
		return false;
	}
	if (__builtin_mul_overflow(rows - 1, p.row, &end) || __builtin_add_overflow(end, bytes, &end) ||
	    __builtin_add_overflow(end, (size_t)(p.base - start), &end)) {
		return false;
	}
	return end <= alloc;
}

/* the bytes of a texel of the formats the plug-in copies a surface into (RedwoodMetalDevice's import:
 * MTLPixelFormatBGRA8Unorm, its sRGB twin, RGBA8Unorm, RG8Unorm, R8Unorm), 0 for any other */
size_t
texel_bytes(unsigned format)
{
	switch (format) {
	case 80: case 81: case 70:
		return 4;
	case 30:
		return 2;
	case 10:
		return 1;
	}
	return 0;
}

} // namespace

extern "C" struct rwm_texture *
rwm_texture_new(struct rwa_texture *texture, const struct rwa_texture_shape *shape)
{
	rwm_texture *made = new (std::nothrow) rwm_texture;

	if (made == nullptr) {
		rwm_air()->texture_destroy(texture);
		return nullptr;
	}
	made->texture = texture;
	made->shape = *shape;
	return made;
}

extern "C" void
rwm_texture_free(struct rwm_texture *texture)
{
	delete texture;
}

extern "C" struct rwa_texture *
rwm_texture_mesa(const struct rwm_texture *texture)
{
	return texture->texture;
}

extern "C" const struct rwa_texture_shape *
rwm_texture_shape(const struct rwm_texture *texture)
{
	return &texture->shape;
}

/*
 * The copy-IN road: a surface another process draws (a widget's, a layer's backing store) that cannot
 * be laid under a texture as it is -- rows not a multiple of 64 bytes, or a format the import does not
 * serve ('LA08') -- is copied into the library's own texture, again before every command buffer that
 * samples it once the surface has changed (its seed). Before, such a texture was made empty and stayed
 * so: the widgets' solid squares (24 October).
 */
extern "C" void
rwm_texture_copy_in_from(struct rwm_texture *texture, IOSurfaceRef surface, bool two_planes)
{
	if (texture->source != nullptr) {
		CFRelease(texture->source);
	}
	texture->source = (IOSurfaceRef)CFRetain(surface);
	texture->source_seed = IOSurfaceGetSeed(surface) - 1;
	texture->source_two_planes = two_planes;
}

extern "C" int
rwm_texture_copy_in(struct rwm_texture *texture, size_t *bytes, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	const unsigned width = texture->shape.width, height = texture->shape.height;
	const unsigned origin[3] = { 0, 0, 0 }, size[3] = { width, height, 1 };

	*bytes = 0;
	if (texture->source == nullptr || air->texture_write == nullptr) {
		return 0;
	}

	uint32_t seed = IOSurfaceGetSeed(texture->source);

	if (seed != 0 && seed == texture->source_seed) {
		return 0;               /* (a surface whose seed never moves is copied every time) */
	}

	surface_lock lock(texture->source, kIOSurfaceLockReadOnly);

	if (!lock.held()) {
		return 0;
	}

	const plane first = plane_of(texture->source, 0);
	const void *texels = first.base;
	size_t row = first.row;
	block packed;

	/*
	 * A YCbCr surface in two planes ('420f', '420v': the wallpaper, photographs), which Metal samples
	 * as one texture of format 500 -- (Y, Cb, Cr) out of the sampler, and QuartzCore's shader
	 * converting with its own matrix (fc_has_ycbcr_matrix). Here it is an RGBA8 texture holding
	 * exactly that: Y in red, the chroma of its 2 x 2 block in green and blue.
	 */
	if (texture->source_two_planes) {
		const plane second = plane_of(texture->source, 1);
		/* a byte of Y per texel; a pair (Cb, Cr) per two texels, a row of them per two rows */
		const size_t chroma_bytes = (size_t)((width - 1) / 2) * 2 + 2, chroma_rows = (height - 1) / 2 + 1;

		if (width == 0 || height == 0 || !covers(texture->source, first, width, height) ||
		    !covers(texture->source, second, chroma_bytes, chroma_rows)) {
			snprintf(why, why_size, "the two-plane surface (%zux%zu, %zu and %zu bytes a row) is smaller "
				 "than its %ux%u texture", first.width, first.height, first.row, second.row, width,
				 height);
			return -1;
		}

		const uint8_t *luma = first.base, *chroma = second.base;
		const size_t luma_row = first.row, chroma_row = second.row;

		packed.reset((uint8_t *)malloc((size_t)width * height * 4));
		if (packed == nullptr) {
			return 0;
		}
		for (unsigned y = 0; y < height; y++) {
			const uint8_t *l = luma + y * luma_row, *c = chroma + (y / 2) * chroma_row;
			uint8_t *out = packed.get() + (size_t)y * width * 4;

			for (unsigned x = 0; x < width; x++) {
				out[4 * x + 0] = l[x];
				out[4 * x + 1] = c[(x / 2) * 2];
				out[4 * x + 2] = c[(x / 2) * 2 + 1];
				out[4 * x + 3] = 255;
			}
		}
		texels = packed.get();
		row = (size_t)width * 4;
	} else {
		/* the library reads `width` texels of the texture's format a row, rows `row` apart: that many
		 * bytes of every one of `height` rows inside the plane and the allocation, measured by the
		 * texture's own format and not the surface's element, which need not agree (the automated
		 * review's finding) */
		const size_t texel = texel_bytes(texture->shape.pixel_format);

		if (texel == 0) {
			snprintf(why, why_size, "a surface is copied into formats 80, 81, 70, 30 and 10, not %u",
				 texture->shape.pixel_format);
			return -1;
		}
		if (width == 0 || height == 0 || width > first.width ||
		    !covers(texture->source, first, (size_t)width * texel, height)) {
			snprintf(why, why_size, "the surface (%zux%zu, %zu bytes a row) is smaller than its %ux%u "
				 "texture of format %u", first.width, first.height, first.row, width, height,
				 texture->shape.pixel_format);
			return -1;
		}
	}
	*bytes = row * height;
	if (air->texture_write(texture->texture, 0, 0, origin, size, texels, row, why, why_size) != 0) {
		return -1;
	}
	texture->source_seed = seed;
	return 0;
}

/*
 * The copy-BACK road, for a GPU whose kernel driver cannot hand an IOSurface over as GPU memory (the
 * Mali's: there is no RedwoodGPU service, which is what kRedwoodSurface lives on). The texture is then
 * the library's own, and what was drawn into it goes into the surface's memory at the end of each
 * command buffer that drew into it -- the one copy the drawable path exists to save, paid so that the
 * window server's frames reach the display at all.
 */
extern "C" void
rwm_texture_copy_back_to(struct rwm_texture *texture, IOSurfaceRef surface)
{
	if (texture->surface != nullptr) {
		CFRelease(texture->surface);
	}
	texture->surface = (IOSurfaceRef)CFRetain(surface);
}

extern "C" bool
rwm_texture_has_surface(const struct rwm_texture *texture)
{
	return texture->surface != nullptr;
}

/*
 * What touches a texture's memory at once (-replaceRegion:, -getBytes:, a depth read) waits first for the buffers
 * committed before it: in Metal's order they come first, and on the driver's worker they are drawn later.
 */
extern "C" bool
rwm_texture_write(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		  const unsigned size[3], const void *bytes, size_t stride)
{
	char why[256] = { 0 };

	rwm_worker_drain("replaceRegion");
	if (rwm_air()->texture_write(texture->texture, level, slice, origin, size, bytes, stride, why, sizeof(why)) != 0) {
		rwm_print("writing texels: %s\n", why);
		return false;
	}
	return true;
}

extern "C" bool
rwm_texture_read(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		 const unsigned size[3], void *bytes, size_t stride)
{
	char why[256] = { 0 };

	rwm_worker_drain("getBytes");
	if (rwm_air()->texture_read(texture->texture, level, slice, origin, size, bytes, stride, why, sizeof(why)) != 0) {
		rwm_print("reading texels: %s\n", why);
		return false;
	}
	return true;
}

/* a depth/stencil texture by aspect, which Metal's -getBytes: has no way to name and the library's texture read
 * refuses by name */
extern "C" bool
rwm_texture_read_depth(struct rwm_texture *texture, const unsigned origin[3], const unsigned size[3], float *depth,
		       uint8_t *stencil)
{
	const struct rwm_air *air = rwm_air();
	char why[256] = { 0 };

	if (air->depth_read == nullptr) {
		rwm_print("this compiler library has no rwa_depth_read, so it is "
			  "older than this driver and cannot say what a depth buffer holds\n");
		return false;
	}
	rwm_worker_drain("depth read");
	if (air->depth_read(texture->texture, 0, 0, origin, size, depth, stencil, why, sizeof(why)) != 0) {
		rwm_print("reading the depth of a texture: %s\n", why);
		return false;
	}
	return true;
}

extern "C" uint32_t
rwm_texture_in_place(const struct rwm_texture *texture)
{
	return texture->in_place;
}

extern "C" void
rwm_texture_set_in_place(struct rwm_texture *texture, uint32_t surface)
{
	texture->in_place = surface;
}

extern "C" int
rwm_texture_sync_surface(struct rwm_texture *texture, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	const unsigned origin[3] = { 0, 0, 0 };
	const unsigned size[3] = { texture->shape.width, texture->shape.height, 1 };
	const size_t row = (size_t)texture->shape.width * 4, rows = texture->shape.height;
	IOSurfaceRef surface = texture->surface;
	char inner[256] = { 0 };

	if (surface == nullptr || air->texture_read == nullptr) {
		return 0;
	}
	/* one buffer per call: the window server commits from more than one thread, and a buffer
	   shared between them was freed under a copy (signal 11 in memmove) */
	block scratch((uint8_t *)malloc(row * rows));

	if (scratch == nullptr || air->texture_read(texture->texture, 0, 0, origin, size, scratch.get(),
						     row /* bytes per row */, inner, sizeof(inner)) != 0) {
		snprintf(why, why_size, "the frame could not be read: %s", inner);
		return -1;
	}

	surface_lock lock(surface, 0);

	if (!lock.held()) {
		snprintf(why, why_size, "the surface could not be locked");
		return -1;
	}
	/* a surface without planes answers the plane calls with zero: the whole-surface ones */
	const bool planar = IOSurfaceGetPlaneCount(surface) > 0;
	uint8_t *base = (uint8_t *)(planar ? IOSurfaceGetBaseAddressOfPlane(surface, 0)
					   : IOSurfaceGetBaseAddress(surface));
	const size_t pitch = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, 0) : IOSurfaceGetBytesPerRow(surface);
	const size_t height = planar ? IOSurfaceGetHeightOfPlane(surface, 0) : IOSurfaceGetHeight(surface);
	const size_t alloc = IOSurfaceGetAllocSize(surface), copy = row < pitch ? row : pitch;

	for (size_t y = 0; base != nullptr && pitch != 0 && y < rows && y < height &&
	     y * pitch + copy <= alloc; y++) {
		memcpy(base + y * pitch, scratch.get() + y * row, copy);
	}
	return 0;
}
