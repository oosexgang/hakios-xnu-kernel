/*
 * Where a frame's time goes, as kept lines every five seconds ("rwm perf", "rwm waits", "rwm making",
 * "rwm batches", "rwm draw us"): what the window server's frame rate is made of, measured rather than
 * guessed. The counters are added to where the work is (the plug-in and the core alike); the formats of
 * the lines are kept to the character. And the boot arguments, read once.
 */
#include "rwm.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

struct rwm_perf rwm_perf;

namespace {

void
say(const char *line)
{
	sysctlbyname("kern.redwood.say", NULL, NULL, (void *)line, strlen(line));
}

/* one of the compiler library's profile calls, if it is loaded and has it (not part of the contract) */
void *
profile_call(const char *name)
{
	void *lib = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_NOLOAD);

	return lib != nullptr ? dlsym(lib, name) : nullptr;
}

} // namespace

extern "C" const char *
rwm_boot_args(void)
{
	static char arguments[1024];
	static int read;

	if (!read) {
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
			arguments[0] = '\0';
		}
		arguments[sizeof(arguments) - 1] = '\0';
		read = 1;
	}
	return arguments;
}

extern "C" bool
rwm_boot_arg(const char *text)
{
	return strstr(rwm_boot_args(), text) != nullptr;
}

extern "C" uint64_t
rwm_thread_cpu_ns(void)
{
	thread_basic_info_data_t info;
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	mach_port_t self = mach_thread_self();
	uint64_t ns = 0;

	if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
		ns = (uint64_t)(info.user_time.seconds + info.system_time.seconds) * 1000000000ull +
		     (uint64_t)(info.user_time.microseconds + info.system_time.microseconds) * 1000ull;
	}
	mach_port_deallocate(mach_task_self(), self);
	return ns;
}

extern "C" uint64_t
rwm_ns(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom;
}

extern "C" void
rwm_perf_report(uint64_t now)
{
	struct rwm_perf &p = rwm_perf;
	char line[320];
	const uint64_t span = now - p.since, cbs = p.cbs ? p.cbs : 1;

	if (p.since == 0) {
		p.since = now;
		return;
	}
	if (span < 5000000000ull) {
		return;
	}
	snprintf(line, sizeof(line), "rwm perf %llus: %llu cb (%.1f/s), %llu draws, cpu %.1f ms/cb, "
		 "gpu wait %.1f ms/cb, worst cb %.1f ms, %llu pipelines in %.0f ms; recording %.1f, between %.1f ms/cb; "
		 "%llu copied in, %llu KB, %.1f ms",
		 span / 1000000000ull, p.cbs, p.cbs * 1e9 / span, p.draws, p.encode / 1e6 / cbs, p.wait / 1e6 / cbs,
		 p.worst / 1e6, p.pipes, p.compile / 1e6, p.recording / 1e6 / cbs, p.between / 1e6 / cbs, p.copyins,
		 p.copyin_bytes >> 10, p.copyin_ns / 1e6);
	say(line);
	snprintf(line, sizeof(line), "rwm waits: queue full x%llu %.0f ms, until processed x%llu %.0f ms, "
		 "until completed x%llu %.0f ms; the worker %.1f ms a buffer", p.queue_waits, p.queue_ns / 1e6,
		 p.processed_waits, p.processed_ns / 1e6, p.completed_waits, p.completed_ns / 1e6,
		 p.worker_cbs ? p.worker_ns / 1e6 / p.worker_cbs : 0.0);
	{
		size_t at = strlen(line);

		snprintf(line + at, sizeof(line) - at, " (running %.1f of them)",
			 p.worker_cbs ? p.worker_cpu / 1e6 / p.worker_cbs : 0.0);
	}
	say(line);
	snprintf(line, sizeof(line), "rwm making: %llu buffers, %.1f ms each from made to committed, the maker running %.1f of them",
		 p.made_cbs, p.made_cbs ? p.made_ns / 1e6 / p.made_cbs : 0.0, p.made_cbs ? p.made_cpu / 1e6 / p.made_cbs : 0.0);
	say(line);

	static void (*profile)(uint64_t *);
	static void (*submits)(uint64_t *);
	static int looked;

	if (!looked) {
		looked = 1;
		profile = (void (*)(uint64_t *))profile_call("rwa_draw_profile");
		submits = (void (*)(uint64_t *))profile_call("rwa_submit_profile");
	}
	if (submits != nullptr) {
		uint64_t sb[5] = { 0, 0, 0, 0, 0 };

		submits(sb);
		snprintf(line, sizeof(line), "rwm batches: %llu submitted, %llu us each; %llu blend shaders compiled; "
			 "%llu shader variants compiled in %llu ms", sb[0], sb[0] ? sb[1] / 1000 / sb[0] : 0, sb[2], sb[3],
			 sb[4] / 1000000);
		say(line);
	}
	if (profile != nullptr) {
		uint64_t d[7];

		profile(d);
		if (d[6] != 0) {
			snprintf(line, sizeof(line), "rwm draw us: check %llu, bindcheck %llu, state %llu, "
				 "resources %llu, draw_vbo %llu, unbind %llu (%llu draws)",
				 d[0] / 1000 / d[6], d[1] / 1000 / d[6], d[2] / 1000 / d[6], d[3] / 1000 / d[6],
				 d[4] / 1000 / d[6], d[5] / 1000 / d[6], d[6]);
			say(line);
		}
	}
	memset(&p, 0, sizeof(p));
	p.since = now;
}
