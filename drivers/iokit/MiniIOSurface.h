/*
 * MiniIOSurface: a stand-in for Apple's closed IOSurface.kext, enough of it for the Darwin
 * window server and its clients to share pixel buffers on a machine without Apple hardware.
 *
 * The protocol (user client selectors, traps and the layout of the structures the kernel
 * hands to IOSurface.framework) was worked out from the user-space side of Darwin 16.5.
 */
#ifndef _IOKIT_MINIIOSURFACE_H
#define _IOKIT_MINIIOSURFACE_H

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniDrivers.h"

#define MINISURF_MAX_PLANES     4
#define MINISURF_RESULT_MAX     0xa68           /* what IOSurface.framework reserves for a result */

/* One plane, as IOSurface.framework reads it (0x80 bytes). */
struct minisurf_plane {
	uint32_t width;                         /* 0x00 */
	uint32_t height;                        /* 0x04 */
	uint32_t reserved08;
	uint32_t offset;                        /* 0x0c from the base address */
	uint32_t bytesPerRow;                   /* 0x10 */
	uint32_t size;                          /* 0x14 */
	uint16_t bytesPerElement;               /* 0x18 */
	uint8_t  elementWidth;                  /* 0x1a */
	uint8_t  elementHeight;                 /* 0x1b */
	uint8_t  componentCount;                /* 0x1c */
	uint8_t  addressFormat;                 /* 0x1d */
	uint8_t  reserved1e[2];
	uint8_t  componentBitDepth[4];          /* 0x20 */
	uint8_t  componentBitOffset[4];         /* 0x24 */
	uint8_t  componentName[4];              /* 0x28 */
	uint8_t  componentType[4];              /* 0x2c */
	uint8_t  componentRange[4];             /* 0x30 */
	uint8_t  reserved34[0x20];
	uint8_t  compression;                   /* 0x54: 1 = compressed, then there is no base address */
	uint8_t  reserved55[0x1b];
	uint32_t extendedPixels[4];             /* 0x70 */
};

/* What "create" and the lookups return: this header, then the planes (at least one). */
struct minisurf_result {
	uint64_t base;                          /* 0x00 where the memory is mapped in the client */
	uint64_t shared;                        /* 0x08 struct minisurf_shared, mapped in the client */
	uint64_t aux;                           /* 0x10 struct minisurf_aux, mapped in the client */
	uint32_t sid;                           /* 0x18 */
	uint32_t allocSize;                     /* 0x1c */
	uint32_t width;                         /* 0x20 */
	uint32_t height;                        /* 0x24 */
	uint32_t bytesPerRow;                   /* 0x28 */
	uint32_t offset;                        /* 0x2c */
	uint32_t pixelFormat;                   /* 0x30 */
	uint32_t reserved34;
	uint32_t planeCount;                    /* 0x38 */
	uint32_t reserved3c;
	uint16_t bytesPerElement;               /* 0x40 */
	uint8_t  elementWidth;                  /* 0x42 */
	uint8_t  elementHeight;                 /* 0x43 */
	uint32_t cacheMode;                     /* 0x44 */
	uint32_t lockSeed;                      /* 0x48 the client keeps the seed of its last lock here */
	uint8_t  subsampling;                   /* 0x4c */
	uint8_t  prohibitUseCount;              /* 0x4d */
	uint16_t reserved4e;
	uint32_t parentID;                      /* 0x50 */
	uint32_t sliceCount;                    /* 0x54 */
	uint64_t traceID;                       /* 0x58 */
	uint64_t protectionOptions;             /* 0x60 */
};

/* State that every client reads straight from memory shared with the kernel. */
struct minisurf_shared {
	uint64_t registryID;                    /* 0x00 */
	uint16_t accelID;                       /* 0x08 */
	uint16_t accelSubID;                    /* 0x0a */
	uint32_t seed;                          /* 0x0c changes when somebody has written the pixels */
	uint32_t reserved10;
	uint8_t  tiled;                         /* 0x14 */
	uint8_t  reserved15[3];
	uint64_t useCount;                      /* 0x18 two 32-bit counters, one per category */
	uint32_t valuesSeed;                    /* 0x20 */
	uint32_t ycbcrMatrix;                   /* 0x24 */
	uint32_t valuesSize;                    /* 0x28 room needed to fetch the values; 0 = none */
	uint8_t  pixelSizeCasting;              /* 0x2c */
	uint8_t  displayable;                   /* 0x2d */
	uint8_t  sysMemOnly;                    /* 0x2e */
	uint8_t  reserved2f;
	uint8_t  attachments[0x70];             /* 0x30 the "bulk attachments" */
};

struct minisurf_aux {
	uint64_t timestamps[4];
	uint64_t detachModeCode;
};

#define MINISURF_AUX_OFFSET     0x400           /* of the aux structure inside the shared page */

