/*
 * HakiosMetal: the plug-in Metal loads when the kernel names it (boot-arg metal=N): a device, and the first objects it makes
 * that hold something (buffers, libraries, functions: "STEP TWO" below).
 *
 * What Metal does (docs/ru/metal.md section 1): it finds the IOKit service of class IOAcceleratorES, reads its
 * properties MetalPluginName ("HakiosMetal") and MetalPluginClassName ("HakiosMetalDevice"), opens
 * /System/Library/Extensions/HakiosMetal.bundle, takes that class, insists that it is a subclass of Metal's private
 * _MTLDevice, and sends it -initWithAcceleratorPort:. That object is the system's one device.
 *
 * What this is, and is not. HakiosMetalDevice is a subclass of _MTLDevice that answers -initWithAcceleratorPort: and -name,
 * makes buffers that hold memory and libraries read from a .metallib, and stand-ins for the rest (a queue that queues nothing,
 * pipeline and state objects that hold nothing). It cannot run anything on the GPU yet, and says so: a message it does not know
 * is logged ("HakiosMetal: asked and not implemented: -...") and answers nil/0/NO instead of crashing, so that the question
 * "what does Darwin ask a device first, and in what order?" can be read off the log rather than guessed. The compiler for the shaders is a separate piece and is largely done
 * (docs/air.md: Apple's shader bitcode into Mesa's NIR); the command queue and the kernel side are not started (docs/metal-plan.md).
 *
 * _MTLDevice is not in the SDK's Metal.tbd (it lives in the system's own copy of the framework), so the class is made at run
 * time, in the constructor that runs when the bundle is loaded, by objc_allocateClassPair on the _MTLDevice that is there.
 * If there is none, nothing is registered, and Metal drops this plug-in as it does any that is not a _MTLDevice.
 *
 * Status. Step one ran in QEMU (docs/metal-plan.md: what it found). Step two -- buffers, libraries, functions -- was built for
 * Darwin userland 16.5 and run on a Mac only, against the Mac's own Metal (tools/metal-plugin-test.sh, under the address sanitizer: every
 * buffer written and read back and freed, the system's five MTLB libraries read, all of their functions by name with their
 * bitcode); it has not run on Darwin yet: metal_test's "what the device makes" is the check for that (QEMU, metal=1).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static Class gBase;                     /* Metal's private _MTLDevice */
static unsigned int gPort;              /* the IOAcceleratorES service this device was made for */
static id gDevice;                      /* the device, once made: there is one */

static void
say(NSString * text)
{
	NSLog(@"HakiosMetal: %@", text);
}

/* -initWithAcceleratorPort: the fifth step of how Metal finds a device. */
static id
deviceInit(id self, SEL _cmd, unsigned int port)
{
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL) = (id (*)(struct objc_super *, SEL))objc_msgSendSuper;

	self = send(&sup, @selector(init));
	gPort = port;
	gDevice = self;                         /* what the objects it hands out answer to -device */
	say([NSString stringWithFormat:@"a device for the accelerator port %u (%@)", port, self ? @"made" : @"_MTLDevice's init gave nil"]);
	return self;
}

static id
deviceName(__unused id self, __unused SEL _cmd)
{
	return @"HakiOS Adreno 642L (Metal: a device that exists, nothing more)";
}

static uint64_t
deviceRegistryID(__unused id self, __unused SEL _cmd)
{
	return gPort;
}

/* the largest buffer: what the GPU memory could hold, not a promise (nothing allocates a buffer yet) */
static uint64_t
deviceMaxBufferLength(__unused id self, __unused SEL _cmd)
{
	return 256ull << 20;
}

/* the largest threadgroup: the Adreno 6xx's (a workgroup of 1024 invocations), as Apple's GPUs answer too */
static MTLSize
deviceMaxThreadsPerThreadgroup(__unused id self, __unused SEL _cmd)
{
	return MTLSizeMake(1024, 1024, 1024);
}

/*
 * The command queue: the first thing the window server wants after the device (the second run's log: "[Metal] Failed to create
 * MTLCommandQueue", then the process ends). Made as a subclass of Metal's private _MTLCommandQueue, at the same time as the device's
 * class, with the same stub for what it does not answer. It queues nothing and executes nothing: a command buffer asked of it is
 * nil, and the log says so, which is the next step's list.
 */
static Class gQueueClass;

#ifdef HAKIOS_METAL_GPU
/*
 * The GPU itself, through hakios-metal (github.com/oosexgang/hakios-metal, C++, its C interface hakios_metal.h): buffers in
 * the GPU's memory, command buffers, and blits done by the GPU's command processor. Without the kernel's GPU (no gpu=1,
 * QEMU) nothing of this is used and the device is what it was: buffers in plain memory, a queue that queues nothing.
 */
#include "hakios-metal/hakios_metal.h"

static void **stateOf(id self);

static hakios_metal_device *gGPU;               /* opened once; NULL: the kernel has no GPU to give */
static Class gCommandBufferClass, gBlitEncoderClass;

struct commandbuffer {
	hakios_metal_cb *cb;
	id queue;                               /* retained */
	NSMutableArray *handlers;               /* the completed handlers, copied */
	NSMutableArray *scheduled;              /* the scheduled handlers, copied */
	BOOL committed;
	id label;
};

struct blitencoder {
	id commandBuffer;                       /* retained */
	BOOL ended;
	id label;
};

static hakios_metal_device *
gpu(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
#if !TARGET_OS_IPHONE
		/* on a Mac, HAKIOS_METAL_FAKE=1: the core's fake GPU, which runs the command streams it is given, so that command
		 * buffers and blits are tried there too (plugin/tests/metal-plugin-test.sh, GPU=1) */
		gGPU = getenv("HAKIOS_METAL_FAKE") != NULL ? hakios_metal_open_fake() : hakios_metal_open();
#else
		gGPU = hakios_metal_open();
#endif
		say(gGPU != NULL ? [NSString stringWithFormat:@"the GPU: Adreno %#llx, through hakios-metal", hakios_metal_gpu_id(gGPU)]
		                 : [NSString stringWithFormat:@"no GPU (%s): buffers in plain memory, no command buffers", hakios_metal_error()]);
	});
	return gGPU;
}
#endif

static id
queueDevice(__unused id self, __unused SEL _cmd)
{
	return gDevice;
}

static id
queueLabel(__unused id self, __unused SEL _cmd)
{
	return @"HakiosMetalQueue";
}

static void
queueSetGPUPriority(__unused id self, __unused SEL _cmd, __unused unsigned long priority)
{
}

static id
deviceNewQueue(id self, __unused SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id queue;

	gDevice = self;
	if (gQueueClass == Nil) {
		say(@"newCommandQueue: no queue class (this system has no _MTLCommandQueue): nil");
		return nil;
	}
	queue = plain(plain((id)gQueueClass, sel_registerName("alloc")), sel_registerName("init"));
#ifdef HAKIOS_METAL_GPU
	if (queue != nil && gpu() != NULL) {
		void **slot = stateOf(queue);
		hakios_metal_queue *q = hakios_metal_queue_new(gGPU);

		if (slot != NULL && q != NULL) {
			*slot = q;
			say(@"newCommandQueue: a queue of the GPU's");
			return queue;
		}
		say([NSString stringWithFormat:@"newCommandQueue: no GPU queue (%s)", hakios_metal_error()]);
	}
#endif
	say([NSString stringWithFormat:@"newCommandQueue: a queue that queues nothing (%@)", queue ? @"made" : @"_MTLCommandQueue's init gave nil"]);
	return queue;
}

