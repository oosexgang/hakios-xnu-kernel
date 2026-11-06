/*
 * RedwoodMali: the Infinix's Mali-G52 for programs to draw with.
 *
 * bsd/dev/redwood_mali.c brings the GPU up (boot-arg mali=1) and, once a job has run through its
 * page tables, starts this service. A program that opens it speaks the interface of Linux's
 * panfrost DRM driver, the one Mesa's panfrost knows: every request is a call of the user client
 * with the request's number as the selector and its structure passed in and back -- as RedwoodGPU
 * does it for the other phone's Adreno and freedreno. What Linux does with mmap() on the device is
 * the client memory of the type that is the buffer's handle.
 *
 * What there is: parameters, buffers (made, entered into the GPU's address space, told their
 * address, mapped, given back), and job chains. A buffer is wired kernel memory, zero when made and
 * write-combined, so what the program writes goes to memory where the GPU reads it. A job chain is
 * run to its end before the call returns, so waiting for a buffer has nothing left to wait for, and
 * the synchronisation objects Mesa asks about can all be answered "signalled" in the program
 * (tools/mesa/redwood-drm). Not here yet: jobs that overlap, an address space for each program,
 * interrupts, buffers shared between programs.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void redwood_keep(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/redwood_mali.c */
bool redwood_mali_ready(void);
bool redwood_mali_parameter(uint32_t which, uint64_t * value);
uint64_t redwood_mali_address(uint64_t size);
bool redwood_mali_map(uint64_t gpu, uint64_t phys);
void redwood_mali_mapped(void);
void redwood_mali_unmap(uint64_t gpu, uint64_t size);
void redwood_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset);
uint32_t redwood_mali_submit(uint64_t chain, bool fragment);
uint64_t redwood_mali_enqueue(uint64_t chain, bool fragment);
bool redwood_mali_wait(uint64_t seq, uint64_t timeout_ns);

void RedwoodMaliStart(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);

/* what programs have done with the GPU so far, for bsd/dev/redwood_mali.c to repeat in the log */
uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;
/* the first buffers ever made, for matching a GPU fault's address to one of them */
struct mali_made { uint64_t address, size; uint32_t asked, flags; };
struct mali_made gMaliMade[32];
}

/* Linux's include/uapi/drm/panfrost_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBO         = 0x41,
	kPanfrostCreateBO       = 0x42,
	kPanfrostMmapBO         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBOOffset    = 0x45,
	kPanfrostMadvise        = 0x48,
	kRedwoodSurfaceSeq      = 0x7c,         /* ours: a surface's last job, or "drawing, not yet sent" (struct redwood_surface_seq) */
	kRedwoodWait            = 0x7d,         /* ours: wait for a job by its number (struct redwood_wait) */
	kRedwoodSurface         = 0x7e,         /* ours, as RedwoodGPU's: an IOSurface's memory as a buffer */
	kRedwoodShow            = 0x7f,         /* ours, as RedwoodGPU's: a test's pixels onto the panel */
};

struct redwood_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kRedwoodDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
#define kRedwoodDescribe        0xffffffffu

/* kRedwoodWait: job `seq` (what kPanfrostSubmit gave back in `jc`) done within timeout_ns, or kIOReturnTimeout */
struct redwood_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

/* kRedwoodSurfaceSeq: the job after which the surface is drawn (MiniIOSurfaceDrawn), or
 * REDWOOD_SEQ_PENDING while the Metal driver's thread has its frame still to send */
struct redwood_surface_seq {
	uint32_t surface, pad;
	uint64_t seq;
};
#define REDWOOD_SEQ_PENDING     (~0ull)

struct redwood_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

struct drm_gem_close {
	uint32_t handle, pad;
};

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
#define PANFROST_JD_REQ_FS      (1u << 0)

struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};

struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};

struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};

#define MALI_BUFFERS            4096
#define MALI_BUFFER_MAX         (256ull << 20)

struct mali_buffer {
	IOMemoryDescriptor * memory;            /* made here, or the pixels of an IOSurface (`surface`) */
	MiniIOSurface * surface;
	uint64_t address, size;
	uint64_t last_seq;                      /* the last job given it (kPanfrostSubmit's bo_handles) */
};

static IOLock * gMaliLock;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodMali : public IOService
{
	OSDeclareDefaultStructors(RedwoodMali);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodMali, IOService);

bool
RedwoodMali::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "RedwoodMaliUserClient");
	setName("RedwoodMali");
	registerService();
	return true;
}

void
RedwoodMaliStart(void)
{
	IOService * provider = IOService::getPlatform();
	RedwoodMali * gpu;

	gMaliLock = IOLockAlloc();
	gpu = OSTypeAlloc(RedwoodMali);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("RedwoodMali: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("RedwoodMali: the service did not start\n");
	}
	gpu->release();
	IOLog("RedwoodMali: the GPU is there for programs to draw with\n");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(RedwoodMaliUserClient);

	struct mali_buffer *    fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextBuffer;
	uint32_t                fSubmits, fFailed, fRequests;

	struct mali_buffer * buffer(uint32_t handle);
	void freeBuffer(struct mali_buffer * b);
	IOReturn createBO(struct drm_panfrost_create_bo * request);
	struct mali_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle);
	IOReturn surfaceBuffer(struct redwood_surface * request);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodMaliUserClient, IOUserClient);

