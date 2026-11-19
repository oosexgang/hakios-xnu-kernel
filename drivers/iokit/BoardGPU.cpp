/*
 * HakiosGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/board/adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: components/metal/tools/mesa/darwin-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such.
 *
 * Not here yet: interrupts (waiting is asking again five times a millisecond), more than one
 * program's worth of care (every client's buffers are in the one address space; a client that
 * goes takes its own away), fences and buffers as file descriptors. A GPU that has stopped
 * answering is reported (hakios_gpu_report()) and, with boot-arg gpu_recover=1, reset and
 * started again, so that the submission that hung is all that is lost: what that does and
 * which source says so is at hakios_gpu_recover() in bsd/dev/board/adreno.c. Without the
 * boot-arg it stays stopped and nothing is waited for again.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
#include <sys/systm.h>

#include "MiniIOSurface.h"
#include <vm/vm_kern_xnu.h>             /* kmem_alloc for the allocation diagnostic (gemNew) */
extern "C" unsigned int vm_page_free_count, vm_page_wire_count;

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/board/adreno.c ("the GPU kept on"; declared in bsd/sys/board/input.h, which is not seen from here) */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
void hakios_gpu_space(uint64_t * used, uint32_t * live, uint64_t * largest, unsigned * holes, unsigned * most,
    uint64_t * lost, uint32_t * refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
bool hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void * start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern uint32_t gpu_log;
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void hakios_gpu_report(const char * why);
bool hakios_gpu_recover(void);
void hakios_gpu_stop(void);

void HakiosGPUStart(void);
void HakiosGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
void MiniIOMobileFramebufferPanel(uint32_t * width, uint32_t * height);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHakiosUserMemory      = 0x7b,         /* ours: pages of the caller's own as a buffer (Linux's userptr: hakios-metal, Turnip) */
	kHakiosHost            = 0x7c,         /* ours: pages of the caller's own memory as a buffer (Metal's no-copy buffers) */
	kHakiosGPUWorked       = 0x7d,         /* ours: how long the 3D core itself has worked, and on how many */
	kHakiosSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kHakiosShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hakios_user_memory {
	uint64_t address, size;                 /* in the caller, in whole pages */
	uint32_t handle;                        /* back: the buffer */
	uint32_t flags;
};
/*
 * kHakiosHost: `bytes` of the caller's memory from `address` (both 4 KB aligned) as a buffer: the pages are wired and entered
 * into the GPU's address space for as long as the buffer is there, and pushed out of the cache before every submission that
 * names the buffer (the memory is the program's ordinary, cached memory). Back: the buffer.
 */
struct hakios_host {
	uint64_t address;
	uint64_t bytes;
	uint32_t handle;
	uint32_t flags;                         /* MSM_BO_GPU_READONLY, or 0 */
};
#define GPU_BUFFER_HOST         0x40000000u     /* in gpu_buffer.flags: a kHakiosHost buffer (never kept for another) */

struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHakiosDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};
/*
 * kHakiosGPUWorked: the GPU's own side of a measurement a program makes with its own clock.
 *
 * A program timing this driver (components/metal/src/metal_drv_test.m --time) can measure only wall
 * time: the whole of a draw, which is its own work, this ioctl, the submission, the core's
 * execution and the wait. Nothing it can reach says how much of that was the core -- and the
 * kernel has known all along, because bsd/dev/board/adreno.c times every submission off
 * CP_ALWAYS_ON_COUNTER (19.2 ticks a microsecond) and until now only ever printed it. This
 * request hands the same three numbers to the program, so the subtraction happens where the
 * two halves are both in hand instead of in whoever reads the log afterwards.
 *
 * It RESETS what it reports, which is hakios_gpu_worked()'s own contract ("since this was
 * last asked") and is what makes it useful: a caller reads once to zero the counters, does the
 * work it wants to price, and reads again. Two consequences, and they are the whole of the
 * sharp edge: two programs asking at once each get a part of the truth, and the kernel's own
 * five-second report in submit() below shares the same counters -- so a boot that reads this
 * will see gaps in those lines, and that is expected rather than a fault.
 */
