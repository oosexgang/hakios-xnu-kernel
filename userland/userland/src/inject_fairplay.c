/*
 * libredwood-fairplay.dylib: code of ours inside fairplayd.H2 (see inject.c for how it gets there).
 *
 * Why. The App Store's front page never loads, and the root of that is not in the store: an
 * AppleMediaServices thread inside appstored sits in a Mach call in _AMSFairPlayGetDeviceType
 * waiting for a reply, and seventeen promises queue behind that one wait (src/inject_store.c). The
 * reply never comes because com.apple.fairplayd.H2 starts, exits 0 at once and is restarted by
 * launchd every ten seconds -- 34 times in a six-minute session, each "Service only ran for 0
 * seconds". It does not crash; it declines to work. A daemon in that state leaves no failed lookup
 * at all, which is why the shape was invisible to the method that found apsd.
 *
 * What this does. Nothing is replaced: it only watches, through dyld's interposing (__DATA,__interpose,
 * as src/inject_app.c does), the four calls that decide whether fairplayd has anything to work with:
 *   IOServiceMatching            -- THE NAME IT LOOKS FOR, which is the one thing a driver of ours
 *                                   has to answer to; the binary is obfuscated and does not carry it
 *                                   as a plain string, so it is asked for at run time instead of guessed
 *   IOServiceGetMatchingServices -- and whether anything was found
 *   IOServiceOpen                -- the user client it wants (its entitlement names
 *                                   com_apple_driver_FairPlayIOKitUserClient) and the type
 *   IOConnectCallMethod          -- every selector it then sends, which is the minimum set a stub
 *                                   would have to implement
 * and exit(), with a walk up the frame pointers, which says where it decides to leave.
 *
 * The fault reporter is here as well: an Objective-C exception ends in abort(), and iOS answers a
 * crash in a stock daemon with "Failed to send exception EXC_CORPSE_NOTIFY" and nothing else.
 *
 * WHAT IT ANSWERED, and it says the obvious next step would have been the wrong one. Over three runs:
 *   argv[0] = /usr/sbin/fairplayd.H2, one argument, as Apple's own job plist asks for (ours matches
 *     it field for field but KeepAlive, which mkjobs.py drops; MachServices are all three there)
 *   open() of the CoreFoundation and containermanagerd feature flags, and of /usr/sbin/Info.plist
 *     -- CFBundle finding its own bundle, and nothing else. WITH THE RESULTS, which is the only way
 *     to tell "it read the flags" from "it tried and got ENOENT": the four that exist on the volume
 *     are opened successfully (Global.plist, GlobalDisclosures.plist, Domain/CoreFoundation.plist,
 *     Domain/containermanagerd.plist -> fd 3), and every -1 is a file a stock iPhone has not got
 *     either (/Library/Preferences/FeatureFlags/..., Settings.plist, Profiles.plist, and
 *     /usr/sbin/Info.plist, which no unbundled daemon has). Nothing it needs is missing from this
 *     image: the feature flags are not the reason it leaves.
 *   NOT ONE IOServiceMatching, IOServiceGetMatchingService(s), IOServiceOpen or IOConnectCallMethod
 *   no xpc_connection_create_mach_service, no sysctlbyname, no dlopen, no stat or mkdir of its own
 *   the exit handler runs, so it leaves of its own accord -- and neither exit() nor _exit() nor
 *     abort() is caught, so main simply returns
 *   and the sampler: every program counter of its ~10 ms of life, 13 to 25 samples, lies inside its
 *     own binary (0x1000d9a40 to 0x1008565ac, which is 0xd9a40 to 0x8565ac into the file). Not one
 *     sample in the shared cache.
 * So it decides with code we cannot interpose, inlining whatever syscalls it makes -- which is what
 * an obfuscated FairPlay binary would do on purpose. IT NEVER ASKS IOKIT FOR ANYTHING, so a stub of
 * com_apple_driver_FairPlayIOKit in the kernel, which was the plan after the last finding, would not
 * be reached at all: the daemon gives up before that. (Its entitlement does name
 * com_apple_driver_FairPlayIOKitUserClient, which is what made the driver look like the next step.)
 * One guess checked and killed before it cost a run: not the platform-binary flag. iOS's own
 * binaries are ad hoc signed and this kernel marks exactly those as platform binaries
 * (bsd/kern/kern_minimac.c), so fairplayd is one here.
 * Two warnings for whoever reads this next.
 * AN ENTITLEMENT NAMES WHAT A PROGRAM MAY DO, NEVER WHAT IT DOES. fairplayd's signature carries
 * com.apple.security.iokit-user-client-class = com_apple_driver_FairPlayIOKitUserClient, and on that
 * evidence a kernel driver looked like the obvious next piece of work. It never opens one. The
 * entitlement is permission, not behaviour, and one run of watching beat a day of writing.
 * AND mkjobs.py DROPS KeepAlive from the job plists it makes (ours is Apple's field for field
 * otherwise, MachServices included). Harmless here -- launchd restarts fairplayd anyway, on demand,
 * 34 times in six minutes -- but worth knowing, because a daemon that is meant to stay resident and
 * is not kept alive looks exactly like a daemon that quits, and two shapes with the same log have
 * cost this project an evening once already.
 *
 * WHAT THE SYSTEM-CALL TRACE ADDS (systrace=2 systrace_name=fairplayd.H2, which logs every BSD call
 * of one process and nothing else -- a few hundred lines rather than a flood). Its whole life, by
 * census over four of its starts: open x241, write x220, close x78, mprotect x57, stat64 x54,
 * getattrlist x45, lseek x39, pread x39, SYSCTL x33, mmap x31, fcntl x24, munmap x21, kevent_qos
 * x16, sigaction x15, thread_selfid x13, proc_info x12, fsgetpath x12, __MAC_SYSCALL x12, openat
 * x12, geteuid x12. The opens are the feature flags and its own bundle, and the writes at the end
 * are OUR OWN lines, not its: it prints nothing of its own before leaving.
 * Three things follow. It touches no IOKit node, no socket and no file of its own, so those theories
 * are closed by trace and not by inference. It calls SYSCTL by number, never sysctlbyname -- which
 * is why the hook on the name caught nothing all along, the wrong-name shape once more; the numeric
 * one is hooked now. And __mac_syscall x12 is the channel to AMFI and the sandbox, so a question
 * asked there is among the few things left that it could be deciding on.
 * Mach traps are not BSD calls and so are invisible to this trace: whatever it asks over Mach --
 * including a bootstrap check-in -- would not appear here.
 *
 * AND A LIMIT WORTH KNOWING BEFORE ANYONE TRIES AGAIN: __sysctl and __mac_syscall were hooked here
 * too, and neither fired, while the kernel's own trace counted 33 and 12 of them in the same life.
 * open() and access() fire in the same build, 1300 lines of them, so the library is plainly there.
 * The reason is the mechanism, not the names this time: dyld interposing replaces a symbol where one
 * image calls ANOTHER image's stub, and these calls are made inside libsystem itself. USER-SPACE
 * INTERPOSING CANNOT SEE A LIBSYSTEM-INTERNAL SYSCALL. For arguments to those, the instrument has to
 * be the kernel: systrace already prints the call and the return, and printing the sysctl MIB and
 * the __mac_syscall policy and request beside them is a small addition in the same place
 * (xnu/redwood.patch, systrace_report). That is the next instrument, and it wants a kernel build.
 * It mattered because this kernel's AMFI stand-in answers exactly two requests, 0x5a and 0x60, and
 * returns ENOPOLICY to every other (bsd/kern/kern_minimac.c), and a daemon refused there would be
 * refused invisibly. MEASURED, AMFI IS CLEARED, and by two roads at once.
 * The kernel's own trace, once it was taught to print the policy and the return:
 *   policy "AMFI" request 90 = 0          (0x5a, the dyld policy check, answered)
 *   policy "Sandbox" request 2 = 0, request 4 = 0
 * and not one other request, not one ENOPOLICY. (That 0x5a is also the known call to check the
 * instrument against: it is dyld's, it happens at every process start, and seeing it with its
 * return proves the trace prints both halves.)
 * And a static read of the binary agrees from the other side: fairplayd.H2 imports neither
 * __mac_syscall nor sysctl, and has no svc instruction anywhere in its text, so the twelve AMFI and
 * thirty-three sysctl calls in its trace belong to dyld, libSystem and CoreFoundation -- not to it.
 * It imports no exit either, so its departure is a return from main, which is a decision and not a
 * refusal: a MAC denial forced by the kernel kills a process, it does not give a clean exit(0).
 *
 * The next instrument should therefore be on the other side of the wall: this kernel is ours, so
 * logging the system calls of the process named fairplayd.H2 sees through the inlining that defeats
 * interposing. The other way in is to read the binary, and this project has a worked path for
 * exactly that (it is what the camera work used on CMCapture): Ghidra in ~/.local/share/ghidra,
 * imported with -noanalysis because a full analysis runs for hours, a project under
 * ~/ghidra-projects whose path has no dot in it, and ~/.local/share/ghidra/tools/dec.sh to
 * decompile by symbol with the cache's island and stub names filled in; got.py, cfstr.py and
 * callers.py beside it; "ipsw dyld" only for dump, a2s and a2f, since disass runs out of memory.
 * The window is small: offsets 0xd9a40 to 0x8565ac of /usr/sbin/fairplayd.H2, and what to look for
 * is an early return from main and the strings of whatever it compared against just before -- a
 * comparison, not a failed call, since every sampled program counter is in code Apple shipped
 * precisely so that it could not be watched from outside.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <dispatch/dispatch.h>

/* (the SDK here has no xpc/xpc.h; only this one call is wanted, and only its name is read) */
typedef void *xpc_connection_t;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <dlfcn.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