bool
RedwoodMaliUserClient::start(IOService * provider)
{
	if (!redwood_mali_ready() || !IOUserClient::start(provider)) {
		IOLog("RedwoodMali: a client could not be started\n");
		return false;
	}
	IOLog("RedwoodMali: a client opened the GPU\n");
	OSIncrementAtomic((SInt32 *) &gMaliClients);
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	return fBuffers != NULL;
}

struct mali_buffer *
RedwoodMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* With the lock held. No job can still be using it: every job has finished before its call returned. */
void
RedwoodMaliUserClient::freeBuffer(struct mali_buffer * b)
{
	/* not while a job queued or running may still use it (jobs are run in a queue now) */
	if (b->last_seq != 0) {
		(void)redwood_mali_wait(b->last_seq, 2ull * NSEC_PER_SEC);
	}
	redwood_mali_unmap(b->address, b->size);
	b->memory->release();
	if (b->surface != NULL) {
		MiniIOSurfaceDrop(b->surface);
	}
	bzero(b, sizeof(*b));
}

/*
 * A handle and GPU addresses for `size` bytes of `memory` from byte `from` on, every page entered
 * into the GPU's tables. With the lock held; NULL: no handle, no addresses, or a page that would
 * not go in. The caller sets `memory` (and `surface`) of what comes back.
 */
