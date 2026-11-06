/*
 * A texture over an IOSurface (-newTextureWithDescriptor:iosurface:plane:), by the first road that serves it:
 *   in place       the surface's own memory under the library's texture (the plug-in's drawable import);
 *   two planes     YCbCr 4:2:0 read and never drawn into: copied into an RGBA8 texture of the library's;
 *   copy in        read and never drawn into, one plane of 80, 81, 70, 30 or 10: copied into one of its own;
 *   copy back      BGRA8 drawn into: the library's own texture, copied into the surface after each command
 *                  buffer that draws into it (the Mali's road: no RedwoodGPU service to lay one over).
 * Each kind of surface asked for is said once in the kernel's kept lines, and whether it came: a widget is
 * another process's surface, and one that did not come is a layer drawn with nothing (24 October).
 */
#include "rwm.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

void
say(const char *line)
{
	sysctlbyname("kern.redwood.say", NULL, NULL, (void *)line, strlen(line));
}

/* the first, second, fourth, eighth... time a site says it (as the plug-in's RW_AGAIN) */
bool
again(unsigned long &times)
{
	return (++times & (times - 1)) == 0;
}

void
tell_kind(IOSurfaceRef s, unsigned plane, const struct rwa_texture_shape &shape, bool placed, const char *why)
{
	static struct { unsigned format, fourcc, plane, usage; } seen[24];
	static unsigned seen_count;
	static int told;
	const unsigned fourcc = (unsigned)IOSurfaceGetPixelFormat(s);
	unsigned i;

	for (i = 0; i < seen_count; i++) {
		if (seen[i].format == shape.pixel_format && seen[i].fourcc == fourcc && seen[i].plane == plane &&
		    seen[i].usage == shape.usage) {
			break;
		}
	}
	/* each whole-screen surface a texture is made over, by its number (the snapshots of the closing
	 * animation read black: which of them this process draws) */
	if (told < 30 && IOSurfaceGetWidthOfPlane(s, plane) == 750 && IOSurfaceGetHeightOfPlane(s, plane) == 1624) {
		char line[120];

		told++;
		snprintf(line, sizeof(line), "!rwm surface %u: a texture over it in %s, usage %#x, %s", IOSurfaceGetID(s),
			 getprogname(), shape.usage, placed ? "in place" : "NOT in place");
		say(line);
	}
	if (i == seen_count && seen_count < 24) {
		char line[240];

		seen[seen_count].format = shape.pixel_format;
		seen[seen_count].fourcc = fourcc;
		seen[seen_count].plane = plane;
		seen[seen_count].usage = shape.usage;
		seen_count++;
		snprintf(line, sizeof(line), "!rwm surface texture: format %u, surface '%c%c%c%c' %zux%zu plane %u, "
			 "usage %#x, type %u: %s", shape.pixel_format, fourcc >> 24 & 0xff, fourcc >> 16 & 0xff,
			 fourcc >> 8 & 0xff, fourcc & 0xff, IOSurfaceGetWidthOfPlane(s, plane),
			 IOSurfaceGetHeightOfPlane(s, plane), plane, shape.usage, shape.type, placed ? "in place" : why);
		say(line);
	}
}

} // namespace

/*
 * The formats CoreAnimation's Metal renderer picks for a display it believes is extended-range (the iPhone
 * X's): Apple's private 5xx values beside BGRA10_XR (552), which no public SDK names and the compiler
 * library refuses as unknown. This panel is 8-bit BGRA, so they are made BGRA8Unorm (80), and said once.
 */
extern "C" unsigned
rwm_known_format(unsigned format)
{
	static uint8_t told[8];

	if (format >= 550 && format <= 555) {
		if (!told[format - 550]) {
			told[format - 550] = 1;
			rwm_print("pixel format %u is taken as BGRA8Unorm (80): an extended-range format of Apple's own, "
				  "for an 8-bit panel\n", format);
		}
		return 80;
	}
	return format;
}

