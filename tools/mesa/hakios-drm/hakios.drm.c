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
#include <pthread.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"

#define HAKIOS_GPU_SERVICE     "HakiosGPU"
#define HAKIOS_GPU_USER_MEMORY 0x7d            /* ours too: pages of this process as a buffer */
#define HAKIOS_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define HAKIOS_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

static io_connect_t connection;
static int connection_fd = -1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(HAKIOS_GPU_SERVICE));
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

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

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

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

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

static int submit_with_syncobjs(struct drm_msm_gem_submit *submit, unsigned long size);

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	if (index == DRM_MSM_GEM_SUBMIT && size >= sizeof(struct drm_msm_gem_submit)) {
		return submit_with_syncobjs(data, size);
	}
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
		version->version_minor = 7;
		version->name = strdup("msm");
		version->name_len = 3;
		version->date = strdup("");
		version->desc = strdup("Adreno on Darwin");
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

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
hakios_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
hakios_drm_surface(int fd, struct hakios_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = HAKIOS_GPU_DESCRIBE;
	error = call(HAKIOS_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct hakios_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(HAKIOS_GPU_SURFACE, &request, sizeof(request));

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

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)

/*
 * DRM's synchronisation objects (syncobj), which Turnip's msm backend makes its VkFences and VkSemaphores of, kept in
 * this process: there is one GPU and one connection to it here, so what a syncobj waits for is always one of this
 * process's submissions, a fence number of the kernel's (MSM_WAIT_FENCE tells when it is done). A syncobj is a timeline:
 * the last point known reached, and the points still on their way, each with the submission that reaches it. A binary
 * one is the timeline of point 1 (signalled: 1, reset: 0). A submission's MSM_SUBMIT_SYNCOBJ_IN syncobjs are waited for
 * here, before it goes to the kernel, and its MSM_SUBMIT_SYNCOBJ_OUT ones are given its fence (submit_with_syncobjs).
 * Not here: syncobjs and sync files as file descriptors (nothing outside this process waits for them).
 */
#define SYNCOBJS        4096
#define PENDING         16

struct syncobj {
	bool used;
	uint64_t reached;                       /* the last point known reached */
	unsigned pending;
	struct { uint64_t point; uint32_t fence; } on_the_way[PENDING];
};
static struct syncobj syncobjs[SYNCOBJS];
static pthread_mutex_t syncobj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t syncobj_submitted = PTHREAD_COND_INITIALIZER;

static struct syncobj *
syncobj_of(uint32_t handle)
{
	return handle != 0 && handle < SYNCOBJS && syncobjs[handle].used ? &syncobjs[handle] : NULL;
}

/* whether the kernel is done with submission FENCE: waited for until DEADLINE (absolute CLOCK_MONOTONIC ns; 0: not at all) */
static bool
fence_done(uint32_t fence, int64_t deadline)
{
	struct drm_msm_wait_fence wait = { .fence = fence, .queueid = 0 };

	wait.timeout.tv_sec = deadline / 1000000000;
	wait.timeout.tv_nsec = deadline % 1000000000;
	return call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &wait, sizeof(wait)) == 0;
}

static int64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* the points on their way that their submissions have reached since: into REACHED (lock held) */
static void
syncobj_poll(struct syncobj *s)
{
	unsigned kept = 0;

	for (unsigned i = 0; i < s->pending; i++) {
		if (fence_done(s->on_the_way[i].fence, 0)) {
			if (s->on_the_way[i].point > s->reached) s->reached = s->on_the_way[i].point;
		} else {
			s->on_the_way[kept++] = s->on_the_way[i];
		}
	}
	s->pending = kept;
}