/* MobileGestalt and the entitlement reader, declared here because the SDK keeps no headers for them */
extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern CFTypeRef SecTaskCopyValueForEntitlement(void *task, CFStringRef entitlement, CFErrorRef *error);

#define CACHE_BASE 0x180000000UL

static uintptr_t slide;
static bool ours;

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* up the frame pointers, unslid, for "ipsw dyld a2s" */
static void
frames_from(uintptr_t fp, char *text, size_t size)
{
	size_t used = 0;

	text[0] = 0;
	for (unsigned i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && used + 20 < size; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(text + used, size - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;
	}
}

static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, fp = 0;
	char frames[10 * 20 + 1] = "";

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss);
	}
	frames_from(fp, frames, sizeof(frames));
	say("redwood: fairplayd: signal %d at %p, pc %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

/* ---- the four questions, and the leaving ------------------------------------------------------- */

static CFMutableDictionaryRef
watched_IOServiceMatching(const char *name)
{
	CFMutableDictionaryRef matching = IOServiceMatching(name);

	if (ours) {
		say("redwood: fairplayd: IOServiceMatching(\"%s\") -> %s\n", name != NULL ? name : "(null)",
		    matching != NULL ? "a dictionary" : "nothing");
	}
	return matching;
}

static kern_return_t
watched_IOServiceGetMatchingServices(mach_port_t port, CFDictionaryRef matching, io_iterator_t *existing)
{
	kern_return_t result = IOServiceGetMatchingServices(port, matching, existing);

	if (ours) {
		io_object_t one = existing != NULL ? IOIteratorNext(*existing) : 0;

		say("redwood: fairplayd: IOServiceGetMatchingServices -> %#x, %s\n", result,
		    one != 0 ? "something matched" : "NOTHING MATCHED");
		if (one != 0) {
			IOObjectRelease(one);
			IOIteratorReset(*existing);
		}
	}
	return result;
}

static io_service_t
watched_IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching)
{
	io_service_t service = IOServiceGetMatchingService(port, matching);

	if (ours) {
		say("redwood: fairplayd: IOServiceGetMatchingService -> %s\n", service != 0 ? "a service" : "NOTHING");
	}
	return service;
}

