/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"

#define REDWOOD_GPU_SERVICE     "RedwoodGPU"
#define REDWOOD_MALI_SERVICE    "RedwoodMali"   /* the Infinix's Mali, for panfrost (iokit/Kernel/RedwoodMali.cpp) */
#define REDWOOD_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define REDWOOD_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

static io_connect_t connection;
static int connection_fd = -1;
static int mali;                                /* the GPU is a Mali, and its driver panfrost */
static uint32_t next_syncobj = 1;

/*
 * Fences as job numbers. The kernel queues a job chain and gives back its number (in the
 * submission's `jc`); a syncobj stands for the number of the last job submitted with it as its
 * out_sync, a sync file (a descriptor of /dev/null's) for the number it was exported with, and a
 * wait is the kernel's wait for that number (REDWOOD_WAIT).
 */
#define REDWOOD_WAIT            0x7d
#define FENCES                  1024
static uint64_t syncobj_seq[FENCES];            /* by handle % FENCES */
static uint64_t syncfile_seq[FENCES];           /* by descriptor % FENCES */
static uint64_t last_seq;                       /* the last job submitted */

struct redwood_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0 && strcmp(name, "panfrost") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	mali = strcmp(name, "panfrost") == 0;
	service = IOServiceGetMatchingService(0, IOServiceMatching(mali ? REDWOOD_MALI_SERVICE : REDWOOD_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

/*
 * How many calls into the kernel's driver, of which kind, and how long they took: said every five
 * seconds ("rwm kernel calls"). A draw of the window server's measured 100 us in panfrost while a
 * profiler found a fraction of that in Mesa's own code (25 October).
 */
static uint64_t call_count[256], call_ns[256], map_count, map_ns, calls_said, create_sizes[32];

static uint64_t
call_clock(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void
calls_say(uint64_t now)
{
	char line[600];
	int length;

	if (calls_said == 0) {
		calls_said = now;
		return;
	}
	if (now - calls_said < 5000000000ull) {
		return;
	}
	length = snprintf(line, sizeof(line), "rwm kernel calls %llus:", (unsigned long long)((now - calls_said) / 1000000000ull));
	for (int top = 0; top < 6; top++) {
		int best = -1;

		for (int i = 0; i < 256; i++) {
			if (call_count[i] != 0 && (best < 0 || call_ns[i] > call_ns[best])) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		length += snprintf(line + length, sizeof(line) - (size_t)length, " %#x x%llu %llu ms (%llu us);", best,
		    (unsigned long long)call_count[best], (unsigned long long)(call_ns[best] / 1000000),
		    (unsigned long long)(call_ns[best] / 1000 / call_count[best]));
		call_count[best] = 0;
		call_ns[best] = 0;
	}
	length += snprintf(line + length, sizeof(line) - (size_t)length, " maps x%llu %llu ms; made by size:",
	    (unsigned long long)map_count, (unsigned long long)(map_ns / 1000000));
	for (int i = 0; i < 32 && length < (int)sizeof(line) - 24; i++) {
		if (create_sizes[i] != 0) {
			length += snprintf(line + length, sizeof(line) - (size_t)length, " %uK x%llu", (1u << i) >> 10,
			    (unsigned long long)create_sizes[i]);
		}
	}
	memset(create_sizes, 0, sizeof(create_sizes));
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	memset(call_count, 0, sizeof(call_count));
	memset(call_ns, 0, sizeof(call_ns));
	map_count = map_ns = 0;
	calls_said = now;
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	uint64_t began = call_clock();
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);
	uint64_t ended = call_clock();

	call_count[number & 0xff]++;
	if ((number & 0xff) == 0x42 && size >= 8) {        /* CREATE_BO: its size, by power of two */
		uint32_t asked = *(uint32_t *)data;
		create_sizes[asked ? 31 - __builtin_clz(asked) : 0]++;
	}
	call_ns[number & 0xff] += ended - began;
	calls_say(ended);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

/* panfrost's submission, as far as this file reads it */
struct redwood_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};

static uint64_t
monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* wait for job `seq`: 0 when done, ETIMEDOUT when not by then (an absolute CLOCK_MONOTONIC time) */
static int
wait_seq(uint64_t seq, int64_t absolute_ns)
{
	struct redwood_wait w = { seq, 0 };
	uint64_t now = monotonic_ns();

	if (seq == 0) {
		return 0;
	}
	w.timeout_ns = absolute_ns <= 0 || (uint64_t)absolute_ns <= now ? 0 : absolute_ns - (int64_t)now;
	return call(REDWOOD_WAIT, &w, sizeof(w));
}

/* For the Metal driver (air_metal.c rwa_flush_seq, rwa_wait_seq): the number of the last job
 * submitted, and a wait for a job by its number for `relative_ns` at most -- 0 when it is done. */
uint64_t redwood_drm_last_seq(void);
uint64_t
redwood_drm_last_seq(void)
{
	return last_seq;
}

/* a surface's last job, or ~0 while it is drawn and not yet sent (the kernel's display waits) */
int redwood_drm_surface_seq(uint32_t surface, uint64_t seq);
int
redwood_drm_surface_seq(uint32_t surface, uint64_t seq)
{
	struct { uint32_t surface, pad; uint64_t seq; } r = { surface, 0, seq };

	return call(0x7c, &r, sizeof(r));
}

int redwood_drm_wait_seq(uint64_t seq, int64_t relative_ns);
int
redwood_drm_wait_seq(uint64_t seq, int64_t relative_ns)
{
	struct redwood_wait w = { seq, relative_ns };

	return seq == 0 ? 0 : call(REDWOOD_WAIT, &w, sizeof(w));
}

/*
 * Buffers given back, kept a while for the next ask of the same size and kind. A buffer made here
 * is 2.5 ms of the kernel's (memory allocated, cleared, made write-combined, entered page by page),
 * and panfrost makes some 60 a second in the window server and gives as many back (run 141, 26
 * October): its own cache will not hand out a buffer the GPU may still be using, and with jobs
 * queued the oldest one in it usually is. Here a buffer is handed out again once the kernel says
 * its last job is done, as panfrost's cache would; it is not cleared, as panfrost's is not either.
 * Two seconds and 32 MB at most -- this phone has little memory to spare. Only buffers made by
 * CREATE_BO: an imported surface is given back for real.
 */
struct panfrost_create { uint32_t size, flags, handle, pad; uint64_t offset; };
#define MAPS 8192
static void maps_forget(uint32_t handle);
#define PARKED 256
#define PARKED_BYTES (32ull << 20)
#define PARKED_NS 2000000000ull
static struct { uint32_t handle, size, flags; uint64_t offset, since; } parked[PARKED];
static unsigned parked_count;
static uint64_t parked_bytes;
static uint8_t made_here[MAPS];
static uint32_t made_size[MAPS], made_flags[MAPS];
static uint64_t made_offset[MAPS];

static void
made_note(const struct panfrost_create *create)
{
	if (create->handle < MAPS) {
		made_here[create->handle] = 1;
		made_size[create->handle] = (create->size + 4095) & ~4095u;
		made_flags[create->handle] = create->flags;
		made_offset[create->handle] = create->offset;
	}
}

static void
made_forget(uint32_t handle)
{
	made_here[handle] = 0;
}
static pthread_mutex_t parked_lock = PTHREAD_MUTEX_INITIALIZER;

static void
really_close(uint32_t handle)
{
	uint32_t request[2] = { handle, 0 };

	made_forget(handle);
	maps_forget(handle);
	(void)call(0x09, request, sizeof(request));
}

/* with parked_lock held */
static void
parked_drop(unsigned i)
{
	really_close(parked[i].handle);
	parked_bytes -= parked[i].size;
	parked[i] = parked[--parked_count];
}

/* with parked_lock held: the old ones, and the oldest while over the budget */
static void
parked_trim(uint64_t now)
{
	for (unsigned i = 0; i < parked_count;) {
		if (now - parked[i].since > PARKED_NS) {
			parked_drop(i);
		} else {
			i++;
		}
	}
	while (parked_bytes > PARKED_BYTES && parked_count > 0) {
		unsigned oldest = 0;

		for (unsigned i = 1; i < parked_count; i++) {
			if (parked[i].since < parked[oldest].since) {
				oldest = i;
			}
		}
		parked_drop(oldest);
	}
}

/* boot-arg drm_pool=0: no buffer is kept for the next ask (for telling what the pool changes) */
static bool
pool_wanted(void)
{
	static int wanted = -1;

	if (wanted < 0) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		wanted = !(sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
		           strstr(arguments, "drm_pool=0") != NULL);
	}
	return wanted;
}

static bool
parked_take(struct panfrost_create *create)
{
	uint32_t size = (create->size + 4095) & ~4095u;
	bool found = false;

	pthread_mutex_lock(&parked_lock);
	parked_trim(call_clock());
	for (unsigned i = 0; i < parked_count; i++) {
		struct { uint32_t handle, pad; int64_t timeout; } wait = { parked[i].handle, 0, 0 };

		if (parked[i].size != size || parked[i].flags != create->flags || call(0x41, &wait, sizeof(wait)) != 0) {
			continue;       /* another size or kind, or a job still has it */
		}
		create->handle = parked[i].handle;
		create->offset = parked[i].offset;
		parked_bytes -= parked[i].size;
		parked[i] = parked[--parked_count];
		found = true;
		break;
	}
	pthread_mutex_unlock(&parked_lock);
	return found;
}

static bool
parked_put(uint32_t handle)
{
	bool kept = false;

	if (!mali || handle >= MAPS || !made_here[handle] || !pool_wanted()) {
		return false;
	}
	pthread_mutex_lock(&parked_lock);
	parked_trim(call_clock());
	if (parked_count == PARKED) {
		parked_drop(0);
	}
	parked[parked_count].handle = handle;
	parked[parked_count].size = made_size[handle];
	parked[parked_count].flags = made_flags[handle];
	parked[parked_count].offset = made_offset[handle];
	parked[parked_count].since = call_clock();
	parked_bytes += made_size[handle];
	parked_count++;
	kept = true;
	pthread_mutex_unlock(&parked_lock);
	return kept;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error;

	if (mali && (request & 0xff) == 0x42 && IOCPARM_LEN(request) >= sizeof(struct panfrost_create) &&
	    parked_take(arg)) {
		return 0;
	}
	/*
	 * Two answers known without asking, for calls panfrost makes every time its cache hands a buffer
	 * out or takes one back -- 10 500 and 4 000 of them every five seconds in the window server, some
	 * 225 ms of the worker's (run 191): MADVISE, which the kernel's driver always answers "retained"
	 * (it never lets a buffer go behind panfrost's back), and MMAP_BO, whose offset is the handle
	 * shifted 12 (RedwoodMali.cpp) and only ever checked for not being negative (redwood_drm_map maps
	 * by handle). A handle not made here still goes to the kernel.
	 */
	if (mali && (request & 0xff) == 0x48 && IOCPARM_LEN(request) >= 12) {
		uint32_t *madvise = arg;        /* handle, madv, retained */

		if (madvise[0] < MAPS && made_here[madvise[0]]) {
			madvise[2] = 1;
			return 0;
		}
	}
	if (mali && (request & 0xff) == 0x43 && IOCPARM_LEN(request) >= 16) {
		struct { uint32_t handle, flags; uint64_t offset; } *mmap_bo = arg;

		if (mmap_bo->handle < MAPS && made_here[mmap_bo->handle]) {
			mmap_bo->offset = (uint64_t)mmap_bo->handle << 12;
			return 0;
		}
	}
	error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));
	if (mali && error == 0 && (request & 0xff) == 0x42 && IOCPARM_LEN(request) >= sizeof(struct panfrost_create)) {
		made_note(arg);
	}

	if (mali && error == 0 && (request & 0xff) == 0x40) {
		struct redwood_submit *submit = arg;      /* DRM_IOCTL_PANFROST_SUBMIT: the number back in jc */

		if (submit->jc > last_seq) {
			last_seq = submit->jc;
		}
		if (submit->out_sync != 0) {
			syncobj_seq[submit->out_sync % FENCES] = submit->jc;
		}
	}

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = mali ? 2 : 7;
		version->name = strdup(mali ? "panfrost" : "msm");
		version->name_len = (int)strlen(version->name);
		version->date = strdup("");
		version->desc = strdup(mali ? "Mali on Darwin" : "Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

/*
 * Every buffer's mapping, kept until its handle is given back. panfrost unmaps a buffer each time it
 * goes into its cache and maps it again each time it comes out, and here a mapping is a call into
 * the kernel of some 240 us: 1870 of them every five seconds in the window server, 0.45 s of its
 * time (run 138, 26 October). The kernel's driver gives a handle back only after GEM_CLOSE, which
 * is where the mapping really goes.
 */
static struct { void *address; uint64_t size; } maps[MAPS];
static pthread_mutex_t maps_lock = PTHREAD_MUTEX_INITIALIZER;

static void *redwood_drm_map_new(uint32_t handle, uint64_t size);

void *
redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	void *address;

	(void)fd;
	if (handle >= MAPS) {
		return redwood_drm_map_new(handle, size);
	}
	pthread_mutex_lock(&maps_lock);
	if (maps[handle].address != NULL && maps[handle].size >= size) {
		address = maps[handle].address;
	} else {
		if (maps[handle].address != NULL) {
			IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)maps[handle].address);
			maps[handle].address = NULL;
		}
		address = redwood_drm_map_new(handle, size);
		if (address != (void *)-1) {
			maps[handle].address = address;
			maps[handle].size = size;
		}
	}
	pthread_mutex_unlock(&maps_lock);
	return address;
}

