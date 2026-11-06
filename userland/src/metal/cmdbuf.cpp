/*
 * A command buffer's life, as far as it is not Metal's objects: made, committed -- run here, or handed to the driver's
 * worker --, run, scheduled, completed, and the waits for each. Metal's own rule is kept: the recorded commands run
 * at -commit and not when they were encoded, in the order they were encoded.
 */
#include "rwm.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

namespace {

/*
 * Boot-arg rwm_async=1: the driver's own thread (src/metal/worker.cpp). Off unless asked since 27 October: with the
 * worker, frames drew parts of other frames (the "breakcore": runs 236, 240, 244, the in-use counts and the waits for
 * it notwithstanding -- and runs 415 and 416, the old plug-in and the new alike), without it they are clean and no
 * slower -- 88% of busy frames within 18 ms against 74% (runs 243, 236). The GPU stays asynchronous: the next frame is
 * built while it draws this one.
 */
bool
async_wanted()
{
	static int wanted = -1;

	if (wanted < 0) {
		const struct rwm_air *air = rwm_air();

		wanted = rwm_boot_arg("rwm_async=1") && air->flush_seq != nullptr && air->surface_seq != nullptr;
	}
	return wanted;
}

/*
 * Boot-arg rwm_finish=1: every command buffer waits for the GPU to be idle before it counts as done, the display's
 * wait for the surface's last job then having nothing left to wait for. A test (27 October): if the strips go with
 * it, the job the display waits for is not the one that finishes the picture.
 */
bool
finish_wanted()
{
	static int wanted = -1;

	if (wanted < 0) {
		wanted = rwm_boot_arg("rwm_finish=1");
	}
	return wanted;
}

/*
 * The surfaces a buffer samples in place, counted in use from -commit until the GPU has read them, as Apple's GPU
 * drivers count them (src/metal/surfaces.cpp). Boot-arg rwm_inuse=0: not counted, as until 27 October.
 */
bool
inuse_wanted()
{
	static int wanted = -1;

	if (wanted < 0) {
		wanted = !rwm_boot_arg("rwm_inuse=0");
	}
	return wanted;
}

/* (the black snapshots: the first command buffer that draws into each surface, what it does) */
void
say_first_drawn(struct rwm_work *const *works, unsigned count, const char *(*label_of)(void *))
{
	static uint32_t seen_ids[64];
	static unsigned seen_n;
	uint32_t sid0 = 0;
	unsigned draws = 0, clears = 0, k;
	const char *label = "";
	float cc[4] = { 0, 0, 0, 0 };

	for (unsigned n = 0; n < count; n++) {
		const struct rwm_work *w = works[n];
		const uint32_t sid = w->kind == RWM_WORK_DRAW && w->target_surface_count != 0 ? w->target_surface[0] : 0;

		if (sid == 0 || (sid0 != 0 && sid != sid0)) {
			continue;
		}
		sid0 = sid;
		draws++;
		if (w->draw.clear_color != nullptr) {
			clears++;
			memcpy(cc, w->clear_color, sizeof(cc));
		}
		if (*label == 0) {
			const char *l = label_of != nullptr ? label_of(w->pipeline) : nullptr;

			label = l != nullptr ? l : "?";
		}
	}
	for (k = 0; sid0 != 0 && k < seen_n && seen_ids[k] != sid0; k++) {
	}
	if (sid0 != 0 && k == seen_n && seen_n < 64) {
		char line[200];

		seen_ids[seen_n++] = sid0;
		snprintf(line, sizeof(line), "!rwm surface %u first drawn: %u draws (%u clearing, to %.2f %.2f %.2f %.2f), first %s",
			 sid0, draws, clears, cc[0], cc[1], cc[2], cc[3], label);
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}
}

} // namespace

extern "C" bool
rwm_copyin_late(void)
{
	static int late = -1;

	if (late < 0) {
		late = rwm_boot_arg("rwm_copyin_late=1");
	}
	return late;
}

extern "C" void
rwm_command_begin(struct rwm_command *c)
{
	memset(c, 0, sizeof(*c));
	c->born = rwm_ns();
	c->born_cpu = rwm_thread_cpu_ns();
	c->status = RWM_CB_ENQUEUED;    /* nothing is pending yet */
}