static kern_return_t
watched_IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect)
{
	kern_return_t result = IOServiceOpen(service, task, type, connect);

	if (ours) {
		say("redwood: fairplayd: IOServiceOpen(type %u) -> %#x\n", type, result);
	}
	return result;
}

static kern_return_t
watched_IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t input_count,
    const void *input_struct, size_t input_struct_count, uint64_t *output, uint32_t *output_count,
    void *output_struct, size_t *output_struct_count)
{
	kern_return_t result = IOConnectCallMethod(connection, selector, input, input_count, input_struct,
	    input_struct_count, output, output_count, output_struct, output_struct_count);

	if (ours) {
		say("redwood: fairplayd: IOConnectCallMethod selector %u -> %#x\n", selector, result);
	}
	return result;
}

/*
 * It leaves without ever asking IOKit for anything -- the first run of this library caught not one
 * IOServiceMatching -- so the net is wider: the files it opens, the sysctls and the libraries it
 * asks for, and every way out of a process. Whatever it looked at last is where it made up its mind.
 */
static int
watched_open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if ((flags & O_CREAT) != 0) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	int fd = open(path, flags, mode);

	/*
	 * The RESULT, not only the call: "it read the feature flags" and "it tried to and got ENOENT"
	 * are the same line without it, and completely different findings. Every watcher here says what
	 * came back.
	 */
	if (ours) {
		say("redwood: fairplayd: open(\"%s\") -> %d%s\n", path != NULL ? path : "(null)", fd,
		    fd < 0 ? (errno == ENOENT ? " (no such file)" : " (refused)") : "");
	}
	return fd;
}