static void
syncobj_add(struct syncobj *s, uint64_t point, uint32_t fence)
{
	if (s->pending == PENDING) {
		syncobj_poll(s);
	}
	if (s->pending == PENDING) {
		/* (full: the oldest is waited for, its point taken as reached) */
		fence_done(s->on_the_way[0].fence, now_ns() + 10000000000LL);
		if (s->on_the_way[0].point > s->reached) s->reached = s->on_the_way[0].point;
		memmove(&s->on_the_way[0], &s->on_the_way[1], (PENDING - 1) * sizeof(s->on_the_way[0]));
		s->pending--;
	}
	s->on_the_way[s->pending].point = point;
	s->on_the_way[s->pending].fence = fence;
	s->pending++;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t h = 1; h < SYNCOBJS; h++) {
		if (!syncobjs[h].used) {
			memset(&syncobjs[h], 0, sizeof(syncobjs[h]));
			syncobjs[h].used = true;
			syncobjs[h].reached = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0;
			*handle = h;
			pthread_mutex_unlock(&syncobj_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	errno = ENOMEM;
	return -ENOMEM;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct syncobj *s;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	s = syncobj_of(handle);
	if (s != NULL) s->used = false;
	pthread_mutex_unlock(&syncobj_lock);
	return s != NULL ? 0 : -EINVAL;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		if (s != NULL) s->reached = 0, s->pending = 0;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		uint64_t point = points != NULL && points[i] != 0 ? points[i] : 1;
		if (s != NULL && point > s->reached) s->reached = point;
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	return drmSyncobjTimelineSignal(fd, handles, NULL, count);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	(void)fd;
	(void)flags;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		if (s != NULL) syncobj_poll(s);
		points[i] = s != NULL ? s->reached : 0;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return drmSyncobjQuery2(fd, handles, points, count, 0);
}

/*
 * Wait for each syncobj's point (0 or NULL: 1, a binary one's) -- all of them, or the first (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
 * clear) -- until DEADLINE (absolute CLOCK_MONOTONIC ns). A point no submission is on its way to yet is waited for to be
 * submitted with DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, an error without (as Linux answers); WAIT_AVAILABLE: submitted
 * is enough.
 */
int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t deadline, unsigned flags,
    uint32_t *first)
{
	const bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	int result = -ETIME;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (;;) {
		unsigned reached = 0, unknown = 0;
		uint32_t wait_fence = 0;
		bool have_fence = false;

		for (unsigned i = 0; i < count; i++) {
			struct syncobj *s = syncobj_of(handles[i]);
			uint64_t point = points != NULL && points[i] != 0 ? points[i] : 1;
			bool on_the_way = false;

			if (s == NULL) {
				pthread_mutex_unlock(&syncobj_lock);
				return -EINVAL;
			}
			syncobj_poll(s);
			for (unsigned k = 0; k < s->pending; k++) {
				if (s->on_the_way[k].point >= point) {
					on_the_way = true;
					if (!have_fence || (int32_t)(s->on_the_way[k].fence - wait_fence) < 0) wait_fence = s->on_the_way[k].fence;
					have_fence = true;
					break;
				}
			}
			if (s->reached >= point || (on_the_way && (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE))) {
				reached++;
				if (!all) {
					if (first != NULL) *first = i;
					pthread_mutex_unlock(&syncobj_lock);
					return 0;
				}
			} else if (!on_the_way) {
				unknown++;
			}
		}
		if (reached == count) {
			if (first != NULL) *first = 0;
			pthread_mutex_unlock(&syncobj_lock);
			return 0;
		}
		if (unknown != 0 && !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
			pthread_mutex_unlock(&syncobj_lock);
			return -EINVAL;
		}
		if (now_ns() >= deadline) {
			break;
		}
		if (have_fence && unknown == 0) {
			/* every point is on its way: the earliest submission among them waited for, without the lock */
			pthread_mutex_unlock(&syncobj_lock);
			fence_done(wait_fence, deadline);
			pthread_mutex_lock(&syncobj_lock);
		} else {
			/* some point not submitted yet: until a submission or a signal, in steps of a millisecond at most */
			struct timespec until;
			int64_t step = now_ns() + 1000000;
			struct timeval tv;
			gettimeofday(&tv, NULL);
			int64_t real = (int64_t)tv.tv_sec * 1000000000 + (int64_t)tv.tv_usec * 1000 + (step < deadline ? 1000000 : deadline - now_ns());
			until.tv_sec = real / 1000000000;
			until.tv_nsec = real % 1000000000;
			pthread_cond_timedwait(&syncobj_submitted, &syncobj_lock, &until);
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	return result;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{
	return drmSyncobjTimelineWait(fd, handles, NULL, count, deadline, flags, first);
}

/* DST at DST_POINT (0: binary) gets what SRC at SRC_POINT (0: binary) is or will be */
int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	struct syncobj *d, *s;
	uint64_t want = src_point != 0 ? src_point : 1, to = dst_point != 0 ? dst_point : 1;

	(void)fd;
	(void)flags;
	pthread_mutex_lock(&syncobj_lock);
	d = syncobj_of(dst);
	s = syncobj_of(src);
	if (d == NULL || s == NULL) {
		pthread_mutex_unlock(&syncobj_lock);
		return -EINVAL;
	}
	syncobj_poll(s);
	if (dst_point == 0) d->reached = 0, d->pending = 0;
	if (s->reached >= want) {
		if (to > d->reached) d->reached = to;
	} else {
		for (unsigned k = 0; k < s->pending; k++) {
			if (s->on_the_way[k].point >= want) {
				syncobj_add(d, to, s->on_the_way[k].fence);
				break;
			}
		}
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

static uint32_t last_fence;     /* the fence of the last submission the kernel took */

static int
submit_with_syncobjs(struct drm_msm_gem_submit *submit, unsigned long size)
{
	const struct drm_msm_gem_submit_syncobj *in = (const void *)(uintptr_t)submit->in_syncobjs;
	const struct drm_msm_gem_submit_syncobj *out = (const void *)(uintptr_t)submit->out_syncobjs;
	const uint32_t stride = submit->syncobj_stride ? submit->syncobj_stride : sizeof(*in);
	const uint32_t nin = (submit->flags & MSM_SUBMIT_SYNCOBJ_IN) ? submit->nr_in_syncobjs : 0;
	const uint32_t nout = (submit->flags & MSM_SUBMIT_SYNCOBJ_OUT) ? submit->nr_out_syncobjs : 0;
	int error;

	/* what it waits for first (this queue's own submissions are in order on the one ring anyway) */
	for (uint32_t i = 0; i < nin; i++) {
		const struct drm_msm_gem_submit_syncobj *x = (const void *)((const uint8_t *)in + i * stride);
		uint32_t h = x->handle;
		uint64_t p = x->point;
		int r = drmSyncobjTimelineWait(0, &h, &p, 1, now_ns() + 10000000000LL,
		    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
		if (r != 0) return r;
		if (x->flags & MSM_SUBMIT_SYNCOBJ_RESET) drmSyncobjReset(0, &h, 1);
	}
	submit->flags &= ~(uint32_t)(MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT);
	submit->in_syncobjs = submit->out_syncobjs = 0;
	submit->nr_in_syncobjs = submit->nr_out_syncobjs = 0;
	if (submit->nr_cmds == 0) {
		/*
		 * Nothing for the GPU (Turnip's submissions that only signal): the kernel takes none, and what it stands for
		 * is done when everything submitted before it is, the last fence -- there is one ring.
		 */
		submit->fence = __atomic_load_n(&last_fence, __ATOMIC_ACQUIRE);
	} else {
		error = call((unsigned)(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT), submit, size);
		if (error != 0) return -error;
		__atomic_store_n(&last_fence, submit->fence, __ATOMIC_RELEASE);
	}
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < nout; i++) {
		const struct drm_msm_gem_submit_syncobj *x = (const void *)((const uint8_t *)out + i * stride);
		struct syncobj *s = syncobj_of(x->handle);
		if (s == NULL) continue;
		if (x->point == 0) s->reached = 0, s->pending = 0;   /* a binary one: what it holds now is this submission */
		syncobj_add(s, x->point != 0 ? x->point : 1, submit->fence);
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

/* Linux's device nodes, which Turnip's lists of devices ask about: one GPU, a render node 128 */
int drmGetNodeTypeFromFd(int fd) { (void)fd; return DRM_NODE_RENDER; }

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices) { (void)flags; (void)devices; (void)max_devices; return 0; }
void drmFreeDevices(drmDevicePtr devices[], int count) { (void)devices; (void)count; }
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) { (void)fd; (void)flags; *device = NULL; return -ENODEV; }
void drmFreeDevice(drmDevicePtr *device) { (void)device; }

/*
 * The IOSurfaces mapped in this process, by address (hakios_drm_host_surface): a host pointer Turnip is asked to import
 * (VK_EXT_external_memory_host) is the memory of one of them, and the kernel makes a buffer of a surface by its ID
 * (drmPrimeFDToHandle). A surface's mapping lasts as long as the surface; a later surface at the same address replaces it.
 */
#define HOST_SURFACES 1024
static struct { uintptr_t base; uint64_t size; uint32_t surface; } host_surfaces[HOST_SURFACES];
static unsigned host_surfaces_next;
static pthread_mutex_t host_surfaces_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((visibility("default")))     /* (called by AdrenoMetal's frontend, outside Turnip) */
void
hakios_drm_host_surface(const void *base, uint64_t size, uint32_t surface)
{
	unsigned i, free_slot = HOST_SURFACES;

	pthread_mutex_lock(&host_surfaces_lock);
	for (i = 0; i < HOST_SURFACES; i++) {
		if (host_surfaces[i].base == (uintptr_t)base) {
			break;
		}
		if (host_surfaces[i].base == 0 && free_slot == HOST_SURFACES) {
			free_slot = i;
		}
	}
	if (i == HOST_SURFACES) {
		i = free_slot != HOST_SURFACES ? free_slot : host_surfaces_next++ % HOST_SURFACES;
	}
	host_surfaces[i].base = (uintptr_t)base;
	host_surfaces[i].size = size;
	host_surfaces[i].surface = surface;
	pthread_mutex_unlock(&host_surfaces_lock);
}

uint32_t
hakios_drm_host_surface_at(const void *base, uint64_t size)
{
	uint32_t surface = 0;

	pthread_mutex_lock(&host_surfaces_lock);
	for (unsigned i = 0; i < HOST_SURFACES; i++) {
		if (host_surfaces[i].base == (uintptr_t)base && host_surfaces[i].size >= size) {
			surface = host_surfaces[i].surface;
			break;
		}
	}
	pthread_mutex_unlock(&host_surfaces_lock);
	return surface;
}

int
hakios_drm_user_memory(int fd, const void *address, uint64_t size, uint32_t *handle)
{
	struct { uint64_t address, size; uint32_t handle, flags; } request = { (uintptr_t)address, size, 0, 0 };
	int error = call(HAKIOS_GPU_USER_MEMORY, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}