extern "C" struct rwm_texture *
rwm_surface_import(struct rwa_device *device, IOSurfaceRef s, unsigned plane, const struct rwa_texture_shape *asked,
		   rwm_in_place_fn in_place, void *context, struct rwm_import *road, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	struct rwa_texture_shape shape = *asked;
	struct rwa_texture *texture;

	memset(road, 0, sizeof(*road));
	if (device == nullptr) {
		rwm_print("a drawable's texture needs the compiler library\n");
		return nullptr;
	}
	texture = in_place(context, device, s, plane, &shape, road, why, why_size);
	tell_kind(s, plane, shape, texture != nullptr, why);
	if (texture == nullptr && shape.pixel_format == 500) {
		static int said500;

		if (said500++ < 4) {
			char line[200];

			snprintf(line, sizeof(line), "!rwm ycbcr surface: plane %u, %zu planes, usage %#x, %zux%zu", plane,
				 IOSurfaceGetPlaneCount(s), shape.usage, IOSurfaceGetWidth(s), IOSurfaceGetHeight(s));
			say(line);
		}
	}
	/* YCbCr in two planes, read and never drawn into: packed into one RGBA8 texture */
	if (texture == nullptr && plane == 0 && shape.pixel_format == 500 && !(shape.usage & 0x4) &&
	    IOSurfaceGetPlaneCount(s) == 2 && air->texture_create != nullptr && air->texture_write != nullptr) {
		char why2[256] = { 0 };
		struct rwa_texture_shape rgba = shape;
		struct rwa_texture *own;

		rgba.pixel_format = 70;         /* MTLPixelFormatRGBA8Unorm */
		own = air->texture_create(device, &rgba, why2, sizeof(why2));
		if (own != nullptr) {
			struct rwm_texture *copy = rwm_texture_new(own, &rgba);

			if (copy != nullptr) {
				rwm_texture_copy_in_from(copy, s, true);
				road->kind = RWM_IMPORT_COPY_IN;
			}
			return copy;
		}

		static unsigned long times;

		if (again(times)) {
			rwm_print("no texture to pack a YCbCr surface into: %s\n", why2);
		}

		char line[240];

		snprintf(line, sizeof(line), "!rwm ycbcr: no RGBA8 texture: %s", why2);
		say(line);
	}
	/* read and never drawn into (MTLTextureUsageRenderTarget, 0x4, not asked for): copied in */
	if (texture == nullptr && plane == 0 && !(shape.usage & 0x4) && IOSurfaceGetPlaneCount(s) <= 1 &&
	    (shape.pixel_format == 80 || shape.pixel_format == 81 || shape.pixel_format == 70 ||
	     shape.pixel_format == 30 || shape.pixel_format == 10) && air->texture_create != nullptr &&
	    air->texture_write != nullptr) {
		char why2[256] = { 0 };
		struct rwa_texture *own = air->texture_create(device, &shape, why2, sizeof(why2));

		if (own != nullptr) {
			struct rwm_texture *copy = rwm_texture_new(own, &shape);

			if (copy != nullptr) {
				rwm_texture_copy_in_from(copy, s, false);
				road->kind = RWM_IMPORT_COPY_IN;
			}
			return copy;
		}

		static unsigned long times;

		if (again(times)) {
			rwm_print("no texture to copy a surface into: %s\n", why2);
		}
	}
	if (texture == nullptr && plane == 0 && shape.pixel_format == 80 && air->texture_create != nullptr &&
	    air->texture_read != nullptr) {
		char why2[256] = { 0 };

		texture = air->texture_create(device, &shape, why2, sizeof(why2));
		if (texture != nullptr) {
			struct rwm_texture *copy = rwm_texture_new(texture, &shape);
			static int told;

			if (!told) {
				char line[200];

				told = 1;
				rwm_print("a surface's texture is Mesa's own, copied into the surface after each command "
					  "buffer that draws into it (%s)\n", why);
				snprintf(line, sizeof(line), "!rwm surfaces copied back: %s", why);
				say(line);
			}
			if (copy != nullptr) {
				rwm_texture_copy_back_to(copy, s);
				road->kind = RWM_IMPORT_COPY_BACK;
			}
			return copy;
		}
		rwm_print("no drawable and no texture to copy back from: %s\n", why2);
	}
	if (texture == nullptr) {
		static unsigned long times;

		if (again(times)) {
			rwm_print("no drawable: %s\n", why);
		}
		return nullptr;
	}

	static int imported;

	if (!imported) {
		char line[200];

		imported = 1;
		snprintf(line, sizeof(line), "!rwm surfaces drawn in place: surface %u, %ux%u, %u bytes a row",
			 road->surface, road->width, road->height, road->row_bytes);
		say(line);
	}
	road->kind = RWM_IMPORT_IN_PLACE;

	struct rwm_texture *made = rwm_texture_new(texture, &shape);

	if (made != nullptr) {
		rwm_texture_set_in_place(made, road->surface);
	}
	return made;
}
