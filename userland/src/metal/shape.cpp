/*
 * What a render pipeline asks for, said as it arrives, before the library can refuse any of it: each colour
 * attachment's format and blend state as Metal's own numbers -- `blend 1 rgb 1/5 op 0 alpha 1/5 op 0` is
 * premultiplied source-over (One, OneMinusSourceAlpha, Add), `blend 1 rgb 4/5` straight alpha -- and what the
 * library made of it. The plug-in reads Metal's descriptor into the shape; this says it.
 *
 * A BLEND LINE is deduplicated on its CONTENT and not on the call site: the window server built 127 pipelines
 * in logs/080121-run.log and printed 240 attachment lines, thirteen distinct, and the loader keeps the first
 * 256 KB of a log and the last megabyte. So a line is said the first time its exact content is seen and then on
 * powers of two of that content's own count, carrying the count; past the table every further distinct line
 * shares one counter rather than being dropped.
 */
#include "rwm.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

namespace {

/* the first, second, fourth... time this content is seen; `times` the count to say */
bool
due(unsigned long &times)
{
	return (++times & (times - 1)) == 0;
}

bool
blend_line(unsigned attachment, unsigned format, const struct rwa_color_blend *b)
{
	static struct {
		unsigned attachment, format;
		struct rwa_color_blend b;
		unsigned long times;
	} seen[32];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = nullptr;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].attachment == attachment && seen[i].format == format && memcmp(&seen[i].b, b, sizeof(*b)) == 0) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == nullptr && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].attachment = attachment;
		seen[nseen].format = format;
		seen[nseen].b = *b;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == nullptr) {
		times = &beyond;
	}
	if (!due(*times)) {
		return false;
	}
	rwm_print("blend: attachment %u format %u blend %u rgb %u/%u op %u alpha %u/%u op %u writeMask 0x%x\n", attachment,
		  format, b->blending_enabled, b->source_rgb_blend_factor, b->destination_rgb_blend_factor,
		  b->rgb_blend_operation, b->source_alpha_blend_factor, b->destination_alpha_blend_factor,
		  b->alpha_blend_operation, b->write_mask);
	if (*times > 1) {
		rwm_print("  ... and that is the %lu'th pipeline asking for exactly that%s\n", *times,
			  times == &beyond ? ", counting every configuration past the thirty-two this keeps apart" : "");
	}
	return true;
}

bool
ends_with(const char *text, const char *end)
{
	const size_t a = strlen(text), b = strlen(end);

	return a >= b && strcmp(text + a - b, end) == 0;
}

} // namespace

extern "C" void
rwm_shape_say(const char *label, const struct rwa_render_shape *shape, bool has_vertex_descriptor)
{
	bool blend_is_new = false;

	for (unsigned i = 0; i < shape->color_count && i < 8; i++) {
		blend_is_new |= blend_line(i, shape->color_format[i], &shape->color_blend[i]);
	}
	/* THE LABEL IS THE ANSWER: CoreAnimation emits a B<mnemonic> segment iff the fixed-function blend mode is
	 * not its "none", so draw.PBGRABsovXm_ is a pipeline whose blending this descriptor carries and
	 * draw.PBGRAXm_ one that blends inside its fragment shader -- said once per distinct configuration */
	if (blend_is_new) {
		rwm_print("blend: ... and the first such pipeline's label is %s\n", label != nullptr ? label : "(none)");
	}
	if (!has_vertex_descriptor) {
		return;
	}

	/* the vertex layout of the plain colour fill, once, for the log's end: a white fill came out blue
	 * (24 October), and its colour is a vertex attribute */
	static int layout_said;

	if (!layout_said && label != nullptr && ends_with(label, "draw.PBGRAXm_A2Xhf")) {
		const struct rwa_color_blend &b = shape->color_blend[0];
		char line[240];
		int n = snprintf(line, sizeof(line), "!rwm A2Xhf blend %u op %u/%u src %u/%u dst %u/%u mask %#x; layout:",
				 b.blending_enabled, b.rgb_blend_operation, b.alpha_blend_operation, b.source_rgb_blend_factor,
				 b.source_alpha_blend_factor, b.destination_rgb_blend_factor, b.destination_alpha_blend_factor,
				 b.write_mask);

		layout_said = 1;
		for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES && n < (int)sizeof(line) - 40; i++) {
			if (shape->vertex_attribute[i].format != 0) {
				const unsigned buffer = shape->vertex_attribute[i].buffer_index;

				n += snprintf(line + n, sizeof(line) - n, " a%u fmt %u off %u buf %u stride %u;", i,
					      shape->vertex_attribute[i].format, shape->vertex_attribute[i].offset, buffer,
					      buffer < RWA_MAX_VERTEX_BUFFERS ? shape->vertex_layout[buffer].stride : 0);
			}
		}
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}

	unsigned configured = 0, highest = 0;

	for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
		if (shape->vertex_attribute[i].format != 0) {
			configured++;
			if (shape->vertex_attribute[i].buffer_index > highest) {
				highest = shape->vertex_attribute[i].buffer_index;
			}
		}
	}
	if (configured > 0) {
		rwm_print("this pipeline's vertex descriptor configures %u attribute%s, the highest from buffer index %u\n",
			  configured, configured == 1 ? "" : "s", highest);
	}
}

/* The same, for what the LIBRARY made of it: four distinct lines in that whole boot. */
extern "C" void
rwm_blend_result(unsigned enabled_mask, unsigned write_masks)
{
	static struct { unsigned enabled, masks; unsigned long times; } seen[16];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = nullptr;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].enabled == enabled_mask && seen[i].masks == write_masks) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == nullptr && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].enabled = enabled_mask;
		seen[nseen].masks = write_masks;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == nullptr) {
		times = &beyond;
	}
	if (!due(*times)) {
		return;
	}
	rwm_print("blend: the library blends attachments 0x%x and writes 0x%08x (four Gallium bits per attachment, "
		  "R 0x1 .. A 0x8)\n", enabled_mask, write_masks);
	if (*times > 1) {
		rwm_print("  ... and that is the %lu'th pipeline it made that of\n", *times);
	}
}