/*
 * The numeric form, which is the one it actually uses: a trace of its system calls
 * (systrace=2 systrace_name=fairplayd.H2) counts 33 sysctl and not one sysctlbyname, so the hook on
 * the name took nothing all along -- the wrong name, again. The MIB is printed as numbers: the
 * first two say which subsystem, and that is enough to name what it asked about.
 */
static int
watched_sysctl(int *mib, unsigned count, void *out, size_t *out_size, void *in, size_t in_size)
{
	int result = sysctl(mib, count, out, out_size, in, in_size);

	if (ours && mib != NULL && count >= 1) {
		say("redwood: fairplayd: sysctl [%d %d %d %d] (%u deep) -> %d\n", mib[0], count > 1 ? mib[1] : -1,
		    count > 2 ? mib[2] : -1, count > 3 ? mib[3] : -1, count, result);
	}
	return result;
}

/*
 * ... and the private name underneath it. The hook on sysctl() did not fire once while the trace
 * counted thirty-three of them, because inside libsystem the call is __sysctl: a stub is interposed
 * only where another image calls through it. That is the wrong-name shape for the fourth time on
 * this branch, and the answer each time is the same -- hook every name the thing is known by, and
 * let the one that never fires say so.
 */
extern int __sysctl(int *mib, unsigned count, void *out, size_t *out_size, void *in, size_t in_size);

static int
watched___sysctl(int *mib, unsigned count, void *out, size_t *out_size, void *in, size_t in_size)
{
	int result = __sysctl(mib, count, out, out_size, in, in_size);

	if (ours && mib != NULL && count >= 1) {
		say("redwood: fairplayd: sysctl [%d %d %d %d] (%u deep) -> %d\n", mib[0], count > 1 ? mib[1] : -1,
		    count > 2 ? mib[2] : -1, count > 3 ? mib[3] : -1, count, result);
	}
	return result;
}

/* the channel to AMFI and the sandbox: twelve calls a life, and one of the few things left */
extern int __mac_syscall(const char *policy, int call, void *arg);

static int
watched___mac_syscall(const char *policy, int call, void *arg)
{
	int result = __mac_syscall(policy, call, arg);

	if (ours) {
		say("redwood: fairplayd: __mac_syscall(\"%s\", %d) -> %d\n", policy != NULL ? policy : "(null)",
		    call, result);
	}
	return result;
}

static int
watched_sysctlbyname(const char *name, void *out, size_t *out_size, void *in, size_t in_size)
{
	int result = sysctlbyname(name, out, out_size, in, in_size);

	if (ours) {
		say("redwood: fairplayd: sysctlbyname(\"%s\") -> %d\n", name != NULL ? name : "(null)", result);
	}
	return result;
}

static void *
watched_dlopen(const char *path, int mode)
{
	void *library = dlopen(path, mode);

	if (ours) {
		say("redwood: fairplayd: dlopen(\"%s\") -> %s\n", path != NULL ? path : "(null)",
		    library != NULL ? "loaded" : dlerror());
	}
	return library;
}

/*
 * It reads the feature flags of containermanagerd and its own Info.plist and then leaves, so the
 * next question is who it asks over XPC and what it looks for on disk: its container, most likely
 * (its entitlements name com.apple.security.system-container and
 * com.apple.private.security.storage.fpsd), and a lookup that fails there would explain a daemon
 * that declines to work without complaining.
 */
static xpc_connection_t
watched_xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags)
{
	xpc_connection_t connection = xpc_connection_create_mach_service(name, queue, flags);

	if (ours) {
		say("redwood: fairplayd: xpc_connection_create_mach_service(\"%s\") -> %s\n",
		    name != NULL ? name : "(null)", connection != NULL ? "a connection" : "nothing");
	}
	return connection;
}

static int
watched_stat(const char *path, struct stat *out)
{
	int result = stat(path, out);

	if (ours) {
		say("redwood: fairplayd: stat(\"%s\") -> %d\n", path != NULL ? path : "(null)", result);
	}
	return result;
}