class MiniIOSurface : public OSObject
{
	OSDeclareDefaultStructors(MiniIOSurface);

public:
	uint32_t                        sid;
	uint32_t                        holders;        /* clients, send rights, kernel users */
	IOBufferMemoryDescriptor *      memory;         /* one page of shared state, then the pixels */
	uint8_t *                       kbase;          /* the same, in the kernel */
	/*
	 * ... unless the surface was made around memory its maker has already (IOSurfaceAddress):
	 * then the pixels are that memory, held in place and seen from the kernel as well, and
	 * `memory` is the page of shared state alone.
	 */
	IOMemoryDescriptor *            client;
	IOMemoryMap *                   clientMap;
	task_t                          clientTask;     /* whose memory it is (not held: only compared) */
	mach_vm_address_t               clientAddress;  /* ... and where it is there */
	/*
	 * ... and its maker may only read it (a picture that is only shown: a file mapped read-only, say), so it was held
	 * for reading alone (kIODirectionOut). Nobody may write it through the surface then: not the GPU either, which
	 * reaches the pages themselves and not the maker's mapping of them (BoardGPU.cpp maps them read-only).
	 */
	bool                            clientReadOnly;
	uint8_t *                       kpixels;        /* where the pixel memory starts in the kernel, either way */
	/*
	 * ... and for a surface made around its maker's memory, a copy of the pixels as they were when it was made (or
	 * last written under a lock: snapshotSeed), which is what the GPU draws from (BoardGPU.cpp). Its maker may draw
	 * the next picture into the same memory as soon as the surface is handed over -- iconservicesagent does, and the
	 * icons on the home screen showed the labels of other icons -- and what was handed over is what was there then.
	 */
	IOBufferMemoryDescriptor *      snapshot;
	uint32_t                        snapshotSeed;
	uint32_t                        drawn;          /* the GPU's last submission that names the pixels (0: none; BoardGPU.cpp) */
	/*
	 * The display's driver is copying the pixels to the panel (MiniIOMobileFramebuffer show()): a submission that has the
	 * GPU draw into them waits until it is done (BoardGPU.cpp submit). The window server draws its next frame into the
	 * page of its display it swapped two frames before; while it waited for the GPU between frames that page's copy was
	 * long over, and once it drew a frame while the GPU had the one before, the GPU wrote the page under the copy -- rows
	 * of two frames in one picture.
	 */
	volatile uint32_t               shownReading;
	struct minisurf_result          info;           /* addresses not filled in */
	struct minisurf_plane           planes[MINISURF_MAX_PLANES];
	OSDictionary *                  values;
	uint32_t                        useCount[2];
	uint32_t                        purgeable;
	/*
	 * Of `holders`, those of other parts of the kernel (MiniIOSurfaceCopy(): a swap the display has yet to show, a GPU
	 * buffer), and when one was last taken or given back or the surface made: a surface its makers let be thrown away
	 * (purgeable) is counted as idle only when neither has it now nor had it a moment ago (MiniIOSurfaceVolatile()).
	 */
	uint32_t                        kernelHolds;
	uint64_t                        touched;
	bool                            traced;         /* made by a program of boot-arg surftrace_of: its ports are said too */

	virtual bool
	init(void) APPLE_KEXT_OVERRIDE
	{
		return OSObject::init();
	}
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	struct minisurf_shared *
	shared(void) const
	{
		return (struct minisurf_shared *) kbase;
	}
	struct minisurf_aux *
	aux(void) const
	{
		return (struct minisurf_aux *) (kbase + MINISURF_AUX_OFFSET);
	}
	/* The first byte of the pixels (the "base address" clients see). */
	uint8_t *
	pixels(void) const
	{
		return kpixels + info.offset;
	}
	/*
	 * Whether LENGTH bytes from byte AT of the pixels (from pixels()) lie in the surface's memory, which is allocSize bytes
	 * from kpixels (ours, or its maker's mapped). Its rows' geometry and its size are the maker's numbers and are not tied
	 * to each other when it is made: whoever reads rows of it asks this first.
	 */
	bool
	holds(uint64_t at, uint64_t length) const
	{
		uint64_t start = (uint64_t) info.offset + at;

		return start <= info.allocSize && length <= info.allocSize - start;
	}
	void updateValuesState(void);
};

/*
 * For other parts of the kernel (the framebuffer).
 * A surface found or made here is held until MiniIOSurfaceDrop().
 */
MiniIOSurface * MiniIOSurfaceCopy(uint32_t sid);
/*
 * What making and freeing surfaces has cost since this was last asked: how many, microseconds
 * each; how many got memory that was kept from one before.
 */
void            MiniIOSurfaceCost(uint32_t * made, uint64_t * making, uint32_t * freed, uint64_t * freeing, uint32_t * again);
/*
 * The surfaces of our own memory their makers have marked volatile (IOSurfaceSetPurgeable): how many and how many bytes are
 * idle -- no use count, no other part of the kernel holding them, none for a few seconds -- and how many are marked at all.
 * Only counted: nothing is taken from them (see the comment above it in MiniIOSurface.cpp).
 */
extern "C" void MiniIOSurfaceVolatile(uint32_t * idle, uint64_t * idleBytes, uint32_t * marked, uint64_t * markedBytes);
/* Clear the memory that is kept of surfaces that are gone, for the next ones: when the caller has a moment. */
void            MiniIOSurfaceClearKept(void);
/* Wait, for so many milliseconds at most, until the GPU is done with a surface's pixels (BoardGPU.cpp). */
bool            MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds);
MiniIOSurface * MiniIOSurfaceCreateSimple(uint32_t width, uint32_t height, uint32_t pixelFormat);
void            MiniIOSurfaceDrop(MiniIOSurface * surface);
/* A Mach send right for the surface in the task (what IOSurfaceCreateMachPort gives); 0 on failure. */
mach_port_name_t MiniIOSurfaceMakeSendRight(task_t task, MiniIOSurface * surface);


#endif /* _IOKIT_MINIIOSURFACE_H */
