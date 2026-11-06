/*
 * A command buffer's work, run when it is committed -- Metal's rule, kept: the recorded commands run here and not
 * when they were encoded, in the order they were encoded.
 */
#include "rwm.h"

#include <string.h>

namespace {

/* the first, second, fourth... time a site says it, with the count after the first (the plug-in's RW_AGAIN) */
bool
again(unsigned long &times)
{
	return (++times & (times - 1)) == 0;
}

void
say_again(unsigned long &times, const char *what, const char *why)
{
	if (again(times)) {
		rwm_print("%s: %s\n", what, why);
		if (times > 1) {
			rwm_print("  ... and that is the %lu'th time\n", times);
		}
	}
}

/* the targets of a command that are over a surface, once each */
void
note_drawn(const struct rwm_work *w, struct rwm_texture **drawn, unsigned &ndrawn, unsigned room)
{
	for (unsigned i = 0; i < w->target_count; i++) {
		struct rwm_texture *t = w->targets[i];
		unsigned k;

		if (t == nullptr || !rwm_texture_has_surface(t)) {
			continue;
		}
		for (k = 0; k < ndrawn && drawn[k] != t; k++) {
		}
		if (k == ndrawn && ndrawn < room) {
			drawn[ndrawn++] = t;
		}
	}
}

} // namespace

/* a texture over another process's surface brought up to date, counted in rwm_perf (also -[RedwoodMetalTexture
 * redwoodCopyInNow]) */
extern "C" void
rwm_copy_in(struct rwm_texture *texture)
{
	static unsigned long times;
	char why[256] = { 0 };
	size_t bytes = 0;
	const uint64_t began = rwm_ns();

	if (rwm_texture_copy_in(texture, &bytes, why, sizeof(why)) != 0) {
		say_again(times, "copy-in: the surface could not be written into its texture", why);
	}
	if (bytes != 0) {
		rwm_perf.copyins++;
		rwm_perf.copyin_bytes += bytes;
		rwm_perf.copyin_ns += rwm_ns() - began;
	}
}

/* at -commit with the driver's worker: the surfaces the works sample copied in as they are now */
extern "C" void
rwm_works_copy_in(struct rwm_work *const *works, unsigned count)
{
	for (unsigned n = 0; n < count; n++) {
		for (unsigned i = 0; i < works[n]->copy_in_count; i++) {
			rwm_copy_in(works[n]->copy_in[i]);
		}
	}
}

extern "C" int
rwm_run_works(struct rwa_device *device, struct rwm_work *const *works, unsigned count, bool copy, bool finish,
	      uint64_t *seq, uint64_t *waited_ns, char *why, size_t why_size)
{
	struct rwm_texture *drawn[64];
	unsigned ndrawn = 0;

	*waited_ns = 0;
	for (unsigned n = 0; n < count; n++) {
		struct rwm_work *w = works[n];

		for (unsigned i = 0; copy && i < w->copy_in_count; i++) {
			rwm_copy_in(w->copy_in[i]);
		}
		switch (w->kind) {
		case RWM_WORK_BLIT:
			if (rwm_run_blit(&w->blit, why, why_size) != 0) {
				static unsigned long times;

				say_again(times, "a copy skipped", why);
			} else {
				note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			}
			why[0] = '\0';
			break;
		case RWM_WORK_DRAW: {
			int failed;

			rwm_perf.draws++;
			failed = rwm_run_draw(&w->draw, why, why_size);
			if (failed == -2) {
				rwm_print("the draw failed: %s\n", why);
			}
			if (failed != 0) {
				/*
				 * A buffer the shader is declared to read and the window server did not bind: QuartzCore's
				 * specialized uber shader keeps its configuration buffer (0) among its arguments although the
				 * constants decide it is never read. The draw is skipped and the rest of the frame drawn --
				 * failing the whole command buffer for it left every frame black (24 October).
				 */
				if (strstr(why, "reads buffer") != nullptr && strstr(why, "were bound") != nullptr) {
					static unsigned long times;

					say_again(times, "a draw skipped, the frame kept", why);
					why[0] = '\0';
					break;
				}
				return RWM_RUN_DRAW_FAILED;
			}
			note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			break;
		}
		case RWM_WORK_DISPATCH:
			if (rwm_run_dispatch(&w->dispatch, why, why_size) != 0) {
				rwm_print("the dispatch failed: %s\n", why);
				return RWM_RUN_DISPATCH_FAILED;
			}
			break;
		}
	}

	/*
	 * Sent off and NOT waited for when the kernel queues the Mali's jobs: the window server goes on to its next frame
	 * while the GPU draws this one; the display waits for the surface's last job before it shows it, the completed
	 * handlers and -waitUntilCompleted for the buffer's. A surface drawn by the copy-back road still needs the frame
	 * now, and waits.
	 */
	const uint64_t began = rwm_ns();

	if (rwm_submit(device, ndrawn != 0 || finish, seq, why, why_size) != 0) {
		rwm_print("THE GPU DID NOT FINISH the command buffer: %s\n", why);
	}
	*waited_ns = rwm_ns() - began;
	for (unsigned k = 0; k < ndrawn; k++) {
		static unsigned long times;
		char reason[256] = { 0 };

		if (rwm_texture_sync_surface(drawn[k], reason, sizeof(reason)) != 0) {
			say_again(times, "copy-back", reason);
		}
	}
	return 0;
}
