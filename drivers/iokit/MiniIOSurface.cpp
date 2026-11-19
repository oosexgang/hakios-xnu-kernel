/*
 * MiniIOSurface: a stand-in for Apple's closed IOSurface.kext (see MiniIOSurface.h).
 *
 * A surface is a piece of wired kernel memory: one page of state that clients read directly,
 * followed by the pixels. Every client that knows a surface has the whole of it mapped once.
 * Surfaces travel between processes as Mach send rights (IOSurfaceSendRight) or by number.
 *
 * What is not here: child surfaces, wrapping of client memory, purgeability (what is marked
 * volatile is only counted: MiniIOSurfaceVolatile()), compression, notifications. Calls that are
 * not implemented are logged once and refused.
 */

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOKitServer.h>                  /* ipc_port_copy_send_mqueue: a send right of its own for each waiting notification */
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/thread_call.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
/* hakios: once user space owns the console the kernel's IOLog reaches no log we can read: say it in the loader's */
#define IOLog(...) hakios_tl(__VA_ARGS__)

static_assert(sizeof(struct minisurf_plane) == 0x80, "plane layout");
static_assert(sizeof(struct minisurf_result) == 0x68, "result layout");
static_assert(offsetof(struct minisurf_shared, attachments) == 0x30, "shared layout");
static_assert(sizeof(struct minisurf_result) + 20 * sizeof(struct minisurf_plane) == MINISURF_RESULT_MAX, "result size");

/* IOConnectCallMethod selectors of IOSurfaceRootUserClient (Darwin 16.5) */
enum {
	kSurfaceMethodCreate            = 0,
	kSurfaceMethodLookup            = 4,
	kSurfaceMethodSetYCbCrMatrix    = 5,
	kSurfaceMethodWrapClientImage   = 6,
	kSurfaceMethodWrapClientMemory  = 7,
	kSurfaceMethodSetValue          = 9,
	kSurfaceMethodCopyValue         = 10,
	kSurfaceMethodRemoveValue       = 11,
	kSurfaceMethodBindAccel         = 12,
	kSurfaceMethodInit              = 13,
	kSurfaceMethodLog               = 19,
	kSurfaceMethodSetPurgeable      = 20,
	kSurfaceMethodSetOwnership      = 21,
	kSurfaceMethodSetTiled          = 22,
	kSurfaceMethodSetTimestamp      = 24,
	kSurfaceMethodSetBulkAttachments = 27,
	kSurfaceMethodCommPageAddress   = 32,
	kSurfaceMethodSetIndexedTimestamp = 33,
	kSurfaceMethodLookupFromPort    = 34,
	kSurfaceMethodCreatePort        = 35,
	kSurfaceMethodEventCreate       = 36,   /* IOSurfaceSharedEvent: in options; out its port, its value's address here */
	kSurfaceMethodEventMap          = 38,   /* in an event's port; out -, its value's address here, its id, 0 */
	kSurfaceMethodEventNotify       = 39,   /* in the event's port, the listener, the event object, the block, the value */
	kSurfaceMethodEventAddListener  = 40,   /* async: the listener's notification port and its reference (callback, listener) */
	kSurfaceMethodEventRemoveListener = 41, /* in the listener */
	kSurfaceMethodDetachModeCode    = 43,
	kSurfaceMethodSetOwnershipIdentity = 44,
};

/* IOConnectTrap indices */
enum {
	kSurfaceTrapIncrementUseCount   = 0,
	kSurfaceTrapDecrementUseCount   = 1,
	kSurfaceTrapLock                = 2,
	kSurfaceTrapUnlock              = 3,
	kSurfaceTrapDetach              = 4,    /* the client is about to release: forget it has the surface */
	kSurfaceTrapRelease             = 5,
	kSurfaceTrapEventWait           = 6,    /* the event's port, the value, a timeout in ms */
	kSurfaceTrapEventSignal         = 7,    /* the event's port, the value */
	kSurfaceTrapCount
};

#define kSurfaceLockReadOnly    1u

#define MINISURF_TABLE_SIZE     16384
#define MINISURF_MAX_DIMENSION  16384
#define MINISURF_MAX_ALLOC      (512u << 20)
#define MINISURF_ROW_ALIGN      64
#define MINISURF_MAX_BYTES_PER_ROW (MINISURF_MAX_DIMENSION * 16)   /* what methodInit says a row may be */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static IOLock *         gSurfaceLock;
static MiniIOSurface *  gSurfaceTable[MINISURF_TABLE_SIZE];     /* index = surface number; [0] unused */
static uint32_t         gSurfaceNext = 1;
static uint32_t         gSurfaceCount;
static uint64_t         gSurfaceBytes;
static uint32_t         gSurfaceUnheld;         /* surfaces around client memory that could not be made (said a few times) */
/* what making surfaces and taking them away has cost since it was last asked (MiniIOSurfaceCost()) */
static uint64_t         gSurfaceMaking, gSurfaceFreeing;
static uint32_t         gSurfacesMade, gSurfacesFreed;
static uint64_t         gSurfaceLogged[2];                      /* selectors already complained about */
/* Boot-arg surftrace=N: log the first N surfaces created (default 24). */
static TUNABLE(uint32_t, gSurfaceTrace, "surftrace", 24);
static uint32_t         gSurfaceTraced;                         /* (a tunable is read-only after boot) */
/*
 * Boot-arg surftrace_of=NAME: only the surfaces made by programs whose name begins with NAME count for
 * surftrace (then up to N of theirs), and what becomes of them is said as well: the ports made for them
 * and who looks them up from a port (another program, which shows it: the window server for a layer).
 */
static char             gSurfaceTraceOf[32];

extern "C" void loader_log_line(const char *line);