struct mali_buffer *
RedwoodMaliUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle)
{
	struct mali_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL) {
		IOLog("RedwoodMali: no handles left\n");
		return NULL;
	}
	b->address = redwood_mali_address(size);
	/* every 4 KB of it: the GPU's pages, a quarter of the kernel's 16 KB ones */
	for (uint64_t offset = 0; offset < size && mapped && b->address != 0; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && redwood_mali_map(b->address + offset, phys);
	}
	if (b->address == 0 || !mapped) {
		if (b->address != 0) {
			redwood_mali_unmap(b->address, size);
		}
		bzero(b, sizeof(*b));
		IOLog("RedwoodMali: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	redwood_mali_mapped();
	if (gMaliBuffers < 32) {
		gMaliMade[gMaliBuffers] = (struct mali_made) { b->address, size, (uint32_t) size, 0 };
	}
	OSIncrementAtomic((SInt32 *) &gMaliBuffers);
	b->size = size;
	return b;
}

/* With the lock held. */
IOReturn
RedwoodMaliUserClient::createBO(struct drm_panfrost_create_bo * request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor * memory;
	struct mali_buffer * b;

	if (size == 0 || size > MALI_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("RedwoodMali: no memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	b = newBuffer(memory, 0, size, &request->handle);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	request->offset = b->address;
	redwood_mali_tag(b->address, 1, 0, 0, 0, 0, 0);
	return kIOReturnSuccess;
}

/*
 * The pixels of an IOSurface as a buffer, from the page after the surface's shared state on
 * (MiniIOSurface.h): what the window server draws into, and what is shown. The surface is held
 * for as long as the buffer is there; a surface that is a buffer of this client's already is that
 * buffer again. Asked only to describe (kRedwoodDescribe), no buffer is made. As RedwoodGPU's.
 */
IOReturn
RedwoodMaliUserClient::surfaceBuffer(struct redwood_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct mali_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {                /* a program's own memory with a surface around it: not whole pages */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > MALI_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kRedwoodDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	b = newBuffer(s->memory, PAGE_SIZE, size, &request->handle);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	b->surface = s;
	redwood_mali_tag(b->address, 2, s->info.width, s->info.height, s->info.bytesPerRow, s->info.pixelFormat,
	    (uint32_t) s->info.offset);
	return kIOReturnSuccess;
}

IOReturn
RedwoodMaliUserClient::clientClose(void)
{
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; fBuffers != NULL && i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	IOLockUnlock(gMaliLock);
	if (fSubmits != 0) {
		IOLog("RedwoodMali: a client goes after %u job chains, %u of them unfinished\n", fSubmits, fFailed);
	}
	terminate();
	return kIOReturnSuccess;
}

void
RedwoodMaliUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
RedwoodMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	return ret;
}

IOReturn
RedwoodMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panfrost_submit submit;
		struct drm_panfrost_wait_bo wait;
		struct drm_panfrost_create_bo create;
		struct drm_panfrost_mmap_bo mmap;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
		struct redwood_show show;
		struct redwood_surface surface;
		struct redwood_wait waitseq;
		struct redwood_surface_seq sseq;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!redwood_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	/* the two waits, without holding everybody else up */
	if (selector == kRedwoodWait || selector == kPanfrostWaitBO) {
		uint64_t seq = r.waitseq.seq;
		int64_t timeout = r.waitseq.timeout_ns;

		if (selector == kPanfrostWaitBO) {
			IOLockLock(gMaliLock);
			b = buffer(r.wait.handle);
			seq = b != NULL ? b->last_seq : 0;
			IOLockUnlock(gMaliLock);
			timeout = r.wait.timeout_ns;
			if (b == NULL) {
				return kIOReturnNotFound;
			}
		}
		ret = seq == 0 || redwood_mali_wait(seq, timeout > 0 ? (uint64_t) timeout : 0) ? kIOReturnSuccess : kIOReturnTimeout;
		if (args->structureOutput != NULL && args->structureOutputSize >= size) {
			memcpy(args->structureOutput, &r, size);
			args->structureOutputSize = (uint32_t) size;
		}
		return ret;
	}
	IOLockLock(gMaliLock);
	if (fRequests++ < 32) {                 /* the first ones a client makes, for following it from the log */
		IOLog("RedwoodMali: request %#x, %zu bytes\n", selector, size);
	}
	switch (selector) {
	case kPanfrostGetParam:
		if (!redwood_mali_parameter(r.param.param, &r.param.value)) {
			ret = kIOReturnBadArgument;
		}
		break;
	case kPanfrostCreateBO:
		ret = createBO(&r.create);
		break;
	case kPanfrostMmapBO:                   /* the offset is the handle: the program maps client memory of that type */
		ret = buffer(r.mmap.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.mmap.offset = (uint64_t) r.mmap.handle << 12;
		break;
	case kPanfrostGetBOOffset:
		b = buffer(r.offset.handle);
		ret = b != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.offset.offset = b != NULL ? b->address : 0;
		break;
	case kPanfrostSubmit: {
		/* queued, not run here: the job's number goes back in `jc`, and every buffer it names
		 * remembers it (kPanfrostWaitBO, and no buffer is freed under a job that uses it) */
		uint64_t seq = 0;

		fSubmits++;
		gMaliSubmits++;
		if (r.submit.jc != 0) {
			seq = redwood_mali_enqueue(r.submit.jc, r.submit.requirements & PANFROST_JD_REQ_FS);
		}
		for (uint32_t i = 0; seq != 0 && i < r.submit.bo_handle_count && i < 4096; i += 64) {
			uint32_t handles[64];
			uint32_t n = r.submit.bo_handle_count - i < 64 ? r.submit.bo_handle_count - i : 64;

			if (copyin((user_addr_t) (r.submit.bo_handles + i * sizeof(uint32_t)), handles, n * sizeof(uint32_t)) != 0) {
				break;
			}
			for (uint32_t k = 0; k < n; k++) {
				b = buffer(handles[k]);
				if (b != NULL) {
					b->last_seq = seq;
					if (b->surface != NULL) {
						b->surface->maliSeq = seq;      /* (MiniIOSurfaceDrawn) */
					}
				}
			}
		}
		r.submit.jc = seq;
		break;
	}
	case kRedwoodSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kRedwoodSurfaceSeq: {
		MiniIOSurface * surface = MiniIOSurfaceCopy(r.sseq.surface);

		if (surface != NULL) {
			/* drawing begins into a surface the display still holds (queued or being copied): what it
			 * shows would be part this frame, part the next -- counted, the first few by name */
			static uint32_t overDisplayed;

			if (r.sseq.seq == ~0ull && surface->useCount[0] != 0 && ++overDisplayed <= 8) {
				redwood_keep("RedwoodMali: surface %u drawn into while the display holds it (%u so far)",
				    r.sseq.surface, overDisplayed);
			} else if (r.sseq.seq == ~0ull && surface->useCount[0] != 0 && (overDisplayed & 255) == 0) {
				redwood_keep("RedwoodMali: surfaces drawn into while the display holds them: %u", overDisplayed);
			}
			surface->maliSeq = r.sseq.seq;
			MiniIOSurfaceDrop(surface);
			wakeup((caddr_t) &surface->maliSeq);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	}
	case kRedwoodShow: {
		struct redwood_show * show = (struct redwood_show *) &r;
		uint64_t bytes = (uint64_t) show->rowBytes * show->height;
		uint8_t * pixels;

		if (show->width == 0 || show->width > 4096 || show->height == 0 || show->height > 4096 ||
		    show->rowBytes < 4 * show->width || (pixels = (uint8_t *) IOMallocData(bytes)) == NULL) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (copyin((user_addr_t) show->pixels, pixels, bytes) != 0) {
			ret = kIOReturnVMError;
		} else {
			MiniIOMobileFramebufferShow(pixels, show->width, show->height, show->rowBytes);
			gMaliShown++;
		}
		IOFreeData(pixels, bytes);
		break;
	}
	case kPanfrostWaitBO:                   /* every job has finished before its submission returned */
		ret = buffer(r.wait.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		break;
	case kPanfrostMadvise:
		r.madvise.retained = 1;
		break;
	case kDrmGemClose:
		b = buffer(r.close.handle);
		if (b != NULL) {
			freeBuffer(b);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	default:
		IOLog("RedwoodMali: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gMaliLock);

	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return ret;
}