static id
deviceNewQueueMax(id self, SEL _cmd, __unused unsigned long maxBuffers)
{
	return deviceNewQueue(self, _cmd);
}

/* the protocols the class declares (the ones Metal checks for), for the signatures below */
static Protocol * gProtocols[64];
static unsigned gProtocolCount;

/*
 * A message the class does not have, but a protocol it declares does: Metal sends the whole of MTLDevice and the private
 * MTLDeviceSPI, and a method that is missing would end the process ("unrecognized selector": -featureProfile was the first).
 * The type of the answer is read from the protocol's own description of the method, so that zero comes back in the right
 * size, and the message is logged once. What a method should answer is learned one message at a time from this log.
 */
static id
deviceSignature(id self, SEL _cmd, SEL selector)
{
	for (unsigned i = 0; i < gProtocolCount; i++) {
		for (int required = 1; required >= 0; required--) {
			struct objc_method_description d = protocol_getMethodDescription(gProtocols[i], selector, required, YES);

			if (d.name != NULL && d.types != NULL) {
				id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;

				return make(objc_getClass("NSMethodSignature"), sel_registerName("signatureWithObjCTypes:"), d.types);
			}
		}
	}
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL, SEL) = (id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper;

	(void)_cmd;
	return send(&sup, sel_registerName("methodSignatureForSelector:"), selector);
}