struct hakios_worked {
	uint64_t microseconds;                  /* the core's own time on the submissions counted here */
	uint32_t submissions;                   /* how many of them it timed; 0 means nothing finished since */
	uint32_t by[7];                         /* ... of which under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
};

#define kHakiosDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_SET_IOVA       4
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define MSM_SUBMIT_BO_READ      0x0001          /* a submission's buffer flags: read by it ... */
#define MSM_SUBMIT_BO_WRITE     0x0002          /* ... written by it */
#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_IB_WORDS_MOST       0xfffffu        /* CP_INDIRECT_BUFFER's size field: 20 bits */

/*
 * Whether a refusal of a client's request is still said in the log: the first 64 of a boot. Any program may open the GPU
 * and ask what it likes as often as it likes, and every refusal was a line -- the kernel's log filled by one loop.
 */
static bool
gpuRefusalSaid(void)
{
	static volatile uint32_t said;

	return OSIncrementAtomic((volatile SInt32 *) &said) < 64;
}
#define GPU_SUBMIT_BOS          8192
#define GPU_SHOWN_WAIT_MS       30              /* a submission waits for the display's copy of its surface this long at most */
#define GPU_SUBMIT_BO_TRACK     0x8000u         /* a submission's buffer flag (Turnip's darwin transport): named for the surface's
                                                 * "drawn" mark only, not to be pushed out of the cache here (the plugin does) */
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting for the core: how long between two looks at how far it is */
#define GPU_PUSH_AGAIN_MS       16              /* a surface is pushed out of the cache again for a submission this long after the last (4 ms was 8 MB a submission, 2.4 GB/s on the home screen; 100 ms left a writer that does not say so stale for a tenth of a second, black or old blocks that flicker: one frame) */

static IOLock * gGPULock;                       /* around everything that is the GPU's (hakios_gpu_...) */
static bool gGPUReported;
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */
/*
 * boot-arg gpu_fair_ms=N: the GPU has one ring and no preemption, so a program that hands it frame after frame (the wallpaper's
 * 40-55 ms ones, widgets) has the window server's frames wait behind all of them. Every program but the window server
 * (backboardd) then submits only once its last submission is done, or N milliseconds have gone by: at most one of its
 * submissions is ahead of the window server's next. (0: off.)
 */
static uint32_t gGPUFairMs;
extern "C" void hakios_cputop_start(void);
static uint64_t gGPUFairWaits, gGPUFairWaited;  /* submissions that waited so, and for how long together (absolute time) */
static TUNABLE(uint32_t, gGPUClientSnapshot, "gpu_client_snapshot", 1);
static bool gGPUSaidSetIova;                    /* MSM_INFO_SET_IOVA has been refused out loud once */

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
	uint32_t shownWaits;                    /* submissions that waited for the display's copy of a surface they draw into */
} gGPUStat;

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hakios_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	/*
	 * A wait of a second or more that came to nothing: the core has stopped answering. What it
	 * and its SMMU context say is put down, and then, with boot-arg gpu_recover=1, the core is
	 * reset and started again, which gives up the submission that hung and leaves later ones
	 * the core's own. Without that -- or if it does not work -- nothing is waited for again in
	 * this run: a GPU that may yet write memory of ours is given nothing more. Either way this
	 * wait has failed, and says so: the submission did not finish.
	 */
	if (milliseconds >= 1000 && !gGPUReported) {
		hakios_gpu_report("no answer");
		gGPUReported = !hakios_gpu_recover();
	}
	return false;
}

/*
 * With two rings: every one of `fences` (0s left out) done, in `milliseconds` together -- as gpuWait, of which this is
 * one call a fence, each with what is left of the time.
 */
static bool
gpuWaitList(const uint32_t * fences, unsigned count, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), ns, used;

	for (unsigned i = 0; i < count; i++) {
		if (fences[i] == 0) {
			continue;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		used = ns / NSEC_PER_MSEC;
		if (!gpuWait(fences[i], used < milliseconds ? milliseconds - used : 0)) {
			return false;
		}
	}
	return true;
}

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	uint32_t fence = surface->drawn;
	bool done = true;

	if (fence != 0 && gGPULock != NULL) {
		IOLockLock(gGPULock);
		done = !hakios_gpu_ready() || gpuWait(fence, milliseconds);
		IOLockUnlock(gGPULock);
	}
	return done;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		hakios_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left, end;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	/* the seconds are the caller's: a number too big to be nanoseconds is a time far ahead (or far behind), not a wrapped one */
	if (__builtin_mul_overflow(when->seconds, (int64_t) NSEC_PER_SEC, &end) || __builtin_add_overflow(end, when->nanoseconds, &end)) {
		return when->seconds > 0 && when->nanoseconds >= 0 ? GPU_PATIENCE_MS : 0;
	}
	if (end <= 0) {
		return 0;                       /* (the clock's reading is never negative: this is past) */
	}
	left = end - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("HakiosGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosGPU : public IOService
{
	OSDeclareDefaultStructors(HakiosGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPU, IOService);

bool
HakiosGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	setName("HakiosGPU");
	registerService();
	return true;
}

void
HakiosGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosGPU * gpu;
	uint32_t cached = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	(void)PE_parse_boot_argn("gpu_fair_ms", &gGPUFairMs, sizeof(gGPUFairMs));
	hakios_cputop_start();                         /* (boot-arg cputop=S: who the CPUs work for; bsd/dev/board/cputop.c) */
	gGPUFairMs = MIN(gGPUFairMs, 500u);             /* (a wait of a second is taken for a GPU that has stopped: gpuWait) */
	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(HakiosGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosGPU: the service did not start\n");
	}
	gpu->release();
}

/* The run is ending (board/adreno.c): nobody is in the middle of anything when the GPU goes. */
void
HakiosGPUStop(void)
{
	IOLockLock(gGPULock);
	hakios_gpu_stop();
	IOLockUnlock(gGPULock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own */
	bool                            client;         /* the client's own pages (no kernel view: out of the cache by their physical addresses) */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen;           /* by the program: mapped, and from then on written by it */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
	uint64_t                        flushSize;      /* what of it is pushed out of the cache, when not all (0: all) */
	IOMemoryMap *                   map;            /* a kHakiosHost buffer's pages in the kernel (for the cache pushes) */
	uint32_t                        flushed;        /* the submission (fFlushes) that last looked at it for the cache: once a submission */
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	IOMemoryMap *                   map;            /* gpu_buffer's */
	MiniIOSurface *                 surface;
	bool                            client;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
};
#define GPU_LEFT                64

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A buffer kept is given again as it is, not cleared: it was the same program's (Mesa's own
 * cache gives buffers back the same way). At most GPU_KEPT_BYTES are kept (the one kept longest
 * makes way), none for longer than GPU_KEPT_SECONDS. rc.jobs.fps, Search: 8 such textures made
 * slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* bytes: smaller ones are not worth it */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0 (a test): none kept; gpu_keep=2: kept ones cleared when given again */

class HakiosGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosGPUUserClient);

	task_t                  fTask;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	bool                    fClosed;                /* clientClose() gives everything back: nothing more is made (under gGPULock) */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fFlushes;               /* submissions that pushed buffers out of the cache (gpu_buffer.flushed; never 0) */
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	uint32_t                fLastFence;             /* this client's last submission */
	bool                    fYields;                /* ... which the next waits for (gpu_fair_ms): not the window server */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle,
	    uint64_t shift = 0, uint64_t length = 0);
	void freeBuffer(struct gpu_buffer * b);
	void giveBack(void);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	IOReturn hostBuffer(struct hakios_host * request);
	IOReturn userBuffer(struct hakios_user_memory * request);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn clientSurfaceBuffer(MiniIOSurface * s, struct hakios_surface * request, uint64_t began);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct hakios_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPUUserClient, IOUserClient);

bool
HakiosGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	/* (held: the requests that wire its pages name it, and a request from whoever has the connection may come after it is gone) */
	fTask = owningTask;
	task_reference(fTask);
	return true;
}

bool
HakiosGPUUserClient::start(IOService * provider)
{
	if (!hakios_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	{
		char name[32] = "";

		proc_name(proc_selfpid(), name, sizeof(name));
		fYields = strcmp(name, "backboardd") != 0;
	}
	return fBuffers != NULL;
}

/*
 * With the lock held, and the GPU done with everything it has been given (`idle`). If it is
 * not -- it has stopped answering -- the buffer's memory stays the GPU's for good: it may yet be
 * written, and must not be somebody else's by then.
 */
void
HakiosGPUUserClient::freeBuffer(struct gpu_buffer * slot)
{
	/*
	 * gpuWait() below lets go of the lock: take the buffer out of its handle first and work on a copy, so that
	 * a second close of the same handle (or the client's closing) cannot free the same memory and address twice.
	 */
	struct gpu_buffer saved = *slot;
	struct gpu_buffer * b = &saved;

	slot->memory = NULL;
	slot->surface = NULL;
	slot->map = NULL;
	slot->client = false;
	fBufferCount--;
	fBufferBytes -= b->size;

	bool busy = hakios_gpu_ready() && b->fence != 0 && !hakios_gpu_done(b->fence);

	if (busy && fLeftCount == GPU_LEFT) {
		busy = !gpuWait(b->fence, 1000);        /* (nowhere to keep it: this one is waited for) */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->map = b->map;
		left->surface = b->surface;
		left->client = b->client;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
	} else {
		if (busy) {
			static uint32_t said;

			/*
			 * The GPU may still write it, its SMMU mapping is left: the pages under it are kept for good -- the buffer's own
			 * memory (for a surface made around its maker's memory that is the snapshot or the maker's pages, not the
			 * surface's state page), and the surface itself, whose going would free the snapshot and let the maker's
			 * pages go
			 */
			b->memory->retain();
			if (b->surface != NULL) {
				b->surface->retain();
			}
			if (said++ < 8) {
				IOLog("HakiosGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
			}
		} else if (b->surface == NULL && !b->client && hakios_gpu_ready() &&
		    keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;               /* (now the kept one's) */
		} else if (hakios_gpu_ready()) {
			if (hakios_gpu_unmap(b->address, b->size)) {
				hakios_gpu_address_free(b->address, b->size);
			} else {
				if (b->memory != NULL) b->memory->retain();    /* (the 3D core may still reach them: the pages, and the addresses, stay its own) */
			}
		}
		if (b->map != NULL) {
			b->map->release();
		}
		if (b->memory != NULL) {
			b->memory->release();
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
HakiosGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (hakios_gpu_ready() && !hakios_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && !left->client && hakios_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (hakios_gpu_ready()) {
			if (hakios_gpu_unmap(left->address, left->size)) {
				hakios_gpu_address_free(left->address, left->size);
			} else {
				if (left->memory != NULL) left->memory->retain(); /* (the 3D core may still reach them: the pages, and the addresses, stay its own) */
			}
		}
		if (left->map != NULL) {
			left->map->release();
		}
		if (left->memory != NULL) {
			left->memory->release();
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

/*
 * Everything of the client's given back, with the lock held (which freeBuffer() may let go of for a while): its buffers,
 * those given back early that the GPU is done with now, and those kept for it.
 */
void
HakiosGPUUserClient::giveBack(void)
{
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	reap();
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		dropKept(&fKept[i]);
	}
}

IOReturn
HakiosGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/*
		 * Closed before anything is given back, not after: the waits here and in freeBuffer() let go of the lock, and a call
		 * on its way that took it then put its buffer into a place already passed, which nobody gave back -- wired pages,
		 * GPU addresses and a surface held for good. Now nothing is put into the table once this is set (newBuffer,
		 * gemNew) and nothing more is kept (keep).
		 */
		fClosed = true;
		/*
		 * what the client had the GPU do last may still be going on, in these buffers (its own last submission: every fence
		 * of its buffers is one of its own. Not all the GPU had been given -- another program's second of work queued behind
		 * it was waited for here, and then said to be a GPU that had stopped answering, reset or given up for the run)
		 */
		if (hakios_gpu_ready() && fLastFence != 0) {
			gpuWait(fLastFence, 1000);
		}
		giveBack();
		if (fLeftCount != 0) {
			IOLog("HakiosGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
HakiosGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		/*
		 * (what is still here: a buffer given back by a call that was waiting for the GPU while the client closed -- early,
		 * to be freed once the GPU is done -- or everything, for a client that never was closed. Given back as at close;
		 * the table is empty after clientClose() otherwise, as nothing is put into it once it has closed.)
		 */
		if (gGPULock != NULL) {
			IOLockLock(gGPULock);
			fClosed = true;
			giveBack();
			IOLockUnlock(gGPULock);
		}
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	if (fTask != NULL) {
		task_deallocate(fTask);
		fTask = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
HakiosGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HakiosGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: there is no room.
 */
struct gpu_buffer *
HakiosGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle,
    uint64_t shift, uint64_t length)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	/*
	 * (the client has closed: clientClose() is giving its buffers back, or has, and one put in now -- the memory of a call
	 * that was on its way, prepared before the lock was taken -- would be nobody's to give back: wired pages, GPU addresses
	 * and a surface held for good. The caller undoes what it made, as for no room.)
	 */
	if (fClosed) {
		return NULL;
	}
	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = hakios_gpu_address(size)) == 0) {
		if (gpuRefusalSaid()) {
			IOLog("HakiosGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		}
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount got = 0;
		addr64_t phys;

		/*
		 * `shift`: memory that does not start on a page of the GPU's (a program's own, which a
		 * surface was made around): GPU page n holds the physical page that byte n * 4096 - shift
		 * of the memory is in, so the memory's first byte is at the buffer's `shift`. No byte past
		 * `length` is asked for: the last page is the one the last byte is in.
		 */
		if (shift != 0 || length != 0) {
			uint64_t at = offset > shift ? offset - shift : 0;

			if (length != 0 && at >= length) {
				at = length - 1;
			}
			phys = memory->getPhysicalSegment(from + at, &got, kIOMemoryMapperNone) & ~(addr64_t) 4095;
		} else {
			phys = memory->getPhysicalSegment(from + offset, &got, kIOMemoryMapperNone);
		}

		mapped = phys != 0 && hakios_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	hakios_gpu_mapped();
	if (!mapped) {
		(void)hakios_gpu_unmap(b->address, size);
		hakios_gpu_address_free(b->address, size);
		IOLog("HakiosGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->flushSize = 0;
	b->flushed = 0;
	b->surface = NULL;
	b->map = NULL;
	b->client = false;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
void
HakiosGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (hakios_gpu_ready()) {
		if (hakios_gpu_unmap(k->address, k->size)) {
			hakios_gpu_address_free(k->address, k->size);
		} else {
			k->memory->retain();    /* (the 3D core may still reach them: the pages, and the addresses, stay its own) */
		}
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
HakiosGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	if (flags & GPU_BUFFER_HOST) {
		return false;                   /* the program's own pages: never another's */
	}
	if (fClosed) {
		return false;                   /* (the kept ones were given back as the client closed: none is kept after) */
	}
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
HakiosGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	IOLockLock(gGPULock);
	if (fClosed) {
		IOLockUnlock(gGPULock);
		return kIOReturnNotOpen;                /* (nothing is put in the table once the client has closed: newBuffer) */
	}
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			IOLockUnlock(gGPULock);
			if (gGPUKeep == 2 && b->kernel != NULL) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(gGPULock);
	/*
	 * The memory is no task's (pageable, wired here by prepare(), mapped only into the client that asks for it), not
	 * the kernel's: kernel buffers come from the kernel's shared data range, which the Metal clients' buffers filled until
	 * every allocation of the system's, IOSurfaces' too, failed with KERN_NO_SPACE. The zeroes are pushed out of the cache
	 * through a passing kernel mapping.
	 */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(NULL, kIODirectionInOut | kIOMemoryPageable |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory != NULL && memory->prepare() != kIOReturnSuccess) {
		memory->release();
		memory = NULL;
	}
	if (memory != NULL) {
		IOMemoryMap * passing = memory->map(kIOMapAnywhere | kIOMapDefaultCache);
		if (passing != NULL) {
			bzero((void *) passing->getVirtualAddress(), (size_t) size);
			hakios_gpu_to_memory((void *) passing->getVirtualAddress(), size);
			passing->release();
		}
	}
	if (memory == NULL) {
		/* Why: the same size from the kernel's shared data range directly, for its error */
		static uint32_t asked;
		if (asked++ < 6) {
			vm_offset_t probe = 0;
			kern_return_t kr = kmem_alloc(kernel_map, &probe, (vm_size_t) size, (kma_flags_t)(KMA_DATA_SHARED | KMA_ZERO), VM_KERN_MEMORY_IOKIT);
			IOLog("HakiosGPU: kmem_alloc of the same %llu bytes: %d; free pages %u, wired %u\n", size, kr, vm_page_free_count,
			    vm_page_wire_count);
			if (kr == KERN_SUCCESS) {
				kmem_free(kernel_map, probe, (vm_size_t) size);
			}
		}
		IOLog("HakiosGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = NULL;               /* (no kernel mapping is kept: gpu_cached=1 and gpu_keep=2 have nothing to clear) */
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::hostBuffer(struct hakios_host * request)
{
	const uint64_t start = trunc_page_64(request->address), end = round_page_64(request->address + request->bytes);
	const uint64_t from = request->address - start, size = (request->bytes + 4095) & ~4095ull;
	struct gpu_buffer * b;
	IOMemoryDescriptor * memory;
	IOReturn ret;

	/*
	 * (the size before it is rounded: bytes within 4095 of 2^64 round to a size of 0, which passed, and the GPU address
	 * given for it was given again to the next buffer)
	 */
	if (request->bytes == 0 || request->bytes > GPU_BUFFER_MAX || (request->address & 4095) != 0 || size == 0 ||
	    size > GPU_BUFFER_MAX || end <= start) {
		return kIOReturnBadArgument;
	}
	memory = IOMemoryDescriptor::withAddressRange(start, end - start, kIODirectionInOut, fTask);
	if (memory == NULL) {
		return kIOReturnNoMemory;
	}
	if ((ret = memory->prepare()) != kIOReturnSuccess) {
		memory->release();
		return ret;
	}
	IOMemoryMap * map = memory->map(kIOMapAnywhere);       /* the kernel's view, to push the program's writes out of the cache */
	if (map == NULL) {
		memory->complete();
		memory->release();
		return kIOReturnVMError;
	}
	IOLockLock(gGPULock);
	b = newBuffer(memory, from, size, GPU_BUFFER_HOST | (request->flags & MSM_BO_GPU_READONLY), &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->map = map;
		b->kernel = (uint8_t *) map->getVirtualAddress() + from;
		b->flushSize = request->bytes;
		b->seen = true;                 /* the program writes it whenever it likes: pushed out for every submission */
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		map->release();
		memory->complete();
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kHakiosDescribe), no buffer is made.
 */
IOReturn
HakiosGPUUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		return clientSurfaceBuffer(s, request, began);
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	/*
	 * (the GPU's buffer runs past the surface: freedreno lays an image out with its height aligned to four
	 * rows, and SpringBoard's 2622x1206 surfaces -- 1208 rows of 10496 bytes -- were refused for wanting
	 * 14 KB more than they have; the pages past the end repeat the last one, as for a program's memory)
	 */
	uint64_t mapped = round_page_64(size + 4 * (uint64_t)s->info.bytesPerRow) + PAGE_SIZE;

	request->size = mapped;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(s->memory, PAGE_SIZE, mapped, 0, &request->handle, 0, size)) != NULL) {
		b->memory = memory;
		b->kernel = s->kbase + PAGE_SIZE;
		b->flushSize = size;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * A surface made around memory of a program's own (IOSurfaceAddress): its pages are held in place
 * already (MiniIOSurface's prepare()), so they are entered into the GPU's address space as they
 * are. The memory need not start on a page of the GPU's: the buffer starts on the page its first
 * byte is in, and the pixels at that byte's place in it, which goes back to the program as more of
 * the offset. The rows are the program's and the GPU may not take them; that is for the program's
 * side to see in the description (rowBytes). iconservicesagent and SpringBoard hand the window
 * server every icon this way; refused, the home screen's draws had no textures and it stayed black.
 */
IOReturn
HakiosGPUUserClient::clientSurfaceBuffer(MiniIOSurface * s, struct hakios_surface * request, uint64_t began)
{
	struct gpu_buffer * b = NULL;
	uint64_t length = s->info.allocSize, shift = s->clientAddress & 4095, size;

	/* (a page more than the memory: freedreno lays an image out with its height aligned to four
	 * rows, and a 41-row icon was refused for wanting 44; the page past the end repeats the last
	 * one of the memory, so whatever reads there reads the program's own bytes) */
	size = round_page_64(shift + length) + PAGE_SIZE;
	if (length == 0 || size > GPU_BUFFER_MAX) {
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = (uint32_t) (s->info.offset + shift);
	if (gGPUClientSnapshot && s->kpixels != NULL) {          /* (as the copy below will be) */
		request->size = round_page_64(length + 4 * (uint64_t) s->info.bytesPerRow) + PAGE_SIZE;
		request->offset = (uint32_t) s->info.offset;
	}
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		MiniIOSurfaceDrop(s);
	} else if (gGPUClientSnapshot && s->snapshot != NULL) {
		/* the copy made with the surface (MiniIOSurface.h `snapshot`), again if its maker has written it since */
		IOBufferMemoryDescriptor * copy = s->snapshot;

		if (s->shared()->seed != s->snapshotSeed) {
			memcpy(copy->getBytesNoCopy(), s->kpixels, (size_t) length);
			s->snapshotSeed = s->shared()->seed;
		}
		uint64_t mapped = copy->getLength();

		request->size = mapped;
		request->offset = (uint32_t) s->info.offset;
		if ((b = newBuffer(copy, 0, mapped, 0, &request->handle)) != NULL) {
			copy->retain();
			b->memory = copy;
			b->kernel = (uint8_t *) copy->getBytesNoCopy();
			b->surface = s;
			b->seed = s->snapshotSeed;
			gGPUStat.surfaces++;
		}
	} else if (gGPUClientSnapshot && s->kpixels != NULL) {
		/*
		 * (boot-arg gpu_client_snapshot, default 1) The pixels as they are now, copied into memory of the GPU's
		 * own, rather than the program's pages themselves: iconservicesagent hands the window server an icon
		 * made around its own memory and then draws the next picture -- a label -- into that same memory, and the
		 * icons on the home screen had the labels of other icons in their lower part. The
		 * window server takes such a surface when it has been drawn; what it is to show is what was there then.
		 */
		IOBufferMemoryDescriptor * copy = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared | (gGPUCached ? 0 : kIOMapWriteCombineCache),
		    (vm_size_t) round_page_64(length + 4 * (uint64_t) s->info.bytesPerRow) + PAGE_SIZE, PAGE_SIZE);

		if (copy != NULL) {
			uint64_t mapped = copy->getLength();

			memcpy(copy->getBytesNoCopy(), s->kpixels, (size_t) length);
			request->size = mapped;
			request->offset = (uint32_t) s->info.offset;
			if ((b = newBuffer(copy, 0, mapped, 0, &request->handle)) != NULL) {
				b->memory = copy;
				b->kernel = (uint8_t *) copy->getBytesNoCopy();
				b->surface = s;
				gGPUStat.surfaces++;
			} else {
				copy->release();
			}
		}
	} else if ((b = newBuffer(s->client, 0, size, s->clientReadOnly ? MSM_BO_GPU_READONLY : 0, &request->handle, shift,
	    length)) != NULL) {
		/* (pages its maker may only read are the GPU's to read only: it reaches them past the maker's mapping, and they
		 * may be a file's, shared with every other program that maps it) */
		s->client->retain();
		b->memory = s->client;
		b->kernel = s->kpixels;
		b->flushSize = length;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * Pages of the client's own as a buffer, as Linux's userptr: wired for as long as the buffer is there,
 * entered into the GPU's address space, and seen by the kernel through a mapping of its own (for the
 * cache: they are ordinary memory, pushed out of this processor's cache at every submission that
 * names them). Vulkan's VK_EXT_external_memory_host over it (Turnip, components/metal/tools/mesa/darwin-drm): Metal's
 * no-copy buffers, CoreAnimation's backing stores.
 */
IOReturn
HakiosGPUUserClient::userBuffer(struct hakios_user_memory * request)
{
	IOMemoryDescriptor * memory = NULL;
	struct gpu_buffer * b;
	uint64_t began = mach_absolute_time();

	if ((request->address & PAGE_MASK) || (request->size & PAGE_MASK) || request->size == 0 ||
	    request->size > GPU_BUFFER_MAX) {
		static uint32_t said;

		if (said++ < 8) {
			IOLog("HakiosGPU: pages at %#llx (%llu bytes) of the client's: not whole pages of %u bytes\n",
			    request->address, request->size, (unsigned) PAGE_SIZE);
		}
		return kIOReturnBadArgument;
	}
	/* pages the client may only read (a window server's view of another program's backing store): the GPU reads them only */
	IOReturn prepared = kIOReturnNoMemory;
	for (int pass = 0; pass < 2 && prepared != kIOReturnSuccess; pass++) {
		memory = IOMemoryDescriptor::withAddressRange(request->address, request->size,
		    pass == 0 ? kIODirectionInOut : kIODirectionOut, fTask);
		if (memory == NULL) {
			IOLog("HakiosGPU: pages at %#llx (%llu bytes) of the client's: no descriptor\n", request->address, request->size);
			return kIOReturnNoMemory;
		}
		prepared = memory->prepare();
		if (prepared != kIOReturnSuccess) {
			memory->release();
			memory = NULL;
		} else if (pass == 1) {
			request->flags |= MSM_BO_GPU_READONLY;
		}
	}
	if (memory == NULL) {
		static uint32_t said;

		if (said++ < 8) {
			IOLog("HakiosGPU: pages at %#llx (%llu bytes) of the client's cannot be wired: %#x\n",
			    request->address, request->size, prepared);
		}
		return kIOReturnVMError;
	}
	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, request->size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;             /* (its pages unwired as it goes: IOGeneralMemoryDescriptor::free) */
		b->kernel = NULL;
		b->client = true;
		b->seen = true;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		static uint32_t said;

		if (said++ < 8) {
			IOLog("HakiosGPU: pages at %#llx (%llu bytes) of the client's: no room for them in the GPU\n",
			    request->address, request->size);
		}
		memory->complete();
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		/*
		 * The address the buffer was given when it was made, and the buffer's until it is
		 * given back. Nothing here ever moves a live buffer, so a caller may ask once and
		 * keep the answer -- which is what freedreno does (fd_bo_init_common).
		 */
		request->value = b->address;
	} else if (request->info == MSM_INFO_SET_IOVA) {
		/*
		 * Where a buffer goes is not the program's to choose here: every client is in the
		 * one SMMU context 0, so an address one program picked would be a place in every
		 * other program's space as well. Linux draws the line in the same place for the
		 * two parameters that describe a space -- VA_START and VA_SIZE are refused unless
		 * the client has page tables of its own (adreno_gpu.c, adreno_get_param) -- and
		 * placing a buffer is the stronger request of the two. Said once, by name, rather
		 * than returned as a bare code nobody can look up.
		 */
		if (!gGPUSaidSetIova) {
			gGPUSaidSetIova = true;
			IOLog("HakiosGPU: a program asked to place buffer %u at %#llx itself: refused. There is one "
			    "page table here for every client, so addresses are the kernel's to give and "
			    "MSM_INFO_GET_IOVA is how to learn them\n", request->handle, request->value);
		}
		ret = kIOReturnUnsupported;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		if (gpuRefusalSaid()) {
			IOLog("HakiosGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		}
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	/*
	 * A surface this submission draws into that the display's driver is copying to the panel right now: waited for, the
	 * lock let go meanwhile (MiniIOSurface shownReading). A copy takes some 10 ms at most; past GPU_SHOWN_WAIT_MS the
	 * submission goes on all the same, as before.
	 */
	for (uint32_t waited = 0; waited < GPU_SHOWN_WAIT_MS * 10; waited++) {
		bool reading = false;

		for (uint32_t i = 0; i < request->nr_bos && !reading; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);
			uint32_t flags = bos[i].flags;

			reading = b != NULL && b->surface != NULL && b->surface->shownReading != 0 &&
			    ((flags & MSM_SUBMIT_BO_WRITE) != 0 || (flags & (MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE)) == 0);
		}
		if (!reading) {
			break;
		}
		if (waited == 0) {
			gGPUStat.shownWaits++;
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(100, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	if (gGPUFairMs != 0 && fYields && fLastFence != 0 && !hakios_gpu_done(fLastFence)) {
		uint64_t waited = mach_absolute_time();

		(void)gpuWait(fLastFence, gGPUFairMs);
		gGPUFairWaits++;
		gGPUFairWaited += mach_absolute_time() - waited;
	}
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	if (++fFlushes == 0) {
		fFlushes = 1;
	}
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		/*
		 * (a buffer named again in the same submission is not pushed out again: one 256 MB buffer of the program's own named
		 * 8192 times would be 2 TB pushed out of the cache, with the lock every program's GPU work waits for held all along)
		 */
		if (b == NULL || (bos[i].flags & GPU_SUBMIT_BO_TRACK) || b->flushed == fFlushes) {
			continue;
		}
		b->flushed = fFlushes;
		if (b->surface != NULL) {
			MiniIOSurface * s = b->surface;
			uint32_t seed = s->shared()->seed;

			/* a snapshot of its maker's memory (MiniIOSurface.h): taken again only when the maker has written the
			 * surface under a lock since, and then out of the cache; never merely because time has passed, which
			 * would copy whatever the maker has drawn into that memory since */
			if (s->snapshot != NULL && b->memory == s->snapshot) {
				if (seed == s->snapshotSeed) {
					continue;
				}
				memcpy(s->snapshot->getBytesNoCopy(), s->kpixels, (size_t) s->info.allocSize);
				s->snapshotSeed = seed;
				b->seed = seed;
				b->pushed = began;
			} else if (seed == b->seed && !b->seen && began - b->pushed < again) {
				continue;
			} else {
				b->seed = seed;
				b->pushed = began;
			}
		} else if (b->client) {
			/* the client's own pages are ordinary memory: out of the cache every time, by their physical addresses */
			for (uint64_t offset = 0; offset < b->size;) {
				IOByteCount length = 0;
				addr64_t phys = b->memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);

				if (phys == 0 || length == 0) {
					break;
				}
				length = MIN(length, b->size - offset);
				flush_dcache64(phys, (unsigned) length, TRUE);
				offset += length;
			}
			gGPUStat.flushed += b->size;
			continue;
		} else if (!(b->flags & GPU_BUFFER_HOST) && (!b->seen || !gGPUCached)) {
			continue;               /* (a kHakiosHost buffer is the program's cached memory: pushed out every time) */
		}
		if (b->kernel == NULL) {
			continue;               /* (a buffer of no kernel mapping: write-combined for the program, nothing cached) */
		}
		hakios_gpu_to_memory(b->kernel, b->flushSize ? b->flushSize : b->size);
		gGPUStat.flushed += b->flushSize ? b->flushSize : b->size;
	}
	hakios_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		/*
		 * (and no more words than an indirect buffer's size field holds, 20 bits: the ring takes the count as it is, and
		 * the GPU would run it modulo 2^20 -- draws dropped, or a packet cut in two and the rest read as commands)
		 */
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size || cmds[i].size / 4 > GPU_IB_WORDS_MOST) {
			if (gpuRefusalSaid()) {
				IOLog("HakiosGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
				    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			}
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	if (count == 0) {
		/* (every one of another kind: nothing for this GPU to run, and nothing to say it ran) */
		if (gpuRefusalSaid()) {
			IOLog("HakiosGPU: a submission of %u command buffers, none of them for this GPU: not taken\n", request->nr_cmds);
		}
		IOLockUnlock(gGPULock);
		goto out;
	}
	request->fence = hakios_gpu_submit(addresses, words, count);
	/*
	 * The ring is full and the core is not reading it: it has stopped. With gpu_recover=1 it is
	 * reset and given this submission again -- after a reset the ring is empty, so there is
	 * room for it. (A wait of a second or more notices a stopped core first, as a rule; this is
	 * for a program that submits without waiting.)
	 */
	if (request->fence == 0 && !gGPUReported) {
		hakios_gpu_report("the ring is full");
		gGPUReported = !hakios_gpu_recover();
		if (!gGPUReported) {
			request->fence = hakios_gpu_submit(addresses, words, count);
		}
	}
	/* (after the submission given again, too: clientClose() and gpu_fair_ms wait for this one) */
	fLastFence = request->fence != 0 ? request->fence : fLastFence;
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands": what was given (tl: q: what it took) */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		hakios_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * A surface's "drawn" mark is the last submission that WRITES it, where the client says
	 * which (hakios-metal does: an attachment or a copy's destination is written, a texture read).
	 * Marked by a read too, a surface that a later submission only read looked drawn again when
	 * it was swapped before its own frame's submission had come (show() waits for a newer mark
	 * only when there is none), and the panel showed a picture some frames old: a dismissed alert
	 * back for one frame. freedreno names every buffer for reading and writing alike, and a buffer
	 * with neither flag is taken as written, as before.
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);
		uint32_t flags = bos[i].flags;

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL && ((flags & (MSM_SUBMIT_BO_WRITE | GPU_SUBMIT_BO_TRACK)) != 0 ||
			    (flags & (MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE)) == 0)) {
				b->surface->drawn = request->fence;
			}
		}
	}
	reap();
	if (fSubmits++ < 4) {
		/* (addresses and words are only set up to `count`: with no command buffer among the commands, the first would be stack garbage) */
		IOLog("HakiosGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    count != 0 ? addresses[0] : 0ull, count != 0 ? words[0] : 0u);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		hakios_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		IOLog("HakiosGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
		    gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
		IOLog("HakiosGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB; %u submissions waited for the display's "
		    "copy of a surface they draw into\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20, gGPUStat.shownWaits);
		if (gGPUFairMs != 0) {
			uint64_t waited;

			absolutetime_to_nanoseconds(gGPUFairWaited, &waited);
			IOLog("HakiosGPU: %llu submissions waited for their program's last first (gpu_fair_ms=%u), %llu us each\n",
			    gGPUFairWaits, gGPUFairMs, gGPUFairWaits ? waited / 1000 / gGPUFairWaits : 0);
			gGPUFairWaits = gGPUFairWaited = 0;
		}
		if (gpu_va != 0) {
			/*
			 * The address space itself, because nothing else said anything about it: what
			 * argument buffers and heaps are built on is that this allocator holds, and
			 * "lost" and "refused" are the two ways it stops holding (bsd/dev/
			 * board/adreno.c, hakios_gpu_address_free).
			 */
			uint64_t used, largest, lost;
			uint32_t live, refused;
			unsigned holes, most;

			hakios_gpu_space(&used, &live, &largest, &holes, &most, &lost, &refused);
			IOLog("HakiosGPU: addresses: %llu MB in %u buffers, longest run free %llu MB, %u holes "
			    "(%u at most, of 1024), %llu KB lost, %u allocations refused\n", used >> 20, live,
			    largest >> 20, holes, most, lost >> 10, refused);
		}
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/*
 * Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn.
 *
 * Only for root (the tests run as root, from launchd's demo job): any other program could paint what it liked over
 * every other's on the panel -- a password prompt, say -- and have the kernel take as much memory as it asked for each
 * time. And no more of the picture is taken in than the panel shows: the rows and columns past its edges, which
 * MiniIOMobileFramebufferShow() would cut off, are not copied in at all.
 */
IOReturn
HakiosGPUUserClient::show(const struct hakios_show * request)
{
	uint32_t panelWidth = 0, panelHeight = 0, columns, rows;
	uint64_t size;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	if (clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
		static uint32_t said;

		if (said++ < 4) {
			char name[32] = "?";

			proc_name(proc_selfpid(), name, sizeof(name));
			IOLog("HakiosGPU: %s[%d] asks to put pixels on the panel and is not root: refused\n", name, proc_selfpid());
		}
		return kIOReturnNotPrivileged;
	}
	MiniIOMobileFramebufferPanel(&panelWidth, &panelHeight);
	if (panelWidth == 0 || panelHeight == 0) {
		return kIOReturnSuccess;                /* (no panel to show it on: nothing is shown, as before) */
	}
	columns = MIN(request->width, panelWidth);
	rows = MIN(request->height, panelHeight);
	size = (uint64_t) columns * 4 * rows;           /* (at most the panel's own size) */
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (request->rowBytes == 4 * columns) {
		/* (rows that follow each other without a gap: all of them at once) */
		if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
			ret = kIOReturnVMError;
		}
	} else {
		for (uint32_t y = 0; y < rows && ret == kIOReturnSuccess; y++) {
			if (copyin((user_addr_t) (request->pixels + (uint64_t) y * request->rowBytes), pixels + (size_t) y * columns * 4,
			    (size_t) columns * 4) != 0) {
				ret = kIOReturnVMError;
			}
		}
	}
	if (ret == kIOReturnSuccess) {
		MiniIOMobileFramebufferShow(pixels, columns, rows, columns * 4);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HakiosGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = fClosed ? NULL : buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct hakios_surface surface;
		struct hakios_host host;
		struct hakios_user_memory user;
		struct hakios_show show;
		struct hakios_worked worked;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (fClosed) {
		/* (its buffers were all given back. Read without the lock, this is only the quick answer: a call that passed it as
		 * the client closed is refused under the lock, where a buffer would go into the table: newBuffer, gemNew) */
		return kIOReturnNotOpen;
	}
	if (!hakios_gpu_ready() && selector != kHakiosShow) {
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		ret = hakios_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with every submission that named it */
		/*
		 * (with all the GPU had been given, before: anybody's -- a widget's wait for its own buffer waited for the wallpaper's
		 * 40-55 ms frames too. A surface's last submission may be another program's: the later of the two.)
		 */
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		{
			struct gpu_buffer * b = buffer(r.cpuPrep.handle);
			uint32_t fence = b != NULL ? b->fence : hakios_gpu_last();
			bool done;

			if (b != NULL && b->surface != NULL && (int32_t)(b->surface->drawn - fence) > 0) {
				fence = b->surface->drawn;
			}
			done = gpuWait(fence, patience);
			if (!done) {
				ret = kIOReturnBusy;
				if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
					waitOver(&r.cpuPrep.timeout, fence);
				}
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		/* a fence that was never issued is never done: refuse it, so that waiting for it cannot make gpuWait() declare the GPU dead for every client */
		if ((int32_t)(r.waitFence.fence - hakios_gpu_last()) > 0) {
			IOLockUnlock(gGPULock);
			ret = kIOReturnBadArgument;
			break;
		}
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		IOLockUnlock(gGPULock);
		break;
	case kHakiosGPUWorked:
		/*
		 * Under gGPULock because hakios_gpu_worked() calls gpu_times(), which reads the
		 * scratch page and advances the same counters the submit path advances -- and
		 * submit() holds this lock while it does exactly that. The output goes back
		 * through the common copy at the end of this method, so a caller that passes a
		 * struct of this size in gets it filled on the way out; one that passes fewer
		 * bytes gets as many as it asked for, and the rest of the numbers are lost with
		 * the reset, which is the one way to misuse this request.
		 */
		IOLockLock(gGPULock);
		hakios_gpu_worked(&r.worked.microseconds, &r.worked.submissions, r.worked.by);
		IOLockUnlock(gGPULock);
		break;
	case kHakiosSurface: {
		static uint32_t said;
		uint32_t asked = r.surface.handle;

		ret = surfaceBuffer(&r.surface);
		/* (the first few, and every refusal of the first forty: which surfaces a program could not have) */
		if (said < 40 && (ret != kIOReturnSuccess || said < 12)) {
			said++;
			IOLog("HakiosGPU: surface %u%s for pid %d: %#x (%ux%u '%c%c%c%c', %u a row, offset %u, %llu bytes)\n",
			    r.surface.surface, asked == kHakiosDescribe ? " described" : "", proc_selfpid(), ret, r.surface.width,
			    r.surface.height, (char) (r.surface.pixelFormat >> 24), (char) (r.surface.pixelFormat >> 16),
			    (char) (r.surface.pixelFormat >> 8), (char) r.surface.pixelFormat, r.surface.rowBytes,
			    r.surface.offset, r.surface.size);
		}
		break;
	}
	case kHakiosShow:
		ret = show(&r.show);
		break;
	case kHakiosHost:
		ret = hostBuffer(&r.host);
		break;
	case kHakiosUserMemory:
		ret = userBuffer(&r.user);
		break;
	default:
		if (gpuRefusalSaid()) {
			IOLog("HakiosGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		}
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