static void
maps_forget(uint32_t handle)
{
	if (handle >= MAPS) {
		return;
	}
	pthread_mutex_lock(&maps_lock);
	if (maps[handle].address != NULL) {
		IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)maps[handle].address);
		maps[handle].address = NULL;
		maps[handle].size = 0;
	}
	pthread_mutex_unlock(&maps_lock);
}

static void *
redwood_drm_map_new(uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	uint64_t began = call_clock();

	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	map_count++;
	map_ns += call_clock() - began;
	if (mapped < size) {
		/* a mapping shorter than the buffer it was asked for: what a write past it would be */
		char line[160];
		static int said;

		if (said++ < 8) {
			snprintf(line, sizeof(line), "!rwm map short: handle %u, %llu bytes asked, %llu mapped at %#llx",
			         handle, (unsigned long long)size, (unsigned long long)mapped, (unsigned long long)address);
			sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
		}
	}
	return (void *)(uintptr_t)address;
}

void
redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	if (handle < MAPS) {
		return;         /* kept for the next map of the same buffer: maps_forget at GEM_CLOSE */
	}
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = REDWOOD_GPU_DESCRIBE;
	error = call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(REDWOOD_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)

/*
 * Fences. freedreno does without them here (none(): it falls back to its own). panfrost cannot,
 * and on the Mali it does not need to: the kernel runs a job chain to its end before the
 * submission returns, so every fence is signalled by the time anyone can ask about it. A fence is
 * a number and nothing else.
 */