static int
watched_access(const char *path, int mode)
{
	int result = access(path, mode);

	if (ours) {
		say("redwood: fairplayd: access(\"%s\") -> %d\n", path != NULL ? path : "(null)", result);
	}
	return result;
}

static int
watched_mkdir(const char *path, mode_t mode)
{
	int result = mkdir(path, mode);

	if (ours) {
		say("redwood: fairplayd: mkdir(\"%s\") -> %d\n", path != NULL ? path : "(null)", result);
	}
	return result;
}

/*
 * WHAT IT ASKS ABOUT THE DEVICE, which is the question left after AMFI was cleared twice over --
 * by a static read of the binary (it imports neither __mac_syscall nor sysctl, and has no svc
 * instruction at all, so the twelve and the thirty-three in the kernel's trace are dyld's and
 * libSystem's, not its) and by the trace itself (policy "AMFI" request 90 = 0, policy "Sandbox"
 * requests 2 and 4 = 0, and no ENOPOLICY anywhere).
 * MGCopyAnswer and SecTaskCopyValueForEntitlement are called ACROSS AN IMAGE BOUNDARY, which is
 * exactly what dyld interposing can stand between -- unlike the libsystem-internal calls this
 * library could not see. The key is built at run time by the obfuscator, but at the call it is a
 * finished string.
 * ONLY THE KEY IS PRINTED, never the answer: MobileGestalt answers with serial numbers, the unique
 * device identifier and the like, and this log goes into a repository.
 *
 * AND THE ANSWER IS THAT IT ASKS NOTHING. Not one MGCopyAnswer and not one entitlement read in a
 * session, while the same library's open() and access() hooks print 1675 lines from the same
 * processes -- so the interposing works and the calls do not happen. It is not for want of the
 * machinery: its import table carries _MGCopyAnswer, _SecTaskCopyValueForEntitlement,
 * _IOServiceMatching, _IOServiceGetMatchingServices, _IOServiceOpen, _bootstrap_check_in,
 * _bootstrap_look_up, _notify_register_check and the rest. It imports all of it and calls none of
 * it.
 * So the decision is made BEFORE it asks the device anything at all, which rules out the reading
 * that was most tempting -- that it looked for H2 hardware, did not find it, and left. It never
 * looks. What it does do first, from the kernel's trace, is what CoreFoundation does for every
 * process: the feature flags, and its own bundle, where /usr/sbin/Info.plist does not exist.
 */
static CFTypeRef
watched_MGCopyAnswer(CFStringRef key)
{
	CFTypeRef answer = MGCopyAnswer(key);

	if (ours) {
		char text[96] = "";

		if (key == NULL || !CFStringGetCString(key, text, sizeof(text), kCFStringEncodingUTF8)) {
			strlcpy(text, "(unreadable)", sizeof(text));
		}
		say("redwood: fairplayd: MGCopyAnswer(\"%s\") -> %s\n", text, answer != NULL ? "an answer" : "NOTHING");
	}
	return answer;
}

static CFTypeRef
watched_SecTaskCopyValueForEntitlement(void *task, CFStringRef entitlement, CFErrorRef *error)
{
	CFTypeRef value = SecTaskCopyValueForEntitlement(task, entitlement, error);

	if (ours) {
		char text[96] = "";

		if (entitlement == NULL || !CFStringGetCString(entitlement, text, sizeof(text), kCFStringEncodingUTF8)) {
			strlcpy(text, "(unreadable)", sizeof(text));
		}
		say("redwood: fairplayd: entitlement \"%s\" -> %s\n", text, value != NULL ? "held" : "NOT HELD");
	}
	return value;
}

static void
watched__exit(int status)
{
	char frames[10 * 20 + 1] = "";

	if (ours) {
		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("redwood: fairplayd: LEAVING, _exit(%d), called from%s (unslid)\n", status, frames);
	}
	_exit(status);
}

static void
watched_abort(void)
{
	char frames[10 * 20 + 1] = "";

	if (ours) {
		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("redwood: fairplayd: LEAVING, abort(), called from%s (unslid)\n", frames);
	}
	abort();
}