/* A line about a traced surface: to the loader's log too, where it outlives the kernel's own, short log. */
static void
traceLine(const char * format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (gSurfaceTraceOf[0] != 0) {
		loader_log_line(line);
	}
	IOLog("%s", line);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndStructors(MiniIOSurface, OSObject);

/*
 * The memory of the big surfaces is kept when they go, a few pieces of it, and given to the
 * next surface of the same size. CoreAnimation makes a surface of the whole display for every
 * frame and frees one: 14 MB got, cleared, entered into the kernel's map and taken out of it
 * again, sixty times a second -- 2.8 ms to make and 2 ms to free each, a quarter of the time
 * there is for a frame, on the thread that draws it. A piece that is kept is cleared again
 * before it is given out: a new surface is all zeroes, whoever makes it. The clearing is done
 * by the display's driver between two frames (MiniIOSurfaceClearKept(): while it copies a
 * frame to the panel the memory is busy enough, and the copy took half as long again when
 * this ran beside it), or a moment later by a thread of this file's if no frames are going
 * out, or by whoever asks for the memory if that comes first.
 */
#define SURFACE_KEEP_FROM       (4u << 20)      /* bytes: smaller ones are not worth it */
#define SURFACE_KEPT            4
#define SURFACE_CLEAR_MS        40              /* kept memory is cleared this long after, if the display's driver has not done it */

static struct {
	IOBufferMemoryDescriptor *      memory;
	bool                            clear;          /* cleared since it was last a surface's */
	bool                            busy;           /* being cleared right now */
	uint64_t                        since;          /* when it was kept */
} gSurfaceKept[SURFACE_KEPT];
static uint32_t         gSurfacesNotKept[2];    /* memory not kept: somebody else had it too; it put out a piece kept longer */
static IOLock *         gSurfaceKeptLock;
static thread_call_t    gSurfaceClearing;
static uint32_t         gSurfacesGivenAgain;    /* for MiniIOSurfaceCost() */
/* boot-arg surf_clear=0: kept memory is given out as it is (to see what the clearing costs; a surface is then not all zeroes) */
static TUNABLE(uint32_t, gSurfaceClear, "surf_clear", 1);

/*
 * Zeroes the processor wrote are in its cache, dirty, until they are written back: the GPU then draws into the memory
 * underneath, and the display's driver cleans and invalidates the surface before it copies it out -- which could write
 * those zeroes back OVER the GPU's frame. Memory a surface gets is written back to memory as soon as it has been zeroed.
 */
static void
surfaceMemoryWriteBack(const void * from, vm_size_t bytes)
{
	for (uintptr_t line = (uintptr_t) from & ~(uintptr_t) 63; line < (uintptr_t) from + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
MiniIOSurfaceClearKept(void)
{
	if (gSurfaceKeptLock == NULL || !gSurfaceClear) {
		return;
	}
	IOLockLock(gSurfaceKeptLock);
	for (unsigned i = 0; i < SURFACE_KEPT; i++) {
		IOBufferMemoryDescriptor * memory = gSurfaceKept[i].memory;

		if (memory == NULL || gSurfaceKept[i].clear || gSurfaceKept[i].busy) {
			continue;
		}
		gSurfaceKept[i].busy = true;
		IOLockUnlock(gSurfaceKeptLock);
		bzero(memory->getBytesNoCopy(), memory->getCapacity());
		surfaceMemoryWriteBack(memory->getBytesNoCopy(), memory->getCapacity());
		IOLockLock(gSurfaceKeptLock);
		gSurfaceKept[i].busy = false;
		gSurfaceKept[i].clear = true;
	}
	IOLockUnlock(gSurfaceKeptLock);
}

static void
surfaceMemoryClear(thread_call_param_t unused0 __unused, thread_call_param_t unused1 __unused)
{
	MiniIOSurfaceClearKept();
}

/* Memory for a surface: the state page and `bytes` after it, all zeroes. */
static IOBufferMemoryDescriptor *
surfaceMemory(vm_size_t bytes)
{
	IOBufferMemoryDescriptor * memory = NULL;
	bool clear = false;

	if (bytes >= SURFACE_KEEP_FROM && gSurfaceKeptLock != NULL) {
		IOLockLock(gSurfaceKeptLock);
		for (int pass = 0; pass < 2 && memory == NULL; pass++) {
			/* one that is clear already, if there is one */
			for (unsigned i = 0; i < SURFACE_KEPT && memory == NULL; i++) {
				if (gSurfaceKept[i].memory != NULL && !gSurfaceKept[i].busy && (pass == 1 || gSurfaceKept[i].clear) &&
				    gSurfaceKept[i].memory->getCapacity() == bytes) {
					memory = gSurfaceKept[i].memory;
					clear = gSurfaceKept[i].clear;
					gSurfaceKept[i].memory = NULL;
				}
			}
		}
		gSurfacesGivenAgain += memory != NULL;
		IOLockUnlock(gSurfaceKeptLock);
		if (memory != NULL && !clear) {
			bzero(memory->getBytesNoCopy(), gSurfaceClear ? bytes : PAGE_SIZE);
			surfaceMemoryWriteBack(memory->getBytesNoCopy(), gSurfaceClear ? bytes : PAGE_SIZE);
		}
	}
	if (memory == NULL) {
		memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, bytes,
		    PAGE_SIZE);
		/* (new memory is zeroed by the processor too, page by page) */
		if (memory != NULL) {
			surfaceMemoryWriteBack(memory->getBytesNoCopy(), memory->getCapacity());
		}
	}
	return memory;
}

/*
 * A surface is gone: its memory is kept for the next, if it is big and nobody else has it. When
 * all the places are taken, it takes the place of the piece kept longest: pieces of a size nobody
 * asks for again (a big picture made once) once stayed there for good, and then no frame's memory
 * was kept any more -- each made afresh and freed (2 to 4 ms, and 1 to 2) instead of 0.3 and 0.05.
 */
static void
surfaceMemoryBack(IOBufferMemoryDescriptor * memory)
{
	IOBufferMemoryDescriptor * evicted = NULL;

	if (memory->getCapacity() >= SURFACE_KEEP_FROM && memory->getRetainCount() != 1) {
		OSIncrementAtomic(&gSurfacesNotKept[0]);
	}
	if (memory->getCapacity() >= SURFACE_KEEP_FROM && memory->getRetainCount() == 1 && gSurfaceKeptLock != NULL) {
		int oldest = -1;

		IOLockLock(gSurfaceKeptLock);
		for (unsigned i = 0; i < SURFACE_KEPT && memory != NULL; i++) {
			if (gSurfaceKept[i].memory == NULL) {
				gSurfaceKept[i].memory = memory;
				gSurfaceKept[i].clear = gSurfaceKept[i].busy = false;
				gSurfaceKept[i].since = mach_absolute_time();
				memory = NULL;
			} else if (!gSurfaceKept[i].busy && (oldest < 0 || gSurfaceKept[i].since < gSurfaceKept[oldest].since)) {
				oldest = (int) i;
			}
		}
		if (memory != NULL && oldest >= 0) {
			evicted = gSurfaceKept[oldest].memory;
			gSurfaceKept[oldest].memory = memory;
			gSurfaceKept[oldest].clear = gSurfaceKept[oldest].busy = false;
			gSurfaceKept[oldest].since = mach_absolute_time();
			memory = NULL;
			gSurfacesNotKept[1]++;
		}
		IOLockUnlock(gSurfaceKeptLock);
		if (evicted != NULL) {
			evicted->release();
		}
		if (memory == NULL) {
			if (gSurfaceClear) {
				AbsoluteTime deadline;

				clock_interval_to_deadline(SURFACE_CLEAR_MS, kMillisecondScale, &deadline);
				thread_call_enter_delayed(gSurfaceClearing, deadline);
			}
			return;
		}
	}
	memory->release();
}

void
MiniIOSurface::free(void)
{
	uint64_t began = mach_absolute_time();

	if (traced && kpixels != NULL) {
		uint32_t nonzero = 0, middle = 0;

		for (uint32_t at = 0; at + 4 <= info.allocSize; at += 64) {
			nonzero += kpixels[at] != 0 || kpixels[at + 1] != 0 || kpixels[at + 2] != 0 || kpixels[at + 3] != 0;
		}
		memcpy(&middle, kpixels + (info.allocSize / 2 & ~3u), 4);
		traceLine("MiniIOSurface: surface %u (%ux%u) goes: of every 64 bytes %u of %u begin with a pixel that is not 0, one in "
		    "the middle 0x%08x\n", sid, info.width, info.height, nonzero, info.allocSize / 64, middle);
	}

	OSSafeReleaseNULL(values);
	OSSafeReleaseNULL(snapshot);
	OSSafeReleaseNULL(clientMap);
	if (client != NULL) {
		client->complete();
		client->release();
		client = NULL;
	}
	if (memory != NULL) {
		surfaceMemoryBack(memory);
		memory = NULL;
	}
	OSObject::free();
	OSAddAtomic64((SInt64) (mach_absolute_time() - began), (SInt64 *) &gSurfaceFreeing);
	OSIncrementAtomic(&gSurfacesFreed);
}

/* How many surfaces were made and freed since this was last asked, and the microseconds each took on average. */
void
MiniIOSurfaceCost(uint32_t * made, uint64_t * making, uint32_t * freed, uint64_t * freeing, uint32_t * again)
{
	uint64_t nanos;

	*again = OSBitAndAtomic(0, &gSurfacesGivenAgain);

	*made = OSBitAndAtomic(0, &gSurfacesMade);
	*freed = OSBitAndAtomic(0, &gSurfacesFreed);
	absolutetime_to_nanoseconds(gSurfaceMaking, &nanos);
	*making = *made ? nanos / NSEC_PER_USEC / *made : 0;
	absolutetime_to_nanoseconds(gSurfaceFreeing, &nanos);
	*freeing = *freed ? nanos / NSEC_PER_USEC / *freed : 0;
	gSurfaceMaking = gSurfaceFreeing = 0;
}

/* The values changed: a new seed, and how much room a client needs to fetch them. */
void
MiniIOSurface::updateValuesState(void)
{
	struct minisurf_shared * sh = shared();
	uint32_t size = 0;

	if (values != NULL && values->getCount() != 0) {
		OSSerialize * s = OSSerialize::binaryWithCapacity(4096);
		if (s != NULL) {
			if (values->serialize(s)) {
				size = s->getLength() + 8;
			}
			s->release();
		}
	}
	sh->valuesSeed = (sh->valuesSeed + 2) | 1;
	sh->valuesSize = size;
}

static uint32_t
dictNumber(OSDictionary * dict, const char * key, uint32_t missing)
{
	OSObject * obj = dict ? dict->getObject(key) : NULL;
	OSNumber * num = OSDynamicCast(OSNumber, obj);

	if (num != NULL) {
		return num->unsigned32BitValue();
	}
	if (obj == kOSBooleanTrue) {
		return 1;
	}
	if (obj == kOSBooleanFalse) {
		return 0;
	}
	return missing;
}

static uint32_t
alignUp(uint64_t value, uint32_t to)
{
	return (uint32_t) ((value + to - 1) / to * to);
}

/* Bytes per element when the client did not say. */
static uint32_t
defaultBytesPerElement(uint32_t format)
{
	switch (format) {
	case FOURCC('L', '0', '0', '8'):
	case FOURCC('A', '0', '0', '8'):
	case 0x00000008:
		return 1;
	case FOURCC('L', '5', '6', '5'):
	case FOURCC('B', '5', '6', '5'):
	case FOURCC('L', '5', '5', '5'):
	case FOURCC('2', 'v', 'u', 'y'):
	case FOURCC('y', 'u', 'v', 's'):
	case FOURCC('y', 'u', 'v', 'f'):
	case FOURCC('L', '0', '1', '6'):
	case FOURCC('L', '0', '0', 'h'):
		return 2;
	case FOURCC('R', 'G', 'h', 'A'):
		return 8;
	case FOURCC('R', 'G', 'f', 'A'):
		return 16;
	default:
		return 4;
	}
}

/*
 * Two-plane YCbCr formats: luma, then interleaved chroma.
 * Returns false for anything else. *wide: 10-bit samples in 16 bits.
 */
static bool
biplanarLayout(uint32_t format, uint32_t * hsub, uint32_t * vsub, bool * wide)
{
	switch (format) {
	case FOURCC('4', '2', '0', 'v'): case FOURCC('4', '2', '0', 'f'):
		*hsub = 2; *vsub = 2; *wide = false; return true;
	case FOURCC('4', '2', '2', 'v'): case FOURCC('4', '2', '2', 'f'):
		*hsub = 2; *vsub = 1; *wide = false; return true;
	case FOURCC('4', '4', '4', 'v'): case FOURCC('4', '4', '4', 'f'):
		*hsub = 1; *vsub = 1; *wide = false; return true;
	case FOURCC('x', '4', '2', '0'): case FOURCC('x', 'f', '2', '0'):
		*hsub = 2; *vsub = 2; *wide = true; return true;
	case FOURCC('x', '4', '2', '2'): case FOURCC('x', 'f', '2', '2'):
		*hsub = 2; *vsub = 1; *wide = true; return true;
	case FOURCC('x', '4', '4', '4'): case FOURCC('x', 'f', '4', '4'):
		*hsub = 1; *vsub = 1; *wide = true; return true;
	default:
		return false;
	}
}

static void
describeComponents(struct minisurf_plane * plane, uint32_t format)
{
	/* in memory order; names: 1 alpha, 2 red, 3 green, 4 blue */
	static const uint8_t bgra[4] = { 4, 3, 2, 1 };
	static const uint8_t rgba[4] = { 2, 3, 4, 1 };
	const uint8_t * names;

	if (format == FOURCC('B', 'G', 'R', 'A')) {
		names = bgra;
	} else if (format == FOURCC('R', 'G', 'B', 'A')) {
		names = rgba;
	} else {
		return;
	}
	plane->componentCount = 4;
	for (unsigned i = 0; i < 4; i++) {
		plane->componentBitDepth[i] = 8;
		plane->componentBitOffset[i] = (uint8_t) (8 * i);
		plane->componentName[i] = names[i];
		plane->componentType[i] = 1;            /* unsigned integer */
		plane->componentRange[i] = 1;           /* full range */
	}
}

/*
 * Make a surface from the properties a client gave to IOSurfaceCreate().
 * Returned with one holder, and entered in the table.
 */
static MiniIOSurface *
surfaceCreate(OSDictionary * props, IOReturn * error)
{
	MiniIOSurface * s;
	struct minisurf_result * info;
	uint32_t width, height, format, bpe, ew, eh, bpr, alloc, offset, planeCount = 0;
	uint32_t hsub, vsub;
	bool wide, onePlane = false, lossless = false;
	OSArray * planeInfo;
	OSNumber * wrapped;
	uint64_t needed;

	*error = kIOReturnBadArgument;
	width = dictNumber(props, "IOSurfaceWidth", 0);
	height = dictNumber(props, "IOSurfaceHeight", 0);
	format = dictNumber(props, "IOSurfacePixelFormat", 0);
	/*
	 * Ten bits a colour, packed into 32-bit pixels: what a device with a wide-gamut display
	 * (the iPhone X is one) asks for when it wants a picture of the screen, a snapshot for a
	 * transition for instance. CoreAnimation's software renderer does not draw into those
	 * ("SW renderer invalid destination format w30r": the transition comes out black), and
	 * there is no wide gamut on this panel anyway. The surface is made as ordinary BGRA of
	 * the same geometry, and says so: whoever uses it reads the format back from the surface.
	 */
	if (format == 'w30r' || format == 'l10r' || format == 'w30R') {
		format = 'BGRA';
	}
	/*
	 * The same with alpha in a plane of its own: what CoreAnimation makes the screen's own
	 * surfaces of once it draws with a GPU's renderer (OpenGL ES, userland/src/eagl.c) instead
	 * of its software one. One plane of BGRA here too; the framebuffer shows nothing else.
	 */
	if (format == 'b3a8') {
		format = 'BGRA';
		onePlane = true;
	}
	/*
	 * Darwin 26: what CoreAnimation makes the screen's surfaces of once it draws with Metal --
	 * kCVPixelFormatType_Lossless_32BGRA, BGRA that Apple's GPU keeps compressed, described in
	 * planes (the pixels and the compression's metadata). There is no compression here: one
	 * plane of plain BGRA, which is what the display driver copies to the panel (it skipped
	 * these, a plane count of more than none, and the panel kept the loader's picture).
	 */
	if (format == '&BGA') {
		format = 'BGRA';
		onePlane = true;
		lossless = true;
	}
	ew = dictNumber(props, "IOSurfaceElementWidth", 1);
	eh = dictNumber(props, "IOSurfaceElementHeight", 1);
	bpe = dictNumber(props, "IOSurfaceBytesPerElement", 0);
	offset = dictNumber(props, "IOSurfaceOffset", 0);
	if (lossless) {
		/* (the element geometry asked for is the compressed one's: plain pixels here) */
		ew = eh = 1;
		bpe = 4;
	}
	if (width > MINISURF_MAX_DIMENSION || height > MINISURF_MAX_DIMENSION || ew == 0 || eh == 0 || ew > 255 || eh > 255) {
		return NULL;
	}
	if (props->getObject("IOSurfaceAddressRanges") != NULL) {
		IOLog("MiniIOSurface: wrapping client memory in pieces is not supported\n");
		*error = kIOReturnUnsupported;
		return NULL;
	}
	/*
	 * A surface around memory the client has already: CoreAnimation's OpenGL ES renderer makes
	 * them around pictures it has in memory (an application's icon, a layer's contents as the
	 * application has drawn them), to hand them to the GPU as they are. The surface's pixels
	 * are that memory itself, not a copy: the picture goes on being drawn into after the
	 * surface is made, and whoever looks at the surface is to see what is there now.
	 */
	wrapped = OSDynamicCast(OSNumber, props->getObject("IOSurfaceAddress"));

	s = OSTypeAlloc(MiniIOSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		*error = kIOReturnNoMemory;
		return NULL;
	}
	info = &s->info;

	planeInfo = onePlane ? NULL : OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	needed = 0;
	if (planeInfo != NULL && planeInfo->getCount() != 0) {
		planeCount = planeInfo->getCount();
		if (planeCount > MINISURF_MAX_PLANES) {
			IOLog("MiniIOSurface: %u planes asked for, %u supported\n", planeCount, MINISURF_MAX_PLANES);
			s->release();
			return NULL;
		}
		for (uint32_t i = 0; i < planeCount; i++) {
			OSDictionary * pd = OSDynamicCast(OSDictionary, planeInfo->getObject(i));
			struct minisurf_plane * p = &s->planes[i];
			uint32_t pew = dictNumber(pd, "IOSurfacePlaneElementWidth", 1);
			uint32_t peh = dictNumber(pd, "IOSurfacePlaneElementHeight", 1);

			if (pew == 0 || peh == 0 || pew > 255 || peh > 255) {
				s->release();
				return NULL;
			}
			p->width = dictNumber(pd, "IOSurfacePlaneWidth", width);
			p->height = dictNumber(pd, "IOSurfacePlaneHeight", height);
			/* a plane is no bigger than a surface may be: the sizes below are not allowed to wrap */
			if (p->width > MINISURF_MAX_DIMENSION || p->height > MINISURF_MAX_DIMENSION) {
				s->release();
				return NULL;
			}
			p->bytesPerElement = (uint16_t) dictNumber(pd, "IOSurfacePlaneBytesPerElement", bpe ? bpe : 1);
			p->elementWidth = (uint8_t) pew;
			p->elementHeight = (uint8_t) peh;
			p->bytesPerRow = dictNumber(pd, "IOSurfacePlaneBytesPerRow", 0);
			if (p->bytesPerRow == 0) {
				p->bytesPerRow = alignUp((uint64_t) ((p->width + pew - 1) / pew) * p->bytesPerElement, MINISURF_ROW_ALIGN);
			}
			p->size = dictNumber(pd, "IOSurfacePlaneSize", 0);
			if (p->size == 0) {
				/* (in 64 bits: a stride of the caller's times the rows can pass 32) */
				uint64_t planeBytes = (uint64_t) p->bytesPerRow * ((p->height + peh - 1) / peh);

				if (planeBytes > MINISURF_MAX_ALLOC) {
					s->release();
					return NULL;
				}
				p->size = (uint32_t) planeBytes;
			}
			p->offset = dictNumber(pd, "IOSurfacePlaneOffset", dictNumber(pd, "IOSurfacePlaneBase", (uint32_t) needed));
			p->extendedPixels[0] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnLeft", 0);
			p->extendedPixels[1] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnRight", 0);
			p->extendedPixels[2] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnTop", 0);
			p->extendedPixels[3] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnBottom", 0);
			if ((uint64_t) p->offset + p->size > needed) {
				needed = (uint64_t) p->offset + p->size;
			}
		}
	} else if (biplanarLayout(format, &hsub, &vsub, &wide) && width != 0 && height != 0) {
		struct minisurf_plane * y = &s->planes[0], * c = &s->planes[1];

		planeCount = 2;
		y->width = width;
		y->height = height;
		y->bytesPerElement = wide ? 2 : 1;
		y->elementWidth = y->elementHeight = 1;
		y->bytesPerRow = alignUp((uint64_t) width * y->bytesPerElement, MINISURF_ROW_ALIGN);
		y->size = y->bytesPerRow * height;
		y->offset = 0;
		c->width = (width + hsub - 1) / hsub;
		c->height = (height + vsub - 1) / vsub;
		c->bytesPerElement = wide ? 4 : 2;
		c->elementWidth = c->elementHeight = 1;
		c->bytesPerRow = alignUp((uint64_t) c->width * c->bytesPerElement, MINISURF_ROW_ALIGN);
		c->size = c->bytesPerRow * c->height;
		c->offset = y->size;
		needed = (uint64_t) c->offset + c->size;
	}

	if (bpe == 0) {
		bpe = planeCount ? s->planes[0].bytesPerElement : defaultBytesPerElement(format);
	}
	bpr = dictNumber(props, "IOSurfaceBytesPerRow", 0);
	if (lossless && bpr < (uint64_t) width * 4) {
		bpr = 0;
	}
	if (bpr == 0) {
		bpr = planeCount ? s->planes[0].bytesPerRow
		    : alignUp((uint64_t) ((width + ew - 1) / ew) * bpe, MINISURF_ROW_ALIGN);
	}
	/*
	 * A surface around its maker's memory gets a copy of that memory (`snapshot`, below) with four
	 * rows to spare after it, and the GPU's side sizes its own copy of one the same way (BoardGPU.cpp
	 * clientSurfaceBuffer): rows of the maker's bytesPerRow, which nothing above ties to the memory
	 * the maker has. A 4 KB picture said to have one row of 512 MB had the kernel ask for 2 GB of
	 * wired memory for its copy, and one of no rows said to be 4 GB less a byte long, 16 GB. A row of
	 * such a surface is no longer than methodInit says a row may be (the widest surface at 16 bytes
	 * a pixel), so the rows to spare are a megabyte at most past memory that is the maker's size.
	 * Surfaces of our own memory are left as they were: nothing is allocated past their end (the
	 * GPU sees the pages there as repeats of the last one), and their rows times their height are
	 * held to MINISURF_MAX_ALLOC below.
	 */
	if (wrapped != NULL && bpr > MINISURF_MAX_BYTES_PER_ROW) {
		s->release();
		return NULL;
	}
	if (planeCount == 0) {
		needed = (uint64_t) bpr * ((height + eh - 1) / eh);
		/* the one implicit plane, for the per-plane getters */
		s->planes[0].width = width;
		s->planes[0].height = height;
		s->planes[0].bytesPerRow = bpr;
		s->planes[0].bytesPerElement = (uint16_t) bpe;
		s->planes[0].elementWidth = (uint8_t) ew;
		s->planes[0].elementHeight = (uint8_t) eh;
		s->planes[0].size = (uint32_t) needed;
		describeComponents(&s->planes[0], format);
	}
	needed += offset;
	alloc = dictNumber(props, "IOSurfaceAllocSize", 0);
	if (alloc < needed) {
		/* (a surface around the owner's memory has the owner's size: a mask's last row is only as long as it is wide) */
		if (alloc != 0 && wrapped == NULL) {
			IOLog("MiniIOSurface: alloc size %u too small for %ux%u '%.4s', using %llu\n", alloc, width, height,
			    (const char *) &format, needed);
		}
		if (needed > MINISURF_MAX_ALLOC) {
			s->release();
			return NULL;
		}
		alloc = (uint32_t) needed;
	}
	if (alloc == 0 || alloc > MINISURF_MAX_ALLOC) {
		s->release();
		return NULL;
	}
	if (wrapped == NULL) {
		alloc = alignUp(alloc, (uint32_t) PAGE_SIZE);
	} else {
		/* (the memory is as big as its owner says, and not a byte of ours is added to it) */
		alloc = dictNumber(props, "IOSurfaceAllocSize", (uint32_t) needed);
		/* the owner's number replaced the checked one above: it gets the same limits, so that a size of 0 or of gigabytes is not taken */
		if (alloc == 0 || alloc > MINISURF_MAX_ALLOC) {
			s->release();
			return NULL;
		}
	}

	s->memory = surfaceMemory((vm_size_t) PAGE_SIZE + (wrapped ? 0 : alloc));
	if (s->memory == NULL) {
		IOLog("MiniIOSurface: no memory for %u bytes (%u surfaces, %llu MB in use)\n", alloc, gSurfaceCount,
		    gSurfaceBytes >> 20);
		s->release();
		*error = kIOReturnNoMemory;
		return NULL;
	}
	s->kbase = (uint8_t *) s->memory->getBytesNoCopy();
	s->values = OSDictionary::withCapacity(4);
	s->kpixels = s->kbase + PAGE_SIZE;
	if (wrapped != NULL) {
		s->clientTask = current_task();
		s->clientAddress = wrapped->unsigned64BitValue();
		IOReturn held = kIOReturnNoMemory;

		/* (held for writing if it can be written, which a picture that is only shown need not be) */
		for (unsigned attempt = 0; attempt < 2 && held != kIOReturnSuccess; attempt++) {
			OSSafeReleaseNULL(s->client);
			s->client = IOMemoryDescriptor::withAddressRange(s->clientAddress, alloc,
			    attempt == 0 ? kIODirectionInOut : kIODirectionOut, s->clientTask);
			held = s->client != NULL ? s->client->prepare() : kIOReturnNoMemory;
			s->clientReadOnly = attempt != 0;
		}
		if (held == kIOReturnSuccess) {
			s->clientMap = s->client->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
		} else {
			OSSafeReleaseNULL(s->client);
		}
		if (s->clientMap == NULL) {
			if (gSurfaceUnheld++ < 8) {
				IOLog("MiniIOSurface: the memory to make a surface around (%u bytes at %#llx) cannot be %s (%#x)\n", alloc,
				    s->clientAddress, held == kIOReturnSuccess ? "seen from here" : "held", held);
			}
			s->release();
			*error = kIOReturnVMError;
			return NULL;
		}
		s->kpixels = (uint8_t *) s->clientMap->getVirtualAddress();
		/* the pixels as they are now (see `snapshot`): rows to spare after them, as freedreno lays an image out
		 * with its height aligned to four rows, and the page after (BoardGPU.cpp clientSurfaceBuffer) */
		s->snapshot = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
		    (vm_size_t) round_page_64((uint64_t) alloc + 4 * (uint64_t) bpr) + PAGE_SIZE, PAGE_SIZE);
		if (s->snapshot != NULL) {
			memcpy(s->snapshot->getBytesNoCopy(), s->kpixels, alloc);
			s->snapshotSeed = s->shared()->seed;
		}
	}

	info->allocSize = alloc;
	info->width = width;
	info->height = height;
	info->bytesPerRow = bpr;
	info->offset = offset;
	info->pixelFormat = format;
	info->planeCount = planeCount;
	info->bytesPerElement = (uint16_t) bpe;
	info->elementWidth = (uint8_t) ew;
	info->elementHeight = (uint8_t) eh;
	info->cacheMode = dictNumber(props, "IOSurfaceCacheMode", 0);
	info->subsampling = (uint8_t) dictNumber(props, "IOSurfaceSubsampling", 0);
	info->prohibitUseCount = (uint8_t) dictNumber(props, "IOSurfaceProhibitUseCount", 0);
	info->sliceCount = dictNumber(props, "IOSurfaceSliceCount", 0);
	info->protectionOptions = dictNumber(props, "IOSurfaceProtectionOptions", 0);
	s->touched = mach_absolute_time();

	IOLockLock(gSurfaceLock);
	uint32_t sid = 0;
	for (uint32_t tries = 0; tries < MINISURF_TABLE_SIZE - 1; tries++) {
		uint32_t candidate = gSurfaceNext;

		gSurfaceNext = (gSurfaceNext + 1 < MINISURF_TABLE_SIZE) ? gSurfaceNext + 1 : 1;
		if (gSurfaceTable[candidate] == NULL) {
			sid = candidate;
			break;
		}
	}
	if (sid == 0) {
		IOLockUnlock(gSurfaceLock);
		IOLog("MiniIOSurface: the surface table is full\n");
		s->release();
		*error = kIOReturnNoResources;
		return NULL;
	}
	s->sid = sid;
	s->holders = 1;
	info->sid = sid;
	info->traceID = sid;
	gSurfaceTable[sid] = s;                 /* the table owns the object until the last holder goes */
	gSurfaceCount++;
	gSurfaceBytes += alloc;

	struct minisurf_shared * sh = s->shared();
	sh->registryID = 0x100000000ULL + sid;
	sh->seed = 1;
	sh->valuesSeed = 1;
	sh->pixelSizeCasting = (uint8_t) dictNumber(props, "IOSurfacePixelSizeCastingAllowed", 1);
	sh->displayable = 1;
	sh->sysMemOnly = (uint8_t) dictNumber(props, "IOSurfaceSysMemOnly", 0);
	IOLockUnlock(gSurfaceLock);

	*error = kIOReturnSuccess;
	return s;
}

/* With the lock held. */
static void
surfaceHold(MiniIOSurface * s)
{
	s->holders++;
}

/* With the lock held. The caller must not touch the surface afterwards. */
static void
surfaceUnhold(MiniIOSurface * s)
{
	if (--s->holders != 0) {
		return;
	}
	gSurfaceTable[s->sid] = NULL;
	gSurfaceCount--;
	gSurfaceBytes -= s->info.allocSize;
	s->release();
}

MiniIOSurface *
MiniIOSurfaceCopy(uint32_t sid)
{
	MiniIOSurface * s = NULL;

	if (gSurfaceLock == NULL || sid == 0 || sid >= MINISURF_TABLE_SIZE) {
		return NULL;
	}
	IOLockLock(gSurfaceLock);
	s = gSurfaceTable[sid];
	if (s != NULL) {
		surfaceHold(s);
		s->kernelHolds++;
		s->touched = mach_absolute_time();
	}
	IOLockUnlock(gSurfaceLock);
	return s;
}

/* How many surfaces there are and how much (wired) memory they hold: for the kernel's memory report. */
extern "C" void
MiniIOSurfaceUsage(uint32_t * count, uint64_t * bytes)
{
	*count = gSurfaceCount;
	*bytes = gSurfaceBytes;
}

/*
 * Surfaces marked volatile (IOSurfaceSetPurgeable): their makers have said the pixels may be thrown away and that they will
 * ask before they draw again. Apple's driver takes the memory of such surfaces when memory runs short; here every surface
 * stays wired whatever it is marked, and the state is only kept to be answered. Whether that matters is what this counts:
 * the OnePlus Nord 2 port of iOS found Photos holding 521 volatile surfaces, 312 MB, never given back while the phone
 * thrashed for memory. After the idea of the OnePlus Nord 2 port's pieces 0417 and 0418, which purge them.
 *
 * Idle, which is what such a purge could take: marked volatile, a use count of 0 in both categories, no part of the kernel
 * holding it (a swap the display has not shown yet, a GPU buffer: kernelHolds) nor the display reading it, and none of that,
 * nor its making, for SURFACE_IDLE_S. Only surfaces of our own memory are counted: one made around its maker's memory
 * (IOSurfaceAddress) is that program's memory, which it would have to give back itself.
 *
 * Why it is only counted. To give such memory back, the pixels under every client's mapping would have to be replaced at
 * once (by one page shared by all, as that port does), the memory freed, and the surface answered Empty from then on. That
 * is not safe with what this file has:
 *  - a client's mapping is of the whole of `memory`, the page of shared state first and the pixels after it, and clients
 *    read that page directly (the seed, the use counts): the pixels cannot go without moving the page, and `kbase` with
 *    it, while the GPU's driver and the display's read `shared()` and the pixels through their own pointers;
 *  - the mappings are known only to each client (fRefs), with no list of them by surface, and the GPU's driver maps the
 *    same pages into its client and its SMMU as buffers of its own (BoardGPU.cpp newBuffer);
 *  - IOMemoryMap::redirect() is made for the display's physical memory (it blocks access with a UPL and maps again as a
 *    unique mapping), and these mappings were not made unique and may be one object for two of a client's references; a
 *    mapping over part of another (kIOMapOverwrite) keeps the old memory held by the first, so nothing would be freed;
 *  - one page shared by every purged surface is a page every client can write and every other read, and made read-only
 *    it kills the first client that draws into a surface it was told was gone.
 * So the count, with sysctl kern.surfaces_volatile and a line in the memory report (loader_return.c), says first whether a
 * phone has enough of them for that work to be worth it.
 */
#define SURFACE_PURGEABLE_VOLATILE      1               /* kIOSurfacePurgeableVolatile */
#define SURFACE_IDLE_S                  3

extern "C" void
MiniIOSurfaceVolatile(uint32_t * idle, uint64_t * idleBytes, uint32_t * marked, uint64_t * markedBytes)
{
	uint64_t now = mach_absolute_time(), settle;

	*idle = *marked = 0;
	*idleBytes = *markedBytes = 0;
	if (gSurfaceLock == NULL) {
		return;
	}
	nanoseconds_to_absolutetime((uint64_t) SURFACE_IDLE_S * NSEC_PER_SEC, &settle);
	IOLockLock(gSurfaceLock);
	for (uint32_t sid = 1; sid < MINISURF_TABLE_SIZE; sid++) {
		MiniIOSurface * s = gSurfaceTable[sid];

		if (s == NULL || s->client != NULL || s->purgeable != SURFACE_PURGEABLE_VOLATILE) {
			continue;
		}
		(*marked)++;
		*markedBytes += s->info.allocSize;
		if (s->useCount[0] == 0 && s->useCount[1] == 0 && s->kernelHolds == 0 && s->shownReading == 0 &&
		    now - s->touched >= settle) {
			(*idle)++;
			*idleBytes += s->info.allocSize;
		}
	}
	IOLockUnlock(gSurfaceLock);
}

/* "IDLE IDLE_BYTES MARKED MARKED_BYTES": what MiniIOSurfaceVolatile() counts, for a script on the phone to read. */
static int
surfacesVolatileSysctl(struct sysctl_oid * oidp, void * arg1 __unused, int arg2 __unused, struct sysctl_req * req)
{
	char text[96];
	uint32_t idle, marked;
	uint64_t idleBytes, markedBytes;

	MiniIOSurfaceVolatile(&idle, &idleBytes, &marked, &markedBytes);
	snprintf(text, sizeof(text), "%u %llu %u %llu", idle, idleBytes, marked, markedBytes);
	return sysctl_handle_string(oidp, text, 0, req);
}

SYSCTL_PROC(_kern, OID_AUTO, surfaces_volatile, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, surfacesVolatileSysctl, "A",
    "IOSurfaces marked volatile: idle ones and their bytes, then all of them and their bytes (only counted, never purged)");

void
MiniIOSurfaceDrop(MiniIOSurface * s)
{
	if (s == NULL) {
		return;
	}
	IOLockLock(gSurfaceLock);
	if (s->kernelHolds != 0) {
		s->kernelHolds--;
	}
	s->touched = mach_absolute_time();
	surfaceUnhold(s);
	IOLockUnlock(gSurfaceLock);
}

MiniIOSurface *
MiniIOSurfaceCreateSimple(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
	OSDictionary * props = OSDictionary::withCapacity(4);
	MiniIOSurface * s = NULL;
	IOReturn error;

	if (props == NULL || gSurfaceLock == NULL) {
		OSSafeReleaseNULL(props);
		return NULL;
	}
	OSNumber * n;
	n = OSNumber::withNumber(width, 32); props->setObject("IOSurfaceWidth", n); n->release();
	n = OSNumber::withNumber(height, 32); props->setObject("IOSurfaceHeight", n); n->release();
	n = OSNumber::withNumber(pixelFormat, 32); props->setObject("IOSurfacePixelFormat", n); n->release();
	s = surfaceCreate(props, &error);
	props->release();
	if (s != NULL) {
		IOLockLock(gSurfaceLock);
		s->kernelHolds = 1;             /* (the maker's hold is the kernel's here, given back with MiniIOSurfaceDrop()) */
		IOLockUnlock(gSurfaceLock);
	}
	return s;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* What a Mach send right for a surface points at. Holds the surface while any right exists. */
/*
 * IOSurfaceSharedEvent, the events processes signal each other's frames with (CoreAnimation, PosterKit): a value of 64 bits in a
 * page every holder maps read-only, set through a trap, waited for through another, and listeners told by an asynchronous message
 * once it reaches theirs. The event's port is the event object's own.
 */
/* The event: kept by an IOSurfaceSendRight (whose port is the event's) in memory of no type -- a class of its own was one kalloc type
 * too many for its size (g2k17 panicked at boot: KT_ZONES_FOR_SIZE_SIZE). */
struct mini_event_data {
	IOBufferMemoryDescriptor *      page;
	volatile uint64_t *             value;
	uint64_t                        id;
};
class IOSurfaceSendRight;
typedef IOSurfaceSendRight MiniSharedEvent;

#define MINI_EVENT_WAITS        512
struct mini_event_wait {
	MiniSharedEvent *       event;          /* held while waiting */
	void *                  client;         /* the user client that asked (forgotten as it closes) */
	uint64_t                target;
	OSAsyncReference64      ref;
	/*
	 * A send right of its own to the port `ref` names (the listener's wake port), given back once the notification is
	 * sent or dropped: the listener's own right goes with the listener, which may be replaced or removed, or its client
	 * closed, while this still waits -- or is being sent, outside the lock (eventTell).
	 */
	mach_port_t             port;
	io_user_reference_t     args[3];        /* the block, the event object, the value */
};
static IOLock * gEventLock;
static struct mini_event_wait gEventWaits[MINI_EVENT_WAITS];
static uint64_t gEventIds;

class IOSurfaceSendRight : public OSObject
{
	OSDeclareDefaultStructors(IOSurfaceSendRight);

public:
	MiniIOSurface * surface;
	struct mini_event_data * ev;            /* an IOSurfaceSharedEvent's, not a surface's */

	virtual bool
	init(void) APPLE_KEXT_OVERRIDE
	{
		return OSObject::init();
	}
	virtual void free(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOSurfaceSendRight, OSObject);

void
IOSurfaceSendRight::free(void)
{
	if (ev != NULL) {
		OSSafeReleaseNULL(ev->page);
		IOFreeData(ev, sizeof(*ev));
		ev = NULL;
	}
	if (surface != NULL) {
		/* (the right's hold, taken by makeSendRight(): not one of the kernel's, which MiniIOSurfaceDrop() would count off) */
		IOLockLock(gSurfaceLock);
		surfaceUnhold(surface);
		IOLockUnlock(gSurfaceLock);
	}
	surface = NULL;
	OSObject::free();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOCoreSurfaceRoot : public IOService
{
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	setName("IOCoreSurfaceRoot");
	registerService();
	return true;
}

void
MiniIOSurfaceStart(IOService * provider)
{
	IOCoreSurfaceRoot * root;

	gSurfaceLock = IOLockAlloc();
	gSurfaceKeptLock = IOLockAlloc();
	gEventLock = IOLockAlloc();
	if (!PE_parse_boot_arg_str("surftrace_of", gSurfaceTraceOf, sizeof(gSurfaceTraceOf))) {
		gSurfaceTraceOf[0] = 0;
	}
	gSurfaceClearing = thread_call_allocate(surfaceMemoryClear, NULL);
	root = OSTypeAlloc(IOCoreSurfaceRoot);
	if (root == NULL || !root->init() || !root->attach(provider)) {
		OSSafeReleaseNULL(root);
		IOLog("MiniIOSurface: cannot create the root service\n");
		return;
	}
	if (!root->start(provider)) {
		root->detach(provider);
		IOLog("MiniIOSurface: the root service did not start\n");
	}
	root->release();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* One surface as one client has it. */
struct minisurf_client_ref {
	MiniIOSurface * surface;                /* held */
	IOMemoryMap *   map;                    /* the whole of it in the client's address space */
	IOMemoryMap *   pixelMap;               /* ... the pixels, where those are another client's memory */
	uint32_t        use[2];                 /* what this client added to the use counts */
	bool            detached;               /* the client let go of it and will release shortly */
};

class IOSurfaceRootUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t                          fTask;          /* held (initWithTask): a call may come after its program is gone */
	bool                            fClosed;        /* clientClose(): no call is served after (set under gSurfaceLock) */
	struct minisurf_client_ref *    fRefs;
	uint32_t                        fRefCount;
	uint32_t                        fRefCapacity;
	IOExternalTrap                  fTraps[kSurfaceTrapCount];
	/* Under gEventLock. */
	struct {
		uint64_t                key;            /* the listener object's address (its reference's refcon); 0: a free place */
		OSAsyncReference64      ref;
		/*
		 * The call that added the listener handed over a send right to its wake port (which `ref` names), the driver's
		 * once the call succeeded: kept here, and given back when the listener is replaced, removed or forgotten.
		 */
		mach_port_t             port;
	}                               fListeners[16];
	OSArray *                       fEventMaps;     /* the event pages mapped into the task */

	struct minisurf_client_ref * findRef(uint32_t sid, bool detached);
	struct minisurf_client_ref * addRef(MiniIOSurface * surface);
	void dropRef(struct minisurf_client_ref * ref);
	IOReturn returnSurface(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args);
	IOReturn returnSurface26(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args);

	IOReturn method(uint32_t selector, IOExternalMethodArguments * args);
	IOReturn methodInit(IOExternalMethodArguments * args);
	IOReturn methodCreate(IOExternalMethodArguments * args);
	IOReturn methodLookup(IOExternalMethodArguments * args);
	IOReturn methodLookupFromPort(IOExternalMethodArguments * args);
	IOReturn methodCreatePort(IOExternalMethodArguments * args);
	IOReturn methodSetValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodCopyValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodRemoveValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodSetBulkAttachments(const uint8_t * in, size_t inSize);

	IOReturn trapIncrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapDecrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapLock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapUnlock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapDetach(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapRelease(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapEventWait(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapEventSignal(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	MiniSharedEvent * copyEvent(mach_port_name_t name);
	IOReturn mapEvent(MiniSharedEvent * event, uint64_t * address);
	IOReturn methodEventCreate(IOExternalMethodArguments * args);
	IOReturn methodEventMap(IOExternalMethodArguments * args);
	IOReturn methodEventNotify(IOExternalMethodArguments * args);
	IOReturn methodEventAddListener(IOExternalMethodArguments * args);
	IOReturn methodEventRemoveListener(IOExternalMethodArguments * args);
	void forgetEventWaits(void);
	static void eventTell(MiniSharedEvent * event, uint64_t value);
	IOReturn lockCommon(uint32_t sid, uint32_t options, user_addr_t seedOut);
	IOReturn useCountCommon(uint32_t sid, uint32_t category, int delta);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
	virtual IOExternalTrap * getTargetAndTrapForIndex(IOService ** targetP, UInt32 index) APPLE_KEXT_OVERRIDE;
	static mach_port_name_t makeSendRight(task_t task, MiniIOSurface * surface);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	/*
	 * (held, not only named: surfaces are mapped and ports looked up and made in it. A program that hands its connection
	 * to another and exits is gone before the connection is, and the other's calls -- refused once it has closed, but one
	 * may be on its way -- must not find a task that has been freed)
	 */
	fTask = owningTask;
	task_reference(fTask);
	{
		char procname[32] = "?";
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOSurface: opened by %s[%d], type %u\n", procname, proc_selfpid(), (unsigned) type);
	}
	return true;
}

bool
IOSurfaceRootUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fTraps[kSurfaceTrapIncrementUseCount].func = (IOTrap) & IOSurfaceRootUserClient::trapIncrementUseCount;
	fTraps[kSurfaceTrapDecrementUseCount].func = (IOTrap) & IOSurfaceRootUserClient::trapDecrementUseCount;
	fTraps[kSurfaceTrapLock].func = (IOTrap) & IOSurfaceRootUserClient::trapLock;
	fTraps[kSurfaceTrapUnlock].func = (IOTrap) & IOSurfaceRootUserClient::trapUnlock;
	fTraps[kSurfaceTrapDetach].func = (IOTrap) & IOSurfaceRootUserClient::trapDetach;
	fTraps[kSurfaceTrapRelease].func = (IOTrap) & IOSurfaceRootUserClient::trapRelease;
	fTraps[kSurfaceTrapEventWait].func = (IOTrap) & IOSurfaceRootUserClient::trapEventWait;
	fTraps[kSurfaceTrapEventSignal].func = (IOTrap) & IOSurfaceRootUserClient::trapEventSignal;
	for (unsigned i = 0; i < kSurfaceTrapCount; i++) {
		fTraps[i].object = this;
	}
	return true;
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	/* (first: whoever else has the connection -- it was handed on -- is not served in the place of the program that is gone) */
	IOLockLock(gSurfaceLock);
	fClosed = true;
	IOLockUnlock(gSurfaceLock);
	forgetEventWaits();
	IOLockLock(gSurfaceLock);
	while (fRefCount != 0) {
		dropRef(&fRefs[fRefCount - 1]);
	}
	IOLockUnlock(gSurfaceLock);
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOSurfaceRootUserClient::free(void)
{
	if (fRefs != NULL) {
		if (gSurfaceLock != NULL) {
			IOLockLock(gSurfaceLock);
			while (fRefCount != 0) {
				dropRef(&fRefs[fRefCount - 1]);
			}
			IOLockUnlock(gSurfaceLock);
		}
		IODelete(fRefs, struct minisurf_client_ref, fRefCapacity);
		fRefs = NULL;
	}
	forgetEventWaits();
	OSSafeReleaseNULL(fEventMaps);
	if (fTask != NULL) {
		task_deallocate(fTask);
		fTask = NULL;
	}
	IOUserClient::free();
}

/* All three with the lock held. */
struct minisurf_client_ref *
IOSurfaceRootUserClient::findRef(uint32_t sid, bool detached)
{
	for (uint32_t i = 0; i < fRefCount; i++) {
		if (fRefs[i].surface->sid == sid && fRefs[i].detached == detached) {
			return &fRefs[i];
		}
	}
	return NULL;
}

struct minisurf_client_ref *
IOSurfaceRootUserClient::addRef(MiniIOSurface * surface)
{
	struct minisurf_client_ref * ref;
	IOMemoryMap * map;

	if (fRefCount == fRefCapacity) {
		uint32_t capacity = fRefCapacity ? fRefCapacity * 2 : 16;
		struct minisurf_client_ref * refs = IONew(struct minisurf_client_ref, capacity);

		if (refs == NULL) {
			return NULL;
		}
		if (fRefs != NULL) {
			memcpy(refs, fRefs, fRefCount * sizeof(*refs));
			IODelete(fRefs, struct minisurf_client_ref, fRefCapacity);
		}
		fRefs = refs;
		fRefCapacity = capacity;
	}
	map = surface->memory->createMappingInTask(fTask, 0, kIOMapAnywhere);
	if (map == NULL) {
		IOLog("MiniIOSurface: cannot map surface %u (%u bytes) into the client\n", surface->sid, surface->info.allocSize);
		return NULL;
	}
	ref = &fRefs[fRefCount++];
	bzero(ref, sizeof(*ref));
	ref->surface = surface;
	ref->map = map;
	if (surface->client != NULL && surface->clientTask != fTask) {
		ref->pixelMap = surface->client->createMappingInTask(fTask, 0, kIOMapAnywhere);
	}
	surfaceHold(surface);
	return ref;
}

void
IOSurfaceRootUserClient::dropRef(struct minisurf_client_ref * ref)
{
	MiniIOSurface * s = ref->surface;

	for (unsigned c = 0; c < 2; c++) {
		s->useCount[c] -= ref->use[c];
	}
	s->shared()->useCount = (uint64_t) s->useCount[1] << 32 | s->useCount[0];
	OSSafeReleaseNULL(ref->map);
	OSSafeReleaseNULL(ref->pixelMap);
	*ref = fRefs[--fRefCount];
	surfaceUnhold(s);
}

static uint32_t
resultSize(const MiniIOSurface * s)
{
	uint32_t planes = s->info.planeCount ? s->info.planeCount : 1;

	return (uint32_t) (sizeof(struct minisurf_result) + planes * sizeof(struct minisurf_plane));
}

/*
 * The description Darwin 26's IOSurface.framework takes (it reserves 0xc68 bytes for it, and copies it to
 * offset 0x70 of its client object, where its getters read it): the Darwin 16.5 one with the sizes widened
 * to 64 bits. In the header, allocSize, width, height, bytesPerRow and offset become 64-bit and what
 * follows them moves down; each plane's first six words (width, height, -, offset, bytesPerRow, size)
 * become 64-bit and the rest of the plane moves down by 0x18, so a plane is 0x98 bytes.
 */
#define MINISURF_RESULT26_HEADER 0x88
#define MINISURF_PLANE26_SIZE    0x98
#define MINISURF_RESULT26_MAX    (MINISURF_RESULT26_HEADER + 20 * MINISURF_PLANE26_SIZE)
static_assert(MINISURF_RESULT26_MAX == 0xc68, "Darwin 26 result size");

static void
put64(uint8_t * p, size_t at, uint64_t v)
{
	memcpy(p + at, &v, sizeof(v));
}

static void
put32(uint8_t * p, size_t at, uint32_t v)
{
	memcpy(p + at, &v, sizeof(v));
}

IOReturn
IOSurfaceRootUserClient::returnSurface26(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args)
{
	MiniIOSurface * s = ref->surface;
	const struct minisurf_result * i = &s->info;
	uint32_t planes = i->planeCount ? i->planeCount : 1;
	uint32_t size = MINISURF_RESULT26_HEADER + planes * MINISURF_PLANE26_SIZE;
	uint8_t * r = (uint8_t *) args->structureOutput;
	mach_vm_address_t address = ref->map->getAddress();

	if (r == NULL || args->structureOutputSize < size) {
		return kIOReturnBadArgument;
	}
	bzero(r, size);
	put64(r, 0x00, known ? ~0ULL : s->client == NULL ? address + PAGE_SIZE :
	    ref->pixelMap != NULL ? ref->pixelMap->getAddress() : s->clientAddress);
	put64(r, 0x08, address);
	put64(r, 0x10, address + MINISURF_AUX_OFFSET);
	put32(r, 0x18, i->sid);
	put64(r, 0x20, i->allocSize);
	put64(r, 0x28, i->width);
	put64(r, 0x30, i->height);
	put64(r, 0x38, i->bytesPerRow);
	put64(r, 0x40, i->offset);
	put32(r, 0x48, i->pixelFormat);
	put32(r, 0x58, i->planeCount);
	/* bytesPerElement .. lockSeed, then subsampling .. protectionOptions: as in Darwin 16.5, 0x20 further on */
	memcpy(r + 0x60, (const uint8_t *) i + 0x40, 0x88 - 0x60);
	for (uint32_t k = 0; k < planes; k++) {
		const struct minisurf_plane * p = &s->planes[k];
		uint8_t * o = r + MINISURF_RESULT26_HEADER + k * MINISURF_PLANE26_SIZE;

		put64(o, 0x00, p->width);
		put64(o, 0x08, p->height);
		put64(o, 0x18, p->offset);
		put64(o, 0x20, p->bytesPerRow);
		put64(o, 0x28, p->size);
		memcpy(o + 0x30, (const uint8_t *) p + 0x18, sizeof(*p) - 0x18);
	}
	args->structureOutputSize = size;
	return kIOReturnSuccess;
}

/* Describe the surface to the client. known: it has the surface already, do not build a second view. */
IOReturn
IOSurfaceRootUserClient::returnSurface(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args)
{
	MiniIOSurface * s = ref->surface;
	uint32_t size = resultSize(s);
	struct minisurf_result * r = (struct minisurf_result *) args->structureOutput;
	mach_vm_address_t address = ref->map->getAddress();

	if (r == NULL || args->structureOutputSize < size) {
		return kIOReturnBadArgument;
	}
	if (args->structureOutputSize >= MINISURF_RESULT26_MAX) {
		return returnSurface26(ref, known, args);
	}
	*r = s->info;
	memcpy(r + 1, s->planes, size - sizeof(*r));
	r->shared = address;
	r->aux = address + MINISURF_AUX_OFFSET;
	r->base = known ? ~0ULL : s->client == NULL ? address + PAGE_SIZE :
	    ref->pixelMap != NULL ? ref->pixelMap->getAddress() : s->clientAddress;
	args->structureOutputSize = size;
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodInit(IOExternalMethodArguments * args)
{
	struct {
		uint32_t allocAlignMask;
		uint32_t rowAlignMask;
		uint32_t maxBytesPerRow;
		uint32_t maxWidth;
		uint32_t maxHeight;
		uint16_t reserved;
		uint16_t version;
	} reply;

	/*
	 * Darwin 26 (IOSurface.framework, _iosConnectInitalize) asks for 0x28 bytes: the same limits with the
	 * maxima widened to 64 bits, a byte at 0x20 it reads in IOSurfaceClientInitDetachModeCode and
	 * IOSurfaceClientSetIndexedTimestamp (left 0), and the version, which must be 1, at 0x22.
	 */
	struct {
		uint32_t allocAlignMask;
		uint32_t rowAlignMask;
		uint64_t maxBytesPerRow;
		uint64_t maxWidth;
		uint64_t maxHeight;
		uint8_t  features;
		uint8_t  reserved;
		uint16_t version;
		uint32_t reserved2;
	} reply26;

	static_assert(sizeof(reply) == 0x18, "init reply");
	static_assert(sizeof(reply26) == 0x28, "init reply (Darwin 26)");
	if (args->structureOutput != NULL && args->structureOutputSize >= sizeof(reply26)) {
		bzero(&reply26, sizeof(reply26));
		reply26.allocAlignMask = (uint32_t) PAGE_MASK;
		reply26.rowAlignMask = MINISURF_ROW_ALIGN - 1;
		reply26.maxBytesPerRow = MINISURF_MAX_BYTES_PER_ROW;
		reply26.maxWidth = MINISURF_MAX_DIMENSION;
		reply26.maxHeight = MINISURF_MAX_DIMENSION;
		reply26.version = 1;
		memcpy(args->structureOutput, &reply26, sizeof(reply26));
		args->structureOutputSize = sizeof(reply26);
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || args->structureOutputSize < sizeof(reply)) {
		return kIOReturnBadArgument;
	}
	reply.allocAlignMask = (uint32_t) PAGE_MASK;
	reply.rowAlignMask = MINISURF_ROW_ALIGN - 1;
	reply.maxBytesPerRow = MINISURF_MAX_BYTES_PER_ROW;
	reply.maxWidth = MINISURF_MAX_DIMENSION;
	reply.maxHeight = MINISURF_MAX_DIMENSION;
	reply.reserved = 0;
	reply.version = 1;
	memcpy(args->structureOutput, &reply, sizeof(reply));
	args->structureOutputSize = sizeof(reply);
	return kIOReturnSuccess;
}

/* The serialized object in a request, or NULL. */
static OSObject *
unserialize(const uint8_t * data, size_t size)
{
	if (data == NULL || size < 4) {
		return NULL;
	}
	return OSUnserializeXML((const char *) data, size, NULL);
}

IOReturn
IOSurfaceRootUserClient::methodCreate(IOExternalMethodArguments * args)
{
	IOReturn ret;
	IOMemoryMap * inMap = NULL;
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	OSObject * obj;
	OSDictionary * props;
	MiniIOSurface * s;
	struct minisurf_client_ref * ref;
	uint64_t began = mach_absolute_time();

	if (args->scalarInputCount >= 1 && args->scalarInput[0] != 0) {
		IOLog("MiniIOSurface: child surfaces are not supported (parent %llu)\n", args->scalarInput[0]);
		return kIOReturnUnsupported;
	}
	if (args->structureInputDescriptor != NULL) {
		inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
		if (inMap == NULL) {
			return kIOReturnNoMemory;
		}
		in = (const uint8_t *) inMap->getVirtualAddress();
		inSize = inMap->getLength();
	}
	obj = unserialize(in, inSize);
	OSSafeReleaseNULL(inMap);
	props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	uint64_t made_at = mach_absolute_time();
	s = surfaceCreate(props, &ret);
	made_at = mach_absolute_time() - made_at;
	if (s == NULL) {
		IOLog("MiniIOSurface: create failed 0x%x: %ux%u format 0x%x alloc %u\n", ret,
		    dictNumber(props, "IOSurfaceWidth", 0), dictNumber(props, "IOSurfaceHeight", 0),
		    dictNumber(props, "IOSurfacePixelFormat", 0), dictNumber(props, "IOSurfaceAllocSize", 0));
		props->release();
		return ret;
	}
	{
		char procname[32] = "?";
		static unsigned keysSaid;

		proc_selfname(procname, sizeof(procname));
		/* what a traced program asks for, key by key (the first few surfaces) */
		if (gSurfaceTraceOf[0] != 0 && strncmp(procname, gSurfaceTraceOf, strlen(gSurfaceTraceOf)) == 0 && keysSaid++ < 6) {
			OSCollectionIterator * keys = OSCollectionIterator::withCollection(props);
			char line[256];
			size_t used = 0;
			OSSymbol * key;

			line[0] = 0;
			while (keys != NULL && (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL && used < sizeof(line)) {
				OSNumber * n = OSDynamicCast(OSNumber, props->getObject(key));

				used += (size_t) snprintf(line + used, sizeof(line) - used, " %s=%lld", key->getCStringNoCopy(),
				    n ? (long long) n->unsigned64BitValue() : -1LL);
			}
			OSSafeReleaseNULL(keys);
			traceLine("MiniIOSurface: %s asks for a surface with%s\n", procname, line);
		}
	}
	props->release();
	{
		char procname[32] = "?";

		proc_selfname(procname, sizeof(procname));
		if (gSurfaceTraceOf[0] != 0 && strncmp(procname, gSurfaceTraceOf, strlen(gSurfaceTraceOf)) != 0) {
			procname[0] = 0;        /* (not one of those asked about: not counted) */
		}
		if (procname[0] != 0 && gSurfaceTraced < gSurfaceTrace) {
			s->traced = gSurfaceTraceOf[0] != 0;
		}
	}
	if (s->traced || (gSurfaceTraceOf[0] == 0 && gSurfaceTraced < gSurfaceTrace)) {
		char procname[32] = "?";

		gSurfaceTraced++;
		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] created surface %u: %ux%u '%c%c%c%c', %u bytes per row, %u planes, %u bytes\n",
		    procname, proc_selfpid(), s->sid, s->info.width, s->info.height, (int) (s->info.pixelFormat >> 24 & 0xff),
		    (int) (s->info.pixelFormat >> 16 & 0xff), (int) (s->info.pixelFormat >> 8 & 0xff),
		    (int) (s->info.pixelFormat & 0xff), s->info.bytesPerRow, s->info.planeCount, s->info.allocSize);
	}

	uint64_t mapped_at = mach_absolute_time();
	/* (read now: where the client got no reference, the hold given up below was the last and s is gone) */
	bool big = s->info.allocSize >= SURFACE_KEEP_FROM;
	IOLockLock(gSurfaceLock);
	ref = addRef(s);
	ret = ref ? returnSurface(ref, false, args) : kIOReturnNoMemory;
	if (ref != NULL && ret != kIOReturnSuccess) {
		dropRef(ref);
	}
	surfaceUnhold(s);                       /* the creator's hold; the client's own is in the ref */
	IOLockUnlock(gSurfaceLock);
	mapped_at = mach_absolute_time() - mapped_at;
	OSAddAtomic64((SInt64) (mach_absolute_time() - began), (SInt64 *) &gSurfaceMaking);
	OSIncrementAtomic(&gSurfacesMade);
	if (big) {
		/* the big ones (the window server's of the whole display): where the time to make one goes, every 5 s */
		static uint64_t since, all, making, mapping;
		static uint32_t count;
		uint64_t now = mach_absolute_time(), ns;

		count++;
		all += now - began;
		making += made_at;
		mapping += mapped_at;
		absolutetime_to_nanoseconds(now - since, &ns);
		if (since == 0) {
			since = now;
		} else if (ns >= 5 * NSEC_PER_SEC) {
			uint64_t a, m, p;

			absolutetime_to_nanoseconds(all, &a);
			absolutetime_to_nanoseconds(making, &m);
			absolutetime_to_nanoseconds(mapping, &p);
			IOLog("MiniIOSurface: %u big surfaces made, %llu us each: %llu getting the memory and the surface, %llu mapping it "
			    "for its maker; memory not kept as somebody else had it %u times, a piece kept longer put out for it %u times\n",
			    count, a / 1000 / count, m / 1000 / count, p / 1000 / count, OSBitAndAtomic(0, &gSurfacesNotKept[0]),
			    OSBitAndAtomic(0, &gSurfacesNotKept[1]));
			since = now;
			count = 0;
			all = making = mapping = 0;
		}
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookup(IOExternalMethodArguments * args)
{
	IOReturn ret = kIOReturnNotFound;
	uint32_t sid;
	struct minisurf_client_ref * ref;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	sid = (uint32_t) args->scalarInput[0];
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL && sid != 0 && sid < MINISURF_TABLE_SIZE && gSurfaceTable[sid] != NULL) {
		ref = addRef(gSurfaceTable[sid]);
		if (ref == NULL) {
			ret = kIOReturnNoMemory;
		}
	}
	if (ref != NULL) {
		ret = returnSurface(ref, false, args);
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookupFromPort(IOExternalMethodArguments * args)
{
	IOReturn ret;
	OSObject * obj = NULL;
	IOSurfaceSendRight * right;
	struct minisurf_client_ref * ref;
	bool known;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	ret = copyObjectForPortNameInTask(fTask, (mach_port_name_t) args->scalarInput[0], &obj);
	right = OSDynamicCast(IOSurfaceSendRight, obj);
	if (ret != kIOReturnSuccess || right == NULL || right->surface == NULL) {
		static unsigned said;

		if (said++ < 48) {
			char procname[32] = "?";

			proc_selfname(procname, sizeof(procname));
			traceLine("MiniIOSurface: %s[%d] looked up a surface from port %#llx: %s\n", procname, proc_selfpid(),
			    args->scalarInput[0], ret != kIOReturnSuccess ? "no object behind the port" : obj ? "not a surface's port" :
			    "no surface");
		}
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	{
		static unsigned found;

		if (found++ < 48) {
			char procname[32] = "?";

			proc_selfname(procname, sizeof(procname));
			traceLine("MiniIOSurface: %s[%d] found surface %u (%ux%u) from port %#llx (%u found)\n", procname, proc_selfpid(),
			    right->surface->sid, right->surface->info.width, right->surface->info.height, args->scalarInput[0], found);
		}
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(right->surface->sid, false);
	known = ref != NULL;
	if (ref == NULL) {
		ref = addRef(right->surface);
	}
	ret = ref ? returnSurface(ref, known, args) : kIOReturnNoMemory;
	IOLockUnlock(gSurfaceLock);
	if (right->surface->traced) {
		char procname[32] = "?";
		uint32_t format = right->surface->info.pixelFormat, nonzero = 0, first = 0, middle = 0;
		const uint8_t * pixels = right->surface->kpixels;
		uint32_t size = right->surface->info.allocSize;

		/* what is in it by now: how many of its bytes are not zero, its first pixel and one in the middle */
		for (uint32_t at = 0; pixels != NULL && at < size; at += 64) {
			nonzero += pixels[at] != 0 || pixels[at + 1] != 0 || pixels[at + 2] != 0 || pixels[at + 3] != 0;
		}
		if (pixels != NULL && size >= 8) {
			memcpy(&first, pixels, 4);
			memcpy(&middle, pixels + (size / 2 & ~3u), 4);
		}
		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] looked up surface %u (%ux%u '%c%c%c%c') from a port: %#x; of every 64 bytes %u of %u "
		    "begin with a pixel that is not 0, the first 0x%08x, one in the middle 0x%08x\n", procname, proc_selfpid(),
		    right->surface->sid, right->surface->info.width, right->surface->info.height, (int) (format >> 24 & 0xff),
		    (int) (format >> 16 & 0xff), (int) (format >> 8 & 0xff), (int) (format & 0xff), ret, nonzero, size / 64, first, middle);
	}
	obj->release();
	return ret;
}

mach_port_name_t
IOSurfaceRootUserClient::makeSendRight(task_t task, MiniIOSurface * s)
{
	IOSurfaceSendRight * right;
	mach_port_name_t name = 0;

	right = OSTypeAlloc(IOSurfaceSendRight);
	if (right == NULL || !right->init()) {
		OSSafeReleaseNULL(right);
		return 0;
	}
	IOLockLock(gSurfaceLock);
	surfaceHold(s);                         /* for as long as the send right exists */
	IOLockUnlock(gSurfaceLock);
	right->surface = s;
	copyPortNameForObjectInTask(task, right, &name);
	right->release();
	return name;
}

mach_port_name_t
MiniIOSurfaceMakeSendRight(task_t task, MiniIOSurface * surface)
{
	return IOSurfaceRootUserClient::makeSendRight(task, surface);
}

IOReturn
IOSurfaceRootUserClient::methodCreatePort(IOExternalMethodArguments * args)
{
	MiniIOSurface * s;
	mach_port_name_t name;

	if (args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
		return kIOReturnBadArgument;
	}
	s = MiniIOSurfaceCopy((uint32_t) args->scalarInput[0]);
	if (s == NULL) {
		static unsigned missed;

		/* backboardd looked up port 0 for the wallpaper's surfaces: a maker's port that was never made */
		if (missed++ < 32) {
			char procname[32] = "?";

			proc_selfname(procname, sizeof(procname));
			traceLine("MiniIOSurface: %s[%d] asked a port for surface %llu, which is not there\n", procname, proc_selfpid(),
			    args->scalarInput[0]);
		}
		return kIOReturnNotFound;
	}
	name = makeSendRight(fTask, s);
	{
		static unsigned made;

		if (made++ < 48 || name == 0) {
			char procname[32] = "?";

			proc_selfname(procname, sizeof(procname));
			traceLine("MiniIOSurface: %s[%d] port for surface %u (%ux%u): %#x (%u ports made)\n", procname, proc_selfpid(), s->sid,
			    s->info.width, s->info.height, name, made);
		}
	}
	if (s->traced) {
		char procname[32] = "?";

		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] made a port for surface %u: %#x\n", procname, proc_selfpid(), s->sid, name);
	}
	MiniIOSurfaceDrop(s);
	args->scalarOutput[0] = name;
	args->scalarOutputCount = 1;
	return name != 0 ? kIOReturnSuccess : kIOReturnNoMemory;
}

/* Requests about values start with the surface number in 8 bytes; the reply to a change is the new seed. */
static IOReturn
returnSeed(IOExternalMethodArguments * args, uint32_t seed)
{
	if (args->structureOutput != NULL && args->structureOutputSize >= sizeof(seed)) {
		memcpy(args->structureOutput, &seed, sizeof(seed));
		args->structureOutputSize = sizeof(seed);
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodSetValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	OSObject * obj;
	OSArray * array;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnBadArgument;

	if (inSize < 12) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	obj = unserialize(in + 8, inSize - 8);
	array = OSDynamicCast(OSArray, obj);
	if (array == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL) {
		ret = kIOReturnNotFound;
	} else if (ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		OSString * key = OSDynamicCast(OSString, array->getObject(1));
		OSDictionary * all = OSDynamicCast(OSDictionary, array->getObject(0));

		if (array->getCount() == 2 && key != NULL) {
			s->values->setObject(key, array->getObject(0));         /* [value, key] */
			ret = kIOReturnSuccess;
		} else if (array->getCount() == 1 && all != NULL) {
			s->values->merge(all);                                   /* [all the values] */
			ret = kIOReturnSuccess;
		}
		if (ret == kIOReturnSuccess) {
			s->updateValuesState();
			seed = s->shared()->valuesSeed;
		}
	}
	IOLockUnlock(gSurfaceLock);
	obj->release();
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

IOReturn
IOSurfaceRootUserClient::methodRemoveValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		const char * key = (const char *) in + 8;

		if (key[0] == 0) {
			s->values->flushCollection();
		} else {
			s->values->removeObject(key);
		}
		s->updateValuesState();
		seed = s->shared()->valuesSeed;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

/* The reply: 4 bytes unused, the seed, then the serialized value (a dictionary when all were asked for). */
IOReturn
IOSurfaceRootUserClient::methodCopyValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, header[2] = { 0, 0 };
	struct minisurf_client_ref * ref;
	OSSerialize * s = NULL;
	IOReturn ret = kIOReturnNotFound;
	uint32_t length = 0, total;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		const char * key = (const char *) in + 8;
		OSObject * value = key[0] ? ref->surface->values->getObject(key) : ref->surface->values;

		header[1] = ref->surface->shared()->valuesSeed;
		ret = kIOReturnSuccess;
		if (value != NULL && (key[0] != 0 || ref->surface->values->getCount() != 0)) {
			s = OSSerialize::binaryWithCapacity(4096);
			if (s == NULL || !value->serialize(s)) {
				ret = kIOReturnNoMemory;
			} else {
				length = s->getLength();
			}
		}
	}
	IOLockUnlock(gSurfaceLock);

	total = (uint32_t) sizeof(header) + length;
	if (ret == kIOReturnSuccess) {
		if (args->structureOutputDescriptor != NULL) {
			IOMemoryDescriptor * out = args->structureOutputDescriptor;

			if (out->getLength() < total) {
				ret = kIOReturnNoSpace;
			} else if ((ret = out->prepare()) == kIOReturnSuccess) {
				out->writeBytes(0, header, sizeof(header));
				if (length != 0) {
					out->writeBytes(sizeof(header), s->text(), length);
				}
				out->complete();
				args->structureOutputDescriptorSize = total;
			}
		} else if (args->structureOutput == NULL || args->structureOutputSize < total) {
			ret = kIOReturnNoSpace;
		} else {
			memcpy(args->structureOutput, header, sizeof(header));
			if (length != 0) {
				memcpy((uint8_t *) args->structureOutput + sizeof(header), s->text(), length);
			}
			args->structureOutputSize = total;
		}
	}
	OSSafeReleaseNULL(s);
	return ret;
}

/*
 * Colour space, transfer function and the like: a fixed structure kept in the shared page.
 * The request: the structure, its size, a mask of the fields that changed, the surface number.
 */
IOReturn
IOSurfaceRootUserClient::methodSetBulkAttachments(const uint8_t * in, size_t inSize)
{
	static const struct {
		uint8_t from, to;
	} fields[15] = {
		{ 0x00, 0x20 }, { 0x20, 0x30 }, { 0x30, 0x38 }, { 0x38, 0x39 }, { 0x39, 0x3a }, { 0x3a, 0x3b }, { 0x3b, 0x3c },
		{ 0x3c, 0x3d }, { 0x3d, 0x3e }, { 0x3e, 0x3f }, { 0x3f, 0x40 }, { 0x40, 0x58 }, { 0x58, 0x5c }, { 0x5c, 0x64 },
		{ 0x64, 0x65 },
	};
	uint64_t size, mask;
	uint32_t sid;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (inSize < 0x88) {
		return kIOReturnBadArgument;
	}
	memcpy(&size, in + 0x70, sizeof(size));
	memcpy(&mask, in + 0x78, sizeof(mask));
	memcpy(&sid, in + 0x80, sizeof(sid));
	if (size > 0x70) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		uint8_t * attachments = ref->surface->shared()->attachments;

		for (unsigned i = 0; i < 15; i++) {
			if ((mask & (1ULL << i)) && fields[i].to <= size) {
				memcpy(attachments + fields[i].from, in + fields[i].from, fields[i].to - fields[i].from);
			}
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

/*
 * A call made asynchronously hands over a send right to its wake port, which is the driver's once the call succeeds (MIG
 * takes it back only on an error). Adding a listener keeps it (methodEventAddListener); no other call does, so it is
 * given back here, or every such call would leave a port behind for good (as MiniIOMobileFramebuffer.cpp).
 */
IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	IOReturn ret;

	(void) dispatch;
	(void) target;
	(void) reference;
	/*
	 * (closed: its program is gone, or let go of it, and whoever else has the connection -- it was handed on -- is not
	 * served in its place. Read without the lock, which the task being held makes enough: a call that passed this as the
	 * client closed finds the task still there, and what it leaves behind goes with free())
	 */
	if (fClosed) {
		return kIOReturnNotOpen;
	}
	ret = method(selector, args);
	if (ret == kIOReturnSuccess && args->asyncWakePort != MACH_PORT_NULL && selector != kSurfaceMethodEventAddListener) {
		releaseAsyncReference64(args->asyncReference);
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::method(uint32_t selector, IOExternalMethodArguments * args)
{
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	IOMemoryMap * inMap = NULL;
	struct minisurf_client_ref * ref;
	IOReturn ret;
	static uint32_t hk_calls;

	if (hk_calls < 40) {
		char procname[32] = "?";
		hk_calls++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOSurface: %s[%d] selector %u (scalars %u in, %u out; structure %u in, %u out)\n", procname,
		    proc_selfpid(), selector, args->scalarInputCount, args->scalarOutputCount, args->structureInputSize,
		    args->structureOutputSize);
	}

	switch (selector) {
	case kSurfaceMethodInit:
		return methodInit(args);
	case kSurfaceMethodCreate:
		return methodCreate(args);
	case kSurfaceMethodLookup:
		return methodLookup(args);
	case kSurfaceMethodLookupFromPort:
		return methodLookupFromPort(args);
	case kSurfaceMethodCreatePort:
		return methodCreatePort(args);
	case kSurfaceMethodEventCreate:
		return methodEventCreate(args);
	case kSurfaceMethodEventMap:
		return methodEventMap(args);
	case kSurfaceMethodEventNotify:
		return methodEventNotify(args);
	case kSurfaceMethodEventAddListener:
		return methodEventAddListener(args);
	case kSurfaceMethodEventRemoveListener:
		return methodEventRemoveListener(args);

	case kSurfaceMethodSetValue:
	case kSurfaceMethodCopyValue:
	case kSurfaceMethodRemoveValue:
	case kSurfaceMethodSetBulkAttachments:
		if (args->structureInputDescriptor != NULL) {
			inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
			if (inMap == NULL) {
				return kIOReturnNoMemory;
			}
			in = (const uint8_t *) inMap->getVirtualAddress();
			inSize = inMap->getLength();
		}
		if (in == NULL) {
			ret = kIOReturnBadArgument;
		} else if (selector == kSurfaceMethodSetValue) {
			ret = methodSetValue(args, in, inSize);
		} else if (selector == kSurfaceMethodCopyValue) {
			ret = methodCopyValue(args, in, inSize);
		} else if (selector == kSurfaceMethodRemoveValue) {
			ret = methodRemoveValue(args, in, inSize);
		} else {
			ret = methodSetBulkAttachments(in, inSize);
		}
		OSSafeReleaseNULL(inMap);
		return ret;

	case kSurfaceMethodSetPurgeable:                /* in: surface, new state; out: old state */
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		ret = kIOReturnNotFound;
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			if (args->scalarOutputCount >= 1) {
				args->scalarOutput[0] = ref->surface->purgeable;
				args->scalarOutputCount = 1;
			}
			if (args->scalarInput[1] != 3) {        /* 3 = keep current */
				ref->surface->purgeable = (uint32_t) args->scalarInput[1];
			}
			ret = kIOReturnSuccess;
		}
		IOLockUnlock(gSurfaceLock);
		return ret;

	case kSurfaceMethodBindAccel:                   /* in: surface, accelerator, sub-id */
		if (args->scalarInputCount < 3) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			ref->surface->shared()->accelID = (uint16_t) args->scalarInput[1];
			ref->surface->shared()->accelSubID = (uint16_t) args->scalarInput[2];
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetYCbCrMatrix:              /* in: surface, matrix */
		if (args->scalarInputCount < 2) {
			break;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			ref->surface->shared()->ycbcrMatrix = (uint32_t) args->scalarInput[1];
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetOwnership:
	case kSurfaceMethodSetOwnershipIdentity:
	case kSurfaceMethodSetTimestamp:
	case kSurfaceMethodSetIndexedTimestamp:
	case kSurfaceMethodDetachModeCode:
	case kSurfaceMethodLog:
		/* accounting and diagnostics: nothing depends on them here */
		if (args->scalarOutputCount != 0) {
			bzero(args->scalarOutput, args->scalarOutputCount * sizeof(args->scalarOutput[0]));
		}
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gSurfaceLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gSurfaceLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOSurface: selector %u not implemented (scalars %u in, %u out; structure %u in, %u out)\n", selector,
		    args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* IOSurfaceSharedEvent (MiniSharedEvent above) */

MiniSharedEvent *
IOSurfaceRootUserClient::copyEvent(mach_port_name_t name)
{
	OSObject * obj = NULL;
	MiniSharedEvent * event;

	if (copyObjectForPortNameInTask(fTask, name, &obj) != kIOReturnSuccess) {
		return NULL;
	}
	event = OSDynamicCast(IOSurfaceSendRight, obj);
	if (event == NULL || event->ev == NULL) {
		OSSafeReleaseNULL(obj);
		return NULL;
	}
	return event;
}

/* The event's page, read-only, in this client's task (kept until the client goes): its value's address there. */
IOReturn
IOSurfaceRootUserClient::mapEvent(MiniSharedEvent * event, uint64_t * address)
{
	IOMemoryMap * map;

	map = event->ev->page->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (map == NULL) {
		return kIOReturnNoMemory;
	}
	IOLockLock(gEventLock);
	if (fEventMaps == NULL) {
		fEventMaps = OSArray::withCapacity(4);
	}
	if (fEventMaps != NULL) {
		fEventMaps->setObject(map);
	}
	IOLockUnlock(gEventLock);
	*address = map->getAddress();
	map->release();
	return fEventMaps != NULL ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn
IOSurfaceRootUserClient::methodEventCreate(IOExternalMethodArguments * args)
{
	MiniSharedEvent * event;
	mach_port_name_t name = 0;
	uint64_t address = 0;
	IOReturn ret;

	if (args->scalarOutputCount < 2) {
		return kIOReturnBadArgument;
	}
	event = OSTypeAlloc(IOSurfaceSendRight);
	if (event == NULL || !event->init()) {
		OSSafeReleaseNULL(event);
		return kIOReturnNoMemory;
	}
	event->ev = (struct mini_event_data *) IOMallocZeroData(sizeof(struct mini_event_data));
	if (event->ev != NULL) {
		event->ev->page = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, PAGE_SIZE, PAGE_SIZE);
	}
	if (event->ev == NULL || event->ev->page == NULL) {
		event->release();
		return kIOReturnNoMemory;
	}
	event->ev->value = (volatile uint64_t *) event->ev->page->getBytesNoCopy();
	bzero((void *) event->ev->value, PAGE_SIZE);
	event->ev->id = OSIncrementAtomic64((SInt64 *) &gEventIds) + 1;
	ret = mapEvent(event, &address);
	if (ret == kIOReturnSuccess) {
		copyPortNameForObjectInTask(fTask, event, &name);
	}
	event->release();
	if (ret != kIOReturnSuccess || name == 0) {
		return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
	}
	args->scalarOutput[0] = name;
	args->scalarOutput[1] = address;
	args->scalarOutputCount = 2;
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodEventMap(IOExternalMethodArguments * args)
{
	MiniSharedEvent * event;
	uint64_t address = 0;
	IOReturn ret;

	if (args->scalarInputCount < 1 || args->scalarOutputCount < 4) {
		return kIOReturnBadArgument;
	}
	event = copyEvent((mach_port_name_t) args->scalarInput[0]);
	if (event == NULL) {
		return kIOReturnBadArgument;
	}
	ret = mapEvent(event, &address);
	if (ret == kIOReturnSuccess) {
		args->scalarOutput[0] = 0;
		args->scalarOutput[1] = address;
		args->scalarOutput[2] = event->ev->id;
		args->scalarOutput[3] = 0;
		args->scalarOutputCount = 4;
	}
	event->release();
	return ret;
}

/* Tell, outside the lock, the listeners whose values `event` has reached (every event's where `event` is NULL and `client` given: none) */
void
IOSurfaceRootUserClient::eventTell(MiniSharedEvent * event, uint64_t value)
{
	struct {
		OSAsyncReference64 ref;
		io_user_reference_t args[3];
		MiniSharedEvent * event;
		mach_port_t port;
	} told[16];
	unsigned count;

	do {
		count = 0;
		IOLockLock(gEventLock);
		for (unsigned i = 0; i < MINI_EVENT_WAITS && count < 16; i++) {
			struct mini_event_wait * w = &gEventWaits[i];

			if (w->event == event && w->target <= value) {
				memcpy(told[count].ref, w->ref, sizeof(w->ref));
				memcpy(told[count].args, w->args, sizeof(w->args));
				told[count].event = w->event;
				told[count].port = w->port;
				count++;
				bzero(w, sizeof(*w));
			}
		}
		IOLockUnlock(gEventLock);
		for (unsigned i = 0; i < count; i++) {
			IOUserClient::sendAsyncResult64(told[i].ref, kIOReturnSuccess, told[i].args, 3);
			IOUserClient::releaseNotificationPort(told[i].port);
			told[i].event->release();
		}
	} while (count == 16);
}

IOReturn
IOSurfaceRootUserClient::methodEventNotify(IOExternalMethodArguments * args)
{
	MiniSharedEvent * event;
	uint64_t listener, target;
	IOReturn ret = kIOReturnNoResources;
	mach_port_t port = MACH_PORT_NULL;
	unsigned l;

	if (args->scalarInputCount < 5) {
		return kIOReturnBadArgument;
	}
	event = copyEvent((mach_port_name_t) args->scalarInput[0]);
	if (event == NULL) {
		return kIOReturnBadArgument;
	}
	listener = args->scalarInput[1];
	target = args->scalarInput[4];
	IOLockLock(gEventLock);
	for (l = 0; l < 16 && fListeners[l].key != listener; l++) {
	}
	if (l == 16 || listener == 0) {
		ret = kIOReturnNotFound;
	} else {
		/* (a right of the wait's own: see mini_event_wait. NULL for a port of the kernel's, the dead name for one that is
		 * gone: nobody to tell either way) */
		port = ipc_port_copy_send_mqueue(fListeners[l].port);
		if (!IPC_PORT_VALID(port)) {
			port = MACH_PORT_NULL;
			ret = kIOReturnBadArgument;
		}
	}
	if (port != MACH_PORT_NULL) {
		for (unsigned i = 0; i < MINI_EVENT_WAITS; i++) {
			struct mini_event_wait * w = &gEventWaits[i];

			if (w->event == NULL) {
				w->event = event;
				event->retain();
				w->client = this;
				w->target = target;
				w->port = port;
				port = MACH_PORT_NULL;          /* (the wait's now) */
				memcpy(w->ref, fListeners[l].ref, sizeof(w->ref));
				w->args[0] = args->scalarInput[3];      /* the block */
				w->args[1] = args->scalarInput[2];      /* the event object */
				w->args[2] = target;
				ret = kIOReturnSuccess;
				break;
			}
		}
	}
	IOLockUnlock(gEventLock);
	if (port != MACH_PORT_NULL) {
		releaseNotificationPort(port);          /* (no place left for the wait) */
	}
	if (ret == kIOReturnSuccess) {
		eventTell(event, *event->ev->value);        /* reached already */
	} else {
		static unsigned said;

		if (said++ < 8) {
			IOLog("MiniIOSurface: an event's notification refused: %#x\n", ret);
		}
	}
	event->release();
	return ret;
}

/*
 * The wake port's send right that came with the call is the listener's from here on (on a refusal MIG takes it back);
 * a listener of the same key that was there before is replaced, and its right given back.
 */
IOReturn
IOSurfaceRootUserClient::methodEventAddListener(IOExternalMethodArguments * args)
{
	uint64_t key;
	IOReturn ret = kIOReturnNoResources;
	mach_port_t replaced = MACH_PORT_NULL;
	unsigned l, place = 16;

	if (args->asyncWakePort == MACH_PORT_NULL || args->asyncReference == NULL || args->asyncReferenceCount == 0) {
		return kIOReturnBadArgument;
	}
	key = args->asyncReference[kIOAsyncCalloutRefconIndex];
	if (key == 0) {
		return kIOReturnBadArgument;            /* (0 marks a free place, and no notification names it: methodEventNotify) */
	}
	IOLockLock(gEventLock);
	/* (its own place again if it has one, so that a key is never in two places; else the first free one) */
	for (l = 0; l < 16 && fListeners[l].key != key; l++) {
		if (fListeners[l].key == 0 && place == 16) {
			place = l;
		}
	}
	if (l < 16) {
		place = l;
		replaced = fListeners[l].port;
	}
	if (place < 16) {
		fListeners[place].key = key;
		bzero(fListeners[place].ref, sizeof(fListeners[place].ref));
		memcpy(fListeners[place].ref, args->asyncReference,
		    MIN(sizeof(fListeners[place].ref), args->asyncReferenceCount * sizeof(io_user_reference_t)));
		fListeners[place].port = args->asyncWakePort;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gEventLock);
	if (replaced != MACH_PORT_NULL) {
		releaseNotificationPort(replaced);      /* (a notification still waiting for it has a right of its own) */
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodEventRemoveListener(IOExternalMethodArguments * args)
{
	mach_port_t ports[16];
	unsigned count = 0;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	if (args->scalarInput[0] == 0) {
		return kIOReturnSuccess;                /* (0 is no listener's: methodEventAddListener) */
	}
	IOLockLock(gEventLock);
	for (unsigned l = 0; l < 16; l++) {
		if (fListeners[l].key == args->scalarInput[0]) {
			ports[count++] = fListeners[l].port;
			bzero(&fListeners[l], sizeof(fListeners[l]));
		}
	}
	IOLockUnlock(gEventLock);
	for (unsigned i = 0; i < count; i++) {
		releaseNotificationPort(ports[i]);      /* (the waits still for it have rights of their own) */
	}
	return kIOReturnSuccess;
}

/* The client goes: its notifications still waiting are dropped, and its listeners' wake ports given back. */
void
IOSurfaceRootUserClient::forgetEventWaits(void)
{
	MiniSharedEvent * drop[16];
	mach_port_t dropPorts[16], listenerPorts[16];
	unsigned count, listeners;

	if (gEventLock == NULL) {
		return;
	}
	do {
		count = 0;
		listeners = 0;
		IOLockLock(gEventLock);
		for (unsigned i = 0; i < MINI_EVENT_WAITS && count < 16; i++) {
			if (gEventWaits[i].event != NULL && gEventWaits[i].client == this) {
				drop[count] = gEventWaits[i].event;
				dropPorts[count++] = gEventWaits[i].port;
				bzero(&gEventWaits[i], sizeof(gEventWaits[i]));
			}
		}
		for (unsigned l = 0; l < 16; l++) {
			if (fListeners[l].key != 0) {
				listenerPorts[listeners++] = fListeners[l].port;
			}
		}
		bzero(fListeners, sizeof(fListeners));
		IOLockUnlock(gEventLock);
		for (unsigned i = 0; i < count; i++) {
			releaseNotificationPort(dropPorts[i]);
			drop[i]->release();
		}
		for (unsigned i = 0; i < listeners; i++) {
			releaseNotificationPort(listenerPorts[i]);
		}
	} while (count == 16);
}

IOReturn
IOSurfaceRootUserClient::trapEventSignal(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	MiniSharedEvent * event = copyEvent((mach_port_name_t) (uintptr_t) p1);
	uint64_t value = (uint64_t) p2;

	if (event == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gEventLock);
	*event->ev->value = value;
	IOLockWakeup(gEventLock, (event_t) event, false);
	IOLockUnlock(gEventLock);
	eventTell(event, value);
	event->release();
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapEventWait(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	MiniSharedEvent * event = copyEvent((mach_port_name_t) (uintptr_t) p1);
	uint64_t target = (uint64_t) p2, ms = (uint64_t) p3, deadline;
	IOReturn ret = kIOReturnSuccess;

	if (event == NULL) {
		return kIOReturnBadArgument;
	}
	clock_interval_to_deadline((uint32_t) MIN(ms, 0xffffffffull), kMillisecondScale, &deadline);
	IOLockLock(gEventLock);
	while (*event->ev->value < target) {
		int r = IOLockSleepDeadline(gEventLock, (event_t) event, deadline, THREAD_ABORTSAFE);

		if (r != THREAD_AWAKENED && *event->ev->value < target) {
			ret = r == THREAD_TIMED_OUT ? kIOReturnTimeout : kIOReturnAborted;
			break;
		}
	}
	IOLockUnlock(gEventLock);
	event->release();
	return ret;
}

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService ** targetP, UInt32 index)
{
	if (index >= kSurfaceTrapCount || fClosed) {    /* (closed: as externalMethod) */
		return NULL;
	}
	*targetP = this;
	return &fTraps[index];
}

IOReturn
IOSurfaceRootUserClient::useCountCommon(uint32_t sid, uint32_t category, int delta)
{
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (category > 1) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		MiniIOSurface * s = ref->surface;

		if (delta > 0 || ref->use[category] != 0) {
			ref->use[category] += delta;
			s->useCount[category] += delta;
			s->shared()->useCount = (uint64_t) s->useCount[1] << 32 | s->useCount[0];
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapIncrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, 1);
}

IOReturn
IOSurfaceRootUserClient::trapDecrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, -1);
}

/*
 * Lock and unlock do not exclude anybody: the memory is shared and always mapped.
 * What matters is the seed, which tells readers that the pixels may have changed.
 */
IOReturn
IOSurfaceRootUserClient::lockCommon(uint32_t sid, uint32_t options, user_addr_t seedOut)
{
	struct minisurf_client_ref * ref;
	uint32_t seed = 0;

	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		struct minisurf_shared * sh = ref->surface->shared();

		if (!(options & kSurfaceLockReadOnly)) {
			sh->seed++;
		}
		seed = sh->seed;
	}
	IOLockUnlock(gSurfaceLock);
	if (ref == NULL) {
		return kIOReturnNotFound;
	}
	if (seedOut != 0) {
		copyout(&seed, seedOut, sizeof(seed));
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3);
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3);
}

/*
 * A client drops a surface in two steps: "detach" while it still holds its own lock, and
 * "release" after it. A lookup of the same surface in between must make a fresh view.
 */
IOReturn
IOSurfaceRootUserClient::trapDetach(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;

	IOLockLock(gSurfaceLock);
	ref = findRef((uint32_t) (uintptr_t) p1, false);
	if (ref != NULL) {
		ref->detached = true;
	}
	IOLockUnlock(gSurfaceLock);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;
	uint32_t sid = (uint32_t) (uintptr_t) p1;

	IOLockLock(gSurfaceLock);
	ref = findRef(sid, true);
	if (ref == NULL) {
		ref = findRef(sid, false);
	}
	if (ref != NULL) {
		dropRef(ref);
	}
	IOLockUnlock(gSurfaceLock);
	return ref ? kIOReturnSuccess : kIOReturnNotFound;
}
