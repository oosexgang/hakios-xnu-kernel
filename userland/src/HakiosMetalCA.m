/*
 * (hakios) This is vcvkk's plug-in (github.com/vcvkk/darwin-redwood, branch hakios-workspace, userland/src/HakiosMetal.m
 * at eb9fdf5), the one that serves CoreAnimation's window server, with what this tree adds for it: CoreAnimation's
 * shaders chosen by pipeline (hm_bitcode_for()), QuartzCore's pipeline library given a default.metallib, stand-ins for
 * functions the library lacks (hm_stand_ins), and the device shown to the window server alone (metal_all=1: all).
 * Built instead of hakios-metal's plug-in with METAL_PLUGIN=ca (userland/build.sh); the library it needs comes from
 * tools/ca-metallib.py.
 */
/*
 * HakiosMetal: the Metal plug-in of hakios, over the C interface of hakios-metal.
 *
 * Metal.framework finds a GPU by reading MetalPluginName and MetalPluginClassName off the IOAcceleratorES service
 * (docs: the kernel's IOAcceleratorES::setProperties, behind boot-arg metal=1), opens
 * /System/Library/Extensions/<name>.bundle with NSBundle, takes the class the second property names and sends it
 * -initWithAcceleratorPort:. That object is the device; everything Metal does afterwards it does by sending that object
 * and the objects it hands out the selectors of its protocols. This file is those objects. The GPU work itself -- memory,
 * the compiler, command buffers, the ring -- is hakios-metal's (include/hakios-metal/hakios_metal.h), linked in as a
 * static library; nothing here touches a register or a packet.
 *
 * What is written here is written against MEASUREMENTS, taken with an earlier Gallium-based plug-in on the same
 * phone (iOS 16.5, Metal 306.6.4). Each one is a thing that aborted a process before it was known:
 *
 *   - the device class subclasses Metal's private _MTLDevice DIRECTLY. The symbol _OBJC_CLASS_$__MTLDevice links
 *     because IOGPU.framework imports it; IOGPUMetalDevice, the class Apple's own driver subclasses, cannot be used
 *     because its init opens the accelerator with IOServiceOpen type 1, which this kernel refuses.
 *   - the contract MTLAddDevice puts on that object is FOUR methods and one protocol: -initWithAcceleratorPort: not
 *     nil, -featureProfile (0 is the lowest value whose limits table fills; 2, 3 and 10-12 abort inside Apple's
 *     initLimits), -maxBufferLength non-zero, -supportsVertexAmplificationCount:, and conformance to <MTLDeviceSPI>,
 *     which is private, requires 337 methods and is declared here at runtime with class_addProtocol. Both
 *     conformsToProtocol: checks in MTLAddDevice are asserts: a class without the protocol does not get a nil, it
 *     gets abort(), and a daemon that aborts is restarted by launchd (88 crashes in 100 s, measured).
 *   - Apple's own ivars are never laid out here: the few that have to be touched are found BY NAME with
 *     class_getInstanceVariable, in the Metal this code is actually running against.
 *   - -[_MTLCommandQueue initWithDevice:descriptor:] and -[_MTLComputePipelineState
 *     initWithDevice:pipelineStateDescriptor:] assert a non-nil descriptor; the first version of each in the earlier
 *     plug-in passed nil and was killed by it.
 *   - -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] waits on the queue's 64-slot
 *     semaphore, and nothing in Metal signals it for a driver that does its own completion: the 65th -commandBuffer
 *     blocked for four minutes. The slot is given back here when the command buffer completes.
 *   - -[_MTLCommandEncoder initWithCommandBuffer:] retains the command buffer and -endEncoding is the only thing that
 *     releases it; -[_MTLCommandEncoder dealloc] aborts with "Command encoder released without endEncoding" (43 times
 *     in one boot). Every -endEncoding here calls super.
 *   - -commitWithDeadline: is an abstract method of _MTLCommandBuffer (its body is doesNotRecognizeSelector:), so it
 *     has to be overridden; so is -setThreadgroupPackingDisabled: of _MTLCommandEncoder.
 *   - -[_MTLDevice compiler] is a nil-stub and both pipeline entry points route through it, so a driver that leaves
 *     them alone gets nil and no reason. Metal parses .metallib containers itself (newLibraryWithData: asks the driver
 *     nothing) and the function's AIR is one call away: -[_MTLFunction bitcodeData] (<MTLFunctionSPI>) returns the
 *     function's module in LLVM's 0x0B17C0DE wrapper, the same bytes hakios-metal's own reader finds (checked by
 *     checksum on every function of a system library). So the pipeline-state entry points are overridden, as Apple's
 *     own AGX driver overrides them, and the bitcode goes to hakios_metal_kernel_compile_bitcode.
 *   - -targetDeviceInfo is a nil-stub of _MTLDevice that respondsToSelector: says yes to, and its one caller
 *     (QuartzCore asking for its precompiled pipeline library) dereferences the answer: 44 crashes in one boot. It is
 *     answered with a real struct, as Apple's own branch for a device that does not respond would fill one.
 *   - rendering (read off the shared cache of this image, 20F66, with the earlier work's tools): the render pipeline
 *     state's base class is _MTLRenderPipelineState, whose designated initialiser -initWithDevice:pipelineStateDescriptor:
 *     (0x181a294dc) opens the validation context "Render Pipeline Descriptor Validation", asserts "device must not be
 *     nil." / "device is not a MTLDevice." / "desc must not be nil." / "desc is not a MTLRenderPipelineDescriptor." and
 *     one rule about tessellationFactorScaleEnabled with indirect command buffers, then reads label (copied),
 *     supportIndirectCommandBuffers, textureWriteRoundingMode, meshFunction and, with one, the two mesh threadgroup
 *     limits -- and NOT the vertex or fragment function, the colour attachments, the vertex descriptor or the sample
 *     counts. Every one of those is this plug-in's to read, so the pipeline is validated here before super sees the
 *     descriptor (the context's end is the abort of the sampler lesson). There is no _MTLRenderCommandEncoder: Apple's
 *     render encoder descends from _MTLCommandEncoder through IOGPUMetalCommandEncoder, so the plug-in's descends from
 *     _MTLCommandEncoder directly, as the compute one does; -[_MTLCommandBuffer renderCommandEncoderWithDescriptor:]
 *     does not exist (the selector is the public protocol's and the driver's command buffer answers it, as
 *     AGXA11FamilyCommandBuffer does), and -[IOGPUMetalRenderCommandEncoder initWithCommandBuffer:descriptor:] drops
 *     the pass descriptor: the attachments are read here, in full, and nothing of Metal's reads them again.
 *
 * And one rule that is a measurement too: EVERYTHING THIS FILE DOES NOT IMPLEMENT REFUSES OUT LOUD. Of the 563
 * methods the earlier plug-in grew, 181 answered nil, zero or nothing without a line anywhere, and a nil without a
 * line was the failure mode that cost the most boots: a blit encoder answering nil silently hid that the window
 * server asked for a texture copy in every boot anybody had read. So every selector a Metal protocol declares and this
 * file does not answer goes through -forwardInvocation: (hm_forward below), which prints the selector, the class and
 * the caller once (then on powers of two, with the count) and returns zeros, which is what Metal's own nil-stubs
 * return; a selector no protocol declares prints the same line and then raises, which is what Metal's own abstract
 * methods do. Nil-stubs INHERITED from Metal's base classes stay silent -- the runtime cannot tell them from a real
 * method, and only reading the cache can (the private tree's tools/metal/absmeth.py did) -- which is why the ones
 * known to matter are overridden by name here.
 *
 * Every line carries the process it was written by: a driver is a library, several Metal clients may be alive in one
 * boot, and a refusal nobody can attribute is half a refusal. Manual reference counting throughout (no ARC): the file
 * sends objc_msgSend by hand and installs protocols at runtime, and the private base classes are declared by name
 * only, nothing of their layout.
 *
 * SPDX-License-Identifier: MIT
 */

#import <Foundation/Foundation.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hakios-metal/hakios_metal.h"

/* --- printing ----------------------------------------------------------------------------------------------- */

static void hm_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* To stderr, as one write: a launchd job's stderr is the console (userland/mkjobs.py sets StandardErrorPath) and its
 * stdout is nowhere, so the window server's lines -- the client this plug-in is for -- reached no log while they went
 * to stdout; one write keeps two threads' lines from interleaving. Flushed every time for the same reason as before:
 * the processes that reach a refusal are the ones that then abort, and a line not written dies with them. */
static void
hm_print(const char *fmt, ...)
{
	static char who[96];
	char line[1024];
	va_list ap;
	int n;

	if (who[0] == '\0') {
		snprintf(who, sizeof(who), "HakiosMetal[%s %d]", getprogname(), getpid());
	}
	n = snprintf(line, sizeof(line), "%s: ", who);
	va_start(ap, fmt);
	if (vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap) >= (int)(sizeof(line) - (size_t)n)) {
		line[sizeof(line) - 2] = '\n';   /* (cut: the line still ends where a line ends) */
	}
	va_end(ap);
	/* An application's stderr is /dev/null (a launchd job's is the log); there the line goes to the console instead, as
	 * libhakios.sb.dylib's does in the programs it is given to, so that what this plug-in says inside Maps is read too */
	{
		static int fd = -1;

		if (fd < 0) {
			struct stat err, null;

			fd = 2;
			if (fstat(2, &err) == 0 && stat("/dev/null", &null) == 0 && S_ISCHR(err.st_mode) && err.st_rdev == null.st_rdev) {
				int console = open("/dev/console", O_WRONLY | O_NOCTTY);

				if (console >= 0) fd = console;
			}
		}
		(void)!write(fd, line, strlen(line));
	}
}

/* a sentence that is true every time and says nothing new after the first */
#define HM_ONCE(...)							\
	do {								\
		static BOOL hm_said_;					\
									\
		if (!hm_said_) {					\
			hm_said_ = YES;					\
			hm_print(__VA_ARGS__);				\
		}							\
	} while (0)

/*
 * Who sent the selector. backtrace(3) gives the frames; the ones inside this image, libobjc and CoreFoundation (the
 * forwarding machinery itself) are skipped, and the first frame beyond them is named by image and address, the
 * address also unslid (the shared cache's load address taken off), because every measurement of Metal on this port
 * is written in the cache's own addresses.
 */
static void
hm_caller(char *out, size_t size)
{
	void *frames[16];
	int n = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));

	for (int i = 1; i < n; i++) {
		Dl_info info;
		const char *image;
		intptr_t slide = 0;

		if (dladdr(frames[i], &info) == 0 || info.dli_fname == NULL) {
			continue;
		}
		image = strrchr(info.dli_fname, '/');
		image = image != NULL ? image + 1 : info.dli_fname;
		if (strcmp(image, "HakiosMetal") == 0 || strcmp(image, "libobjc.A.dylib") == 0 ||
		    strcmp(image, "CoreFoundation") == 0 || strcmp(image, "Foundation") == 0) {
			continue;
		}
		for (uint32_t k = 0; k < _dyld_image_count(); k++) {
			if ((const void *)_dyld_get_image_header(k) == info.dli_fbase) {
				slide = _dyld_get_image_vmaddr_slide(k);
				break;
			}
		}
		snprintf(out, size, "%s%s%s at %#lx (unslid %#lx)", image, info.dli_sname != NULL ? " " : "",
			 info.dli_sname != NULL ? info.dli_sname : "", (unsigned long)(uintptr_t)frames[i],
			 (unsigned long)((uintptr_t)frames[i] - (uintptr_t)slide));
		return;
	}
	snprintf(out, size, "(no frame outside the runtime among %d)", n);
}

/*
 * One refusal line per (class, selector), then again on powers of two of that pair's count. Per pair and not per call
 * site, because one -forwardInvocation: is the call site of every refused selector of a class; and not on every call,
 * because the window server asked the earlier plug-in for one thing 862 times in a boot and the repetitions pushed
 * the rest of the log out of the loader's window.
 */
static void
hm_refusal(id self, SEL sel, const char *how)
{
	static struct {
		Class cls;
		SEL sel;
		unsigned long times;
	} seen[64];
	static unsigned nseen;
	static unsigned long beyond;
	Class cls = object_getClass(self);
	unsigned long *times = NULL;
	char caller[256];

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].cls == cls && seen[i].sel == sel) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].cls = cls;
		seen[nseen].sel = sel;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == NULL) {
		times = &beyond;
	}
	if ((++*times & (*times - 1)) != 0) {
		return;
	}
	hm_caller(caller, sizeof(caller));
	hm_print("refused: -[%s %s] %s; asked by %s\n", class_getName(cls), sel_getName(sel), how, caller);
	if (*times > 1) {
		hm_print("  ... and that is the %lu'th time%s\n", *times,
			 times == &beyond ? ", counting every pair past the 64 this keeps apart" : "");
	}
}

/* --- the catch-all for selectors nobody here answers -------------------------------------------------------- */

/* the type encoding a protocol (or one it adopts) declares for SEL, or NULL */
static const char *
hm_protocol_types(Protocol *protocol, SEL sel, unsigned depth)
{
	struct objc_method_description d = protocol_getMethodDescription(protocol, sel, YES, YES);
	const char *types = d.types;
	unsigned n = 0;
	Protocol *__unsafe_unretained *adopted;

	if (types == NULL) {
		d = protocol_getMethodDescription(protocol, sel, NO, YES);
		types = d.types;
	}
	if (types != NULL || depth > 8) {
		return types;
	}
	adopted = protocol_copyProtocolList(protocol, &n);
	for (unsigned i = 0; i < n && types == NULL; i++) {
		types = hm_protocol_types(adopted[i], sel, depth + 1);
	}
	free(adopted);
	return types;
}

/*
 * The signature of a selector this class does not implement but one of its protocols declares. That is what makes a
 * refusal SAFE to return from: a method returning a struct is handed a hidden pointer to the caller's buffer, one
 * returning a float is read out of d0, and only the declared encoding says which -- a guessed signature would leave
 * the caller reading what happened to be there. <MTLDeviceSPI>'s 337 encodings are in Metal.framework's metadata, so
 * they are found here; a selector of a protocol that is not in this process at all (the AGX bundle's SPIs) has no
 * signature and goes to -doesNotRecognizeSelector: instead, which raises.
 */
static NSMethodSignature *
hm_protocol_signature(Class cls, SEL sel)
{
	for (Class c = cls; c != Nil; c = class_getSuperclass(c)) {
		unsigned n = 0;
		Protocol *__unsafe_unretained *protocols = class_copyProtocolList(c, &n);
		const char *types = NULL;

		for (unsigned i = 0; i < n && types == NULL; i++) {
			types = hm_protocol_types(protocols[i], sel, 0);
		}
		free(protocols);
		if (types != NULL) {
			return [NSMethodSignature signatureWithObjCTypes:types];
		}
	}
	return nil;
}

/* the refusal itself: the line, then zeros of the declared return width, which is what Metal's own nil-stubs return */
static void
hm_forward(id self, NSInvocation *invocation)
{
	NSMethodSignature *signature = [invocation methodSignature];
	NSUInteger length = [signature methodReturnLength];

	hm_refusal(self, [invocation selector], "is not implemented in this plug-in; it returns zeros");
	if (length > 0) {
		void *zero = calloc(1, length);

		if (zero != NULL) {
			[invocation setReturnValue:zero];
			free(zero);
		}
	}
}

/* The three methods every class of this file carries. -methodSignatureForSelector: asks super first so that a method
 * that exists is never forwarded; -doesNotRecognizeSelector: prints before super raises, so that the exception's
 * sentence -- which names the RECEIVER and reads exactly like a missing method -- has a line beside it that names the
 * sender. */
#define HM_REFUSES_OUT_LOUD								\
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel			\
{										\
	NSMethodSignature *s = [super methodSignatureForSelector:sel];		\
										\
	return s != nil ? s : hm_protocol_signature([self class], sel);		\
}										\
- (void)forwardInvocation:(NSInvocation *)invocation				\
{										\
	hm_forward(self, invocation);						\
}										\
- (void)doesNotRecognizeSelector:(SEL)sel					\
{										\
	hm_refusal(self, sel, "is declared by no protocol in this process and "	\
		   "implemented by nothing in its class chain; it raises");	\
	[super doesNotRecognizeSelector:sel];					\
}

/* a protocol declared on a class at runtime: the compiler put the protocol object into this image by the @protocol()
 * reference, so it exists in the process whether or not Metal's own metadata declares it (the public MTLCommandQueue,
 * for one, is declared only in the AGX bundle on this phone, which never loads here) */
static void
hm_adopt(Class cls, Protocol *protocol, const char *name)
{
	if (protocol == NULL) {
		hm_print("protocol %s is not in this process; %s cannot declare it\n", name, class_getName(cls));
		return;
	}
	if (!class_conformsToProtocol(cls, protocol) && !class_addProtocol(cls, protocol)) {
		hm_print("class_addProtocol(%s, %s) refused\n", class_getName(cls), name);
	}
}

/* an ivar of Apple's, by name, in the Metal this process runs: never an offset written down */
static Ivar
hm_ivar(const char *className, const char *ivarName)
{
	Class cls = objc_getClass(className);

	return cls != Nil ? class_getInstanceVariable(cls, ivarName) : NULL;
}

/* host time as Metal's CFTimeInterval reports it (seconds of mach_absolute_time) */
static double
hm_now(void)
{
	static mach_timebase_info_data_t timebase;

	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	return (double)mach_absolute_time() * timebase.numer / timebase.denom / 1e9;
}

/* --- the private base classes, by name only ---------------------------------------------------------------- */

/* The designated initialisers are the ones Apple's own driver sends to super (IOGPUMetalCommandQueue,
 * IOGPUMetalCommandEncoder, AGXA11FamilyComputePipeline), read out of the cache by the earlier work; nothing of any
 * layout is declared. */
@interface _MTLDevice : NSObject
- (instancetype)init;
@end

@interface _MTLObjectWithLabel : NSObject
- (instancetype)init;
- (NSString *)label;   /* the base's own: ivar _label under _labelLock */
- (void)setLabel:(NSString *)label;
@end

@interface _MTLResource : _MTLObjectWithLabel
@end

@interface _MTLCommandQueue : _MTLObjectWithLabel
- (instancetype)initWithDevice:(id)device descriptor:(id)descriptor;
- (NSUInteger)maxCommandBufferCount;
@end

@interface _MTLCommandBuffer : _MTLObjectWithLabel
- (instancetype)initWithQueue:(id)queue retainedReferences:(BOOL)retained synchronousDebugMode:(BOOL)synchronous;
@end

@interface _MTLCommandEncoder : _MTLObjectWithLabel
- (instancetype)initWithCommandBuffer:(id)commandBuffer;
- (void)endEncoding;
@end

@interface _MTLComputePipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

/* The render pipeline state's base (what AGXA11FamilyRenderPipeline sends to super). It answers <MTLRenderPipelineState>
 * itself: -label and -device are ivars its initialiser fills, -gpuResourceID is -gpuHandle is the resourceIndex ivar,
 * and allocatedSize, imageblockSampleLength, imageblockMemoryLengthForDimensions:, maxTotalThreadsPerThreadgroup and
 * uniqueIdentifier are nil-stubs (tools/metal/absmeth.py over the class: 65 selectors, 5 nil-stubs, 21 ivars). The
 * initialiser asserts a non-nil _MTLDevice and a non-nil MTLRenderPipelineDescriptor (see the header) and reads six
 * properties none of which is a function or an attachment, so what is handed to it has been checked here first. */
@interface _MTLRenderPipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

/* The sampler HAS a base class (ivars _device, _label, _pixelFormat, _resourceIndex, _gpuAddress; it answers its whole
 * protocol itself), and its designated initialiser -- what AGXA11FamilySampler sends to super -- VALIDATES: it opens Metal's
 * "New Sampler State Validation" context and records "device must not be nil.", "desc must not be nil.", "desc is not a
 * MTLSamplerDescriptor.", "filter is not a valid MTLSamplerMinMagFilter." (minFilter, magFilter, mipFilter each < 3),
 * "addressMode is not a valid MTLSamplerAddressMode." (s, t, r each < 7), the border colour (< 4) and the compare function;
 * the context's end is _MTLReportFailure, which aborts the process (the frame the earlier plug-in read as
 * _MTLReportFailure.cold.1 above abort). So every one of those is checked HERE before super is given the descriptor. */
@interface _MTLSamplerState : NSObject
- (instancetype)initWithDevice:(id)device samplerDescriptor:(id)descriptor;
@end

/*
 * The one struct Metal reads out of a device by raw pointer. Name and field types are Apple's own, from the type
 * encoding MTLDeviceSPI records for -targetDeviceInfo (r^{MTLTargetDeviceArch=QI*}16@0:8); which field means what was
 * read off its readers: +0x08 pairs with 1 into a pipeline cache version, +0x10 is formatted into
 * "<pipelinelib>/pipelines.%s" and must be a C string, +0x00 is read by nothing on that path.
 */
struct MTLTargetDeviceArch {
	uint64_t target;
	uint32_t version;
	char *name;
};

_Static_assert(sizeof(struct MTLTargetDeviceArch) == 24 && __builtin_offsetof(struct MTLTargetDeviceArch, version) == 8 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, name) == 16, "Metal reads the name at +0x10 and the version at +0x8");

/* Metal's buffer table has 31 entries per stage (profile 0's maxVertexBuffers); the compute encoder's is the same size */
#define HM_MAX_BUFFERS 31
/* The texture and sampler tables: 128 and 16 are what Apple's AIR modules declare for this phone (air.max_textures 128,
 * air.max_samplers 16 in every .metallib of the image) and what profile 0 gives samplers (16). hakios-metal serves the
 * first 16 texture indices (kernel.hpp: kMaxTextureSlots) and refuses a kernel declaring one beyond that by name at
 * dispatch; the encoder keeps Metal's full table so that a binding past 16 is stored, not dropped, and the dispatch says why. */
#define HM_MAX_TEXTURES 128
#define HM_MAX_SAMPLERS 16

/* --- the classes ------------------------------------------------------------------------------------------- */

@class HakiosMetalCommandQueue;
@class HakiosMetalCommandBuffer;

/* _MTLDevice answers this one itself (it sends the source to MTLCompilerService); the SDK's headers do not say so */
@interface _MTLDevice (HakiosMetalSource)
- (id)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error;
@end

/* and this one (MTLPipelineLibraryBuilder::newLibraryWithFile), reading the .pipelinelib directory */
@interface _MTLDevice (HakiosMetalPipelineLibrary)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error;
@end

@interface HakiosMetalDevice : _MTLDevice {
	io_service_t _acceleratorService;        /* the IOAcceleratorES service Metal handed us */
	hakios_metal_device *_gpu;               /* hakios-metal's device: the kernel's HakiosGPU client */
	struct MTLTargetDeviceArch _target;      /* answered by pointer: see -targetDeviceInfo */
	char _targetName[96];                    /* the bytes _target.name points at, inside the object */
	NSString *_name;
}
- (hakios_metal_device *)hakiosDevice;
@end

@interface HakiosMetalBuffer : _MTLResource {
	HakiosMetalDevice *_device;
	hakios_metal_buffer *_buffer;
	/* a no-copy buffer's own pages and the block Metal promises to call when the buffer goes (see
	 * -newBufferWithBytesNoCopy:...) */
	void (^_deallocator)(void *, NSUInteger);
	void *_callerBytes;
	NSUInteger _callerLength;
}
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device length:(NSUInteger)length;
- (void)hakiosSetDeallocator:(void (^)(void *, NSUInteger))deallocator bytes:(void *)bytes length:(NSUInteger)length;
- (hakios_metal_buffer *)hakiosBuffer;
- (void *)contents;
@end

/*
 * A texture: hakios-metal's, one 2D level in GPU memory of its own, laid out as the GPU reads it (linear rows of
 * -bufferBytesPerRow bytes). Metal has no _MTLTexture base class to inherit from -- Apple's chain is AGXTexture :
 * IOGPUMetalTexture : IOGPUMetalResource : _MTLResource, and IOGPU never loads here -- so the class subclasses _MTLResource
 * directly and declares <MTLTexture>, <MTLTextureSPI>, <MTLTextureImplementation> and <MTLResourceSPI> at +load, the road
 * the other classes take. Metal reads no ivar of a texture: every fact about one is asked by selector, which is why every
 * member of those protocols that has a value here answers it below and none is left to the catch-all's zeros -- a width of
 * zero, pixel format 0 (Invalid), a swizzle of four zeros (every channel reads 0) and NO to isComplete would each be
 * wrong without a word.
 */
@interface HakiosMetalTexture : _MTLResource {
	HakiosMetalDevice *_device;
	hakios_metal_texture *_texture;
	MTLTextureUsage _usage;             /* what hakios-metal was given: ShaderRead, ShaderWrite, or both */
	MTLResourceOptions _askedOptions;   /* the descriptor's, for -unfilteredResourceOptions; what was GIVEN is shared memory */
	BOOL _allowGPUOptimizedContents;    /* the descriptor's word; the layout is linear either way */
	uint64_t _uniqueIdentifier;         /* from a counter, never 0 and never the object's address: see hm_next_resource_id */
	IOSurfaceRef _iosurface;            /* the surface whose pixels the texture's rows are (retained), NULL for one of its own */
	NSUInteger _iosurfacePlane;
	BOOL _shadow;                       /* the rows are a copy of _iosurface's, refreshed when its seed moves (see below) */
	uint32_t _shadowSeed;
	uint8_t *_shadowBase;               /* ... and, for a copy something writes, the surface's rows as the copy last took them */
	hakios_metal_texture *_turnedSurface; /* a framebuffer with a rotation: the surface itself, upright, that _texture is turned into */
	unsigned _turn;                     /* ... by this a6xx_rotation (the 2D engine's, probe "2D rotation") */
	HakiosMetalTexture *_parent;        /* a view: _texture is the parent's and is freed with it, not with the view... */
	BOOL _ownsView;                     /* ...unless the view names a level or slices of it: then _texture is the core's
	                                     * own view of the parent's memory (Texture::view), and is the view's to free */
	NSUInteger _parentLevel, _parentSlice;
	BOOL _swizzled;                     /* a swizzled view: _texture is the core's, read through _swizzle */
	MTLTextureSwizzleChannels _swizzle;
	void (^_deallocator)(void *, NSUInteger);   /* a no-copy texture's: its caller's bytes are given back at dealloc */
	int _ycbcr;                         /* a two-plane surface read as RGBA8 converted from it: 0 not, 1 BT.601, 2 BT.709 */
	BOOL _ycbcrFull;                    /* ... '420f' (full range) rather than '420v' (video range) */
	MTLPixelFormat _ycbcrFormat;        /* ... and the private format it was asked as (500, 540), which it answers */
	void *_deallocBytes;
	NSUInteger _deallocLength;
}
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device texture:(hakios_metal_texture *)texture
			  descriptor:(MTLTextureDescriptor *)descriptor usage:(MTLTextureUsage)usage;
- (hakios_metal_texture *)hakiosTexture;
- (void)hakiosSetIOSurface:(IOSurfaceRef)surface plane:(NSUInteger)plane;
- (void)hakiosShadow:(IOSurfaceRef)surface;
- (BOOL)hakiosIsShadow;
- (BOOL)hakiosIsTurned;
- (void)hakiosTurnInto:(hakios_metal_cb *)cb;
- (void)hakiosSetTurned:(hakios_metal_texture *)surface rotation:(unsigned)rotation;
- (void)hakiosWriteBackShadow;
static void hm_fps_shadow(BOOL in, double seconds);   /* metal_fps's count of the copies below */
static void hm_fps_encoded(double seconds, BOOL held);   /* metal_fps: a buffer's making to its commit */
static BOOL hm_timeline_on(void);   /* metal_fps=timeline */
static void hm_fps_waited(double seconds);   /* metal_fps: time in -waitUntilCompleted */
static void hm_fps_drew(double seconds, unsigned cpu);   /* metal_fps: our time in a draw call */
static void hm_fps_core(double seconds);   /* metal_fps: the core's time in a draw call */
static BOOL hm_fps_on(void);                          /* metal_fps: command buffers timed pass by pass */
static BOOL hm_gmem_on(void);                         /* metal_gmem=1: tiled rendering where a pass allows it */
- (void)hakiosShadowYCbCr:(IOSurfaceRef)surface format:(MTLPixelFormat)format;
- (void)hakiosRefreshShadow;
- (void)hakiosSetDeallocator:(void (^)(void *, NSUInteger))deallocator bytes:(void *)bytes length:(NSUInteger)length;
- (unsigned)hakiosStorageKind;   /* metal_fps=timeline: 0 a surface's, else 1 + its storage mode */
@end

/* A sampler state: hakios-metal's four TEX_SAMP words, computed once from the descriptor, under Metal's own base class
 * (which keeps the device, the label and the pixel format and answers <MTLSamplerState> itself). The ivar names are
 * prefixed so that none can shadow the base's own _device/_label/_pixelFormat/_resourceIndex/_gpuAddress. */
/* A depth/stencil state: the descriptor's depth half as hakios-metal takes it (the compare function and the write), read
 * by this plug-in's render encoder alone. A plain object adopting <MTLDepthStencilState>: nothing in Metal's own code
 * looks inside one on this road (the encoder that binds it is ours), and _MTLDepthStencilState's initialiser is not one
 * this plug-in knows the contract of. */
/* A fence: a name for an ordering this device already keeps. Every encoder of a command buffer runs in the order it
 * was encoded and every command buffer of the queue in commit order, each dispatch and pass flushing what it wrote
 * before the next begins, so -updateFence: and -waitForFence: have nothing to add and the encoders take them as
 * no-ops. What Metal requires of the object is a device and a label. */
@interface HakiosMetalFence : NSObject <MTLFence> {
	id<MTLDevice> _device;
	NSString *_label;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device;
@end

@implementation HakiosMetalFence
- (instancetype)initWithDevice:(id<MTLDevice>)device
{
	self = [super init];
	if (self != nil) {
		_device = [device retain];
	}
	return self;
}
- (void)dealloc
{
	[_label release];
	[_device release];
	[super dealloc];
}
- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label
{
	[label retain];
	[_label release];
	_label = label;
}
@end

/* MTLEvent and MTLSharedEvent: a 64-bit value that command buffers signal when they complete and wait for before they
 * run, and that the processor may read, set and listen to (a shared event). The GPU never touches it: this plug-in's
 * queue runs command buffers one after another, so a signal is applied when its buffer completes and a buffer that
 * waits is committed to the GPU once the value has come (HakiosMetalCommandBuffer). Values only rise, as Metal's do. One
 * class for both: an MTLEvent is a shared event no one shares. A handle for another process is not served. */
@interface HakiosMetalEvent : NSObject <MTLSharedEvent> {
	id<MTLDevice> _device;
	NSString *_label;
	uint64_t _value;                     /* under _lock */
	NSCondition *_lock;
	NSMutableArray *_listeners;          /* { value, listener, block }, under _lock */
}
- (instancetype)initWithDevice:(id<MTLDevice>)device;
- (void)hakiosSignal:(uint64_t)value;
- (BOOL)hakiosReached:(uint64_t)value;
- (void)hakiosWaitFor:(uint64_t)value;
@end

@implementation HakiosMetalEvent
- (instancetype)initWithDevice:(id<MTLDevice>)device
{
	self = [super init];
	if (self != nil) {
		_device = [device retain];
		_lock = [[NSCondition alloc] init];
		_listeners = [[NSMutableArray alloc] init];
	}
	return self;
}
- (void)dealloc
{
	[_listeners release];
	[_lock release];
	[_label release];
	[_device release];
	[super dealloc];
}
- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label
{
	[label retain];
	[_label release];
	_label = label;
}
- (uint64_t)signaledValue
{
	uint64_t v;

	[_lock lock];
	v = _value;
	[_lock unlock];
	return v;
}
- (void)setSignaledValue:(uint64_t)value { [self hakiosSignal:value]; }

/* the value set (a completing command buffer's signal, or the processor's), the waiters woken, and every listener whose
 * value it reaches called on its own dispatch queue, once */
- (void)hakiosSignal:(uint64_t)value
{
	NSMutableArray *due = [NSMutableArray array];

	[_lock lock];
	_value = value;
	for (NSArray *l in [[_listeners copy] autorelease]) {
		if ([[l objectAtIndex:0] unsignedLongLongValue] <= value) {
			[due addObject:l];
			[_listeners removeObject:l];
		}
	}
	[_lock broadcast];
	[_lock unlock];
	for (NSArray *l in due) {
		MTLSharedEventListener *listener = [l objectAtIndex:1];
		MTLSharedEventNotificationBlock block = [l objectAtIndex:2];
		dispatch_queue_t q = [listener respondsToSelector:@selector(dispatchQueue)] ? [listener dispatchQueue] : nil;

		[self retain];
		dispatch_async(q != nil ? q : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
			block((id<MTLSharedEvent>)self, value);
			[self release];
		});
	}
}
- (BOOL)hakiosReached:(uint64_t)value
{
	return [self signaledValue] >= value;
}
- (void)hakiosWaitFor:(uint64_t)value
{
	[_lock lock];
	while (_value < value) {
		[_lock wait];
	}
	[_lock unlock];
}
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block
{
	BOOL now;

	if (block == nil) {
		return;
	}
	[_lock lock];
	now = _value >= value;
	if (!now) {
		[_listeners addObject:@[ @(value), listener != nil ? (id)listener : (id)[NSNull null], [[block copy] autorelease] ]];
	}
	[_lock unlock];
	if (now) {
		dispatch_queue_t q = [listener respondsToSelector:@selector(dispatchQueue)] ? [listener dispatchQueue] : nil;
		const uint64_t v = [self signaledValue];

		[self retain];
		dispatch_async(q != nil ? q : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
			block((id<MTLSharedEvent>)self, v);
			[self release];
		});
	}
}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds
{
	NSDate *until = [NSDate dateWithTimeIntervalSinceNow:(double)milliseconds / 1000.0];
	BOOL reached;

	[_lock lock];
	while (_value < value && [_lock waitUntilDate:until]) {
	}
	reached = _value >= value;
	[_lock unlock];
	return reached;
}
- (id)newSharedEventHandle
{
	hm_refusal(self, _cmd, "has no handle another process could open (the value lives in this process alone); it returns nil");
	return nil;
}
@end

/* A texture layout (-newTextureLayoutWithDescriptor:isHeapOrBufferBacked:): how this device wants a texture's bytes in
 * memory a client fills ahead of the texture. ImageIO's ASTCTwiddler asks for one to stage an asset catalogue's ASTC
 * image ("twiddled" into the GPU's order on an Apple GPU), and the bytes reach Metal again through
 * -newTextureWithBytesNoCopy:length:descriptor:deallocator:. Here the order is the plainest: the levels one after
 * another, each in rows of whole blocks (or pixels) packed tight -- and the no-copy texture reads exactly that, so the two
 * agree by construction. -watermark names the layout, so that bytes staged for another one are not taken for these. */
@interface HakiosMetalTextureLayout : NSObject {
	unsigned _blockW, _blockH, _bytes;   /* hakios_metal_format_block's */
	NSUInteger _width, _height, _levels;
}
- (instancetype)initWithDescriptor:(MTLTextureDescriptor *)descriptor blockW:(unsigned)bw blockH:(unsigned)bh bytes:(unsigned)bytes;
@end

/* the tight layout's numbers for one level: its rows of blocks, a row's bytes, and where the level starts */
static void
hm_tight_level(NSUInteger width, NSUInteger height, NSUInteger levels, unsigned bw, unsigned bh, unsigned bytes, NSUInteger level,
	       NSUInteger *offset, NSUInteger *rowBytes, NSUInteger *rows)
{
	NSUInteger at = 0;

	for (NSUInteger l = 0; l < levels; l++) {
		const NSUInteger w = MAX(width >> l, (NSUInteger)1), h = MAX(height >> l, (NSUInteger)1);
		const NSUInteger row = (w + bw - 1) / bw * bytes, down = (h + bh - 1) / bh;

		if (l == level) {
			*offset = at;
			*rowBytes = row;
			*rows = down;
			return;
		}
		at += row * down;
	}
	*offset = at;   /* (LEVEL past the last: where the next would start, the whole size) */
	*rowBytes = 0;
	*rows = 0;
}

@implementation HakiosMetalTextureLayout
- (instancetype)initWithDescriptor:(MTLTextureDescriptor *)descriptor blockW:(unsigned)bw blockH:(unsigned)bh bytes:(unsigned)bytes
{
	self = [super init];
	if (self != nil) {
		_blockW = bw;
		_blockH = bh;
		_bytes = bytes;
		_width = [descriptor width];
		_height = [descriptor height];
		_levels = MAX([descriptor mipmapLevelCount], (NSUInteger)1);
	}
	return self;
}

- (NSUInteger)size
{
	NSUInteger offset, row, rows;

	hm_tight_level(_width, _height, _levels, _blockW, _blockH, _bytes, _levels, &offset, &row, &rows);
	return offset;
}

- (NSUInteger)alignment { return 64; }
- (void)initializeTextureMemory:(void *)memory { (void)memory; }   /* nothing to set up: the bytes are rows, nothing else */
- (void)finalizeTextureMemory:(void *)memory { (void)memory; }

/* "hakios-tight-v1\0": this layout's name for staged bytes to be checked against (16 bytes, ImageIO keeps them) */
struct hm_watermark { uint64_t a, b; };
- (struct hm_watermark)watermark
{
	struct hm_watermark w;

	memcpy(&w, "hakios-tight-v1", 16);
	return w;
}

/* REGION of LEVEL (slice 0, depth 1) between the caller's rows and the tight layout's, in whole blocks either way */
static BOOL
hm_layout_rows(HakiosMetalTextureLayout *l, unsigned bw, unsigned bh, unsigned bytes, NSUInteger width, NSUInteger height, NSUInteger levels,
	       NSUInteger slice, NSUInteger level, MTLRegion r, NSUInteger *at, NSUInteger *rowBytes, NSUInteger *rows, NSUInteger *pitch)
{
	NSUInteger offset, row, down;

	(void)l;
	if (slice != 0 || level >= levels || r.origin.z != 0 || r.size.depth != 1 || r.origin.x % bw != 0 || r.origin.y % bh != 0) {
		hm_print("refused: a texture layout's copy of %lux%lu at (%lu, %lu) of level %lu, slice %lu: one slice, depth 1, whole blocks\n",
			 (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)r.origin.x, (unsigned long)r.origin.y,
			 (unsigned long)level, (unsigned long)slice);
		return NO;
	}
	hm_tight_level(width, height, levels, bw, bh, bytes, level, &offset, &row, &down);
	*at = offset + r.origin.y / bh * row + r.origin.x / bw * bytes;
	*rowBytes = (r.size.width + bw - 1) / bw * bytes;
	*rows = (r.size.height + bh - 1) / bh;
	*pitch = row;
	return YES;
}

- (void)copyFromLinearBytes:(const void *)linear linearOffset:(NSUInteger)linearOffset linearBytesPerRow:(NSUInteger)linearBytesPerRow
	linearBytesPerImage:(NSUInteger)linearBytesPerImage toTextureMemory:(void *)memory textureSlice:(NSUInteger)slice
	       textureLevel:(NSUInteger)level textureRegion:(MTLRegion)region
{
	NSUInteger at, rowBytes, rows, pitch;

	(void)linearBytesPerImage;
	if (!hm_layout_rows(self, _blockW, _blockH, _bytes, _width, _height, _levels, slice, level, region, &at, &rowBytes, &rows, &pitch)) return;
	for (NSUInteger r = 0; r < rows; r++) {
		memcpy((uint8_t *)memory + at + r * pitch, (const uint8_t *)linear + linearOffset + r * linearBytesPerRow, rowBytes);
	}
}

- (void)copyFromTextureMemory:(const void *)memory textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level
		textureRegion:(MTLRegion)region toLinearBytes:(void *)linear linearOffset:(NSUInteger)linearOffset
	    linearBytesPerRow:(NSUInteger)linearBytesPerRow linearBytesPerImage:(NSUInteger)linearBytesPerImage
{
	NSUInteger at, rowBytes, rows, pitch;

	(void)linearBytesPerImage;
	if (!hm_layout_rows(self, _blockW, _blockH, _bytes, _width, _height, _levels, slice, level, region, &at, &rowBytes, &rows, &pitch)) return;
	for (NSUInteger r = 0; r < rows; r++) {
		memcpy((uint8_t *)linear + linearOffset + r * linearBytesPerRow, (const uint8_t *)memory + at + r * pitch, rowBytes);
	}
}
@end

/* A heap (MTLHeapTypeAutomatic): a budget of SIZE bytes that resources are made from. Every buffer and texture here is a
 * kernel allocation of its own, so one made from the heap is the device's own, counted against the budget (and not given
 * back when it goes: the budget only grows, so a heap refuses early rather than late);
 * what a heap adds on an Apple GPU -- resources aliasing one another's memory after -makeAliasable -- is the one part a
 * resource here cannot do (its -makeAliasable says so). A placement heap is refused where it is asked for. */
@interface HakiosMetalHeap : NSObject <MTLHeap> {
	id<MTLDevice> _device;
	NSString *_label;
	MTLHeapDescriptor *_descriptor;
	NSUInteger _used;
}
- (instancetype)initWithDevice:(id)device descriptor:(MTLHeapDescriptor *)descriptor;
@end

/* An argument encoder (MTLArgumentEncoder) over the layout the AIR front end reads back (nir_air.h, air_argbuf): eight
 * bytes a member, an array member eight bytes an element, the members in order of their [[id(n)]] -- a buffer's GPU
 * address plus its offset, a texture's or sampler's gpuResourceID. A draw or dispatch reads those words when it is
 * recorded (hakios-metal's bindArgumentBuffers) and binds what they name. What this layout cannot hold -- inlined
 * constant data, nested argument buffers, indirect command buffers -- is refused by name where it is asked for. */
@interface HakiosMetalArgumentEncoder : NSObject {
	id<MTLDevice> _device;
	NSString *_label;
	NSUInteger _count;
	NSUInteger *_ids, *_offsets, *_lengths;   /* by position, sorted by id: the member's id, byte offset, array length */
	NSUInteger _length;
	id _buffer;
	NSUInteger _base;
	char _source[96];                         /* where the layout came from, for a refusal to say */
}
- (instancetype)initWithDevice:(id)device arguments:(NSArray *)arguments;
- (void)hakiosSetSource:(const char *)source;
- (instancetype)initWithDevice:(id)device ids:(const unsigned *)ids offsets:(const unsigned *)offsets count:(NSUInteger)count;
@end

@implementation HakiosMetalArgumentEncoder
HM_REFUSES_OUT_LOUD

+ (void)load
{
	hm_adopt(self, @protocol(MTLArgumentEncoder), "MTLArgumentEncoder");
}

- (instancetype)initWithDevice:(id)device arguments:(NSArray *)arguments
{
	self = [super init];
	if (self == nil) return nil;
	NSArray *sorted = [arguments sortedArrayUsingComparator:^NSComparisonResult(MTLArgumentDescriptor *a, MTLArgumentDescriptor *b) {
		return [a index] < [b index] ? NSOrderedAscending : [a index] > [b index] ? NSOrderedDescending : NSOrderedSame;
	}];
	_device = [device retain];
	_count = [sorted count];
	_ids = calloc(_count + 1, sizeof(NSUInteger));
	_offsets = calloc(_count + 1, sizeof(NSUInteger));
	_lengths = calloc(_count + 1, sizeof(NSUInteger));
	for (NSUInteger i = 0; i < _count; i++) {
		MTLArgumentDescriptor *d = sorted[i];
		_ids[i] = [d index];
		_offsets[i] = _length;
		_lengths[i] = [d arrayLength] != 0 ? [d arrayLength] : 1;
		_length += 8 * _lengths[i];
	}
	return self;
}

/* a function's set as its metadata lays it out (hakios_metal_argument_buffer_layout): each member at its own offset,
 * one element each -- the front end reads an array member as members of their own -- and the set eight bytes a member */
- (instancetype)initWithDevice:(id)device ids:(const unsigned *)ids offsets:(const unsigned *)offsets count:(NSUInteger)count
{
	self = [super init];
	if (self == nil) return nil;
	_device = [device retain];
	_count = count;
	_ids = calloc(_count + 1, sizeof(NSUInteger));
	_offsets = calloc(_count + 1, sizeof(NSUInteger));
	_lengths = calloc(_count + 1, sizeof(NSUInteger));
	for (NSUInteger i = 0; i < _count; i++) {
		_ids[i] = ids[i];
		_offsets[i] = offsets[i];
		_lengths[i] = 1;
		_length = MAX(_length, (NSUInteger)offsets[i] + 8);
	}
	return self;
}

- (void)dealloc
{
	free(_ids);
	free(_offsets);
	free(_lengths);
	[_buffer release];
	[_label release];
	[_device release];
	[super dealloc];
}

- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { [label retain]; [_label release]; _label = label; }
- (void)hakiosSetSource:(const char *)source { snprintf(_source, sizeof(_source), "%s", source); }
- (NSUInteger)encodedLength { return _length; }
- (NSUInteger)alignment { return 8; }

- (void)setArgumentBuffer:(id)buffer offset:(NSUInteger)offset
{
	[buffer retain];
	[_buffer release];
	_buffer = buffer;
	_base = offset;
}

- (void)setArgumentBuffer:(id)buffer startOffset:(NSUInteger)startOffset arrayElement:(NSUInteger)arrayElement
{
	[self setArgumentBuffer:buffer offset:startOffset + arrayElement * _length];
}

/* the eight bytes of member ID's element ELEMENT in the buffer set above, or NULL (said once) */
- (uint64_t *)hakiosSlot:(NSUInteger)index element:(NSUInteger)element
{
	if (_buffer == nil) {
		HM_ONCE("an argument encoder was given a member before an argument buffer; nothing is written\n");
		return NULL;
	}
	for (NSUInteger i = 0; i < _count; i++) {
		if (index >= _ids[i] && index < _ids[i] + _lengths[i]) {
			const NSUInteger at = _base + _offsets[i] + 8 * (index - _ids[i] + element);
			if (at + 8 > [(id<MTLBuffer>)_buffer length]) return NULL;
			return (uint64_t *)((uint8_t *)[(id<MTLBuffer>)_buffer contents] + at);
		}
	}
	{
		/* which encoder: where its layout came from and the ids it has, so that a member missing from it is told from a
		 * member missing from the function's own set */
		char ids[160] = "";
		size_t n = 0;

		for (NSUInteger i = 0; i < _count && n < sizeof(ids); i++) {
			n += (size_t)snprintf(ids + n, sizeof(ids) - n, "%s%lu", i ? "," : "", (unsigned long)_ids[i]);
		}
		hm_print("refused: an argument encoder's member %lu, which its layout has not got (ids %s, from %s); nothing is written\n",
			 (unsigned long)index, ids, _source[0] ? _source : "a descriptor array");
	}
	return NULL;
}

- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	uint64_t *w = [self hakiosSlot:index element:0];
	if (w != NULL) *w = buffer != nil ? [(id<MTLBuffer>)buffer gpuAddress] + offset : 0;
}

- (void)setBuffers:(const id *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	uint64_t *w = [self hakiosSlot:index element:0];
	if (w != NULL) *w = texture != nil ? [(id<MTLTexture>)texture gpuResourceID]._impl : 0;
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) [self setTexture:textures[i] atIndex:range.location + i];
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	uint64_t *w = [self hakiosSlot:index element:0];
	if (w != NULL) *w = sampler != nil ? [(id<MTLSamplerState>)sampler gpuResourceID]._impl : 0;
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) [self setSamplerState:samplers[i] atIndex:range.location + i];
}

- (void *)constantDataAtIndex:(NSUInteger)index
{
	hm_print("refused: -[MTLArgumentEncoder constantDataAtIndex:%lu]: inlined constant data has no place in the eight-bytes-a-member "
		 "layout; it returns NULL\n", (unsigned long)index);
	return NULL;
}

- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
	hm_print("refused: a nested argument encoder (member %lu): argument buffers inside argument buffers are not served; it returns nil\n",
		 (unsigned long)index);
	return nil;
}
@end

/* the types whose slices Metal numbers as layers (a 2D or 1D array's, a cube's or cube array's faces): copied a layer
 * at a time by the core's slice copy, Z the slice */
static BOOL
hm_layered(unsigned type)
{
	return type == MTLTextureType2DArray || type == MTLTextureTypeCube || type == MTLTextureType1DArray || type == MTLTextureTypeCubeArray;
}

@implementation HakiosMetalHeap
/* MPS asks a heap Apple's SPI (-protectionOptions first, which raised and took mediaserverd down once heaps were sized
 * and made): what MTLHeapSPI declares is answered or named by the forwarding below, never an unrecognized selector */
HM_REFUSES_OUT_LOUD

- (instancetype)initWithDevice:(id)device descriptor:(MTLHeapDescriptor *)descriptor
{
	if (objc_getProtocol("MTLHeapSPI") != NULL) {
		hm_adopt([self class], objc_getProtocol("MTLHeapSPI"), "MTLHeapSPI");
	}
	self = [super init];
	if (self != nil) {
		_device = [device retain];
		_descriptor = [descriptor copy];
	}
	return self;
}
- (void)dealloc
{
	[_descriptor release];
	[_label release];
	[_device release];
	[super dealloc];
}
- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { [label retain]; [_label release]; _label = label; }
- (MTLStorageMode)storageMode { return [_descriptor storageMode]; }
- (MTLCPUCacheMode)cpuCacheMode { return [_descriptor cpuCacheMode]; }
- (MTLHazardTrackingMode)hazardTrackingMode { return [_descriptor hazardTrackingMode]; }
- (MTLResourceOptions)resourceOptions { return [_descriptor resourceOptions]; }
- (MTLHeapType)type { return [_descriptor type]; }
- (NSUInteger)protectionOptions { return 0; }   /* no protected memory on this port, as a texture answers */
- (NSUInteger)size { return [_descriptor size]; }
- (NSUInteger)usedSize { return _used; }
- (NSUInteger)currentAllocatedSize { return [_descriptor size]; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
	(void)alignment;
	return [_descriptor size] > _used ? [_descriptor size] - _used : 0;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { (void)state; return MTLPurgeableStateNonVolatile; }

/* a resource from the budget: the device's own allocation, refused (nil) when it would overrun the heap, as Metal's is */
- (id)hakiosCounted:(id)resource
{
	NSUInteger size = resource != nil ? [(id<MTLResource>)resource allocatedSize] : 0;

	if (resource == nil) return nil;
	if (_used + size > [_descriptor size]) {
		[resource release];
		return nil;
	}
	_used += size;
	return resource;
}
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	return [self hakiosCounted:[_device newBufferWithLength:length options:options]];
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	return [self hakiosCounted:[_device newTextureWithDescriptor:descriptor]];
}
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
	(void)length; (void)options; (void)offset;
	hm_print("refused: a buffer at an offset of a heap: placement is not served (every resource is its own allocation); it returns nil\n");
	return nil;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset
{
	(void)descriptor; (void)offset;
	hm_print("refused: a texture at an offset of a heap: placement is not served (every resource is its own allocation); it returns nil\n");
	return nil;
}
/* acceleration structures belong to ray tracing, which this GPU's path does not have (the device has none either) */
- (id)newAccelerationStructureWithSize:(NSUInteger)size { (void)size; return [self hakiosNoRayTracing]; }
- (id)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d { (void)d; return [self hakiosNoRayTracing]; }
- (id)newAccelerationStructureWithSize:(NSUInteger)size offset:(NSUInteger)offset { (void)size; (void)offset; return [self hakiosNoRayTracing]; }
- (id)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d offset:(NSUInteger)offset
{
	(void)d; (void)offset;
	return [self hakiosNoRayTracing];
}
- (id)hakiosNoRayTracing
{
	hm_print("refused: an acceleration structure from a heap: no ray tracing on this GPU's path; it returns nil\n");
	return nil;
}
@end

@interface HakiosMetalDepthStencilState : NSObject <MTLDepthStencilState> {
	id<MTLDevice> _device;
	NSString *_label;
	unsigned _compare;
	BOOL _write;
	BOOL _stencil;
	struct hakios_metal_stencil_face _front, _back;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device descriptor:(MTLDepthStencilDescriptor *)descriptor;
- (unsigned)hakiosDepthCompare;
- (BOOL)hakiosDepthWrite;
- (BOOL)hakiosStencil;
- (const struct hakios_metal_stencil_face *)hakiosStencilFront;
- (const struct hakios_metal_stencil_face *)hakiosStencilBack;
@end

@interface HakiosMetalSamplerState : _MTLSamplerState {
	hakios_metal_sampler *_hakiosSampler;
	uint64_t _hakiosUniqueIdentifier;   /* -[_MTLSamplerState uniqueIdentifier] is a nil-stub, and gpuResourceID is built on it */
}
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device sampler:(hakios_metal_sampler *)sampler
			  descriptor:(MTLSamplerDescriptor *)descriptor;
- (hakios_metal_sampler *)hakiosSampler;
@end

@interface HakiosMetalComputePipelineState : _MTLComputePipelineState {
	HakiosMetalDevice *_device;
	hakios_metal_kernel *_kernel;
	NSString *_label;
}
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device kernel:(hakios_metal_kernel *)kernel descriptor:(id)descriptor;
- (hakios_metal_kernel *)hakiosKernel;
@end

/* A render pipeline state: a vertex and a fragment function compiled and linked by hakios-metal for one colour
 * attachment (hakios_metal_program), under Metal's own base class. The ivar names are prefixed so that none shadows the
 * base's _device and _label, which its designated initialiser fills from the descriptor. */
@interface HakiosMetalRenderPipelineState : _MTLRenderPipelineState {
	HakiosMetalDevice *_hakiosDevice;
	hakios_metal_program *_program;
	uint64_t _hakiosUniqueIdentifier;   /* the base's uniqueIdentifier is a nil-stub; see the sampler */
	/* an uber shader's specialisations (hakiosProgramFor:): its fragment bitcode and constants, the constant buffer
	 * whose bytes choose its path, and the programs compiled for each content seen (NSNull while one compiles) */
	NSData *_fsAir;
	hakios_metal_constant _fsConstants[64];
	unsigned _fsConstantCount;
	int _specBinding;
	unsigned _specLength;
	NSMutableDictionary *_specialised;
}
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device program:(hakios_metal_program *)program
			  descriptor:(MTLRenderPipelineDescriptor *)descriptor;
- (hakios_metal_program *)hakiosProgram;
- (void)hakiosKeepFragment:(NSData *)air constants:(const hakios_metal_constant *)constants count:(unsigned)count;
- (hakios_metal_program *)hakiosProgramFor:(hakios_metal_buffer *const *)buffers offsets:(const uint64_t *)offsets count:(unsigned)count;
@end

@interface HakiosMetalCommandQueue : _MTLCommandQueue {
	HakiosMetalDevice *_device;
	hakios_metal_queue *_queue;
	NSUInteger _gpuPriority, _backgroundGPUPriority;
	dispatch_queue_t _completionQueue;       /* only where Apple's own ivar is missing */
	dispatch_queue_t _deferred;              /* commits held for an event, in commit order; made on the first one */
	long _deferring;                         /* commits on _deferred not yet made (atomic) */
}
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device descriptor:(id)descriptor;
- (hakios_metal_queue *)hakiosQueue;
- (dispatch_queue_t)hakiosCompletionQueue;
- (BOOL)hakiosDeferring;
- (void)hakiosDefer:(dispatch_block_t)commit;
@end

@interface HakiosMetalCommandBuffer : _MTLCommandBuffer {
	HakiosMetalCommandQueue *_queue;
	hakios_metal_cb *_cb;
	NSMutableArray *_scheduled, *_completed; /* handler blocks, copied to the heap */
	NSError *_error;
	BOOL _committed;
	BOOL _owesSlot;                          /* holds one of the queue's maxCommandBufferCount slots */
	double _committedAt;                     /* host seconds, for the GPU times Metal reports */
	double _createdAt;                       /* ...and when it was made: metal_fps's encoding time */
	NSUInteger _protectionOptions;
	NSMutableArray *_signals, *_waits;       /* { HakiosMetalEvent, value }: applied at completion / met before the GPU sees it */
	dispatch_semaphore_t _sent;              /* a held commit: signalled once it has reached the GPU */
	NSMutableArray *_turned;                 /* HakiosMetalTexture: turned framebuffers drawn here, copied into their surfaces at commit */
}
- (instancetype)initWithHakiosQueue:(HakiosMetalCommandQueue *)queue;
- (void)hakiosTurnAtCommit:(id)texture;
- (hakios_metal_cb *)hakiosCommandBuffer;
- (double)hakiosCreatedAt;
@end

/* A parallel render encoder: one pass whose draws come from several sub-encoders, filled on any thread in any order and
 * run in the order they were made. Each records into a core command buffer of its own; -endEncoding appends them to
 * this command buffer in that order (hakios_metal_cb_append). The first takes the pass's load actions and the rest load
 * what came before; every one stores, and the pass's resolves run once at the end, over the whole of it. */
@interface HakiosMetalParallelRenderCommandEncoder : _MTLCommandEncoder {
	HakiosMetalCommandBuffer *_commandBuffer;   /* not retained: the base class retains it until -endEncoding */
	struct hakios_metal_render_pass _pass;
	HakiosMetalTexture *_colorTexture;
	MTLRenderPassDescriptor *_descriptor;       /* copied: its resolves are read at -endEncoding */
	NSMutableArray *_subs;                      /* NSValue (pointer) of each sub-encoder's hakios_metal_cb, in creation order */
	NSLock *_lock;
}
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer pass:(const struct hakios_metal_render_pass *)pass
			       colorTexture:(HakiosMetalTexture *)texture descriptor:(MTLRenderPassDescriptor *)descriptor;
@end

@interface HakiosMetalBlitCommandEncoder : _MTLCommandEncoder {
	HakiosMetalCommandBuffer *_commandBuffer;   /* not retained: the base class retains it until -endEncoding */
}
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer;
@end

@interface HakiosMetalComputeCommandEncoder : _MTLCommandEncoder {
	HakiosMetalCommandBuffer *_commandBuffer;   /* not retained, as above */
	HakiosMetalComputePipelineState *_pipeline;
	HakiosMetalBuffer *_buffers[HM_MAX_BUFFERS];
	uint64_t _offsets[HM_MAX_BUFFERS];
	unsigned _highest;                          /* one past the highest index bound */
	HakiosMetalTexture *_textures[HM_MAX_TEXTURES];         /* by Metal texture index: a sampled texture or an image, the kernel decides */
	unsigned _highestTexture;
	HakiosMetalSamplerState *_samplers[HM_MAX_SAMPLERS];    /* by Metal sampler index */
	unsigned _highestSampler;
	NSMutableArray *_scratch;                   /* the buffers -setBytes: made, alive until -endEncoding */
	NSUInteger _threadgroupLength;              /* setThreadgroupMemoryLength: at index 0, given to each dispatch */
	char _refused[160];                         /* a binding this encoder could not take: the dispatch is failed by it */
}
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer;
@end

/*
 * The render encoder: one colour attachment read off the pass descriptor at creation (Metal reads nothing of it
 * afterwards), the pipeline, viewport, scissor and per-stage buffer tables stored as the compute encoder stores its
 * bindings, and each -drawPrimitives: one whole pass of hakios-metal's (begin, state, draw, end: hakios_metal_cb_draw).
 * No _MTLRenderCommandEncoder exists to inherit from; the base is _MTLCommandEncoder, as for the compute encoder.
 */
@interface HakiosMetalRenderCommandEncoder : _MTLCommandEncoder {
	HakiosMetalCommandBuffer *_commandBuffer;   /* not retained: the base class retains it until -endEncoding */
	hakios_metal_cb *_sub;                      /* a parallel encoder's sub-encoder: its own core command buffer, which
	                                             * the parallel encoder appends in creation order; NULL otherwise */
	HakiosMetalTexture *_colorTexture;          /* colour attachment 0, retained until the encoder goes */
	HakiosMetalTexture *_resolveFrom[8], *_resolveTo[8];   /* each colour attachment's samples and its resolveTexture, retained */
	unsigned _resolveLevel[8];
	BOOL _resolving[8];                         /* the store action resolves (2, 3, or Unknown with a resolveTexture): at -endEncoding */
	struct hakios_metal_render_pass _pass;      /* its hakios texture, load and store actions, clear colour */
	HakiosMetalRenderPipelineState *_pipeline;
	struct hakios_metal_viewport _viewport;
	struct hakios_metal_scissor _scissor;
	BOOL _hasViewport, _hasScissor;             /* unset: hakios-metal takes the whole attachment (NULL) */
	id _indirectArgs;                           /* the next draw is indirect: its words at _indirectOffset of this buffer */
	NSUInteger _indirectOffset;
	HakiosMetalBuffer *_vsBuffers[HM_MAX_BUFFERS];
	HakiosMetalBuffer *_fsBuffers[HM_MAX_BUFFERS];
	uint64_t _vsOffsets[HM_MAX_BUFFERS], _fsOffsets[HM_MAX_BUFFERS];
	unsigned _vsHighest, _fsHighest;            /* one past the highest index bound, per stage */
	HakiosMetalTexture *_fsTextures[HM_MAX_TEXTURES];         /* the fragment stage's [[texture(n)]], retained */
	HakiosMetalSamplerState *_fsSamplers[HM_MAX_SAMPLERS];    /* ... and [[sampler(n)]] */
	unsigned _fsHighestTexture, _fsHighestSampler;
	HakiosMetalTexture *_vsTextures[HM_MAX_TEXTURES];         /* the vertex stage's, the same way */
	HakiosMetalSamplerState *_vsSamplers[HM_MAX_SAMPLERS];
	unsigned _vsHighestTexture, _vsHighestSampler;
	NSMutableArray *_scratch;                   /* the buffers -setVertexBytes:/-setFragmentBytes: made */
	MTLWinding _frontFacingWinding;             /* -setFrontFacingWinding:'s, also in _pass.winding for the draws */
	char _refused[160];                         /* a binding or state this encoder could not take: the next draw is failed by it */
	NSMutableIndexSet *_visibilityCounted;      /* the result words (offset / 8) a draw of this encoder has counted into */
}
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer pass:(const struct hakios_metal_render_pass *)pass
			       colorTexture:(HakiosMetalTexture *)texture;
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer pass:(const struct hakios_metal_render_pass *)pass
			       colorTexture:(HakiosMetalTexture *)texture sub:(hakios_metal_cb *)sub;
- (void)hakiosTakeResolves:(MTLRenderPassDescriptor *)descriptor;
@end

/* ============================================================================================================
 * The device
 * ============================================================================================================ */

/* boot-arg metal_dump=refused (a diagnostic, beside metal_dump=FNV): the bitcode of each function in a pipeline the
 * compiler refused, into the log in hex, labelled, for the host's front end to take apart. What it prints is the
 * system's code: it stays in the log. metal_dump=refused:NAME[,NAME...] dumps those functions alone -- a process
 * refusing a dozen pipelines at once overruns the console ring, and a module with lines missing cannot be read back.
 * metal_dump=fn:NAME[,NAME...] dumps those functions when a pipeline is made of them though nothing was refused, with
 * the function constants they were specialised by: a pipeline that compiles and draws the wrong thing (VideoToolbox's
 * converter over the live wallpaper's frames) is read the same way. */
/* What a draw, dispatch or copy this device refuses does to its command buffer. Apple's driver validates nothing and runs
 * a frame whatever is wrong in it, so by default the refused piece of work is left out -- said, as every refusal is --
 * and the rest of the buffer runs: Resident Evil 4 has, every frame, one dispatch this does not serve yet, and failing
 * the buffer for it showed nothing at all. Boot-arg metal_strict ends the buffer in error instead, as the plug-in
 * always did: what to boot with to find the first refusal of a frame; and HAKIOS_METAL_STRICT=1 in a process's
 * environment, read at each command buffer, for the tests whose refusals are checked by the buffer's error. */
static BOOL
hm_strict(void)
{
	static int strict = -1;
	const char *env = getenv("HAKIOS_METAL_STRICT");

	if (env != NULL) {
		return strcmp(env, "1") == 0;
	}
	if (strict < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		strict = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_strict") != NULL;
	}
	return strict != 0;
}

/*
 * CoreAnimation's shaders, chosen (hakios): this image is the iPhone X's, whose QuartzCore carries no default.metallib
 * of its functions -- only default.pipelinelib, a library of pipelines precompiled for Apple's GPU. Its "functions"
 * file holds every pipeline's functions all the same, as AIR: 451 single-function metallibs, the four uber shaders
 * (fixed_vert_lph_gen, fixed_vert_lph_spc, fixed_frag_lph_cpf, fixed_frag_lph_cph) each already specialised for one
 * set of CoreAnimation's constants, and pipelines.desc names them: its section 1 the label of each (in the order of the
 * functions file, "com.apple.coreanimation.frag.A2Xghfc"), its section 7 each pipeline's label with the labels of its
 * two functions. The window server is handed a default.metallib of one variant of each uber shader (ca_metallib=,
 * the kernel's inject.c); here, when a pipeline is made of one of them, the variant its label names is the one
 * compiled -- by the function's own label, or by the pipeline's (draw label -> vertex and fragment labels).
 */
#define HM_CA_PIPELINELIB "/System/Library/Frameworks/QuartzCore.framework/default.pipelinelib"
static NSData *hm_ca_functions;                 /* the functions file, mapped */
static NSMutableDictionary *hm_ca_variants;     /* function label -> NSData of its module (the 0x0B17C0DE wrapper) */
static NSMutableDictionary *hm_ca_pipelines;    /* draw label -> @[vertex label, fragment label] */

static NSData *
hm_ca_module(const uint8_t *lib, size_t size)
{
	uint64_t funcs, bc;
	uint32_t entry;

	/* the one function's bitcode: the bitcode section at +0x48 plus the function's OFFT, MDSZ long */
	if (size < 0x58 || memcmp(lib, "MTLB", 4) != 0) {
		return nil;
	}
	memcpy(&funcs, lib + 0x18, 8);
	memcpy(&bc, lib + 0x48, 8);
	if (funcs + 8 > size) {
		return nil;
	}
	memcpy(&entry, lib + funcs + 4, 4);
	for (uint64_t q = funcs + 8, end = funcs + 4 + entry, offt = ~0ull, mdsz = 0; q + 6 <= end && q + 6 <= size;) {
		uint16_t len;

		if (memcmp(lib + q, "ENDT", 4) == 0) {
			break;
		}
		memcpy(&len, lib + q + 4, 2);
		if (memcmp(lib + q, "OFFT", 4) == 0 && len == 24 && q + 6 + 24 <= size) {
			memcpy(&offt, lib + q + 6 + 16, 8);
		} else if (memcmp(lib + q, "MDSZ", 4) == 0 && len == 8 && q + 6 + 8 <= size) {
			memcpy(&mdsz, lib + q + 6, 8);
		}
		q += 6 + len;
		if ((offt != ~0ull && mdsz != 0) && bc + offt + mdsz <= size) {
			return [NSData dataWithBytes:lib + bc + offt length:(NSUInteger)mdsz];
		}
	}
	return nil;
}

static void
hm_ca_load(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		NSData *desc = [NSData dataWithContentsOfFile:@HM_CA_PIPELINELIB "/pipelines.desc"];
		const uint8_t *d = [desc bytes], *f;
		size_t dn = [desc length], fn, at = 0;
		uint32_t sections, off[16], n;
		NSMutableArray *labels = [NSMutableArray array];

		hm_ca_functions = [[NSData alloc] initWithContentsOfFile:@HM_CA_PIPELINELIB "/functions"
								 options:NSDataReadingMappedIfSafe error:NULL];
		hm_ca_variants = [[NSMutableDictionary alloc] init];
		hm_ca_pipelines = [[NSMutableDictionary alloc] init];
		if (desc == nil || hm_ca_functions == nil || dn < 8) {
			hm_print("CoreAnimation's pipeline library is not at " HM_CA_PIPELINELIB ": no variants to choose from\n");
			return;
		}
		memcpy(&sections, d, 4);
		if (sections > 16 || 4 + 4 * sections > dn) {
			return;
		}
		memcpy(off, d + 4, 4 * sections);
		/* section 1: the functions' labels, in the order of the functions file */
		if (sections > 7 && off[1] + 4 <= dn) {
			memcpy(&n, d + off[1], 4);
			for (uint32_t i = 0; i < n && off[1] + 8 + 4 * i <= dn; i++) {
				uint32_t r;

				memcpy(&r, d + off[1] + 4 + 4 * i, 4);
				if (off[1] + r + 16 < dn) {
					const char *label = (const char *)d + off[1] + r + 16;

					[labels addObject:[NSString stringWithUTF8String:strnlen(label, dn - off[1] - r - 16) < 200 ? label : ""]];
				}
			}
		}
		/* the functions file: the metallibs one after the other, in that order */
		f = [hm_ca_functions bytes];
		fn = [hm_ca_functions length];
		for (NSUInteger i = 0; i < [labels count]; i++) {
			const uint8_t *m = at < fn ? memmem(f + at, fn - at, "MTLB", 4) : NULL;
			uint64_t total;
			NSData *module;

			if (m == NULL || (size_t)(m - f) + 0x18 > fn) {
				break;
			}
			memcpy(&total, m + 0x10, 8);
			if ((size_t)(m - f) + total > fn) {
				break;
			}
			if ((module = hm_ca_module(m, (size_t)total)) != nil) {
				hm_ca_variants[labels[i]] = module;
			}
			at = (size_t)(m - f) + (size_t)total;
		}
		/* section 7: each pipeline's label with its functions' (draw, vert, frag), as strings in its entry */
		if (sections > 8 && off[7] + 4 <= dn) {
			memcpy(&n, d + off[7], 4);
			for (uint32_t i = 0; i < n && off[7] + 8 + 4 * i <= dn; i++) {
				uint32_t r, next = i + 1 < n ? 0 : off[8] - off[7];
				NSString *draw = nil, *vert = nil, *frag = nil;

				memcpy(&r, d + off[7] + 4 + 4 * i, 4);
				if (i + 1 < n) {
					memcpy(&next, d + off[7] + 4 + 4 * (i + 1), 4);
				}
				for (uint32_t q = r; q + 24 < next && off[7] + q + 24 < dn; q++) {
					const char *s = (const char *)d + off[7] + q;

					if (strncmp(s, "com.apple.coreanimation.", 24) != 0) {
						continue;
					}
					NSString *label = [NSString stringWithUTF8String:s];
					if ([label hasPrefix:@"com.apple.coreanimation.draw."]) draw = label;
					else if ([label hasPrefix:@"com.apple.coreanimation.vert."]) vert = label;
					else if ([label hasPrefix:@"com.apple.coreanimation.frag."]) frag = label;
					q += (uint32_t)strlen(s);
				}
				if (draw != nil && vert != nil) {
					hm_ca_pipelines[draw] = frag != nil ? @[vert, frag] : @[vert];
				}
			}
		}
		hm_print("CoreAnimation's shaders: %lu labelled variants of its uber shaders, %lu pipelines (" HM_CA_PIPELINELIB ")\n",
			 (unsigned long)[hm_ca_variants count], (unsigned long)[hm_ca_pipelines count]);
	});
}

/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE, vertex stage or not): its variant when
 * CoreAnimation's labels name one, else what the function itself has. */
static NSData *
hm_bitcode_for(id function, NSString *pipeline, BOOL vertex)
{
	static unsigned said;
	NSString *name = [function respondsToSelector:@selector(name)] ? [function name] : nil, *label = nil, *chosen = nil;
	NSData *own = ((NSData *(*)(id, SEL))objc_msgSend)(function, sel_getUid("bitcodeData")), *variant = nil;

	if (name == nil || ![name hasPrefix:@"fixed_"]) {
		return own;
	}
	hm_ca_load();
	if ([function respondsToSelector:@selector(label)]) {
		label = [function label];
	}
	if (label != nil && (variant = hm_ca_variants[label]) != nil) {
		chosen = label;
	} else if (pipeline != nil && hm_ca_pipelines[pipeline] != nil) {
		NSArray *pair = hm_ca_pipelines[pipeline];

		chosen = vertex ? pair[0] : ([pair count] > 1 ? pair[1] : nil);
		variant = chosen != nil ? hm_ca_variants[chosen] : nil;
	}
	if (said++ < 40) {
		hm_print("CoreAnimation's %s (label %s, pipeline %s): %s%s\n", [name UTF8String], label ? [label UTF8String] : "none",
			 pipeline ? [pipeline UTF8String] : "none", variant ? "the variant " : "no variant: its own module",
			 variant ? [chosen UTF8String] : "");
	}
	return variant != nil ? variant : own;
}

static void
hm_dump_function(const char *label, id function, const char *key, const hakios_metal_constant *constants, unsigned count)
{
	char args[1024] = {0};
	size_t size = sizeof(args) - 1;
	const char *at;

	if (function == nil || sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (at = strstr(args, key)) == NULL ||
	    ![function respondsToSelector:sel_getUid("bitcodeData")]) {
		return;
	}
	at += strlen(key);
	if (*at == ':') {
		const char *name = [[function name] UTF8String];
		bool listed = false;

		for (const char *p = at + 1; name != NULL && *p != '\0' && *p != ' ' && !listed; p += strcspn(p, ", ") + (p[strcspn(p, ", ")] == ',')) {
			size_t len = strcspn(p, ", ");

			listed = strlen(name) == len && strncmp(p, name, len) == 0;
		}
		if (!listed) {
			return;
		}
	}
	NSData *air = hm_bitcode_for(function, nil, NO);
	const uint8_t *b = (const uint8_t *)[air bytes];

	for (NSUInteger o = 0; o < [air length]; o += 64) {
		char line[160];
		size_t n = 0;

		for (NSUInteger i = o; i < o + 64 && i < [air length]; i++) n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x", b[i]);
		hm_print("dump refused-%s %s %06lx %s\n", label, [[function name] UTF8String], (unsigned long)o, line);
	}
	for (unsigned i = 0; i < count; i++) {
		hm_print("dump constant-%s %s %u %#llx\n", label, [[function name] UTF8String], constants[i].index,
			 (unsigned long long)constants[i].bits);
	}
}

static void
hm_dump_refused(const char *label, id function)
{
	hm_dump_function(label, function, "metal_dump=refused", NULL, 0);
}

@implementation HakiosMetalDevice

/* Declared at +load because Metal loads this bundle and is therefore already in the process; -initWithAcceleratorPort:
 * declares it again for a process that loaded the bundle by hand before asking Metal anything. */
static void
hm_ensure_spi(Class cls)
{
	hm_adopt(cls, objc_getProtocol("MTLDeviceSPI"), "MTLDeviceSPI");
}

+ (void)load
{
	hm_ensure_spi(self);
}

HM_REFUSES_OUT_LOUD

/* An argument encoder for buffer INDEX of FUNCTION, from what the function's AIR says the set holds; nil, said, when it has
 * no set there or the front end refused it. */
static id
hm_function_argument_encoder(id function, NSUInteger index)
{
	NSData *air = hm_bitcode_for(function, nil, NO);
	NSString *name = [(id<MTLFunction>)function name];
	unsigned *ids, *offsets;
	int n;

	n = air != nil ? hakios_metal_argument_buffer_layout([air bytes], [air length], (unsigned)index, NULL, NULL, 0) : -1;
	if (n <= 0) {
		hm_print("refused: an argument encoder for buffer %lu of %s: %s; it returns nil\n", (unsigned long)index,
			 name != nil ? [name UTF8String] : "a function", air != nil ? hakios_metal_error() : "no bitcodeData");
		return nil;
	}
	ids = calloc((size_t)n, sizeof(unsigned));
	offsets = calloc((size_t)n, sizeof(unsigned));
	{
		const int filled = hakios_metal_argument_buffer_layout([air bytes], [air length], (unsigned)index, ids, offsets, (unsigned)n);

		if (filled != n) {
			hm_print("refused: an argument encoder for buffer %lu of %s: its layout read %d members, then %d: %s; it returns nil\n",
				 (unsigned long)index, name != nil ? [name UTF8String] : "a function", n, filled, filled < 0 ? hakios_metal_error() : "");
			free(ids);
			free(offsets);
			return nil;
		}
	}
	id encoder = [[HakiosMetalArgumentEncoder alloc] initWithDevice:[(id<MTLFunction>)function device] ids:ids offsets:offsets
								     count:(NSUInteger)n];
	char source[96];

	snprintf(source, sizeof(source), "buffer %lu of %s", (unsigned long)index, name != nil ? [name UTF8String] : "a function");
	[encoder hakiosSetSource:source];
	free(ids);
	free(offsets);
	return encoder;
}

/*
 * MTLFunction's -newArgumentEncoderWithBufferIndex: (and its reflection: form), for functions of this device. Metal's
 * own builds the encoder from the reflection Apple's GPU compiler writes beside a function; no function here has any,
 * so for every index it asserts "bufferIndex 0 does not identify an argument buffer" and aborts the process -- which
 * is how Resident Evil 4 ended, before its first frame. The class's two methods are wrapped once: a function whose
 * device is this one gets an encoder laid out by the metadata, any other the original's answer.
 */
static void
hm_functions_encode_from_metadata(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		Class c = objc_getClass("_MTLFunction");
		SEL plain = sel_getUid("newArgumentEncoderWithBufferIndex:");
		SEL reflected = sel_getUid("newArgumentEncoderWithBufferIndex:reflection:");
		Method m;

		if (c == Nil) {
			hm_print("no _MTLFunction class: argument encoders from functions stay Metal's own\n");
			return;
		}
		if ((m = class_getInstanceMethod(c, plain)) != NULL) {
			id (*original)(id, SEL, NSUInteger) = (id (*)(id, SEL, NSUInteger))method_getImplementation(m);
			method_setImplementation(m, imp_implementationWithBlock(^id(id self, NSUInteger index) {
				if (![[(id<MTLFunction>)self device] isKindOfClass:[HakiosMetalDevice class]]) return original(self, plain, index);
				return hm_function_argument_encoder(self, index);
			}));
		}
		if ((m = class_getInstanceMethod(c, reflected)) != NULL) {
			id (*original)(id, SEL, NSUInteger, id *) = (id (*)(id, SEL, NSUInteger, id *))method_getImplementation(m);
			method_setImplementation(m, imp_implementationWithBlock(^id(id self, NSUInteger index, id *reflection) {
				if (![[(id<MTLFunction>)self device] isKindOfClass:[HakiosMetalDevice class]]) {
					return original(self, reflected, index, reflection);
				}
				if (reflection != NULL) *reflection = nil;   /* no reflection here: the encoder is the whole answer */
				return hm_function_argument_encoder(self, index);
			}));
		}
	});
}

/*
 * Functions CoreAnimation of iOS 16.5 asks its library for that the library given it (QuartzCore-default.metallib:
 * this image's uber shaders and a later CoreAnimation's other functions) does not have under that name, each answered
 * with the nearest one it has: luma_boost (a picture's luminance raised, a later CoreAnimation scales by its EDR
 * factor instead). A function asked for that the library has is never touched. (hakios)
 */
static const char *const hm_stand_ins[][2] = {
	{ "luma_boost_lph", "edr_scale_lph" },
	{ "luma_boost_lpf", "edr_scale_lpf" },
};
static id (*hm_orig_fn_name)(id, SEL, NSString *);
static id (*hm_orig_fn_constants)(id, SEL, NSString *, MTLFunctionConstantValues *, NSError **);

static NSString *
hm_stand_in(NSString *name)
{
	for (size_t i = 0; name != nil && i < sizeof(hm_stand_ins) / sizeof(hm_stand_ins[0]); i++) {
		if (strcmp([name UTF8String], hm_stand_ins[i][0]) == 0) {
			return [NSString stringWithUTF8String:hm_stand_ins[i][1]];
		}
	}
	return nil;
}

static id
hm_fn_name(id self, SEL sel, NSString *name)
{
	id f = hm_orig_fn_name(self, sel, name);
	NSString *other = f == nil ? hm_stand_in(name) : nil;

	if (other != nil && (f = hm_orig_fn_name(self, sel, other)) != nil) {
		HM_ONCE("the library has no %s: %s stands in for it\n", [name UTF8String], [other UTF8String]);
	}
	return f;
}

static id
hm_fn_constants(id self, SEL sel, NSString *name, MTLFunctionConstantValues *values, NSError **error)
{
	id f = hm_orig_fn_constants(self, sel, name, values, error);
	NSString *other = f == nil ? hm_stand_in(name) : nil;

	if (other != nil && (f = hm_orig_fn_constants(self, sel, other, values, error)) != nil) {
		if (error != NULL) *error = nil;
		HM_ONCE("the library has no %s: %s stands in for it (with constants)\n", [name UTF8String], [other UTF8String]);
	}
	return f;
}

static void
hm_library_stand_ins(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		Class library = NSClassFromString(@"_MTLLibrary");
		Method a = library ? class_getInstanceMethod(library, @selector(newFunctionWithName:)) : NULL;
		Method b = library ? class_getInstanceMethod(library, @selector(newFunctionWithName:constantValues:error:)) : NULL;

		if (a != NULL) hm_orig_fn_name = (id (*)(id, SEL, NSString *))method_setImplementation(a, (IMP)hm_fn_name);
		if (b != NULL) hm_orig_fn_constants = (id (*)(id, SEL, NSString *, MTLFunctionConstantValues *, NSError **))
			method_setImplementation(b, (IMP)hm_fn_constants);
	});
}

/*
 * (hakios) boot-arg ca_prof=US, the window server: every US microseconds every thread of the process is looked at
 * -- running or waiting -- and its stack taken (the program counter, the link register, the frame chain); every
 * CA_PROF_DUMP looks the most frequent stacks of each named thread go to the log as "ca-prof: THREAD N%: ...", with
 * the image and offset of each address. Where CoreAnimation's render loop waits on the Metal road is what this is for.
 */
#define CA_PROF_SLOTS   4096
#define CA_PROF_DUMP    400
#define CA_PROF_DEPTH   10
static unsigned ca_prof_us;
static struct {
	char name[32];
	uint64_t stack[CA_PROF_DEPTH];
	unsigned count;
} ca_prof_table[CA_PROF_SLOTS];

static int
ca_prof_order(const void *a, const void *b)
{
	const typeof(ca_prof_table[0]) *x = a, *y = b;
	int byname = strcmp(x->name, y->name);

	return byname != 0 ? byname : (int)y->count - (int)x->count;
}

static void *
ca_prof_main(__unused void *argument)
{
	unsigned looks = 0;
	thread_t self = mach_thread_self();

	for (;;) {
		thread_act_array_t threads;
		mach_msg_type_number_t n = 0;

		usleep(ca_prof_us);
		if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) {
			continue;
		}
		looks++;
		for (mach_msg_type_number_t t = 0; t < n; t++) {
			thread_extended_info_data_t ext;
			mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
			arm_thread_state64_t state;
			mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
			uint64_t stack[CA_PROF_DEPTH] = { 0 }, hash = 0;

			if (threads[t] == self || thread_info(threads[t], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &count) != KERN_SUCCESS ||
			    thread_suspend(threads[t]) != KERN_SUCCESS) {
				mach_port_deallocate(mach_task_self(), threads[t]);
				continue;
			}
			kern_return_t got = thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count);
			if (got == KERN_SUCCESS) {
				uint64_t sp = arm_thread_state64_get_sp(state), fp = arm_thread_state64_get_fp(state), top = 0;
				pthread_t pt = pthread_from_mach_thread_np(threads[t]);

				/* the frame chain is followed only within the thread's own stack (another thread's frame pointer
				 * read blindly faulted the window server: SIGSEGV, logs/155201-run.log) */
				if (pt != NULL) top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(pt);
				stack[0] = arm_thread_state64_get_pc(state);
				stack[1] = arm_thread_state64_get_lr(state);
				for (unsigned d = 2; d < CA_PROF_DEPTH && top != 0 && fp >= sp && fp + 16 <= top && (fp & 15) == 0; d++) {
					const uint64_t *record = (const uint64_t *)(uintptr_t)fp;

					stack[d] = record[1];
					if (record[0] <= fp) break;
					fp = record[0];
				}
			}
			thread_resume(threads[t]);
			mach_port_deallocate(mach_task_self(), threads[t]);
			if (got != KERN_SUCCESS) continue;
			for (unsigned d = 0; d < CA_PROF_DEPTH; d++) hash = (hash ^ stack[d]) * 0x9e3779b97f4a7c15ull;
			for (size_t c = 0; ext.pth_name[c] != 0; c++) hash = (hash ^ (uint8_t)ext.pth_name[c]) * 0x100000001b3ull;
			for (unsigned i = (unsigned)(hash >> 32) % CA_PROF_SLOTS, probes = 0; probes < CA_PROF_SLOTS; i = (i + 1) % CA_PROF_SLOTS, probes++) {
				if (ca_prof_table[i].count == 0 || (memcmp(ca_prof_table[i].stack, stack, sizeof(stack)) == 0 &&
				    strncmp(ca_prof_table[i].name, ext.pth_name, sizeof(ca_prof_table[i].name)) == 0)) {
					memcpy(ca_prof_table[i].stack, stack, sizeof(stack));
					strlcpy(ca_prof_table[i].name, ext.pth_name[0] ? ext.pth_name : "(unnamed)", sizeof(ca_prof_table[i].name));
					ca_prof_table[i].count++;
					break;
				}
			}
		}
		vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(thread_t));
		if (looks < CA_PROF_DUMP) continue;
		qsort(ca_prof_table, CA_PROF_SLOTS, sizeof(ca_prof_table[0]), ca_prof_order);
		hm_print("ca-prof: %u looks at every thread\n", looks);
		for (unsigned i = 0, shown = 0; i < CA_PROF_SLOTS; i++) {
			char line[1400];
			int at;

			if (ca_prof_table[i].count == 0 || (i > 0 && strcmp(ca_prof_table[i].name, ca_prof_table[i - 1].name) == 0 && ++shown > 4)) continue;
			if (i == 0 || strcmp(ca_prof_table[i].name, ca_prof_table[i - 1].name) != 0) shown = 0;
			if (strstr(ca_prof_table[i].name, "render") == NULL && strstr(ca_prof_table[i].name, "coreanimation") == NULL &&
			    strstr(ca_prof_table[i].name, "hakios") == NULL && strcmp(ca_prof_table[i].name, "(unnamed)") != 0) continue;
			at = snprintf(line, sizeof(line), "ca-prof: %s %.0f%%:", ca_prof_table[i].name, 100.0 * ca_prof_table[i].count / looks);
			for (unsigned d = 0; d < CA_PROF_DEPTH && ca_prof_table[i].stack[d] != 0 && at < (int)sizeof(line) - 80; d++) {
				Dl_info where = { 0 };
				const char *image = "?", *slash;
				uint64_t offset = ca_prof_table[i].stack[d];

				if (dladdr((void *)(uintptr_t)offset, &where) != 0 && where.dli_fname != NULL) {
					image = (slash = strrchr(where.dli_fname, '/')) != NULL ? slash + 1 : where.dli_fname;
					offset -= (uint64_t)(uintptr_t)where.dli_fbase;
				}
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %s+0x%llx(%s)", image, (unsigned long long)offset,
					       where.dli_sname != NULL ? where.dli_sname : "?");
			}
			hm_print("%s\n", line);
		}
		memset(ca_prof_table, 0, sizeof(ca_prof_table));
		looks = 0;
	}
	return NULL;
}

static void
ca_prof_start(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;
		pthread_t sampler;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (at = strstr(args, "ca_prof=")) == NULL) return;
		ca_prof_us = (unsigned)atoi(at + 8);
		if (ca_prof_us == 0) return;
		pthread_create(&sampler, NULL, ca_prof_main, NULL);
		hm_print("ca-prof: every thread looked at every %u us\n", ca_prof_us);
	});
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	/* (hakios) the window server alone is shown the device: every other client of this image's Metal (SpringBoard
	 * through SiriUICore, mediaserverd, ...) finds pipeline libraries made for Apple's GPUs with no default.metallib
	 * beside them and aborts, while without a device it takes the road it takes today. Boot-arg metal_all=1: all. */
	{
		static int all = -1;
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *name = getprogname();

		if (all < 0) {
			all = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_all=1") != NULL;
		}
		if (!all && (name == NULL || strcmp(name, "backboardd") != 0)) {
			HM_ONCE("%s is not shown the device: the window server alone is (metal_all=1: every process)\n", name ? name : "?");
			return nil;     /* (not released: before -init, _MTLDevice's -dealloc reads what -init never set) */
		}
	}
	hm_library_stand_ins();
	ca_prof_start();
	/* (hakios) the window server's compiled stages kept on disk (hakios-metal: compileStage's cache), so that a
	 * CoreAnimation shader compiled once is not compiled again by the next window server; and no listings */
	mkdir("/var/tmp/hakios-metal-cache", 0755);
	setenv("HAKIOS_METAL_CACHE", "/var/tmp/hakios-metal-cache", 0);
	setenv("HAKIOS_METAL_LISTING", "0", 0);
	hm_ensure_spi([self class]);
	hm_functions_encode_from_metadata();
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_acceleratorService = port;
	if (port != IO_OBJECT_NULL) {
		IOObjectRetain(port);
	}
	/*
	 * No GPU, no device. hakios-metal opens the kernel's HakiosGPU client, which exists only when the kernel has
	 * started the Adreno (boot-arg gpu=1, firmware in fw/). Returning nil here makes Metal skip the service -- its
	 * MTLRegisterDevices does so silently, which is why the reason is printed first -- and every process then takes
	 * the software road it takes today. The alternative, a device that exists and refuses every buffer, was measured
	 * worse than none: it lets CoreAnimation choose the Metal road and abort on it.
	 */
	_gpu = hakios_metal_open();
	if (_gpu == NULL) {
		hm_print("no device: %s\n", hakios_metal_error());
		[self release];
		return nil;
	}
	/* Without this the device inherits _MTLDevice's own name, "Apple GPU": true of the code, false of the hardware.
	 * The chip id is asked of the kernel (hakios-metal reads it off the GPU's parameters), not written down. */
	_name = [[NSString alloc] initWithFormat:@"Adreno (hakios-metal, chip %#llx)", (unsigned long long)hakios_metal_gpu_id(_gpu)];
	/* The struct -targetDeviceInfo answers by pointer, filled once here where nobody can have asked yet: the name
	 * with its spaces removed is Apple's own rule for a device that does not answer, and the bytes are copied into
	 * the object because the pointer is handed out for as long as the device lives. */
	snprintf(_targetName, sizeof(_targetName), "%s",
		 [[_name stringByReplacingOccurrencesOfString:@" " withString:@""] UTF8String]);
	_target.target = 0;
	_target.version = 0;
	_target.name = _targetName;
	hm_print("device: %s on IOAcceleratorES service %#x; the compiler is %s; feature profile 1 (Apple1, Apple2 and "
		 "Common1); pipelines.%s is the precompiled pipeline library Metal will look for and not find\n",
		 [_name UTF8String], (unsigned)port,
		 hakios_metal_has_compiler() ? "in this build" : "NOT in this build (make ios MESA=1): every pipeline will be refused",
		 _targetName);
	return self;
}

- (void)dealloc
{
	if (_gpu != NULL) {
		hakios_metal_close(_gpu);
	}
	if (_acceleratorService != IO_OBJECT_NULL) {
		IOObjectRelease(_acceleratorService);
	}
	[_name release];
	[super dealloc];
}

- (hakios_metal_device *)hakiosDevice { return _gpu; }

/* The profile is what -[_MTLDevice initLimits] switches on: measured on the phone, 0, 1 and 4 to 9 fill the limits
 * table and 2, 3 and 10 to 12 abort inside Apple on _limits.maxColorAttachments > 0. 0 claims only Apple1 and Common1
 * in -supportsFamily:, and that cost SpringBoard its life: MPSDevice::GetMPSDevice asks for Apple8 down to Apple2
 * (1008 .. 1002) and, with none, MPSKernel's init reports "device is not supported" through MTLReportFailure, which
 * aborts -- from the Siri orb SpringBoard builds at startup (logs/202838-run.log, seven crashes in a row). 1 claims
 * Apple1, Apple2 and Common1 (measured: the line at the first queue), the least that MPS takes; the render test passes
 * as with 0, and SpringBoard then finishes its startup and stays up (logs/203759-run.log). What Apple2 promises beyond
 * Apple1 that is not served -- ASTC, say -- is refused where it is asked for, by name, as everything else is. */
- (NSUInteger)featureProfile { return 1; }

/* Not measured: nothing has asked the kernel what one allocation may be. It only has to be non-zero for initLimits
 * to accept it; a buffer longer than the kernel gives is refused at -newBufferWithLength:options:, with the reason. */
- (uint64_t)maxBufferLength { return 256ULL << 20; }

/* profile 0's own maxVertexAmplificationCount is 1 */
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count <= 1; }

/* No render target here is rotated: the RB writes rows as they lie, and a turned framebuffer is drawn upright and laid
 * in by the 2D engine (-newTextureWithDescriptor:iosurface:plane:). backboardd does not ask this before it makes one --
 * it set rotation 3 over the display surface with NO answered here as well -- so the answer is the truth for whoever
 * does ask, not what keeps backboardd going. */
- (BOOL)supportsRenderTargetTextureRotation { return NO; }

/* The answer that cannot be nil: see the header. A zeroed struct with an empty name is what Metal's own second cache
 * (the one for `functions`) builds for itself, which is the measurement that says this shape is acceptable; the pipeline
 * cache file then fails to open, Metal fills an NSError "Could not open pipeline cache file" and QuartzCore carries on. */
- (const struct MTLTargetDeviceArch *)targetDeviceInfo
{
	if (_target.name == NULL) {
		_target.name = _targetName;   /* an object built with plain -init: the array is zeroed, so it spells "" */
	}
	HM_ONCE("targetDeviceInfo was asked for (pipelines.%s); answered with a real struct because _MTLDevice's own "
		"answer is a NULL its caller dereferences\n", _target.name);
	return &_target;
}

- (NSString *)name { return _name != nil ? _name : @"Adreno (hakios-metal; no GPU)"; }

/* what, then the images and offsets of the calls that led here: for finding whose object a refusal or a free is */
static void
hm_print_callers(const char *what)
{
	void *frames[12];
	int n = backtrace(frames, 12);
	char line[1024];
	size_t at = (size_t)snprintf(line, sizeof(line), "%s", what);

	for (int i = 2; i < n && at < sizeof(line) - 64; i++) {
		Dl_info info;
		if (dladdr(frames[i], &info) != 0 && info.dli_fname != NULL) {
			const char *base = strrchr(info.dli_fname, '/');
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %s+%#lx", base != NULL ? base + 1 : info.dli_fname,
					       (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase));
		}
	}
	hm_print("%s\n", line);
}

/* A pipeline library (a .pipelinelib directory: pipelines.desc naming each pipeline's functions, formats and state, and
 * one file of compiled pipelines per Apple GPU) is how ActivityRingsUI -- the Fitness widget's rings -- and QuartzCore
 * build their pipelines: -newRenderPipelineStateWithName:... turns pipelines.desc into an ordinary descriptor and hands
 * it to -newRenderPipelineStateWithDescriptor:options:reflection:error:, which this plug-in serves from the functions.
 * Metal's loader will not get that far without a compiled-pipeline file for this GPU, which no library has: it fails
 * with "Could not open pipeline cache file: <path>", the widget gets nil and draws with no pipeline.
 *
 * So the library is opened again from a copy of its directory in this process's temporary directory: pipelines.desc,
 * functions and the .metallib files beside it as symbolic links, and as the missing file an empty cache that Metal's own
 * _MTLPipelineCache writes (readOnly:NO, then -write), so no byte of its format is spelled here. An empty cache answers
 * every lookup with a miss, which is the truth: nothing is precompiled for this GPU. */
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	NSError *first = nil;
	id library = [super newPipelineLibraryWithFilePath:path error:&first];
	NSString *message, *missing, *mirror, *parent;
	NSFileManager *fm = [NSFileManager defaultManager];
	NSRange at;
	Class cacheClass = NSClassFromString(@"_MTLPipelineCache");
	id cache;

	if (library != nil || path == nil) {
		if (error != NULL) *error = first;
		return library;
	}
	message = [first localizedDescription];
	at = message != nil ? [message rangeOfString:@"Could not open pipeline cache file: "] : NSMakeRange(NSNotFound, 0);
	if (at.location == NSNotFound || cacheClass == Nil) {
		/* no pipelines.desc is a directory that is no pipeline library (mediaserverd asks of a .bundle holding a
		 * default.metallib alone): Metal's own answer, as an Apple GPU would get, and nothing this device lacks */
		const BOOL notALibrary = message != nil && [message rangeOfString:@"pipelines.desc"].location != NSNotFound;
		hm_print("%s a pipeline library at %s: %s\n", notALibrary ? "no" : "refused:", [path fileSystemRepresentation],
			 message != nil ? [message UTF8String] : "no reason given");
		if (error != NULL) *error = first;
		return nil;
	}
	missing = [[message substringFromIndex:NSMaxRange(at)] lastPathComponent];
	parent = [path stringByDeletingLastPathComponent];
	mirror = [[NSTemporaryDirectory() stringByAppendingPathComponent:@"hakios-pipelinelib"]
		  stringByAppendingPathComponent:[NSString stringWithFormat:@"%lx", (unsigned long)[path hash]]];
	NSString *frame = [mirror stringByAppendingPathComponent:[parent lastPathComponent]];
	NSString *copy = [frame stringByAppendingPathComponent:[path lastPathComponent]];
	[fm createDirectoryAtPath:copy withIntermediateDirectories:YES attributes:nil error:NULL];
	/* the library's own files, and the libraries its descriptions name, which are found beside it */
	for (NSString *entry in [fm contentsOfDirectoryAtPath:path error:NULL]) {
		if ([entry isEqualToString:@"pipelines.desc"] || [entry isEqualToString:@"functions"]) {
			[fm createSymbolicLinkAtPath:[copy stringByAppendingPathComponent:entry]
				 withDestinationPath:[path stringByAppendingPathComponent:entry] error:NULL];
		}
	}
	for (NSString *entry in [fm contentsOfDirectoryAtPath:parent error:NULL]) {
		if ([[entry pathExtension] isEqualToString:@"metallib"]) {
			[fm createSymbolicLinkAtPath:[frame stringByAppendingPathComponent:entry]
				 withDestinationPath:[parent stringByAppendingPathComponent:entry] error:NULL];
		}
	}
	/* QuartzCore's of this image (the iPhone X's) has no default.metallib beside it; the one made of its own functions
	 * file (hakios: merge_metallib.py, the four uber shaders, one variant each -- hm_bitcode_for() picks the variant a
	 * pipeline needs) stands in for it, from where the image puts it */
	NSString *ours = @"/redwood/share/ca-share/QuartzCore-default.metallib";
	NSString *beside = [frame stringByAppendingPathComponent:@"default.metallib"];
	if ([path rangeOfString:@"QuartzCore.framework"].location != NSNotFound && ![fm fileExistsAtPath:beside] &&
	    [fm fileExistsAtPath:ours]) {
		[fm createSymbolicLinkAtPath:beside withDestinationPath:ours error:NULL];
		hm_print("QuartzCore's pipeline library is given %s as its default.metallib\n", [ours fileSystemRepresentation]);
	}
	NSString *cachePath = [copy stringByAppendingPathComponent:missing];
	if (![fm fileExistsAtPath:cachePath]) {
		cache = ((id (*)(id, SEL, NSString *, BOOL, const struct MTLTargetDeviceArch *))objc_msgSend)(
			[cacheClass alloc], sel_registerName("initWithFilePath:readOnly:deviceInfo:"), cachePath, NO, [self targetDeviceInfo]);
		if (cache != nil && [cache respondsToSelector:sel_registerName("write")]) {
			((void (*)(id, SEL))objc_msgSend)(cache, sel_registerName("write"));
		}
		[cache release];
	}
	NSError *second = nil;
	library = [super newPipelineLibraryWithFilePath:copy error:&second];
	hm_print("a pipeline library at %s had no %s; opened through %s with an empty cache Metal wrote: %s\n",
		 [path fileSystemRepresentation], [missing UTF8String], [copy fileSystemRepresentation],
		 library != nil ? "it opened" : (second != nil ? [[second localizedDescription] UTF8String] : "nil, no reason given"));
	if (error != NULL) *error = library != nil ? nil : (second != nil ? second : first);
	return library;
}

- (uint64_t)registryID
{
	uint64_t entry = 0;

	if (_acceleratorService != IO_OBJECT_NULL && IORegistryEntryGetRegistryEntryID(_acceleratorService, &entry) == KERN_SUCCESS) {
		return entry;
	}
	return 0;
}

/* true of this SoC: the GPU reads the same DRAM the processor does, and every buffer here is mapped on both sides */
- (BOOL)hasUnifiedMemory { return YES; }

/* the GPU memory this device has allocated and not given back (hakios-metal counts every buffer it asks the kernel
 * for); Resident Evil 4 sizes what it keeps resident by it */
- (NSUInteger)currentAllocatedSize
{
	return _gpu != NULL ? (NSUInteger)hakios_metal_device_allocated_bytes(_gpu) : 0;
}

/* 1024 is the A6xx threadgroup limit and the NDRANGE fields' width (hakios-metal's compute::kMaxLocalThreads); each
 * dimension alone may be that large, the product may not, and the dispatch refuses one that is, by name */
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 1024); }

/* --- command queues --- */

/* Apple's own -newCommandQueue and -newCommandQueueWithMaxCommandBufferCount: build a descriptor and end here. The
 * base class validates the descriptor and refuses nil, so a direct call with nil gets one made for it. The descriptor
 * class is private in iOS 16.5 (the SDK only forward-declares it), so it is taken from the runtime by name. */
- (id)newCommandQueueWithDescriptor:(id)descriptor
{
	{
		/* what the profile makes the base class claim, once: the families an MPS or a CoreAnimation decides by (asked at the
		 * first queue, not in init: the base class answers from limits MTLAddDevice has not filled yet there -- a NULL read) */
		static const NSInteger families[] = { 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 2001, 2002, 3001, 3002, 3003, 5001 };
		char yes[160] = "";

		for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
			if ([(id<MTLDevice>)self supportsFamily:(MTLGPUFamily)families[i]]) {
				char one[8];

				snprintf(one, sizeof(one), " %ld", (long)families[i]);
				strlcat(yes, one, sizeof(yes));
			}
		}
		HM_ONCE("feature profile %lu: supportsFamily answers YES for%s; maxColorAttachments-limited features follow it\n",
			(unsigned long)[self featureProfile], yes[0] ? yes : " none");
	}
	id use = descriptor;

	if (use == nil) {
		Class cls = objc_getClass("MTLCommandQueueDescriptor");

		use = cls != Nil ? [[[cls alloc] init] autorelease] : nil;
		if (use == nil) {
			hm_print("refused: a command queue: no MTLCommandQueueDescriptor class in this Metal to build one from\n");
			return nil;
		}
	}
	return [[HakiosMetalCommandQueue alloc] initWithHakiosDevice:self descriptor:use];
}

- (id)newCommandQueue { return [self newCommandQueueWithDescriptor:nil]; }

- (id)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count
{
	Class cls = objc_getClass("MTLCommandQueueDescriptor");
	id descriptor = cls != Nil ? [[[cls alloc] init] autorelease] : nil;

	if (descriptor != nil && [descriptor respondsToSelector:@selector(setMaxCommandBufferCount:)]) {
		((void (*)(id, SEL, NSUInteger))objc_msgSend)(descriptor, @selector(setMaxCommandBufferCount:), count);
	}
	return [self newCommandQueueWithDescriptor:descriptor];
}

/* --- buffers --- */

/* every allocation here is one kind of memory -- shared, write-combined -- and the buffer says so; the options are
 * read for what cannot be given, so that a request for private storage is not silently given something else */
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	if ((options & MTLResourceStorageModeMask) == MTLResourceStorageModePrivate) {
		HM_ONCE("a buffer was asked for with private storage; every buffer here is shared memory, and that is what "
			"is given (the GPU reads the same pages the processor maps)\n");
	}
	return [[HakiosMetalBuffer alloc] initWithHakiosDevice:self length:length];
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
{
	HakiosMetalBuffer *buffer = [self newBufferWithLength:length options:options];

	if (buffer != nil && bytes != NULL && [buffer contents] != NULL) {
		memcpy([buffer contents], bytes, length);
	}
	return buffer;
}

/* The GPU's memory is the kernel's allocation and nothing else: an application's own pages cannot be handed to it as
 * they are. So the bytes are copied into GPU memory as the buffer is made, and the deallocator is kept and called when
 * the buffer goes, as Metal promises. What the copy loses is the sharing: a write by the GPU is not seen in the caller's
 * pages, nor a later write by the caller in the GPU's. QuartzCore's one caller (CA::OGL::MetalContext::
 * copy_image_to_texture) wraps a decoded image to blit it into a texture at once, which loses neither; refusing it had
 * left those textures as whatever memory they were given. */
- (id)newBufferWithBytesNoCopy:(void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	HakiosMetalBuffer *buffer = [self newBufferWithBytes:bytes length:length options:options];

	HM_ONCE("a no-copy buffer was asked for; its bytes are copied into GPU memory, so neither side sees the other's later "
		"writes\n");
	if (buffer != nil && deallocator != nil) {
		[buffer hakiosSetDeallocator:deallocator bytes:bytes length:length];
	}
	return buffer;
}

/* the layout of a texture's bytes as -newTextureWithBytesNoCopy: here reads them (HakiosMetalTextureLayout says what) */
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor isHeapOrBufferBacked:(BOOL)backed
{
	unsigned bw, bh, bytes;

	(void)backed;
	if (descriptor == nil || !hakios_metal_format_block((unsigned)[descriptor pixelFormat], &bw, &bh, &bytes) ||
	    ([descriptor textureType] != MTLTextureType2D && [descriptor textureType] != MTLTextureType1D) || [descriptor arrayLength] != 1 ||
	    [descriptor depth] != 1) {
		hm_print("refused: a texture layout for pixel format %lu, type %lu: a 1D or 2D texture of a format this device serves\n",
			 (unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor textureType]);
		return nil;
	}
	return [[HakiosMetalTextureLayout alloc] initWithDescriptor:descriptor blockW:bw blockH:bh bytes:bytes];
}

/* A texture over the caller's bytes (QuartzCore's, for an image it already holds in memory): copied into a texture of
 * the GPU's own, as a no-copy buffer is, with the rows LENGTH / height bytes apart -- the one layout the call can mean
 * without a bytesPerRow of its own. The caller's bytes are handed back to its deallocator when the texture goes. */
- (id)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length descriptor:(MTLTextureDescriptor *)descriptor
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	HakiosMetalTexture *texture = [self newTextureWithDescriptor:descriptor];
	NSUInteger height = [descriptor height], width = [descriptor width];
	NSUInteger pitch = height != 0 ? length / height : 0;
	unsigned bw = 1, bh = 1, bytesPer = 0;

	if (texture == nil) {
		return nil;   /* refused by name in -newTextureWithDescriptor: */
	}
	/* a compressed format, or levels: the bytes are in the layout -newTextureLayoutWithDescriptor: described (each level
	 * in turn, rows of blocks packed tight), as ImageIO staged them through it */
	if (hakios_metal_format_block((unsigned)[descriptor pixelFormat], &bw, &bh, &bytesPer) && (bw > 1 || bh > 1 || [descriptor mipmapLevelCount] > 1)) {
		const NSUInteger levels = MAX([descriptor mipmapLevelCount], (NSUInteger)1);
		NSUInteger offset, row, rows;

		hm_tight_level(width, height, levels, bw, bh, bytesPer, levels, &offset, &row, &rows);
		if (bytes == NULL || length < offset) {
			hm_print("refused: a no-copy %s texture of %lux%lu, %lu levels over %lu bytes: its layout takes %lu\n", bw > 1 ? "compressed" : "mipmapped",
				 (unsigned long)width, (unsigned long)height, (unsigned long)levels, (unsigned long)length, (unsigned long)offset);
			[texture release];
			return nil;
		}
		for (NSUInteger l = 0; l < levels; l++) {
			hm_tight_level(width, height, levels, bw, bh, bytesPer, l, &offset, &row, &rows);
			[(id<MTLTexture>)texture replaceRegion:MTLRegionMake2D(0, 0, MAX(width >> l, (NSUInteger)1), MAX(height >> l, (NSUInteger)1))
						   mipmapLevel:l withBytes:(const uint8_t *)bytes + offset bytesPerRow:row];
		}
		if (deallocator != nil) {
			[texture hakiosSetDeallocator:deallocator bytes:bytes length:length];
		}
		return texture;
	}
	if (bytes == NULL || pitch < width * hakios_metal_texture_bytes_per_pixel([texture hakiosTexture])) {
		hm_print("refused: a no-copy texture of %lux%lu over %lu bytes: rows of %lu bytes do not hold the width\n",
			 (unsigned long)width, (unsigned long)height, (unsigned long)length, (unsigned long)pitch);
		[texture release];
		return nil;
	}
	HM_ONCE("a no-copy texture was asked for; its bytes are copied into GPU memory, so neither side sees the other's later writes\n");
	[(id<MTLTexture>)texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:bytes bytesPerRow:pitch];
	if (deallocator != nil) {
		[texture hakiosSetDeallocator:deallocator bytes:bytes length:length];
	}
	return texture;
}

/* --- pipeline states: where hakios-metal's compiler is asked to work --- */

/* The nil this device returns, with the sentence that goes with it: an NSError for the caller and a printed line for the
 * log, because a caller that swallows the NSError (QuartzCore does) would otherwise swallow the reason with it. */
/* The values a pipeline's function was specialised with (-[MTLLibrary newFunctionWithName:constantValues:error:]), as
 * hakios-metal's compiler takes them: each constant the function declares (functionConstantsDictionary: name to
 * MTLFunctionConstant, its index and type) with the value -[_MTLFunctionInternal baseFunctionConstantValues] holds
 * for it (-constantValueWithFunctionConstant:, a pointer to the value's bytes, or NULL when none was given). The bytes
 * go into the 64-bit word as they lie, which is the component-at-i-times-width packing the C interface asks for; a
 * constant the function declares and nobody set is left out, which tells the front end it is not defined. *COUNT
 * the number written, at most MAX. Read off this Metal's own classes by selector (Metal.framework's class dump):
 * nothing of their layout is assumed. */
static unsigned
hm_data_type_bytes(MTLDataType t)
{
	switch (t) {
	case MTLDataTypeBool: case MTLDataTypeChar: case MTLDataTypeUChar: return 1;
	case MTLDataTypeBool2: case MTLDataTypeChar2: case MTLDataTypeUChar2: case MTLDataTypeShort: case MTLDataTypeUShort: case MTLDataTypeHalf: return 2;
	case MTLDataTypeBool3: case MTLDataTypeChar3: case MTLDataTypeUChar3: return 3;
	case MTLDataTypeBool4: case MTLDataTypeChar4: case MTLDataTypeUChar4: case MTLDataTypeShort2: case MTLDataTypeUShort2: case MTLDataTypeHalf2:
	case MTLDataTypeInt: case MTLDataTypeUInt: case MTLDataTypeFloat: return 4;
	case MTLDataTypeShort3: case MTLDataTypeUShort3: case MTLDataTypeHalf3: return 6;
	case MTLDataTypeShort4: case MTLDataTypeUShort4: case MTLDataTypeHalf4: case MTLDataTypeInt2: case MTLDataTypeUInt2: case MTLDataTypeFloat2:
	case MTLDataTypeLong: case MTLDataTypeULong: return 8;
	default: return 0;   /* wider than the word the front end takes: left out, and the module refuses what it cannot fold */
	}
}

/* A specialised function's values as Metal hands them to its compiler: -[_MTLFunctionInternal specializationAirScript],
 * a FlatBuffers buffer with the file identifier "AIRC" (MTLFunctionConstantValuesInternal's newConstantScriptForFunction:
 * makes it). Read off the bytes of CoreAnimation's own functions on the phone, not a published schema: the root's
 * third field a table whose first field is the function's name and whose third a vector of tables, one per constant,
 * each with the constant's index (u16, field 0, absent for 0), its MTLDataType (a byte, field 1) and, at the forward
 * offset in field 2, the value's bytes. fixed_frag_lph_cph's 58 entries read as plausible values throughout -- every
 * bool 0 or 1, fc_attachment_count 2 where the pass has two targets, fc_framebuffer_fetch 1 where it reads one -- and
 * that is the whole of the evidence. Every offset is checked against the size; a buffer that does not hold together
 * gives no constants, and says so. */
static unsigned hm_function_constants_declared(id function, hakios_metal_constant *out, unsigned max);
static void hm_note_constants(const void *program, NSString *constants);
static BOOL hm_tracing(void);

static unsigned
hm_script_constants(const uint8_t *b, size_t size, hakios_metal_constant *out, unsigned max)
{
#define HM_FITS(o, n) ((uint64_t)(o) + (n) <= size)
	uint32_t root, sub, vec, n, count = 0;

	if (size < 8 || memcmp(b + 4, "AIRC", 4) != 0) {
		return 0;
	}
	/* field K of the table at T: its absolute position, or 0 when absent */
	uint64_t (^field)(uint64_t, unsigned) = ^uint64_t(uint64_t t, unsigned k) {
		int32_t so;
		uint64_t vt;
		uint16_t vlen, off;

		if (!HM_FITS(t, 4)) return 0;
		memcpy(&so, b + t, 4);
		vt = t - (int64_t)so;
		if (!HM_FITS(vt, 4)) return 0;
		memcpy(&vlen, b + vt, 2);
		if (4 + 2 * k + 2 > vlen || !HM_FITS(vt + 4 + 2 * k, 2)) return 0;
		memcpy(&off, b + vt + 4 + 2 * k, 2);
		return off != 0 ? t + off : 0;
	};
	/* the forward offset stored at position P, followed */
	uint64_t (^follow)(uint64_t) = ^uint64_t(uint64_t p) {
		uint32_t o;

		if (p == 0 || !HM_FITS(p, 4)) return 0;
		memcpy(&o, b + p, 4);
		return HM_FITS(p + o, 1) ? p + o : 0;
	};
	memcpy(&root, b, 4);
	sub = (uint32_t)follow(field(root, 2));
	vec = sub != 0 ? (uint32_t)follow(field(sub, 2)) : 0;
	if (vec == 0 || !HM_FITS(vec, 4)) {
		HM_ONCE("a specialization script that does not hold together (%zu bytes): its function is compiled with no constants\n", size);
		return 0;
	}
	memcpy(&n, b + vec, 4);
	for (uint32_t e = 0; e < n && count < max && HM_FITS(vec + 4 + 4 * e, 4); e++) {
		uint64_t et = follow(vec + 4 + 4 * e), fi, ft, fv;
		uint16_t index = 0;
		unsigned bytes;

		if (et == 0) break;
		fi = field(et, 0);
		ft = field(et, 1);
		fv = follow(field(et, 2));
		if (ft == 0 || fv == 0) continue;
		if (fi != 0 && HM_FITS(fi, 2)) memcpy(&index, b + fi, 2);
		bytes = hm_data_type_bytes((MTLDataType)b[ft]);
		if (bytes == 0 || !HM_FITS(fv, bytes)) continue;
		out[count].index = index;
		out[count].bits = 0;
		memcpy(&out[count].bits, b + fv, bytes);
		count++;
	}
	return count;
#undef HM_FITS
}

static unsigned
hm_function_constants(id function, hakios_metal_constant *out, unsigned max)
{
	SEL scriptSel = sel_getUid("specializationAirScript");

	if (function != nil && [function respondsToSelector:scriptSel]) {
		dispatch_data_t script = ((dispatch_data_t(*)(id, SEL))objc_msgSend)(function, scriptSel);

		if (script != nil) {
			const void *bytes = NULL;
			size_t size = 0;
			dispatch_data_t map = dispatch_data_create_map(script, &bytes, &size);
			unsigned n = hm_script_constants(bytes, size, out, max);

			dispatch_release(map);
			HM_ONCE("function constants: %s was specialised by a script of %zu bytes giving %u values (the first such function)\n",
				[[function name] UTF8String], size, n);
			return n;
		}
	}
	return hm_function_constants_declared(function, out, max);
}

static unsigned
hm_function_constants_declared(id function, hakios_metal_constant *out, unsigned max)
{
	SEL baseSel = sel_getUid("baseFunctionConstantValues"), valueSel = sel_getUid("constantValueWithFunctionConstant:");
	NSDictionary *declared;
	id values;
	unsigned n = 0;

	if (function == nil || ![function respondsToSelector:@selector(functionConstantsDictionary)] || ![function respondsToSelector:baseSel]) {
		return 0;
	}
	declared = [function functionConstantsDictionary];
	values = ((id(*)(id, SEL))objc_msgSend)(function, baseSel);
	if ([declared count] == 0 || values == nil || ![values respondsToSelector:valueSel]) {
		return 0;
	}
	for (NSString *name in declared) {
		MTLFunctionConstant *c = [declared objectForKey:name];
		const void *v = ((const void *(*)(id, SEL, id))objc_msgSend)(values, valueSel, c);
		unsigned bytes = hm_data_type_bytes([c type]);

		if (v == NULL || bytes == 0 || n >= max) {
			continue;
		}
		out[n].index = (unsigned)[c index];
		out[n].bits = 0;
		memcpy(&out[n].bits, v, bytes);
		n++;
	}
	HM_ONCE("function constants: %s declares %lu and was given %u of them (the first function seen with constants)\n",
		[[function name] UTF8String], (unsigned long)[declared count], n);
	return n;
}

- (id)hakiosRefuse:(NSError **)error what:(const char *)what why:(NSString *)why
{
	hm_print("refused: %s: %s\n", what, [why UTF8String]);
	if (error != NULL) {
		*error = [NSError errorWithDomain:@"HakiosMetal" code:1 userInfo:@{ NSLocalizedDescriptionKey: why }];
	}
	return nil;
}

- (id)hakiosPipelineForFunction:(id)function descriptor:(id)descriptor error:(NSError **)error
{
	SEL bitcode = sel_getUid("bitcodeData");
	NSData *air;
	hakios_metal_kernel *kernel;

	if (function == nil) {
		return [self hakiosRefuse:error what:"a compute pipeline" why:@"the descriptor names no compute function"];
	}
	if (!hakios_metal_has_compiler()) {
		return [self hakiosRefuse:error what:"a compute pipeline"
				      why:@"libhakios-metal was built without the compiler (make ios MESA=1)"];
	}
	if (![function respondsToSelector:bitcode]) {
		return [self hakiosRefuse:error what:"a compute pipeline"
				      why:@"the function does not answer bitcodeData, so its AIR cannot be read"];
	}
	air = hm_bitcode_for(function, nil, NO);
	if (air == nil || [air length] == 0) {
		return [self hakiosRefuse:error what:"a compute pipeline" why:@"the function has no bitcode"];
	}
	{
		hakios_metal_constant constants[64];
		unsigned count = hm_function_constants(function, constants, 64);

		kernel = hakios_metal_kernel_compile_bitcode_constants([air bytes], [air length], constants, count);
		if (kernel != NULL) {
			hm_dump_function("cs", function, "metal_dump=fn", constants, count);
		}
	}
	if (kernel == NULL) {
		/* the compiler's own sentence, passed through: it names the construct it refused */
		hm_dump_refused("cs", function);
		return [self hakiosRefuse:error what:"a compute pipeline"
				      why:[NSString stringWithFormat:@"%@: %s", [function name], hakios_metal_error()]];
	}
	hm_print("compiled %s: %u resources, %u threads a threadgroup at most, execution width %u, %u bytes of threadgroup memory\n",
		 hakios_metal_kernel_name(kernel), hakios_metal_kernel_resource_count(kernel), hakios_metal_kernel_max_threads(kernel),
		 hakios_metal_kernel_execution_width(kernel), hakios_metal_kernel_threadgroup_memory(kernel));
	return [[HakiosMetalComputePipelineState alloc] initWithHakiosDevice:self kernel:kernel descriptor:descriptor];
}

/* The base class demands a descriptor; Apple's own -newComputePipelineStateWithFunction: builds a
 * MTLComputePipelineDescriptorInternal, so that class is used when it is there and the public one otherwise. */
- (id)newComputePipelineStateWithFunction:(id)function error:(NSError **)error
{
	Class internal = objc_getClass("MTLComputePipelineDescriptorInternal");
	id descriptor = internal != Nil ? [[internal alloc] init] : (id)[[MTLComputePipelineDescriptor alloc] init];
	id state;

	if (descriptor == nil) {
		return [self hakiosRefuse:error what:"a compute pipeline" why:@"no pipeline descriptor class to build one from"];
	}
	[descriptor setComputeFunction:function];
	state = [self hakiosPipelineForFunction:function descriptor:descriptor error:error];
	[descriptor release];
	return state;
}

- (id)newComputePipelineStateWithFunction:(id)function options:(MTLPipelineOption)options
			       reflection:(MTLComputePipelineReflection **)reflection error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;   /* reflection comes out of Metal's compiler, which this device has not got */
	}
	return [self newComputePipelineStateWithFunction:function error:error];
}

- (void)newComputePipelineStateWithFunction:(id)function completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler
{
	NSError *error = nil;
	id state = [self newComputePipelineStateWithFunction:function error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newComputePipelineStateWithFunction:(id)function options:(MTLPipelineOption)options
			  completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	MTLComputePipelineReflection *reflection = nil;
	id state = [self newComputePipelineStateWithFunction:function options:options reflection:&reflection error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
				 reflection:(MTLComputePipelineReflection **)reflection error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self hakiosPipelineForFunction:[descriptor computeFunction] descriptor:descriptor error:error];
}

- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor error:(NSError **)error
{
	return [self hakiosPipelineForFunction:[descriptor computeFunction] descriptor:descriptor error:error];
}

- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
			    completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	MTLComputePipelineReflection *reflection = nil;
	id state = [self newComputePipelineStateWithDescriptor:descriptor options:options reflection:&reflection error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

/* --- render pipeline states: a vertex and a fragment function linked for one colour attachment --- */

/* Metal's buffer tables have 31 entries (profile 0's maxVertexBuffers) and a vertex descriptor as many attribute and
 * layout slots; the SDK's MTLVertexDescriptor arrays are indexed up to that. */
#define HM_MAX_VERTEX_ATTRIBUTES 31

/* A vertex descriptor that fetches nothing: nil, or one whose attributes are all MTLVertexFormatInvalid and whose layouts
 * all have stride 0 (what -[MTLVertexDescriptor init] gives). Metal hands the pipeline a copy of whatever the application
 * set, so "no vertex descriptor" arrives in either shape. Anything else names a vertex fetch ([[stage_in]] attributes
 * through VFD_DECODE/VFD_FETCH); the first such attribute is named, for the log line that says a pipeline fetches. */
static NSString *
hm_vertex_descriptor_fetch(MTLVertexDescriptor *vd)
{
	if (vd == nil) {
		return nil;
	}
	for (NSUInteger i = 0; i < HM_MAX_VERTEX_ATTRIBUTES; i++) {
		MTLVertexAttributeDescriptor *a = [[vd attributes] objectAtIndexedSubscript:i];
		MTLVertexBufferLayoutDescriptor *l = [[vd layouts] objectAtIndexedSubscript:i];

		if (a != nil && [a format] != MTLVertexFormatInvalid) {
			return [NSString stringWithFormat:@"a vertex descriptor with attribute %lu (format %lu, offset %lu, buffer %lu)",
							  (unsigned long)i, (unsigned long)[a format], (unsigned long)[a offset], (unsigned long)[a bufferIndex]];
		}
		if (l != nil && [l stride] != 0) {
			return [NSString stringWithFormat:@"a vertex descriptor with layout %lu (stride %lu)", (unsigned long)i, (unsigned long)[l stride]];
		}
	}
	return nil;
}

/* The descriptor's attributes and layouts, Metal's numbers straight through (hakios_metal.h): every slot, described or
 * not, so that a pipeline state made from a reused descriptor carries no stale attribute. NO with the core's sentence. */
static BOOL
hm_set_vertex_descriptor(hakios_metal_program *program, MTLVertexDescriptor *vd)
{
	for (unsigned i = 0; i < HM_MAX_VERTEX_ATTRIBUTES; i++) {
		MTLVertexAttributeDescriptor *a = vd != nil ? [[vd attributes] objectAtIndexedSubscript:i] : nil;
		MTLVertexBufferLayoutDescriptor *l = vd != nil ? [[vd layouts] objectAtIndexedSubscript:i] : nil;

		if (hakios_metal_program_set_vertex_attribute(program, i, a != nil ? (unsigned)[a format] : 0, a != nil ? (unsigned)[a offset] : 0,
							      a != nil ? (unsigned)[a bufferIndex] : 0) != 0 ||
		    hakios_metal_program_set_vertex_layout(program, i, l != nil ? (unsigned)[l stride] : 0,
							   l != nil ? (unsigned)[l stepFunction] : 1, l != nil ? (unsigned)[l stepRate] : 1) != 0) {
			return NO;
		}
	}
	return YES;
}

/*
 * Why a render pipeline descriptor cannot be served, or nil. Everything the base initialiser does not read is read here,
 * by name, BEFORE any compilation and before super is given the descriptor: the step-one contract (hakios_metal.h) is one
 * colour attachment of a served format, no depth, no stencil, one sample, no dual-source blend, and a vertex
 * plus a fragment function; a descriptor asking for more gets nil with the sentence that names the first thing it asked
 * for, rather than a pipeline that silently draws without it. Each check is one a client can trip on purpose (the test
 * program's blendingEnabled line is one of them).
 */
- (NSString *)hakiosRenderPipelineWhy:(MTLRenderPipelineDescriptor *)d
{

	if (d == nil || ![d isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
		return [NSString stringWithFormat:@"the descriptor is %s, not an MTLRenderPipelineDescriptor (Metal's own init would abort on it)",
						  d != nil ? class_getName(object_getClass(d)) : "nil"];
	}
	if ([d vertexFunction] == nil) {
		return @"the descriptor names no vertex function";
	}
	if ([[d vertexFunction] functionType] != MTLFunctionTypeVertex) {
		return [NSString stringWithFormat:@"the vertex function %@ is of type %lu, not a vertex function (1)", [[d vertexFunction] name],
						  (unsigned long)[[d vertexFunction] functionType]];
	}
	/* no fragment function is a depth-only pipeline: hakios-metal draws a built-in one that writes nothing */
	if ([d fragmentFunction] != nil && [[d fragmentFunction] functionType] != MTLFunctionTypeFragment) {
		return [NSString stringWithFormat:@"the fragment function %@ is of type %lu, not a fragment function (2)", [[d fragmentFunction] name],
						  (unsigned long)[[d fragmentFunction] functionType]];
	}
	/* colour attachment 0 may be Invalid: a pipeline drawing depth alone (a shadow map's), or one whose first target is a
	 * later index -- its [[color(0)]], if the function writes one, goes nowhere, as Metal discards it */
	/* Attachments past 0 are hakios-metal's to judge at the draw: one the fragment function writes ([[color(n)]]) is
	 * drawn into, its output's class against the attachment's format; one it does not write -- QuartzCore's
	 * downsample_4_frag_lph pass names a second target and writes colour 0 alone -- is left unwritten, as Metal leaves
	 * an output the function does not write. Their blends are handed over after the compile. */
	/* blending and the write mask are hakios-metal's (hakios_metal_program_set_blend after the compile), dual-source
	 * factors included: the draw takes a stage's [[color(0), index(1)]] as the second source */
	if ([d depthAttachmentPixelFormat] != MTLPixelFormatInvalid && [d depthAttachmentPixelFormat] != MTLPixelFormatDepth32Float &&
	    [d depthAttachmentPixelFormat] != MTLPixelFormatDepth32Float_Stencil8) {
		return [NSString stringWithFormat:@"a depth attachment pixel format of %lu; Depth32Float (252) and Depth32Float_Stencil8 "
						  "(260) are the depth formats served", (unsigned long)[d depthAttachmentPixelFormat]];
	}
	/* the stencil is the S8 plane of a Depth32Float_Stencil8 texture, so a stencil format is that one, alone or with the
	 * same depth format beside it */
	if ([d stencilAttachmentPixelFormat] != MTLPixelFormatInvalid && [d stencilAttachmentPixelFormat] != MTLPixelFormatDepth32Float_Stencil8) {
		return [NSString stringWithFormat:@"a stencil attachment pixel format of %lu; Depth32Float_Stencil8 (260) is the stencil "
						  "format served", (unsigned long)[d stencilAttachmentPixelFormat]];
	}
	if ([d rasterSampleCount] != 1 && [d rasterSampleCount] != 2 && [d rasterSampleCount] != 4) {
		return [NSString stringWithFormat:@"rasterSampleCount %lu; 1, 2 and 4 samples are served", (unsigned long)[d rasterSampleCount]];
	}
	/* the vertex descriptor is not judged here: it is handed to hakios-metal after the compile (hm_set_vertex_descriptor),
	 * which knows which attributes the vertex function reads and refuses an unserved one at the draw, by name */
	if ([d isAlphaToCoverageEnabled] || [d isAlphaToOneEnabled]) {
		return @"alphaToCoverage or alphaToOne is enabled; neither is served (no MSAA, no blend state)";
	}
	if (![d isRasterizationEnabled]) {
		return @"rasterizationEnabled is NO; a pipeline that rasterizes nothing has nothing to draw here (no stream-out)";
	}
	if ([d inputPrimitiveTopology] != MTLPrimitiveTopologyClassUnspecified && [d inputPrimitiveTopology] != MTLPrimitiveTopologyClassTriangle) {
		return [NSString stringWithFormat:@"input primitive topology class %lu; triangles (3, or unspecified 0) are what step one draws",
						  (unsigned long)[d inputPrimitiveTopology]];
	}
	if ([d maxVertexAmplificationCount] > 1) {
		return [NSString stringWithFormat:@"maxVertexAmplificationCount %lu; profile 0 amplifies nothing (1)", (unsigned long)[d maxVertexAmplificationCount]];
	}
	if ([[[d vertexLinkedFunctions] functions] count] != 0 || [[[d vertexLinkedFunctions] binaryFunctions] count] != 0 ||
	    [[[d fragmentLinkedFunctions] functions] count] != 0 || [[[d fragmentLinkedFunctions] binaryFunctions] count] != 0) {
		return @"linked functions (dynamic linking, visible functions) are not served; each stage is one module compiled whole";
	}
	if ([[d vertexPreloadedLibraries] count] != 0 || [[d fragmentPreloadedLibraries] count] != 0) {
		return @"preloaded dynamic libraries are not served";
	}
	if ([d supportIndirectCommandBuffers]) {
		HM_ONCE("a render pipeline was asked to support indirect command buffers; the flag is kept by Metal's base class, and the "
			"encoder refuses -executeCommandsInBuffer: by name\n");
	}
	if ([[d binaryArchives] count] != 0) {
		HM_ONCE("a render pipeline descriptor carries %lu binary archives; none is consulted, the AIR is compiled here\n",
			(unsigned long)[[d binaryArchives] count]);
	}
	return nil;
}

/* What a function a pipeline is handed is made of, once per class: its class chain with each class's ivars, and the
 * public functionConstantsDictionary. Which of them carries the values a pipeline specialised the function with
 * (-[MTLLibrary newFunctionWithName:constantValues:error:]) is not known from the SDK, and the compiler here takes
 * every constant as unsupplied until it is: this is the reading that says where they are. */
static void
hm_describe_function(id f)
{
	static Class said[8];
	Class c = f != nil ? object_getClass(f) : Nil;

	if (c == Nil) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		if (said[i] == c) {
			return;
		}
		if (said[i] == Nil) {
			said[i] = c;
			break;
		}
	}
	for (Class k = c; k != Nil && k != [NSObject class]; k = class_getSuperclass(k)) {
		unsigned n = 0;
		Ivar *ivars = class_copyIvarList(k, &n);
		char names[768] = "";

		for (unsigned i = 0; i < n; i++) {
			strlcat(names, i ? ", " : "", sizeof(names));
			strlcat(names, ivar_getName(ivars[i]), sizeof(names));
		}
		free(ivars);
		hm_print("a pipeline's function %s: class %s, ivars: %s\n", [[f name] UTF8String], class_getName(k), names);
	}
	if ([f respondsToSelector:@selector(functionConstantsDictionary)]) {
		NSDictionary *fc = [f functionConstantsDictionary];

		hm_print("  ... functionConstantsDictionary: %lu entries%s%s\n", (unsigned long)[fc count], [fc count] ? ", e.g. " : "",
			 [fc count] ? [[[[fc allKeys] firstObject] description] UTF8String] : "");
	}
}

/*
 * The one entry point every form below ends in. The order is: the descriptor's own rules (above), then the compiler
 * (hakios_metal_program_new_bitcode: AIR to NIR to ir3 for both stages, the link, the format against the fragment
 * output's class -- its refusals name the construct), then Metal's base initialiser with a descriptor already known to
 * pass its asserts. Nothing is asked of Metal's -compiler, which this device has not got.
 */
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error
{
	SEL bitcode = sel_getUid("bitcodeData");
	NSString *why = [self hakiosRenderPipelineWhy:descriptor];
	id vs, fs;
	NSData *vsAir, *fsAir;
	hakios_metal_program *program;
	unsigned format;

	hm_describe_function([descriptor fragmentFunction]);
	if (why != nil) {
		return [self hakiosRefuse:error what:"a render pipeline" why:why];
	}
	if (!hakios_metal_has_compiler()) {
		return [self hakiosRefuse:error what:"a render pipeline" why:@"libhakios-metal was built without the compiler (make ios MESA=1)"];
	}
	vs = [descriptor vertexFunction];
	fs = [descriptor fragmentFunction];
	if (![vs respondsToSelector:bitcode] || (fs != nil && ![fs respondsToSelector:bitcode])) {
		return [self hakiosRefuse:error what:"a render pipeline"
				      why:@"a function does not answer bitcodeData, so its AIR cannot be read (not an _MTLFunction of this Metal?)"];
	}
	vsAir = hm_bitcode_for(vs, [descriptor label], YES);
	fsAir = fs != nil ? hm_bitcode_for(fs, [descriptor label], NO) : nil;   /* nil: the built-in fragment function */
	if (vsAir == nil || [vsAir length] == 0 || (fs != nil && (fsAir == nil || [fsAir length] == 0))) {
		return [self hakiosRefuse:error what:"a render pipeline"
				      why:[NSString stringWithFormat:@"%@ or %@ has no bitcode", [vs name], [fs name]]];
	}
	format = (unsigned)[[[descriptor colorAttachments] objectAtIndexedSubscript:0] pixelFormat];
	{
		hakios_metal_constant vsConstants[64], fsConstants[64];
		unsigned vsCount = hm_function_constants(vs, vsConstants, 64), fsCount = hm_function_constants(fs, fsConstants, 64);

		program = hakios_metal_program_new_bitcode_constants([vsAir bytes], [vsAir length], vsConstants, vsCount, fsAir != nil ? [fsAir bytes] : NULL,
								     fsAir != nil ? [fsAir length] : 0,
								     fsConstants, fsCount, format);
		if (program != NULL) {
			hm_dump_function("vs", vs, "metal_dump=fn", vsConstants, vsCount);
			if (fs != nil) hm_dump_function("fs", fs, "metal_dump=fn", fsConstants, fsCount);
		}
		if (program == NULL) {
			/* the core's sentence, passed through: the stage and the construct it refused, or the format by name -- and the
			 * fragment function's constants, which decide what it is */
			NSMutableString *fc = [NSMutableString string];

			for (unsigned i = 0; i < fsCount; i++) {
				if (fsConstants[i].bits != 0) [fc appendFormat:@" %u=%llu", fsConstants[i].index, (unsigned long long)fsConstants[i].bits];
			}
			hm_dump_refused("vs", vs);
			hm_dump_refused("fs", fs);
			return [self hakiosRefuse:error what:"a render pipeline"
					      why:[NSString stringWithFormat:@"%@ + %@ for pixel format %u (fragment constants not 0:%@): %s", [vs name], [fs name],
							       format, fc, hakios_metal_error()]];
		}
	}
	hakios_metal_program_set_samples(program, (unsigned)[descriptor rasterSampleCount]);   /* 1, 2 or 4: judged above */
	if (!hm_set_vertex_descriptor(program, [descriptor vertexDescriptor])) {
		NSString *why2 = [NSString stringWithFormat:@"the vertex descriptor: %s", hakios_metal_error()];
		hakios_metal_program_free(program);
		return [self hakiosRefuse:error what:"a render pipeline" why:why2];
	}
	{
		MTLRenderPipelineColorAttachmentDescriptor *c0 = [[descriptor colorAttachments] objectAtIndexedSubscript:0];

		hakios_metal_program_set_blend(program, [c0 isBlendingEnabled] ? 1 : 0, (unsigned)[c0 rgbBlendOperation], (unsigned)[c0 alphaBlendOperation],
					       (unsigned)[c0 sourceRGBBlendFactor], (unsigned)[c0 destinationRGBBlendFactor],
					       (unsigned)[c0 sourceAlphaBlendFactor], (unsigned)[c0 destinationAlphaBlendFactor], (unsigned)[c0 writeMask]);
		for (unsigned i = 1; i < 8; i++) {
			MTLRenderPipelineColorAttachmentDescriptor *c = [[descriptor colorAttachments] objectAtIndexedSubscript:i];

			if (c == nil || [c pixelFormat] == MTLPixelFormatInvalid) {
				continue;
			}
			hakios_metal_program_set_blend_at(program, i, [c isBlendingEnabled] ? 1 : 0, (unsigned)[c rgbBlendOperation],
							  (unsigned)[c alphaBlendOperation], (unsigned)[c sourceRGBBlendFactor],
							  (unsigned)[c destinationRGBBlendFactor], (unsigned)[c sourceAlphaBlendFactor],
							  (unsigned)[c destinationAlphaBlendFactor], (unsigned)[c writeMask]);
		}
	}
	{
		NSString *fetches = hm_vertex_descriptor_fetch([descriptor vertexDescriptor]);

		if (fetches != nil) {
			hm_print("%s fetches through %s\n", hakios_metal_program_name(program), [fetches UTF8String]);
		}
	}
	{
		/* the fragment function's constants beside the line: one function (fixed_frag_lph_cph) is most of what
		 * CoreAnimation draws, and its constants are what tell one of its pipelines from another in a log */
		hakios_metal_constant fc[64];
		unsigned count = hm_function_constants([descriptor fragmentFunction], fc, 64);
		NSMutableString *set = [NSMutableString string];
		NSMutableString *zero = [NSMutableString string];

		for (unsigned i = 0; i < count; i++) {
			if (fc[i].bits != 0) [set appendFormat:@" %u=%llu", fc[i].index, (unsigned long long)fc[i].bits];
			else [zero appendFormat:@",%u", fc[i].index];
		}
		hm_note_constants(program, set);
		if (hm_tracing()) {
			/* the module this Metal hands over is the function already specialised (a few KB, not the library's
			 * 100), so its size and hash, not the library's, say which code a pipeline runs */
			NSData *air = hm_bitcode_for([descriptor fragmentFunction], [descriptor label], NO);
			uint32_t h = 2166136261u;
			const uint8_t *b = (const uint8_t *)[air bytes];

			for (NSUInteger i = 0; i < [air length]; i++) h = (h ^ b[i]) * 16777619u;
			hm_print("trace: %s (%p) defines %u constants; the zero ones%s; fragment bitcode %lu bytes, fnv %08x\n", hakios_metal_program_name(program),
				 (void *)program, count, [zero UTF8String], (unsigned long)[air length], h);
			/* boot-arg metal_dump=FNV (a diagnostic): that module's bytes into the log in hex, for the host's own
			 * front end and compiler to take apart. What it prints is the system's code: it stays in the log. */
			{
				char args[1024] = {0}, *at;
				size_t size = sizeof(args) - 1;

				if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_dump=")) != NULL &&
				    (uint32_t)strtoul(at + 11, NULL, 16) == h) {
					for (NSUInteger o = 0; o < [air length]; o += 64) {
						char line[160];
						size_t n = 0;

						for (NSUInteger i = o; i < o + 64 && i < [air length]; i++) n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x", b[i]);
						hm_print("dump %08x %06lx %s\n", h, (unsigned long)o, line);
					}
				}
			}
		}
		hm_print("linked %s (%p) for pixel format %u: the vertex stage binds %u resources, the fragment stage %u; fragment constants not 0:%s\n",
			 hakios_metal_program_name(program), (void *)program, format, hakios_metal_program_resource_count(program, 0),
			 hakios_metal_program_resource_count(program, 1), [set UTF8String]);
	}
	{
		HakiosMetalRenderPipelineState *state = [[HakiosMetalRenderPipelineState alloc] initWithHakiosDevice:self program:program descriptor:descriptor];
		hakios_metal_constant fc[64];
		unsigned count = fs != nil ? hm_function_constants(fs, fc, 64) : 0;

		if (state != nil && fsAir != nil) [state hakiosKeepFragment:fsAir constants:fc count:count];
		return state;
	}
}

/* reflection comes out of Metal's compiler, which this device has not got; nil, as for compute */
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
				reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}

/* The handler forms are answered synchronously, inside the call: Metal promises only that the handler runs, not when,
 * and the earlier plug-in measured the alternative -- a form left to the base class sends to a nil compiler, returns,
 * and the handler never runs; a client building its pipelines asynchronously then waits for ever, the one failure no
 * log shows. The state is released after the handler, which has retained it if it wants it. */
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
			   completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
			   completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor options:options reflection:NULL error:&error];

	if (handler != NULL) {
		handler(state, nil, error);
	}
	[state release];
}

/* The mesh and tile forms are _MTLDevice's own, and the mesh ones RE-ENTER the plain forms above on self with a render
 * descriptor derived from the mesh descriptor (the earlier plug-in read those four entry points): inherited, they would
 * hand a mesh pipeline to the vertex-plus-fragment builder and get something compiled rather than a refusal. Overridden
 * to refuse by name, every form, handlers called at once with the error. */
- (id)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
				    reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error
{
	(void)descriptor; (void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self hakiosRefuse:error what:"a mesh render pipeline" why:@"mesh and object stages are not served; vertex + fragment pipelines are"];
}

- (id)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)descriptor error:(NSError **)error
{
	return [self newRenderPipelineStateWithMeshDescriptor:descriptor options:0 reflection:NULL error:error];
}

- (void)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
			       completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithMeshDescriptor:descriptor options:options reflection:NULL error:&error];

	if (handler != NULL) {
		handler(state, nil, error);
	}
}

- (void)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)descriptor
			       completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithMeshDescriptor:descriptor options:0 reflection:NULL error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
}

- (id)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
				    reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error
{
	(void)descriptor; (void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self hakiosRefuse:error what:"a tile render pipeline"
			      why:@"tile shaders and imageblocks are Apple-GPU tile memory; this GPU draws straight to memory (sysmem) and has none"];
}

- (id)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor error:(NSError **)error
{
	return [self newRenderPipelineStateWithTileDescriptor:descriptor options:0 reflection:NULL error:error];
}

- (void)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
			       completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithTileDescriptor:descriptor options:options reflection:NULL error:&error];

	if (handler != NULL) {
		handler(state, nil, error);
	}
}

- (void)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor
			       completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithTileDescriptor:descriptor options:0 reflection:NULL error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
}

/* Metal source needs Metal's own compiler service, which this device has not got; what it compiles is AIR out of a
 * .metallib, and that road (newLibraryWithData:, newLibraryWithFile:) is the base class's and works */
/* Metal Shading Language source is Metal's own to compile into AIR: _MTLDevice sends it to MTLCompilerService, which
 * runs under launchd, and the library that comes back holds AIR like any .metallib, which this plug-in's compiler then
 * takes function by function (bitcodeData). Without the service -- a program run before launchd -- there is no library,
 * and the base's NSError says so. */
- (id)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
	NSError *mine = nil;
	id library = [super newLibraryWithSource:source options:options error:&mine];

	if (library == nil) {
		hm_print("refused: a library from source: Metal's compiler service gave none: %s\n",
			 mine != nil ? [[mine localizedDescription] UTF8String] : "no error said");
	}
	if (error != NULL) {
		*error = mine;
	}
	return library;
}

- (void)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options
	   completionHandler:(MTLNewLibraryCompletionHandler)handler
{
	NSError *error = nil;
	id library = [self newLibraryWithSource:source options:options error:&error];

	if (handler != NULL) {
		handler(library, error);
	}
}

/* --- textures --- */

/* The number a texture (gpuResourceID, uniqueIdentifier, gpuHandle) is told apart by for as long as it lives. A counter,
 * never the object's address: the allocator reuses addresses, and UIKit's image caches key on this number, so a texture
 * at a dead one's address would be served the dead one's picture. From 1, so that 0 stays "never set". */
static uint64_t
hm_next_resource_id(void)
{
	static uint64_t next;

	return __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
}

/* the nil this device returns for a texture it cannot make, with the sentence that says why: Metal validates nothing before
 * this call (the base class has no -newTextureWithDescriptor: at all), so this line is the only account of the refusal */
static id
hm_refuse_texture(NSString *why)
{
	hm_print("refused: a texture: %s\n", [why UTF8String]);
	return nil;
}

/*
 * Whether a texture is made compressed (UBWC, hakios_metal_texture_new_compressed): one the GPU only samples, which the
 * CPU fills through -replaceRegion: and reads through -getBytes: -- a 2D, one-level, single-sampled RGBA8 or BGRA8
 * texture of shared storage, used for ShaderRead alone, of 256x256 pixels or more. CoreAnimation's large
 * read-only textures are most of what its heavy frames read, and compressed they read in half the time (probe "UBWC").
 * Boot-arg metal_noubwc makes every texture linear as before.
 */
static BOOL
hm_compress_ok(void)
{
	static int off = -1;

	if (off < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		off = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_noubwc") != NULL;
	}
	return !off;
}

static BOOL
hm_compress(MTLTextureDescriptor *descriptor, MTLTextureUsage usage)
{
	return hm_compress_ok() && [descriptor textureType] == MTLTextureType2D && [descriptor mipmapLevelCount] == 1 && [descriptor sampleCount] == 1 &&
	       ([descriptor pixelFormat] == MTLPixelFormatRGBA8Unorm || [descriptor pixelFormat] == MTLPixelFormatBGRA8Unorm) &&
	       usage == MTLTextureUsageShaderRead &&
	       [descriptor storageMode] == MTLStorageModeShared &&
	       [descriptor width] * [descriptor height] >= 65536;
}

/*
 * The one gate a texture descriptor passes here, by hand. Metal's own validator (-[MTLTextureDescriptorInternal
 * validateWithDevice:]) is an assert that ends in abort(), like every validation context of this Metal, so it is never
 * called; and _MTLDevice has no -newTextureWithDescriptor: of its own to fall back on. What is served is what hakios-metal's
 * texture.hpp serves: one 2D level, one slice, one sample, shared memory, read and/or written by a kernel. The type, the
 * pixel format and the usage bits go to hakios_metal_texture_new as Metal's own numbers, so a format outside its table is
 * refused by the core with the format's name, and nothing here translates them a second time.
 */
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	hakios_metal_texture *texture;
	MTLTextureUsage usage;
	MTLTextureSwizzleChannels swizzle;

	if (hm_timeline_on() && descriptor != nil && [descriptor width] * [descriptor height] >= 500000) {
		hm_print("timeline: a texture of %lux%lu, format %lu, storage %lu, usage %#lx (not a surface)\n", (unsigned long)[descriptor width],
			 (unsigned long)[descriptor height], (unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor storageMode],
			 (unsigned long)[descriptor usage]);
	}
	if (descriptor == nil || ![descriptor isKindOfClass:[MTLTextureDescriptor class]]) {
		return hm_refuse_texture([NSString stringWithFormat:@"the descriptor is %s, not an MTLTextureDescriptor",
							    descriptor != nil ? class_getName(object_getClass(descriptor)) : "nil"]);
	}
	if ([descriptor textureType] > MTLTextureType3D) {
		return hm_refuse_texture([NSString stringWithFormat:@"texture type %lu is past 3D (7): a multisample array or a texture buffer, "
							    "not served", (unsigned long)[descriptor textureType]]);
	}
	if ([descriptor arrayLength] != 1 && [descriptor textureType] != MTLTextureType2DArray && [descriptor textureType] != MTLTextureType1DArray &&
	    [descriptor textureType] != MTLTextureTypeCubeArray) {
		return hm_refuse_texture([NSString stringWithFormat:@"an array of %lu slices; one slice is served",
							    (unsigned long)[descriptor arrayLength]]);
	}
	/* a multisample texture is a render target of 2 or 4 samples a pixel, resolved at the end of its pass (the core's
	 * hakios_metal_texture_new_multisample refuses another count or use by name) */
	if (([descriptor sampleCount] != 1) != ([descriptor textureType] == MTLTextureType2DMultisample)) {
		return hm_refuse_texture([NSString stringWithFormat:@"%lu samples for texture type %lu; a count past 1 is a 2D multisample "
							    "texture's (4) alone", (unsigned long)[descriptor sampleCount], (unsigned long)[descriptor textureType]]);
	}
	if ([descriptor depth] != 1 && [descriptor textureType] != MTLTextureType3D) {
		return hm_refuse_texture([NSString stringWithFormat:@"depth %lu for a 2D texture (it has depth 1)", (unsigned long)[descriptor depth]]);
	}
	if ([descriptor textureType] == MTLTextureType1D && [descriptor height] != 1) {
		return hm_refuse_texture([NSString stringWithFormat:@"a 1D texture %lu high (it has height 1)", (unsigned long)[descriptor height]]);
	}
	if ([descriptor width] > 0xffffffffUL || [descriptor height] > 0xffffffffUL) {
		return hm_refuse_texture([NSString stringWithFormat:@"%lux%lu does not fit the core's 32-bit sizes",
							    (unsigned long)[descriptor width], (unsigned long)[descriptor height]]);
	}
	usage = [descriptor usage];
	/* the right to make views of it later: the view that changes nothing is made, one that reinterprets the format is
	 * refused by name where it is asked for (-newTextureViewWithPixelFormat:...), so the bit is nothing to refuse here */
	usage &= ~(MTLTextureUsage)MTLTextureUsagePixelFormatView;
	/* ShaderRead, ShaderWrite and RenderTarget (4) all go to hakios_metal_texture_new as Metal's bits: the layout is the
	 * same linear one for all three, and the usage decides what a dispatch (write) or a draw (render target) may do to
	 * the texture, each refused by name without its bit */
	if (usage & ~(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) {
		return hm_refuse_texture([NSString stringWithFormat:@"usage %#lx has bits this plug-in does not know", (unsigned long)usage]);
	}
	if (usage == MTLTextureUsageUnknown) {
		/* Metal's pre-iOS 9 default, which lets a texture be used every way; here that is read and written by a
		 * kernel and drawn into by a render pass */
		HM_ONCE("a texture was asked for with usage Unknown (0); it is given shader read, shader write and render target, the "
			"three uses one has here\n");
		usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
		if ([descriptor pixelFormat] == MTLPixelFormatDepth32Float || [descriptor pixelFormat] == MTLPixelFormatDepth32Float_Stencil8) {
			usage = MTLTextureUsageRenderTarget;   /* a depth texture is tiled and is an attachment alone (hakios-metal's texture.hpp) */
		}
		{
			unsigned bw = 1, bh = 1, bytes = 0;

			if (hakios_metal_format_block((unsigned)[descriptor pixelFormat], &bw, &bh, &bytes) && (bw > 1 || bh > 1)) {
				usage = MTLTextureUsageShaderRead;   /* a compressed format is sampled alone */
			} else if ([descriptor textureType] == MTLTextureTypeCube || [descriptor textureType] == MTLTextureTypeCubeArray ||
				   [descriptor textureType] == MTLTextureType1DArray) {
				/* their faces and layers are drawn into and sampled; a kernel does not store into them here */
				usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
			}
		}
	}
	/* Memoryless is a render target an Apple GPU keeps in tile memory alone, its contents undefined past the pass. This GPU
	 * draws into memory (sysmem), so the texture is given memory like any other: what Metal leaves undefined is kept, which
	 * is one of the things undefined allows. CoreAnimation asks for these for its blur and backdrop passes, and a refusal
	 * here was a pass with no texture and a layer drawn from nothing. */
	if ([descriptor storageMode] == MTLStorageModeMemoryless) {
		HM_ONCE("a texture was asked for with memoryless storage; this GPU has no tile memory to keep it in, so it is given "
			"memory as any other texture is\n");
	}
	if ([descriptor storageMode] == MTLStorageModePrivate) {
		HM_ONCE("a texture was asked for with private storage; every texture here is shared memory, and that is what is "
			"given (the GPU reads the same pages the processor maps)\n");
	}
	if ([descriptor compressionType] != MTLTextureCompressionTypeLossless) {
		return hm_refuse_texture([NSString stringWithFormat:@"compression type %ld; this GPU's linear layout is not compressed "
							    "(lossless, 0, is what every texture here is)", (long)[descriptor compressionType]]);
	}
	swizzle = [descriptor swizzle];
	if (swizzle.red != MTLTextureSwizzleRed || swizzle.green != MTLTextureSwizzleGreen || swizzle.blue != MTLTextureSwizzleBlue ||
	    swizzle.alpha != MTLTextureSwizzleAlpha) {
		return hm_refuse_texture([NSString stringWithFormat:@"a swizzle of (%u %u %u %u); only the identity (red 2, green 3, blue 4, "
							    "alpha 5) is served: the descriptor's own swizzle field carries the format's channel "
							    "order", swizzle.red, swizzle.green, swizzle.blue, swizzle.alpha]);
	}
	/* the mip levels are the core's (texture.hpp: fdl6_layout's rule, which the TP follows): it refuses a count past the
	 * bottom of the chain with the numbers */
	if ([descriptor textureType] == MTLTextureType2DMultisample) {
		/* drawn into and resolved, never read or written by a shader here: the shader bits Metal's Unknown default
		 * gives are dropped rather than refused */
		texture = hakios_metal_texture_new_multisample(_gpu, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width],
							       (unsigned)[descriptor height], (unsigned)[descriptor sampleCount],
							       (unsigned)(usage & ~(MTLTextureUsage)MTLTextureUsageShaderWrite));
	} else if ([descriptor textureType] == MTLTextureType1DArray || [descriptor textureType] == MTLTextureTypeCubeArray ||
		   [descriptor textureType] == MTLTextureTypeCube || [descriptor textureType] == MTLTextureType2DArray) {
		/* the layered types, with their mip levels (each layer the chain of them). A cube's and a cube array's slices
		 * are its faces, six a cube, as Metal's slice numbering has them; a 1D array is one row high */
		const MTLTextureType type = [descriptor textureType];
		const BOOL faces = type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray;
		const NSUInteger layers = type == MTLTextureTypeCube ? 6 : [descriptor arrayLength] * (faces ? 6 : 1);
		texture = hakios_metal_texture_new_layered(_gpu, (unsigned)type, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width],
							   faces ? (unsigned)[descriptor width] : type == MTLTextureType1DArray ? 1 : (unsigned)[descriptor height],
							   (unsigned)layers, (unsigned)[descriptor mipmapLevelCount], (unsigned)usage);
	} else if ([descriptor textureType] == MTLTextureType3D) {
		texture = hakios_metal_texture_new_3d(_gpu, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width], (unsigned)[descriptor height],
						      (unsigned)[descriptor depth], (unsigned)usage);
	} else if (hm_compress(descriptor, usage)) {
		texture = hakios_metal_texture_new_compressed(_gpu, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width],
							      (unsigned)[descriptor height], (unsigned)usage);
		if (hm_timeline_on()) {
			hm_print("timeline: a compressed texture of %lux%lu, format %lu: %s\n", (unsigned long)[descriptor width], (unsigned long)[descriptor height],
				 (unsigned long)[descriptor pixelFormat], texture != NULL ? "made" : hakios_metal_error());
		}
	} else {
		texture = hakios_metal_texture_new_levels(_gpu, (unsigned)[descriptor textureType], (unsigned)[descriptor pixelFormat],
							  (unsigned)[descriptor width], (unsigned)[descriptor height],
							  (unsigned)[descriptor mipmapLevelCount], (unsigned)usage);
	}
	if (texture == NULL) {
		/* the core's own sentence: the format by name, a size outside 1..32767, or no memory */
		return hm_refuse_texture([NSString stringWithFormat:@"%lux%lu, pixel format %lu, usage %#lx: %s", (unsigned long)[descriptor width],
							    (unsigned long)[descriptor height], (unsigned long)[descriptor pixelFormat],
							    (unsigned long)usage, hakios_metal_error()]);
	}
	return [[HakiosMetalTexture alloc] initWithHakiosDevice:self texture:texture descriptor:descriptor usage:usage];
}

/* An IOSurface is the window server's memory, and what CAMetalLayer makes its drawables of: the texture is the
 * surface's own pixels (hakios-metal asks the kernel for the surface's memory as a GPU buffer), so drawing into it is
 * drawing into what the compositor reads. A refusal here is LOUD and named, never a quiet nil: the earlier plug-in's
 * nil became a drawable with a nil texture that CoreAnimation asserted on somewhere else. */
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane
{
	if (hm_timeline_on() && descriptor != nil && [descriptor width] * [descriptor height] >= 500000) {
		hm_print("timeline: a texture of %lux%lu, format %lu, usage %#lx, over surface %u\n", (unsigned long)[descriptor width],
			 (unsigned long)[descriptor height], (unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor usage],
			 surface != NULL ? IOSurfaceGetID(surface) : 0);
	}
	MTLTextureUsage usage;
	hakios_metal_texture *texture;
	HakiosMetalTexture *t;

	/* The kernel hands the surface's memory to the GPU by its ID (hakios-metal: Texture::wrapSurface), so the texture's
	 * rows are the surface's: what the display or another process reads is what is drawn. Only the descriptor's shape is
	 * checked here; the rows, the FourCC against the format and the size are hakios-metal's to judge, by name. */
	if (descriptor == nil || ![descriptor isKindOfClass:[MTLTextureDescriptor class]] || surface == NULL) {
		return hm_refuse_texture(@"an IOSurface texture needs an MTLTextureDescriptor and a surface");
	}
	if ([descriptor textureType] != MTLTextureType2D || [descriptor mipmapLevelCount] != 1 || [descriptor sampleCount] != 1 ||
	    [descriptor arrayLength] != 1) {
		return hm_refuse_texture([NSString stringWithFormat:@"an IOSurface texture of type %lu, %lu levels, %lu samples, %lu slices: a "
							    "single-level 2D texture is what a surface takes", (unsigned long)[descriptor textureType],
							    (unsigned long)[descriptor mipmapLevelCount], (unsigned long)[descriptor sampleCount],
							    (unsigned long)[descriptor arrayLength]]);
	}
	usage = [descriptor usage];
	if (usage == MTLTextureUsageUnknown) {
		usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	}
	usage &= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
	/*
	 * A framebuffer with a rotation (MTLTextureDescriptor's private -rotation, 1 to 3 quarter turns): backboardd makes
	 * one when an application turns the screen, a 2436 x 1125 texture over the 1125 x 2436 display surface, and draws
	 * it landscape whatever the device answers for render target rotation. The RB writes rows as they lie, so it draws
	 * into a texture of its own of the descriptor's size, kept with the surface's texture so that a pass loading the
	 * last frame finds it there, and the 2D engine lays it into the surface turned (-hakiosTurnInto:, at the command
	 * buffer's commit).
	 */
	{
		const NSUInteger rotation = [descriptor respondsToSelector:sel_getUid("rotation")] ?
						    ((NSUInteger (*)(id, SEL))objc_msgSend)(descriptor, sel_getUid("rotation")) : 0;

		if (rotation >= 1 && rotation <= 3 && IOSurfaceGetPlaneCount(surface) <= 1 && plane == 0) {
			const BOOL quarter = rotation != 2;
			const size_t sw = IOSurfaceGetWidth(surface), sh = IOSurfaceGetHeight(surface);

			if (quarter ? ([descriptor width] == sh && [descriptor height] == sw) : ([descriptor width] == sw && [descriptor height] == sh)) {
				hakios_metal_texture *upright = hakios_metal_texture_new_iosurface(_gpu, IOSurfaceGetID(surface), 0, (unsigned)[descriptor pixelFormat],
												   (unsigned)sw, (unsigned)sh,
												   (unsigned)(MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget));
				hakios_metal_texture *drawn = upright == NULL ? NULL :
							      hakios_metal_texture_new_levels(_gpu, 2, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width],
											      (unsigned)[descriptor height], 1,
											      (unsigned)(MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget));

				if (drawn != NULL) {
					HM_ONCE("a framebuffer turned by rotation %lu over IOSurface %u (%zux%zu) is drawn %lux%lu and turned into the "
						"surface by the 2D engine at each commit\n", (unsigned long)rotation, IOSurfaceGetID(surface), sw, sh,
						(unsigned long)[descriptor width], (unsigned long)[descriptor height]);
					t = [[HakiosMetalTexture alloc] initWithHakiosDevice:self texture:drawn descriptor:descriptor
										       usage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
					[t hakiosSetIOSurface:surface plane:0];
					/* Metal's rotation n is the 2D engine's a6xx_rotation n (a guess the display settles: see the
					 * screenshots of a turned screen) */
					[t hakiosSetTurned:upright rotation:(unsigned)rotation];
					return t;
				}
				if (upright != NULL) {
					hakios_metal_texture_free(upright);
				}
			}
		}
	}
	if (([descriptor pixelFormat] == 500 || [descriptor pixelFormat] == 540) && IOSurfaceGetPlaneCount(surface) == 2 && plane == 0) {
		/* the private two-plane formats a decoded picture is sampled through, read and never drawn into: 500,
		 * CoreAnimation's for a photo, sampled as RGB; and 540 (MTLPixelFormatRGB8_420_2P by the descriptor's own
		 * description), VideoToolbox's for the live wallpaper's 512x512 '420f' frames, sampled as the planes'
		 * own values (-hakiosConvertYCbCr) */
		/* as an RGBA8 copy converted from the planes (-hakiosShadowYCbCr:): the texture unit here does not convert
		 * as Apple's does */
		/* (compressed where it is large enough, as hm_compress decides for a texture of its own: the copy is sampled
		 * and only ever written through replace, and SpringBoard's heavy frames read a 1105x1160 one most after the
		 * wallpaper) */
		hakios_metal_texture *copy = NULL;
		if (hm_compress_ok() && [descriptor width] * [descriptor height] >= 65536) {
			copy = hakios_metal_texture_new_compressed(_gpu, 70, (unsigned)[descriptor width], (unsigned)[descriptor height],
								   (unsigned)MTLTextureUsageShaderRead);
		}
		if (copy == NULL) {
			copy = hakios_metal_texture_new_levels(_gpu, 2, 70, (unsigned)[descriptor width], (unsigned)[descriptor height], 1,
								(unsigned)MTLTextureUsageShaderRead);
		}
		if (copy == NULL) {
			return hm_refuse_texture([NSString stringWithFormat:@"a %lux%lu RGBA8 copy of a two-plane surface: %s",
							    (unsigned long)[descriptor width], (unsigned long)[descriptor height], hakios_metal_error()]);
		}
		HM_ONCE("a two-plane YCbCr surface sampled as format %lu is read through an RGBA8 copy converted from it, made again "
			"when the surface changes\n", (unsigned long)[descriptor pixelFormat]);
		t = [[HakiosMetalTexture alloc] initWithHakiosDevice:self texture:copy descriptor:descriptor usage:MTLTextureUsageShaderRead];
		[t hakiosShadowYCbCr:surface format:[descriptor pixelFormat]];
		return t;
	} else {
		/* A surface is drawn into whatever usage its texture was asked with: CoreAnimation renders a material's
		 * snapshot (the App Library's blurred wallpaper) into a texture it made with ShaderRead alone, and Apple's
		 * driver does not hold it to the usage. A surface's rows are linear, so a render target costs nothing here;
		 * one is added wherever the core takes it for the format, and the usage asked for is the fallback. */
		texture = NULL;
		if ((usage & MTLTextureUsageRenderTarget) == 0) {
			texture = hakios_metal_texture_new_iosurface(_gpu, IOSurfaceGetID(surface), (unsigned)plane, (unsigned)[descriptor pixelFormat],
								     (unsigned)[descriptor width], (unsigned)[descriptor height],
								     (unsigned)(usage | MTLTextureUsageRenderTarget));
			if (texture != NULL) {
				usage |= MTLTextureUsageRenderTarget;
			}
		}
		if (texture == NULL) {
			texture = hakios_metal_texture_new_iosurface(_gpu, IOSurfaceGetID(surface), (unsigned)plane, (unsigned)[descriptor pixelFormat],
								     (unsigned)[descriptor width], (unsigned)[descriptor height], (unsigned)usage);
		}
	}
	if (texture == NULL && IOSurfaceGetPlaneCount(surface) <= 1 && IOSurfaceGetBytesPerRow(surface) % 64 != 0 &&
	    ([descriptor usage] == MTLTextureUsageUnknown ||
	     ([descriptor usage] & ~MTLTextureUsagePixelFormatView & ~MTLTextureUsageShaderRead & ~MTLTextureUsageRenderTarget &
	      ~MTLTextureUsageShaderWrite) == 0) &&
	    plane == 0) {
		/* rows the GPU cannot take where they lie: a copy of them (-hakiosShadow:), made again when the surface
		 * changes. Whatever writes the copy -- a kernel, or a pass drawing into it, which backboardd does with the
		 * App Library's snapshot (4512-byte rows) whatever usage it asked for -- goes back into the surface when its
		 * command buffer completes (-hakiosWriteBackShadow); a render target is given wherever the format takes one,
		 * as over an aligned surface above */
		const MTLTextureUsage writes = [descriptor usage] & MTLTextureUsageShaderWrite;
		MTLTextureUsage copyUsage = MTLTextureUsageShaderRead | writes | MTLTextureUsageRenderTarget;
		hakios_metal_texture *copy = hakios_metal_texture_new_levels(_gpu, 2, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width],
									     (unsigned)[descriptor height], 1, (unsigned)copyUsage);

		if (copy == NULL) {
			copyUsage = MTLTextureUsageShaderRead | writes;
			copy = hakios_metal_texture_new_levels(_gpu, 2, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width],
							       (unsigned)[descriptor height], 1, (unsigned)copyUsage);
		}
		if (copy != NULL) {
			HM_ONCE("a texture over an IOSurface of rows the GPU cannot take (%zu bytes) is a copy of them, made again when the "
				"surface changes\n", IOSurfaceGetBytesPerRow(surface));
			t = [[HakiosMetalTexture alloc] initWithHakiosDevice:self texture:copy descriptor:descriptor usage:copyUsage];
			[t hakiosShadow:surface];
			return t;
		}
	}
	if (texture == NULL) {
		/* what the surface is, beside the core's sentence: a format the core does not take is most often a layout of
		 * the surface's (a FourCC and its planes) that it has not been taught, and these are what teach it */
		OSType fourcc = IOSurfaceGetPixelFormat(surface);
		size_t planes = IOSurfaceGetPlaneCount(surface);
		char shape[160];

		snprintf(shape, sizeof(shape), "'%c%c%c%c', %zux%zu, %zu rows of %zu bytes, %zu plane(s)", (char)(fourcc >> 24), (char)(fourcc >> 16),
			 (char)(fourcc >> 8), (char)fourcc, IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), IOSurfaceGetHeight(surface),
			 IOSurfaceGetBytesPerRow(surface), planes);
		if (planes > 1) {
			size_t l = strlen(shape);

			snprintf(shape + l, sizeof(shape) - l, "; plane 1 %zux%zu, rows of %zu bytes", IOSurfaceGetWidthOfPlane(surface, 1),
				 IOSurfaceGetHeightOfPlane(surface, 1), IOSurfaceGetBytesPerRowOfPlane(surface, 1));
		}
		/* and the pixel format by Metal's own name for it: the private ones (500 and up) are numbered nowhere public,
		 * and the descriptor's description is the one place the system names them */
		NSString *named = @"";
		for (NSString *line in [[descriptor description] componentsSeparatedByString:@"\n"]) {
			if ([line rangeOfString:@"pixelFormat"].location != NSNotFound) {
				named = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
				break;
			}
		}
		const unsigned long rotation = [descriptor respondsToSelector:sel_getUid("rotation")] ?
						   ((NSUInteger (*)(id, SEL))objc_msgSend)(descriptor, sel_getUid("rotation")) : 0;
		return hm_refuse_texture([NSString stringWithFormat:@"over IOSurface %u (%s), plane %lu, usage %#lx, %@, rotation %lu: %s",
								    IOSurfaceGetID(surface), shape, (unsigned long)plane, (unsigned long)[descriptor usage], named,
								    rotation, hakios_metal_error()]);
	}
	t = [[HakiosMetalTexture alloc] initWithHakiosDevice:self texture:texture descriptor:descriptor usage:usage];
	[t hakiosSetIOSurface:surface plane:plane];
	return t;
}

/* The other resource kinds, each refused by name: the base class answers these with nil-stubs that say nothing. */

/* --- samplers --- */

static id
hm_refuse_sampler(NSString *why)
{
	hm_print("refused: a sampler state: %s\n", [why UTF8String]);
	return nil;
}

/*
 * The descriptor's fields, checked against the ranges -[_MTLSamplerState initWithDevice:samplerDescriptor:] asserts on
 * (see its declaration above), BEFORE the sampler is made and before super is called: an invalid value handed to super
 * kills the process, and this method is the only gate in front of it (_MTLDevice has no -newSamplerStateWithDescriptor:
 * of its own). Then the fields go to hakios_metal_sampler_new in Metal's own numbering (MTLSampler.h, MTLDepthStencil.h),
 * and what the core does not serve -- a value outside Metal's enums, or ClampToZero beside a coloured border on one
 * sampler (its one BCOLOR) -- comes back as NULL with the core's sentence, which is printed and turned into nil. Only a
 * descriptor that passed both is given to super.
 */
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	struct hakios_metal_sampler_desc desc;
	hakios_metal_sampler *sampler;

	if (descriptor == nil || ![descriptor isKindOfClass:[MTLSamplerDescriptor class]]) {
		return hm_refuse_sampler([NSString stringWithFormat:@"the descriptor is %s, not an MTLSamplerDescriptor (Metal's own init would abort on it)",
							    descriptor != nil ? class_getName(object_getClass(descriptor)) : "nil"]);
	}
	memset(&desc, 0, sizeof(desc));
	desc.min_filter = (unsigned)[descriptor minFilter];
	desc.mag_filter = (unsigned)[descriptor magFilter];
	desc.mip_filter = (unsigned)[descriptor mipFilter];
	desc.s_address = (unsigned)[descriptor sAddressMode];
	desc.t_address = (unsigned)[descriptor tAddressMode];
	desc.r_address = (unsigned)[descriptor rAddressMode];
	desc.normalized_coordinates = [descriptor normalizedCoordinates] ? 1 : 0;
	desc.lod_min_clamp = [descriptor lodMinClamp];
	desc.lod_max_clamp = [descriptor lodMaxClamp];
	desc.max_anisotropy = (unsigned)[descriptor maxAnisotropy];
	desc.compare_function = (unsigned)[descriptor compareFunction];
	desc.border_color = (unsigned)[descriptor borderColor];
	if (desc.min_filter >= 3 || desc.mag_filter >= 3) {
		return hm_refuse_sampler([NSString stringWithFormat:@"min filter %u, mag filter %u: MTLSamplerMinMagFilter is Nearest 0 or "
							    "Linear 1 (Metal's own init asserts on anything from 3 up)", desc.min_filter, desc.mag_filter]);
	}
	if (desc.mip_filter >= 3) {
		return hm_refuse_sampler([NSString stringWithFormat:@"mip filter %u: MTLSamplerMipFilter is 0, 1 or 2", desc.mip_filter]);
	}
	if (desc.s_address >= 7 || desc.t_address >= 7 || desc.r_address >= 7) {
		return hm_refuse_sampler([NSString stringWithFormat:@"address modes %u %u %u: MTLSamplerAddressMode is 0 to 5 (Metal's own init "
							    "asserts from 7 up)", desc.s_address, desc.t_address, desc.r_address]);
	}
	if (desc.border_color >= 4) {
		return hm_refuse_sampler([NSString stringWithFormat:@"border colour %u: MTLSamplerBorderColor is 0, 1 or 2", desc.border_color]);
	}
	if (desc.compare_function > 7) {
		return hm_refuse_sampler([NSString stringWithFormat:@"compare function %u: MTLCompareFunction is Never 0 to Always 7", desc.compare_function]);
	}
	if ([descriptor supportArgumentBuffers]) {
		HM_ONCE("a sampler was asked for with supportArgumentBuffers; this plug-in has no argument buffers, and the sampler is "
			"made for the encoder's -setSamplerState: alone\n");
	}
	sampler = hakios_metal_sampler_new(_gpu, &desc);
	if (sampler == NULL) {
		/* the core's sentence names the mode it does not serve */
		return hm_refuse_sampler([NSString stringWithFormat:@"filters %u/%u/%u, address modes %u/%u/%u, border %u, compare %u, "
							    "anisotropy %u: %s", desc.min_filter, desc.mag_filter, desc.mip_filter, desc.s_address,
							    desc.t_address, desc.r_address, desc.border_color, desc.compare_function,
							    desc.max_anisotropy, hakios_metal_error()]);
	}
	return [[HakiosMetalSamplerState alloc] initWithHakiosDevice:self sampler:sampler descriptor:descriptor];
}

/* Both halves of a depth/stencil state are served (hakios-metal's DepthState and StencilState): the faces are read into
 * the state once and each draw that binds it carries them. */
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor
{
	if (descriptor == nil || ![descriptor isKindOfClass:[MTLDepthStencilDescriptor class]]) {
		hm_refusal(self, _cmd, "was given no MTLDepthStencilDescriptor; it returns nil");
		return nil;
	}
	return [[HakiosMetalDepthStencilState alloc] initWithDevice:(id<MTLDevice>)self descriptor:descriptor];
}

/* MTLArgumentDescriptor members, in the layout the front end reads back (HakiosMetalArgumentEncoder) */
- (id)newArgumentEncoderWithArguments:(NSArray *)arguments
{
	return [[HakiosMetalArgumentEncoder alloc] initWithDevice:self arguments:arguments];
}

/* What a heap has to hold a texture or a buffer made from it: the core's plan of the texture, the same one creation
 * allocates by, so a heap sized from these (MPS sizes its temporary-texture heaps so) has room for what it is then asked
 * to make; HakiosMetalHeap counts each resource's allocatedSize, which is the same number. 4 KiB alignment: every
 * layout here starts on a page. */
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	MTLTextureUsage usage = [descriptor usage] & ~(MTLTextureUsage)MTLTextureUsagePixelFormatView;
	const MTLTextureType type = [descriptor textureType];
	const BOOL depth = [descriptor pixelFormat] == MTLPixelFormatDepth32Float || [descriptor pixelFormat] == MTLPixelFormatDepth32Float_Stencil8;
	const unsigned layers = type == MTLTextureType2DArray || type == MTLTextureType1DArray ? (unsigned)[descriptor arrayLength] :
				type == MTLTextureTypeCubeArray ? (unsigned)[descriptor arrayLength] * 6 : type == MTLTextureTypeCube ? 6 :
				type == MTLTextureType3D ? (unsigned)[descriptor depth] : 1;
	uint64_t bytes = 0;

	/* (the usage only decides what the plan accepts, not its bytes: Unknown is resolved as creation resolves it) */
	if (usage == MTLTextureUsageUnknown) {
		usage = depth || type == MTLTextureType2DMultisample ? MTLTextureUsageRenderTarget : MTLTextureUsageShaderRead;
	}
	if (type == MTLTextureType2DMultisample) {
		usage &= ~(MTLTextureUsage)MTLTextureUsageShaderWrite;
	}
	if (hakios_metal_texture_plan_bytes((unsigned)type, (unsigned)[descriptor pixelFormat], (unsigned)[descriptor width],
					    (unsigned)[descriptor height], layers, (unsigned)[descriptor mipmapLevelCount],
					    (unsigned)[descriptor sampleCount], (unsigned)usage, &bytes) != 0) {
		hm_print("refused: -heapTextureSizeAndAlignWithDescriptor: of %lux%lu, pixel format %lu, type %lu: %s; it answers 0 bytes\n",
			 (unsigned long)[descriptor width], (unsigned long)[descriptor height], (unsigned long)[descriptor pixelFormat],
			 (unsigned long)type, hakios_metal_error());
		return (MTLSizeAndAlign){ 0, 4096 };
	}
	return (MTLSizeAndAlign){ (NSUInteger)bytes, 4096 };
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	(void)options;
	return (MTLSizeAndAlign){ length, 256 };   /* a buffer's allocatedSize is its length */
}

- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor
{
	if (descriptor == nil || [descriptor type] == MTLHeapTypePlacement) {
		hm_print("refused: a heap of type %ld: a placement heap puts resources at offsets of one allocation, and every resource "
			 "here is an allocation of its own; it returns nil\n", descriptor != nil ? (long)[descriptor type] : -1L);
		return nil;
	}
	return [[HakiosMetalHeap alloc] initWithDevice:self descriptor:descriptor];
}

/* A resource group is a residency set of Apple's GPU drivers: what it names is made resident together through the
 * command buffer that is handed it. Every allocation here is resident for as long as it exists, so a group would say
 * nothing; and its protocol (MTLResourceGroupSPI) is the GPU bundle's, not in this process. nil is the answer callers
 * already take as "no group": AppleCVAPhoto in mediaserverd tests it before each use and skips the call. */
- (id)newResourceGroupFromResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return nil;
}

- (id)newFence
{
	return [[HakiosMetalFence alloc] initWithDevice:(id<MTLDevice>)self];
}

- (id)newEvent
{
	return [[HakiosMetalEvent alloc] initWithDevice:(id<MTLDevice>)self];
}

- (id)newSharedEvent
{
	return [[HakiosMetalEvent alloc] initWithDevice:(id<MTLDevice>)self];
}

@end

/* ============================================================================================================
 * The buffer: hakios-metal's GPU memory with its CPU mapping
 * ============================================================================================================ */

@implementation HakiosMetalBuffer

/* Conformance is a static property of the class and callers test it (MPS tests the command buffer's before anything
 * else); it is declared for the one protocol this class does answer. */
+ (void)load
{
	hm_adopt(self, @protocol(MTLBuffer), "MTLBuffer");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device length:(NSUInteger)length
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_buffer = hakios_metal_buffer_new([device hakiosDevice], length);
	if (_buffer == NULL) {
		hm_print("refused: a buffer of %lu bytes: %s\n", (unsigned long)length, hakios_metal_error());
		[self release];
		return nil;
	}
	_device = [device retain];
	return self;
}

- (void)dealloc
{
	if (_buffer != NULL) {
		hakios_metal_buffer_free(_buffer);   /* a command buffer still using it keeps its own reference */
	}
	if (_deallocator != nil) {
		_deallocator(_callerBytes, _callerLength);
		[_deallocator release];
	}
	[_device release];
	[super dealloc];
}

- (void)hakiosSetDeallocator:(void (^)(void *, NSUInteger))deallocator bytes:(void *)bytes length:(NSUInteger)length
{
	_deallocator = [deallocator copy];
	_callerBytes = bytes;
	_callerLength = length;
}

- (hakios_metal_buffer *)hakiosBuffer { return _buffer; }

- (void *)contents          { return hakios_metal_buffer_contents(_buffer); }
- (NSUInteger)length        { return (NSUInteger)hakios_metal_buffer_length(_buffer); }
- (uint64_t)gpuAddress      { return hakios_metal_buffer_gpu_address(_buffer); }
- (NSUInteger)allocatedSize { return [self length]; }
- (id)device                { return _device; }

/* The memory is write-combined and the GPU reads it through no cache of the processor's, so there is nothing to push
 * out: a no-op that is the truth about this memory, not an omission (and the method is macOS-only in any case). */
- (void)didModifyRange:(NSRange)range { (void)range; }

/* what was asked for does not change what was given: one kind of memory, described as what it is */
- (MTLStorageMode)storageMode               { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode             { return MTLCPUCacheModeWriteCombined; }
- (MTLResourceOptions)resourceOptions       { return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeDefault; }
- (id)heap                                  { return nil; }   /* every buffer is its own allocation; nil is Metal's answer for that */
- (NSUInteger)heapOffset                    { return 0; }
- (BOOL)isAliasable                         { return NO; }

/* the kernel holds the pages from allocation to release, so a buffer here is never volatile, whatever was asked */
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
	if (state == MTLPurgeableStateVolatile || state == MTLPurgeableStateEmpty) {
		HM_ONCE("a buffer was asked to become purgeable; the kernel does not purge GPU memory here, so it stays non-volatile\n");
	}
	return MTLPurgeableStateNonVolatile;
}

/* A texture over the buffer's own bytes (hakios-metal: Texture::overBuffer), when the rows are ones the TP takes from
 * memory as they lie: a pitch and a start that are multiples of 64 bytes. Rows that are not -- QuartzCore's
 * copy_image_to_texture hands an image's own row bytes, whatever its width made them -- are copied into a texture of
 * the GPU's layout instead, as the texture is made: a copy then, not a view, so a later write to the buffer is not seen
 * through it. Its one caller writes the buffer before and never after. */
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow
{
	MTLTextureUsage usage;
	hakios_metal_texture *texture;

	if (descriptor == nil || ![descriptor isKindOfClass:[MTLTextureDescriptor class]]) {
		hm_refusal(self, _cmd, "was given no MTLTextureDescriptor; it returns nil");
		return nil;
	}
	usage = [descriptor usage] == MTLTextureUsageUnknown ? (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)
							      : [descriptor usage];
	usage &= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
	texture = hakios_metal_texture_new_buffer(_buffer, (unsigned)[descriptor textureType], (unsigned)[descriptor pixelFormat],
						  (unsigned)[descriptor width], (unsigned)[descriptor height], (unsigned)usage, (uint64_t)offset,
						  (uint64_t)bytesPerRow);
	if (texture == NULL && (bytesPerRow % 64 != 0 || offset % 64 != 0)) {
		const uint8_t *from = (const uint8_t *)hakios_metal_buffer_contents(_buffer) + offset;

		HM_ONCE("a texture over a buffer's rows of %lu bytes from %lu, which the GPU cannot take as they lie, is a copy of them, "
			"made now\n", (unsigned long)bytesPerRow, (unsigned long)offset);
		texture = hakios_metal_texture_new_levels([_device hakiosDevice], (unsigned)[descriptor textureType], (unsigned)[descriptor pixelFormat],
							  (unsigned)[descriptor width], (unsigned)[descriptor height], 1, (unsigned)usage);
		if (texture != NULL && hakios_metal_texture_replace(texture, 0, 0, (unsigned)[descriptor width], (unsigned)[descriptor height], from,
								    (uint64_t)bytesPerRow) != 0) {
			hakios_metal_texture_free(texture);
			texture = NULL;
		}
	}
	if (texture == NULL) {
		hm_print("refused: a texture over a buffer (%lux%lu, pixel format %lu, rows of %lu bytes from %lu): %s; nil\n",
			 (unsigned long)[descriptor width], (unsigned long)[descriptor height], (unsigned long)[descriptor pixelFormat],
			 (unsigned long)bytesPerRow, (unsigned long)offset, hakios_metal_error());
		return nil;
	}
	return [[HakiosMetalTexture alloc] initWithHakiosDevice:_device texture:texture descriptor:descriptor usage:usage];
}

/* the SPI form QuartzCore sends: the image size beside the rows, which a single 2D level does not need */
- (id)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow
			bytesPerImage:(NSUInteger)bytesPerImage
{
	(void)bytesPerImage;
	return [self newTextureWithDescriptor:descriptor offset:offset bytesPerRow:bytesPerRow];
}

/* debug markers name ranges for a GPU debugger; there is none here, and keeping nothing loses nothing */
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range { (void)marker; (void)range; }
- (void)removeAllDebugMarkers { }

@end

/* ============================================================================================================
 * The texture: hakios-metal's GPU memory in the GPU's own layout, with Metal's vocabulary for a texture over it
 * ============================================================================================================ */

@implementation HakiosMetalTexture

- (unsigned)hakiosStorageKind
{
	return _iosurface != NULL ? 0 : 1 + (unsigned)((_askedOptions >> MTLResourceStorageModeShift) & 0xf);
}

/* The four protocols a texture answers, declared at runtime because no base class brings them: the public <MTLTexture>
 * (compiled into this image by the @protocol() reference) and three of Metal.framework's own, which are in every process
 * that loaded Metal. Where Metal tests a texture's conformance at all it tests MTLTextureImplementation, not MTLTexture
 * (-[MTLRenderPassColorAttachmentDescriptorInternal setTexture:], a render path); the compute path tests nothing. */
+ (void)load
{
	hm_adopt(self, @protocol(MTLTexture), "MTLTexture");
	hm_adopt(self, objc_getProtocol("MTLTextureSPI"), "MTLTextureSPI");
	hm_adopt(self, objc_getProtocol("MTLTextureImplementation"), "MTLTextureImplementation");
	hm_adopt(self, objc_getProtocol("MTLResourceSPI"), "MTLResourceSPI");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device texture:(hakios_metal_texture *)texture
			  descriptor:(MTLTextureDescriptor *)descriptor usage:(MTLTextureUsage)usage
{
	/* -[_MTLObjectWithLabel init] is the designated initialiser of a resource: _MTLResource adds no ivars and no
	 * initialiser of its own, and Apple's own texture chain ends in the same init */
	self = [super init];
	if (self == nil) {
		hakios_metal_texture_free(texture);
		return nil;
	}
	_device = [device retain];
	_texture = texture;
	_usage = usage;
	_askedOptions = [descriptor resourceOptions];
	_allowGPUOptimizedContents = [descriptor allowGPUOptimizedContents];
	_uniqueIdentifier = hm_next_resource_id();
	return self;
}

- (void)dealloc
{
	if (_texture != NULL && (_parent == nil || _ownsView)) {
		hakios_metal_texture_free(_texture);   /* a command buffer still using it keeps its memory */
	}
	if (_iosurface != NULL) {
		CFRelease(_iosurface);
	}
	free(_shadowBase);
	if (_turnedSurface != NULL) {
		hakios_metal_texture_free(_turnedSurface);
	}
	[_parent release];
	if (_deallocator != nil) {
		_deallocator(_deallocBytes, _deallocLength);
		Block_release(_deallocator);
	}
	[_device release];
	[super dealloc];
}

- (hakios_metal_texture *)hakiosTexture { return _texture; }

- (void)hakiosSetDeallocator:(void (^)(void *, NSUInteger))deallocator bytes:(void *)bytes length:(NSUInteger)length
{
	_deallocator = Block_copy(deallocator);
	_deallocBytes = bytes;
	_deallocLength = length;
}

/* --- <MTLResource>: one kind of memory, described as what it is (the buffer says the same) --- */

- (id)device                                { return _device; }
- (MTLStorageMode)storageMode               { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode             { return MTLCPUCacheModeWriteCombined; }
- (MTLResourceOptions)resourceOptions       { return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeDefault; }
- (id)heap                                  { return nil; }
- (NSUInteger)heapOffset                    { return 0; }
- (BOOL)isAliasable                         { return NO; }

/* the layout's own bytes, every level and slice of it (the core's plan, which -heapTextureSizeAndAlignWithDescriptor:
 * answers with too, so that a heap counts a texture as it sized it); a view's are its parent's memory */
- (NSUInteger)allocatedSize
{
	return (NSUInteger)hakios_metal_texture_allocated_bytes(_texture);
}

/* a texture is its own kernel allocation, as a buffer is; there is no heap to alias within */
- (void)makeAliasable
{
	HM_ONCE("a texture was asked to become aliasable; every texture here is its own allocation and none aliases another\n");
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
	if (state == MTLPurgeableStateVolatile || state == MTLPurgeableStateEmpty) {
		HM_ONCE("a texture was asked to become purgeable; the kernel does not purge GPU memory here, so it stays non-volatile\n");
	}
	return MTLPurgeableStateNonVolatile;
}

/* --- <MTLResourceSPI> --- */

/* complete the moment it exists: the memory is mapped and -replaceRegion: has copied when it returns; nothing is deferred
 * (CoreAnimation's finish_image waits on this, and NO would be a wait for ever) */
- (BOOL)isComplete                               { return YES; }
- (void)waitUntilComplete                        { }
- (MTLResourceOptions)unfilteredResourceOptions  { return _askedOptions; }
- (NSUInteger)protectionOptions                  { return 0; }   /* no protected memory on this port */
- (BOOL)isPurgeable                              { return NO; }
- (BOOL)doesAliasResource:(id)resource           { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count { (void)resources; (void)count; return NO; }
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count { (void)resources; (void)count; return NO; }
- (int)responsibleProcess                        { return 0; }   /* no accounting of GPU memory by process in this kernel */
- (void)setResponsibleProcess:(int)pid           { (void)pid; }

/* --- <MTLTexture>: the shape --- */

- (NSUInteger)width                 { return hakios_metal_texture_width(_texture); }
- (NSUInteger)height                { return hakios_metal_texture_height(_texture); }
- (NSUInteger)depth                 { return hakios_metal_texture_type(_texture) == MTLTextureType3D ? hakios_metal_texture_depth(_texture) : 1; }
- (MTLPixelFormat)pixelFormat       { return _ycbcr != 0 ? _ycbcrFormat : (MTLPixelFormat)hakios_metal_texture_format(_texture); }
- (MTLTextureType)textureType       { return (MTLTextureType)hakios_metal_texture_type(_texture); }
- (MTLTextureUsage)usage            { return _usage; }
- (NSUInteger)mipmapLevelCount      { return hakios_metal_texture_levels(_texture); }
- (NSUInteger)arrayLength
{
	const unsigned type = hakios_metal_texture_type(_texture);

	return type == MTLTextureType2DArray || type == MTLTextureType1DArray ? hakios_metal_texture_depth(_texture) :
	       type == MTLTextureTypeCubeArray ? hakios_metal_texture_depth(_texture) / 6 : 1;
}
- (NSUInteger)sampleCount           { return hakios_metal_texture_samples(_texture); }
- (BOOL)isFramebufferOnly           { return NO; }    /* every texture here can be read back and written by the processor */
- (BOOL)allowGPUOptimizedContents   { return _allowGPUOptimizedContents; }
- (id)parentTexture                 { return _parent; }
- (NSUInteger)parentRelativeLevel   { return _parentLevel; }
- (NSUInteger)parentRelativeSlice   { return _parentSlice; }
- (id)rootResource                  { return self; }
- (id)buffer                        { return nil; }   /* not a view of an MTLBuffer either: the memory is the texture's own */
- (NSUInteger)bufferOffset          { return 0; }
- (NSUInteger)bufferBytesPerRow     { return (NSUInteger)hakios_metal_texture_bytes_per_row(_texture); }   /* the GPU's pitch */
- (IOSurfaceRef)iosurface           { return _iosurface; }
- (NSUInteger)iosurfacePlane        { return _iosurfacePlane; }
/* A surface whose rows the GPU cannot take as they lie (a pitch that is not a multiple of 64 bytes: a client that chose
 * its own bytesPerRow, 480 for a 120-pixel BGRA picture) is read through a copy of its rows in a texture of the GPU's
 * layout. The copy is made again whenever the surface's seed has moved, which IOSurface does on every unlock after a
 * write, at the moment the texture is bound for a draw -- so a draw sees the surface as it was when the draw was encoded,
 * which is what a sample of a surface another program is still writing could show anyway. */
- (BOOL)hakiosIsShadow { return _shadow; }
- (BOOL)hakiosIsTurned { return _turnedSurface != NULL; }

- (void)hakiosSetTurned:(hakios_metal_texture *)surface rotation:(unsigned)rotation
{
	_turnedSurface = surface;
	_turn = rotation;
}

/* the framebuffer as drawn, turned into the surface the display scans out */
- (void)hakiosTurnInto:(hakios_metal_cb *)cb
{
	if (_turnedSurface != NULL && hakios_metal_cb_blit_rotated(cb, _texture, _turnedSurface, _turn) != 0) {
		HM_ONCE("refused: a turned framebuffer's copy into its surface: %s\n", hakios_metal_error());
	}
}

/* A copy a kernel or a pass may have written: its rows back into the surface, the surface's seed moving as for any
 * write, and the copy then current with it (the next refresh does not read back what was just written). */
- (void)hakiosWriteBackShadow
{
	if (_parent != nil) {
		[_parent hakiosWriteBackShadow];
		return;
	}
	if (!_shadow || _iosurface == NULL || (_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) == 0) {
		return;
	}
	/* Only the bytes the GPU changed from the base go back. CoreAnimation draws one surface (the App Library's
	 * snapshot) in several passes through several textures, each a copy of its own, and the next pass's copy is taken
	 * when it is encoded -- before the last one's command buffer has written back when the GPU is busy. Writing a
	 * whole copy back then put its stale rows over what the other pass drew: black bands that moved from frame to
	 * frame. Against the base, each copy carries only its own drawing, and passes into different parts merge. */
	const size_t bytes = IOSurfaceGetBytesPerRow(_iosurface) * IOSurfaceGetHeight(_iosurface);
	const double began = hm_now();
	uint8_t *now = malloc(bytes);

	if (now == NULL || IOSurfaceLock(_iosurface, 0, NULL) != kIOReturnSuccess) {
		free(now);
		return;
	}
	uint8_t *surface = IOSurfaceGetBaseAddress(_iosurface);

	memcpy(now, _shadowBase != NULL ? _shadowBase : surface, bytes);
	(void)hakios_metal_texture_read(_texture, 0, 0, hakios_metal_texture_width(_texture), hakios_metal_texture_height(_texture), now,
					(uint64_t)IOSurfaceGetBytesPerRow(_iosurface));
	if (_shadowBase == NULL) {
		memcpy(surface, now, bytes);
	} else {
		for (size_t i = 0; i < bytes; i++) {
			if (now[i] != _shadowBase[i]) {
				surface[i] = now[i];
			}
		}
	}
	IOSurfaceUnlock(_iosurface, 0, NULL);
	free(_shadowBase);
	_shadowBase = now;
	_shadowSeed = IOSurfaceGetSeed(_iosurface);
	hm_fps_shadow(NO, hm_now() - began);
}

- (void)hakiosShadow:(IOSurfaceRef)surface
{
	[self hakiosSetIOSurface:surface plane:0];
	_shadow = YES;
	_shadowSeed = IOSurfaceGetSeed(surface) + 1;   /* (not the surface's seed: the first refresh copies) */
	[self hakiosRefreshShadow];
}

/* A '420f' or '420v' surface sampled as Metal's private format 500. Apple's texture unit converts YCbCr to RGB as it
 * samples, and CoreAnimation's shader (fixed_frag_lph_cph) takes the sample as the colour: it does no conversion of
 * its own. The A6xx texture unit has no such conversion -- it returns (Y, Cb, Cr) as they are (probe 9m2), which drew
 * the home screen's wallpaper with its orange pink and its teal olive. So such a texture is an RGBA8 copy converted
 * here, from the surface's matrix (its IOSurfaceYCbCrMatrix attachment: BT.709 when it says so, BT.601 otherwise,
 * which is what the wallpaper's own HEIC declares) and range, made again when the surface's seed moves as the other
 * copies are. Each 2x2 block of luma takes its one chroma sample (the 4:2:0 siting). */
- (void)hakiosShadowYCbCr:(IOSurfaceRef)surface format:(MTLPixelFormat)format
{
	CFTypeRef matrix = IOSurfaceCopyValue(surface, CFSTR("IOSurfaceYCbCrMatrix"));

	_ycbcr = 1;
	_ycbcrFormat = format;
	if (matrix != NULL) {
		if (CFGetTypeID(matrix) == CFStringGetTypeID() && CFStringFind((CFStringRef)matrix, CFSTR("709"), 0).location != kCFNotFound) {
			_ycbcr = 2;
		}
		CFRelease(matrix);
	}
	_ycbcrFull = IOSurfaceGetPixelFormat(surface) != 0x34323076;   /* not '420v' */
	[self hakiosShadow:surface];
}

static uint8_t hm_clamp8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

- (void)hakiosConvertYCbCr
{
	const size_t w = hakios_metal_texture_width(_texture), h = hakios_metal_texture_height(_texture);
	const uint8_t *luma = IOSurfaceGetBaseAddressOfPlane(_iosurface, 0), *chroma = IOSurfaceGetBaseAddressOfPlane(_iosurface, 1);
	const size_t lumaRow = IOSurfaceGetBytesPerRowOfPlane(_iosurface, 0), chromaRow = IOSurfaceGetBytesPerRowOfPlane(_iosurface, 1);
	const size_t cw = IOSurfaceGetWidthOfPlane(_iosurface, 1), ch = IOSurfaceGetHeightOfPlane(_iosurface, 1);
	/* Kr, Kb of the matrix; G from Y = Kr R + Kg G + Kb B */
	const float kr = _ycbcr == 2 ? 0.2126f : 0.299f, kb = _ycbcr == 2 ? 0.0722f : 0.114f, kg = 1.0f - kr - kb;
	const float ys = _ycbcrFull ? 1.0f : 255.0f / 219.0f, yo = _ycbcrFull ? 0.0f : 16.0f, cs = _ycbcrFull ? 1.0f : 255.0f / 224.0f;
	uint8_t *rows = malloc(w * h * 4);

	if (rows == NULL || luma == NULL || chroma == NULL || cw == 0 || ch == 0) {
		free(rows);
		return;
	}
	for (size_t j = 0; j < h; j++) {
		const uint8_t *yl = luma + j * lumaRow, *cl = chroma + MIN(j / 2, ch - 1) * chromaRow;
		uint8_t *out = rows + j * w * 4;

		for (size_t i = 0; i < w; i++) {
			const size_t c = MIN(i / 2, cw - 1) * 2;

			/* RGB8_420_2P (540) is the planes as they are, the chroma at each pixel: R Cr, G Y, B Cb. VideoToolbox's
			 * converter samples it, takes (G, B, R) as (Y, Cb, Cr) and applies the matrix itself; converted here
			 * too, the live wallpaper came out in colours it never had. */
			if (_ycbcrFormat == (MTLPixelFormat)540) {
				out[4 * i] = cl[c + 1];
				out[4 * i + 1] = yl[i];
				out[4 * i + 2] = cl[c];
				out[4 * i + 3] = 255;
				continue;
			}
			const float y = (yl[i] - yo) * ys, cb = (cl[c] - 128.0f) * cs, cr = (cl[c + 1] - 128.0f) * cs;
			const float r = y + 2.0f * (1.0f - kr) * cr, b = y + 2.0f * (1.0f - kb) * cb;
			const float g = (y - kr * r - kb * b) / kg;

			out[4 * i] = hm_clamp8(r);
			out[4 * i + 1] = hm_clamp8(g);
			out[4 * i + 2] = hm_clamp8(b);
			out[4 * i + 3] = 255;
		}
	}
	(void)hakios_metal_texture_replace(_texture, 0, 0, (unsigned)w, (unsigned)h, rows, (uint64_t)w * 4);
	free(rows);
}

- (void)hakiosRefreshShadow
{
	if (_parent != nil) {
		[_parent hakiosRefreshShadow];   /* the rows a view reads are its parent's, copy included */
		return;
	}
	uint32_t seed;

	if (!_shadow || _iosurface == NULL || (seed = IOSurfaceGetSeed(_iosurface)) == _shadowSeed) {
		return;
	}
	if (IOSurfaceLock(_iosurface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
		return;
	}
	const double began = hm_now();
	if (_ycbcr != 0) {
		[self hakiosConvertYCbCr];
	} else {
		(void)hakios_metal_texture_replace(_texture, 0, 0, hakios_metal_texture_width(_texture), hakios_metal_texture_height(_texture),
						   IOSurfaceGetBaseAddress(_iosurface), (uint64_t)IOSurfaceGetBytesPerRow(_iosurface));
		/* a copy something writes keeps what it took, for -hakiosWriteBackShadow to tell its own drawing from */
		if ((_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) != 0) {
			const size_t bytes = IOSurfaceGetBytesPerRow(_iosurface) * IOSurfaceGetHeight(_iosurface);

			if (_shadowBase == NULL) {
				_shadowBase = malloc(bytes);
			}
			if (_shadowBase != NULL) {
				memcpy(_shadowBase, IOSurfaceGetBaseAddress(_iosurface), bytes);
			}
		}
	}
	IOSurfaceUnlock(_iosurface, kIOSurfaceLockReadOnly, NULL);
	_shadowSeed = seed;
	hm_fps_shadow(YES, hm_now() - began);
}

- (void)hakiosSetIOSurface:(IOSurfaceRef)surface plane:(NSUInteger)plane
{
	if (surface != NULL) {
		CFRetain(surface);
	}
	if (_iosurface != NULL) {
		CFRelease(_iosurface);
	}
	_iosurface = surface;
	_iosurfacePlane = plane;
}
- (BOOL)isShareable                 { return NO; }
- (BOOL)isSparse                    { return NO; }
- (NSUInteger)firstMipmapInTail     { return 0; }
- (NSUInteger)tailSizeInBytes       { return 0; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
/* the identity: red 2, green 3, blue 4, alpha 5 (MTLTextureSwizzle). The catch-all's zeros would be four
 * MTLTextureSwizzleZero, a texture every channel of which reads 0. */
- (MTLTextureSwizzleChannels)swizzle { return _swizzled ? _swizzle : MTLTextureSwizzleChannelsDefault; }

/* what an argument buffer holds for this texture: the core's id, which a draw or dispatch reading the buffer finds the
 * texture by (a view of a level or slices is a texture of its own, with its own) */
- (MTLResourceID)gpuResourceID
{
	MTLResourceID rid = { hakios_metal_texture_resource_id(_texture) };

	return rid;
}

/* A shared texture handle names GPU memory to another process; the kernel's allocations are per client here. */
- (id)newSharedTextureHandle
{
	hm_refusal(self, _cmd, "has no shared textures to hand across processes; it returns nil");
	return nil;
}

/* A view is a second descriptor over the same memory. The one this plug-in makes is the view that changes nothing --
 * same format, type, every level, every slice -- which is what sokol-gfx asks for every image it samples (it binds
 * views, never images): such a view IS the texture under a second object, so it shares the parent's hakios texture and
 * keeps the parent alive. A view that reinterprets the format or picks out levels needs a descriptor of its own, which
 * hakios-metal does not make yet; LOUD because of what a quiet nil did in the earlier plug-in: CoreAnimation's
 * update_image stored it, unchecked, over a texture it already had, and images composited as nothing. */
- (id)hakiosViewWithPixelFormat:(MTLPixelFormat)pixelFormat textureType:(MTLTextureType)type levels:(NSRange)levels
			 slices:(NSRange)slices asked:(SEL)cmd
{
	HakiosMetalTexture *view;
	hakios_metal_texture *own = NULL;
	/* the texture's slices, as Metal counts a view's: a 2D array's layers, a cube's six faces, one for the rest */
	const NSUInteger allSlices = [self textureType] == MTLTextureType2DArray || [self textureType] == MTLTextureType1DArray ? [self arrayLength] :
				     [self textureType] == MTLTextureTypeCubeArray ? 6 * [self arrayLength] : [self textureType] == MTLTextureTypeCube ? 6 : 1;

	/* the whole texture as it is shares its handle; anything else (a level, a slice, another format of the same size a
	 * pixel -- MPS takes each level of a pyramid so) is the core's own view of the same memory */
	if (pixelFormat != [self pixelFormat] || type != [self textureType] || levels.location != 0 ||
	    levels.length != [self mipmapLevelCount] || slices.location != 0 || slices.length != allSlices) {
		own = hakios_metal_texture_new_view(_texture, (unsigned)pixelFormat, (unsigned)type, (unsigned)levels.location, (unsigned)levels.length,
						    (unsigned)slices.location, (unsigned)slices.length);
		if (own == NULL) {
			hm_print("refused: -[%s %s] format %lu type %lu levels %lu+%lu slices %lu+%lu of a texture of format %lu type %lu "
				 "with %lu levels: %s; it returns nil\n",
				 class_getName(object_getClass(self)), sel_getName(cmd), (unsigned long)pixelFormat, (unsigned long)type,
				 (unsigned long)levels.location, (unsigned long)levels.length, (unsigned long)slices.location,
				 (unsigned long)slices.length, (unsigned long)[self pixelFormat], (unsigned long)[self textureType],
				 (unsigned long)[self mipmapLevelCount], hakios_metal_error());
			return nil;
		}
	}
	view = [[HakiosMetalTexture alloc] init];   /* -[_MTLObjectWithLabel init], as the designated initialiser above */
	if (view == nil) {
		if (own != NULL) hakios_metal_texture_free(own);
		return nil;
	}
	view->_parent = [self retain];
	view->_device = [_device retain];
	view->_texture = own != NULL ? own : _texture;
	view->_ownsView = own != NULL;
	view->_parentLevel = levels.location;
	view->_parentSlice = slices.location;
	view->_usage = _usage;
	view->_askedOptions = _askedOptions;
	view->_allowGPUOptimizedContents = _allowGPUOptimizedContents;
	view->_uniqueIdentifier = hm_next_resource_id();
	if (_iosurface != NULL) {
		view->_iosurface = (IOSurfaceRef)CFRetain(_iosurface);
		view->_iosurfacePlane = _iosurfacePlane;
	}
	return view;
}

- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat
{
	return [self hakiosViewWithPixelFormat:pixelFormat textureType:[self textureType]
					levels:NSMakeRange(0, [self mipmapLevelCount]) slices:NSMakeRange(0, 1) asked:_cmd];
}

- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices
{
	return [self hakiosViewWithPixelFormat:pixelFormat textureType:type levels:levels slices:slices asked:_cmd];
}

- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices
			    swizzle:(MTLTextureSwizzleChannels)swizzle
{
	HakiosMetalTexture *view = [self hakiosViewWithPixelFormat:pixelFormat textureType:type levels:levels slices:slices asked:_cmd];
	hakios_metal_texture *swizzled;

	if (view == nil || (swizzle.red == MTLTextureSwizzleRed && swizzle.green == MTLTextureSwizzleGreen &&
			    swizzle.blue == MTLTextureSwizzleBlue && swizzle.alpha == MTLTextureSwizzleAlpha)) {
		return view;
	}
	/* the same memory through a descriptor of its own, its swizzle over the view's (the core composes them) */
	swizzled = hakios_metal_texture_new_swizzled(view->_texture, swizzle.red, swizzle.green, swizzle.blue, swizzle.alpha);
	if (swizzled == NULL) {
		hm_print("refused: a view swizzled (%u %u %u %u): %s; it returns nil\n", swizzle.red, swizzle.green, swizzle.blue, swizzle.alpha,
			 hakios_metal_error());
		[view release];
		return nil;
	}
	if (view->_ownsView) hakios_metal_texture_free(view->_texture);
	view->_texture = swizzled;
	view->_ownsView = YES;
	view->_swizzled = YES;
	view->_swizzle = swizzle;
	return view;
}

/* --- <MTLTexture>: the bytes --- */

/* a level or slice this texture has not got, a region with depth, or no bytes at all: refused by name and nothing is
 * copied, because a copy into level 0 instead would read as a result */
- (BOOL)hakiosRegion:(MTLRegion)region level:(NSUInteger)level slice:(NSUInteger)slice bytes:(const void *)bytes isFor:(const char *)what
{
	const BOOL layered = hm_layered(hakios_metal_texture_type(_texture));
	if (level >= hakios_metal_texture_levels(_texture) || (slice != 0 && !(layered && slice < hakios_metal_texture_depth(_texture)))) {
		hm_print("refused: %s of level %lu, slice %lu: this texture has %u level(s) and one slice; nothing is copied\n",
			 what, (unsigned long)level, (unsigned long)slice, hakios_metal_texture_levels(_texture));
		return NO;
	}
	if ((region.origin.z != 0 || region.size.depth != 1) && hakios_metal_texture_type(_texture) != MTLTextureType3D) {
		hm_print("refused: %s of a region at z %lu with depth %lu: a 2D texture has z 0 and depth 1; nothing is copied\n",
			 what, (unsigned long)region.origin.z, (unsigned long)region.size.depth);
		return NO;
	}
	if (bytes == NULL) {
		hm_print("refused: %s with no bytes; nothing is copied\n", what);
		return NO;
	}
	return YES;
}

/* The 4-argument form is Apple's own convenience over the 6-argument one: -[IOGPUMetalTexture replaceRegion:mipmapLevel:
 * withBytes:bytesPerRow:] sends the 6-argument form with slice 0 and bytesPerImage 0, and AGXTexture overrides only the
 * 6-argument forms. The same shape here, so that one implementation holds every rule. */
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bytesPerRow
{
	[self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:bytesPerRow bytesPerImage:0];
}

- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)bytesPerRow bytesPerImage:(NSUInteger)bytesPerImage
{
	if (![self hakiosRegion:region level:level slice:slice bytes:bytes isFor:"replaceRegion"]) {
		return;
	}
	/* a 2D array's layer SLICE, or a cube's face: the core's slice copy, one slice at the layer */
	if (hm_layered(hakios_metal_texture_type(_texture))) {
		if (hakios_metal_texture_replace_3d(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y, (unsigned)slice, (unsigned)region.size.width,
						    (unsigned)region.size.height, 1, bytes, bytesPerRow, bytesPerImage) != 0) {
			hm_print("refused: replaceRegion of %lux%lu of layer %lu of a 2D array: %s; nothing is copied\n", (unsigned long)region.size.width,
				 (unsigned long)region.size.height, (unsigned long)slice, hakios_metal_error());
		}
		return;
	}
	/* a 3D texture's slices, each image BYTES_PER_IMAGE further on in the caller's memory (ignored for one image) */
	if (hakios_metal_texture_type(_texture) == MTLTextureType3D) {
		if (hakios_metal_texture_replace_3d(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y, (unsigned)region.origin.z,
						    (unsigned)region.size.width, (unsigned)region.size.height, (unsigned)region.size.depth, bytes,
						    bytesPerRow, bytesPerImage) != 0) {
			hm_print("refused: replaceRegion of %lux%lux%lu of a 3D texture: %s; nothing is copied\n", (unsigned long)region.size.width,
				 (unsigned long)region.size.height, (unsigned long)region.size.depth, hakios_metal_error());
		}
		return;
	}
	/* BYTES_PER_ROW is the CALLER's stride for the caller's memory; the texture's rows are hakios-metal's pitch apart, and
	 * the core copies row by row between the two (a region outside the texture, or a stride shorter than its rows, is
	 * its refusal, with the numbers) */
	if (hakios_metal_texture_replace_level(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,
					       (unsigned)region.size.width, (unsigned)region.size.height, bytes, bytesPerRow) != 0) {
		hm_print("refused: replaceRegion of %lux%lu at (%lu, %lu), the caller's rows %lu bytes apart: %s; nothing is copied\n",
			 (unsigned long)region.size.width, (unsigned long)region.size.height, (unsigned long)region.origin.x,
			 (unsigned long)region.origin.y, (unsigned long)bytesPerRow, hakios_metal_error());
	}
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:bytesPerRow bytesPerImage:0 fromRegion:region mipmapLevel:level slice:0];
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bytesPerRow bytesPerImage:(NSUInteger)bytesPerImage fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
{
	if (![self hakiosRegion:region level:level slice:slice bytes:bytes isFor:"getBytes"]) {
		return;
	}
	if (hm_layered(hakios_metal_texture_type(_texture))) {
		if (hakios_metal_texture_read_3d(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y, (unsigned)slice, (unsigned)region.size.width,
						 (unsigned)region.size.height, 1, bytes, bytesPerRow, bytesPerImage) != 0) {
			hm_print("refused: getBytes of %lux%lu of layer %lu of a 2D array: %s; nothing is copied\n", (unsigned long)region.size.width,
				 (unsigned long)region.size.height, (unsigned long)slice, hakios_metal_error());
		}
		return;
	}
	if (hakios_metal_texture_type(_texture) == MTLTextureType3D) {
		if (hakios_metal_texture_read_3d(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y, (unsigned)region.origin.z,
						 (unsigned)region.size.width, (unsigned)region.size.height, (unsigned)region.size.depth, bytes, bytesPerRow,
						 bytesPerImage) != 0) {
			hm_print("refused: getBytes of %lux%lux%lu of a 3D texture: %s; nothing is copied\n", (unsigned long)region.size.width,
				 (unsigned long)region.size.height, (unsigned long)region.size.depth, hakios_metal_error());
		}
		return;
	}
	if (hakios_metal_texture_read_level(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,
					    (unsigned)region.size.width, (unsigned)region.size.height, bytes, bytesPerRow) != 0) {
		hm_print("refused: getBytes of %lux%lu at (%lu, %lu), the caller's rows %lu bytes apart: %s; nothing is copied\n",
			 (unsigned long)region.size.width, (unsigned long)region.size.height, (unsigned long)region.origin.x,
			 (unsigned long)region.origin.y, (unsigned long)bytesPerRow, hakios_metal_error());
	}
}

/* --- <MTLTextureSPI> --- */

- (BOOL)isCompressed                      { return NO; }   /* linear rows; no lossless compression on this GPU's path */
- (NSInteger)compressionFeedback          { return 0; }
- (BOOL)isDrawable                        { return NO; }   /* no CAMetalLayer hands these out */
- (unsigned)swizzleKey                    { return 0; }    /* the identity swizzle's key */
- (NSUInteger)numFaces                    { return 1; }    /* a cube has six; this is not one */
- (NSUInteger)rotation                    { return 0; }
- (uint64_t)uniqueIdentifier              { return _uniqueIdentifier; }
- (uint64_t)gpuHandle                     { return _uniqueIdentifier; }
- (uint64_t)gpuAddress                    { return hakios_metal_texture_gpu_address(_texture); }   /* real: the Bo's */
- (uint64_t)sparseSurfaceDefaultValue     { return 0; }    /* nothing is sparse, so nothing is unmapped */

/* the index in a resource table, for argument buffers: there is no table here (the dispatch binds by descriptor), so the
 * answer is 0 and the line is what makes a caller that writes it somewhere readable in the log */
- (NSUInteger)resourceIndex
{
	HM_ONCE("a texture's resourceIndex was asked for; this plug-in has no resource table (no argument buffers), and answers 0\n");
	return 0;
}

/* --- <MTLTextureImplementation> --- */

/* what Metal's diagnostics print for this texture; the argument is Apple's indent level */
- (NSString *)formattedDescription:(NSUInteger)level
{
	(void)level;
	return [NSString stringWithFormat:@"<%s %p \"%@\": %lux%lu, pixel format %lu, usage %#lx, rows %lu bytes apart (the GPU's "
					  "pitch), GPU address %#llx, id %llu>", class_getName(object_getClass(self)), self, [self label],
					  (unsigned long)[self width], (unsigned long)[self height], (unsigned long)[self pixelFormat],
					  (unsigned long)_usage, (unsigned long)[self bufferBytesPerRow],
					  (unsigned long long)hakios_metal_texture_gpu_address(_texture), (unsigned long long)_uniqueIdentifier];
}

@end

/* ============================================================================================================
 * The sampler state: four TEX_SAMP words of hakios-metal's, under Metal's own base class
 * ============================================================================================================ */

@implementation HakiosMetalDepthStencilState

- (instancetype)initWithDevice:(id<MTLDevice>)device descriptor:(MTLDepthStencilDescriptor *)descriptor
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = [device retain];
	_label = [[descriptor label] copy];
	_compare = (unsigned)[descriptor depthCompareFunction];
	_write = [descriptor isDepthWriteEnabled];
	/* a face Metal leaves nil is its default, Always / Keep / Keep / Keep with all-ones masks; the test is on only when
	 * some face can change a pixel's fate or the stencil, so that a state with the default faces costs no stencil
	 * registers and binds on a pass with no stencil attachment as Metal lets it */
	{
		MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil], [descriptor backFaceStencil] };
		struct hakios_metal_stencil_face *out[2] = { &_front, &_back };

		for (int i = 0; i < 2; i++) {
			MTLStencilDescriptor *f = faces[i];
			struct hakios_metal_stencil_face *o = out[i];

			o->compare = f != nil ? (unsigned)[f stencilCompareFunction] : (unsigned)MTLCompareFunctionAlways;
			o->stencil_fail = f != nil ? (unsigned)[f stencilFailureOperation] : (unsigned)MTLStencilOperationKeep;
			o->depth_fail = f != nil ? (unsigned)[f depthFailureOperation] : (unsigned)MTLStencilOperationKeep;
			o->pass = f != nil ? (unsigned)[f depthStencilPassOperation] : (unsigned)MTLStencilOperationKeep;
			o->read_mask = f != nil ? (unsigned)[f readMask] & 0xff : 0xff;
			o->write_mask = f != nil ? (unsigned)[f writeMask] & 0xff : 0xff;
			if (o->compare != (unsigned)MTLCompareFunctionAlways || o->stencil_fail != (unsigned)MTLStencilOperationKeep ||
			    o->depth_fail != (unsigned)MTLStencilOperationKeep || o->pass != (unsigned)MTLStencilOperationKeep) {
				_stencil = YES;
			}
		}
	}
	return self;
}

- (void)dealloc
{
	[_device release];
	[_label release];
	[super dealloc];
}

- (NSString *)label { return _label; }
- (id<MTLDevice>)device { return _device; }
- (MTLResourceID)gpuResourceID { MTLResourceID r = { 0 }; return r; }   /* argument buffers are not served */
- (unsigned)hakiosDepthCompare { return _compare; }
- (BOOL)hakiosDepthWrite { return _write; }
- (BOOL)hakiosStencil { return _stencil; }
- (const struct hakios_metal_stencil_face *)hakiosStencilFront { return &_front; }
- (const struct hakios_metal_stencil_face *)hakiosStencilBack { return &_back; }

@end

@implementation HakiosMetalSamplerState

/* the base declares <MTLSamplerStateSPI>, which adopts the public protocol; declared again here so that a
 * conformsToProtocol: on the public name is a static yes whatever the SPI's adoption list says in this Metal */
+ (void)load
{
	hm_adopt(self, @protocol(MTLSamplerState), "MTLSamplerState");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device sampler:(hakios_metal_sampler *)sampler descriptor:(MTLSamplerDescriptor *)descriptor
{
	/* the designated initialiser, given only what -newSamplerStateWithDescriptor: has already checked it accepts: it
	 * keeps the device (retained), the label (copied) and the pixel format, and answers <MTLSamplerState> from them */
	self = [super initWithDevice:device samplerDescriptor:descriptor];
	if (self == nil) {
		hakios_metal_sampler_free(sampler);
		return nil;
	}
	_hakiosSampler = sampler;
	_hakiosUniqueIdentifier = hm_next_resource_id();
	return self;
}

- (void)dealloc
{
	if (_hakiosSampler != NULL) {
		hakios_metal_sampler_free(_hakiosSampler);   /* a dispatch copied its four words; nothing in flight points at it */
	}
	[super dealloc];
}

- (hakios_metal_sampler *)hakiosSampler { return _hakiosSampler; }

/* The base's -gpuResourceID is -gpuHandle is -uniqueIdentifier, and the last is a nil-stub: a sampler would have
 * gpuResourceID 0, which an argument-buffer encoder would write as a handle. uniqueIdentifier is answered from the
 * textures' counter (never 0, never an address); gpuResourceID is the core's id, what an argument buffer holds for the
 * sampler and a draw or dispatch reading it finds the sampler by. */
- (uint64_t)uniqueIdentifier { return _hakiosUniqueIdentifier; }
- (MTLResourceID)gpuResourceID
{
	MTLResourceID rid = { hakios_metal_sampler_resource_id(_hakiosSampler) };

	return rid;
}

@end

/* ============================================================================================================
 * The compute pipeline state: one compiled kernel
 * ============================================================================================================ */

@implementation HakiosMetalComputePipelineState

+ (void)load
{
	hm_adopt(self, @protocol(MTLComputePipelineState), "MTLComputePipelineState");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device kernel:(hakios_metal_kernel *)kernel descriptor:(id)descriptor
{
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		hakios_metal_kernel_free(kernel);
		return nil;
	}
	_device = [device retain];
	_kernel = kernel;
	_label = [[descriptor label] copy];
	return self;
}

- (void)dealloc
{
	if (_kernel != NULL) {
		hakios_metal_kernel_free(_kernel);   /* a dispatch in flight keeps its own reference */
	}
	[_device release];
	[_label release];
	[super dealloc];
}

- (hakios_metal_kernel *)hakiosKernel { return _kernel; }

/* the three numbers the base class answers with nothing: they come from whoever compiled the kernel */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return hakios_metal_kernel_max_threads(_kernel); }
- (NSUInteger)threadExecutionWidth          { return hakios_metal_kernel_execution_width(_kernel); }
- (NSUInteger)staticThreadgroupMemoryLength { return hakios_metal_kernel_threadgroup_memory(_kernel); }
- (id)device                                { return _device; }
- (NSString *)label                         { return _label; }
- (BOOL)supportIndirectCommandBuffers       { return NO; }   /* a dispatch's grid is read here, not by the GPU */

@end

/* ============================================================================================================
 * The command queue
 * ============================================================================================================ */

/* Apple's own semaphore on the queue, by name. A Metal that keeps it elsewhere is said out loud: the right answer then
 * is to find out, not to run with a queue that blocks on its 64th command buffer without a word. */
static dispatch_semaphore_t
hm_queue_semaphore(id queue)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		looked = YES;
		ivar = hm_ivar("_MTLCommandQueue", "_commandBufferSemaphore");
		if (ivar == NULL) {
			hm_print("_MTLCommandQueue has no _commandBufferSemaphore in this Metal: the slot every command buffer "
				 "takes cannot be given back, and a queue will block on its maxCommandBufferCount'th buffer\n");
		}
	}
	return ivar != NULL ? (dispatch_semaphore_t)object_getIvar(queue, ivar) : nil;
}

/* Apple's own completion-queue ivar, for the same reason: the base class's getter reads it, so a queue made from a
 * descriptor that carried one must read back as what it is */
static Ivar
hm_completion_ivar(void)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		looked = YES;
		ivar = hm_ivar("_MTLCommandQueue", "_completionQueue");
		if (ivar == NULL) {
			hm_print("_MTLCommandQueue has no _completionQueue in this Metal; -setCompletionQueue: keeps it in the plug-in's own field\n");
		}
	}
	return ivar;
}

@implementation HakiosMetalCommandQueue

+ (void)load
{
	hm_adopt(self, @protocol(MTLCommandQueue), "MTLCommandQueue");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device descriptor:(id)descriptor
{
	/* the initialiser IOGPUMetalCommandQueue sends to super; the descriptor must not be nil (the base asserts) */
	self = [super initWithDevice:device descriptor:descriptor];
	if (self == nil) {
		return nil;
	}
	_queue = hakios_metal_queue_new([device hakiosDevice]);
	if (_queue == NULL) {
		hm_print("refused: a command queue: %s\n", hakios_metal_error());
		[self release];
		return nil;
	}
	_device = [device retain];
	HM_ONCE("a command queue takes %lu command buffers at a time (maxCommandBufferCount); each one waits on the queue's "
		"semaphore in -[_MTLCommandBuffer initWithQueue:...], and this plug-in gives the slot back when the buffer "
		"completes%s\n", (unsigned long)[self maxCommandBufferCount],
		hm_queue_semaphore(self) != nil ? "" : ", EXCEPT that the semaphore was not found and it cannot");
	return self;
}

- (void)dealloc
{
	if (_queue != NULL) {
		hakios_metal_queue_free(_queue);
	}
	[_device release];
	[_completionQueue release];   /* only the plug-in's own copy; Apple's ivar is -[_MTLCommandQueue dealloc]'s */
	if (_deferred != NULL) {
		dispatch_release(_deferred);   /* every block on it holds a command buffer, which holds this queue: none is left */
	}
	[super dealloc];
}

- (hakios_metal_queue *)hakiosQueue { return _queue; }
- (id)device { return _device; }

- (id)commandBuffer
{
	return [[[HakiosMetalCommandBuffer alloc] initWithHakiosQueue:self] autorelease];
}

/* retained and unretained references are the same here: hakios-metal keeps every buffer and kernel a command buffer
 * names alive until the GPU is done with it, whatever the application releases meanwhile */
- (id)commandBufferWithUnretainedReferences { return [self commandBuffer]; }

- (id)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)descriptor
{
	if (descriptor != nil && [descriptor errorOptions] != MTLCommandBufferErrorOptionNone) {
		HM_ONCE("a command buffer was asked for with error options %lu (encoder execution status); the error this "
			"plug-in reports is hakios-metal's sentence, without per-encoder status\n", (unsigned long)[descriptor errorOptions]);
	}
	return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary { }   /* there is no GPU capture tool to mark a boundary for */

/*
 * What the system's daemons send a queue first (backboardd and mediaserverd: 58 -setGPUPriority: and 30
 * -setCompletionQueue: in a hundred seconds, each one an unrecognized-selector abort until answered). Both are required
 * members of <MTLCommandQueueSPI>, which is declared in the AGX bundle and so in no process here. -setGPUPriority: is
 * not bookkeeping in Apple's driver -- the number goes to the GPU's scheduler -- but hakios-metal has one ring and one
 * priority, so the number is remembered, reported back and said once not to schedule anything. The range check is
 * Apple's own (0..5, and 3 is background-only), and like Apple's it reports rather than refuses.
 */
- (BOOL)setGPUPriority:(NSUInteger)priority
{
	if (priority > 5 || priority == 3) {
		HM_ONCE("GPU priority %lu is outside what Apple's _validateGPUPriority allows (0..5, 3 background-only); kept as asked\n",
			(unsigned long)priority);
	}
	HM_ONCE("a GPU priority of %lu was asked for; this plug-in has one ring and one priority, so the number is remembered "
		"and reported back but nothing schedules by it\n", (unsigned long)priority);
	_gpuPriority = priority;
	return YES;
}

- (BOOL)setGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	if (offset != 0) {
		HM_ONCE("a GPU priority offset of %u is ignored: one ring has no bands to be placed in\n", (unsigned)offset);
	}
	return [self setGPUPriority:priority];
}

- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
	HM_ONCE("a background GPU priority of %lu was asked for; remembered, not honoured, as the foreground one\n",
		(unsigned long)priority);
	_backgroundGPUPriority = priority;
	return YES;
}

- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	(void)offset;
	return [self setBackgroundGPUPriority:priority];
}

- (NSUInteger)getGPUPriority           { return _gpuPriority; }
- (NSUInteger)getBackgroundGPUPriority { return _backgroundGPUPriority; }

/* SERVED: the dispatch queue a daemon wants its completion handlers on, so that none runs on its render thread. Kept
 * in Apple's own ivar so that the base class's getter and this plug-in read the same thing. */
- (void)setCompletionQueue:(dispatch_queue_t)queue
{
	Ivar ivar = hm_completion_ivar();
	dispatch_queue_t held = ivar != NULL ? (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;

	if (held == queue) {
		return;
	}
	[queue retain];
	[held release];
	if (ivar != NULL) {
		object_setIvar(self, ivar, queue);
	} else {
		_completionQueue = queue;
	}
}

- (dispatch_queue_t)hakiosCompletionQueue
{
	Ivar ivar = hm_completion_ivar();

	return ivar != NULL ? (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;
}

/* A command buffer that waits for an event is committed to the GPU on _deferred once the value has come; every buffer
 * committed after it, while one is held, goes the same way, so that the queue's order is the order they reach the GPU
 * in (Metal's: a queue's buffers run in commit order). */
- (BOOL)hakiosDeferring
{
	return __atomic_load_n(&_deferring, __ATOMIC_ACQUIRE) != 0;
}

- (void)hakiosDefer:(dispatch_block_t)commit
{
	@synchronized (self) {
		if (_deferred == NULL) {
			_deferred = dispatch_queue_create("hakios.metal.deferred-commits", DISPATCH_QUEUE_SERIAL);
		}
	}
	__atomic_add_fetch(&_deferring, 1, __ATOMIC_ACQ_REL);
	dispatch_async(_deferred, ^{
		commit();
		__atomic_sub_fetch(&_deferring, 1, __ATOMIC_ACQ_REL);
	});
}

@end

/* ============================================================================================================
 * The command buffer: hakios-metal's, with Metal's handlers and slot bookkeeping around it
 * ============================================================================================================ */

@implementation HakiosMetalCommandBuffer

/* _MTLCommandBuffer declares conformance to nothing, and MPSCommandBufferImageCache tests conformsToProtocol: before it
 * asks the buffer anything else; a NO there ended mediaserverd 59 times. */
+ (void)load
{
	hm_adopt(self, @protocol(MTLCommandBuffer), "MTLCommandBuffer");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosQueue:(HakiosMetalCommandQueue *)queue
{
	/* what IOGPUMetalCommandBuffer sends to super. THIS CALL BLOCKS when the queue's semaphore is empty, so by the
	 * time it returns a slot is held and owed (see -hakiosPaySlot) */
	self = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];
	if (self == nil) {
		return nil;
	}
	_queue = [queue retain];
	_cb = hakios_metal_cb_new([queue hakiosQueue]);
	_createdAt = hm_now();
	if (_cb != NULL) {
		hakios_metal_cb_skip_refused(_cb, !hm_strict());
		hakios_metal_cb_set_profile(_cb, hm_fps_on());
		hakios_metal_cb_set_gmem(_cb, hm_gmem_on() || hm_gmem_draws() != 0);
		hakios_metal_cb_set_gmem_min_draws(_cb, hm_gmem_draws());
		hakios_metal_cb_set_gmem_binning(_cb, (hm_gmem_off() & 16) == 0);   /* metal_gmem_off's 16: every draw in every tile */
		hakios_metal_cb_set_null_fs_textures(_cb, (hm_gmem_off() & 32) != 0);   /* 32: no texture reads, to time the shaders alone */
		hakios_metal_cb_set_gmem_off(_cb, hm_gmem_off());
		hakios_metal_cb_set_gmem_tuned(_cb, (hm_gmem_off() & 8) == 0);   /* metal_gmem_off's 8: GMEM wherever a pass can have it */
		hakios_metal_cb_set_profile_draws(_cb, hm_fps_draws());
	}
	_owesSlot = YES;
	return self;
}

/*
 * Give the queue's slot back, exactly once: at completion, which is when Metal's own drivers give it back, or at
 * -dealloc for a buffer that was never committed. Apple's own "this buffer still owes a signal" flag is cleared at the
 * same time, so that its bookkeeping and this one cannot disagree about a debt that has been settled.
 */
- (void)hakiosPaySlot
{
	static Ivar flag;
	static BOOL looked;
	dispatch_semaphore_t semaphore;

	if (!_owesSlot) {
		return;
	}
	_owesSlot = NO;
	if (!looked) {
		looked = YES;
		flag = hm_ivar("_MTLCommandBuffer", "_needsCommandBufferSemaphoreSignal");
	}
	if (flag != NULL) {
		*(unsigned char *)((char *)self + ivar_getOffset(flag)) = 0;
	}
	semaphore = hm_queue_semaphore(_queue);
	if (semaphore != nil) {
		dispatch_semaphore_signal(semaphore);
	}
}

- (void)dealloc
{
	[self hakiosPaySlot];
	if (_cb != NULL) {
		hakios_metal_cb_free(_cb);   /* one in flight is waited for inside; one completed is not */
	}
	[_scheduled release];
	[_completed release];
	[_signals release];
	[_turned release];
	[_waits release];
	if (_sent != NULL) {
		dispatch_release(_sent);
	}
	[_error release];
	[_queue release];
	[super dealloc];
}

- (hakios_metal_cb *)hakiosCommandBuffer { return _cb; }

- (id)device               { return [_queue device]; }
- (id)commandQueue         { return _queue; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferErrorOption)errorOptions { return MTLCommandBufferErrorOptionNone; }

/* Metal's own numbers, which hakios_metal_status copies: not enqueued 0, enqueued 1, committed 2, scheduled 3,
 * completed 4, error 5 */
- (MTLCommandBufferStatus)status { return (MTLCommandBufferStatus)hakios_metal_cb_status(_cb); }

- (NSError *)error
{
	@synchronized (self) {
		if (_error == nil && hakios_metal_cb_status(_cb) == HAKIOS_METAL_ERROR) {
			NSString *why = [NSString stringWithUTF8String:hakios_metal_cb_error(_cb)];

			_error = [[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
						  userInfo:@{ NSLocalizedDescriptionKey: why != nil ? why : @"(no reason recorded)" }] retain];
		}
		return _error;
	}
}

/* no logs: the kernels this plug-in runs have no logging to collect; said once rather than answered nil quietly */
- (id)logs
{
	HM_ONCE("a command buffer's logs were asked for; there is no shader logging here, and nil is answered\n");
	return nil;
}

/* The times Metal reports: the start is when the work was committed (host clock), the end that plus what the GPU
 * says it took (hakios_metal_cb_gpu_nanoseconds: commit to fence). The kernel's times are the same two numbers --
 * there is no separate kernel scheduler clock to read on this port. */
- (CFTimeInterval)GPUStartTime    { return _committedAt; }
- (double)hakiosCreatedAt         { return _createdAt; }
- (CFTimeInterval)GPUEndTime      { return _committedAt + (double)hakios_metal_cb_gpu_nanoseconds(_cb) / 1e9; }
- (CFTimeInterval)kernelStartTime { return [self GPUStartTime]; }
- (CFTimeInterval)kernelEndTime   { return [self GPUEndTime]; }

/* --- the encoders --- */

- (id)computeCommandEncoder
{
	return [[[HakiosMetalComputeCommandEncoder alloc] initWithHakiosCommandBuffer:self] autorelease];
}

/* serial and concurrent dispatch are the same thing on one ring that runs a buffer's commands in order */
- (id)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
	(void)type;
	return [self computeCommandEncoder];
}

- (id)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)descriptor
{
	(void)descriptor;   /* its sample buffer attachments name counters this GPU does not publish */
	return [self computeCommandEncoder];
}

- (id)blitCommandEncoder
{
	return [[[HakiosMetalBlitCommandEncoder alloc] initWithHakiosCommandBuffer:self] autorelease];
}

- (id)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)descriptor
{
	(void)descriptor;
	return [self blitCommandEncoder];
}

/*
 * The pass descriptor, read in full and once: colour attachment 0 (texture, load action, store action, clear colour)
 * becomes hakios-metal's render pass, and everything else the descriptor can carry is either harmless here or refused by
 * name. Metal reads none of it on this road (IOGPUMetalRenderCommandEncoder's init drops the descriptor), and the one
 * place Metal does look is -[MTLRenderPassColorAttachmentDescriptorInternal setTexture:], which asserts conformance to
 * MTLTextureImplementation -- declared by HakiosMetalTexture at +load for exactly this. NO: *why says what, and the pass
 * is not begun.
 */
/* A colour attachment's store action as the core takes it, and its resolve. MultisampleResolve (2) resolves and keeps
 * nothing of the samples (DontCare), StoreAndMultisampleResolve (3) keeps them too (Store); Unknown (4) is a Store until
 * -setColorStoreAction: decides. The resolve itself is a copy of the samples into RESOLVE at the end of the pass
 * (-endEncoding, hakios_metal_cb_blit reading SAMPLES a pixel): a texture of one sample, the attachment's size. */
static BOOL
hm_store_of(MTLRenderPassColorAttachmentDescriptor *c, NSUInteger i, unsigned *store, char *why, size_t size)
{
	MTLStoreAction a = [c storeAction];
	id<MTLTexture> r = [c resolveTexture], t = [c texture];
	BOOL resolves = a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve;

	if (r != nil || resolves) {
		if (r == nil || ![r isKindOfClass:[HakiosMetalTexture class]] || [r sampleCount] != 1 || [t sampleCount] < 2 ||
		    [c resolveSlice] != 0 || [c resolveDepthPlane] != 0 || [r width] != [t width] || [r height] != [t height]) {
			snprintf(why, size, "colour attachment %lu asks for a resolve (store action %lu) %s; a multisample attachment resolved into a "
				 "texture of this device of one sample, its size, one slice, is what is served", (unsigned long)i, (unsigned long)a,
				 r == nil ? "with no resolveTexture" : "of an attachment or into a texture that does not fit");
			return NO;
		}
	}
	*store = a == MTLStoreActionMultisampleResolve ? (unsigned)MTLStoreActionDontCare
	       : a == MTLStoreActionStoreAndMultisampleResolve || a == MTLStoreActionUnknown ? (unsigned)MTLStoreActionStore : (unsigned)a;
	return YES;
}

static BOOL
hm_read_render_pass(MTLRenderPassDescriptor *d, struct hakios_metal_render_pass *pass, HakiosMetalTexture **texture, char *why, size_t size)
{
	MTLRenderPassColorAttachmentDescriptor *c0;
	id t0, depth, stencil;
	MTLClearColor clear;

	memset(pass, 0, sizeof(*pass));
	*texture = nil;
	if (d == nil || ![d isKindOfClass:[MTLRenderPassDescriptor class]]) {
		snprintf(why, size, "the pass descriptor is %s, not an MTLRenderPassDescriptor", d != nil ? class_getName(object_getClass(d)) : "nil");
		return NO;
	}
	c0 = [[d colorAttachments] objectAtIndexedSubscript:0];
	t0 = c0 != nil ? [c0 texture] : nil;   /* nil: a pass of depth alone, or one whose first target is a later index */
	/* the targets past 0, read as attachment 0 is (hakios-metal draws into the ones the function writes and clears the
	 * ones that ask for a Clear; the ones it does not write it leaves as they are) */
	for (NSUInteger i = 1; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *c = [[d colorAttachments] objectAtIndexedSubscript:i];
		id t = c != nil ? [c texture] : nil;

		if (t == nil) {
			continue;
		}
		if (![t isKindOfClass:[HakiosMetalTexture class]] || ([t usage] & MTLTextureUsageRenderTarget) == 0) {
			snprintf(why, size, "colour attachment %lu is a %s%s", (unsigned long)i, class_getName(object_getClass(t)),
				 [t isKindOfClass:[HakiosMetalTexture class]] ? " made without MTLTextureUsageRenderTarget" : ", not a texture of this device");
			return NO;
		}
		/* (a 2D array's layer or a cube's face is its slice, a 3D texture's plane its depth plane: hakios-metal draws
		 * into that one, and refuses one past the texture's) */
		{
			MTLClearColor cc = [c clearColor];
			struct hakios_metal_color_attachment *m = &pass->more[i - 1];
			unsigned store;

			if (!hm_store_of(c, i, &store, why, size)) {
				return NO;
			}
			m->texture = [(HakiosMetalTexture *)t hakiosTexture];
			m->level = (unsigned)[c level];
			m->slice = (unsigned)([t textureType] == MTLTextureType3D ? [c depthPlane] : [c slice]);
			m->load_action = (unsigned)[c loadAction];
			m->store_action = store;
			m->clear_color[0] = cc.red;
			m->clear_color[1] = cc.green;
			m->clear_color[2] = cc.blue;
			m->clear_color[3] = cc.alpha;
		}
	}
	if (t0 != nil && ![t0 isKindOfClass:[HakiosMetalTexture class]]) {
		snprintf(why, size, "the texture of colour attachment 0 is a %s, not a texture of this device", class_getName(object_getClass(t0)));
		return NO;
	}
	if (t0 != nil && ([t0 usage] & MTLTextureUsageRenderTarget) == 0) {
		snprintf(why, size, "the texture of colour attachment 0 was made with usage %#lx, without MTLTextureUsageRenderTarget (4); a "
			 "draw into it is refused", (unsigned long)[t0 usage]);
		return NO;
	}
	/* (colour attachment 0's slice or depth plane as the others', below) */
	if (t0 != nil && !hm_store_of(c0, 0, &pass->store_action, why, size)) {
		return NO;
	}
	if ([c0 storeAction] == MTLStoreActionCustomSampleDepthStore) {
		snprintf(why, size, "colour attachment 0 has store action CustomSampleDepthStore (5), a depth store action, on a colour attachment");
		return NO;
	}
	if ([c0 storeActionOptions] != MTLStoreActionOptionNone) {
		HM_ONCE("a colour attachment carries store action options %#lx (custom sample positions); one sample here, nothing to position\n",
			(unsigned long)[c0 storeActionOptions]);
	}
	depth = [[d depthAttachment] texture];
	stencil = [[d stencilAttachment] texture];
	if (stencil != nil && stencil != depth) {
		snprintf(why, size, "a stencil attachment (%s) that is not the depth attachment's texture; the stencil served is the S8 "
			 "plane of the one Depth32Float_Stencil8 texture both name", class_getName(object_getClass(stencil)));
		return NO;
	}
	if (depth != nil && ![depth isKindOfClass:[HakiosMetalTexture class]]) {
		snprintf(why, size, "the depth attachment is a %s, not a texture of this device", class_getName(object_getClass(depth)));
		return NO;
	}
	if ([d visibilityResultBuffer] != nil) {
		/* occlusion queries: each draw's -setVisibilityResultMode:offset: counts into it (hakios-metal's draw) */
		if (![[d visibilityResultBuffer] isKindOfClass:[HakiosMetalBuffer class]]) {
			snprintf(why, size, "the visibility result buffer is a %s, not a buffer of this device",
				 class_getName(object_getClass([d visibilityResultBuffer])));
			return NO;
		}
		pass->visibility = [(HakiosMetalBuffer *)[d visibilityResultBuffer] hakiosBuffer];
	}
	if ([d renderTargetArrayLength] > 1) {
		snprintf(why, size, "renderTargetArrayLength %lu (layered rendering); one layer is served", (unsigned long)[d renderTargetArrayLength]);
		return NO;
	}
	if ([d defaultRasterSampleCount] > 1) {
		snprintf(why, size, "defaultRasterSampleCount %lu; one sample is served", (unsigned long)[d defaultRasterSampleCount]);
		return NO;
	}
	if ([d rasterizationRateMap] != nil) {
		snprintf(why, size, "a rasterization rate map is set (variable rate shading); not served");
		return NO;
	}
	if ([d imageblockSampleLength] != 0 || [d threadgroupMemoryLength] != 0 || [d tileWidth] != 0 || [d tileHeight] != 0) {
		HM_ONCE("a render pass asks for tile shading state (imageblock %lu bytes, threadgroup memory %lu, tile %lux%lu); this GPU has no "
			"tile memory and no tile pipeline can be made, so the numbers decide nothing\n", (unsigned long)[d imageblockSampleLength],
			(unsigned long)[d threadgroupMemoryLength], (unsigned long)[d tileWidth], (unsigned long)[d tileHeight]);
	}
	/* Unknown (4) is Metal's "decided later by -setColorStoreAction:"; until then it is a store, which is what sysmem does
	 * anyway: the RB writes the memory as it draws, there is no tile to store from */
	if (t0 == nil && depth == nil && pass->more[0].texture == NULL && pass->more[1].texture == NULL && pass->more[2].texture == NULL &&
	    pass->more[3].texture == NULL && pass->more[4].texture == NULL && pass->more[5].texture == NULL && pass->more[6].texture == NULL) {
		snprintf(why, size, "the pass has no attachment at all (no colour texture, no depth); nothing to draw into");
		return NO;
	}
	pass->color0 = t0 != nil ? [(HakiosMetalTexture *)t0 hakiosTexture] : NULL;
	pass->color0_level = (unsigned)[c0 level];   /* the core refuses a level past the texture's, with the numbers */
	pass->color0_slice = t0 != nil ? (unsigned)([t0 textureType] == MTLTextureType3D ? [c0 depthPlane] : [c0 slice]) : 0;
	/* (with no texture at 0 its actions say nothing; a Load spends no Clear on an attachment that is not there) */
	pass->load_action = t0 != nil ? (unsigned)[c0 loadAction] : (unsigned)MTLLoadActionLoad;
	if (t0 != nil && hm_timeline_on() && [t0 width] >= 1000 && [t0 height] >= 2000) {
		static unsigned seen[3], told, formats[8][2];
		if (pass->load_action < 3) seen[pass->load_action]++;
		for (unsigned f = 0; f < 8; f++) {
			if (formats[f][0] == (unsigned)[t0 pixelFormat] || formats[f][1] == 0) {
				formats[f][0] = (unsigned)[t0 pixelFormat];
				formats[f][1]++;
				break;
			}
		}
		static unsigned acts[3][2];
		if (pass->more[0].texture != NULL && pass->more[0].load_action < 3) acts[pass->more[0].load_action][pass->more[0].store_action ? 1 : 0]++;
		if (told % 200 == 199) {
			hm_print("timeline: attachment 1's load/store: dontcare %u/%u, load %u/%u, clear %u/%u (not stored/stored)\n", acts[0][0], acts[0][1], acts[1][0],
				 acts[1][1], acts[2][0], acts[2][1]);
		}
		static unsigned more[4];
		unsigned extra = 0;
		for (unsigned i = 0; i < 7; i++) extra += pass->more[i].texture != NULL;
		more[extra < 3 ? extra : 3]++;
		if (told % 200 == 199) hm_print("timeline: full-screen passes with 0, 1, 2, 3+ more colour attachments: %u %u %u %u\n", more[0], more[1], more[2], more[3]);
		if (++told % 200 == 0) {
			hm_print("timeline: full-screen passes by load action: dontcare %u load %u clear %u; by format: %u x%u, %u x%u, %u x%u, %u x%u\n", seen[0], seen[1],
				 seen[2], formats[0][0], formats[0][1], formats[1][0], formats[1][1], formats[2][0], formats[2][1], formats[3][0], formats[3][1]);
		}
	}
	/* (pass->store_action is hm_store_of's, read above with the resolve) */
	clear = [c0 clearColor];
	pass->clear_color[0] = clear.red;
	pass->clear_color[1] = clear.green;
	pass->clear_color[2] = clear.blue;
	pass->clear_color[3] = clear.alpha;
	if (depth != nil) {
		/* the depth attachment: Metal's actions passed through (Unknown, 4, is a store until -setDepthStoreAction: says
		 * otherwise, as for colour), the clear depth read only for a Clear; hakios-metal judges the format and size */
		MTLRenderPassDepthAttachmentDescriptor *da = [d depthAttachment];

		pass->depth = [(HakiosMetalTexture *)depth hakiosTexture];
		pass->depth_level = (unsigned)[da level];   /* the core refuses one past the texture's */
		pass->depth_slice = (unsigned)[da slice];   /* a depth 2D array's layer (a shadow cascade); the core refuses one past them */
		pass->depth_load_action = (unsigned)[da loadAction];
		pass->depth_store_action = [da storeAction] == MTLStoreActionUnknown ? (unsigned)MTLStoreActionStore : (unsigned)[da storeAction];
		pass->clear_depth = [da clearDepth];
	}
	if (stencil != nil) {
		MTLRenderPassStencilAttachmentDescriptor *sa = [d stencilAttachment];

		pass->stencil = [(HakiosMetalTexture *)stencil hakiosTexture];
		pass->stencil_load_action = (unsigned)[sa loadAction];
		pass->stencil_store_action = [sa storeAction] == MTLStoreActionUnknown ? (unsigned)MTLStoreActionStore : (unsigned)[sa storeAction];
		pass->clear_stencil = [sa clearStencil] & 0xff;
	}
	*texture = t0;
	return YES;
}

/* whether a pass still owes a Clear -- of any colour attachment or of the depth -- and its Clears spent (made Loads) once
 * a draw or the pass's own clear has recorded them: one fill per attachment and pass, however many draws follow */
static BOOL
hm_owes_clear(const struct hakios_metal_render_pass *pass)
{
	if (pass->load_action == (unsigned)MTLLoadActionClear || (pass->depth != NULL && pass->depth_load_action == (unsigned)MTLLoadActionClear) ||
	    (pass->stencil != NULL && pass->stencil_load_action == (unsigned)MTLLoadActionClear)) {
		return YES;
	}
	for (int i = 0; i < 7; i++) {
		if (pass->more[i].texture != NULL && pass->more[i].load_action == (unsigned)MTLLoadActionClear) {
			return YES;
		}
	}
	return NO;
}

static void
hm_spend_clears(struct hakios_metal_render_pass *pass)
{
	pass->load_action = (unsigned)MTLLoadActionLoad;
	pass->depth_load_action = (unsigned)MTLLoadActionLoad;
	pass->stencil_load_action = (unsigned)MTLLoadActionLoad;
	for (int i = 0; i < 7; i++) {
		pass->more[i].load_action = (unsigned)MTLLoadActionLoad;
	}
}

/* A surface drawn into through a copy of its rows (-hakiosShadow:): the copy as the surface is now, for a Load, and
 * what the pass drew back into the surface once the command buffer is done -- as for a kernel's writes */
static void
hm_shadow_targets(MTLRenderPassDescriptor *d, id<MTLCommandBuffer> cb)
{
	for (NSUInteger i = 0; i < 8; i++) {
		id t = [[[d colorAttachments] objectAtIndexedSubscript:i] texture];

		if (t != nil && [t isKindOfClass:[HakiosMetalTexture class]] && [(HakiosMetalTexture *)t hakiosIsTurned]) {
			[(HakiosMetalCommandBuffer *)cb hakiosTurnAtCommit:t];
			continue;
		}
		if (t == nil || ![t isKindOfClass:[HakiosMetalTexture class]] || ![(HakiosMetalTexture *)t hakiosIsShadow]) {
			continue;
		}
		[(HakiosMetalTexture *)t hakiosRefreshShadow];
		{
			HakiosMetalTexture *drawn = [(HakiosMetalTexture *)t retain];

			[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
				(void)done;
				[drawn hakiosWriteBackShadow];
				[drawn release];
			}];
		}
	}
}

/* A pass that cannot be served ends the command buffer in error and gives NO encoder: a caller then sends its draws
 * to nil, and -commit reports the sentence. (The alternative, an encoder that fails its draws one by one, was measured
 * no clearer in the earlier plug-in, and a nil texture in a drawable was what cost it most -- here the nil is said.) */
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor
{
	struct hakios_metal_render_pass pass;
	HakiosMetalTexture *texture = nil;
	char why[256];

	if (!hm_read_render_pass(descriptor, &pass, &texture, why, sizeof(why))) {
		hm_print("refused: a render pass: %s; the command buffer is failed and no encoder is returned\n", why);
		hakios_metal_cb_refuse(_cb, why);
		return nil;
	}
	{
		HakiosMetalRenderCommandEncoder *encoder = [[[HakiosMetalRenderCommandEncoder alloc] initWithHakiosCommandBuffer:self pass:&pass
													     colorTexture:texture] autorelease];

		hm_shadow_targets(descriptor, (id<MTLCommandBuffer>)self);

		[encoder hakiosTakeResolves:descriptor];   /* hm_read_render_pass judged them */
		return encoder;
	}
}

/* a parallel encoder splits one pass across threads, each sub-encoder its own object (HakiosMetalParallelRenderCommandEncoder) */
- (id)parallelRenderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor
{
	struct hakios_metal_render_pass pass;
	HakiosMetalTexture *texture = nil;
	char why[256];

	if (!hm_read_render_pass(descriptor, &pass, &texture, why, sizeof(why))) {
		hm_print("refused: a parallel render pass: %s; the command buffer is failed and no encoder is returned\n", why);
		hakios_metal_cb_refuse(_cb, why);
		return nil;
	}
	hm_shadow_targets(descriptor, (id<MTLCommandBuffer>)self);
	return [[[HakiosMetalParallelRenderCommandEncoder alloc] initWithHakiosCommandBuffer:self pass:&pass colorTexture:texture
											descriptor:descriptor] autorelease];
}

- (id)resourceStateCommandEncoder
{
	hm_refusal(self, _cmd, "has no sparse textures to map; it returns nil");
	return nil;
}

- (id)accelerationStructureCommandEncoder
{
	hm_refusal(self, _cmd, "has no ray tracing; it returns nil");
	return nil;
}

/* --- ordering --- */

/* Metal's -enqueue reserves this buffer's place in the queue's order before it is committed. hakios-metal runs
 * command buffers in COMMIT order, so a buffer enqueued first and committed second runs second: said once, because
 * that is the one case where the order an application asked for is not the order it gets. */
- (void)enqueue
{
	HM_ONCE("a command buffer was enqueued; this plug-in runs command buffers in the order they are COMMITTED, so a "
		"place reserved by enqueue is not kept\n");
}

/*
 * The work goes to the GPU here. hakios_metal_cb_commit submits synchronously (the status is 'scheduled' when it
 * returns) and runs the one callback on the queue's thread after completion or error -- or on THIS thread, inside
 * the call, for a buffer whose recording already failed, which is why nothing after the call assumes the callback has
 * not run yet. The object is retained across its own flight and released by the callback, so a buffer an application
 * released right after -commit lives until its handlers have run, as Metal's do.
 */
/*
 * boot-arg metal_fps (a diagnostic): every two seconds of a process's command buffers, how many frames it presented, how
 * many buffers it committed, how long the GPU was busy with them, how long -commit kept the caller's thread and how
 * long a buffer took from commit to completion. Which of the three is near the two seconds says where a low frame
 * rate comes from: the GPU, the driver's own work on the caller's thread, or the waiting between them.
 */
static struct {
	pthread_mutex_t lock;
	double since;
	unsigned presents, buffers;
	double gpu, commit, latency, latencyMax;
	unsigned passes, draws, dispatches, copies, gmemPasses, tiles;
	double shadowIn, shadowOut;   /* seconds in surface copies: taken from the surface, written back into it */
	unsigned shadowIns, shadowOuts;
	struct { unsigned kind, width, height, draws, tiles; double us; } slowest[4];   /* the slowest passes of the period */
	double passUs;                /* all passes timed, together */
	char gmemWhy[160];            /* the last reason a pass stayed in sysmem */
	struct { char name[96]; unsigned count; double us; } programs[48];   /* metal_fps=draws: GPU time by program */
	BOOL heavyShown;              /* metal_fps=draws: a heavy pass's draws printed this period */
	/* the gaps between one commit and the next, where something moves (under 0.25 s): on time for 60 Hz (20 ms at
	 * most), a frame missed (to 50), a stall (past 50) -- what a user sees as smooth or not, which the GPU's busy
	 * time alone does not say */
	double lastCommit;
	unsigned gapsOnTime, gapsLate, gapsStall;
	double encode, encodeMax;
	unsigned held;
	double waited;
	unsigned waits;
	double drawCpu, coreCpu;
	unsigned drawCalls;
	unsigned drawsOn[8];   /* the draw calls on each processor (TPIDR_EL0's number), and their time there */
	double drawUsOn[8];
	uint64_t lastTpidr;
	double sampledBytes[6];   /* metal_fps=timeline: the bound fragment textures' sizes, by hakiosStorageKind */
	struct { unsigned w, h, origin; double bytes; } bySize[16];
} hm_fps = { PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {{0, 0, 0, 0, 0, 0}}, 0, "", {{"", 0, 0}}, NO, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}, 0, {0}, {{0, 0, 0, 0}} };

/* boot-arg metal_gmem=1: tiled rendering (render::Options::gmem) for a pass that can have it, GMEM or sysmem chosen by
 * what each cost such passes before (hakios_metal_cb_set_gmem_tuned). Off by default: for CoreAnimation it won nothing
 * -- a pass of 23 draws took the GPU 5 ms in GMEM against 0.13 in sysmem, and SpringBoard's tour as a whole 7.1 ms a
 * command buffer against 6.5 -- since its passes touch little of the screen and read it back through framebuffer fetch,
 * which the texture unit does from GMEM at a quarter of its rate from memory. A game blending whole screens gains. */
static BOOL
hm_gmem_on(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_gmem=1") != NULL;
	}
	return on != 0;
}

/* metal_fps=draws: each draw of a sysmem pass timed alone, by program (only sysmem passes are: without metal_gmem=1) */
static BOOL
hm_fps_draws(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_fps=draws") != NULL;
	}
	return on != 0;
}

/* boot-arg metal_gmem_off=N: GMEM's refinements off by the bits of N (hakios_metal_cb_set_gmem_off; 8: the choice of GMEM
 * or sysmem by what each costs, hakios_metal_cb_set_gmem_tuned; 16: the hardware's binning,
 * hakios_metal_cb_set_gmem_binning; 32: every fragment texture the null one, wrong pictures but each draw's time without
 * its texture reads, hakios_metal_cb_set_null_fs_textures), for a bisect */
static unsigned
hm_gmem_off(void)
{
	static int bits = -1;

	if (bits < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;
		const char *at;

		bits = 0;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_gmem_off=")) != NULL) {
			bits = atoi(at + strlen("metal_gmem_off="));
		}
	}
	return (unsigned)bits;
}

/* boot-arg metal_gmem_draws=N: GMEM for a pass whose like had at least N draws in the frame before
 * (hakios_metal_cb_set_gmem_min_draws), sysmem for the rest; 56 when not given, 0 turns it off. SpringBoard's tour: its
 * heavy passes in GMEM 14 ms where 80 draws took 12 to 32 in sysmem, 18 where 141 took 19 to 23, the light ones left
 * in sysmem, the GPU's whole time the same (4.48 s against 4.43). */
static unsigned
hm_gmem_draws(void)
{
	static int draws = -1;

	if (draws < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;
		const char *at;

		draws = 56;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_gmem_draws=")) != NULL) {
			draws = atoi(at + strlen("metal_gmem_draws="));
		}
	}
	return (unsigned)draws;
}

static BOOL
hm_fps_on(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_fps") != NULL;
	}
	return on != 0;
}

/* one buffer done (GPU seconds, commit-to-completion seconds, what it held) or one frame presented (present set) */
static void
hm_fps_note(BOOL present, double gpu, double latency, hakios_metal_cb *cb)
{
	const double now = hm_now();

	if (!hm_fps_on()) {
		return;
	}
	pthread_mutex_lock(&hm_fps.lock);
	if (hm_fps.since == 0) {
		hm_fps.since = now;
	}
	if (present) {
		hm_fps.presents++;
	} else {
		hm_fps.buffers++;
		hm_fps.gpu += gpu;
		hm_fps.latency += latency;
		hm_fps.latencyMax = MAX(hm_fps.latencyMax, latency);
		if (cb != NULL) {
			unsigned passes = 0, draws = 0, dispatches = 0, copies = 0, clears = 0;

			hakios_metal_cb_counts(cb, &passes, &draws, &dispatches, &copies, &clears);
			hm_fps.passes += passes + copies + clears + dispatches;
			hm_fps.draws += draws;
			hm_fps.dispatches += dispatches;
			hm_fps.copies += copies + clears;
			{
				unsigned gp = 0, tl = 0;

				hakios_metal_cb_gmem_counts(cb, &gp, &tl);
				hm_fps.gmemPasses += gp;
				hm_fps.tiles += tl;
				if (hakios_metal_cb_gmem_refusal(cb)[0] != 0) {
					strlcpy(hm_fps.gmemWhy, hakios_metal_cb_gmem_refusal(cb), sizeof(hm_fps.gmemWhy));
				}
			}
			/* the draws of the slowest pass over 20 ms, once a period: what a heavy frame is made of */
			/* (and with metal_fps=timeline every pass of a few draws over 8 ms, 15 of them: a few draws that cost so much
			 * are a shader's doing, not the pass's) */
			static unsigned fewShown;
			if (hm_fps_draws() && (!hm_fps.heavyShown || hm_timeline_on())) {
				unsigned slow = 0, kind, width, height, draws, slowDraws = 0;
				double slowUs = 0, us;
				int n = hakios_metal_cb_pass_time(cb, 0, NULL, NULL, NULL, NULL, NULL);

				for (int i = 0; i < n; i++) {
					hakios_metal_cb_pass_time(cb, (unsigned)i, &kind, &width, &height, &draws, &us);
					if (us > slowUs) slowUs = us, slow = (unsigned)i, slowDraws = draws;
				}
				const BOOL few = hm_timeline_on() && slowUs > 8000 && slowDraws <= 20 && fewShown < 15;
				fewShown += few ? 1 : 0;
				if ((slowUs > 20000 && !hm_fps.heavyShown) || few) {
					hm_fps.heavyShown = YES;
					hm_print("fps: ... a heavy pass, %.1f ms; its draws:\n", slowUs / 1e3);
					for (unsigned i = 0;; i++) {
						const char *name = NULL;
						unsigned pass, area[4];
						double dus = 0;

						if (hakios_metal_cb_draw_place(cb, i, &pass, area) <= (int)i || hakios_metal_cb_draw_time(cb, i, &name, &dus) <= (int)i) {
							break;
						}
						if (pass == slow) {
							hm_print("fps: ...   %s %.0f us over %ux%u at (%u,%u)\n", name, dus, area[2] - area[0] + 1, area[3] - area[1] + 1, area[0], area[1]);
						}
					}
				}
			}
			for (unsigned i = 0; hm_fps_draws(); i++) {
				const char *name = NULL;
				double us = 0;
				int k;

				if (hakios_metal_cb_draw_time(cb, i, &name, &us) <= (int)i || name == NULL) {
					break;
				}
				for (k = 0; k < 48 && hm_fps.programs[k].name[0] != 0 && strcmp(hm_fps.programs[k].name, name) != 0; k++) {
				}
				if (k == 48) {
					continue;
				}
				if (hm_fps.programs[k].name[0] == 0) {
					strlcpy(hm_fps.programs[k].name, name, sizeof(hm_fps.programs[k].name));
				}
				hm_fps.programs[k].count++;
				hm_fps.programs[k].us += us;
			}
			/* metal_fps=timeline: a heavy buffer's every pass, the first 12 such buffers */
			static unsigned heavyListed;
			const BOOL listAll = gpu > 0.015 && heavyListed < 80 && hm_timeline_on();
			NSMutableString *all = listAll ? [NSMutableString string] : nil;

			heavyListed += listAll ? 1 : 0;
			for (unsigned i = 0;; i++) {
				unsigned kind, width, height, draws;
				double us;

				if (hakios_metal_cb_pass_time(cb, i, &kind, &width, &height, &draws, &us) <= (int)i) {
					if (all != nil) hm_print("timeline: a buffer of %.1f ms on the GPU:%s\n", gpu * 1e3, [all UTF8String]);
					break;
				}
				if (all != nil) {
					unsigned attached = 0, touched = 0;
					hakios_metal_cb_pass_attachments(cb, i, &attached, &touched);
					[all appendFormat:@" [%u %ux%u %u draws%@ %.2f ms]", kind, width, height, draws,
							  hakios_metal_cb_pass_tiles(cb, i) > 0 ? [NSString stringWithFormat:@" %d tiles (attachments %#x, touched %#x)",
														      hakios_metal_cb_pass_tiles(cb, i), attached, touched] : @"",
							  us / 1e3];
				}
				hm_fps.passUs += us;
				for (int k = 0; k < 4; k++) {
					if (us > hm_fps.slowest[k].us) {
						memmove(&hm_fps.slowest[k + 1], &hm_fps.slowest[k], (size_t)(3 - k) * sizeof(hm_fps.slowest[0]));
						hm_fps.slowest[k].kind = kind;
						hm_fps.slowest[k].width = width;
						hm_fps.slowest[k].height = height;
						hm_fps.slowest[k].draws = draws;
						hm_fps.slowest[k].tiles = (unsigned)MAX(hakios_metal_cb_pass_tiles(cb, i), 0);
						hm_fps.slowest[k].us = us;
						break;
					}
				}
			}
		}
	}
	if (now - hm_fps.since >= 2.0) {
		const double span = now - hm_fps.since;

		const double per = hm_fps.buffers ? (double)hm_fps.buffers : 1.0;

		hm_print("fps: %.1f frames/s over %.1f s (%u presented, %u command buffers); GPU busy %.0f ms (%.0f%%), -commit on the "
			 "caller's thread %.0f ms, commit to completion %.1f ms on average, %.1f at most; a buffer holds %.1f passes "
			 "(each with its flush and wait for idle), %.1f draws, %.1f dispatches, %.1f copies or clears; %.1f passes in GMEM "
			 "(%.1f tiles)\n",
			 hm_fps.presents / span, span, hm_fps.presents, hm_fps.buffers, hm_fps.gpu * 1e3, 100.0 * hm_fps.gpu / span,
			 hm_fps.commit * 1e3, hm_fps.buffers ? hm_fps.latency * 1e3 / hm_fps.buffers : 0.0, hm_fps.latencyMax * 1e3,
			 hm_fps.passes / per, hm_fps.draws / per, hm_fps.dispatches / per, hm_fps.copies / per, hm_fps.gmemPasses / per,
			 hm_fps.tiles / per);
		if (hm_fps.gapsOnTime + hm_fps.gapsLate + hm_fps.gapsStall != 0) {
			hm_print("fps: ... gaps between commits while something moves: %u on time (20 ms at most), %u a frame late (to 50 ms), "
				 "%u stalls (past 50); a buffer made to committed %.1f ms on average, %.1f at most; %u held for an event; "
				 "%u waits for completion, %.0f ms in them; %u draw calls, %.0f us of ours each (%.0f of them the core's)\n",
				 hm_fps.gapsOnTime, hm_fps.gapsLate, hm_fps.gapsStall, hm_fps.encode * 1e3 / per, hm_fps.encodeMax * 1e3, hm_fps.held,
				 hm_fps.waits, hm_fps.waited * 1e3, hm_fps.drawCalls, hm_fps.drawCalls ? hm_fps.drawCpu * 1e6 / hm_fps.drawCalls : 0.0,
				 hm_fps.drawCalls ? hm_fps.coreCpu * 1e6 / hm_fps.drawCalls : 0.0);
		}
		if (hm_sampling()) hm_sample_report();
		if (hm_timeline_on() && hm_fps.sampledBytes[0] + hm_fps.sampledBytes[1] + hm_fps.sampledBytes[2] + hm_fps.sampledBytes[3] + hm_fps.sampledBytes[4] +
		    hm_fps.sampledBytes[5] > 0) {
			hm_print("fps: ... textures bound to draws (MB, by their size): surfaces %.0f, shared %.0f, over buffers %.0f, private %.0f, memoryless %.0f, "
				 "compressed %.0f\n",
				 hm_fps.sampledBytes[0] / 1e6, hm_fps.sampledBytes[1] / 1e6, hm_fps.sampledBytes[2] / 1e6, hm_fps.sampledBytes[3] / 1e6,
				 hm_fps.sampledBytes[4] / 1e6, hm_fps.sampledBytes[5] / 1e6);
			memset(hm_fps.sampledBytes, 0, sizeof(hm_fps.sampledBytes));
			for (unsigned k = 0; k < 16 && hm_fps.bySize[k].bytes != 0; k++) {
				hm_print("fps: ... sampled: %ux%u origin %u: %.0f MB\n", hm_fps.bySize[k].w, hm_fps.bySize[k].h, hm_fps.bySize[k].origin,
					 hm_fps.bySize[k].bytes / 1e6);
			}
			memset(hm_fps.bySize, 0, sizeof(hm_fps.bySize));
		}
		{
			unsigned made, reused;
			unsigned long long bytes;

			hakios_metal_buffer_counts(&made, &reused, &bytes);
			hm_print("fps: ... buffers: %u made (%llu KB), %u taken again from the pool\n", made, bytes >> 10, reused);
		}
		if (hm_fps.drawCalls != 0) {
			char by[200] = "";
			size_t at = 0;

			for (unsigned c = 0; c < 8; c++) {
				if (hm_fps.drawsOn[c] != 0 && at < sizeof(by)) {
					at += (size_t)snprintf(by + at, sizeof(by) - at, " cpu%u %u at %.0f us", c, hm_fps.drawsOn[c], hm_fps.drawUsOn[c] / hm_fps.drawsOn[c]);
				}
			}
			hm_print("fps: ... draw calls by processor:%s (TPIDR_EL0 %#llx)\n", by, (unsigned long long)hm_fps.lastTpidr);
			memset(hm_fps.drawsOn, 0, sizeof(hm_fps.drawsOn));
			memset(hm_fps.drawUsOn, 0, sizeof(hm_fps.drawUsOn));
		}
		if (hm_fps.slowest[0].us > 0) {
			static const char *const kinds[] = {"draw", "copy", "clear", "dispatch"};
			char line[400];
			size_t n = 0;

			for (int k = 0; k < 4 && hm_fps.slowest[k].us > 0; k++) {
				char tiles[24] = "";

				if (hm_fps.slowest[k].tiles != 0) {
					snprintf(tiles, sizeof(tiles), " in %u tiles", hm_fps.slowest[k].tiles);
				}
				n += (size_t)snprintf(line + n, sizeof(line) - n, "%s %s %ux%u%s%u%s %.2f ms", k ? ";" : "",
						      kinds[hm_fps.slowest[k].kind & 3], hm_fps.slowest[k].width, hm_fps.slowest[k].height,
						      hm_fps.slowest[k].kind == 0 ? " draws " : " ", hm_fps.slowest[k].kind == 0 ? hm_fps.slowest[k].draws : 0,
						      tiles, hm_fps.slowest[k].us / 1e3);
			}
			hm_print("fps: ... passes timed by the GPU: %.0f ms in all; the slowest:%s\n", hm_fps.passUs / 1e3, line);
		}
		for (int shown = 0; shown < 8; shown++) {
			int best = -1;

			for (int k = 0; k < 48 && hm_fps.programs[k].name[0] != 0; k++) {
				if (hm_fps.programs[k].count != 0 && (best < 0 || hm_fps.programs[k].us > hm_fps.programs[best].us)) {
					best = k;
				}
			}
			if (best < 0) {
				break;
			}
			hm_print("fps: ... draws of %s: %u, %.2f ms on the GPU (%.0f us each)\n", hm_fps.programs[best].name,
				 hm_fps.programs[best].count, hm_fps.programs[best].us / 1e3, hm_fps.programs[best].us / hm_fps.programs[best].count);
			hm_fps.programs[best].count = 0;
		}
		memset(hm_fps.programs, 0, sizeof(hm_fps.programs));
		hm_fps.heavyShown = NO;
		if (hm_fps.gmemWhy[0] != 0) {
			hm_print("fps: ... a pass left in sysmem: %s\n", hm_fps.gmemWhy);
			hm_fps.gmemWhy[0] = 0;
		}
		memset(hm_fps.slowest, 0, sizeof(hm_fps.slowest));
		hm_fps.passUs = 0;
		if (hm_fps.shadowIns + hm_fps.shadowOuts != 0) {
			hm_print("fps: ... surface copies on the processor: %u taken from their surfaces in %.0f ms, %u written back in %.0f ms\n",
				 hm_fps.shadowIns, hm_fps.shadowIn * 1e3, hm_fps.shadowOuts, hm_fps.shadowOut * 1e3);
		}
		hm_fps.passes = hm_fps.draws = hm_fps.dispatches = hm_fps.copies = hm_fps.gmemPasses = hm_fps.tiles = 0;
		hm_fps.shadowIn = hm_fps.shadowOut = 0;
		hm_fps.shadowIns = hm_fps.shadowOuts = 0;
		hm_fps.since = now;
		hm_fps.presents = hm_fps.buffers = 0;
		hm_fps.gpu = hm_fps.commit = hm_fps.latency = hm_fps.latencyMax = 0;
		hm_fps.gapsOnTime = hm_fps.gapsLate = hm_fps.gapsStall = hm_fps.held = 0;
		hm_fps.encode = hm_fps.encodeMax = hm_fps.waited = 0;
		hm_fps.waits = hm_fps.drawCalls = 0;
		hm_fps.drawCpu = hm_fps.coreCpu = 0;
	}
	pthread_mutex_unlock(&hm_fps.lock);
}

/* time spent copying a surface's rows into its copy (IN) or back (OUT) */
static void
hm_fps_shadow(BOOL in, double seconds)
{
	if (!hm_fps_on()) {
		return;
	}
	pthread_mutex_lock(&hm_fps.lock);
	if (in) {
		hm_fps.shadowIn += seconds;
		hm_fps.shadowIns++;
	} else {
		hm_fps.shadowOut += seconds;
		hm_fps.shadowOuts++;
	}
	pthread_mutex_unlock(&hm_fps.lock);
}

/*
 * boot-arg metal_fps=sample (a diagnostic): where a draw call's time goes on this phone, sampled. A thread of its own
 * stops the thread that is inside a draw call every 200 us, reads its program counter and lets it go on; the counts by
 * function (dladdr) are printed with metal_fps's lines. The host's run of the same encoding takes 5 us a draw where
 * this phone took 40 on a fast core, so the difference is something the host does not show.
 */
static volatile mach_port_t hm_sample_thread;   /* the thread inside a draw call, or 0 */
static uint64_t hm_samples[16384], hm_samplesBy[16384], hm_samplesBy2[16384];   /* the PC, and the first caller outside the system's libraries */
static volatile unsigned hm_sampled;

static BOOL
hm_sampling(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_fps=sample") != NULL;
	}
	return on != 0;
}

static void *
hm_sampler(void *unused)
{
	(void)unused;
	for (;;) {
		usleep(200);
		const mach_port_t t = hm_sample_thread;
		if (t == 0 || hm_sampled >= sizeof(hm_samples) / sizeof(hm_samples[0])) continue;
		if (thread_suspend(t) != KERN_SUCCESS) continue;
		arm_thread_state64_t st;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS && hm_sample_thread == t) {
			/* (the frame chain, for the first return address in this plug-in: who called into the system) */
			static uintptr_t lo, hi;
			if (hi == 0) {
				Dl_info me;
				if (dladdr((const void *)hm_sampler, &me)) lo = (uintptr_t)me.dli_fbase, hi = lo + (64u << 20);
			}
			uint64_t by = 0, by2 = 0, fp = __darwin_arm_thread_state64_get_fp(st), lr = __darwin_arm_thread_state64_get_lr(st);
			if (lr >= lo && lr < hi) by = lr;
			for (int d = 0; d < 24 && by2 == 0 && fp != 0 && (fp & 7) == 0; d++) {
				const uint64_t *f = (const uint64_t *)fp;
				const uint64_t ret = f[1] & 0x0000007fffffffffull;
				if (ret >= lo && ret < hi && ret != by) {
					if (by == 0) by = ret;
					else by2 = ret;
				}
				fp = f[0];
			}
			hm_samplesBy[hm_sampled] = by;
			hm_samplesBy2[hm_sampled] = by2;
			hm_samples[hm_sampled++] = __darwin_arm_thread_state64_get_pc(st);
		}
		thread_resume(t);
	}
	return NULL;
}

/* the samples so far by function, the 12 most, into the log; then from nothing again */
static void
hm_sample_report(void)
{
	static struct { const char *name; unsigned count; } by[256];
	unsigned kinds = 0, total = hm_sampled;

	if (total == 0) return;
	for (unsigned i = 0; i < total; i++) {
		Dl_info info;
		const char *name = dladdr((const void *)hm_samples[i], &info) && info.dli_sname ? info.dli_sname : "?";
		/* (in the system's code: named by the caller of ours it was reached from) */
		static char named[256][160];
		Dl_info caller;
		if (hm_samplesBy[i] != 0 && (strcmp(name, "<redacted>") == 0 || strncmp(name, "mach_", 5) == 0 || strncmp(name, "_kernelrpc", 10) == 0) &&
		    dladdr((const void *)hm_samplesBy[i], &caller) && caller.dli_sname) {
			char buf[160];
			Dl_info caller2;
			const char *n2 = hm_samplesBy2[i] != 0 && dladdr((const void *)hm_samplesBy2[i], &caller2) && caller2.dli_sname ? caller2.dli_sname : "";
			snprintf(buf, sizeof(buf), "%.24s <- %.60s <- %.60s", name, caller.dli_sname, n2);
			unsigned k = 0;
			while (k < 256 && named[k][0] != 0 && strcmp(named[k], buf) != 0) k++;
			if (k < 256) {
				if (named[k][0] == 0) strlcpy(named[k], buf, sizeof(named[k]));
				name = named[k];
			}
		}
		unsigned k = 0;
		while (k < kinds && by[k].name != name && strcmp(by[k].name, name) != 0) k++;
		if (k == kinds) {
			if (kinds == 256) continue;
			by[kinds].name = name, by[kinds].count = 0, kinds++;
		}
		by[k].count++;
	}
	for (unsigned r = 0; r < 12 && r < kinds; r++) {
		unsigned best = r;
		for (unsigned k = r + 1; k < kinds; k++) if (by[k].count > by[best].count) best = k;
		const typeof(by[0]) t = by[r]; by[r] = by[best]; by[best] = t;
		hm_print("fps: ... sampled in draw calls: %4.1f%% %s\n", 100.0 * by[r].count / total, by[r].name);
	}
	hm_sampled = 0;
}

/* the driver's own time in a draw call (the plug-in's and the core's encoding), of the caller's thread */
static void
hm_fps_drew(double seconds, unsigned cpu)
{
	pthread_mutex_lock(&hm_fps.lock);
	hm_fps.drawCpu += seconds;
	hm_fps.drawCalls++;
	if (cpu < 8) {
		hm_fps.drawsOn[cpu]++;
		hm_fps.drawUsOn[cpu] += seconds * 1e6;
	}
	pthread_mutex_unlock(&hm_fps.lock);
}

/* ...and of that, the core's (hakios_metal_cb_draw*) */
static void
hm_fps_core(double seconds)
{
	pthread_mutex_lock(&hm_fps.lock);
	hm_fps.coreCpu += seconds;
	pthread_mutex_unlock(&hm_fps.lock);
}

/* time a client spent in -waitUntilCompleted: a renderer that waits there cannot draw its next frame meanwhile */
static void
hm_fps_waited(double seconds)
{
	if (!hm_fps_on()) {
		return;
	}
	pthread_mutex_lock(&hm_fps.lock);
	hm_fps.waited += seconds;
	hm_fps.waits++;
	pthread_mutex_unlock(&hm_fps.lock);
}

/* a command buffer's life from its making to -commit (the client's encoding, ours included), and whether it was held
 * for an event it waits on */
static void
hm_fps_encoded(double seconds, BOOL held)
{
	if (!hm_fps_on()) {
		return;
	}
	pthread_mutex_lock(&hm_fps.lock);
	hm_fps.encode += seconds;
	if (seconds > hm_fps.encodeMax) hm_fps.encodeMax = seconds;
	hm_fps.held += held ? 1 : 0;
	pthread_mutex_unlock(&hm_fps.lock);
}

static void
hm_fps_commit(double seconds)
{
	if (!hm_fps_on()) {
		return;
	}
	pthread_mutex_lock(&hm_fps.lock);
	hm_fps.commit += seconds;
	{
		const double now = hm_now(), gap = now - hm_fps.lastCommit;

		if (hm_fps.lastCommit != 0 && gap < 0.25) {
			if (gap <= 0.020) hm_fps.gapsOnTime++;
			else if (gap <= 0.050) hm_fps.gapsLate++;
			else hm_fps.gapsStall++;
		}
		hm_fps.lastCommit = now;
	}
	pthread_mutex_unlock(&hm_fps.lock);
}

/* boot-arg metal_fps=timeline (a diagnostic): each command buffer of the first 400 after the 30th second, as when it was
 * made, committed and completed (ms of this process's clock) and what the GPU spent on it -- where a frame's time goes
 * between the client, the driver and the GPU */
static BOOL
hm_timeline_on(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_fps=timeline") != NULL;
	}
	return on != 0;
}

static void
hm_timeline(double created, double committed, double completed, double gpu)
{
	static unsigned printed;
	static double first;

	if (first == 0) first = hm_now();
	if (!hm_timeline_on() || created - first < 30.0 || printed >= 400) return;
	printed++;
	hm_print("timeline: made %.1f committed %.1f done %.1f gpu %.2f\n", created * 1e3, committed * 1e3, completed * 1e3, gpu * 1e3);
}

static void
hm_completed(hakios_metal_cb *cb, void *context)
{
	HakiosMetalCommandBuffer *self = (HakiosMetalCommandBuffer *)context;

	hm_timeline([self hakiosCreatedAt], [self GPUStartTime], hm_now(), (double)hakios_metal_cb_gpu_nanoseconds(cb) / 1e9);
	{
		/* (hakios) boot-arg touch_lat=1: "tl: M made US committed US done US gpu US" of every command buffer, in
		 * the microseconds of mach_absolute_time the other "tl:" lines use (the display's tl: v and tl: y beside it) */
		static int lines = -1;

		if (lines < 0) {
			char args[1024] = "";
			size_t size = sizeof(args) - 1;

			lines = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "touch_lat=1") != NULL;
		}
		if (lines) {
			fprintf(stderr, "tl: M made %.0f committed %.0f done %.0f gpu %.0f\n", [self hakiosCreatedAt] * 1e6,
				[self GPUStartTime] * 1e6, hm_now() * 1e6, (double)hakios_metal_cb_gpu_nanoseconds(cb) / 1e3);
		}
	}
	hm_fps_note(NO, (double)hakios_metal_cb_gpu_nanoseconds(cb) / 1e9, hm_now() - [self GPUStartTime], cb);
	/* hakios-metal's queue thread has no autorelease pool of its own, and the NSError built here and whatever a
	 * handler autoreleases would otherwise leak, silently, once per command buffer */
	@autoreleasepool {
		[self hakiosCompletedNow];
	}
	[self release];
}

- (void)hakiosTurnAtCommit:(id)texture
{
	if (_turned == nil) _turned = [[NSMutableArray alloc] init];
	if (![_turned containsObject:texture]) [_turned addObject:texture];
}

- (void)commit
{
	BOOL met = YES;

	if (_committed) {
		HM_ONCE("a command buffer was committed twice; Metal asserts on that, this plug-in does nothing the second time\n");
		return;
	}
	_committed = YES;
	_committedAt = hm_now();
	/* each turned framebuffer drawn into, laid into its surface by the 2D engine after everything that drew it */
	for (HakiosMetalTexture *t in _turned) {
		[t hakiosTurnInto:_cb];
	}
	[self retain];
	for (NSArray *w in _waits) {
		met = met && [[w objectAtIndex:0] hakiosReached:[[w objectAtIndex:1] unsignedLongLongValue]];
	}
	hm_fps_encoded(_committedAt - _createdAt, !met || [_queue hakiosDeferring]);
	if (!met || [_queue hakiosDeferring]) {
		/* held: committed to the GPU from the queue's own serial queue once every value it waits for has come */
		NSArray *waits = [_waits copy];

		_sent = dispatch_semaphore_create(0);
		[self retain];
		[_queue hakiosDefer:^{
			for (NSArray *w in waits) {
				[[w objectAtIndex:0] hakiosWaitFor:[[w objectAtIndex:1] unsignedLongLongValue]];
			}
			[waits release];
			hakios_metal_cb_commit(_cb, hm_completed, self);
			dispatch_semaphore_signal(_sent);
			[self hakiosDeliverScheduled];
			[self release];
		}];
		return;
	}
	{
		const double began = hm_now();

		hakios_metal_cb_commit(_cb, hm_completed, self);
		hm_fps_commit(hm_now() - began);
	}
	[self hakiosDeliverScheduled];
}

/* abstract in _MTLCommandBuffer (its body is doesNotRecognizeSelector:), so inheriting it aborts. The deadline is
 * for a GPU scheduler to order by; there is one ring here and nothing to tell, but the COMMIT must happen: a late
 * frame is a late frame, a frame never committed is a daemon waiting for ever. */
- (void)commitWithDeadline:(NSUInteger)deadline
{
	(void)deadline;
	HM_ONCE("a command buffer was committed with a deadline; one ring, no scheduler to tell: committed, deadline not promised\n");
	[self commit];
}

- (void)waitUntilScheduled
{
	if (!_committed) {
		HM_ONCE("a command buffer was waited on for scheduling before it was committed; Metal would wait for ever, "
			"this plug-in returns\n");
	}
	/* committed: hakios_metal_cb_commit returned with it scheduled, so there is nothing left to wait for */
}

- (void)waitUntilCompleted
{
	if (!_committed) {
		HM_ONCE("a command buffer was waited on before it was committed; Metal would wait for ever, this plug-in returns\n");
		return;
	}
	if (_sent != NULL) {
		/* a held commit: first that it has reached the GPU (passed on to the next waiter) */
		dispatch_semaphore_wait(_sent, DISPATCH_TIME_FOREVER);
		dispatch_semaphore_signal(_sent);
	}
	{
		const double began = hm_now();

		hakios_metal_cb_wait(_cb);   /* returns after the completion callback has run: the handlers are delivered by then */
		hm_fps_waited(hm_now() - began);
	}
}

/* --- handlers --- */

/* where a handler runs: on the completion queue the daemon named, if it named one, else right here */
- (void)hakiosDeliver:(MTLCommandBufferHandler)handler
{
	dispatch_queue_t queue = [_queue hakiosCompletionQueue];

	if (queue != nil) {
		dispatch_async(queue, ^{ handler((id<MTLCommandBuffer>)self); });   /* the block retains the buffer and the handler */
		return;
	}
	handler((id<MTLCommandBuffer>)self);
}

- (void)hakiosDeliverScheduled
{
	NSArray *scheduled;

	@synchronized (self) {
		scheduled = _scheduled;
		_scheduled = nil;
	}
	for (id handler in scheduled) {
		[self hakiosDeliver:(MTLCommandBufferHandler)handler];
	}
	[scheduled release];
}

- (void)hakiosCompletedNow
{
	NSArray *completed;

	[self error];   /* built now, on the completing thread, so that a handler reading -error gets the sentence */
	/* the events this buffer signals, now that its work is done: before its handlers, which may read them */
	for (NSArray *sg in _signals) {
		[[sg objectAtIndex:0] hakiosSignal:[[sg objectAtIndex:1] unsignedLongLongValue]];
	}
	@synchronized (self) {
		completed = _completed;
		_completed = nil;
	}
	for (id handler in completed) {
		[self hakiosDeliver:(MTLCommandBufferHandler)handler];
	}
	[completed release];
	[self hakiosPaySlot];
}

/* A handler added once the stage it is about has passed is delivered at once rather than dropped: Metal calls that
 * undefined, and losing it would be exactly the kind of silence this file exists to stop having. */
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler
{
	if (handler == NULL) {
		return;
	}
	@synchronized (self) {
		if (!_committed) {
			if (_scheduled == nil) {
				_scheduled = [[NSMutableArray alloc] init];
			}
			[_scheduled addObject:[[handler copy] autorelease]];
			return;
		}
	}
	[self hakiosDeliver:handler];
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)handler
{
	if (handler == NULL) {
		return;
	}
	@synchronized (self) {
		if (hakios_metal_cb_status(_cb) < HAKIOS_METAL_COMPLETED) {
			if (_completed == nil) {
				_completed = [[NSMutableArray alloc] init];
			}
			[_completed addObject:[[handler copy] autorelease]];
			return;
		}
	}
	[self hakiosDeliver:handler];
}

/* --- presenting --- */

/* Metal's definition is a scheduled handler that sends the drawable -present (MTLCommandBuffer.h): its display then
 * waits for the GPU's work on the surface before it shows it. Nothing here waits so: the compositor and the display
 * read the surface as soon as it is presented, and a drawable presented when its buffer was merely scheduled was
 * shown half resolved -- sokol's clear sample, a band of black rows in some frames. So it is presented when the
 * buffer has COMPLETED, which is what Metal's display waits for anyway; the drawable is kept until then */
- (void)presentDrawable:(id)drawable
{
	if (drawable == nil) {
		return;
	}
	if (![drawable respondsToSelector:@selector(present)]) {
		hm_print("refused: -presentDrawable: of a %s, which does not answer -present; nothing is presented\n", class_getName(object_getClass(drawable)));
		return;
	}
	[drawable retain];
	[self addCompletedHandler:^(id<MTLCommandBuffer> cb) {
		(void)cb;
		[drawable present];
		[drawable release];
		hm_fps_note(YES, 0, 0, NULL);
	}];
}

- (void)presentDrawable:(id)drawable atTime:(CFTimeInterval)time
{
	(void)time;
	[self presentDrawable:drawable];
}

- (void)presentDrawable:(id)drawable afterMinimumDuration:(CFTimeInterval)duration
{
	(void)duration;
	[self presentDrawable:drawable];
}

/* MTLEvent orders work across command buffers and queues (HakiosMetalEvent): a signal is applied when this buffer
 * completes, a wait holds the buffer back from the GPU until the value has come. An event of another kind -- another
 * device's, another process's handle -- has no value here to wait for, and fails the buffer by name. */
- (void)encodeSignalEvent:(id)event value:(uint64_t)value
{
	if (![event isKindOfClass:[HakiosMetalEvent class]]) {
		hm_print("refused: a signal of a %s, not an event of this device; the command buffer is failed\n", class_getName(object_getClass(event)));
		hakios_metal_cb_refuse(_cb, "a signal of an event that is not this device's");
		return;
	}
	if (_signals == nil) _signals = [[NSMutableArray alloc] init];
	[_signals addObject:@[ event, @(value) ]];
}

- (void)encodeWaitForEvent:(id)event value:(uint64_t)value
{
	if (![event isKindOfClass:[HakiosMetalEvent class]]) {
		hm_print("refused: a wait for a %s, not an event of this device; the command buffer is failed\n", class_getName(object_getClass(event)));
		hakios_metal_cb_refuse(_cb, "a wait for an event that is not this device's");
		return;
	}
	if (_waits == nil) _waits = [[NSMutableArray alloc] init];
	[_waits addObject:@[ event, @(value) ]];
}

/* Sent by the window server to every command buffer it makes (78 unrecognized-selector aborts in one boot): declared on
 * IOGPUMetalCommandBuffer, a class and not a protocol. Protected content is memory the GPU reads and the processor
 * cannot, which this port has none of, so the value is kept and not honoured -- the window server asks for it on
 * buffers that carry no protected content at all. */
- (void)setProtectionOptions:(NSUInteger)options
{
	if (options != 0 && _protectionOptions != options) {
		HM_ONCE("a command buffer was asked for protection options %lu; there is no protected memory here, so the "
			"request is kept and not honoured\n", (unsigned long)options);
	}
	_protectionOptions = options;
}

- (NSUInteger)protectionOptions { return _protectionOptions; }

@end

/* ============================================================================================================
 * The blit encoder: copy and fill, by the GPU's command processor
 * ============================================================================================================ */

@implementation HakiosMetalBlitCommandEncoder

+ (void)load
{
	hm_adopt(self, @protocol(MTLBlitCommandEncoder), "MTLBlitCommandEncoder");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer
{
	self = [super initWithCommandBuffer:commandBuffer];   /* retains the buffer; -endEncoding gives it back */
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

/* a buffer of another device, or an object standing in for one, cannot be bound here; said by name */
static hakios_metal_buffer *
hm_buffer_of(id self, id object, const char *what)
{
	if (object == nil) {
		hm_print("refused: a blit on %s without a %s buffer\n", class_getName(object_getClass(self)), what);
		return NULL;
	}
	if (![object isKindOfClass:[HakiosMetalBuffer class]]) {
		hm_print("refused: the %s buffer is a %s, not a buffer of this device\n", what, class_getName(object_getClass(object)));
		return NULL;
	}
	return [(HakiosMetalBuffer *)object hakiosBuffer];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset toBuffer:(id)destination
     destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size
{
	hakios_metal_buffer *from = hm_buffer_of(self, source, "source");
	hakios_metal_buffer *to = hm_buffer_of(self, destination, "destination");
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];

	if (from == NULL || to == NULL) {
		hakios_metal_cb_refuse(cb, "a buffer copy named a buffer that is not this device's");
		return;
	}
	if (hakios_metal_cb_copy(cb, from, sourceOffset, to, destinationOffset, size) != 0) {
		hm_print("refused: a copy of %lu bytes: %s\n", (unsigned long)size, hakios_metal_error());   /* the buffer ends in error */
	}
}

- (void)fillBuffer:(id)buffer range:(NSRange)range value:(uint8_t)value
{
	hakios_metal_buffer *target = hm_buffer_of(self, buffer, "fill");
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];

	if (target == NULL) {
		hakios_metal_cb_refuse(cb, "a fill named a buffer that is not this device's");
		return;
	}
	if (hakios_metal_cb_fill(cb, target, range.location, range.length, value) != 0) {
		hm_print("refused: a fill of %lu bytes: %s\n", (unsigned long)range.length, hakios_metal_error());
	}
}

/* Correct as nothing, and a statement about this device rather than a shortcut: a fence orders passes within a queue,
 * and this queue runs every command buffer, and every command in it, in order -- hakios-metal invalidates the GPU's
 * caches before a dispatch and flushes them after it, so what a blit wrote is what the next dispatch reads. */
- (void)updateFence:(id)fence  { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }

/* a texture of this device, or a refusal by name (the command buffer ends in error) */
static hakios_metal_texture *
hm_blit_texture(hakios_metal_cb *cb, id object, const char *what)
{
	if (object == nil || ![object isKindOfClass:[HakiosMetalTexture class]]) {
		char why[160];

		snprintf(why, sizeof(why), "a texture copy's %s is %s, not a texture of this device", what,
			 object != nil ? class_getName(object_getClass(object)) : "nil");
		hm_print("refused: %s\n", why);
		hakios_metal_cb_refuse(cb, why);
		return NULL;
	}
	return [(HakiosMetalTexture *)object hakiosTexture];
}

/* Texture copies are hakios-metal's 2D engine (hakios_metal_cb_blit: turnip's r2d copy, one pixel format, texel for
 * texel). One slice is what a texture here has; a region with depth other than 1 is refused by name. */
- (void)copyFromTexture:(id)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
	   sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toTexture:(id)destinationTexture
       destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
      destinationOrigin:(MTLOrigin)destinationOrigin
{
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];
	hakios_metal_texture *from = hm_blit_texture(cb, sourceTexture, "source");
	hakios_metal_texture *to = from != NULL ? hm_blit_texture(cb, destinationTexture, "destination") : NULL;

	if (to == NULL) {
		return;
	}
	if (sourceSlice != 0 || destinationSlice != 0 || sourceOrigin.z != 0 || destinationOrigin.z != 0 || sourceSize.depth != 1) {
		char why[160];

		snprintf(why, sizeof(why), "a texture copy of slice %lu into slice %lu, depth %lu at z %lu: one slice and depth 1 are served",
			 (unsigned long)sourceSlice, (unsigned long)destinationSlice, (unsigned long)sourceSize.depth, (unsigned long)sourceOrigin.z);
		hm_print("refused: %s\n", why);
		hakios_metal_cb_refuse(cb, why);
		return;
	}
	if (hakios_metal_cb_blit(cb, from, (unsigned)sourceLevel, (unsigned)sourceOrigin.x, (unsigned)sourceOrigin.y, (unsigned)sourceSize.width,
				 (unsigned)sourceSize.height, to, (unsigned)destinationLevel, (unsigned)destinationOrigin.x,
				 (unsigned)destinationOrigin.y, (unsigned)sourceSize.width, (unsigned)sourceSize.height, 0) != 0) {
		/* the buffer ends in error; what was asked, for telling a copy of a texture onto itself from two textures that
		 * share their memory */
		hm_print("refused: a texture copy: %s (%s texture %p, level %lu, %lux%lu at (%lu, %lu), into %p level %lu at (%lu, %lu))\n",
			 hakios_metal_error(), sourceTexture == destinationTexture ? "one" : "two", (void *)sourceTexture,
			 (unsigned long)sourceLevel, (unsigned long)sourceSize.width, (unsigned long)sourceSize.height,
			 (unsigned long)sourceOrigin.x, (unsigned long)sourceOrigin.y, (void *)destinationTexture,
			 (unsigned long)destinationLevel, (unsigned long)destinationOrigin.x, (unsigned long)destinationOrigin.y);
	}
}

/* levelCount levels from sourceLevel, each whole, into the same levels from destinationLevel */
- (void)copyFromTexture:(id)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
	      toTexture:(id)destinationTexture destinationSlice:(NSUInteger)destinationSlice
       destinationLevel:(NSUInteger)destinationLevel sliceCount:(NSUInteger)sliceCount levelCount:(NSUInteger)levelCount
{
	if (sliceCount != 1) {
		hakios_metal_cb_refuse([_commandBuffer hakiosCommandBuffer], "a texture copy of several slices: one slice is served");
		hm_print("refused: a texture copy of %lu slices: one slice is served\n", (unsigned long)sliceCount);
		return;
	}
	for (NSUInteger l = 0; l < levelCount; l++) {
		NSUInteger w = [sourceTexture width] >> (sourceLevel + l), h = [sourceTexture height] >> (sourceLevel + l);

		[self copyFromTexture:sourceTexture sourceSlice:sourceSlice sourceLevel:sourceLevel + l sourceOrigin:MTLOriginMake(0, 0, 0)
			   sourceSize:MTLSizeMake(w ? w : 1, h ? h : 1, 1) toTexture:destinationTexture destinationSlice:destinationSlice
		     destinationLevel:destinationLevel + l destinationOrigin:MTLOriginMake(0, 0, 0)];
	}
}

/* every level, each whole: the two textures of one size and level count (Metal's own rule for this form) */
- (void)copyFromTexture:(id)sourceTexture toTexture:(id)destinationTexture
{
	NSUInteger levels = MIN([sourceTexture mipmapLevelCount], [destinationTexture mipmapLevelCount]);

	[self copyFromTexture:sourceTexture sourceSlice:0 sourceLevel:0 toTexture:destinationTexture destinationSlice:0 destinationLevel:0
		   sliceCount:1 levelCount:levels];
}

/* each level from the one above, halved with linear filtering (hakios_metal_cb_generate_mipmaps) */
- (void)generateMipmapsForTexture:(id)texture
{
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];
	hakios_metal_texture *t = hm_blit_texture(cb, texture, "texture");

	if (t != NULL && hakios_metal_cb_generate_mipmaps(cb, t) != 0) {
		hm_print("refused: generateMipmaps of a %lux%lu texture of %lu levels: %s\n", (unsigned long)[texture width],
			 (unsigned long)[texture height], (unsigned long)[texture mipmapLevelCount], hakios_metal_error());
	}
}

/* Buffer-texture copies go through the 2D engine too: the buffer's rows are a texture of the other side's format over the
 * same memory (hakios_metal_texture_new_buffer), and that texture is blitted. The GPU takes such rows only at a pitch and
 * a start that are multiples of 64 bytes; rows that are not are first copied, one GPU copy per row in the same command
 * buffer, through a staging buffer laid out that way -- sokol-gfx uploads a 320-byte-wide picture from wherever its
 * staging ring puts it. The command buffer keeps every buffer and texture it names, so the handles go at once. */
static hakios_metal_texture *
hm_rows_texture(HakiosMetalBlitCommandEncoder *self, hakios_metal_cb *cb, hakios_metal_buffer *buffer, uint64_t offset,
		uint64_t bytesPerRow, hakios_metal_texture *like, MTLSize size, hakios_metal_buffer **staging, uint64_t *stagedPitch)
{
	hakios_metal_texture *rows;

	*staging = NULL;
	*stagedPitch = bytesPerRow;
	if (offset % 64 != 0 || bytesPerRow % 64 != 0) {
		*stagedPitch = (bytesPerRow + 63) & ~(uint64_t)63;
		*staging = hakios_metal_buffer_new([(HakiosMetalDevice *)[self device] hakiosDevice], *stagedPitch * size.height);
		if (*staging == NULL) {
			return NULL;
		}
		offset = 0;
	}
	rows = hakios_metal_texture_new_buffer(*staging != NULL ? *staging : buffer, MTLTextureType2D, hakios_metal_texture_format(like),
					       (unsigned)size.width, (unsigned)size.height, 1 | 2 | 4, offset, *stagedPitch);
	(void)cb;
	return rows;
}

/* the same copy with blit options: MTLBlitOptionNone is the copy above; the others (a depth or stencil plane of a
 * combined format, PVRTC rows) are layouts this device does not have, refused by name */
- (void)copyFromBuffer:(id)sourceBuffer sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)sourceBytesPerRow
   sourceBytesPerImage:(NSUInteger)sourceBytesPerImage sourceSize:(MTLSize)sourceSize toTexture:(id)destinationTexture
      destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin options:(MTLBlitOption)options
{
	if (options != MTLBlitOptionNone) {
		char why[120];

		snprintf(why, sizeof(why), "a buffer-to-texture copy with blit options %#lx: only MTLBlitOptionNone is served", (unsigned long)options);
		hm_print("refused: %s\n", why);
		hakios_metal_cb_refuse([_commandBuffer hakiosCommandBuffer], why);
		return;
	}
	[self copyFromBuffer:sourceBuffer sourceOffset:sourceOffset sourceBytesPerRow:sourceBytesPerRow sourceBytesPerImage:sourceBytesPerImage
		  sourceSize:sourceSize toTexture:destinationTexture destinationSlice:destinationSlice destinationLevel:destinationLevel
	   destinationOrigin:destinationOrigin];
}

- (void)copyFromBuffer:(id)sourceBuffer sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)sourceBytesPerRow
   sourceBytesPerImage:(NSUInteger)sourceBytesPerImage sourceSize:(MTLSize)sourceSize toTexture:(id)destinationTexture
      destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin
{
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];
	hakios_metal_buffer *from = hm_buffer_of(self, sourceBuffer, "source");
	hakios_metal_texture *to = from != NULL ? hm_blit_texture(cb, destinationTexture, "destination") : NULL;
	hakios_metal_buffer *staging;
	hakios_metal_texture *rows;
	uint64_t pitch;
	int failed = 0;

	(void)sourceBytesPerImage;   /* one image: depth 1 is all a texture here has */
	if (from == NULL) {
		hakios_metal_cb_refuse(cb, "a buffer-to-texture copy named a buffer that is not this device's");
		return;
	}
	if (to == NULL) {
		return;
	}
	/* a layer of an array or cube, or planes of a 3D texture: the processor copies them out of the buffer's shared memory
	 * when the copy is encoded, as for block formats below -- the 2D copy writes one 2D image, and Resident Evil 4 fills
	 * its 64-layer and 3D tables so */
	if ((destinationSlice != 0 || destinationOrigin.z != 0 || sourceSize.depth != 1 ||
	     hm_layered(hakios_metal_texture_type(to)) || hakios_metal_texture_type(to) == MTLTextureType3D) &&
	    sourceSize.width != 0 && sourceSize.height != 0 && sourceSize.depth != 0) {
		const uint8_t *base = hakios_metal_buffer_contents(from);
		const BOOL planes = hakios_metal_texture_type(to) == MTLTextureType3D;

		if (base == NULL || (!planes && sourceSize.depth != 1) ||
		    hakios_metal_texture_replace_3d(to, (unsigned)destinationLevel, (unsigned)destinationOrigin.x, (unsigned)destinationOrigin.y,
						    (unsigned)(planes ? destinationOrigin.z : destinationSlice), (unsigned)sourceSize.width,
						    (unsigned)sourceSize.height, (unsigned)sourceSize.depth, base + sourceOffset, sourceBytesPerRow,
						    sourceBytesPerImage) != 0) {
			char why[200];

			snprintf(why, sizeof(why), "a buffer-to-texture copy of %lux%lux%lu into slice %lu at z %lu: %s", (unsigned long)sourceSize.width,
				 (unsigned long)sourceSize.height, (unsigned long)sourceSize.depth, (unsigned long)destinationSlice,
				 (unsigned long)destinationOrigin.z, base == NULL ? "the buffer is mapped nowhere" :
				 (!planes && sourceSize.depth != 1) ? "several slices in one copy are not served" : hakios_metal_error());
			hm_print("refused: %s\n", why);
			hakios_metal_cb_refuse(cb, why);
		}
		return;
	}
	if (sourceSize.width == 0 || sourceSize.height == 0) {
		char why[160];

		snprintf(why, sizeof(why), "a buffer-to-texture copy of %lux%lux%lu into slice %lu at z %lu: one slice, depth 1 and a size are served",
			 (unsigned long)sourceSize.width, (unsigned long)sourceSize.height, (unsigned long)sourceSize.depth,
			 (unsigned long)destinationSlice, (unsigned long)destinationOrigin.z);
		hm_print("refused: %s\n", why);
		hakios_metal_cb_refuse(cb, why);
		return;
	}
	/* A block-compressed format (BC 130-143, PVRTC 160-167, ETC2/EAC 170-183, ASTC 186-236): the 2D engine copies texels,
	 * and a buffer's rows of blocks are no texture it can read, so the blocks are copied by the processor out of the
	 * buffer's memory, which it shares with the GPU -- PVRTC decoded to the RGBA8 its texture holds -- when the copy is
	 * encoded, not when it runs: what an engine wrote into its staging buffer before encoding the upload, as engines do.
	 * Resident Evil 4 loads its ASTC and ETC2 textures so, and drew its missing-texture magenta while these were refused. */
	{
		const unsigned format = hakios_metal_texture_format(to);

		if (format >= 130 && format <= 236) {
			const uint8_t *base = hakios_metal_buffer_contents(from);

			if (base == NULL || hakios_metal_texture_replace_level(to, (unsigned)destinationLevel, (unsigned)destinationOrigin.x,
									       (unsigned)destinationOrigin.y, (unsigned)sourceSize.width,
									       (unsigned)sourceSize.height, base + sourceOffset, sourceBytesPerRow) != 0) {
				hm_print("refused: a buffer-to-texture copy of %lux%lu at (%lu, %lu) into block format %u: %s\n", (unsigned long)sourceSize.width,
					 (unsigned long)sourceSize.height, (unsigned long)destinationOrigin.x, (unsigned long)destinationOrigin.y, format,
					 base == NULL ? "the buffer is mapped nowhere" : hakios_metal_error());
				hakios_metal_cb_refuse(cb, "a buffer-to-texture copy into a block-compressed texture");
			}
			return;
		}
	}
	rows = hm_rows_texture(self, cb, from, sourceOffset, sourceBytesPerRow, to, sourceSize, &staging, &pitch);
	if (rows == NULL) {
		hm_print("refused: a buffer-to-texture copy of %lux%lu from offset %lu, %lu bytes a row: %s\n", (unsigned long)sourceSize.width,
			 (unsigned long)sourceSize.height, (unsigned long)sourceOffset, (unsigned long)sourceBytesPerRow, hakios_metal_error());
		hakios_metal_cb_refuse(cb, "a buffer-to-texture copy's rows could not be made a texture");
		if (staging != NULL) {
			hakios_metal_buffer_free(staging);
		}
		return;
	}
	if (staging != NULL) {
		uint64_t length = hakios_metal_buffer_length(from);

		for (NSUInteger y = 0; y < sourceSize.height && !failed; y++) {
			uint64_t at = sourceOffset + (uint64_t)y * sourceBytesPerRow;
			uint64_t n = at < length ? MIN((uint64_t)sourceBytesPerRow, length - at) : 0;   /* the last row may end early */

			failed = hakios_metal_cb_copy(cb, from, at, staging, (uint64_t)y * pitch, n) != 0;
		}
	}
	if (!failed) {
		failed = hakios_metal_cb_blit(cb, rows, 0, 0, 0, (unsigned)sourceSize.width, (unsigned)sourceSize.height, to,
					      (unsigned)destinationLevel, (unsigned)destinationOrigin.x, (unsigned)destinationOrigin.y,
					      (unsigned)sourceSize.width, (unsigned)sourceSize.height, 0) != 0;
	}
	if (failed) {
		hm_print("refused: a buffer-to-texture copy of %lux%lu: %s\n", (unsigned long)sourceSize.width,
			 (unsigned long)sourceSize.height, hakios_metal_error());   /* the buffer ends in error */
	}
	hakios_metal_texture_free(rows);
	if (staging != NULL) {
		hakios_metal_buffer_free(staging);
	}
}

- (void)copyFromTexture:(id)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
	   sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toBuffer:(id)destinationBuffer
      destinationOffset:(NSUInteger)destinationOffset destinationBytesPerRow:(NSUInteger)destinationBytesPerRow
    destinationBytesPerImage:(NSUInteger)destinationBytesPerImage
{
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];
	hakios_metal_texture *from = hm_blit_texture(cb, sourceTexture, "source");
	hakios_metal_buffer *to = from != NULL ? hm_buffer_of(self, destinationBuffer, "destination") : NULL;
	hakios_metal_buffer *staging;
	hakios_metal_texture *rows;
	uint64_t pitch;
	int failed;

	(void)destinationBytesPerImage;
	if (from == NULL) {
		return;
	}
	if (to == NULL) {
		hakios_metal_cb_refuse(cb, "a texture-to-buffer copy named a buffer that is not this device's");
		return;
	}
	if (sourceSlice != 0 || sourceOrigin.z != 0 || sourceSize.depth != 1 || sourceSize.width == 0 || sourceSize.height == 0) {
		char why[160];

		snprintf(why, sizeof(why), "a texture-to-buffer copy of %lux%lux%lu from slice %lu at z %lu: one slice, depth 1 and a size are served",
			 (unsigned long)sourceSize.width, (unsigned long)sourceSize.height, (unsigned long)sourceSize.depth,
			 (unsigned long)sourceSlice, (unsigned long)sourceOrigin.z);
		hm_print("refused: %s\n", why);
		hakios_metal_cb_refuse(cb, why);
		return;
	}
	rows = hm_rows_texture(self, cb, to, destinationOffset, destinationBytesPerRow, from, sourceSize, &staging, &pitch);
	if (rows == NULL) {
		hm_print("refused: a texture-to-buffer copy of %lux%lu to offset %lu, %lu bytes a row: %s\n", (unsigned long)sourceSize.width,
			 (unsigned long)sourceSize.height, (unsigned long)destinationOffset, (unsigned long)destinationBytesPerRow, hakios_metal_error());
		hakios_metal_cb_refuse(cb, "a texture-to-buffer copy's rows could not be made a texture");
		if (staging != NULL) {
			hakios_metal_buffer_free(staging);
		}
		return;
	}
	failed = hakios_metal_cb_blit(cb, from, (unsigned)sourceLevel, (unsigned)sourceOrigin.x, (unsigned)sourceOrigin.y,
				      (unsigned)sourceSize.width, (unsigned)sourceSize.height, rows, 0, 0, 0, (unsigned)sourceSize.width,
				      (unsigned)sourceSize.height, 0) != 0;
	if (staging != NULL) {
		uint64_t length = hakios_metal_buffer_length(to);

		/* only the row's own bytes: what lies between rows in the destination is the caller's and is not written */
		uint64_t rowBytes = (uint64_t)hakios_metal_texture_bytes_per_pixel(from) * sourceSize.width;

		for (NSUInteger y = 0; y < sourceSize.height && !failed; y++) {
			uint64_t at = destinationOffset + (uint64_t)y * destinationBytesPerRow;
			uint64_t n = at < length ? MIN(MIN(rowBytes, (uint64_t)destinationBytesPerRow), length - at) : 0;

			failed = hakios_metal_cb_copy(cb, staging, (uint64_t)y * pitch, to, at, n) != 0;
		}
	}
	if (failed) {
		hm_print("refused: a texture-to-buffer copy of %lux%lu: %s\n", (unsigned long)sourceSize.width,
			 (unsigned long)sourceSize.height, hakios_metal_error());
	}
	hakios_metal_texture_free(rows);
	if (staging != NULL) {
		hakios_metal_buffer_free(staging);
	}
}

/* indirect command buffers: every one of those selectors is declared by <MTLBlitCommandEncoder>, so each is refused
 * through -forwardInvocation: with its name and its caller */

/* the base class retains the command buffer in its initialiser and releases it only here; its -dealloc aborts on an
 * encoder released without this */
- (void)endEncoding
{
	[super endEncoding];
}

@end

/* ============================================================================================================
 * The compute encoder: bindings and dispatches, each dispatch recorded into hakios-metal's command buffer
 * ============================================================================================================ */

@implementation HakiosMetalComputeCommandEncoder

+ (void)load
{
	hm_adopt(self, @protocol(MTLComputeCommandEncoder), "MTLComputeCommandEncoder");
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	NSMethodSignature *s = [super methodSignatureForSelector:sel];

	return s != nil ? s : hm_protocol_signature([self class], sel);
}

/* A refused binding (a function table, a stage-in region, an acceleration structure ...) is remembered, and the next
 * dispatch is FAILED by it: a kernel run without one of its inputs computes something, and that something would look
 * like a result. Textures and samplers no longer come this way: they have methods below. */
- (void)forwardInvocation:(NSInvocation *)invocation
{
	hm_forward(self, invocation);
	if (_refused[0] == '\0') {
		snprintf(_refused, sizeof(_refused), "-%s was refused on this encoder", sel_getName([invocation selector]));
	}
}

- (void)doesNotRecognizeSelector:(SEL)sel
{
	hm_refusal(self, sel, "is declared by no protocol in this process and implemented by nothing in its class chain; it raises");
	[super doesNotRecognizeSelector:sel];
}

- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (void)dealloc
{
	[_pipeline release];
	for (unsigned i = 0; i < HM_MAX_BUFFERS; i++) {
		[_buffers[i] release];
	}
	for (unsigned i = 0; i < HM_MAX_TEXTURES; i++) {
		[_textures[i] release];
	}
	for (unsigned i = 0; i < HM_MAX_SAMPLERS; i++) {
		[_samplers[i] release];
	}
	[_scratch release];
	[super dealloc];
}

- (id)device { return [_commandBuffer device]; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }

- (void)setComputePipelineState:(id)state
{
	if (state != nil && ![state isKindOfClass:[HakiosMetalComputePipelineState class]]) {
		hm_print("refused: a pipeline state of class %s is not this device's\n", class_getName(object_getClass(state)));
		snprintf(_refused, sizeof(_refused), "the pipeline state is not this device's");
		return;
	}
	[state retain];
	[_pipeline release];
	_pipeline = state;
}

- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= HM_MAX_BUFFERS) {
		hm_print("refused: buffer index %lu is past Metal's table of %d\n", (unsigned long)index, HM_MAX_BUFFERS);
		snprintf(_refused, sizeof(_refused), "a buffer index past %d", HM_MAX_BUFFERS);
		return;
	}
	if (buffer != nil && ![buffer isKindOfClass:[HakiosMetalBuffer class]]) {
		hm_print("refused: the buffer at index %lu is a %s, not a buffer of this device\n", (unsigned long)index,
			 class_getName(object_getClass(buffer)));
		snprintf(_refused, sizeof(_refused), "the buffer at index %lu is not this device's", (unsigned long)index);
		return;
	}
	[buffer retain];
	[_buffers[index] release];
	_buffers[index] = buffer;
	_offsets[index] = offset;
	if (buffer != nil && index + 1 > _highest) {
		_highest = (unsigned)index + 1;
	}
}

/* Metal's rule: this moves a buffer already bound at the index, so with nothing bound there is nothing to move */
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index < HM_MAX_BUFFERS && _buffers[index] != nil) {
		_offsets[index] = offset;
	}
}

- (void)setBuffers:(const id *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

/* Metal's shorthand for small constant data: a buffer here, because that is what the kernel's binding is. The scratch
 * buffer is the encoder's until -endEncoding; hakios-metal keeps it alive past that until the GPU is done with it. */
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	HakiosMetalBuffer *buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer == nil) {
		snprintf(_refused, sizeof(_refused), "no scratch buffer for %lu bytes at index %lu", (unsigned long)length, (unsigned long)index);
		return;
	}
	if (_scratch == nil) {
		_scratch = [[NSMutableArray alloc] init];
	}
	[_scratch addObject:buffer];
	[self setBuffer:buffer offset:0 atIndex:index];
	[buffer release];
}

/*
 * Textures and samplers, stored BY INDEX and nothing more: Metal allows -setTexture: before -setComputePipelineState:, so
 * the kernel cannot be consulted here, and it is the dispatch (hakios_metal_cb_dispatch2, from the kernel's own declaration
 * of each index as a sampled texture or a written image) that routes a Metal texture index to a TEX_CONST slot or an IBO
 * slot, matches the texture's format against the kernel's element class and refuses by name what does not fit. The class
 * check is the one -setBuffer: makes: an object of another device, or one standing in for a texture, fails the dispatch.
 * Bound objects are retained until the encoder goes, as the buffers are; the dispatch keeps its own references past that.
 */
- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= HM_MAX_TEXTURES) {
		hm_print("refused: texture index %lu is past Metal's table of %d\n", (unsigned long)index, HM_MAX_TEXTURES);
		snprintf(_refused, sizeof(_refused), "a texture index past %d", HM_MAX_TEXTURES);
		return;
	}
	if (texture != nil && ![texture isKindOfClass:[HakiosMetalTexture class]]) {
		hm_print("refused: the texture at index %lu is a %s, not a texture of this device\n", (unsigned long)index,
			 class_getName(object_getClass(texture)));
		snprintf(_refused, sizeof(_refused), "the texture at index %lu is not this device's", (unsigned long)index);
		return;
	}
	[texture retain];
	[_textures[index] release];
	_textures[index] = texture;
	/* a surface read through a copy: the copy as the surface is now, and what a kernel writes back into the surface
	 * once the command buffer is done */
	[(HakiosMetalTexture *)texture hakiosRefreshShadow];
	if ([(HakiosMetalTexture *)texture hakiosIsShadow] && ([(HakiosMetalTexture *)texture usage] & MTLTextureUsageShaderWrite) != 0) {
		HakiosMetalTexture *written = [(HakiosMetalTexture *)texture retain];
		[_commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
			(void)cb;
			[written hakiosWriteBackShadow];
			[written release];
		}];
	}
	if (texture != nil && index + 1 > _highestTexture) {
		_highestTexture = (unsigned)index + 1;
	}
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= HM_MAX_SAMPLERS) {
		hm_print("refused: sampler index %lu is past Metal's table of %d\n", (unsigned long)index, HM_MAX_SAMPLERS);
		snprintf(_refused, sizeof(_refused), "a sampler index past %d", HM_MAX_SAMPLERS);
		return;
	}
	if (sampler != nil && ![sampler isKindOfClass:[HakiosMetalSamplerState class]]) {
		hm_print("refused: the sampler at index %lu is a %s, not a sampler state of this device\n", (unsigned long)index,
			 class_getName(object_getClass(sampler)));
		snprintf(_refused, sizeof(_refused), "the sampler at index %lu is not this device's", (unsigned long)index);
		return;
	}
	[sampler retain];
	[_samplers[index] release];
	_samplers[index] = sampler;
	if (sampler != nil && index + 1 > _highestSampler) {
		_highestSampler = (unsigned)index + 1;
	}
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/* The LOD clamps narrow a sampler's range per binding; on this GPU they are TEX_SAMP_1's MIN_LOD/MAX_LOD (Mesa
 * fd6_texture.cc:285-286), which hakios-metal computes once per sampler state and the C interface takes no per-binding
 * variant of yet. So the sampler is bound as made, and the clamps are applied only when they ask for nothing the default
 * does not already give: a minimum of 0 and a maximum at or beyond 1000 (Metal's FLT_MAX default, far past any level a
 * one-level texture has). Anything narrower is said once, not silently dropped. */
- (void)setSamplerState:(id)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index
{
	if (!(lodMinClamp == 0.0f && lodMaxClamp >= 1000.0f)) {
		HM_ONCE("a sampler was bound with LOD clamps %g..%g at index %lu; this plug-in binds the sampler as it was made and "
			"does NOT apply per-binding clamps yet (the textures here have one level, which the clamps could only hide)\n",
			(double)lodMinClamp, (double)lodMaxClamp, (unsigned long)index);
	}
	[self setSamplerState:sampler atIndex:index];
}

- (void)setSamplerStates:(const id *)samplers lodMinClamps:(const float *)lodMinClamps lodMaxClamps:(const float *)lodMaxClamps withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] lodMinClamp:lodMinClamps[i] lodMaxClamp:lodMaxClamps[i] atIndex:range.location + i];
	}
}

/*
 * Threadgroup memory a kernel declares without a size, for the encoder to supply ([[threadgroup(n)]] with no bound).
 * Index 0 is the argument the front end places past the kernel's static arrays, and each dispatch that follows is
 * given that much past them (hakios_metal_cb_set_threadgroup_length). A second index would start where the first one's
 * length ends, a layout the front end does not make, so a real length there fails the encoder's dispatches rather than
 * have the kernel read memory nobody gave it. (The compositor sends this for its display-dimming kernel.)
 */
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	if (index == 0) {
		_threadgroupLength = length;
		return;
	}
	if (length != 0) {
		hm_print("refused: %lu bytes of threadgroup memory at index %lu: one threadgroup argument (index 0) is served; "
			 "this encoder's dispatches are failed\n", (unsigned long)length, (unsigned long)index);
		snprintf(_refused, sizeof(_refused), "%lu bytes of dynamic threadgroup memory at index %lu cannot be given",
			 (unsigned long)length, (unsigned long)index);
	}
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads
{
	[self hakiosDispatch:groups indirect:nil offset:0 threads:threads];
}

/* both dispatches: GROUPS, or (ARGS not nil) the counts three words at OFFSET of ARGS, read by the GPU as it runs */
- (void)hakiosDispatch:(MTLSize)groups indirect:(id)args offset:(NSUInteger)offset threads:(MTLSize)threads
{
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];
	hakios_metal_buffer *buffers[HM_MAX_BUFFERS];
	uint64_t offsets[HM_MAX_BUFFERS];
	hakios_metal_texture *textures[HM_MAX_TEXTURES];
	hakios_metal_sampler *samplers[HM_MAX_SAMPLERS];
	const unsigned g[3] = { (unsigned)groups.width, (unsigned)groups.height, (unsigned)groups.depth };
	const unsigned t[3] = { (unsigned)threads.width, (unsigned)threads.height, (unsigned)threads.depth };

	if (_pipeline == nil) {
		hm_print("refused: a dispatch with no pipeline state; the command buffer is failed\n");
		hakios_metal_cb_refuse(cb, "a dispatch with no pipeline state");
		return;
	}
	if (_refused[0] != '\0') {
		hm_print("refused: this dispatch, because %s; the command buffer is failed\n", _refused);
		hakios_metal_cb_refuse(cb, _refused);
		return;
	}
	for (unsigned i = 0; i < _highest; i++) {
		buffers[i] = _buffers[i] != nil ? [_buffers[i] hakiosBuffer] : NULL;
		offsets[i] = _offsets[i];
	}
	for (unsigned i = 0; i < _highestTexture; i++) {
		textures[i] = _textures[i] != nil ? [_textures[i] hakiosTexture] : NULL;
	}
	for (unsigned i = 0; i < _highestSampler; i++) {
		samplers[i] = _samplers[i] != nil ? [_samplers[i] hakiosSampler] : NULL;
	}
	/* hakios-metal judges the bindings against the kernel's own declaration (a missing buffer, texture or sampler, an
	 * offset that is not a multiple of 4, a size the metadata contradicts, a texture whose format is not the kernel's
	 * element class or that was made without write usage where the kernel writes) and refuses by name; the command
	 * buffer then ends in error with that sentence, and it is printed here as well. The arrays go up to their
	 * watermarks: an entry past what the kernel declares is ignored there, as Metal ignores it. */
	hakios_metal_cb_set_threadgroup_length(cb, (unsigned)_threadgroupLength);
	if (args != nil && ![args respondsToSelector:@selector(hakiosBuffer)]) {
		hm_print("refused: an indirect dispatch whose argument buffer is not this device's; the command buffer is failed\n");
		hakios_metal_cb_refuse(cb, "an indirect dispatch's argument buffer from another device");
		return;
	}
	if (args != nil ? hakios_metal_cb_dispatch_indirect(cb, [_pipeline hakiosKernel], [(HakiosMetalBuffer *)args hakiosBuffer], (uint64_t)offset, t,
							    buffers, offsets, _highest, textures, _highestTexture, samplers, _highestSampler) != 0
			: hakios_metal_cb_dispatch2(cb, [_pipeline hakiosKernel], g, t, buffers, offsets, _highest, textures, _highestTexture,
						    samplers, _highestSampler) != 0) {
		hm_print("refused: a dispatch of %s over %ux%ux%u groups of %ux%ux%u with %u buffer, %u texture and %u sampler slots: %s\n",
			 hakios_metal_kernel_name([_pipeline hakiosKernel]), g[0], g[1], g[2], t[0], t[1], t[2], _highest, _highestTexture,
			 _highestSampler, hakios_metal_error());
		hakios_metal_cb_refuse(cb, hakios_metal_error());
	}
}

/* Metal counts threads here and threadgroups above; a grid that does not divide evenly is refused rather than rounded,
 * because rounding would run threads the application did not ask for (hakios-metal has no partial-group support) */
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)size
{
	MTLSize groups;

	if (size.width == 0 || size.height == 0 || size.depth == 0 || threads.width % size.width != 0 ||
	    threads.height % size.height != 0 || threads.depth % size.depth != 0) {
		hm_print("refused: %lux%lux%lu threads do not divide into groups of %lux%lux%lu; the command buffer is failed\n",
			 (unsigned long)threads.width, (unsigned long)threads.height, (unsigned long)threads.depth,
			 (unsigned long)size.width, (unsigned long)size.height, (unsigned long)size.depth);
		hakios_metal_cb_refuse([_commandBuffer hakiosCommandBuffer], "a thread grid that does not divide into its threadgroups");
		return;
	}
	groups = MTLSizeMake(threads.width / size.width, threads.height / size.height, threads.depth / size.depth);
	[self dispatchThreadgroups:groups threadsPerThreadgroup:size];
}

/* An indirect dispatch reads its grid out of GPU memory as it runs (CP_EXEC_CS_INDIRECT): an earlier dispatch may
 * write it, so nothing here reads it; the core copies it where the kernel's own grid constants load from */
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)size
{
	[self hakiosDispatch:MTLSizeMake(0, 0, 0) indirect:buffer offset:offset threads:size];
}

/*
 * Served by construction, every one of these: hakios-metal invalidates the GPU's caches before each dispatch and
 * flushes them after it, one queue runs its command buffers in commit order, and every allocation is resident for its
 * whole life. A barrier, a fence or a residency request asks for an ordering that is already the only one there is.
 */
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope { (void)scope; }
- (void)memoryBarrierWithResources:(const id *)resources count:(NSUInteger)count { (void)resources; (void)count; }
- (void)updateFence:(id)fence  { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }
- (void)useResource:(id)resource usage:(MTLResourceUsage)usage { (void)resource; (void)usage; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(MTLResourceUsage)usage { (void)resources; (void)count; (void)usage; }
- (void)useHeap:(id)heap { (void)heap; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count { (void)heaps; (void)count; }

/* abstract in _MTLCommandEncoder (inheriting it aborts); a packing hint the compiler decides for itself here */
- (void)setThreadgroupPackingDisabled:(BOOL)disabled { (void)disabled; }

/* The three the AGX compute context answers and <MTLComputeCommandEncoderSPI> declares (a protocol not in this
 * process): substreams are an Apple GPU's several independent streams, ordered by progress counters. There is one
 * stream here, replayed in record order, so selecting another cannot move work, a signal has nobody to reach and a
 * wait is already satisfied -- stricter ordering than asked for, which is the safe direction. A non-zero substream is
 * the case where that argument stops being true, hence the line. */
- (void)setSubstream:(unsigned)substream
{
	if (substream != 0) {
		HM_ONCE("a dispatch was directed to substream %u; one stream here, so it runs in record order instead\n", substream);
	}
}
- (void)signalProgress:(unsigned)value { (void)value; }
- (void)waitForProgress:(unsigned)value { (void)value; }

/* function tables, acceleration structures, stage-in regions, imageblocks and indirect command buffers are declared by
 * <MTLComputeCommandEncoder> and refused through -forwardInvocation: above, by name */

- (void)endEncoding
{
	/* the bindings were handed to hakios-metal at each dispatch, which holds them until the GPU is done */
	[_scratch release];
	_scratch = nil;
	[super endEncoding];   /* MUST: the base class retains the command buffer and this is what releases it */
}

@end

/* ============================================================================================================
 * The render pipeline state: one linked vertex + fragment program
 * ============================================================================================================ */

@implementation HakiosMetalRenderPipelineState

/* the base declares the public protocol itself; declared again so that a conformsToProtocol: is a static yes whatever
 * this Metal's adoption list says, as the sampler does */
+ (void)load
{
	hm_adopt(self, @protocol(MTLRenderPipelineState), "MTLRenderPipelineState");
}

/* boot-arg metal_trace_release (a diagnostic): the release that frees a render pipeline state says so, with its label
 * and who called -- each frame as its image and the offset in it, for the host to name (ipsw dyld a2s). A state freed
 * while a client still draws with it is otherwise seen only as some other object at its address. */
- (oneway void)release
{
	static int traced = -1;

	if (traced < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;
		traced = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_trace_release") != NULL;
	}
	if (traced && [self retainCount] == 1) {
		char what[128];
		NSString *label = [(id<MTLRenderPipelineState>)self label];
		snprintf(what, sizeof(what), "trace: freeing render pipeline state %p \"%s\":", (void *)self,
			 label != nil ? [label UTF8String] : "-");
		hm_print_callers(what);
	}
	[super release];
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device program:(hakios_metal_program *)program
			  descriptor:(MTLRenderPipelineDescriptor *)descriptor
{
	/* the designated initialiser, given a descriptor -newRenderPipelineStateWithDescriptor:error: has already checked
	 * (non-nil, the right class, no tessellation-with-ICB): it keeps the label (copied) and the device, reads
	 * supportIndirectCommandBuffers, textureWriteRoundingMode and meshFunction (nil here), and answers <MTLRenderPipelineState> from them */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		hakios_metal_program_free(program);
		return nil;
	}
	_hakiosDevice = [device retain];
	_program = program;
	_hakiosUniqueIdentifier = hm_next_resource_id();
	return self;
}

- (void)dealloc
{
	for (id v in [_specialised objectEnumerator]) {
		if ([v isKindOfClass:[NSValue class]]) hakios_metal_program_free((hakios_metal_program *)[v pointerValue]);
	}
	[_specialised release];
	[_fsAir release];
	if (_program != NULL) {
		hakios_metal_program_free(_program);   /* a draw in flight keeps its own reference */
	}
	[_hakiosDevice release];
	[super dealloc];
}

- (hakios_metal_program *)hakiosProgram { return _program; }

/* boot-arg metal_nospec: every draw with the pipeline's own program, no uber shader specialised */
static BOOL
hm_spec_off(void)
{
	static int off = -1;

	if (off < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;

		off = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_nospec") != NULL;
	}
	return off != 0;
}

/* An uber shader -- a fragment stage of thousands of instructions that reads a small constant buffer -- takes its path
 * through them by that buffer's bytes in every fragment: CoreAnimation's fixed_frag_lph_cpf is 15 000 instructions and
 * one of its draws took 9.6 ms over 1.2 million pixels with its textures' reads gone or not, when the path its
 * configuration takes is 16 to 48 instructions. Such a pipeline keeps its fragment function so that a program can be
 * compiled for each content of the buffer (hakios_metal_program_specialize). */
- (void)hakiosKeepFragment:(NSData *)air constants:(const hakios_metal_constant *)constants count:(unsigned)count
{
	_specBinding = -1;
	if (hm_spec_off() || hakios_metal_program_stage_words(_program, 1) < 4096) return;
	for (unsigned i = 0; i < hakios_metal_program_resource_count(_program, 1); i++) {
		unsigned kind, index, size;

		if (hakios_metal_program_resource(_program, 1, i, &kind, &index, &size) == 0 && kind == HAKIOS_METAL_RESOURCE_CONSTANT_BUFFER &&
		    size != 0 && size <= 256 && (_specBinding < 0 || (unsigned)index < (unsigned)_specBinding)) {
			_specBinding = (int)index;
			_specLength = size;
		}
	}
	if (_specBinding < 0) return;
	_fsAir = [air retain];
	_fsConstantCount = count < 64 ? count : 64;
	memcpy(_fsConstants, constants, _fsConstantCount * sizeof(*constants));
	_specialised = [[NSMutableDictionary alloc] init];
}

/* the program for a draw whose fragment buffers are BUFFERS: the one compiled for what the deciding buffer holds, or
 * the pipeline's own while that one compiles (on a queue of its own: a compilation is 0.4 s on the host, and a frame
 * must not wait for it). 64 contents at most a pipeline; past them the pipeline's own. */
- (hakios_metal_program *)hakiosProgramFor:(hakios_metal_buffer *const *)buffers offsets:(const uint64_t *)offsets count:(unsigned)count
{
	static dispatch_queue_t compiling;
	static dispatch_once_t once;

	if (_specBinding < 0 || (unsigned)_specBinding >= count || buffers[_specBinding] == NULL) return _program;
	const uint8_t *bytes = (const uint8_t *)hakios_metal_buffer_contents(buffers[_specBinding]);
	const uint64_t length = hakios_metal_buffer_length(buffers[_specBinding]), at = offsets[_specBinding];
	if (bytes == NULL || at > length || length - at < _specLength) return _program;
	NSData *key = [NSData dataWithBytes:bytes + at length:_specLength];
	hakios_metal_program *found = _program;
	BOOL start = NO;

	@synchronized(self) {
		id have = [_specialised objectForKey:key];

		if ([have isKindOfClass:[NSValue class]]) {
			found = (hakios_metal_program *)[have pointerValue];
		} else if (have == nil && [_specialised count] < 64) {
			[_specialised setObject:[NSNull null] forKey:key];
			start = YES;
		}
	}
	if (start) {
		dispatch_once(&once, ^{
			compiling = dispatch_queue_create("hakios.metal.specialise", DISPATCH_QUEUE_SERIAL);
		});
		[self retain];
		dispatch_async(compiling, ^{
			struct timespec t0, t1;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			hakios_metal_program *p = hakios_metal_program_specialize(_program, [_fsAir bytes], [_fsAir length], _fsConstants, _fsConstantCount,
										 (unsigned)_specBinding, [key bytes], [key length]);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (p == NULL) {
				hm_print("specialise: %s for its buffer %d: %s; its own program goes on\n", hakios_metal_program_name(_program), _specBinding,
					 hakios_metal_error());
			} else if (hm_fps_on()) {
				hm_print("specialise: %s for %u bytes of its buffer %d: %u instruction words, not %u, in %.0f ms\n", hakios_metal_program_name(_program),
					 _specLength, _specBinding, hakios_metal_program_stage_words(p, 1), hakios_metal_program_stage_words(_program, 1),
					 (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
			}
			@synchronized(self) {
				if (p != NULL) [_specialised setObject:[NSValue valueWithPointer:p] forKey:key];
			}
			[self release];
		});
	}
	return found;
}

/* The base's uniqueIdentifier is a nil-stub and its gpuResourceID is the resourceIndex ivar (0): a pipeline would be
 * told apart from every other by nothing. Answered from the textures' counter, for the same reason (never 0, never an
 * address). Matters only to argument buffers and indirect command buffers, neither of which is here. */
- (uint64_t)uniqueIdentifier { return _hakiosUniqueIdentifier; }

- (MTLResourceID)gpuResourceID
{
	MTLResourceID rid = { _hakiosUniqueIdentifier };

	return rid;
}

@end

/* ============================================================================================================
 * The render encoder: one colour attachment, state by value, each draw one pass of hakios-metal's
 * ============================================================================================================ */

@implementation HakiosMetalRenderCommandEncoder

/* The public protocol is defined only in the AGX bundle on this phone (as MTLComputeCommandEncoder is); the @protocol()
 * reference compiles it into this image, and the catch-all finds every member's encoding through it. */
+ (void)load
{
	hm_adopt(self, @protocol(MTLRenderCommandEncoder), "MTLRenderCommandEncoder");
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	NSMethodSignature *s = [super methodSignatureForSelector:sel];

	return s != nil ? s : hm_protocol_signature([self class], sel);
}

/* A refused binding or state (tile buffers, function tables, imageblocks ...) is remembered and the next draw is FAILED
 * by it, as the compute encoder does: a pass drawn without one of its inputs draws something, and that something would
 * look like a result. */
- (void)forwardInvocation:(NSInvocation *)invocation
{
	hm_forward(self, invocation);
	if (_refused[0] == '\0') {
		snprintf(_refused, sizeof(_refused), "-%s was refused on this encoder", sel_getName([invocation selector]));
	}
}

/* Reached for the 34 methods _MTLCommandEncoder declares ABSTRACT (their bodies are doesNotRecognizeSelector:): the
 * object and mesh stage setters, invalidateCompressedTexture: and setThreadgroupPackingDisabled: (overridden below).
 * They are members of <MTLRenderCommandEncoder>, so the usual sentence would be false; this one says what they are. */
- (void)doesNotRecognizeSelector:(SEL)sel
{
	hm_refusal(self, sel, hm_protocol_signature([self class], sel) != nil
			       ? "is abstract in Metal's _MTLCommandEncoder (an object/mesh stage or compressed-texture method); "
				 "mesh shading is not served here, and it raises as Metal's own body does"
			       : "is declared by no protocol in this process and implemented by nothing in its class chain; it raises");
	[super doesNotRecognizeSelector:sel];
}

- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer pass:(const struct hakios_metal_render_pass *)pass
			       colorTexture:(HakiosMetalTexture *)texture
{
	return [self initWithHakiosCommandBuffer:commandBuffer pass:pass colorTexture:texture sub:NULL];
}

/* where this encoder's work is recorded: the command buffer's, or a sub-encoder's own */
- (hakios_metal_cb *)hakiosCb
{
	return _sub != NULL ? _sub : [_commandBuffer hakiosCommandBuffer];
}

- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer pass:(const struct hakios_metal_render_pass *)pass
			       colorTexture:(HakiosMetalTexture *)texture sub:(hakios_metal_cb *)sub
{
	self = [super initWithCommandBuffer:commandBuffer];   /* retains the buffer; -endEncoding gives it back */
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	_sub = sub;
	_colorTexture = [texture retain];
	_pass = *pass;
	_frontFacingWinding = MTLWindingClockwise;   /* Metal's default */
	return self;
}

- (void)hakiosTakeResolves:(MTLRenderPassDescriptor *)descriptor
{
	for (NSUInteger i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *c = [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		MTLStoreAction a = [c storeAction];

		if (c == nil || [c texture] == nil || [c resolveTexture] == nil) {
			continue;
		}
		_resolveFrom[i] = [(HakiosMetalTexture *)[c texture] retain];
		_resolveTo[i] = [(HakiosMetalTexture *)[c resolveTexture] retain];
		_resolveLevel[i] = (unsigned)[c resolveLevel];
		_resolving[i] = a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve || a == MTLStoreActionUnknown;
	}
}

- (void)dealloc
{
	for (unsigned i = 0; i < 8; i++) {
		[_resolveFrom[i] release];
		[_resolveTo[i] release];
	}
	[_colorTexture release];
	[_pipeline release];
	for (unsigned i = 0; i < HM_MAX_BUFFERS; i++) {
		[_vsBuffers[i] release];
		[_fsBuffers[i] release];
	}
	for (unsigned i = 0; i < HM_MAX_TEXTURES; i++) {
		[_fsTextures[i] release];
		[_vsTextures[i] release];
	}
	for (unsigned i = 0; i < HM_MAX_SAMPLERS; i++) {
		[_fsSamplers[i] release];
		[_vsSamplers[i] release];
	}
	[_scratch release];
	[_visibilityCounted release];
	[super dealloc];
}

- (id)device { return [_commandBuffer device]; }

/* --- the pipeline --- */

- (void)setRenderPipelineState:(id)state
{
	if (state != nil && ![state isKindOfClass:[HakiosMetalRenderPipelineState class]]) {
		static int told;
		char what[160];
		snprintf(what, sizeof(what), "refused: a render pipeline state %p of class %s is not this device's:", (void *)state,
			 class_getName(object_getClass(state)));
		if (told++ < 4) hm_print_callers(what);
		else hm_print("%s\n", what);
		snprintf(_refused, sizeof(_refused), "the render pipeline state is not this device's");
		return;
	}
	[state retain];
	[_pipeline release];
	_pipeline = state;
}

/* --- viewport and scissor: stored, handed to the draw; unset means the whole attachment (znear 0, zfar 1) --- */

- (void)setViewport:(MTLViewport)viewport
{
	_viewport.x = viewport.originX;
	_viewport.y = viewport.originY;
	_viewport.width = viewport.width;
	_viewport.height = viewport.height;
	_viewport.znear = viewport.znear;
	_viewport.zfar = viewport.zfar;
	_hasViewport = YES;
}

- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count
{
	if (count != 1 || viewports == NULL) {
		hm_print("refused: %lu viewports; one viewport is served (no viewport arrays, no layer/viewport index output); the next draw is failed\n",
			 (unsigned long)count);
		snprintf(_refused, sizeof(_refused), "%lu viewports were set; one is served", (unsigned long)count);
		return;
	}
	[self setViewport:viewports[0]];
}

- (void)setScissorRect:(MTLScissorRect)rect
{
	_scissor.x = (unsigned)rect.x;
	_scissor.y = (unsigned)rect.y;
	_scissor.width = (unsigned)rect.width;
	_scissor.height = (unsigned)rect.height;
	_hasScissor = YES;
}

- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count
{
	if (count != 1 || rects == NULL) {
		hm_print("refused: %lu scissor rectangles; one is served; the next draw is failed\n", (unsigned long)count);
		snprintf(_refused, sizeof(_refused), "%lu scissor rectangles were set; one is served", (unsigned long)count);
		return;
	}
	[self setScissorRect:rects[0]];
}

/* --- the buffer tables, one per stage, stored by index as the compute encoder stores its own --- */

/* a buffer of this device at INDEX of TABLE, or the reason it cannot be; nil unbinds */
- (void)hakiosBind:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index stage:(const char *)stage
	     table:(HakiosMetalBuffer **)table offsets:(uint64_t *)offsets highest:(unsigned *)highest
{
	if (index >= HM_MAX_BUFFERS) {
		hm_print("refused: %s buffer index %lu is past Metal's table of %d\n", stage, (unsigned long)index, HM_MAX_BUFFERS);
		snprintf(_refused, sizeof(_refused), "a %s buffer index past %d", stage, HM_MAX_BUFFERS);
		return;
	}
	if (buffer != nil && ![buffer isKindOfClass:[HakiosMetalBuffer class]]) {
		hm_print("refused: the %s buffer at index %lu is a %s, not a buffer of this device\n", stage, (unsigned long)index,
			 class_getName(object_getClass(buffer)));
		snprintf(_refused, sizeof(_refused), "the %s buffer at index %lu is not this device's", stage, (unsigned long)index);
		return;
	}
	[buffer retain];
	[table[index] release];
	table[index] = buffer;
	offsets[index] = offset;
	if (buffer != nil && index + 1 > *highest) {
		*highest = (unsigned)index + 1;
	}
}

- (void)setVertexBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	[self hakiosBind:buffer offset:offset atIndex:index stage:"vertex" table:_vsBuffers offsets:_vsOffsets highest:&_vsHighest];
}

- (void)setFragmentBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	[self hakiosBind:buffer offset:offset atIndex:index stage:"fragment" table:_fsBuffers offsets:_fsOffsets highest:&_fsHighest];
}

/* Metal's rule: this moves a buffer already bound at the index, so with nothing bound there is nothing to move */
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index < HM_MAX_BUFFERS && _vsBuffers[index] != nil) {
		_vsOffsets[index] = offset;
	}
}

- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index < HM_MAX_BUFFERS && _fsBuffers[index] != nil) {
		_fsOffsets[index] = offset;
	}
}

- (void)setVertexBuffers:(const id *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

- (void)setFragmentBuffers:(const id *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

/* Metal's shorthand for small constant data: a buffer here, because that is what a stage's binding is. The scratch buffer
 * is the encoder's until -endEncoding; hakios-metal keeps it alive past that until the GPU is done with it. */
- (HakiosMetalBuffer *)hakiosScratch:(const void *)bytes length:(NSUInteger)length stage:(const char *)stage index:(NSUInteger)index
{
	HakiosMetalBuffer *buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer == nil) {
		snprintf(_refused, sizeof(_refused), "no scratch buffer for %lu %s bytes at index %lu", (unsigned long)length, stage, (unsigned long)index);
		return nil;
	}
	if (_scratch == nil) {
		_scratch = [[NSMutableArray alloc] init];
	}
	[_scratch addObject:buffer];
	[buffer release];
	return buffer;
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	HakiosMetalBuffer *buffer = [self hakiosScratch:bytes length:length stage:"vertex" index:index];

	if (buffer != nil) {
		[self setVertexBuffer:buffer offset:0 atIndex:index];
	}
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	HakiosMetalBuffer *buffer = [self hakiosScratch:bytes length:length stage:"fragment" index:index];

	if (buffer != nil) {
		[self setFragmentBuffer:buffer offset:0 atIndex:index];
	}
}

/* --- textures and samplers in a render stage: the fragment stage's go to the draw calls (SB6_FS_TEX, the prefetch), the
 * vertex stage's to hakios_metal_cb_set_vertex_textures before each draw (SB6_VS_TEX); one path for both --- */
- (void)hakiosSetTexture:(id)texture atIndex:(NSUInteger)index vertex:(BOOL)vertex
{
	const char *stage = vertex ? "vertex" : "fragment";
	if (index >= HM_MAX_TEXTURES) {
		hm_print("refused: %s texture index %lu is past Metal's table of %d\n", stage, (unsigned long)index, HM_MAX_TEXTURES);
		snprintf(_refused, sizeof(_refused), "a %s texture index past %d", stage, HM_MAX_TEXTURES);
		return;
	}
	if (texture != nil && ![texture isKindOfClass:[HakiosMetalTexture class]]) {
		hm_print("refused: the %s texture at index %lu is a %s, not a texture of this device\n", stage, (unsigned long)index,
			 class_getName(object_getClass(texture)));
		snprintf(_refused, sizeof(_refused), "the %s texture at index %lu is not this device's", stage, (unsigned long)index);
		return;
	}
	HakiosMetalTexture **table = vertex ? _vsTextures : _fsTextures;
	unsigned *highest = vertex ? &_vsHighestTexture : &_fsHighestTexture;
	[texture retain];
	[table[index] release];
	table[index] = texture;
	[(HakiosMetalTexture *)texture hakiosRefreshShadow];
	if (texture != nil && index + 1 > *highest) {
		*highest = (unsigned)index + 1;
	}
}
- (void)hakiosSetSampler:(id)sampler atIndex:(NSUInteger)index vertex:(BOOL)vertex
{
	const char *stage = vertex ? "vertex" : "fragment";
	if (index >= HM_MAX_SAMPLERS) {
		hm_print("refused: %s sampler index %lu is past Metal's table of %d\n", stage, (unsigned long)index, HM_MAX_SAMPLERS);
		snprintf(_refused, sizeof(_refused), "a %s sampler index past %d", stage, HM_MAX_SAMPLERS);
		return;
	}
	if (sampler != nil && ![sampler isKindOfClass:[HakiosMetalSamplerState class]]) {
		hm_print("refused: the %s sampler at index %lu is a %s, not a sampler state of this device\n", stage, (unsigned long)index,
			 class_getName(object_getClass(sampler)));
		snprintf(_refused, sizeof(_refused), "the %s sampler at index %lu is not this device's", stage, (unsigned long)index);
		return;
	}
	HakiosMetalSamplerState **table = vertex ? _vsSamplers : _fsSamplers;
	unsigned *highest = vertex ? &_vsHighestSampler : &_fsHighestSampler;
	[sampler retain];
	[table[index] release];
	table[index] = sampler;
	if (sampler != nil && index + 1 > *highest) {
		*highest = (unsigned)index + 1;
	}
}
- (void)setVertexTexture:(id)texture atIndex:(NSUInteger)index           { [self hakiosSetTexture:texture atIndex:index vertex:YES]; }
- (void)setFragmentTexture:(id)texture atIndex:(NSUInteger)index         { [self hakiosSetTexture:texture atIndex:index vertex:NO]; }
- (void)setVertexSamplerState:(id)sampler atIndex:(NSUInteger)index      { [self hakiosSetSampler:sampler atIndex:index vertex:YES]; }
- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index    { [self hakiosSetSampler:sampler atIndex:index vertex:NO]; }

- (void)setVertexSamplerState:(id)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index
{
	(void)lodMinClamp; (void)lodMaxClamp;
	[self setVertexSamplerState:sampler atIndex:index];
}

- (void)setFragmentSamplerState:(id)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index
{
	(void)lodMinClamp; (void)lodMaxClamp;
	[self setFragmentSamplerState:sampler atIndex:index];
}

- (void)setVertexTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setFragmentTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setVertexSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexSamplerState:samplers[i] atIndex:range.location + i];
	}
}

- (void)setFragmentSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] atIndex:range.location + i];
	}
}

- (void)setVertexSamplerStates:(const id *)samplers lodMinClamps:(const float *)lodMinClamps lodMaxClamps:(const float *)lodMaxClamps withRange:(NSRange)range
{
	(void)lodMinClamps; (void)lodMaxClamps;
	[self setVertexSamplerStates:samplers withRange:range];
}

- (void)setFragmentSamplerStates:(const id *)samplers lodMinClamps:(const float *)lodMinClamps lodMaxClamps:(const float *)lodMaxClamps withRange:(NSRange)range
{
	(void)lodMinClamps; (void)lodMaxClamps;
	[self setFragmentSamplerStates:samplers withRange:range];
}

/* The SPI that binds a texture and its sampler in one message (QuartzCore's image passes send it): the two public
 * setters, each with its own index, as the selector's words say */
- (void)setFragmentTexture:(id)texture atTextureIndex:(NSUInteger)textureIndex samplerState:(id)sampler atSamplerIndex:(NSUInteger)samplerIndex
{
	[self setFragmentTexture:texture atIndex:textureIndex];
	[self setFragmentSamplerState:sampler atIndex:samplerIndex];
}

- (void)setVertexTexture:(id)texture atTextureIndex:(NSUInteger)textureIndex samplerState:(id)sampler atSamplerIndex:(NSUInteger)samplerIndex
{
	[self setVertexTexture:texture atIndex:textureIndex];
	[self setVertexSamplerState:sampler atIndex:samplerIndex];
}

/* --- rasterizer state: what step one fixes, said once when asked to change --- */

/* Culling and the front face: the pass struct's CULL_MODE and WINDING, which every draw after reads (hakios-metal's
 * GRAS_SU_CNTL; which winding FRONT_CW is was measured on the phone, probe 9cu) */
- (void)setCullMode:(MTLCullMode)cullMode
{
	_pass.cull_mode = (unsigned)cullMode;
}

- (void)setFrontFacingWinding:(MTLWinding)winding
{
	_frontFacingWinding = winding;
	_pass.winding = (unsigned)winding;
}

- (void)setTriangleFillMode:(MTLTriangleFillMode)fillMode
{
	if (fillMode != MTLTriangleFillModeFill) {
		hm_print("refused: triangle fill mode %lu (1 lines); VPC/PC_POLYGON_MODE are fixed at triangles in step one; the next draw is failed\n",
			 (unsigned long)fillMode);
		snprintf(_refused, sizeof(_refused), "triangle fill mode %lu is not served", (unsigned long)fillMode);
	}
}

/* Clip is Metal's default and what GRAS_CL_CNTL has (near and far clip enabled); clamping is a bit nothing sets yet */
- (void)setDepthClipMode:(MTLDepthClipMode)mode
{
	if (mode != MTLDepthClipModeClip) {
		HM_ONCE("depth clip mode %lu (clamp) was set; step one clips, and without a depth attachment the difference is not visible\n",
			(unsigned long)mode);
	}
}

/* The depth test each later draw runs (nil: Metal's default, no test). A foreign object fails the next draw by name;
 * a test with no depth attachment is hakios-metal's refusal at the draw. */
- (void)setDepthStencilState:(id)state
{
	if (state == nil) {
		_pass.depth_state_set = 0;
		_pass.stencil_state_set = 0;
		return;
	}
	if (![state isKindOfClass:[HakiosMetalDepthStencilState class]]) {
		hm_print("refused: a depth/stencil state that is a %s, not one of this device's; the next draw is failed\n", class_getName(object_getClass(state)));
		snprintf(_refused, sizeof(_refused), "a depth/stencil state that is not this device's was bound");
		return;
	}
	_pass.depth_state_set = 1;
	_pass.depth_compare = [(HakiosMetalDepthStencilState *)state hakiosDepthCompare];
	_pass.depth_write = [(HakiosMetalDepthStencilState *)state hakiosDepthWrite] ? 1 : 0;
	/* Metal ignores the stencil half on a pass with no stencil attachment; so does this, rather than fail the draw */
	_pass.stencil_state_set = [(HakiosMetalDepthStencilState *)state hakiosStencil] && _pass.stencil != NULL;
	_pass.stencil_front = *[(HakiosMetalDepthStencilState *)state hakiosStencilFront];
	_pass.stencil_back = *[(HakiosMetalDepthStencilState *)state hakiosStencilBack];
}

/* the blend colour is the encoder's state each later draw reads (the BlendColor / BlendAlpha factors) */
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha
{
	_pass.blend_color[0] = red;
	_pass.blend_color[1] = green;
	_pass.blend_color[2] = blue;
	_pass.blend_color[3] = alpha;
}

/* inert given the fixed state, each said once: there is no depth and no stencil, so a depth bias or a stencil reference
 * changes nothing a draw here could show */

- (void)setDepthBias:(float)depthBias slopeScale:(float)slopeScale clamp:(float)clamp
{
	if (depthBias != 0.0f || slopeScale != 0.0f || clamp != 0.0f) {
		HM_ONCE("a depth bias (%g, slope %g, clamp %g) was set; there is no depth attachment, so it is not applied\n",
			(double)depthBias, (double)slopeScale, (double)clamp);
	}
}

/* the references each later draw compares against (Metal's are 32 bits; the plane is 8, and the test reads the low 8) */
- (void)setStencilReferenceValue:(uint32_t)value
{
	_pass.stencil_front_reference = value & 0xff;
	_pass.stencil_back_reference = value & 0xff;
}

- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	_pass.stencil_front_reference = front & 0xff;
	_pass.stencil_back_reference = back & 0xff;
}

/* the draws after this count into the pass's visibility result buffer at OFFSET (hakios-metal checks the mode, the
 * buffer and the offset at each draw, and refuses by name) */
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
	_pass.visibility_mode = (unsigned)mode;
	_pass.visibility_offset = offset;
}

- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const MTLVertexAmplificationViewMapping *)mappings
{
	(void)mappings;
	if (count > 1) {
		hm_print("refused: vertex amplification count %lu; profile 0 amplifies nothing; the next draw is failed\n", (unsigned long)count);
		snprintf(_refused, sizeof(_refused), "vertex amplification count %lu is not served", (unsigned long)count);
	}
}

/* --- store actions set during the pass (Metal allows them when the descriptor said Unknown) --- */

- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index
{
	BOOL resolves = action == MTLStoreActionMultisampleResolve || action == MTLStoreActionStoreAndMultisampleResolve;
	unsigned store;

	if (index >= 8 || (index == 0 ? _pass.color0 == NULL : _pass.more[index - 1].texture == NULL)) {
		hm_print("refused: a store action for colour attachment %lu, which this pass does not have; the next draw is failed\n", (unsigned long)index);
		snprintf(_refused, sizeof(_refused), "a store action for colour attachment %lu, which the pass does not have", (unsigned long)index);
		return;
	}
	if ((action != MTLStoreActionStore && action != MTLStoreActionDontCare && action != MTLStoreActionUnknown && !resolves) ||
	    (resolves && _resolveTo[index] == nil)) {
		hm_print("refused: colour store action %lu at %lu%s; Store, DontCare and a resolve into the pass's resolveTexture are served; the next "
			 "draw is failed\n", (unsigned long)action, (unsigned long)index, resolves ? " with no resolveTexture" : "");
		snprintf(_refused, sizeof(_refused), "colour store action %lu at %lu is not served", (unsigned long)action, (unsigned long)index);
		return;
	}
	_resolving[index] = resolves;
	store = action == MTLStoreActionMultisampleResolve ? (unsigned)MTLStoreActionDontCare
	      : action == MTLStoreActionStoreAndMultisampleResolve || action == MTLStoreActionUnknown ? (unsigned)MTLStoreActionStore : (unsigned)action;
	if (index == 0) {
		_pass.store_action = store;
	} else {
		_pass.more[index - 1].store_action = store;
	}
}

- (void)setDepthStoreAction:(MTLStoreAction)action
{
	if (_pass.depth == NULL) {
		HM_ONCE("a depth store action (%lu) was set on a pass with no depth attachment; nothing to store\n", (unsigned long)action);
		return;
	}
	_pass.depth_store_action = action == MTLStoreActionUnknown ? (unsigned)MTLStoreActionStore : (unsigned)action;
}

- (void)setStencilStoreAction:(MTLStoreAction)action
{
	if (_pass.stencil == NULL) {
		HM_ONCE("a stencil store action (%lu) was set on a pass with no stencil attachment; nothing to store\n", (unsigned long)action);
		return;
	}
	_pass.stencil_store_action = action == MTLStoreActionUnknown ? (unsigned)MTLStoreActionStore : (unsigned)action;
}

- (void)setColorStoreActionOptions:(MTLStoreActionOptions)options atIndex:(NSUInteger)index
{
	HM_ONCE("colour store action options %#lx at index %lu; one sample here, no custom sample positions to keep\n", (unsigned long)options, (unsigned long)index);
}

- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)options
{
	HM_ONCE("depth store action options %#lx on a pass with no depth attachment\n", (unsigned long)options);
}

- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)options
{
	HM_ONCE("stencil store action options %#lx on a pass with no stencil attachment\n", (unsigned long)options);
}

/* --- the draws --- */

/*
 * One whole pass of hakios-metal's per call (begin, the state, the draw, the flushes that put the colour in memory): the
 * C interface's shape, chosen over a pass object because the private tree measured 41 us of fixed cost per draw and
 * nothing here yet draws more than once per pass. In sysmem mode Load and DontCare are the same thing (the RB writes the
 * memory as it draws, there is no tile to load into). A Clear belongs to the pass, not to each draw: the first draw
 * carries it (hakios-metal fills the attachment with the 2D engine before that draw), and once it is recorded the
 * stored action becomes Load, so a second draw does not wipe the first; an encoder that ends with the Clear unspent
 * records it alone at -endEncoding. hakios-metal judges the rest -- the program against the attachment's format, the primitive,
 * a zero vertex count, an instance count other than 1, a viewport or scissor outside the target, the buffer tables (step
 * one: empty) -- and refuses by name; the command buffer then ends in error with that sentence, printed here as well.
 */
- (void)hakiosDraw:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
{
	[self hakiosDraw:primitive vertexStart:start vertexCount:count instanceCount:instances indexBuffer:nil indexType:MTLIndexTypeUInt16
	     indexOffset:0 baseVertex:0 baseInstance:0];
}

/* the fragment constants each program was linked with, for the trace's lines (metal_trace, metal_skip) */
static NSMutableDictionary *hm_program_constants;

static void
hm_note_constants(const void *program, NSString *constants)
{
	@synchronized ([NSMutableDictionary class]) {
		if (hm_program_constants == nil) hm_program_constants = [[NSMutableDictionary alloc] init];
		[hm_program_constants setObject:constants forKey:[NSValue valueWithPointer:program]];
	}
}

static NSString *
hm_constants_of(const void *program)
{
	@synchronized ([NSMutableDictionary class]) {
		NSString *c = [hm_program_constants objectForKey:[NSValue valueWithPointer:program]];
		return c != nil ? c : @"";
	}
}

/* boot-arg metal_trace=1: each kind of draw said once -- its program, its target and the textures it samples -- so that
 * a wrong picture can be traced to the pass that made it. Off by default: a boot draws thousands. */
static BOOL
hm_tracing(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_trace=1") != NULL;
	}
	return on == 1;
}

/* boot-arg metal_skip=WORD (a diagnostic, beside metal_trace): a draw whose trace line holds WORD, its spaces written
 * as '_', is not drawn -- to find which kind of draw makes a part of the picture by taking it away */
static const char *
hm_skip_word(void)
{
	static char word[128];
	static int read;

	if (!read) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		read = 1;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_skip=")) != NULL) {
			at += strlen("metal_skip=");
			for (size_t i = 0; i + 1 < sizeof(word) && at[i] != '\0' && at[i] != ' '; i++) {
				word[i] = at[i] == '_' ? ' ' : at[i];
			}
		}
	}
	return word[0] != '\0' ? word : NULL;
}

/* boot-arg metal_dump=cfg:NAME (a diagnostic): for draws whose program's name holds NAME, the first 128 bytes of each
 * fragment buffer, each content once (40 at most). An uber shader's loops run as many times as its configuration's bytes
 * say, so what one of its draws costs is in this buffer, not in its code */
static void
hm_dump_config(const char *program, hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count)
{
	static char name[64];
	static int state = -1;
	static NSMutableSet *seen;

	if (state < 0) {
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;
		const char *at;

		state = 0;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_dump=cfg:")) != NULL) {
			at += strlen("metal_dump=cfg:");
			for (size_t i = 0; i + 1 < sizeof(name) && at[i] != '\0' && at[i] != ' '; i++) name[i] = at[i];
			state = name[0] != '\0';
			seen = [[NSMutableSet alloc] init];
		}
	}
	if (state != 1 || program == NULL || strstr(program, name) == NULL || [seen count] >= 40) return;
	for (unsigned i = 0; i < count; i++) {
		if (buffers[i] == NULL) continue;
		const uint8_t *bytes = (const uint8_t *)hakios_metal_buffer_contents(buffers[i]);
		const uint64_t length = hakios_metal_buffer_length(buffers[i]);
		if (bytes == NULL || offsets[i] >= length) continue;
		const uint64_t n = length - offsets[i] < 128 ? length - offsets[i] : 128;
		char hex[2 * 128 + 1];
		for (uint64_t k = 0; k < n; k++) snprintf(hex + 2 * k, 3, "%02x", bytes[offsets[i] + k]);
		NSString *key = [NSString stringWithFormat:@"%u:%s", i, hex];
		if ([seen containsObject:key]) continue;
		[seen addObject:key];
		hm_print("cfg: %s fragment buffer %u (%llu bytes from %llu): %s\n", program, i, (unsigned long long)(length - offsets[i]),
			 (unsigned long long)offsets[i], hex);
	}
}

/* YES when the draw is to be left out (metal_skip); prints the kind once when tracing */
static BOOL
hm_trace_draw(const void *which, const char *program, const struct hakios_metal_render_pass *pass, hakios_metal_texture *const *textures, unsigned count,
	      unsigned primitive, unsigned blend)
{
	static NSMutableSet *seen;
	static unsigned printed;
	NSMutableString *line;
	const char *skip = hm_skip_word();
	BOOL skipped;

	if (!hm_tracing() && skip == NULL) {
		return NO;
	}
	line = [NSMutableString stringWithFormat:@"%s (%p fc%@) prim %u into %ux%u fmt %u load %u%s%s", program, which, hm_constants_of(which), primitive,
		pass->color0 != NULL ? hakios_metal_texture_width(pass->color0) : 0, pass->color0 != NULL ? hakios_metal_texture_height(pass->color0) : 0,
		pass->color0 != NULL ? hakios_metal_texture_format(pass->color0) : 0, pass->load_action,
		pass->color0 != NULL && hakios_metal_texture_iosurface(pass->color0) != 0 ? " (surface)" : "", blend ? " blend" : ""];
	for (unsigned i = 0; i < count; i++) {
		if (textures[i] == NULL) continue;
		[line appendFormat:@" t%u=%ux%u fmt %u%s", i, hakios_metal_texture_width(textures[i]), hakios_metal_texture_height(textures[i]),
			hakios_metal_texture_format(textures[i]), hakios_metal_texture_iosurface(textures[i]) != 0 ? " (surface)" : ""];
	}
	skipped = skip != NULL && strstr([line UTF8String], skip) != NULL;
	if (!hm_tracing() || printed >= 3000) {
		return skipped;
	}
	@synchronized ([NSMutableSet class]) {
		if (seen == nil) seen = [[NSMutableSet alloc] init];
		if ([seen containsObject:line]) return skipped;
		[seen addObject:line];
		printed++;
	}
	hm_print("trace: %s%s\n", [line UTF8String], skipped ? " (skipped)" : "");
	return skipped;
}

/* the draws in one: INDEX_BUFFER non-nil is an indexed draw (COUNT its indices, START unread); timed for metal_fps */
- (void)hakiosDraw:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
       indexBuffer:(id)indexBuffer indexType:(MTLIndexType)indexType indexOffset:(NSUInteger)indexOffset baseVertex:(NSInteger)baseVertex
      baseInstance:(NSUInteger)baseInstance
{
	const double began = hm_fps_on() ? hm_now() : 0;
	uint64_t tpidr = 0;

	if (began != 0) __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(tpidr));   /* (the processor number: set_thread_registers, cswitch.s) */
	if (began != 0 && hm_sampling()) {
		static dispatch_once_t once;
		dispatch_once(&once, ^{
			pthread_t th;
			pthread_create(&th, NULL, hm_sampler, NULL);
		});
		hm_sample_thread = mach_thread_self();
	}
	[self hakiosDrawOnce:primitive vertexStart:start vertexCount:count instanceCount:instances indexBuffer:indexBuffer indexType:indexType
		 indexOffset:indexOffset baseVertex:baseVertex baseInstance:baseInstance];
	if (began != 0 && hm_sampling()) {
		const mach_port_t me = hm_sample_thread;
		hm_sample_thread = 0;
		mach_port_deallocate(mach_task_self(), me);
	}
	if (began != 0) hm_fps_drew(hm_now() - began, (unsigned)(tpidr & 0xfff));
	if (began != 0) hm_fps.lastTpidr = tpidr;
}

- (void)hakiosDrawOnce:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
	   indexBuffer:(id)indexBuffer indexType:(MTLIndexType)indexType indexOffset:(NSUInteger)indexOffset baseVertex:(NSInteger)baseVertex
	  baseInstance:(NSUInteger)baseInstance
{
	hakios_metal_cb *cb = [self hakiosCb];
	int drawn;
	hakios_metal_buffer *vs[HM_MAX_BUFFERS], *fs[HM_MAX_BUFFERS];
	/* an indirect draw's buffer, taken here so that no return below leaves it for the next draw */
	id indirectArgs = _indirectArgs;
	NSUInteger indirectOffset = _indirectOffset;
	_indirectArgs = nil;
	hakios_metal_texture *fsTex[HM_MAX_TEXTURES], *vsTex[HM_MAX_TEXTURES];
	hakios_metal_sampler *fsSamp[HM_MAX_SAMPLERS], *vsSamp[HM_MAX_SAMPLERS];

	if (_pipeline == nil) {
		hm_print("refused: a draw with no render pipeline state; the command buffer is failed\n");
		hakios_metal_cb_refuse(cb, "a draw with no render pipeline state");
		return;
	}
	if (_refused[0] != '\0') {
		hm_print("refused: this draw, because %s; the command buffer is failed\n", _refused);
		hakios_metal_cb_refuse(cb, _refused);
		return;
	}
	for (unsigned i = 0; i < _vsHighest; i++) {
		vs[i] = _vsBuffers[i] != nil ? [_vsBuffers[i] hakiosBuffer] : NULL;
	}
	for (unsigned i = 0; i < _fsHighest; i++) {
		fs[i] = _fsBuffers[i] != nil ? [_fsBuffers[i] hakiosBuffer] : NULL;
	}
	for (unsigned i = 0; i < _fsHighestTexture; i++) {
		fsTex[i] = _fsTextures[i] != nil ? [_fsTextures[i] hakiosTexture] : NULL;
		if (_fsTextures[i] != nil && hm_timeline_on()) {
			unsigned kind = [_fsTextures[i] hakiosStorageKind];
			const unsigned origin = hakios_metal_texture_origin([_fsTextures[i] hakiosTexture]);
			if (origin == 2) kind = 2;   /* (over a buffer's rows: counted where managed was, which iOS has not) */
			if (origin == 3) kind = 5;   /* (compressed) */
			const double bytes = 4.0 * [_fsTextures[i] width] * [_fsTextures[i] height];
			if (kind < 6 && bytes >= 65536) {
				pthread_mutex_lock(&hm_fps.lock);
				hm_fps.sampledBytes[kind] += bytes;
				/* ...and by size and origin, the 16 first kinds seen */
				const unsigned w = (unsigned)[_fsTextures[i] width], h = (unsigned)[_fsTextures[i] height];
				unsigned k = 0;
				while (k < 16 && hm_fps.bySize[k].bytes != 0 && !(hm_fps.bySize[k].w == w && hm_fps.bySize[k].h == h && hm_fps.bySize[k].origin == origin)) k++;
				if (k < 16) {
					hm_fps.bySize[k].w = w, hm_fps.bySize[k].h = h, hm_fps.bySize[k].origin = origin;
					hm_fps.bySize[k].bytes += bytes;
				}
				pthread_mutex_unlock(&hm_fps.lock);
			}
		}
	}
	for (unsigned i = 0; i < _fsHighestSampler; i++) {
		fsSamp[i] = _fsSamplers[i] != nil ? [_fsSamplers[i] hakiosSampler] : NULL;
	}
	for (unsigned i = 0; i < _vsHighestTexture; i++) {
		vsTex[i] = _vsTextures[i] != nil ? [_vsTextures[i] hakiosTexture] : NULL;
	}
	for (unsigned i = 0; i < _vsHighestSampler; i++) {
		vsSamp[i] = _vsSamplers[i] != nil ? [_vsSamplers[i] hakiosSampler] : NULL;
	}
	hakios_metal_cb_set_vertex_textures(cb, vsTex, _vsHighestTexture, vsSamp, _vsHighestSampler);
	hakios_metal_program *program = [_pipeline hakiosProgramFor:fs offsets:_fsOffsets count:_fsHighest];

	hm_dump_config(hakios_metal_program_name(program), fs, _fsOffsets, _fsHighest);
	if (hm_trace_draw([_pipeline hakiosProgram], hakios_metal_program_name([_pipeline hakiosProgram]), &_pass, fsTex, _fsHighestTexture, (unsigned)primitive, 0)) {
		return;
	}
	if (indexBuffer != nil && ![indexBuffer isKindOfClass:[HakiosMetalBuffer class]]) {
		hm_print("refused: an indexed draw from a %s, not a buffer of this device; the command buffer is failed\n", class_getName(object_getClass(indexBuffer)));
		hakios_metal_cb_refuse(cb, "an indexed draw from a buffer that is not this device's");
		return;
	}
	/* a word this encoder has not counted into yet starts from zero: Metal's result is the pass's count */
	if (_pass.visibility_mode != 0) {
		if (_visibilityCounted == nil) _visibilityCounted = [[NSMutableIndexSet alloc] init];
		_pass.visibility_reset = ![_visibilityCounted containsIndex:(NSUInteger)(_pass.visibility_offset / 8)];
	}
	if (indirectArgs != nil) {
		if (![indirectArgs isKindOfClass:[HakiosMetalBuffer class]]) {
			hm_print("refused: an indirect draw whose arguments are in a %s, not a buffer of this device; the command buffer is failed\n",
				 class_getName(object_getClass(indirectArgs)));
			hakios_metal_cb_refuse(cb, "an indirect draw's arguments in a buffer that is not this device's");
			return;
		}
		hakios_metal_cb_set_indirect(cb, [(HakiosMetalBuffer *)indirectArgs hakiosBuffer], (uint64_t)indirectOffset);
	}
	const double coreBegan = hm_fps_on() ? hm_now() : 0;
	if (indexBuffer != nil) {
		drawn = hakios_metal_cb_draw_indexed(cb, program, &_pass, _hasViewport ? &_viewport : NULL, _hasScissor ? &_scissor : NULL,
						     (unsigned)primitive, (unsigned)count, (unsigned)indexType, [(HakiosMetalBuffer *)indexBuffer hakiosBuffer],
						     (uint64_t)indexOffset, (unsigned)instances, (int)baseVertex, (unsigned)baseInstance, vs, _vsOffsets, _vsHighest, fs,
						     _fsOffsets, _fsHighest, fsTex, _fsHighestTexture, fsSamp, _fsHighestSampler);
	} else {
		drawn = hakios_metal_cb_draw3(cb, program, &_pass, _hasViewport ? &_viewport : NULL, _hasScissor ? &_scissor : NULL,
					      (unsigned)primitive, (unsigned)start, (unsigned)count, (unsigned)instances, (unsigned)baseInstance,
					      vs, _vsOffsets, _vsHighest, fs, _fsOffsets, _fsHighest, fsTex, _fsHighestTexture, fsSamp, _fsHighestSampler);
	}
	if (coreBegan != 0) hm_fps_core(hm_now() - coreBegan);
	if (drawn == 0 && _pass.visibility_mode != 0) {
		[_visibilityCounted addIndex:(NSUInteger)(_pass.visibility_offset / 8)];
	}
	if (drawn != 0) {
		hm_print("refused: a draw of %s, primitive %lu, %lu vertices from %lu, %lu instance(s), load %u store %u, %u vertex and %u fragment "
			 "buffer slots: %s\n", hakios_metal_program_name([_pipeline hakiosProgram]), (unsigned long)primitive, (unsigned long)count,
			 (unsigned long)start, (unsigned long)instances, _pass.load_action, _pass.store_action, _vsHighest, _fsHighest, hakios_metal_error());
		hakios_metal_cb_refuse(cb, hakios_metal_error());
		return;
	}
	hm_spend_clears(&_pass);   /* the fills are recorded: the pass's later draws load what they left */
}

- (void)drawPrimitives:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
	[self hakiosDraw:primitive vertexStart:start vertexCount:count instanceCount:1];
}

- (void)drawPrimitives:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
{
	[self hakiosDraw:primitive vertexStart:start vertexCount:count instanceCount:instances];
}

- (void)drawPrimitives:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
	  baseInstance:(NSUInteger)baseInstance
{
	[self hakiosDraw:primitive vertexStart:start vertexCount:count instanceCount:instances indexBuffer:nil indexType:MTLIndexTypeUInt16
	     indexOffset:0 baseVertex:0 baseInstance:baseInstance];
}

/* Indirect, patch and mesh draws: each is refused by name AND fails the command buffer at once, because a draw that
 * vanishes without a word is the worst outcome available (the compute encoder's indirect dispatch rule). Indirect ones
 * need a grid read by the GPU, patches the tessellation stages (HS/DS off here), mesh draws the object/mesh stages. */
- (void)hakiosRefuseDraw:(SEL)sel what:(const char *)what
{
	hm_refusal(self, sel, what);
	hakios_metal_cb_refuse([self hakiosCb], "a draw of a kind that is not served (indexed, indirect, patches or mesh)");
}

/* Indexed draws: hakios-metal's 7-word CP_DRAW_INDX_OFFSET with Metal's primitive restart; it refuses by name what it
 * does not serve (a negative base vertex, an offset that is not a multiple of the index size) */
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitive indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType
		  indexBuffer:(id)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset
{
	if (indexBuffer == nil) {
		hm_print("refused: an indexed draw with no index buffer; the command buffer is failed\n");
		hakios_metal_cb_refuse([self hakiosCb], "an indexed draw with no index buffer");
		return;
	}
	[self hakiosDraw:primitive vertexStart:0 vertexCount:indexCount instanceCount:1 indexBuffer:indexBuffer indexType:indexType
	     indexOffset:indexBufferOffset baseVertex:0 baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitive indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType
		  indexBuffer:(id)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instances
{
	if (indexBuffer == nil) {
		hm_print("refused: an indexed draw with no index buffer; the command buffer is failed\n");
		hakios_metal_cb_refuse([self hakiosCb], "an indexed draw with no index buffer");
		return;
	}
	[self hakiosDraw:primitive vertexStart:0 vertexCount:indexCount instanceCount:instances indexBuffer:indexBuffer indexType:indexType
	     indexOffset:indexBufferOffset baseVertex:0 baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitive indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType
		  indexBuffer:(id)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instances
		   baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance
{
	if (indexBuffer == nil) {
		hm_print("refused: an indexed draw with no index buffer; the command buffer is failed\n");
		hakios_metal_cb_refuse([self hakiosCb], "an indexed draw with no index buffer");
		return;
	}
	[self hakiosDraw:primitive vertexStart:0 vertexCount:indexCount instanceCount:instances indexBuffer:indexBuffer indexType:indexType
	     indexOffset:indexBufferOffset baseVertex:baseVertex baseInstance:baseInstance];
}

/* Indirect draws: the counts, starts and bases are read by the GPU as it runs the draw (CP_DRAW_INDIRECT_MULTI), so
 * the counts handed on here are placeholders the core does not read */
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitive indexType:(MTLIndexType)indexType indexBuffer:(id)indexBuffer
	    indexBufferOffset:(NSUInteger)indexBufferOffset indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
	if (indexBuffer == nil || indirectBuffer == nil) {
		hm_print("refused: an indirect indexed draw with no %s; the command buffer is failed\n", indexBuffer == nil ? "index buffer" : "argument buffer");
		hakios_metal_cb_refuse([self hakiosCb], "an indirect indexed draw with a buffer missing");
		return;
	}
	_indirectArgs = indirectBuffer;
	_indirectOffset = indirectBufferOffset;
	[self hakiosDraw:primitive vertexStart:0 vertexCount:1 instanceCount:1 indexBuffer:indexBuffer indexType:indexType
	     indexOffset:indexBufferOffset baseVertex:0 baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)primitive indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
	if (indirectBuffer == nil) {
		hm_print("refused: an indirect draw with no argument buffer; the command buffer is failed\n");
		hakios_metal_cb_refuse([self hakiosCb], "an indirect draw with no argument buffer");
		return;
	}
	_indirectArgs = indirectBuffer;
	_indirectOffset = indirectBufferOffset;
	[self hakiosDraw:primitive vertexStart:0 vertexCount:1 instanceCount:1 indexBuffer:nil indexType:MTLIndexTypeUInt16
	     indexOffset:0 baseVertex:0 baseInstance:0];
}

- (void)drawPatches:(NSUInteger)patches patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(id)patchIndexBuffer
     patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)baseInstance
{
	(void)patches; (void)patchStart; (void)patchCount; (void)patchIndexBuffer; (void)patchIndexBufferOffset; (void)instances; (void)baseInstance;
	[self hakiosRefuseDraw:_cmd what:"has no tessellation stages (HS and DS are off); the command buffer is failed"];
}

- (void)drawPatches:(NSUInteger)patches patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
     indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
	(void)patches; (void)patchIndexBuffer; (void)patchIndexBufferOffset; (void)indirectBuffer; (void)indirectBufferOffset;
	[self hakiosRefuseDraw:_cmd what:"has no tessellation stages (HS and DS are off); the command buffer is failed"];
}

- (void)drawIndexedPatches:(NSUInteger)patches patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(id)patchIndexBuffer
    patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id)controlPointIndexBuffer
 controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)baseInstance
{
	(void)patches; (void)patchStart; (void)patchCount; (void)patchIndexBuffer; (void)patchIndexBufferOffset; (void)controlPointIndexBuffer;
	(void)controlPointIndexBufferOffset; (void)instances; (void)baseInstance;
	[self hakiosRefuseDraw:_cmd what:"has no tessellation stages (HS and DS are off); the command buffer is failed"];
}

- (void)drawIndexedPatches:(NSUInteger)patches patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
   controlPointIndexBuffer:(id)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset
	    indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
	(void)patches; (void)patchIndexBuffer; (void)patchIndexBufferOffset; (void)controlPointIndexBuffer; (void)controlPointIndexBufferOffset;
	(void)indirectBuffer; (void)indirectBufferOffset;
	[self hakiosRefuseDraw:_cmd what:"has no tessellation stages (HS and DS are off); the command buffer is failed"];
}

/* the mesh draws are REAL in _MTLCommandEncoder (they re-send themselves), so they are overridden rather than left to
 * reach code written for Apple's mesh encoder */
- (void)drawMeshThreadgroups:(MTLSize)groups threadsPerObjectThreadgroup:(MTLSize)objectThreads threadsPerMeshThreadgroup:(MTLSize)meshThreads
{
	(void)groups; (void)objectThreads; (void)meshThreads;
	[self hakiosRefuseDraw:_cmd what:"has no mesh or object stages; the command buffer is failed"];
}

- (void)drawMeshThreads:(MTLSize)threads threadsPerObjectThreadgroup:(MTLSize)objectThreads threadsPerMeshThreadgroup:(MTLSize)meshThreads
{
	(void)threads; (void)objectThreads; (void)meshThreads;
	[self hakiosRefuseDraw:_cmd what:"has no mesh or object stages; the command buffer is failed"];
}

- (void)drawMeshThreadgroupsWithIndirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset
		   threadsPerObjectThreadgroup:(MTLSize)objectThreads threadsPerMeshThreadgroup:(MTLSize)meshThreads
{
	(void)indirectBuffer; (void)indirectBufferOffset; (void)objectThreads; (void)meshThreads;
	[self hakiosRefuseDraw:_cmd what:"has no mesh or object stages; the command buffer is failed"];
}

- (void)executeCommandsInBuffer:(id)icb withRange:(NSRange)range
{
	(void)icb; (void)range;
	[self hakiosRefuseDraw:_cmd what:"has no indirect command buffers (the GPU reads no draw arguments here); the command buffer is failed"];
}

- (void)executeCommandsInBuffer:(id)icb indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
	(void)icb; (void)indirectBuffer; (void)indirectBufferOffset;
	[self hakiosRefuseDraw:_cmd what:"has no indirect command buffers (the GPU reads no draw arguments here); the command buffer is failed"];
}

/*
 * Served by construction, as in the compute encoder: each draw here is a whole pass whose tail flushes the CCU and the
 * caches (PC_CCU_FLUSH_COLOR_TS, CACHE_FLUSH_TS) and whose head invalidates them, one queue runs its command buffers
 * in commit order, and every allocation is resident for its whole life. A barrier, a fence or a residency request asks
 * for an ordering that is already the only one there is.
 */
- (void)updateFence:(id)fence afterStages:(MTLRenderStages)stages  { (void)fence; (void)stages; }
- (void)waitForFence:(id)fence beforeStages:(MTLRenderStages)stages { (void)fence; (void)stages; }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before
{
	(void)scope; (void)after; (void)before;
}
- (void)memoryBarrierWithResources:(const id *)resources count:(NSUInteger)count afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before
{
	(void)resources; (void)count; (void)after; (void)before;
}
- (void)useResource:(id)resource usage:(MTLResourceUsage)usage { (void)resource; (void)usage; }
- (void)useResource:(id)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages { (void)resource; (void)usage; (void)stages; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(MTLResourceUsage)usage { (void)resources; (void)count; (void)usage; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
{
	(void)resources; (void)count; (void)usage; (void)stages;
}
- (void)useHeap:(id)heap { (void)heap; }
- (void)useHeap:(id)heap stages:(MTLRenderStages)stages { (void)heap; (void)stages; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count { (void)heaps; (void)count; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count stages:(MTLRenderStages)stages { (void)heaps; (void)count; (void)stages; }

/* abstract in _MTLCommandEncoder (inheriting it aborts); a packing hint the compiler decides for itself here */
- (void)setThreadgroupPackingDisabled:(BOOL)disabled { (void)disabled; }

/* The tile an Apple GPU's imageblock covers, which the protocol lets a client read at any time. This GPU draws straight
 * into memory and has no tile here; the honest number is 0, and a READER is not a binding -- left to the catch-all it
 * would fail the next draw, which only a dropped input deserves -- so both are answered here, with a line once. */
- (NSUInteger)tileWidth
{
	HM_ONCE("a render encoder's tile width or height was asked for; this GPU draws in sysmem with no tile memory, and 0 is answered\n");
	return 0;
}

- (NSUInteger)tileHeight
{
	HM_ONCE("a render encoder's tile width or height was asked for; this GPU draws in sysmem with no tile memory, and 0 is answered\n");
	return 0;
}

/* tile shading (setTileBytes:, dispatchThreadsPerTile:, setThreadgroupMemoryLength:offset:atIndex:), function tables,
 * tessellation factor buffers, counter sampling and the object/mesh setters are declared by <MTLRenderCommandEncoder>
 * and refused through -forwardInvocation: (or Metal's own abstract bodies) above, by name */

- (void)endEncoding
{
	/* a Clear no draw spent: the pass still owes it (a pass that only clears is how many clients wipe a drawable) */
	/* (not on a command buffer a refused draw already failed -- it holds an error before commit: its first sentence is the one that matters) */
	if (hm_owes_clear(&_pass) && hakios_metal_cb_error([self hakiosCb])[0] == '\0' &&
	    hakios_metal_cb_clear([self hakiosCb], &_pass) != 0) {
		hm_print("refused: a pass that only clears: %s\n", hakios_metal_error());
		hakios_metal_cb_refuse([self hakiosCb], hakios_metal_error());
	}
	hm_spend_clears(&_pass);
	/* the resolves, after the pass: each multisample attachment's samples into its resolveTexture, pixel for pixel (the
	 * blit closes the pass first, so it reads what the draws wrote) */
	for (unsigned i = 0; i < 8; i++) {
		if (!_resolving[i] || hakios_metal_cb_error([self hakiosCb])[0] != '\0') {
			continue;
		}
		if (hakios_metal_cb_blit([self hakiosCb], [_resolveFrom[i] hakiosTexture], 0, 0, 0, (unsigned)[_resolveFrom[i] width],
					 (unsigned)[_resolveFrom[i] height], [_resolveTo[i] hakiosTexture], _resolveLevel[i], 0, 0,
					 (unsigned)[_resolveFrom[i] width], (unsigned)[_resolveFrom[i] height], 0) != 0) {
			hm_print("refused: the resolve of colour attachment %u: %s\n", i, hakios_metal_error());
			hakios_metal_cb_refuse([self hakiosCb], hakios_metal_error());
		}
	}
	/* the bindings were handed to hakios-metal at each draw, which holds them until the GPU is done */
	[_scratch release];
	_scratch = nil;
	[super endEncoding];   /* MUST: the base class retains the command buffer and this is what releases it */
}

@end

@implementation HakiosMetalParallelRenderCommandEncoder

+ (void)load
{
	hm_adopt(self, @protocol(MTLParallelRenderCommandEncoder), "MTLParallelRenderCommandEncoder");
}

HM_REFUSES_OUT_LOUD

- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer pass:(const struct hakios_metal_render_pass *)pass
			       colorTexture:(HakiosMetalTexture *)texture descriptor:(MTLRenderPassDescriptor *)descriptor
{
	self = [super initWithCommandBuffer:commandBuffer];   /* retains the buffer; -endEncoding gives it back */
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	_pass = *pass;
	_colorTexture = [texture retain];
	_descriptor = [descriptor copy];
	_subs = [[NSMutableArray alloc] init];
	_lock = [[NSLock alloc] init];
	return self;
}

- (void)dealloc
{
	for (NSValue *v in _subs) hakios_metal_cb_free((hakios_metal_cb *)[v pointerValue]);
	[_subs release];
	[_lock release];
	[_descriptor release];
	[_colorTexture release];
	[super dealloc];
}

- (id)device { return [_commandBuffer device]; }

/* a sub-encoder: the pass as the descriptor gave it for the first, loading what came before for the rest; storing
 * always, since the next one (or the resolve) reads it */
- (id)renderCommandEncoder
{
	struct hakios_metal_render_pass pass = _pass;
	hakios_metal_cb *sub = hakios_metal_cb_new([(HakiosMetalCommandQueue *)[_commandBuffer commandQueue] hakiosQueue]);
	if (sub != NULL) {
		hakios_metal_cb_skip_refused(sub, !hm_strict());
	}

	[_lock lock];
	const BOOL first = [_subs count] == 0;
	[_subs addObject:[NSValue valueWithPointer:sub]];
	[_lock unlock];
	if (!first) {
		pass.load_action = MTLLoadActionLoad;
		pass.depth_load_action = MTLLoadActionLoad;
		pass.stencil_load_action = MTLLoadActionLoad;
		for (unsigned i = 0; i < 7; i++) pass.more[i].load_action = MTLLoadActionLoad;
	}
	pass.store_action = MTLStoreActionStore;
	pass.depth_store_action = MTLStoreActionStore;
	pass.stencil_store_action = MTLStoreActionStore;
	for (unsigned i = 0; i < 7; i++) pass.more[i].store_action = MTLStoreActionStore;
	return [[[HakiosMetalRenderCommandEncoder alloc] initWithHakiosCommandBuffer:_commandBuffer pass:&pass colorTexture:_colorTexture
										  sub:sub] autorelease];
}

/* every sub-encoder stores (above); a resolve asked for here is read from the descriptor's store action at the end */
- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index
{
	if (index < 8) [[[_descriptor colorAttachments] objectAtIndexedSubscript:index] setStoreAction:action];
}
- (void)setColorStoreActionOptions:(MTLStoreActionOptions)options atIndex:(NSUInteger)index { (void)options; (void)index; }
- (void)setDepthStoreAction:(MTLStoreAction)action { (void)action; }
- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)options { (void)options; }
- (void)setStencilStoreAction:(MTLStoreAction)action { (void)action; }
- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)options { (void)options; }

- (void)endEncoding
{
	hakios_metal_cb *cb = [_commandBuffer hakiosCommandBuffer];

	[_lock lock];
	for (NSValue *v in _subs) {
		hakios_metal_cb *sub = (hakios_metal_cb *)[v pointerValue];

		if (hakios_metal_cb_append(cb, sub) != 0) {
			hm_print("refused: a parallel render pass's sub-encoder: %s\n", hakios_metal_error());
		}
		hakios_metal_cb_free(sub);
	}
	const BOOL none = [_subs count] == 0;
	[_subs removeAllObjects];
	[_lock unlock];
	/* no sub-encoder at all: the pass's own Clear is still owed */
	if (none && hm_owes_clear(&_pass) && hakios_metal_cb_error(cb)[0] == '\0' && hakios_metal_cb_clear(cb, &_pass) != 0) {
		hm_print("refused: a parallel pass that only clears: %s\n", hakios_metal_error());
		hakios_metal_cb_refuse(cb, hakios_metal_error());
	}
	/* the resolves, once, over everything the sub-encoders drew */
	for (NSUInteger i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *c = [[_descriptor colorAttachments] objectAtIndexedSubscript:i];
		const MTLStoreAction a = [c storeAction];
		HakiosMetalTexture *from = (HakiosMetalTexture *)[c texture], *to = (HakiosMetalTexture *)[c resolveTexture];

		if (from == nil || to == nil || hakios_metal_cb_error(cb)[0] != '\0' ||
		    !(a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve || a == MTLStoreActionUnknown)) {
			continue;
		}
		if (hakios_metal_cb_blit(cb, [from hakiosTexture], 0, 0, 0, (unsigned)[from width], (unsigned)[from height], [to hakiosTexture],
					 (unsigned)[c resolveLevel], 0, 0, (unsigned)[from width], (unsigned)[from height], 0) != 0) {
			hm_print("refused: the resolve of colour attachment %lu: %s\n", (unsigned long)i, hakios_metal_error());
			hakios_metal_cb_refuse(cb, hakios_metal_error());
		}
	}
	[super endEncoding];   /* MUST: the base class retains the command buffer and this is what releases it */
}

@end