static void
deviceForward(id self, SEL _cmd, id invocation)
{
	static NSMutableSet * seen;
	SEL selector = ((SEL (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("selector"));
	id signature = ((id (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("methodSignature"));
	NSUInteger length = ((NSUInteger (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnLength"));
	NSString * name = NSStringFromSelector(selector);

	(void)self;
	(void)_cmd;
	@synchronized(gBase) {
		if (seen == nil) {
			seen = [NSMutableSet new];
		}
		if (![seen containsObject:name]) {
			[seen addObject:name];
			const char * encoding = ((const char * (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnType"));

			say([NSString stringWithFormat:@"asked and not implemented: -%@ (returns %lu bytes, type %s; answered with zero)", name,
			    (unsigned long)length, encoding ? encoding : "?"]);
		}
	}
	if (length != 0) {
		void * zero = calloc(1, length);

		((void (*)(id, SEL, void *))objc_msgSend)(invocation, sel_registerName("setReturnValue:"), zero);
		free(zero);
	}
}

/*
 * The objects a device hands out. Every one is a class of ours made at load time, a subclass of Metal's private base where this
 * system has one (and of NSObject where it does not), declaring the protocol Metal checks and any private protocol of the same
 * name family, answering the rest through the same logged zero stub as the device. None of them does anything: a state object
 * holds nothing, a buffer has memory and a length and nothing else. They exist so that the next question reaches the log, which is
 * the work list of this plug-in (docs/metal-plan.md).
 */
static const struct stubSpec {
	const char * selector;          /* the device's method that makes it */
	const char * cls;               /* our class */
	const char * base;              /* Metal's private base class, if the system has it */
	const char * protocol;          /* the public protocol, and the family of private ones beginning with it */
} gStubSpecs[] = {
	{ "newDepthStencilStateWithDescriptor:", "HakiosMetalDepthStencilState", "_MTLDepthStencilState", "MTLDepthStencilState" },
	{ "newSamplerStateWithDescriptor:", "HakiosMetalSamplerState", "_MTLSamplerState", "MTLSamplerState" },
	{ "newTextureWithDescriptor:", "HakiosMetalTexture", "_MTLTexture", "MTLTexture" },
	{ "newRenderPipelineStateWithDescriptor:error:", "HakiosMetalRenderPipelineState", "_MTLRenderPipelineState", "MTLRenderPipelineState" },
	{ "newComputePipelineStateWithFunction:error:", "HakiosMetalComputePipelineState", "_MTLComputePipelineState", "MTLComputePipelineState" },
	{ "newEventWithOptions:", "HakiosMetalEvent", "_MTLEvent", "MTLEvent" },
	{ "newFence", "HakiosMetalFence", "_MTLFence", "MTLFence" },
};
#define STUB_COUNT (sizeof(gStubSpecs) / sizeof(gStubSpecs[0]))
static Class gStubClass[STUB_COUNT];

/* a device method that makes one of the objects above: the class is found by the selector it was called by */
static id
stubFactory(__unused id self, SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char * called = sel_getName(_cmd);

	for (unsigned i = 0; i < STUB_COUNT; i++) {
		if (strcmp(gStubSpecs[i].selector, called) == 0) {
			id object;

			if (gStubClass[i] == Nil) {
				return nil;
			}
			object = plain(plain((id)gStubClass[i], sel_registerName("alloc")), sel_registerName("init"));
			say([NSString stringWithFormat:@"%s: a %s that holds nothing (%@)", called, gStubSpecs[i].cls, object ? @"made" : @"init gave nil"]);
			return object;
		}
	}
	return nil;
}

/*
 * STEP TWO (docs/metal-plan.md, steps 3 and 4 as far as they go without the GPU): objects that hold something.
 *
 *   buffers     -newBufferWithLength:options:, ...WithBytes:..., ...WithBytesNoCopy:...: memory a program writes and reads back
 *               through -contents. The Adreno shares the system's memory, so a buffer in shared storage is the same bytes
 *               for the CPU and, later, the GPU; until the GPU is used (hakios.gpu.cpp's buffers, mapped into its address
 *               space) it is ordinary memory of this process, zeroed, and -gpuAddress is 0.
 *   libraries   -newLibraryWithData:error:, -newLibraryWithURL:error:, -newLibraryWithFile:error:, -newDefaultLibrary...:
 *               the .metallib read (hakios_metallib.c, every offset checked), its functions listed by name and type, and
 *               -newFunctionWithName: giving a function that knows its name, its type and its module (the AIR bitcode the
 *               shader compiler of docs/air.md takes). Nothing is compiled yet: a pipeline made of such a function is still
 *               the empty stub above, and the log says so.
 *
 * Each is a class of ours made at load time, as the others are (a subclass of Metal's private _MTLResource, _MTLLibrary and
 * _MTLFunction where this system has them), with one instance variable, _hakios, pointing at its state in C.
 */
#include "hakios_metallib.h"
#include <dispatch/dispatch.h>
#include <sys/mman.h>

static Class gBufferClass, gLibraryClass, gFunctionClass;

struct buffer {
	void *memory;
	uint64_t length;
	unsigned long options;                  /* MTLResourceOptions: cache mode, storage mode, hazard tracking */
	void (^deallocator)(void *, NSUInteger); /* for ...NoCopy: the caller's, called with its memory when the buffer goes */
	BOOL mapped;                            /* ours, from mmap */
	id label;
	void *gpu;                              /* (HAKIOS_METAL_GPU) GPU memory of hakios-metal's, when the kernel has a GPU */
};

struct library {
	NSData *data;                           /* the file's bytes, kept for as long as a function of it may want its module */
	struct hakios_metallib info;
	struct hakios_metallib_function *functions;
	id label;
	id names;                               /* NSArray of the functions' names, made once */
};

struct function {
	id library;                             /* retained: the module is in its bytes */
	struct hakios_metallib_function f;
	id name;
	id label;
};

static struct library *libraryOf(id self);


static void **
stateOf(id self)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(self), "_hakios");

	return ivar != NULL ? (void **)((char *)(__bridge void *)self + ivar_getOffset(ivar)) : NULL;
}

/* -dealloc: the state freed by FREE, then the superclass's own */
static void
deallocWith(id self, SEL _cmd, Class cls, void (*free_state)(void *))
{
	void **slot = stateOf(self);
	struct objc_super sup = { self, class_getSuperclass(cls) };

	if (slot != NULL && *slot != NULL) {
		free_state(*slot);
		*slot = NULL;
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

/*
 * -dealloc of the queue (and of what is made like it): the state freed by FREE (NULL: none), then NSObject's own, not Metal's.
 * The queue is made with -init, not with Metal's initializer (-initWithDevice:), and _MTLCommandQueue's -dealloc lets go of what
 * that makes: its dispatch queue, NULL here. dispatch_release(NULL) faulted on the phone as metal_test's blits ended, and does
 * in plugin/tests/metal_plugin_test.m on a Mac (GPU=1).
 */
static void
deallocPlain(id self, SEL _cmd, void (*free_state)(void *))
{
	void **slot = stateOf(self);
	struct objc_super sup = { self, objc_getClass("NSObject") };

	if (free_state != NULL && slot != NULL && *slot != NULL) {
		free_state(*slot);
		*slot = NULL;
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

static void
queueDealloc(id self, SEL _cmd)
{
#ifdef HAKIOS_METAL_GPU
	/* (no command buffer is in flight: each holds its queue) */
	deallocPlain(self, _cmd, (void (*)(void *))hakios_metal_queue_free);
#else
	deallocPlain(self, _cmd, NULL);
#endif
}

static id
labelGet(id self, __unused SEL _cmd)
{
	id *label = NULL;
	void **slot = stateOf(self);

	if (slot == NULL || *slot == NULL) {
		return nil;
	}
	if (object_getClass(self) == gBufferClass) {
		label = &((struct buffer *)*slot)->label;
	} else if (object_getClass(self) == gLibraryClass) {
		label = &((struct library *)*slot)->label;
#ifdef HAKIOS_METAL_GPU
	} else if (object_getClass(self) == gCommandBufferClass) {
		label = &((struct commandbuffer *)*slot)->label;
	} else if (object_getClass(self) == gBlitEncoderClass) {
		label = &((struct blitencoder *)*slot)->label;
#endif
	} else {
		label = &((struct function *)*slot)->label;
	}
	return *label;
}

static void
labelSet(id self, __unused SEL _cmd, id text)
{
	void **slot = stateOf(self);
	id *label;

	if (slot == NULL || *slot == NULL) {
		return;
	}
	if (object_getClass(self) == gBufferClass) {
		label = &((struct buffer *)*slot)->label;
	} else if (object_getClass(self) == gLibraryClass) {
		label = &((struct library *)*slot)->label;
#ifdef HAKIOS_METAL_GPU
	} else if (object_getClass(self) == gCommandBufferClass) {
		label = &((struct commandbuffer *)*slot)->label;
	} else if (object_getClass(self) == gBlitEncoderClass) {
		label = &((struct blitencoder *)*slot)->label;
#endif
	} else {
		label = &((struct function *)*slot)->label;
	}
	[*label release];
	*label = [text copy];
}

static id
objectDevice(__unused id self, __unused SEL _cmd)
{
	return gDevice;
}

/* ---- buffers ---- */

static void
bufferFree(void *state)
{
	struct buffer *b = state;

	if (b->mapped) {
		munmap(b->memory, b->length ? b->length : 1);
#ifdef HAKIOS_METAL_GPU
	} else if (b->gpu != NULL) {
		hakios_metal_buffer_free(b->gpu);       /* (a command buffer still using it keeps the memory until it is done) */
#endif
	} else if (b->deallocator != nil) {
		b->deallocator(b->memory, (NSUInteger)b->length);
	}
	[b->deallocator release];
	[b->label release];
	free(b);
}

static void
bufferDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gBufferClass, bufferFree);
}

static struct buffer *
bufferOf(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static void *
bufferContents(id self, __unused SEL _cmd)
{
	struct buffer *b = bufferOf(self);

	return b != NULL ? b->memory : NULL;
}

static unsigned long
bufferLength(id self, __unused SEL _cmd)
{
	struct buffer *b = bufferOf(self);

	return b != NULL ? (unsigned long)b->length : 0;
}

static unsigned long
bufferOptions(id self, __unused SEL _cmd)
{
	struct buffer *b = bufferOf(self);

	return b != NULL ? b->options : 0;
}

static unsigned long
bufferStorageMode(id self, SEL _cmd)
{
	return bufferOptions(self, _cmd) >> MTLResourceStorageModeShift & 0xf;
}

static unsigned long
bufferCacheMode(id self, SEL _cmd)
{
	return bufferOptions(self, _cmd) >> MTLResourceCPUCacheModeShift & 0xf;
}

static unsigned long
bufferHazardMode(id self, SEL _cmd)
{
	return bufferOptions(self, _cmd) >> MTLResourceHazardTrackingModeShift & 0x3;
}

/* the memory a buffer takes: its pages */
static unsigned long
bufferAllocatedSize(id self, SEL _cmd)
{
	unsigned long page = (unsigned long)getpagesize();

	return (bufferLength(self, _cmd) + page - 1) / page * page;
}

/* shared memory: what the CPU wrote is what there is; nothing to tell anyone */
static void
bufferDidModify(__unused id self, __unused SEL _cmd, __unused NSRange range)
{
}

/* no GPU address until the GPU's own buffers are used (hakios.gpu.cpp) */
static uint64_t
bufferGPUAddress(__unused id self, __unused SEL _cmd)
{
#ifdef HAKIOS_METAL_GPU
	struct buffer *b = bufferOf(self);

	if (b != NULL && b->gpu != NULL) {
		return hakios_metal_buffer_gpu_address(b->gpu);
	}
#endif
	return 0;
}

static unsigned long
bufferPurgeable(__unused id self, __unused SEL _cmd, __unused unsigned long state)
{
	return MTLPurgeableStateNonVolatile;
}

static id
bufferMake(void *memory, uint64_t length, unsigned long options, BOOL mapped, void (^deallocator)(void *, NSUInteger))
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct buffer *b;
	id buffer;
	void **slot;

	if (gBufferClass == Nil || (buffer = plain(plain((id)gBufferClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		say(@"a buffer: the class could not make one (its base's -init gave nil)");
		return nil;
	}
	b = calloc(1, sizeof(*b));
	slot = stateOf(buffer);
	if (b == NULL || slot == NULL) {
		free(b);
		[buffer release];
		return nil;
	}
	b->memory = memory;
	b->length = length;
	b->options = options;
	b->mapped = mapped;
	b->deallocator = [deallocator copy];
	*slot = b;
	return buffer;
}

static void *
pages(uint64_t length)
{
	void *memory = mmap(NULL, length ? length : 1, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	return memory == MAP_FAILED ? NULL : memory;
}

static id
deviceNewBuffer(__unused id self, __unused SEL _cmd, unsigned long length, unsigned long options)
{
	void *memory;
	id buffer;

#ifdef HAKIOS_METAL_GPU
	if (gpu() != NULL && length <= deviceMaxBufferLength(nil, NULL)) {
		hakios_metal_buffer *g = hakios_metal_buffer_new(gGPU, length);

		if (g != NULL) {
			buffer = bufferMake(hakios_metal_buffer_contents(g), length, options, NO, nil);
			if (buffer == nil) {
				hakios_metal_buffer_free(g);
				return nil;
			}
			bufferOf(buffer)->gpu = g;
			return buffer;
		}
		say([NSString stringWithFormat:@"newBufferWithLength:%lu: no GPU memory (%s): plain memory instead", length, hakios_metal_error()]);
	}
#endif
	if (length > deviceMaxBufferLength(nil, NULL) || (memory = pages(length)) == NULL) {
		say([NSString stringWithFormat:@"newBufferWithLength:%lu: no (more than maxBufferLength, or no memory): nil", length]);
		return nil;
	}
	buffer = bufferMake(memory, length, options, YES, nil);
	if (buffer == nil) {
		munmap(memory, length ? length : 1);
	}
	return buffer;
}

static id
deviceNewBufferBytes(id self, SEL _cmd, const void *bytes, unsigned long length, unsigned long options)
{
	id buffer = deviceNewBuffer(self, _cmd, length, options);

	if (buffer != nil && bytes != NULL && length != 0) {
		memcpy(bufferContents(buffer, NULL), bytes, length);
	}
	return buffer;
}

static id
deviceNewBufferNoCopy(__unused id self, __unused SEL _cmd, void *bytes, unsigned long length, unsigned long options,
    void (^deallocator)(void *, NSUInteger))
{
	if (bytes == NULL || ((uintptr_t)bytes & (uintptr_t)(getpagesize() - 1)) != 0) {
		say(@"newBufferWithBytesNoCopy: the memory must start on a page (as Metal says): nil");
		return nil;
	}
	return bufferMake(bytes, length, options, NO, deallocator);
}

/* ---- functions ---- */

static void
functionFree(void *state)
{
	struct function *f = state;

	[f->library release];
	[f->name release];
	[f->label release];
	free(f);
}

static void
functionDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gFunctionClass, functionFree);
}

static struct function *
functionOf(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static id
functionName(id self, __unused SEL _cmd)
{
	struct function *f = functionOf(self);

	return f != NULL ? f->name : nil;
}

/* MTLFunctionType: the metallib's TYPE as Metal numbers it */
static unsigned long
functionType(id self, __unused SEL _cmd)
{
	struct function *f = functionOf(self);

	switch (f != NULL ? f->f.type : HAKIOS_METALLIB_NO_TYPE) {
	case HAKIOS_METALLIB_VERTEX:
		return MTLFunctionTypeVertex;
	case HAKIOS_METALLIB_FRAGMENT:
		return MTLFunctionTypeFragment;
	case HAKIOS_METALLIB_KERNEL:
		return MTLFunctionTypeKernel;
	case HAKIOS_METALLIB_INTERSECTION:
		return MTLFunctionTypeIntersection;
	default:
		return MTLFunctionTypeVisible;
	}
}

static id
functionNoAttributes(__unused id self, __unused SEL _cmd)
{
	return [NSArray array];
}

static id
functionNoConstants(__unused id self, __unused SEL _cmd)
{
	return [NSDictionary dictionary];
}

/* not Metal's: the function's AIR module, for the shader compiler (docs/air.md) */
static id
functionBitcode(id self, __unused SEL _cmd)
{
	struct function *f = functionOf(self);
	struct library *l;
	const uint8_t *bitcode;
	uint64_t length;

	if (f == NULL || (l = libraryOf(f->library)) == NULL ||
	    hakios_metallib_bitcode([l->data bytes], [l->data length], &f->f, &bitcode, &length) != 0) {
		return nil;
	}
	return [NSData dataWithBytesNoCopy:(void *)bitcode length:(NSUInteger)length freeWhenDone:NO];
}

/* ---- libraries ---- */

static void
libraryFree(void *state)
{
	struct library *l = state;

	[l->data release];
	[l->names release];
	[l->label release];
	free(l->functions);
	free(l);
}

static void
libraryDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gLibraryClass, libraryFree);
}

static struct library *
libraryOf(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static id
libraryFunctionNames(id self, __unused SEL _cmd)
{
	struct library *l = libraryOf(self);

	return l != NULL ? l->names : nil;
}

static id
libraryNewFunction(id self, __unused SEL _cmd, id name)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct library *l = libraryOf(self);
	const char *wanted = [name UTF8String];
	size_t length = wanted != NULL ? strlen(wanted) : 0;

	if (l == NULL || wanted == NULL) {
		return nil;
	}
	for (uint32_t i = 0; i < l->info.count; i++) {
		struct hakios_metallib_function *f = &l->functions[i];

		if (f->name_length == length && memcmp(f->name, wanted, length) == 0) {
			struct function *state = calloc(1, sizeof(*state));
			id function = gFunctionClass != Nil ? plain(plain((id)gFunctionClass, sel_registerName("alloc")), sel_registerName("init")) : nil;
			void **slot = function != nil ? stateOf(function) : NULL;

			if (state == NULL || slot == NULL) {
				free(state);
				[function release];
				say([NSString stringWithFormat:@"newFunctionWithName:%@: the function class could not make one: nil", name]);
				return nil;
			}
			state->library = [self retain];
			state->f = *f;
			state->name = [name copy];
			*slot = state;
			return function;
		}
	}
	return nil;
}

/* the same, with function constants: there is no compiler yet to specialise with them, so they are only said */
static id
libraryNewFunctionConstants(id self, SEL _cmd, id name, id constants, NSError **error)
{
	id function = libraryNewFunction(self, _cmd, name);

	say([NSString stringWithFormat:@"newFunctionWithName:%@ constantValues: the constants are not applied (no compiler yet)", name]);
	(void)constants;
	if (function == nil && error != NULL) {
		*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorFunctionNotFound
		    userInfo:@{ NSLocalizedDescriptionKey: [NSString stringWithFormat:@"no function %@ in the library", name] }];
	}
	return function;
}

static unsigned long
libraryType(__unused id self, __unused SEL _cmd)
{
	return MTLLibraryTypeExecutable;
}

static id
libraryError(MTLLibraryError code, NSString *text, NSError **error)
{
	say(text);
	if (error != NULL) {
		*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:code userInfo:@{ NSLocalizedDescriptionKey: text }];
	}
	return nil;
}

/* a library of the bytes of a .metallib (DATA retained by the library); WHERE names them in the log */
static id
libraryMake(NSData *data, NSString *where, NSError **error)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct library *l;
	const char *why = NULL;
	id library;
	void **slot;
	unsigned kinds[8] = { 0 };

	if (gLibraryClass == Nil) {
		return libraryError(MTLLibraryErrorInternal, @"a library: this system has no class for one", error);
	}
	/* a universal file (AIR64 beside code for Apple's GPUs): its AIR64 slice is the library, as Metal itself takes it */
	const uint8_t *thin;
	size_t thin_size;
	if (hakios_metallib_thin([data bytes], [data length], &thin, &thin_size, &why) != 0) {
		return libraryError(MTLLibraryErrorInternal, [NSString stringWithFormat:@"a library from %@: %s", where, why], error);
	}
	if (thin != [data bytes]) {
		data = [data subdataWithRange:NSMakeRange((NSUInteger)(thin - (const uint8_t *)[data bytes]), thin_size)];
		say([NSString stringWithFormat:@"a library from %@: a universal file, its AIR64 slice taken (%zu bytes)", where, thin_size]);
	}
	l = calloc(1, sizeof(*l));
	if (l == NULL) {
		return libraryError(MTLLibraryErrorInternal, @"a library: no memory", error);
	}
	if (hakios_metallib_parse([data bytes], [data length], &l->info, NULL, 0, &why) != 0 ||
	    (l->functions = calloc(l->info.count ? l->info.count : 1, sizeof(*l->functions))) == NULL ||
	    hakios_metallib_parse([data bytes], [data length], &l->info, l->functions, l->info.count, &why) != 0) {
		free(l->functions);
		free(l);
		return libraryError(MTLLibraryErrorInternal, [NSString stringWithFormat:@"a library from %@: %s", where,
		    why ? why : "no memory"], error);
	}
	NSMutableArray *names = [NSMutableArray arrayWithCapacity:l->info.count];
	for (uint32_t i = 0; i < l->info.count; i++) {
		NSString *name = [[NSString alloc] initWithBytes:l->functions[i].name length:l->functions[i].name_length encoding:NSUTF8StringEncoding];

		if (name != nil) {
			[names addObject:name];
			[name release];
		}
		kinds[l->functions[i].type < 7 ? l->functions[i].type : 7]++;
	}
	library = plain(plain((id)gLibraryClass, sel_registerName("alloc")), sel_registerName("init"));
	slot = library != nil ? stateOf(library) : NULL;
	if (slot == NULL) {
		free(l->functions);
		free(l);
		[library release];
		return libraryError(MTLLibraryErrorInternal, @"a library: the class could not make one (its base's -init gave nil)", error);
	}
	l->data = [data retain];
	l->names = [names copy];
	*slot = l;
	say([NSString stringWithFormat:@"a library from %@: %u functions (%u vertex, %u fragment, %u kernel, %u other), container %u.%u",
	    where, l->info.count, kinds[0], kinds[1], kinds[2], l->info.count - kinds[0] - kinds[1] - kinds[2],
	    l->info.version[0], l->info.version[1]]);
	return library;
}

static id
deviceNewLibraryURL(__unused id self, __unused SEL _cmd, NSURL *url, NSError **error)
{
	NSData *data = url != nil ? [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:NULL] : nil;

	if (data == nil) {
		return libraryError(MTLLibraryErrorFileNotFound, [NSString stringWithFormat:@"a library from %@: no such file", url], error);
	}
	return libraryMake(data, [url path], error);
}

static id
deviceNewLibraryFile(id self, SEL _cmd, NSString *path, NSError **error)
{
	return deviceNewLibraryURL(self, _cmd, path != nil ? [NSURL fileURLWithPath:path] : nil, error);
}

static id
deviceNewLibraryData(__unused id self, __unused SEL _cmd, dispatch_data_t data, NSError **error)
{
	const void *bytes = NULL;
	size_t size = 0;
	dispatch_data_t map;
	NSData *copy;

	if (data == nil) {
		return libraryError(MTLLibraryErrorFileNotFound, @"a library from data: no data", error);
	}
	map = dispatch_data_create_map(data, &bytes, &size);
	copy = [NSData dataWithBytes:bytes length:size];        /* the caller may let go of its data; the library keeps its own */
	dispatch_release(map);
	return libraryMake(copy, @"data", error);
}

/* a bundle's default.metallib: the main bundle's for -newDefaultLibrary */
static id
deviceNewDefaultLibraryBundle(id self, SEL _cmd, NSBundle *bundle, NSError **error)
{
	NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];

	if (url == nil) {
		return libraryError(MTLLibraryErrorFileNotFound, [NSString stringWithFormat:@"the default library of %@: no default.metallib",
		    [bundle bundlePath]], error);
	}
	return deviceNewLibraryURL(self, _cmd, url, error);
}

static id
deviceNewDefaultLibrary(id self, SEL _cmd)
{
	return deviceNewDefaultLibraryBundle(self, _cmd, [NSBundle mainBundle], NULL);
}

static BOOL
deviceUnifiedMemory(__unused id self, __unused SEL _cmd)
{
	return YES;                             /* the Adreno has no memory of its own */
}

/*
 * What CoreImage's CIMetalDeviceGetLimits reads of a device and _MTLDevice does not answer (Apple's own GPU devices do): without
 * them every CIContext creation throws, and a process drawing symbols through CoreImage (iconservicesagent) ends. UniXNU's
 * AdrenoMetal found this on iPadOS 17; the answers are the same here: no memory of its own, the system's shared.
 */
static uint64_t
deviceNoDedicatedMemory(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static uint64_t
deviceSharedMemory(__unused id self, __unused SEL _cmd)
{
	return [[NSProcessInfo processInfo] physicalMemory];
}

static BOOL
deviceNo(__unused id self, __unused SEL _cmd)
{
	return NO;
}

/* no legacy feature set is claimed: _MTLDevice's own answer reads tables of Apple's GPUs */
static BOOL
deviceNoFeatureSet(__unused id self, __unused SEL _cmd, __unused unsigned long set)
{
	return NO;
}

/*
 * The public protocol and Metal's private one of the same name (MTLBufferSPI ...), where this system has it: not every protocol
 * whose name begins the same, as for the stub objects, because MTLFunction begins a dozen others (MTLFunctionStitchingNode ...)
 * that a function is not.
 */
static void
declareProtocols(Class cls, const char *const *names)
{
	char spi[96];

	for (; *names != NULL; names++) {
		for (int private = 0; private < 2; private++) {
			Protocol *protocol;

			snprintf(spi, sizeof(spi), "%s%s", *names, private ? "SPI" : "");
			if ((protocol = objc_getProtocol(spi)) == NULL) {
				continue;
			}
			class_addProtocol(cls, protocol);
			if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
				gProtocols[gProtocolCount++] = protocol;
			}
		}
	}
}

/* a class of ours with the _hakios slot, on Metal's private BASE where there is one */
static Class
objectClass(const char *name, const char *base, const char *const *protocols, IMP dealloc)
{
	Class super = objc_getClass(base);
	Class cls = objc_allocateClassPair(super != Nil ? super : objc_getClass("NSObject"), name, 0);

	if (cls == Nil) {
		say([NSString stringWithFormat:@"could not make the class %s", name]);
		return Nil;
	}
	class_addIvar(cls, "_hakios", sizeof(void *), sizeof(void *) == 8 ? 3 : 2, "^v");
	declareProtocols(cls, protocols);
	class_addMethod(cls, sel_registerName("dealloc"), dealloc, "v@:");
	class_addMethod(cls, sel_registerName("device"), (IMP)objectDevice, "@@:");
	class_addMethod(cls, sel_registerName("label"), (IMP)labelGet, "@@:");
	class_addMethod(cls, sel_registerName("setLabel:"), (IMP)labelSet, "v@:@");
	class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
	class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
	say([NSString stringWithFormat:@"%s is registered (a subclass of %s)", name, super != Nil ? base : "NSObject: this system has no such class"]);
	return cls;
}

static void
registerObjectClasses(Class device)
{
	static const char *const buffer[] = { "MTLBuffer", "MTLResource", NULL }, *const function[] = { "MTLFunction", NULL },
	    *const library[] = { "MTLLibrary", NULL };

	if ((gBufferClass = objectClass("HakiosMetalBuffer", "_MTLResource", buffer, (IMP)bufferDealloc)) != Nil) {
		class_addMethod(gBufferClass, sel_registerName("contents"), (IMP)bufferContents, "^v@:");
		class_addMethod(gBufferClass, sel_registerName("length"), (IMP)bufferLength, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("resourceOptions"), (IMP)bufferOptions, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("storageMode"), (IMP)bufferStorageMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("cpuCacheMode"), (IMP)bufferCacheMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("hazardTrackingMode"), (IMP)bufferHazardMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("allocatedSize"), (IMP)bufferAllocatedSize, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("didModifyRange:"), (IMP)bufferDidModify, "v@:{_NSRange=QQ}");
		class_addMethod(gBufferClass, sel_registerName("gpuAddress"), (IMP)bufferGPUAddress, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("setPurgeableState:"), (IMP)bufferPurgeable, "Q@:Q");
		objc_registerClassPair(gBufferClass);
		class_addMethod(device, sel_registerName("newBufferWithLength:options:"), (IMP)deviceNewBuffer, "@@:QQ");
		class_addMethod(device, sel_registerName("newBufferWithBytes:length:options:"), (IMP)deviceNewBufferBytes, "@@:r^vQQ");
		class_addMethod(device, sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:"), (IMP)deviceNewBufferNoCopy,
		    "@@:^vQQ@?");
	}
	if ((gFunctionClass = objectClass("HakiosMetalFunction", "_MTLFunction", function, (IMP)functionDealloc)) != Nil) {
		class_addMethod(gFunctionClass, sel_registerName("name"), (IMP)functionName, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("functionType"), (IMP)functionType, "Q@:");
		class_addMethod(gFunctionClass, sel_registerName("vertexAttributes"), (IMP)functionNoAttributes, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("stageInputAttributes"), (IMP)functionNoAttributes, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("functionConstantsDictionary"), (IMP)functionNoConstants, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("hakiosBitcode"), (IMP)functionBitcode, "@@:");
		objc_registerClassPair(gFunctionClass);
	}
	if ((gLibraryClass = objectClass("HakiosMetalLibrary", "_MTLLibrary", library, (IMP)libraryDealloc)) != Nil) {
		class_addMethod(gLibraryClass, sel_registerName("functionNames"), (IMP)libraryFunctionNames, "@@:");
		class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:"), (IMP)libraryNewFunction, "@@:@");
		class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:constantValues:error:"), (IMP)libraryNewFunctionConstants,
		    "@@:@@^@");
		class_addMethod(gLibraryClass, sel_registerName("type"), (IMP)libraryType, "Q@:");
		objc_registerClassPair(gLibraryClass);
		class_addMethod(device, sel_registerName("newLibraryWithURL:error:"), (IMP)deviceNewLibraryURL, "@@:@^@");
		class_addMethod(device, sel_registerName("newLibraryWithFile:error:"), (IMP)deviceNewLibraryFile, "@@:@^@");
		class_addMethod(device, sel_registerName("newLibraryWithData:error:"), (IMP)deviceNewLibraryData, "@@:@^@");
		class_addMethod(device, sel_registerName("newDefaultLibrary"), (IMP)deviceNewDefaultLibrary, "@@:");
		class_addMethod(device, sel_registerName("newDefaultLibraryWithBundle:error:"), (IMP)deviceNewDefaultLibraryBundle, "@@:@^@");
	}
	class_addMethod(device, sel_registerName("hasUnifiedMemory"), (IMP)deviceUnifiedMemory, "B@:");
	class_addMethod(device, sel_registerName("dedicatedMemorySize"), (IMP)deviceNoDedicatedMemory, "Q@:");
	class_addMethod(device, sel_registerName("sharedMemorySize"), (IMP)deviceSharedMemory, "Q@:");
	class_addMethod(device, sel_registerName("isLowPower"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("isRemovable"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("isHeadless"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("supportsFeatureSet:"), (IMP)deviceNoFeatureSet, "B@:Q");
}

/* Declares `protocol` and every private protocol whose name begins with it on `cls`, and keeps them for the stub's signatures. */
static void
declareProtocolFamily(Class cls, const char * protocolName)
{
	unsigned int count = 0;
	Protocol * __unsafe_unretained * all = objc_copyProtocolList(&count);

	for (unsigned int i = 0; i < count; i++) {
		if (strncmp(protocol_getName(all[i]), protocolName, strlen(protocolName)) == 0) {
			class_addProtocol(cls, all[i]);
			if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
				gProtocols[gProtocolCount++] = all[i];
			}
		}
	}
	free(all);
}

static void
registerStubClasses(Class device)
{
	for (unsigned i = 0; i < STUB_COUNT; i++) {
		Class base = objc_getClass(gStubSpecs[i].base);
		Class cls = objc_allocateClassPair(base != Nil ? base : objc_getClass("NSObject"), gStubSpecs[i].cls, 0);

		if (cls == Nil) {
			say([NSString stringWithFormat:@"could not make the class %s", gStubSpecs[i].cls]);
			continue;
		}
		declareProtocolFamily(cls, gStubSpecs[i].protocol);
		class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
		class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
		objc_registerClassPair(cls);
		gStubClass[i] = cls;
		class_addMethod(device, sel_registerName(gStubSpecs[i].selector), (IMP)stubFactory, "@@:@");
		say([NSString stringWithFormat:@"%s is a %s (a subclass of %s)", gStubSpecs[i].selector, gStubSpecs[i].cls,
		    base != Nil ? gStubSpecs[i].base : "NSObject: this system has no such class"]);
	}
}

#ifdef HAKIOS_METAL_GPU
/* ---- command buffers and blits, on the GPU (hakios-metal) ---- */

static void *
slotState(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static void
commandBufferFree(void *state)
{
	struct commandbuffer *c = state;

	hakios_metal_cb_free(c->cb);
	[c->handlers release];
	[c->scheduled release];
	[c->queue release];
	[c->label release];
	free(c);
}

static void
commandBufferDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gCommandBufferClass, commandBufferFree);
}

static id
queueCommandBuffer(id self, __unused SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	hakios_metal_queue *q = slotState(self);
	struct commandbuffer *c;
	void **slot;
	id cb;

	if (q == NULL || gCommandBufferClass == Nil) {
		say(@"commandBuffer: the queue has no GPU queue: nil");
		return nil;
	}
	if ((cb = plain(plain((id)gCommandBufferClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		return nil;
	}
	c = calloc(1, sizeof(*c));
	slot = stateOf(cb);
	if (c == NULL || slot == NULL || (c->cb = hakios_metal_cb_new(q)) == NULL) {
		free(c);
		[cb release];
		return nil;
	}
	c->queue = [self retain];
	c->handlers = [[NSMutableArray alloc] init];
	c->scheduled = [[NSMutableArray alloc] init];
	*slot = c;
	return [cb autorelease];
}

/* on the queue's thread, once the GPU is done with it (or it ended in error): its handlers, then the reference commit took */
static void
commandBufferDone(__unused hakios_metal_cb *cb, void *context)
{
	id self = (id)context;
	struct commandbuffer *c = slotState(self);

	@autoreleasepool {
		for (void (^handler)(id) in c->handlers) {
			handler(self);
		}
	}
	[self release];
}

static void
commandBufferCommit(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	if (c == NULL || c->committed) {
		return;
	}
	c->committed = YES;
	for (void (^handler)(id) in c->scheduled) {
		handler(self);
	}
	hakios_metal_cb_commit(c->cb, commandBufferDone, [self retain]);
}

static void
commandBufferEnqueue(__unused id self, __unused SEL _cmd)
{
}

static void
commandBufferWait(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	if (c != NULL && c->committed) {
		hakios_metal_cb_wait(c->cb);    /* (its completed handlers have run when this returns) */
	}
}

static void
commandBufferWaitScheduled(__unused id self, __unused SEL _cmd)
{
}

static void
commandBufferAddHandler(id self, __unused SEL _cmd, void (^handler)(id))
{
	struct commandbuffer *c = slotState(self);

	if (c != NULL && !c->committed && handler != nil) {
		[c->handlers addObject:[[handler copy] autorelease]];
	}
}

static void
commandBufferAddScheduled(id self, __unused SEL _cmd, void (^handler)(id))
{
	struct commandbuffer *c = slotState(self);

	if (c != NULL && !c->committed && handler != nil) {
		[c->scheduled addObject:[[handler copy] autorelease]];
	}
}

static unsigned long
commandBufferStatus(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	return c == NULL || !c->committed ? MTLCommandBufferStatusNotEnqueued : (unsigned long)hakios_metal_cb_status(c->cb);
}

static id
commandBufferError(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	if (c == NULL || !c->committed || hakios_metal_cb_status(c->cb) != HAKIOS_METAL_ERROR) {
		return nil;
	}
	return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
	    userInfo:@{ NSLocalizedDescriptionKey: [NSString stringWithUTF8String:hakios_metal_cb_error(c->cb)] }];
}

static id
commandBufferQueue(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	return c != NULL ? c->queue : nil;
}

static double
commandBufferNoTime(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static void
blitEncoderFree(void *state)
{
	struct blitencoder *e = state;

	[e->commandBuffer release];
	[e->label release];
	free(e);
}

static void
blitEncoderDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gBlitEncoderClass, blitEncoderFree);
}

static id
commandBufferBlitEncoder(id self, __unused SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct commandbuffer *c = slotState(self);
	struct blitencoder *e;
	void **slot;
	id encoder;

	if (c == NULL || c->committed || gBlitEncoderClass == Nil ||
	    (encoder = plain(plain((id)gBlitEncoderClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		return nil;
	}
	e = calloc(1, sizeof(*e));
	slot = stateOf(encoder);
	if (e == NULL || slot == NULL) {
		free(e);
		[encoder release];
		return nil;
	}
	e->commandBuffer = [self retain];
	*slot = e;
	return [encoder autorelease];
}

/* the command buffer an encoder records into, while it may */
static struct commandbuffer *
recording(id encoder)
{
	struct blitencoder *e = slotState(encoder);
	struct commandbuffer *c = e != NULL && !e->ended ? slotState(e->commandBuffer) : NULL;

	return c != NULL && !c->committed ? c : NULL;
}

/* a buffer of the GPU's, or NULL (made from the caller's memory, or not ours at all) */
static hakios_metal_buffer *
gpuBufferOf(id buffer)
{
	struct buffer *b = object_getClass(buffer) == gBufferClass ? bufferOf(buffer) : NULL;

	return b != NULL ? b->gpu : NULL;
}

static void
blitCopy(id self, __unused SEL _cmd, id from, unsigned long fromOffset, id to, unsigned long toOffset, unsigned long size)
{
	struct commandbuffer *c = recording(self);
	hakios_metal_buffer *a = gpuBufferOf(from), *b = gpuBufferOf(to);

	if (c == NULL) {
		return;
	}
	if (a == NULL || b == NULL) {
		hakios_metal_cb_fail(c->cb, "copyFromBuffer: a buffer that is not in the GPU's memory (made with ...BytesNoCopy?)");
	} else if (hakios_metal_cb_copy(c->cb, a, fromOffset, b, toOffset, size) != 0) {
		say([NSString stringWithFormat:@"copyFromBuffer: %s", hakios_metal_error()]);
	}
}

static void
blitFill(id self, __unused SEL _cmd, id buffer, NSRange range, unsigned char value)
{
	struct commandbuffer *c = recording(self);
	hakios_metal_buffer *b = gpuBufferOf(buffer);

	if (c == NULL) {
		return;
	}
	if (b == NULL) {
		hakios_metal_cb_fail(c->cb, "fillBuffer: a buffer that is not in the GPU's memory");
	} else if (hakios_metal_cb_fill(c->cb, b, range.location, range.length, value) != 0) {
		say([NSString stringWithFormat:@"fillBuffer: %s", hakios_metal_error()]);
	}
}

static void
blitEnd(id self, __unused SEL _cmd)
{
	struct blitencoder *e = slotState(self);

	if (e != NULL) {
		e->ended = YES;
	}
}

static void
blitNothing(__unused id self, __unused SEL _cmd, __unused id resource)
{
	/* synchronizeResource: the memory is the CPU's and the GPU's both (write-combined): there is nothing to copy back */
}

static void
registerCommandClasses(void)
{
	static const char *const cb[] = { "MTLCommandBuffer", NULL }, *const blit[] = { "MTLBlitCommandEncoder", "MTLCommandEncoder", NULL };

	if ((gCommandBufferClass = objectClass("HakiosMetalCommandBuffer", "_MTLCommandBuffer", cb, (IMP)commandBufferDealloc)) != Nil) {
		class_addMethod(gCommandBufferClass, sel_registerName("commit"), (IMP)commandBufferCommit, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("enqueue"), (IMP)commandBufferEnqueue, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("waitUntilCompleted"), (IMP)commandBufferWait, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("waitUntilScheduled"), (IMP)commandBufferWaitScheduled, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("addCompletedHandler:"), (IMP)commandBufferAddHandler, "v@:@?");
		class_addMethod(gCommandBufferClass, sel_registerName("addScheduledHandler:"), (IMP)commandBufferAddScheduled, "v@:@?");
		class_addMethod(gCommandBufferClass, sel_registerName("status"), (IMP)commandBufferStatus, "Q@:");
		class_addMethod(gCommandBufferClass, sel_registerName("error"), (IMP)commandBufferError, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("commandQueue"), (IMP)commandBufferQueue, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("blitCommandEncoder"), (IMP)commandBufferBlitEncoder, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("GPUStartTime"), (IMP)commandBufferNoTime, "d@:");
		class_addMethod(gCommandBufferClass, sel_registerName("GPUEndTime"), (IMP)commandBufferNoTime, "d@:");
		objc_registerClassPair(gCommandBufferClass);
	}
	if ((gBlitEncoderClass = objectClass("HakiosMetalBlitEncoder", "_MTLCommandEncoder", blit, (IMP)blitEncoderDealloc)) != Nil) {
		class_addMethod(gBlitEncoderClass, sel_registerName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"),
		    (IMP)blitCopy, "v@:@Q@QQ");
		class_addMethod(gBlitEncoderClass, sel_registerName("fillBuffer:range:value:"), (IMP)blitFill, "v@:@{_NSRange=QQ}C");
		class_addMethod(gBlitEncoderClass, sel_registerName("endEncoding"), (IMP)blitEnd, "v@:");
		class_addMethod(gBlitEncoderClass, sel_registerName("synchronizeResource:"), (IMP)blitNothing, "v@:@");
		objc_registerClassPair(gBlitEncoderClass);
	}
}
#endif

static void
registerQueueClass(void)
{
	Class base = objc_getClass("_MTLCommandQueue");
	Protocol * protocol = @protocol(MTLCommandQueue);

	if (base == Nil) {
		say(@"this system has no _MTLCommandQueue: no queue class is registered");
		return;
	}
	gQueueClass = objc_allocateClassPair(base, "HakiosMetalQueue", 0);
	if (gQueueClass == Nil) {
		say(@"could not make the class HakiosMetalQueue");
		return;
	}
	class_addProtocol(gQueueClass, protocol);
	if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
		gProtocols[gProtocolCount++] = protocol;
	}
	/*
	 * The third run: -setCompletionQueue: is not in MTLCommandQueue but in a private protocol of Metal's own (as MTLDeviceSPI is
	 * for the device), and the window server sends it. Every protocol of this process whose name begins with MTLCommandQueue is
	 * taken, so that the stub answers what any of them declares; their names go to the log, which is how the next one is found.
	 */
	unsigned int count = 0;
	Protocol * __unsafe_unretained * all = objc_copyProtocolList(&count);

	for (unsigned int i = 0; i < count; i++) {
		const char * name = protocol_getName(all[i]);

		if (strncmp(name, "MTLCommandQueue", 15) == 0 && all[i] != protocol && gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
			class_addProtocol(gQueueClass, all[i]);
			gProtocols[gProtocolCount++] = all[i];
			say([NSString stringWithFormat:@"the queue also declares the protocol %s", name]);
		}
	}
	free(all);
	class_addMethod(gQueueClass, sel_registerName("device"), (IMP)queueDevice, "@@:");
	class_addMethod(gQueueClass, sel_registerName("label"), (IMP)queueLabel, "@@:");
	class_addMethod(gQueueClass, sel_registerName("setGPUPriority:"), (IMP)queueSetGPUPriority, "v@:Q");
	class_addMethod(gQueueClass, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
	class_addMethod(gQueueClass, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
	class_addMethod(gQueueClass, sel_registerName("dealloc"), (IMP)queueDealloc, "v@:");
#ifdef HAKIOS_METAL_GPU
	class_addIvar(gQueueClass, "_hakios", sizeof(void *), sizeof(void *) == 8 ? 3 : 2, "^v");
	class_addMethod(gQueueClass, sel_registerName("commandBuffer"), (IMP)queueCommandBuffer, "@@:");
	class_addMethod(gQueueClass, sel_registerName("commandBufferWithUnretainedReferences"), (IMP)queueCommandBuffer, "@@:");
#endif
	objc_registerClassPair(gQueueClass);
	say(@"the class HakiosMetalQueue is registered (a subclass of _MTLCommandQueue)");
#ifdef HAKIOS_METAL_GPU
	registerCommandClasses();
#endif
}

/*
 * Whether this process may be shown the device, by the level of boot-arg metal (read from sysctl kern.bootargs):
 *
 *   metal=1  only the program metal_test (userland/src/metal_test.m): the safe level, for seeing what Metal does with the device
 *   metal=2  every process but backboardd
 *   metal=3  every process, backboardd too, to find out what the window server asks
 *
 * Why not simply everyone. CoreAnimation's window server tries Metal FIRST and goes to its OpenGL ES path (which works on this port)
 * only when no Metal device exists; with ours it takes Metal and stops on "metallib_url != nil": its own shader library,
 * QuartzCore.framework/default.metallib, is not in this system image (the framework has only a default.pipelinelib that names
 * it), so the window server cannot run on Metal here at all. And mediaserverd, given a device that cannot do anything, ends and
 * is started again. A device that is shown to a process that cannot use it costs that process, so it is shown to as few as the
 * experiment needs.
 *
 * Besides the level, a file HakiosMetal.clients beside the bundle (the way UniXNU's AdrenoMetal chooses its clients) names more
 * processes, one per line, "*" for every one: an image can show the device to the one program being worked on without a boot-arg.
 */
static BOOL
listedBesideTheBundle(const char *name)
{
	Dl_info info;
	char path[1024], line[256];
	char *slash;
	FILE *file;
	BOOL listed = NO;

	if (dladdr((const void *)&listedBesideTheBundle, &info) == 0 || info.dli_fname == NULL) {
		return NO;
	}
	strlcpy(path, info.dli_fname, sizeof(path));            /* <Extensions>/HakiosMetal.bundle/HakiosMetal */
	if ((slash = strrchr(path, '/')) == NULL) {
		return NO;
	}
	*slash = '\0';
	if ((slash = strrchr(path, '/')) == NULL) {
		return NO;
	}
	strlcpy(slash + 1, "HakiosMetal.clients", sizeof(path) - (size_t)(slash + 1 - path));
	if ((file = fopen(path, "r")) == NULL) {
		return NO;
	}
	while (!listed && fgets(line, sizeof(line), file) != NULL) {
		line[strcspn(line, " \t\r\n#")] = '\0';
		listed = strcmp(line, "*") == 0 || (line[0] != '\0' && strcmp(line, name) == 0);
	}
	fclose(file);
	return listed;
}

static BOOL
shownToThisProcess(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char * name = getprogname();
	int level = 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		const char * at = strstr(args, "metal=");

		if (at != NULL) {
			level = atoi(at + 6);
		}
	}
	if (name == NULL) {
		return NO;
	}
	if (level >= 3 || (level == 2 && strcmp(name, "backboardd") != 0) || (level == 1 && strcmp(name, "metal_test") == 0)) {
		return YES;
	}
	if (listedBesideTheBundle(name)) {
		say([NSString stringWithFormat:@"%s is shown the device: HakiosMetal.clients beside the bundle names it", name]);
		return YES;
	}
	say([NSString stringWithFormat:@"%s is not shown the device (metal=%d; 1 is metal_test only, 2 all but backboardd, 3 all)", name, level]);
	return NO;
}

__attribute__((constructor)) static void
registerDevice(void)
{
	Class device;

	if (!shownToThisProcess()) {
		return;
	}

	gBase = objc_getClass("_MTLDevice");
	if (gBase == Nil) {
		say(@"this system has no _MTLDevice: no device class is registered");
		return;
	}
	if (objc_getClass("HakiosMetalDevice") != Nil) {
		return;
	}
	device = objc_allocateClassPair(gBase, "HakiosMetalDevice", 0);
	if (device == Nil) {
		say(@"could not make the class HakiosMetalDevice");
		return;
	}
	/*
	 * The first run said it: MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)] and aborts the process
	 * (backboardd) when the class does not declare it. _MTLDevice itself does not, so the class does.
	 */
	class_addProtocol(device, @protocol(MTLDevice));
	gProtocols[gProtocolCount++] = @protocol(MTLDevice);
	/*
	 * ... and the second run said that MTLAddDevice asserts the private protocol MTLDeviceSPI as well (line 835). Private
	 * protocols are not in the SDK, so they are taken by name from the system's own Metal; each next assertion of this kind
	 * that a run shows is a name for this list.
	 */
	static const char * const privateProtocols[] = { "MTLDeviceSPI", NULL };
	for (const char * const * name = privateProtocols; *name != NULL; name++) {
		Protocol * protocol = objc_getProtocol(*name);

		if (protocol != NULL && gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
			class_addProtocol(device, protocol);
			gProtocols[gProtocolCount++] = protocol;
		} else {
			say([NSString stringWithFormat:@"the protocol %s is not in this system's Metal", *name]);
		}
	}
	class_addMethod(device, @selector(initWithAcceleratorPort:), (IMP)deviceInit, "@@:I");
	class_addMethod(device, @selector(name), (IMP)deviceName, "@@:");
	class_addMethod(device, @selector(registryID), (IMP)deviceRegistryID, "Q@:");
	class_addMethod(device, sel_registerName("maxBufferLength"), (IMP)deviceMaxBufferLength, "Q@:");
	class_addMethod(device, sel_registerName("maxThreadsPerThreadgroup"), (IMP)deviceMaxThreadsPerThreadgroup, "{?=QQQ}@:");
	class_addMethod(device, sel_registerName("newCommandQueue"), (IMP)deviceNewQueue, "@@:");
	class_addMethod(device, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"), (IMP)deviceNewQueueMax, "@@:Q");
	class_addMethod(device, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
	class_addMethod(device, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
	objc_registerClassPair(device);
	say(@"the class HakiosMetalDevice is registered (a subclass of _MTLDevice)");
	registerQueueClass();
	registerStubClasses(device);
	registerObjectClasses(device);
}