/*
 * A sampler, because it does not call anything we can watch. It reads the feature flags, opens its
 * own bundle and leaves, without IOKit, without XPC, without a sysctl or a dlopen -- so whatever it
 * decides on, it decides with code we cannot interpose. The main thread's program counter, read a
 * few hundred times a second from a thread of ours (the same thread_get_state the store library uses
 * for its thread dump), says where it was instead. The daemon lives about a fifth of a second, so a
 * sample every half a millisecond covers the whole of it.
 * Addresses are printed three ways -- as they are, less the shared cache's slide (for
 * "ipsw dyld a2s"), and less this program's own slide (for its own 19 MB of obfuscated code).
 */
#define SAMPLES 2048
static uintptr_t samples[SAMPLES];
static unsigned sample_count;
static mach_port_t main_thread;
static uintptr_t own_slide;

static void *
sampler(__unused void *ignored)
{
	while (sample_count < SAMPLES) {
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;

		if (thread_get_state(main_thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
			break;
		}
		samples[sample_count++] = (uintptr_t)arm_thread_state64_get_pc(state);
		usleep(500);
	}
	return NULL;
}

static void
at_the_end(void)
{
	unsigned shown = 0;

	if (!ours) {
		return;
	}
	say("redwood: fairplayd: at the end (an exit handler ran: it left of its own accord), %u samples\n",
	    sample_count);
	/* every distinct pc, newest first: a fifth of a second of a daemon is a short story */
	for (unsigned i = sample_count; i > 0 && shown < 40; i--) {
		uintptr_t pc = samples[i - 1];
		bool seen = false;

		for (unsigned j = i; j < sample_count; j++) {
			seen |= samples[j] == pc;
		}
		if (seen) {
			continue;
		}
		shown++;
		say("redwood: fairplayd: was at %#lx (cache %#lx, own %#lx)\n", pc, pc - slide, pc - own_slide);
	}
}

static void
watched_exit(int status)
{
	char frames[10 * 20 + 1] = "";

	if (ours) {
		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("redwood: fairplayd: LEAVING, exit(%d), called from%s (unslid)\n", status, frames);
	}
	exit(status);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)watched_IOServiceMatching, (const void *)IOServiceMatching },
	{ (const void *)watched_IOServiceGetMatchingServices, (const void *)IOServiceGetMatchingServices },
	{ (const void *)watched_IOServiceGetMatchingService, (const void *)IOServiceGetMatchingService },
	{ (const void *)watched_IOServiceOpen, (const void *)IOServiceOpen },
	{ (const void *)watched_IOConnectCallMethod, (const void *)IOConnectCallMethod },
	{ (const void *)watched_exit, (const void *)exit },
	{ (const void *)watched__exit, (const void *)_exit },
	{ (const void *)watched_abort, (const void *)abort },
	{ (const void *)watched_open, (const void *)open },
	{ (const void *)watched_sysctlbyname, (const void *)sysctlbyname },
	{ (const void *)watched_sysctl, (const void *)sysctl },
	{ (const void *)watched___sysctl, (const void *)__sysctl },
	{ (const void *)watched___mac_syscall, (const void *)__mac_syscall },
	{ (const void *)watched_MGCopyAnswer, (const void *)MGCopyAnswer },
	{ (const void *)watched_SecTaskCopyValueForEntitlement, (const void *)SecTaskCopyValueForEntitlement },
	{ (const void *)watched_dlopen, (const void *)dlopen },
	{ (const void *)watched_xpc_connection_create_mach_service, (const void *)xpc_connection_create_mach_service },
	{ (const void *)watched_stat, (const void *)stat },
	{ (const void *)watched_access, (const void *)access },
	{ (const void *)watched_mkdir, (const void *)mkdir },
};

__attribute__((constructor))
static void
redwood_fairplay_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	struct sigaction action;

	ours = name != NULL && strncmp(name, "fairplayd", 9) == 0;
	if (!ours) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
	main_thread = mach_thread_self();
	own_slide = (uintptr_t)_dyld_get_image_vmaddr_slide(0);
	atexit(at_the_end);
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, sampler, NULL) == 0) {
			pthread_detach(thread);
		}
	}
	{
		char ***argv = _NSGetArgv();
		int *argc = _NSGetArgc();

		say("redwood: fairplayd: ours is in, %d argument(s)\n", argc != NULL ? *argc : -1);
		for (int i = 0; argv != NULL && argc != NULL && i < *argc && i < 6; i++) {
			say("redwood: fairplayd: argv[%d] = %s\n", i, (*argv)[i] != NULL ? (*argv)[i] : "(null)");
		}
	}
}