int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	if (!mali) {
		return none();
	}
	*handle = __atomic_fetch_add(&next_syncobj, 1, __ATOMIC_RELAXED);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	(void)handle;
	return mali ? 0 : none();
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	for (uint32_t i = 0; mali && i < count; i++) {
		syncobj_seq[handles[i] % FENCES] = 0;
	}
	return mali ? 0 : none();
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout, unsigned flags, uint32_t *first)
{
	(void)fd;
	(void)flags;
	if (!mali) {
		return none();
	}
	if (first != NULL) {
		*first = 0;
	}
	/* (jobs finish in the order given: waiting for each is waiting for all, and for any the first
	 * done is the lowest number) */
	for (unsigned i = 0; i < count; i++) {
		int error = wait_seq(syncobj_seq[handles[i] % FENCES], timeout);

		if (error != 0) {
			errno = error;
			return -1;
		}
	}
	return 0;
}

/*
 * A fence as a sync file, and back: what panfrost makes of every flush (panfrost_fence_create), and
 * on the Mali a fence that is always signalled can be any descriptor -- /dev/null's. Without it every
 * frame of the window server said "export failed" and had no fence.
 */
int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	(void)handle;
	if (!mali) {
		return none();
	}
	*sync_file_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (*sync_file_fd >= 0) {
		syncfile_seq[*sync_file_fd % FENCES] = syncobj_seq[handle % FENCES];
	}
	return *sync_file_fd < 0 ? -1 : 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	if (mali && sync_file_fd >= 0) {
		syncobj_seq[handle % FENCES] = syncfile_seq[sync_file_fd % FENCES];
	}
	return mali ? 0 : none();
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout, unsigned flags,
    uint32_t *first)
{
	(void)points;
	return drmSyncobjWait(fd, handles, count, timeout, flags, first);
}

int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)dst_point;
	(void)src_point;
	(void)flags;
	if (mali) {
		syncobj_seq[dst % FENCES] = syncobj_seq[src % FENCES];
	}
	return mali ? 0 : none();
}

/* A buffer's handle given back: Linux's GEM_CLOSE. */
int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	uint32_t request[2] = { handle, 0 };

	(void)fd;
	if (parked_put(handle)) {
		return 0;
	}
	if (mali && handle < MAPS) {
		made_forget(handle);
	}
	maps_forget(handle);
	return call(0x09, request, sizeof(request)) == 0 ? 0 : -1;
}