extern "C" bool
rwm_command_commit(struct rwm_command *c, struct rwm_work *const *works, unsigned count,
		   const char *(*label)(void *pipeline))
{
	static __thread bool bound;

	if (!rwm_worker_on()) {
		rwm_perf.made_cbs++;
		rwm_perf.made_ns += rwm_ns() - c->born;
		rwm_perf.made_cpu += rwm_thread_cpu_ns() - c->born_cpu;
	}
	if (!bound) {
		bound = true;
		if (!rwm_worker_on()) {
			rwm_bind_big(1);
		}
	}
	if (inuse_wanted()) {
		rwm_works_sampled(works, count, &c->sampled);
	}
	if (!async_wanted()) {
		return false;
	}
	c->status = RWM_CB_COMMITTED;
	c->drawn.count = 0;
	if (!rwm_copyin_late()) {
		/* the surfaces this buffer samples, copied in now, as they are when it is committed: on the worker, later,
		 * the process drawing one may already be halfway into its next picture -- icons cut in two and widgets
		 * drawn as an "H" while paging, gone with rwm_async=0 (runs 234, 236, 242) */
		rwm_works_copy_in(works, count);
	}
	say_first_drawn(works, count, label);
	rwm_works_drawn(works, count, &c->drawn);       /* (pending for the display) */
	c->queued = true;
	return true;
}

extern "C" int
rwm_command_run(struct rwm_command *c, struct rwa_device *device, struct rwm_work *const *works, unsigned count,
		char *why, size_t why_size)
{
	const uint64_t began = rwm_ns();
	uint64_t waited = 0;
	int ran;

	rwm_perf.recording += began - c->born;
	if (rwm_perf.last_end != 0 && began > rwm_perf.last_end) {
		rwm_perf.between += began - rwm_perf.last_end;
	}
	c->status = RWM_CB_COMMITTED;
	/* its copies in -- here, unless the driver's worker made them at -commit --, the draws, dispatches and copies in
	 * order, the work sent off, the surfaces copied back */
	ran = rwm_run_works(device, works, count, !(rwm_worker_on() && !rwm_copyin_late()), finish_wanted(), &c->seq,
			    &waited, why, why_size);
	/* submitted; the fence is what makes it completed */
	c->status = ran == 0 ? RWM_CB_SCHEDULED : RWM_CB_ERROR;

	const uint64_t now = rwm_ns(), took = now - began;

	rwm_perf.last_end = now;
	rwm_perf.cbs++;
	rwm_perf.wait += waited;
	rwm_perf.encode += took - waited;
	if (took > rwm_perf.worst) {
		rwm_perf.worst = took;
	}
	rwm_perf_report(now);
	return ran;
}

extern "C" void
rwm_command_processed(struct rwm_command *c)
{
	rwm_drawn_done(&c->drawn, c->seq);
	rwm_worker_mark(&c->processed);
}

extern "C" void
rwm_command_wait_processed(struct rwm_command *c)
{
	if (!c->queued || rwm_worker_on()) {
		return;
	}
	rwm_worker_wait(&c->processed);
}

/*
 * Scheduled once it is in the worker's queue: the worker runs buffers in the order committed, and whatever reads what
 * one draws waits for it on its own -- the display for the surface's jobs, a lock of the surface likewise,
 * -waitUntilCompleted for the GPU. Waiting here for the worker to have recorded every draw (as until 26 October) held
 * CoreAnimation's next frame for the whole of a heavy one's recording: 20 of 53 late frames began within 2 ms of the
 * worker's finishing the buffer before (run 183). Boot-arg rwm_sched_wait=1: the old wait.
 */
extern "C" void
rwm_command_wait_scheduled(struct rwm_command *c)
{
	static int wait = -1;

	if (wait < 0) {
		wait = rwm_boot_arg("rwm_sched_wait=1");
	}
	if (wait || !c->queued) {
		rwm_command_wait_processed(c);
	}
}

/* The fence is the whole point: a dispatch that the GPU does not finish must not read as a result. air_run learned
 * that the expensive way (docs/air.md section 5). */
extern "C" int
rwm_command_wait_completed(struct rwm_command *c, struct rwa_device *device, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();

	if (c->queued && !c->processed && !rwm_worker_on()) {
		rwm_command_wait_processed(c);
	}
	if (c->status == RWM_CB_ERROR || c->status == RWM_CB_COMPLETED) {
		return 0;
	}
	if (c->seq != 0) {
		const uint64_t from = rwm_ns();

		/* its own last job, and nothing after it */
		if (air->wait_seq(c->seq, 5000000000ll) != 0) {
			rwm_print("THE GPU DID NOT FINISH job %llu\n", (unsigned long long)c->seq);
		}
		rwm_perf.completed_waits++;
		rwm_perf.completed_ns += rwm_ns() - from;
		c->status = RWM_CB_COMPLETED;
		return 0;
	}
	if (air->finish(device, why, why_size) != 0) {
		rwm_print("THE GPU DID NOT FINISH: %s\n", why);
		c->status = RWM_CB_ERROR;
		return -1;
	}
	c->status = RWM_CB_COMPLETED;   /* the fence has been paid, and only now */
	return 0;
}
