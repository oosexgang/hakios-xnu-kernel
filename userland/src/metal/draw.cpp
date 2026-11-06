/*
 * A draw, run when its command buffer is committed: Metal's tables made into the library's rwa_draw
 * (tools/mesa/air/air_metal.h) and drawn.
 */
#include "rwm.h"

#include <stdio.h>

static_assert(RWM_MAX_BUFFERS <= 32, "the stream mask is 32 bits");

extern "C" int
rwm_run_draw(const struct rwm_draw *d, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	struct rwa_buffer *vertex_buffers[RWM_MAX_BUFFERS] = {};
	struct rwa_vertex_stream vertex_streams[RWM_MAX_BUFFERS] = {};
	/* and Metal's own table for each stage: the same entries with the OFFSET beside each, which
	 * rwa_bindings has nowhere to put. One array for both of the things an offset means -- which
	 * encoding of an argument buffer, and which block of a buffer argument -- because Metal has one
	 * call behind both. */
	struct rwa_buffer_bind vertex_table[RWM_MAX_BUFFERS] = {};
	struct rwa_buffer_bind fragment_table[RWM_MAX_BUFFERS] = {};
	const unsigned vertex_count = d->vertex_count_bound < RWM_MAX_BUFFERS ? d->vertex_count_bound : RWM_MAX_BUFFERS;
	const unsigned fragment_count = d->fragment_count_bound < RWM_MAX_BUFFERS ? d->fragment_count_bound
										  : RWM_MAX_BUFFERS;

	if (air->render_draw == nullptr) {
		snprintf(why, why_size, "this compiler library can compute but not yet draw");
		return -1;
	}
	/*
	 * Metal's one vertex buffer table, split in two by the pipeline's own mask: an index the vertex
	 * descriptor fetches attributes from is a STREAM, with its offset, and every other index is a
	 * [[buffer(n)]] ARGUMENT of the vertex stage. The library refuses a descriptor that claims an index
	 * the module also reads as a buffer (air_metal.h), so the two sets cannot overlap and this split
	 * cannot drop a binding.
	 */
	for (unsigned i = 0; i < vertex_count; i++) {
		const bool stream = (d->stream_mask & (1u << i)) != 0;

		/* Metal's own offset, for whichever of the other two things index i is: an argument buffer
		 * (which encoding to read) or a buffer argument (which block); a stream's offset is the
		 * stream array's, and an index is never both */
		if (!stream) {
			vertex_table[i].buffer = d->vertex[i];
			vertex_table[i].offset = d->vertex_offset[i];
		}
		if (i < RWA_MAX_VERTEX_BUFFERS && stream) {
			vertex_streams[i].buffer = d->vertex[i];
			vertex_streams[i].offset = d->vertex_offset[i];
			if (d->vertex_bound[i] && d->vertex[i] == nullptr) {
				snprintf(why, why_size, "the buffer bound as vertex stream %u did not come from the "
					 "compiler library, so there is nothing to fetch vertices out of", i);
				return -1;
			}
			continue;
		}
		vertex_buffers[i] = d->vertex[i];
	}
	if (d->stream_count > RWM_MAX_BUFFERS) {
		snprintf(why, why_size, "this pipeline fetches from %u vertex stream slots and this driver's table "
			 "holds %u", d->stream_count, RWM_MAX_BUFFERS);
		return -1;
	}
	for (unsigned i = 0; i < fragment_count; i++) {
		fragment_table[i].buffer = d->fragment[i];
		fragment_table[i].offset = d->fragment_offset[i];
		if (d->fragment_bound[i] && d->fragment[i] == nullptr) {
			snprintf(why, why_size, "the buffer bound at fragment buffer index %u did not come from the "
				 "compiler library, so there is nothing to bind", i);
			return -1;
		}
	}
	/*
	 * The index buffer, by the same rule as every other resource here: it has to be one of the
	 * library's, because what the library needs is the Gallium resource behind it. NULL here would be
	 * a draw the library reads as non-indexed -- the vertices in the wrong order, which is the one
	 * outcome worse than an error -- so it is caught here and not left to the library.
	 */
	if (d->index_bound && d->index_buffer == nullptr) {
		snprintf(why, why_size, "the index buffer of this draw did not come from the compiler library, so "
			 "there is nothing to read the indices out of");
		return -1;
	}

	struct rwa_draw draw = {};

	draw.color = d->color;
	draw.color_count = d->color_count;
	draw.clear_color = d->clear_color;
	/* the depth attachment, its two load actions, the state that was bound, and the depth range of the
	 * viewport -- where the quad's own depth comes from (without one, the contract's [0, 1]) */
	draw.depth = d->depth;
	draw.clear_depth = d->clear_depth;
	draw.clear_stencil = d->clear_stencil;
	draw.depth_stencil = d->depth_stencil;
	draw.stencil_reference[0] = d->stencil_reference[0];
	draw.stencil_reference[1] = d->stencil_reference[1];
	draw.depth_range = d->depth_range;
	draw.viewport = d->viewport;
	draw.scissor = d->scissor;
	draw.vertex.buffers = vertex_buffers;
	draw.vertex.buffer_count = vertex_count;
	/* the streams the layout fetches from, at Metal's own indices with Metal's own offsets; the
	 * count is the highest index the pipeline fetches from plus one */
	draw.vertex_streams = vertex_streams;
	draw.vertex_stream_count = d->stream_count;
	draw.fragment.buffers = const_cast<struct rwa_buffer **>(d->fragment);
	draw.fragment.buffer_count = fragment_count;
	draw.fragment.textures = const_cast<struct rwa_texture **>(d->textures);
	draw.fragment.texture_count = d->texture_count;
	draw.fragment.samplers = const_cast<struct rwa_sampler **>(d->samplers);
	draw.fragment.sampler_count = d->sampler_count;
	draw.primitive = d->primitive;
	draw.vertex_start = d->vertex_start;
	draw.vertex_count = d->vertex_count;
	draw.index_buffer = d->index_buffer;
	draw.index_type = d->index_type;
	draw.index_buffer_offset = d->index_offset;
	draw.base_vertex = d->base_vertex;
	draw.instance_count = d->instance_count;
	draw.base_instance = d->base_instance;
	/* Metal's own table for each stage, at Metal's own indices: the library takes an argument buffer's
	 * whole binding from here and a buffer argument's offset only, and checks that where both name a
	 * buffer they name the same one */
	draw.vertex_buffer_table = vertex_table;
	draw.vertex_buffer_table_count = vertex_count;
	draw.fragment_buffer_table = fragment_table;
	draw.fragment_buffer_table_count = fragment_count;

	if (air->render_draw(d->render, &draw, why, why_size) != 0) {
		return -2;
	}
	return 0;
}
