/*
 * A blit encoder's work, run when its command buffer is committed: copies between textures and
 * buffers, a fill, a pass that only clears, a texture's mipmaps -- each one call of the library's.
 */
#include "rwm.h"

#include <stdio.h>
#include <string.h>

/* a copy as its encoder records it: what it copies from is copied in first when it is another process's surface, and
 * read in place counted in use; what it copies into is drawn, as a draw's target is */
extern "C" void
rwm_blit_record(struct rwm_work *w, const struct rwm_blit *blit, void *from, struct rwm_texture *from_core, void *to,
		struct rwm_texture *to_core)
{
	memset(w, 0, sizeof(*w));
	w->kind = RWM_WORK_BLIT;
	w->blit = *blit;
	rwm_work_keep(w, from);
	rwm_work_keep(w, to);
	if (from_core != nullptr) {
		w->copy_in[w->copy_in_count++] = from_core;
		if (rwm_texture_in_place(from_core) != 0) {
			w->sampled[w->sampled_count++] = rwm_texture_in_place(from_core);
		}
	}
	if (to_core != nullptr) {
		w->targets[w->target_count++] = to_core;
		w->target_surface[0] = rwm_texture_in_place(to_core);
		w->target_surface_count = 1;
	}
}

extern "C" int
rwm_run_blit(const struct rwm_blit *blit, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();

	if (air->texture_copy == nullptr || air->buffer_copy == nullptr || air->buffer_fill == nullptr) {
		snprintf(why, why_size, "the compiler library has no copies (rwa_texture_copy)");
		return -1;
	}
	switch (blit->kind) {
	case RWM_BLIT_TEXTURE_TO_TEXTURE:
		return air->texture_copy(blit->to_texture, blit->to_level, blit->to_slice, blit->to_origin,
					 blit->from_texture, blit->from_level, blit->from_slice, blit->from_origin,
					 blit->size, why, why_size) == 0 ? 0 : -1;
	case RWM_BLIT_BUFFER_TO_BUFFER:
		return air->buffer_copy(blit->to_buffer, blit->to_offset, blit->from_buffer, blit->from_offset,
					blit->bytes, why, why_size) == 0 ? 0 : -1;
	case RWM_BLIT_BUFFER_TO_TEXTURE:
		return air->texture_write(blit->to_texture, blit->to_level, blit->to_slice, blit->to_origin,
					  blit->size, blit->from_contents + blit->from_offset, blit->row_bytes,
					  why, why_size) == 0 ? 0 : -1;
	case RWM_BLIT_TEXTURE_TO_BUFFER:
		return air->texture_read(blit->from_texture, blit->from_level, blit->from_slice,
					 blit->from_origin, blit->size, blit->to_contents + blit->to_offset,
					 blit->row_bytes, why, why_size) == 0 ? 0 : -1;
	case RWM_BLIT_CLEAR:
		if (air->render_clear == nullptr) {
			return 0;
		}
		return air->render_clear(blit->device, blit->colors, blit->color_count, blit->clear_color,
					 blit->depth, blit->clear_depth, blit->clear_stencil, why,
					 why_size) == 0 ? 0 : -1;
	case RWM_BLIT_MIPMAPS:
		return air->texture_mipmaps == nullptr ||
		       air->texture_mipmaps(blit->to_texture, why, why_size) == 0 ? 0 : -1;
	case RWM_BLIT_FILL:
		return air->buffer_fill(blit->to_buffer, blit->to_offset, blit->bytes, blit->fill_value, why,
					why_size) == 0 ? 0 : -1;
	}
	return 0;
}
