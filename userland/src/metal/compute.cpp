/*
 * A compute encoder's state, and its dispatches recorded from it (the plug-in hands over each object with the
 * library's under it). As with the render encoder, nothing runs here: what a dispatch reads is what was bound when it
 * was encoded, recorded into a work that src/metal/run.cpp runs at -commit.
 */
#include "rwm.h"

#include <string.h>

extern "C" void
rwm_compute_set_pipeline(struct rwm_compute_state *s, void *object, struct rwa_compute *compute)
{
	s->pipeline = object;
	s->compute = compute;
}

/*
 * The offset is recorded and judged at the dispatch, as the render encoder's is. An argument buffer is bound through
 * this call with an offset of its own -- which encoding in that buffer to read -- and a plain buffer argument with one
 * is an application packing several blocks into an allocation, which is served: the library takes a `constant`
 * binding at any offset and refuses a `device` one the GPU cannot fetch from, by name.
 */
extern "C" void
rwm_compute_set_buffer(struct rwm_compute_state *s, size_t index, void *object, struct rwa_buffer *buffer,
		       size_t offset)
{
	if (index >= RWM_MAX_BUFFERS) {
		rwm_print("buffer index %lu is past this driver's %d\n", (unsigned long)index, RWM_MAX_BUFFERS);
		return;
	}
	s->buffers[index] = rwm_bound_buffer{ object, buffer, offset };
	if (index + 1 > s->buffer_count) {
		s->buffer_count = (unsigned)index + 1;
	}
}

/* Metal's rule: this moves a buffer already bound at that index, so with nothing bound there is nothing to move --
 * binding nothing here would add an empty slot to the dispatch and a complaint about a buffer nobody asked for */
extern "C" void
rwm_compute_set_buffer_offset(struct rwm_compute_state *s, size_t index, size_t offset)
{
	if (index >= RWM_MAX_BUFFERS) {
		rwm_print("buffer index %lu is past this driver's %d\n", (unsigned long)index, RWM_MAX_BUFFERS);
		return;
	}
	if (s->buffers[index].object != nullptr) {
		s->buffers[index].offset = offset;
	}
}

extern "C" void
rwm_compute_set_texture(struct rwm_compute_state *s, size_t index, void *object, struct rwm_texture *core)
{
	if (index >= RWM_MAX_BUFFERS) {
		rwm_print("texture index %lu is past this driver's %d\n", (unsigned long)index, RWM_MAX_BUFFERS);
		return;
	}
	s->textures[index] = rwm_bound_texture{ object, core };
	if (index + 1 > s->texture_count) {
		s->texture_count = (unsigned)index + 1;
	}
}

extern "C" void
rwm_compute_set_sampler(struct rwm_compute_state *s, size_t index, void *object, struct rwa_sampler *sampler)
{
	if (index >= RWM_MAX_BUFFERS) {
		rwm_print("sampler index %lu is past this driver's %d\n", (unsigned long)index, RWM_MAX_BUFFERS);
		return;
	}
	s->sampler_object[index] = object;
	s->samplers[index] = sampler;
	if (index + 1 > s->sampler_count) {
		s->sampler_count = (unsigned)index + 1;
	}
}

/* Metal lets a binding narrow the sampler's lod range; the library takes the range when the sampler is made, so
 * honouring it here would need a second sampler object -- until it does, saying nothing would sample the wrong level */
extern "C" void
rwm_compute_lod_clamp(struct rwm_compute_state *s, float lod_min, float lod_max)
{
	if (!s->complained && (lod_min != 0.0f || lod_max < 1000.0f)) {
		rwm_print("a per-binding lod clamp (%g..%g) is not implemented; the "
			  "sampler's own range is used instead\n", (double)lod_min, (double)lod_max);
		s->complained = true;
	}
}

extern "C" bool
rwm_compute_dispatch(struct rwm_compute_state *s, const unsigned long groups[3], const unsigned long size[3],
		     struct rwm_work *w)
{
	struct rwm_dispatch *d = &w->dispatch;

	if (s->pipeline == nullptr || s->complained) {
		rwm_print("this dispatch has %s and is dropped\n",
			  s->pipeline == nullptr ? "no pipeline state" : "a binding this driver refused");
		return false;
	}
	memset(w, 0, sizeof(*w));
	w->kind = RWM_WORK_DISPATCH;
	w->pipeline = s->pipeline;
	rwm_work_keep(w, s->pipeline);
	d->compute = s->compute;
	d->count = s->buffer_count;
	for (unsigned i = 0; i < s->buffer_count; i++) {
		d->buffers[i] = s->buffers[i].buffer;
		d->offsets[i] = s->buffers[i].offset;
		d->bound[i] = s->buffers[i].object != nullptr;
		rwm_work_keep(w, s->buffers[i].object);
	}
	d->texture_count = s->texture_count;
	for (unsigned i = 0; i < s->texture_count; i++) {
		d->textures[i] = s->textures[i].core != nullptr ? rwm_texture_mesa(s->textures[i].core) : nullptr;
		rwm_work_keep(w, s->textures[i].object);
	}
	d->sampler_count = s->sampler_count;
	for (unsigned i = 0; i < s->sampler_count; i++) {
		d->samplers[i] = s->samplers[i];
		rwm_work_keep(w, s->sampler_object[i]);
	}
	for (unsigned i = 0; i < 3; i++) {
		d->groups[i] = (unsigned)groups[i];
		d->size[i] = (unsigned)size[i];
	}
	return true;
}

/* Metal counts threads here and threadgroups above; a grid that does not divide evenly is refused rather than rounded,
 * because rounding runs threads the application did not ask for */
extern "C" bool
rwm_compute_dispatch_threads(struct rwm_compute_state *s, const unsigned long threads[3], const unsigned long size[3],
			     struct rwm_work *w)
{
	unsigned long groups[3];

	if (size[0] == 0 || size[1] == 0 || size[2] == 0 || threads[0] % size[0] != 0 || threads[1] % size[1] != 0 ||
	    threads[2] % size[2] != 0) {
		rwm_print("%lux%lux%lu threads do not divide into groups of "
			  "%lux%lux%lu, and this driver does not have the partial-group support "
			  "that would need\n", threads[0], threads[1], threads[2], size[0], size[1], size[2]);
		return false;
	}
	for (unsigned i = 0; i < 3; i++) {
		groups[i] = threads[i] / size[i];
	}
	return rwm_compute_dispatch(s, groups, size, w);
}
