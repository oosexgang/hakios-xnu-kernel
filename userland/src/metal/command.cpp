/*
 * The compute side of a command buffer's work, and its end: a dispatch run when the command buffer is
 * committed, and the work sent off to the GPU.
 */
#include "rwm.h"

#include <stdio.h>

extern "C" int
rwm_run_dispatch(const struct rwm_dispatch *d, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	const unsigned count = d->count < RWM_MAX_BUFFERS ? d->count : RWM_MAX_BUFFERS;
	/* Metal's one [[buffer(n)]] table, passed BOTH ways: `buffers` is what is bound at each index, the
	 * table the same with Metal's own offset beside it, and the library takes from each side what that
	 * side owns -- an argument buffer's encoding offset and a buffer argument's own offset come from the
	 * table, and which an index is nobody but the shader knows */
	struct rwa_buffer_bind table[RWM_MAX_BUFFERS] = {};

	for (unsigned i = 0; i < count; i++) {
		table[i].buffer = d->buffers[i];
		table[i].offset = d->offsets[i];
		if (d->buffers[i] == nullptr && d->bound[i]) {
			rwm_print("buffer %u of this dispatch did not come from the compiler library, so it cannot be "
				  "bound\n", i);
		}
	}

	struct rwa_bindings bindings = {};

	bindings.buffers = const_cast<struct rwa_buffer **>(d->buffers);
	bindings.buffer_count = count;
	bindings.textures = const_cast<struct rwa_texture **>(d->textures);
	bindings.texture_count = d->texture_count;
	bindings.samplers = const_cast<struct rwa_sampler **>(d->samplers);
	bindings.sampler_count = d->sampler_count;

	/* rwa_dispatch_buffers when the library has it, the same call with Metal's offsets as well; a
	 * library too old to have it is used through rwa_dispatch, and then a kernel with a set is refused
	 * by name there and every offset is read as zero -- what an older library did in any case */
	int rc = air->dispatch_buffers != nullptr
		    ? air->dispatch_buffers(d->compute, &bindings, table, count, d->groups, d->size, why, why_size)
		    : air->dispatch(d->compute, &bindings, d->groups, d->size, why, why_size);

	return rc != 0 ? -1 : 0;
}

extern "C" int
rwm_submit(struct rwa_device *device, bool finish, uint64_t *seq, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();

	if (!finish && air->flush_seq != nullptr && air->wait_seq != nullptr &&
	    (*seq = air->flush_seq(device)) != 0) {
		return 0;               /* sent */
	}
	if (air->finish != nullptr && air->finish(device, why, why_size) != 0) {
		return -1;
	}
	return 0;
}
