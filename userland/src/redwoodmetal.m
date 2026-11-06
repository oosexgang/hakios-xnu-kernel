/*
 * The device object of a Metal driver for this port, as a real library.
 *
 * What Metal demands of a plugin's device class was measured rather than guessed
 * (docs/metal.md section 10): `MTLAddDevice` (0x181a1875c) wants an object that comes back
 * non-nil from -initWithAcceleratorPort:, conforms to MTLDevice and MTLDeviceSPI -- both
 * asserted, both fatal -- and answers -featureProfile, -maxBufferLength and
 * -supportsVertexAmplificationCount:. Four methods. Everything else in the registration
 * path, including the 64-field limits table and the 42 feature queries, _MTLDevice fills
 * in itself once -featureProfile answers.
 *
 * Three decisions in how this is written, each for a measured reason:
 *
 *   - The class is declared in SOURCE, not built with objc_allocateClassPair as the probes
 *     did. That needs the private class symbol at link time, and it is bindable: IOGPU
 *     itself imports `_OBJC_CLASS_$__MTLDevice` together with its metaclass. Only the name
 *     is declared here -- nothing about Apple's 36 ivars or their offsets, which belong to
 *     Metal 306.6.4 and are nowhere written down. Our own ivars are placed by the runtime
 *     after Apple's, which is what the non-fragile ABI is for.
 *   - `<MTLDeviceSPI>` is declared at RUNTIME, with class_addProtocol. The protocol is
 *     private and requires 337 instance methods; writing that declaration out would mean
 *     copying 337 signatures we do not implement, and the only thing the two assertions in
 *     MTLAddDevice read is conformsToProtocol:. One call says the same thing truthfully.
 *     MTLDeviceSPI adopts MTLDevice, so one declaration satisfies both.
 *   - -initWithAcceleratorPort: opens this port's OWN gpu service, `RedwoodGPU`, the way
 *     Mesa's shim does (tools/mesa/redwood-drm/redwood_drm.c: IOServiceOpen with type 0,
 *     then msm-shaped requests through IOConnectCallStructMethod). That is the road the
 *     resource layer will take, and it already carries Mesa's own dispatches today. The
 *     IOGPU road was measured shut: IOServiceOpen on our IOAcceleratorES service with
 *     Apple's type 1 answers kIOReturnUnsupported.
 *
 * The first resource is here too. A Metal buffer on this port is a GEM allocation of the
 * kernel's own gpu driver, made and mapped exactly the way Mesa's shim makes and maps one
 * (tools/mesa/redwood-drm/redwood_drm.c): kMsmGemNew (0x42) for the memory, MSM_INFO_GET_IOVA
 * through kMsmGemInfo (0x43) for the address the GPU sees, IOConnectMapMemory64 for the
 * address the processor sees, kDrmGemClose (0x09) to give it back. The kernel makes that
 * memory write-combined, with no cache of this processor between the program and what the
 * GPU reads, which is also why -didModifyRange: has nothing to do.
 *
 * Metal.framework has no `_MTLBuffer` to inherit -- only `_MTLResource`, a thin base over
 * `_MTLObjectWithLabel` -- so the buffer class is ours, as it is in Apple's own drivers
 * (`IOGPUMetalBuffer`, `AGXA11FamilyBuffer`).
 *
 * The compute path is here too, and it is where subsystem (1) finally carries something.
 * Metal hands a driver one function's LLVM module through -[_MTLFunction bitcodeData], and
 * those bytes were checked against our own AIR reader over all eleven functions of
 * AV1SW.metallib -- same lengths, same checksums (docs/metal.md 10.12). The front end that
 * reads them lives in Mesa, so the compiling and the dispatching are asked of
 * /redwood/lib/libredwoodair.dylib through the contract in tools/mesa/air/air_metal.h.
 *
 * That library is loaded with dlopen rather than linked, for a measured reason: the device
 * object must exist even when there is no GPU. The port starts the Adreno only with the
 * boot argument gpu=1, and a device that refuses to be created at all would take Metal's
 * whole discovery down with it; a device that answers its name and its limits and refuses
 * to make a pipeline is the truthful object in that case.
 *
 * Ordering is Metal's, not Gallium's: an encoder RECORDS its dispatches and the command
 * buffer runs them when it is committed. Dispatching as they are encoded would be less
 * code and would quietly reorder work an application committed in a particular order.
 *
 * Depth and stencil are here, and what that amounts to is four things: a texture of a depth
 * pixel format, an MTLDepthStencilState over the private _MTLDepthStencilState (whose SPI
 * has four methods Apple leaves answering NO, overridden here with the truth about the
 * descriptor), the render pass descriptor's depth and stencil attachments, and the
 * depth-range half of -setViewport:, which is the only thing in Metal that moves this
 * driver's quad in z at all.
 *
 * ARGUMENT BUFFERS are here, and what they are here is a RECORDER: Metal's encoder writes
 * 8-byte handles into a buffer and those are an Apple GPU's descriptors, so this driver
 * records (resource, Metal id) against (destination buffer, destination offset) instead
 * and the draw binds every member the pipeline's shaders declare. The buffer's contents
 * are never read, and an argument buffer the application did not fill through an encoder
 * is refused BY NAME (docs/air.md 8.2, and the MTLArgumentEncoder implementation at the
 * end of this file).
 *
 * BUFFER OFFSETS are honoured, which they were not: Metal's own `offset:` is recorded at
 * the encoder and judged at the draw, where the pipeline says whether an index is a
 * stream, a set or a buffer argument. A `constant` one is served at any offset -- the
 * compiler library uploads the block when the GPU cannot fetch from that offset -- and a
 * `device` one the GPU cannot fetch from is refused by name, because the shader may write
 * it and a copy would lose the writes.
 *
 * What is NOT here, and is not pretended: heaps, blit encoders, events, multisample
 * resolve, and the rectangle half of a viewport -- the rest of docs/metal.md 10.8.
 *
 * Every line this file writes carries the PROCESS it was written by
 * (`redwoodmetal[progname pid]:`, see rw_print): a driver is a library, four or five
 * Metal clients are alive in one boot of this port, and a refusal nobody can attribute
 * is half a refusal.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>          /* for MTLSize and the public enumerations, nothing private */
#import <objc/message.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <notify.h>
#include <pthread.h>

#include "metal/drawable.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* The contract with Mesa's side is tools/mesa/air/air_metal.h itself, through the core
 * (src/metal/rwm.h): this file used to carry a copy of its declarations. */
#include "metal/rwm.h"

/* where a frame's time goes, and the clock it is measured with: src/metal/perf.cpp */
#define rw_perf rwm_perf
#define rw_ns rwm_ns
#define rw_thread_cpu_ns rwm_thread_cpu_ns
#define rw_perf_report rwm_perf_report
/* the driver's own thread: src/metal/worker.cpp */
#define rw_on_worker rwm_worker_on
#define rw_bind_big rwm_bind_big
#define rw_worker_drain rwm_worker_drain

/*
 * The library's MTLVertexFormat table, checked against Apple's own header HERE, where the
 * SDK is in hand: tools/mesa/air/air_metal.c carries those numbers as literals (it has no
 * Metal headers to read them from), this file is compiled against
 * iPhoneOS16.5.sdk/System/Library/Frameworks/Metal.framework/Headers/MTLVertexDescriptor.h,
 * and a disagreement between the two would be a vertex fetched in the wrong format rather
 * than anything that fails. So it is a build failure of this driver instead.
 *
 * Nothing in this file switches on these names; they are asserted and otherwise the
 * numbers pass through as the application gave them, exactly as MTLPixelFormat does.
 */
_Static_assert((int)MTLVertexFormatInvalid == 0 && (int)MTLVertexFormatUChar2 == 1 &&
		   (int)MTLVertexFormatChar2 == 4 &&
		   (int)MTLVertexFormatUChar2Normalized == 7 &&
		   (int)MTLVertexFormatChar2Normalized == 10,
	       "MTLVertexFormat's 8-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUShort2 == 13 && (int)MTLVertexFormatShort2 == 16 &&
		   (int)MTLVertexFormatUShort2Normalized == 19 &&
		   (int)MTLVertexFormatShort2Normalized == 22 &&
		   (int)MTLVertexFormatHalf2 == 25,
	       "MTLVertexFormat's 16-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatFloat == 28 && (int)MTLVertexFormatFloat4 == 31 &&
		   (int)MTLVertexFormatInt == 32 && (int)MTLVertexFormatUInt == 36 &&
		   (int)MTLVertexFormatInt1010102Normalized == 40 &&
		   (int)MTLVertexFormatUInt1010102Normalized == 41 &&
		   (int)MTLVertexFormatUChar4Normalized_BGRA == 42,
	       "MTLVertexFormat's 32-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUChar == 45 && (int)MTLVertexFormatChar == 46 &&
		   (int)MTLVertexFormatUCharNormalized == 47 &&
		   (int)MTLVertexFormatCharNormalized == 48 &&
		   (int)MTLVertexFormatUShort == 49 && (int)MTLVertexFormatShort == 50 &&
		   (int)MTLVertexFormatUShortNormalized == 51 &&
		   (int)MTLVertexFormatShortNormalized == 52 &&
		   (int)MTLVertexFormatHalf == 53,
	       "MTLVertexFormat's one-component forms are not where air_metal.c's table "
	       "has them");
_Static_assert((int)MTLVertexStepFunctionConstant == 0 &&
		   (int)MTLVertexStepFunctionPerVertex == 1 &&
		   (int)MTLVertexStepFunctionPerInstance == 2 &&
		   (int)MTLVertexStepFunctionPerPatch == 3 &&
		   (int)MTLVertexStepFunctionPerPatchControlPoint == 4,
	       "MTLVertexStepFunction is not what air_metal.c's refusals name");

/*
 * THE BLEND ENUMERATIONS, asserted for the same reason and with more riding on it: the
 * compiler library maps all three BY TABLE, and if Apple's numbering were not what those
 * tables assume the result would be a composite with the wrong arithmetic in it rather
 * than anything that fails. MTLBlendFactor and pipe_blendfactor agree on only two of
 * nineteen values, so there is no cast to fall back on and no accident that would be
 * harmless.
 *
 * This is one of the two independent sources for the numbering, and it is the one that
 * can be made into a BUILD FAILURE: the SDK header compiled against here is
 * iPhoneOS16.5.sdk's own MTLRenderPipeline.h, the release this driver runs on.
 *
 * MTLColorWriteMask is the one to look at twice. Its bits run from Alpha at 1 << 0 up to
 * Red at 1 << 3, which is the OPPOSITE ORDER from Gallium's colormask, where red is 0x1.
 * Four of the sixteen masks are palindromes and survive the confusion -- None, All,
 * green|blue and red|alpha -- so a driver that got this backwards would still pass any
 * test built out of "write everything" and "write nothing".
 */
_Static_assert((int)MTLBlendFactorZero == 0 && (int)MTLBlendFactorOne == 1 &&
		   (int)MTLBlendFactorSourceColor == 2 &&
		   (int)MTLBlendFactorOneMinusSourceColor == 3 &&
		   (int)MTLBlendFactorSourceAlpha == 4 &&
		   (int)MTLBlendFactorOneMinusSourceAlpha == 5 &&
		   (int)MTLBlendFactorDestinationColor == 6 &&
		   (int)MTLBlendFactorOneMinusDestinationColor == 7 &&
		   (int)MTLBlendFactorDestinationAlpha == 8 &&
		   (int)MTLBlendFactorOneMinusDestinationAlpha == 9 &&
		   (int)MTLBlendFactorSourceAlphaSaturated == 10,
	       "MTLBlendFactor's served values are not where air_metal.c's table has them");
_Static_assert((int)MTLBlendFactorBlendColor == 11 &&
		   (int)MTLBlendFactorOneMinusBlendColor == 12 &&
		   (int)MTLBlendFactorBlendAlpha == 13 &&
		   (int)MTLBlendFactorOneMinusBlendAlpha == 14 &&
		   (int)MTLBlendFactorSource1Color == 15 &&
		   (int)MTLBlendFactorOneMinusSource1Color == 16 &&
		   (int)MTLBlendFactorSource1Alpha == 17 &&
		   (int)MTLBlendFactorOneMinusSource1Alpha == 18,
	       "MTLBlendFactor's refused values are not where air_metal.c's refusals name "
	       "them, so a refusal would name the wrong factor");
_Static_assert((int)MTLBlendOperationAdd == 0 && (int)MTLBlendOperationSubtract == 1 &&
		   (int)MTLBlendOperationReverseSubtract == 2 &&
		   (int)MTLBlendOperationMin == 3 && (int)MTLBlendOperationMax == 4,
	       "MTLBlendOperation is not the identity air_metal.c's table states it is");
_Static_assert((int)MTLColorWriteMaskNone == 0 && (int)MTLColorWriteMaskAlpha == 1 &&
		   (int)MTLColorWriteMaskBlue == 2 && (int)MTLColorWriteMaskGreen == 4 &&
		   (int)MTLColorWriteMaskRed == 8 && (int)MTLColorWriteMaskAll == 0xf,
	       "MTLColorWriteMask's bits are not the reverse of Gallium's colormask, which "
	       "is what air_metal.c's table is");

/*
 * AND THE SHAPE OF THE TWO STRUCTS THIS FILE TYPES OUT BY HAND, which is the bug this
 * port has paid for twice: a primitive type that arrived as 1871292816, and struct
 * air_resource growing by eight bytes so that every resource after the first was read
 * eight bytes early.
 *
 * The numbers below are the same numbers asserted in tools/mesa/air/air_metal.c against
 * the library's own definitions. Neither file can see the other's structs, so this is
 * not a comparison -- it is two independent statements of one agreement, and an edit to
 * one side alone fails to build on that side. The offsets are asserted as well as the
 * sizes, because the whole discipline here is that a new field goes on the END: a field
 * inserted in the middle keeps the size and moves everything after it, which is exactly
 * the failure that is invisible.
 */
_Static_assert(sizeof(struct rwa_color_blend) == 32 &&
		   sizeof(struct rwa_render_shape) == 1052 &&
		   offsetof(struct rwa_render_shape, vertex_attribute) == 48 &&
		   offsetof(struct rwa_render_shape, vertex_layout) == 420 &&
		   offsetof(struct rwa_render_shape, color_blend) == 792 &&
		   offsetof(struct rwa_render_shape, color_blend_count) == 1048,
	       "struct rwa_render_shape here is not the shape air_metal.c asserts: a field "
	       "was added in the middle, or only one of the two copies was rebuilt");
_Static_assert(sizeof(struct rwa_render_info) == 116 &&
		   offsetof(struct rwa_render_info, vertex_argbuf_mask) == 100 &&
		   offsetof(struct rwa_render_info, blend_enabled_mask) == 108,
	       "struct rwa_render_info here is not the shape air_metal.c asserts, and this "
	       "one the LIBRARY writes into: too short is a buffer overrun");

/*
 * WHOSE SENTENCE THIS IS. Every line this driver writes goes through here, and the reason
 * is an attribution that could not be made: a boot's log held
 *
 *     redwoodmetal: fragment buffer 2 at offset 1164: ...
 *     redwoodmetal: no drawable: pixel format 500 is not one ...
 *
 * one after the other, and four or five Metal clients were alive in that boot (backboardd,
 * SpringBoard, chronod and this port's own application). The drawable refusal was pinned on
 * backboardd only by reading QuartzCore's own os_log line beside it, which is a guess, and
 * the offset refusals could not be attributed at all. A driver is a library: its lines are
 * written by somebody else's process, and the process is the first thing a reader needs.
 *
 * So the prefix is `redwoodmetal[progname pid]:`, built once per process -- getprogname()
 * is the name launchd or the shell gave it, which is what the log's other lines carry too.
 *
 * It flushes, for the reason RW_ONCE below flushes: stdio to a console is block-buffered,
 * the processes that reach these sites are the ones that then abort, and a line written and
 * not flushed dies with the process. One flush per sentence costs nothing next to the draw
 * that provoked it.
 */
#define rw_print rwm_print       /* src/metal/log.cpp */
/* and the one method this driver installs into Metal's own function class, whose reason
 * is a measurement and is written where it is done (the end of this file) */
static void ensure_function_encoder(void);

/* The compiler library's calls and whether it is there: src/metal/air.cpp loads it, typed by
 * air_metal.h's own prototypes, and refuses a library older than this driver. */
#define mesa (*rwm_air())
#define mesa_available() rwm_air_available()

/*
 * Say a thing once, per place that says it.
 *
 * Every method of this driver that declines to do something is called by a daemon that will
 * call it again on the next frame, so a line per call buries the log that matters and a
 * silent return tells us nothing at all. One line the first time a site is reached is what
 * names the feature the system actually wanted -- which is the whole value of answering
 * "no" instead of being absent: an absent selector aborts the daemon and says nothing, an
 * answer that refuses keeps it running AND leaves its name in the log.
 *
 * And it is flushed, because the measurement says it has to be: searching every log in
 * `logs/` for four different RW_ONCE strings -- the GPU priority, the one ring, the texture
 * view, the resource ID -- finds them **zero** times, while plain printf lines from these
 * same files are in every boot. The processes that reach these sites are the ones that then
 * abort (backboardd aborted 43 times in logs/214841-run.log), and stdio to a console is
 * block-buffered, so a line written and not flushed dies with the process. That is a
 * suspicion with evidence rather than a proof -- the sites may also simply never have been
 * reached -- and the flush is what makes the next boot able to tell the two apart: a line
 * that is now still missing was never reached. One flush per site per process costs nothing;
 * a diagnostic that has never once been seen costs the whole point of writing it.
 */
#define RW_ONCE(...)							\
	do {								\
		static BOOL rw_said_;					\
									\
		if (!rw_said_) {					\
			rw_said_ = YES;					\
			rw_print(__VA_ARGS__);				\
		}							\
	} while (0)

/*
 * A sentence that is true every time it is reached and says nothing new after the first: print
 * it, then only on powers of two, carrying the count. The window server asks this driver for a
 * two-plane YCbCr drawable 862 times in a boot, and 862 identical lines pushed the rest of that
 * boot out of the loader's log, which keeps the first 256 KB and the last megabyte. A refusal
 * that drowns the evidence of what it refused is a worse instrument than a quiet one.
 */
#define RW_AGAIN(...)							\
	do {								\
		static unsigned long rw_times_;				\
									\
		if ((++rw_times_ & (rw_times_ - 1)) == 0) {		\
			rw_print(__VA_ARGS__);				\
			if (rw_times_ > 1) {				\
				rw_print("  ... and that is the %lu'th "\
					 "time\n", rw_times_);		\
			}						\
		}							\
	} while (0)



/* The private base class. Only what is used of it is declared, and nothing of its layout.
 * -init is its own (its method list has it); -initWithAcceleratorPort: is abstract there,
 * which is precisely the invitation this file accepts. */
@interface _MTLDevice : NSObject
- (instancetype)init;
@end

/* The private bases this driver stands on. Only what is used of each is declared, and
 * nothing of any layout. The designated initialisers are the ones Apple's own driver sends
 * to super (read out of IOGPUMetalCommandQueue 0x1ec16da90, IOGPUMetalCommandEncoder
 * 0x1ec171d14 and AGXA11FamilyComputePipeline 0x2092477e0), which is a better authority
 * than a guess at what an init of this kind ought to be called. */
@interface _MTLObjectWithLabel : NSObject
- (instancetype)init;
@end

@interface _MTLResource : _MTLObjectWithLabel
@end

@interface _MTLCommandQueue : _MTLObjectWithLabel
- (instancetype)initWithDevice:(id)device descriptor:(id)descriptor;
@end

@interface _MTLCommandBuffer : _MTLObjectWithLabel
- (instancetype)initWithQueue:(id)queue
	   retainedReferences:(BOOL)retained
	 synchronousDebugMode:(BOOL)synchronous;
@end

@interface _MTLCommandEncoder : _MTLObjectWithLabel
- (instancetype)initWithCommandBuffer:(id)commandBuffer;
/* Declared because it is CALLED: that initialiser retains the command buffer and this is the
 * only thing that gives it back (see -[RedwoodMetalRenderEncoder endEncoding]). */
- (void)endEncoding;
@end

@interface _MTLComputePipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

@interface _MTLRenderPipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

/*
 * The depth/stencil state's base class, and it is one of the good ones: everything but four
 * methods is Apple's and works for us. Read out of the cache the way the rest of this driver
 * was (tools/metal/absmeth.py over `_MTLDepthStencilState`, 10 selectors):
 *
 *   - -initWithDevice:depthStencilDescriptor: (0x181a29b38) is REAL and is the designated
 *     initialiser. It validates the descriptor field by field -- depthCompareFunction and
 *     each face's stencilCompareFunction, stencilFailureOperation, depthFailureOperation and
 *     depthStencilPassOperation are each compared against 8 and reported through
 *     MTLReportFailure when out of range -- then sends `init` to super
 *     (objc_msgSendSuper2), retains the device into _device and copies the descriptor's
 *     label into _label. It also asserts isKindOfClass: on both arguments (_MTLDevice for
 *     the device, MTLDepthStencilDescriptorInternal for the descriptor) and REPORTS rather
 *     than refuses: the failure paths branch back into the chain and the object is built
 *     either way.
 *   - -device and -label are ivar reads, so they work once that initialiser has run;
 *     -description, -formattedDescription: and -dealloc are Apple's.
 *   - -readsDepth, -writesDepth, -readsStencil, -writesStencil (0x181b1b5e0 and its three
 *     neighbours) are two-instruction NIL-STUBS: they answer NO. Those four are the whole of
 *     <MTLDepthStencilStateSPI> beyond the public protocol, and for a state that does write
 *     depth, NO is a false statement about this object -- so they are the four this driver
 *     overrides, out of the descriptor it was given.
 */
@interface _MTLDepthStencilState : NSObject
- (instancetype)initWithDevice:(id)device depthStencilDescriptor:(id)descriptor;
@end

/* The sampler has a real base class that owes its protocol nothing, and the initialiser
 * AGXA11FamilySampler sends to super (0x20930f0f0 -> 0x181a2d414). */
@interface _MTLSamplerState : NSObject
- (instancetype)initWithDevice:(id)device samplerDescriptor:(id)descriptor;
@end

/*
 * The one struct Metal reads out of a device by raw pointer, and the one answer on this
 * device that cannot be a refusal. Nothing here is guessed: the name and all three field
 * types are Apple's own, out of the type encoding MTLDeviceSPI records for the method
 * (`tools/metal/protometh.py`, joined to `absmeth.py` by implementation address):
 *
 *     -targetDeviceInfo    r^{MTLTargetDeviceArch=QI*}16@0:8
 *
 * so this declaration makes our method's recorded encoding the same string as Apple's,
 * rather than a `^v` that happens to pass in the same register. Which field means what was
 * read separately, off the readers -- see -targetDeviceInfo.
 */
struct MTLTargetDeviceArch {
	uint64_t target;      /* +0x00: nothing on this path reads it; Apple's own branch writes 0 */
	uint32_t version;     /* +0x08: the second half of MTLCompilerCache::Version{1, version} */
	char *name;           /* +0x10: formatted into "<pipelinelib>/pipelines.%s", never nil */
};

/* The offsets are the point of the struct, so they are checked rather than trusted: 0x10 is
 * the offset in the instruction that crashed without this answer. */
_Static_assert(sizeof(struct MTLTargetDeviceArch) == 24 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, version) == 0x8 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, name) == 0x10,
	       "Metal reads the name at +0x10 (0x181a3ca3c) and the version at +0x8");

@interface RedwoodMetalDevice : _MTLDevice {
	io_service_t _acceleratorService;   /* the IOAcceleratorES service Metal handed us */
	io_connect_t _gpu;                  /* this port's own gpu client, or IO_OBJECT_NULL */
	struct rwa_device *_mesa;           /* Mesa's screen and context, or NULL */
	BOOL _assertions;                   /* Metal's own assertion switch, stored for it */
	struct MTLTargetDeviceArch _target;     /* answered by pointer; see -targetDeviceInfo */
	char _targetName[96];               /* the bytes _target.name points at, inside the object */
}
/* Not part of any Metal protocol: how this driver's own objects reach the compiler. */
- (struct rwa_device *)mesaDevice;
/* and the argument encoder of one function's `[[buffer(n)]]` set, which the method this
 * driver installs into Metal's own _MTLFunction forwards to (the end of this file says
 * why it has to): the layout is read out of the function's own bitcode, which is the one
 * thing Metal has here and its reflection has not. */
- (id)redwoodArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index;
@end

/* Declaring the protocol is the gate both assertions in MTLAddDevice read. Done at +load
 * for the bundle case (Metal loads us, so Metal is already there) and again before the
 * first use, because a library can also be brought in by hand, before Metal. */
static void
ensure_spi(Class cls)
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");

	if (spi == NULL) {
		rw_print("MTLDeviceSPI is not in this process yet; "
		         "MTLAddDevice would abort\n");
		return;
	}
	if (!class_conformsToProtocol(cls, spi) && !class_addProtocol(cls, spi)) {
		rw_print("class_addProtocol(MTLDeviceSPI) refused\n");
	}
	/* and the one method of Metal's OWN function class this driver has to answer: it is
	 * installed here for the same reason the protocol is declared here, and the whole of
	 * why it is installed at all is at the end of this file. */
	ensure_function_encoder();
}

/* One request of this port's own gpu client, shaped like the msm ioctl it stands for
 * (iokit/Kernel/RedwoodGPU.cpp: kMsmGetParam = 0x40, struct msm_param). The parameters
 * worth asking for are 0x03 CHIP_ID (with the speed bin in the high half), 0x02 GMEM_SIZE
 * and 0x04 MAX_FREQ. Mesa reaches the same client the same way. */
static bool
gpu_param(io_connect_t gpu, uint32_t which, uint64_t *value)
{
	struct {
		uint32_t pipe, param;
		uint64_t value;
		uint32_t len, pad;
	} request = { .pipe = 0, .param = which, .value = 0, .len = 0, .pad = 0 };
	size_t size = sizeof(request);

	if (gpu == IO_OBJECT_NULL) {
		return false;
	}
	if (IOConnectCallStructMethod(gpu, 0x40, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS) {
		return false;
	}
	*value = request.value;
	return true;
}

/* (the kernel's GEM requests a buffer is made with are src/metal/buffer.cpp's now) */


@class RedwoodMetalCommandQueue;
@class RedwoodMetalWork;

@interface RedwoodMetalComputePipeline : _MTLComputePipelineState {
	RedwoodMetalDevice *_owner;
	struct rwa_compute *_compute;
	struct rwa_compute_info _info;
}
@end

/* One recorded dispatch. Metal's rule is that nothing runs until the command buffer is
 * committed, so an encoder records and the command buffer runs; dispatching at encode time
 * would be less code and would quietly reorder work. */
@interface RedwoodMetalComputePipeline (RedwoodMetalInternal)
- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor;
- (struct rwa_compute *)compute;
@end

@interface RedwoodMetalCommandQueue : _MTLCommandQueue
- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device descriptor:(id)descriptor;
@end

@interface RedwoodMetalCommandBuffer : _MTLCommandBuffer
- (instancetype)initWithRedwoodQueue:(RedwoodMetalCommandQueue *)queue;
- (void)recordWork:(RedwoodMetalWork *)work;
- (void)redwoodCommitWorkFromThread;
- (void)redwoodWaitProcessed;
- (id)device;
@end

@interface RedwoodMetalBlitEncoder : _MTLCommandEncoder
- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer;
@end

@interface RedwoodMetalComputeEncoder : _MTLCommandEncoder
- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer;
@end

@interface RedwoodMetalRenderEncoder : _MTLCommandEncoder
- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer
				  descriptor:(id)descriptor;
/* Every form of every draw method records through this one, indexed or not: the thirteen
 * lines that copy the encoder's state into the work item are the same either way, and two
 * copies of them are how one form ends up carrying state the other does not. */
- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance;
@end

@interface RedwoodMetalRenderPipeline : _MTLRenderPipelineState {
	RedwoodMetalDevice *_owner;
	struct rwa_render *_render;
	struct rwa_render_info _info;
}
- (struct rwa_render *)render;
/* Not part of any Metal protocol: whether this pipeline's fragment shader stores to
 * [[depth]], which only whoever read the shader knows. A pass without a depth attachment
 * drops those writes -- Metal's own behaviour -- and a draw that says so is the difference
 * between a measurement and a colour attachment holding its clear value. */
- (BOOL)writesDepth;
/* Which buffer indices this pipeline's vertex stage FETCHES ATTRIBUTES from, one bit
 * each, and how many slots that covers. Metal has ONE table behind
 * -setVertexBuffer:offset:atIndex: and both the vertex descriptor's layouts and the
 * stage's [[buffer(n)]] arguments read out of it, so the encoder cannot know which an
 * index is until it knows the pipeline -- and this is what the draw splits the table
 * with. Zero for a stage that fetches nothing, which is every pipeline this driver drew
 * before the layout existed. */
- (unsigned)vertexStreamMask;
- (unsigned)vertexStreamCount;
/* And the element array the compiler library built out of the descriptor and the module's
 * own interface: how many, and which Metal [[attribute(n)]] each one is. The order is the
 * shader's driver_location order and not Metal's attribute order (docs/metal.md 10.44), so
 * these two are the only way a caller can read that mapping back instead of trusting it --
 * which is what metal_drv_test --draw-attrib prints beside its pixels. Not part of any
 * Metal protocol; Metal's own pipeline reflection would report the same thing. */
- (unsigned)vertexElementCount;
- (unsigned)vertexElementAttribute:(unsigned)index;
@end

@interface RedwoodMetalWork : NSObject {
@public
	/* a draw, a dispatch or a copy as the core recorded it from its encoder's state (src/metal/render.cpp,
	 * compute.cpp, blit.cpp): what src/metal/run.cpp runs at -commit, and Metal's objects it keeps alive */
	struct rwm_work core;
	BOOL held;      /* -retainResources has been called: -dealloc lets go */
}
- (void)retainResources;
@end

@interface RedwoodMetalTexture : _MTLResource {
	RedwoodMetalDevice *_owner;
	struct rwm_texture *_core; /* the texture, its shape, its surfaces: src/metal/texture.cpp */
	int _responsibleProcess;   /* as on the buffer: accounting, not behaviour */
}
@property (readonly) struct rwa_texture *mesaTexture;
- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device core:(struct rwm_texture *)core;
- (struct rwm_texture *)redwoodCore;
- (void)redwoodCopyIn;
- (void)redwoodCopyInNow;
/* Not part of MTLTexture: the depth or stencil aspect of this texture, one float and one
 * byte per texel. Metal's own -getBytes: has no aspect to name and the compiler library
 * refuses a depth texture through it by name, so a caller that wants to know what a depth
 * buffer holds asks for an aspect here instead. Either pointer may be NULL. */
- (BOOL)redwoodReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region;
@end

@interface RedwoodMetalSampler : _MTLSamplerState {
	struct rwa_sampler *_sampler;
}
@property (readonly) struct rwa_sampler *mesaSampler;
@end

@interface RedwoodMetalDepthStencilState : _MTLDepthStencilState {
	struct rwa_depth_stencil *_state;
	BOOL _readsDepth, _writesDepth, _readsStencil, _writesStencil;
}
@property (readonly) struct rwa_depth_stencil *mesaDepthStencil;
- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor;
@end

/*
 * MTLArgumentEncoder. Not a writer: the records live in the compiler library with the
 * destination buffer, because the frontend has already resolved every member of every set
 * to a binding and Metal's 8-byte handles are an Apple GPU's descriptors (air_metal.h).
 * The implementation below carries the whole of why.
 */
@interface RedwoodMetalArgumentEncoder : NSObject
- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule;
/* Not part of MTLArgumentEncoder: the mapping between Metal's [[id(n)]] and the index the
 * GPU is programmed with, so a test can read it back instead of trusting it. */
- (NSUInteger)redwoodMemberCount;
- (NSUInteger)redwoodMetalIndex;
- (BOOL)redwoodMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index;
@end

@interface RedwoodMetalBuffer : _MTLResource {
	RedwoodMetalDevice *_owner;
	struct rwm_buffer *_core; /* the memory and its addresses: src/metal/buffer.cpp */
	int _responsibleProcess;  /* what <MTLResourceSPI> calls the owner, for accounting */
}
@property (readonly) struct rwa_buffer *mesaBuffer;
- (instancetype)initWithDevice:(RedwoodMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length;
@end

/*
 * The formats CoreAnimation's Metal renderer picks for a display it believes is extended-range
 * (the iPhone X's): Apple's private 5xx values beside BGRA10_XR (552) -- 551 for the overdrive
 * look-up table's two targets first of all -- which no public SDK names and the compiler library
 * refuses as unknown. This panel is 8-bit BGRA, so they are made BGRA8Unorm (80), and said once.
 */
#define rw_known_format rwm_known_format     /* src/metal/import.cpp */

/*
 * The function constants a function carries when nothing specialized it -- the window server's
 * with camtl=1, where src/inject.c keeps CoreAnimation's values on the plain function (under the
 * selector redwoodFunctionConstants, Metal's index to the value's bits) because Apple's compiler
 * service cannot read the library. Handed to the compiler library for the next pipeline.
 */
static unsigned
rw_function_constants(id function, struct rwa_constant *out, unsigned max)
{
	NSDictionary *kept = function != nil
	    ? objc_getAssociatedObject(function, sel_registerName("redwoodFunctionConstants")) : nil;
	unsigned n = 0;

	for (NSNumber *index in kept) {
		if (n == max) {
			break;
		}
		out[n].index = (unsigned)[index unsignedIntValue];
		out[n].bits = [[kept objectForKey:index] unsignedLongLongValue];
		n++;
	}
	return n;
}

/* whether the lookup below may settle for a pipeline that differs by a blend mode: only once
 * QuartzCore's own shader, specialized with the pipeline's constants, has failed to build
 * (newRenderPipelineStateWithDescriptor:error: tries without it first) */
static __thread BOOL rw_plib_allow_near;

/* the firmware's two modules for a pipeline of CoreAnimation's, or NO: src/metal/plib.cpp reads
 * QuartzCore's default.pipelinelib; here they are Metal's NSData over the mapped file */
static BOOL
rw_plib_modules_for(NSString *label, NSData **vertex, NSData **fragment)
{
	const void *v, *f;
	size_t vs, fs;

	if (!rwm_plib_modules_for([label UTF8String], rw_plib_allow_near, &v, &vs, &f, &fs)) {
		return NO;
	}
	*vertex = [NSData dataWithBytesNoCopy:(void *)v length:vs freeWhenDone:NO];
	*fragment = [NSData dataWithBytesNoCopy:(void *)f length:fs freeWhenDone:NO];
	return YES;
}

#include <pthread.h>
#include <pthread/qos.h>

/* the compiler library on a stack of 16 MB, for a block (src/metal/worker.cpp's rwm_on_big_stack says why) */
static void
rw_block_main(void *block)
{
	((void (^)(void))block)();
}

static void
rw_on_big_stack(void (^work)(void))
{
	rwm_on_big_stack(rw_block_main, work);
}

/*
 * CoreAnimation's pipelines, compiled before they are asked for. A pipeline built the first time
 * an effect is used -- a window opening, Spotlight, the icons' jiggle -- held that frame up for
 * 220 to 450 ms (run 331), every boot again, since nothing outlives the process. Its label and the
 * shape this driver derives from the descriptor are all a build needs (the shaders come from the
 * pipeline library by the label), so:
 *
 *   recorded  every pipeline of CoreAnimation's a process builds is kept as a recipe -- the label
 *             and the shape's non-zero words -- and said, a piece at a time, into the log when
 *             notification com.redwood.rwm_recipes comes (rwtouch notify); tools/rwm-recipes.py
 *             puts them together into userland/share/rwm-recipes.txt for the image;
 *   warmed    at start each process reads its own section of that file ("[backboardd]", "[SpringBoard]"
 *             ...: run 333 showed SpringBoard and the posters build their own) and a thread of its own
 *             builds every recipe in turn, before the home screen is up. A request for one that is ready takes it (the
 *             shape compared whole); one not yet built is built by the caller as before.
 *
 * Builds go one at a time under the compile lock (src/metal/recipes.cpp: render_constants is global state in the library).
 * Boot-arg rwm_warm=0: none of it.
 */
/* (the recipes themselves -- their text, the file, the built pipelines handed over, the compile lock --
 * are src/metal/recipes.cpp's; here is what needs Metal's own objects: the functions a recipe names) */
static BOOL rw_plib_modules_for(NSString *label, NSData **vertex, NSData **fragment);
static id rw_warm_device;               /* the MTLDevice, for QuartzCore-default.metallib's functions */

static void *
rw_warm_main(void *argument)
{
	struct rwa_device *device = argument;
	uint64_t started = rw_ns();
	struct rwm_warm_item w;

	pthread_setname_np("redwood pipeline warm-up");
	for (unsigned i = 0; rwm_warm_item(i, &w); i++) {
		@autoreleasepool {
			NSData *vertex = nil, *fragment = nil;
			BOOL found;

			if (w.fragment[0] != '\0') {
				/* (outside the lock: the library and its functions are Metal's, not the compiler's) */
				static id library;
				static SEL bitcode;

				if (library == nil && rw_warm_device != nil) {
					library = [rw_warm_device newLibraryWithURL:[NSURL fileURLWithPath:@"/redwood/share/QuartzCore-default.metallib"]
									     error:NULL];
					bitcode = sel_getUid("bitcodeData");
				}
				id vf = w.vertex[0] != '\0' ? [[library newFunctionWithName:@(w.vertex)] autorelease] : nil;
				id ff = [[library newFunctionWithName:@(w.fragment)] autorelease];

				vertex = vf != nil && [vf respondsToSelector:bitcode] ? ((NSData *(*)(id, SEL))objc_msgSend)(vf, bitcode) : nil;
				fragment = ff != nil && [ff respondsToSelector:bitcode] ? ((NSData *(*)(id, SEL))objc_msgSend)(ff, bitcode) : nil;
				found = fragment != nil && (w.vertex[0] == '\0' || vertex != nil);
			} else {
				found = rw_plib_modules_for([NSString stringWithUTF8String:w.label], &vertex, &fragment) &&
				    fragment != nil;
			}
			rwm_warm_build(i, device, vertex != nil ? [vertex bytes] : NULL, vertex != nil ? [vertex length] : 0,
				       [fragment bytes], [fragment length], found);
		}
	}
	rwm_warm_report(rw_ns() - started);
	return NULL;
}

/* in every process with this device: the notification heard, its own recipes read and the thread started */
static void
rw_warm_start(id metal_device, struct rwa_device *device)
{
	static const char *const files[] = { "/redwood/share/rwm-recipes.txt", "/System/Library/Caches/rwm-recipes.txt" };
	int token;

	if (device == NULL) {
		return;
	}
	rw_warm_device = metal_device;
	if (rwm_boot_arg("rwm_warm=0")) {
		return;
	}
	notify_register_dispatch("com.redwood.rwm_recipes", &token,
	                         dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int t) {
		(void)t;
		rwm_recipes_say();
	});
	{
		static dispatch_source_t every_minute;

		every_minute = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
		                                      dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
		dispatch_source_set_timer(every_minute, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC),
		                          60 * NSEC_PER_SEC, 5 * NSEC_PER_SEC);
		dispatch_source_set_event_handler(every_minute, ^{
			rwm_recipes_say_new();
		});
		dispatch_resume(every_minute);
	}
	if (rwm_warm_load(files, 2) > 0) {
		pthread_attr_t attr;
		pthread_t thread;

		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 16 << 20);    /* the library's recursion, as rw_on_big_stack */
		if (pthread_create(&thread, &attr, rw_warm_main, device) == 0) {
			pthread_detach(thread);
		}
		pthread_attr_destroy(&attr);
	}
}

/* the drawable import (src/metal/drawable.cpp) as the core's surface import asks for it */
static struct rwa_texture *
rw_in_place(void *context, struct rwa_device *device, IOSurfaceRef surface, unsigned plane,
	    const struct rwa_texture_shape *shape, struct rwm_import *took, char *why, size_t why_size)
{
	struct rwd_surface laid;
	struct rwa_texture *texture;

	(void)context;
	memset(&laid, 0, sizeof(laid));
	/* (rwd_texture_shape is rwa_texture_shape's layout under drawable.cpp's name) */
	texture = rwd_texture_for_surface(device, surface, plane, (const struct rwd_texture_shape *)shape, &laid, why,
					  why_size);
	if (texture != NULL) {
		took->surface = laid.surface;
		took->width = laid.width;
		took->height = laid.height;
		took->row_bytes = laid.rowBytes;
	}
	return texture;
}

@implementation RedwoodMetalDevice

+ (void)load
{
	ensure_spi(self);
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	/*
	 * No device for SpringBoard. With one, SiriUIActivation's set-up (SiriUICore -> MPSImage) makes
	 * a Metal Performance Shaders kernel without asking MPSSupportsMTLDevice first; MPS knows only
	 * Apple's GPUs, has no MPSDevice for this one, and ends the process (MPSKernel.mm:95, "[%@
	 * initWithDevice:] Failed to get MPSDevice."), so launchd restarts SpringBoard every six seconds
	 * and the screen stays black. SpringBoard draws nothing with Metal itself -- the window server
	 * composites it -- and with nil Siri leaves MPS alone, as it did before metal=1. Refused here
	 * rather than from src/inject_sb.c because asking Metal from that library's constructor means
	 * loading this bundle inside dyld's initializers, which SpringBoard did not survive.
	 * Boot-arg sbmtl=1 gives SpringBoard the device as well.
	 */
	if (strcmp(getprogname(), "SpringBoard") == 0 &&
	    !(rwm_boot_arg("sbmtl=1"))) {
		rw_print("no device for SpringBoard (Metal Performance Shaders would end it)\n");
		/* not released: -[_MTLDevice dealloc] on an object whose -init never ran hands
		   libdispatch a null queue (signal 11 under _MTLCreateSystemDefaultDevice). Metal asks
		   once per process, so this is one small object. */
		return nil;
	}
	/*
	 * Nor for the widgets' renderers: chronod (and the widgets' own extensions) draw with RenderBox,
	 * which, given a device, wants a drawable this driver does not make outside the window server --
	 * "RBDisplayList: no drawable" and an abort, a dozen times a minute, and every widget a flat
	 * placeholder (the solid squares, 24 October). Without one they draw with the processor, as
	 * they did before Metal.
	 */
	if (strcmp(getprogname(), "chronod") == 0 || strstr(getprogname(), "Widget") != NULL) {
		rw_print("no device for %s (RenderBox draws with the processor then)\n", getprogname());
		return nil;
	}
	ensure_spi([self class]);

	self = [super init];
	if (self == nil) {
		return nil;
	}
	_acceleratorService = port;
	if (port != IO_OBJECT_NULL) {
		IOObjectRetain(port);
	}

	/* The gpu this driver would actually drive. Not fatal if it is absent: the port
	 * starts the Adreno only with the boot argument gpu=1, and a device object that
	 * cannot allocate is still the thing under test until the resource layer exists. */
	io_service_t gpu = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));

	_gpu = IO_OBJECT_NULL;
	if (gpu == IO_OBJECT_NULL) {
		rw_print("no RedwoodGPU service (the gpu was not started)\n");
	} else {
		kern_return_t kr = IOServiceOpen(gpu, mach_task_self(), 0, &_gpu);

		rw_print("RedwoodGPU client %s (%#x)\n",
		         kr == KERN_SUCCESS ? "opened" : "refused", kr);
		IOObjectRelease(gpu);
	}

	/* And the compiler, if it is there. Its absence is not fatal: the device still has to
	 * exist and answer, it just cannot make a pipeline (and says so when asked). */
	if (mesa_available()) {
		char why[256] = { 0 };

		_mesa = mesa.device_open(why, sizeof(why));
		rw_print("the compiler library %s\n",
		         _mesa != NULL ? mesa.device_name(_mesa) : why);
		rw_warm_start(self, _mesa);
	}

	/* The struct Metal reads out of this device by raw pointer, filled here rather than on
	 * demand: the pointer is handed out without a lock, from whichever thread a renderer
	 * happens to be on, and here is the one moment when nobody can have asked yet. The
	 * name is Apple's own rule for a device that declines -- its -name with the spaces
	 * taken out (0x181a3ca0c) -- but the bytes are COPIED, which Apple's branch does not
	 * need to do: it keeps the autoreleased -UTF8String in a struct on its own stack and is
	 * done with it nine instructions later, while this one is answered to callers for as
	 * long as the device lives. See -targetDeviceInfo. */
	NSString *plain = [[self name] stringByReplacingOccurrencesOfString:@" "
								 withString:@""];
	const char *bytes = [plain UTF8String];

	snprintf(_targetName, sizeof(_targetName), "%s", bytes != NULL ? bytes : "");
	_target.target = 0;
	_target.version = 0;
	_target.name = _targetName;
	rw_print("targetDeviceInfo answers {0, 0, \"%s\"} -- _MTLDevice's own entry "
	         "answers NULL and Metal reads it without looking, so the precompiled pipelines "
	         "this device will be asked for are pipelines.%s\n", _target.name, _target.name);
	return self;
}

- (struct rwa_device *)mesaDevice
{
	return _mesa;
}

- (void)dealloc
{
	if (_mesa != NULL) {
		mesa.device_close(_mesa);
	}
	if (_gpu != IO_OBJECT_NULL) {
		IOServiceClose(_gpu);
	}
	if (_acceleratorService != IO_OBJECT_NULL) {
		IOObjectRelease(_acceleratorService);
	}
	[super dealloc];
}

/* The profile is what -[_MTLDevice initLimits] switches on, and it was swept on the device:
 * 0, 1 and 4 to 9 fill the limits table, while 2, 3 and 10 to 12 abort inside Apple on
 * `_limits.maxColorAttachments > 0`. 0 is taken because it is the lowest that works and
 * because its table (31 buffers, 16 samplers, 512 threads per threadgroup, 16 KB of
 * threadgroup memory, 8192 textures) is a plausible floor for this Adreno.
 *
 * "Which profile the hardware deserves is a separate question, to be answered when something
 * is drawn" -- and something has now been drawn, so here is the answer so far. This number is
 * the ONLY input Apple has for what this device supports: -[_MTLDevice initGPUFamilySupport]
 * switches on it and pushes a list of MTLGPUFamily into the vector that -supportsFamily:
 * searches. Profile 0 pushes Apple1 and Common1 and nothing else.
 *
 * That is why SpringBoard cannot start. Siri's orb prewarms an MPSImageGaussianBlur, and
 * MPSDevice::GetMPSDevice is a descending ladder of -supportsFamily: from Apple8 (1008) down
 * to Apple2 (1002) which returns NULL when none answers. It never asks about Apple1 -- the
 * constant 0x3e9 is nowhere in that function -- so the one family we claim is the one family
 * it does not want, and the process aborts in MPSKernel with "Failed to get MPSDevice"
 * (docs/metal.md 10.47). Profile 1 is the lowest that pushes Apple2.
 *
 * So the profile is a boot-arg (metal_profile=N), not because it should stay configurable but
 * because the cost of each step up has to be measured rather than argued: every family claimed
 * is a promise, and some of those promises this driver cannot keep. Answering LESS is not the
 * safe direction either -- an empty family vector makes
 * +[MTLFXSpatialScalerDescriptor supportsDevice:] take this for a discrete GPU and say yes
 * unconditionally -- so there is no answer here that is safe by default, only one that is
 * measured. Apple's own A11 driver answers 5.
 */
- (NSUInteger)featureProfile
{
	static NSUInteger profile = (NSUInteger)-1;

	if (profile == (NSUInteger)-1) {
		const char *at;

		profile = 0;
		if ((at = strstr(rwm_boot_args(), "metal_profile=")) != NULL && (at == rwm_boot_args() || at[-1] == ' ')) {
			profile = (NSUInteger)strtoul(at + strlen("metal_profile="), NULL, 10);
		}
		rw_print("feature profile %lu, which is what Metal reads this device's "
		         "whole capability surface out of (metal_profile= to change it; 0 claims Apple1 "
		         "alone, 1 adds Apple2, which is the lowest rung MPS asks about)\n",
		         (unsigned long)profile);
	}
	return profile;
}

/* A placeholder, and named as one: nothing has yet measured what the kernel's gemNew will
 * give in one allocation. It only has to be non-zero for initLimits to accept it. */
- (uint64_t)maxBufferLength
{
	return 256ULL << 20;
}

/*
 * The render pipeline, by the same rule as the compute one: both of _MTLDevice's entry points
 * go through its nil -compiler, so a driver that leaves them alone gets nil and no reason.
 *
 * One asymmetry is deliberate and is the measured road rather than a shortcut. When the
 * descriptor names no vertex function, the compiler library supplies a pass-through vertex
 * stage over the whole attachment -- which is exactly how air_run has drawn every fragment
 * shader of the corpus, 255 of them compared pixel by pixel (docs/air.md). A descriptor that
 * does name one passes both modules and nothing is generated.
 */
/* every pipeline this device declines, by its label and the reason, in the kernel's kept lines:
   the window server ends itself on the first one ("Metal failed to build render pipeline") */
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
				     error:(NSError **)error
{
	NSError *mine = nil;
	uint64_t built = rw_ns();
	/*
	 * CoreAnimation's own pipelines, built once: its label names the whole of the state
	 * (draw.<attachments and blend>_<shader specialization>), and the window server asked for
	 * the same one four to six times, each a build of 13 to 19 seconds when the shader is the
	 * specialized uber shader (24 October). Kept for the life of the device.
	 */
	static NSMutableDictionary *built_by_label;
	static pthread_mutex_t built_lock = PTHREAD_MUTEX_INITIALIZER;
	NSString *label = [descriptor label];
	BOOL cacheable = [label hasPrefix:@"com.apple.coreanimation.draw."];

	if (cacheable) {
		pthread_mutex_lock(&built_lock);
		id kept = [built_by_label objectForKey:label];

		[kept retain];
		pthread_mutex_unlock(&built_lock);
		if (kept != nil) {
			if (error != NULL) {
				*error = nil;
			}
			return kept;
		}
	}
	id state = [self redwoodRenderPipelineWithDescriptor:descriptor error:&mine];

	if (state == nil && [[descriptor label] hasPrefix:@"com.apple.coreanimation.draw."]) {
		/* the shader with its own constants did not build: the nearest one the firmware
		 * specialized, which loses the layer's blend mode but keeps the window server up */
		char line[200];

		snprintf(line, sizeof(line), "!rwm %s: %s; the nearest specialized one instead",
		         [[descriptor label] UTF8String], [[mine localizedDescription] UTF8String] ?: "(no reason)");
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
		mine = nil;
		rw_plib_allow_near = YES;
		state = [self redwoodRenderPipelineWithDescriptor:descriptor error:&mine];
		rw_plib_allow_near = NO;
	}
	if (state == nil) {
		char line[200];

		snprintf(line, sizeof(line), "!rwm %s: %s", [[descriptor label] UTF8String] ?: "(no label)",
		         [[mine localizedDescription] UTF8String] ?: "(no reason)");
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
		rw_print("%s\n", line + 1);
	}
	if (cacheable && state != nil) {
		pthread_mutex_lock(&built_lock);
		if (built_by_label == nil) {
			built_by_label = [[NSMutableDictionary alloc] init];
		}
		[built_by_label setObject:state forKey:label];
		pthread_mutex_unlock(&built_lock);
	}
	rw_perf.pipes++;
	rw_perf.compile += rw_ns() - built;
	if (rw_ns() - built > 200000000ull) {
		/* a pipeline that held the window server up for a fifth of a second or more: which one */
		char line[200];

		snprintf(line, sizeof(line), "!rwm slow pipeline %s: %llu ms%s (%s)",
		         [[[descriptor label] stringByReplacingOccurrencesOfString:@"com.apple.coreanimation."
		                                                         withString:@""] UTF8String] ?: "?",
		         (rw_ns() - built) / 1000000ull, state == nil ? " (failed)" : "", getprogname());
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}
	if (error != NULL) {
		*error = mine;
	}
	return state;
}

- (id)newRenderPipelineStateWithTileDescriptor:(id)descriptor
				       options:(NSUInteger)options
				    reflection:(id *)reflection
					 error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	char line[200];

	snprintf(line, sizeof(line), "!rwm tile pipeline %s: not supported",
	         [[descriptor label] UTF8String] ?: "(no label)");
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	return [self fail:error with:@"tile render pipelines: not on this GPU"];
}

- (id)redwoodRenderPipelineWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
				    error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (descriptor == nil) {
		return [self fail:error with:@"no render pipeline descriptor"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libredwoodair.dylib is "
					     @"missing or older than this driver"];
	}
	if (mesa.render_create == NULL) {
		/* The order of these two matters: the first version asked about render_create
		 * first and would have told a caller that a library which never loaded "can
		 * compute but not yet draw", which is a sentence about a library that is not
		 * there. */
		return [self fail:error with:@"this compiler library can compute but not yet "
					     @"draw"];
	}

	id vertex = [descriptor vertexFunction];
	id fragment = [descriptor fragmentFunction];

	if (fragment == nil) {
		return [self fail:error with:@"the descriptor has no fragment function, and this "
					     @"driver has nothing to draw without one"];
	}
	if (![fragment respondsToSelector:bitcode] ||
	    (vertex != nil && ![vertex respondsToSelector:bitcode])) {
		return [self fail:error with:@"a function in this descriptor does not answer "
					     @"bitcodeData, so its AIR cannot be read"];
	}
	NSData *fragment_air = ((NSData *(*)(id, SEL))objc_msgSend)(fragment, bitcode);
	NSData *vertex_air = vertex != nil ?
	    ((NSData *(*)(id, SEL))objc_msgSend)(vertex, bitcode) : nil;

	if (fragment_air == nil || [fragment_air length] == 0) {
		return [self fail:error with:@"the fragment function has no bitcode"];
	}
	/* CoreAnimation's own pipeline, from the firmware's specialized modules (src/metal/plib.cpp) */
	NSData *plib_vertex = nil, *plib_fragment = nil;
	BOOL from_plib = rw_plib_modules_for([descriptor label], &plib_vertex, &plib_fragment);

	if (from_plib) {
		static unsigned told;

		if (told++ < 3) {
			rw_print("%s: QuartzCore's own specialized modules from the pipeline library\n",
			         [[descriptor label] UTF8String]);
		}
		vertex_air = plib_vertex;
		fragment_air = plib_fragment;
	}

	struct rwa_render_shape shape = { .sample_count = (unsigned)[descriptor sampleCount] };

	/*
	 * THE COLOUR ATTACHMENTS, each one's format AND ITS BLEND STATE.
	 *
	 * The blend half is what the home screen was missing: with no blend state in the
	 * contract the library made every pipeline opaque, and CoreAnimation composites with
	 * PREMULTIPLIED ALPHA -- so a label layer whose background is transparent black was
	 * drawn as opaque black, which is the black box behind every icon name in the
	 * photographs.
	 *
	 * All eight are read, as the formats are, and for the same reason: Apple's array
	 * answers -objectAtIndexedSubscript: for every index in range and an attachment an
	 * application never touched holds Apple's own defaults -- blendingEnabled NO,
	 * writeMask All, the factors One and Zero, both operations Add (MTLRenderPipeline.h).
	 * Nothing is interpreted here. Every number goes across as Metal's own and the
	 * library maps the three enumerations by table, because a cast between them is wrong:
	 * MTLBlendFactor and pipe_blendfactor agree on two of nineteen values and
	 * MTLColorWriteMask's bits run in the opposite order to Gallium's colormask.
	 *
	 * `color_blend_count` is set to eight -- all eight entries filled in -- and NOT to
	 * the attachment count: a zero there means "this shape carries no blend state at
	 * all", which is the old opaque behaviour, and that is a thing this driver must never
	 * say by accident.
	 */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		unsigned format = attachment != nil ? rw_known_format((unsigned)[attachment pixelFormat]) : 0;

		shape.color_format[i] = format;
		if (format != 0) {
			shape.color_count = i + 1;
		}
		if (attachment == nil) {
			/* an array that answered nil has no defaults to pass on, so this
			 * entry says what Metal's own untouched descriptor says: no
			 * blending, every channel written */
			shape.color_blend[i].write_mask = 0xf;   /* MTLColorWriteMaskAll */
			continue;
		}
		shape.color_blend[i].blending_enabled =
		    [attachment isBlendingEnabled] ? 1 : 0;
		shape.color_blend[i].rgb_blend_operation =
		    (unsigned)[attachment rgbBlendOperation];
		shape.color_blend[i].alpha_blend_operation =
		    (unsigned)[attachment alphaBlendOperation];
		shape.color_blend[i].source_rgb_blend_factor =
		    (unsigned)[attachment sourceRGBBlendFactor];
		shape.color_blend[i].destination_rgb_blend_factor =
		    (unsigned)[attachment destinationRGBBlendFactor];
		shape.color_blend[i].source_alpha_blend_factor =
		    (unsigned)[attachment sourceAlphaBlendFactor];
		shape.color_blend[i].destination_alpha_blend_factor =
		    (unsigned)[attachment destinationAlphaBlendFactor];
		shape.color_blend[i].write_mask = (unsigned)[attachment writeMask];
	}
	shape.color_blend_count = 8;
	/*
	 * And the measurement this was built to make: what the window server actually asks
	 * for, one line per attachment it has, printed BEFORE the library can refuse any of
	 * it. These are Metal's own numbers, so `blend 1 rgb 1/5 op 0 alpha 1/5 op 0` is
	 * premultiplied source-over (One, OneMinusSourceAlpha, Add) and `blend 1 rgb 4/5` is
	 * straight alpha instead -- the one distinction a boot log has to be able to make.
	 *
	 * THE LABEL IS HERE BECAUSE IT IS THE ANSWER. CoreAnimation builds a pipeline's
	 * label out of the specification it compiled: `encode_segment` emits a `B<mnemonic>`
	 * segment IF AND ONLY IF the fixed-function blend mode is not its "none" value
	 * (0x36), so `com.apple.coreanimation.draw.PBGRABsovXm_...` is a pipeline whose
	 * blending this descriptor carries and `com.apple.coreanimation.draw.PBGRAXm_...` is
	 * one that blends INSIDE ITS FRAGMENT SHADER and leaves this descriptor at Apple's
	 * defaults. One grep over a boot log then says which of the two the home screen is
	 * made of, which no amount of reading the enumerations can.
	 */
	/* (what it asks for is said once the shape is whole: rwm_shape_say, below) */
	/* Both aspect formats, as Metal's descriptor has them, and the library requires
	 * them to name one texture: an application with a combined format sets the same
	 * MTLPixelFormat on both, because there is one attachment behind the two
	 * properties. Passing them through separately rather than folding them here is
	 * what lets the library refuse a descriptor that names two different formats by
	 * name instead of this driver picking one of them. */
	shape.depth_format = (unsigned)[descriptor depthAttachmentPixelFormat];
	shape.stencil_format = (unsigned)[descriptor stencilAttachmentPixelFormat];

	/*
	 * THE VERTEX DESCRIPTOR, which is what the window server asked this driver for and
	 * did not get: one pipeline specification was refused 80 times with "reads vertex
	 * attributes ([[stage_in]]), and this contract carries no vertex attribute layout"
	 * (docs/metal.md 10.44, 10.45), because the layout lives here and nowhere in the AIR.
	 *
	 * Both of Metal's arrays are read whole, index by index, and passed through as
	 * Metal's own numbers -- the library decides what it can honour and refuses the
	 * rest BY NAME, which is the half this driver must not do for it: a format a6xx
	 * cannot fetch and an offset past twelve bits are both things freedreno would take
	 * silently (air_metal.h has the register fields).
	 *
	 * There is no count to read on either side. Apple's arrays answer
	 * -objectAtIndexedSubscript: for every index in range and a descriptor an
	 * application never touched holds MTLVertexFormatInvalid in every attribute, which
	 * is what the library reads as "not configured" -- so the loop is the whole array
	 * and the formats decide. The range is Apple's 31 of each, from the feature tables,
	 * and asking past it would raise.
	 */
	MTLVertexDescriptor *vd = [descriptor vertexDescriptor];

	if (vd != nil) {
		for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
			MTLVertexAttributeDescriptor *a =
			    [[vd attributes] objectAtIndexedSubscript:i];
			unsigned format = a != nil ? (unsigned)[a format] : 0;

			if (format == 0) {
				continue;
			}
			shape.vertex_attribute[i].format = format;
			shape.vertex_attribute[i].offset = (unsigned)[a offset];
			shape.vertex_attribute[i].buffer_index = (unsigned)[a bufferIndex];
		}
		for (unsigned k = 0; k < RWA_MAX_VERTEX_BUFFERS; k++) {
			MTLVertexBufferLayoutDescriptor *l =
			    [[vd layouts] objectAtIndexedSubscript:k];

			if (l == nil) {
				continue;
			}
			shape.vertex_layout[k].stride = (unsigned)[l stride];
			shape.vertex_layout[k].step_function = (unsigned)[l stepFunction];
			shape.vertex_layout[k].step_rate = (unsigned)[l stepRate];
		}
	}
	/* what it asks for said, a ready one from the warm-up, or the build: src/metal/pipeline.cpp */
	const char *label = [[descriptor label] UTF8String];
	struct rwa_render *render = NULL;
	struct rwa_render_info info = { 0 };
	struct rwa_constant vc[128], fc[128];
	char why[512] = { 0 };

	if (!rwm_render_prepare(label, &shape, vd != nil, &render, &info)) {
		struct rwm_render_request request = {
			.vertex = vertex_air != nil ? [vertex_air bytes] : NULL,
			.vertex_size = vertex_air != nil ? [vertex_air length] : 0,
			.fragment = [fragment_air bytes],
			.fragment_size = [fragment_air length],
			.vertex_name = !from_plib && vertex != nil ? [[vertex name] UTF8String] : NULL,
			.fragment_name = !from_plib && fragment != nil ? [[fragment name] UTF8String] : NULL,
			.vertex_constants = vc,
			.fragment_constants = fc,
			.vertex_constant_count = from_plib ? 0 : rw_function_constants(vertex, vc, 128),
			.fragment_constant_count = from_plib ? 0 : rw_function_constants(fragment, fc, 128),
		};

		render = rwm_render_compile(_mesa, label, &request, &shape, &info, why, sizeof(why));
		if (render == NULL) {
			return [self fail:error with:[NSString stringWithUTF8String:why]];
		}
	}
	return [[RedwoodMetalRenderPipeline alloc] initWithRedwoodDevice:self
								 render:render
								   info:&info
							     descriptor:descriptor];
}

- (id)newRenderPipelineStateWithDescriptor:(id)descriptor
				   options:(NSUInteger)options
				reflection:(id *)reflection
				     error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;   /* reflection is Metal's compiler's, and ours is nil */
	}
	return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}

/*
 * The asynchronous forms, and they are not a convenience: without them Metal's own versions
 * send the pipeline request to the nil compiler, return, and NEVER CALL THE HANDLER -- so an
 * application that builds a pipeline asynchronously waits for ever instead of being told no.
 * Reversing the four entry points is what turned that up (0x181aabc48 and 0x181aabccc both
 * end in a stack block handed to the compiler), and a hang is the one failure mode that
 * cannot be read out of a log.
 *
 * Building synchronously and calling the handler at once is allowed: Metal does not promise
 * the handler runs later, only that it runs.
 */
- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
			   completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
				     options:(NSUInteger)options
			   completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor
						     options:options
						  reflection:&reflection
						       error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

/*
 * The mesh forms are the opposite hazard: they are NOT routed through the compiler, they ask
 * the mesh descriptor for a plain render pipeline descriptor and re-enter one of the four
 * forms above on self (0x181aabf60 and its three neighbours). Inherited, they would hand a
 * mesh pipeline to a vertex-and-fragment builder and produce something built wrong instead of
 * a refusal. Mesh shaders are an A14-and-later feature and nothing here implements them.
 */
- (id)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
				       options:(NSUInteger)options
				    reflection:(id *)reflection
					 error:(NSError **)error
{
	(void)descriptor; (void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self fail:error with:@"mesh pipelines are not implemented, and the inherited "
				     @"path would have built this one as a vertex pipeline"];
}

- (void)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
					 options:(NSUInteger)options
			       completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;

	(void)[self newRenderPipelineStateWithMeshDescriptor:descriptor options:options
						 reflection:NULL error:&error];
	if (handler != NULL) {
		handler(nil, nil, error);
	}
}

/* A texture is Metal's descriptor handed through to the compiler library in Metal's own
 * terms; a pixel format it cannot translate is refused by name, because a format that is
 * almost right computes a wrong picture instead of failing. */
/*
 * THE SELECTORS THE PROTOCOLS DO NOT LIST, and this is the second time one of them has killed a
 * process. owes.py counts what the protocols require and found nothing missing; these are
 * declared on Apple's counterpart CLASSES -- AGXA11FamilyDevice and IOGPUMetalDevice -- which our
 * device does not descend from, so nothing in the chain answers them and no protocol says so.
 * tools/metal/owesclass.py counts that other list: 343 such selectors, 72 of which something in
 * this image actually sends, and NONE of the 72 behind a -respondsToSelector:. QuartzCore does not
 * ask, it sends. So every one of them is terminate(), not a composite that merely looks wrong --
 * which is what setProtectionOptions: did to the window server and what the first of these just
 * did to mediaserverd.
 *
 * A texture LAYOUT answers bytes-per-row, image stride, size and alignment for a descriptor
 * without allocating anything, so CoreAnimation can size an IOSurface before it has a texture.
 * nil is the honest answer rather than a measured one: CA::Render::new_mtl_texture_layout hands
 * our result straight back to its own caller, and that caller is in no other image of this cache,
 * so whether a nil is tolerated is unknown. If the log says it is not, the next step is a small
 * object over the arithmetic -newTextureWithDescriptor: below already does.
 */
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor
			isHeapOrBufferBacked:(BOOL)backed
{
	(void)descriptor;
	(void)backed;
	RW_ONCE("redwoodmetal: a texture layout was asked for and this driver has none to give; "
		"CoreAnimation sizes a surface with it before any texture exists, and nil is the "
		"answer until a boot says nil is not tolerated\n");
	return nil;
}

/*
 * A resource group names resources whose cache lines the GPU may drop at the end of a pass -- an
 * AGX cache-management optimisation this port has no control over. nil here is not a guess: the
 * caller tests it (cbz x0 in CA::OGL::MetalContext::mark_cache_drop_hints) and composites without
 * it, and a nil also leaves the counter at zero, which makes -encodeCacheHintTag:... and
 * -encodeCacheHintFinalize:... unreachable. One nil answers three selectors, which is the right
 * shape: refuse where the decision is made rather than at each leaf.
 */
- (id)newResourceGroupFromResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources;
	(void)count;
	return nil;
}

/*
 * Overriding this keeps _MTLDevice's own body from running on our device, and that body is what
 * sends -copyIOSurfaceSharedTextureProperties: and -sparseSurfaceDefaultValue, neither of which
 * we answer. So one override closes three. It agrees with -newSharedTextureHandle below, which
 * answers nil for the same reason: nothing here is shared between processes.
 */
- (id)newSharedTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	(void)descriptor;
	return nil;
}

/*
 * A texture over memory the CALLER allocated, which CoreAnimation's update_texture stores and
 * uses (docs/metal.md 10.48, selector 5 of the class-declared list). It is answered by name
 * rather than left unrecognized, because an unrecognized selector is terminate() and a named nil
 * is a line in the log -- mediaserverd died 43 times on the one above this before it was
 * answered at all.
 *
 * nil and not a texture, because the contract has no call for it. rwa_texture_from_surface takes
 * an IOSurface, which the kernel hands over as a buffer (kRedwoodSurface); a bare pointer into
 * the caller's address space is a different import, and a texture made by copying instead would
 * be wrong in a way nothing would report: the caller writes to its own pages and expects the
 * texture to change.
 */
- (id)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length
		     descriptor:(MTLTextureDescriptor *)descriptor
		    deallocator:(void (^)(void *, NSUInteger))deallocator
{
	(void)bytes;
	(void)length;
	(void)descriptor;
	(void)deallocator;
	RW_ONCE("redwoodmetal: a texture over the caller's own memory was asked for, and the "
		"contract has no import for a bare pointer -- only for an IOSurface. Copying "
		"instead would be worse than nil: the caller writes to its pages and expects the "
		"texture to follow\n");
	return nil;
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a texture needs the compiler library\n");
		return nil;
	}
	struct rwa_texture_shape shape = {
		.pixel_format = rw_known_format((unsigned)[descriptor pixelFormat]),
		.type = (unsigned)[descriptor textureType],
		.width = (unsigned)[descriptor width],
		.height = (unsigned)[descriptor height],
		.depth = (unsigned)[descriptor depth],
		.array_length = (unsigned)[descriptor arrayLength],
		.mip_levels = (unsigned)[descriptor mipmapLevelCount],
		.sample_count = (unsigned)[descriptor sampleCount],
		.usage = (unsigned)[descriptor usage],
	};
	struct rwa_texture *texture = mesa.texture_create(_mesa, &shape, why, sizeof(why));

	if (texture == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[RedwoodMetalTexture alloc] initWithRedwoodDevice:self
							 texture:texture
							   shape:&shape];
}

/* The sampler's own base class exists and owes its protocol nothing, so this is the cheapest
 * object in the driver: the state goes to the compiler library, which maps it the same way it
 * maps a shader's own `constexpr sampler` word -- one vocabulary, so the two can be checked
 * against each other (docs/air.md 3.3). */
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a sampler needs the compiler library\n");
		return nil;
	}
	struct rwa_sampler_state state = {
		.address = { (unsigned)[descriptor sAddressMode],
			     (unsigned)[descriptor tAddressMode],
			     (unsigned)[descriptor rAddressMode] },
		.min_filter = (unsigned)[descriptor minFilter],
		.mag_filter = (unsigned)[descriptor magFilter],
		.mip_filter = (unsigned)[descriptor mipFilter],
		.compare_function = (unsigned)[descriptor compareFunction],
		.max_anisotropy = (unsigned)[descriptor maxAnisotropy],
		.normalized_coordinates = [descriptor normalizedCoordinates] ? 1 : 0,
		.lod_min = [descriptor lodMinClamp],
		.lod_max = [descriptor lodMaxClamp],
	};
	struct rwa_sampler *sampler = mesa.sampler_create(_mesa, &state, why, sizeof(why));

	if (sampler == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[RedwoodMetalSampler alloc] initWithRedwoodDevice:self
							 sampler:sampler
						      descriptor:descriptor];
}

/*
 * The depth/stencil state, which _MTLDevice does not implement at all: unlike -compiler,
 * whose body is `mov x0, #0; ret`, this selector is simply absent from the base class's
 * method list, so the whole of it is a driver's. (Apple's own is AGXA11FamilyDepthStencilState
 * over the same _MTLDepthStencilState this one inherits.)
 *
 * MTLDepthStencilDescriptor is read here, in Metal's words, and translated by the compiler
 * library -- which is where the two enumerations that do NOT line up are turned over:
 * MTLStencilOperationInvert is 5 and Gallium's INVERT is 7, so a cast would turn "invert the
 * stencil" into "increment it and wrap", and that is a different picture rather than a
 * failure. This side only reads properties and names them.
 */
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (descriptor == nil) {
		rw_print("a depth/stencil state needs a descriptor\n");
		return nil;
	}
	if (_mesa == NULL) {
		rw_print("a depth/stencil state needs the compiler library\n");
		return nil;
	}
	if (mesa.depth_stencil_create == NULL) {
		rw_print("this compiler library has no depth/stencil state, so it "
		         "is older than this driver and cannot test depth\n");
		return nil;
	}

	/* frontFaceStencil and backFaceStencil are null_resettable: the setter takes nil
	 * and the getter hands back Metal's default descriptor, so both are read without a
	 * nil check -- and when one is nil anyway, the zeroed face left behind is compare
	 * Always with every operation Keep, which is that same default. */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };
	struct rwa_depth_stencil_state state = {
		.depth_compare_function = (unsigned)[descriptor depthCompareFunction],
		.depth_write_enabled = [descriptor isDepthWriteEnabled] ? 1 : 0,
	};
	struct rwa_stencil_face *out[2] = { &state.front, &state.back };

	for (unsigned i = 0; i < 2; i++) {
		if (faces[i] == nil) {
			*out[i] = (struct rwa_stencil_face){
				.compare_function = MTLCompareFunctionAlways,
				.read_mask = 0xffffffff, .write_mask = 0xffffffff,
			};
			continue;
		}
		*out[i] = (struct rwa_stencil_face){
			.compare_function = (unsigned)[faces[i] stencilCompareFunction],
			.stencil_fail_operation = (unsigned)[faces[i] stencilFailureOperation],
			.depth_fail_operation = (unsigned)[faces[i] depthFailureOperation],
			.depth_stencil_pass_operation =
			    (unsigned)[faces[i] depthStencilPassOperation],
			.read_mask = (unsigned)[faces[i] readMask],
			.write_mask = (unsigned)[faces[i] writeMask],
		};
	}

	struct rwa_depth_stencil *made = mesa.depth_stencil_create(_mesa, &state, why,
								   sizeof(why));

	if (made == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[RedwoodMetalDepthStencilState alloc] initWithRedwoodDevice:self
								     state:made
								descriptor:descriptor];
}

/* _MTLDevice's own newCommandQueue and newCommandQueueWithMaxCommandBufferCount: are real
 * and both end here, where its body is `mov x0, #0; ret` (0x181aac688) -- a refusal that
 * announces itself as nothing at all. */
/*
 * -[MTLDevice newArgumentEncoderWithArguments:], the path an application takes when it
 * describes the set itself instead of taking it from a shader.
 *
 * Everything this encoder needs is in the descriptors -- how many members, which kind
 * each is, and therefore the length and the alignment -- and that is the whole of the
 * layout, because the records are keyed by Metal's `[[id(n)]]` and the FLAT index each
 * member is bound at comes from the pipeline's own shader at the draw. So an encoder made
 * this way records exactly as one made from a module does.
 *
 * Three shapes are refused BY NAME rather than encoded into something, each because the
 * frontend refuses the matching shader when it translates it (docs/air.md 8.2.1), so a
 * record of ours could never be asked for:
 *
 *   - a member that is not a pointer, a texture or a sampler: a VALUE inside the set,
 *     which this frontend binds nothing for;
 *   - an array of descriptors (arrayLength above 1), which no module in reach declares
 *     and which a shader could index at run time;
 *   - indices with a gap in them. Metal's own encodedLength is over the highest index, so
 *     a set declaring [[id(0)]] and [[id(5)]] is 48 bytes and not 16 -- and this library
 *     answers 8 bytes per member. Saying so is better than handing back a length the
 *     application would allocate too little memory from.
 */
- (id)newArgumentEncoderWithArguments:(NSArray *)arguments
{
	struct rwa_argbuf_layout layout;
	NSUInteger count = [arguments count];
	struct rwm_argument *members = calloc(count != 0 ? count : 1, sizeof(*members));
	BOOL served;

	for (NSUInteger i = 0; members != NULL && i < count; i++) {
		MTLArgumentDescriptor *d = [arguments objectAtIndex:i];

		members[i] = (struct rwm_argument){ [d index], [d dataType], [d arrayLength] };
	}
	/* what this port's frontend can bind of it, and what not, said: src/metal/argenc.cpp */
	served = members != NULL && rwm_argenc_layout_of(members, count, &layout);
	free(members);
	if (!served) {
		return nil;
	}
	return [[RedwoodMetalArgumentEncoder alloc] initWithRedwoodDevice:self layout:&layout
							      fromModule:NO];
}

/* The iOS 16 form, which takes a reflection object (MTLBufferBinding) instead of
 * descriptors. Declined by name: every reflection object in this process comes from
 * Metal's own compiler, which is nil here (docs/metal.md 10.7), so an application that
 * has one did not get it from us and this driver has nothing to read out of it. */
- (id)newArgumentEncoderWithBufferBinding:(id)binding
{
	(void)binding;
	rw_print("-newArgumentEncoderWithBufferBinding: takes a reflection "
	         "object, and reflection on this port needs Metal's own compiler, which is "
	         "nil; ask a function for an encoder (-newArgumentEncoderWithBufferIndex:) or "
	         "describe the set with MTLArgumentDescriptors\n");
	return nil;
}

/*
 * And the function path, which the method installed into Metal's own _MTLFunction
 * forwards here.
 * The layout is read out of the function's bitcode -- the one thing this port has that
 * Metal's reflection has not -- so the numbers are the module's own.
 */
- (id)redwoodArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (function == nil || ![function respondsToSelector:bitcode]) {
		rw_print("this function does not answer bitcodeData, so its "
		         "argument buffers cannot be read\n");
		return nil;
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

	if (air == nil || [air length] == 0) {
		rw_print("this function has no bitcode\n");
		return nil;
	}

	struct rwa_argbuf_layout layout;

	/* the sets the module declares and the one at that index, or which there are: src/metal/argenc.cpp */
	if (!rwm_argenc_layout_for(_mesa, [air bytes], [air length], [[function name] UTF8String], index, &layout)) {
		return nil;
	}
	return [[RedwoodMetalArgumentEncoder alloc] initWithRedwoodDevice:self layout:&layout fromModule:YES];
}

- (id)newCommandQueueWithDescriptor:(id)descriptor
{
	/* Apple's own -newCommandQueue and -newCommandQueueWithMaxCommandBufferCount: build a
	 * descriptor and end here, so one usually arrives; when this is called directly with
	 * nil, one is made, because the base class validates it and refuses nil. */
	id use = descriptor != nil ? [[descriptor retain] autorelease] :
	    [[[MTLCommandQueueDescriptor alloc] init] autorelease];

	return [[RedwoodMetalCommandQueue alloc] initWithRedwoodDevice:self descriptor:use];
}

/*
 * Where subsystem (1) is finally asked to do its work.
 *
 * Both pipeline entry points of _MTLDevice route through -[_MTLDevice compiler], which is
 * `mov x0, #0; ret`, so a driver that leaves them alone gets nil and no error -- which is
 * exactly what this device did before this method existed. Overriding them, as AGX does, is
 * what keeps us out of Apple's compiler: the AIR comes from the function object and goes to
 * our own front end.
 *
 * The cost of that road, named in docs/metal.md 10.7: with -compiler still nil,
 * -newFunctionWithName:constantValues: and -[MTLFunction reflectionWithOptions:] return nil,
 * because those go through the compiler inside Metal's own classes where we cannot reach.
 */
- (id)newComputePipelineStateWithDescriptor:(id)descriptor
				    options:(NSUInteger)options
				 reflection:(id *)reflection
				      error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;   /* reflection needs Metal's compiler; saying nil is honest */
	}
	return [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor
				  error:error];
}

- (id)newComputePipelineStateWithDescriptor:(id)descriptor error:(NSError **)error
{
	return [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor
				  error:error];
}

/* _MTLDevice's own version of this builds an MTLComputePipelineDescriptorInternal and
 * re-sends to self, so overriding it is not strictly needed -- but a descriptor is what the
 * pipeline state's base class demands, and making one here keeps the three entry points
 * reaching the same code with the same arguments. */
/* The same hole on the compute side, and the same reason. */
- (void)newComputePipelineStateWithFunction:(id)function
			  completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newComputePipelineStateWithFunction:function error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newComputePipelineStateWithDescriptor:(id)descriptor
				      options:(NSUInteger)options
			    completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newComputePipelineStateWithDescriptor:descriptor
						      options:options
						   reflection:&reflection
							error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

- (id)newComputePipelineStateWithFunction:(id)function error:(NSError **)error
{
	Class internal = objc_getClass("MTLComputePipelineDescriptorInternal");
	id descriptor = internal != Nil ? [[internal alloc] init] :
	    (id)[[MTLComputePipelineDescriptor alloc] init];

	if (descriptor == nil) {
		return [self fail:error with:@"no pipeline descriptor class to build one from"];
	}
	[descriptor setComputeFunction:function];
	id state = [self pipelineForFunction:function descriptor:descriptor error:error];

	[descriptor release];
	return state;
}

/* One place where a function becomes a pipeline, so that the three entry points above
 * cannot drift apart. */
- (id)pipelineForFunction:(id)function descriptor:(id)descriptor error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (function == nil) {
		return [self fail:error with:@"the descriptor has no compute function"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libredwoodair.dylib "
					     @"is missing"];
	}
	if (![function respondsToSelector:bitcode]) {
		return [self fail:error with:@"this function does not answer bitcodeData, so its "
					     @"AIR cannot be read"];
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

	if (air == nil || [air length] == 0) {
		return [self fail:error with:@"the function has no bitcode"];
	}

	char why[512] = { 0 };
	struct rwa_compute_info info = { 0 };
	struct rwa_compute *compute = mesa.compute_create(_mesa, [air bytes], [air length],
							  [[function name] UTF8String], &info,
							  why, sizeof(why));
	if (compute == NULL) {
		/* The front end's own sentence, passed through rather than replaced: it says
		 * which construct it refused, and an application can act on that. */
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	return [[RedwoodMetalComputePipeline alloc] initWithRedwoodDevice:self
								 compute:compute
								    info:&info
							      descriptor:descriptor];
}

- (id)fail:(NSError **)error with:(NSString *)reason
{
	/* said as well: CoreAnimation ends the window server on a pipeline refused here and prints
	   nothing of the NSError ("Metal failed to build render pipeline", namespace 13 code 4) */
	RW_AGAIN("refused by the device: %s\n", [reason UTF8String]);
	if (error != NULL) {
		*error = [NSError errorWithDomain:@"RedwoodMetal" code:1
					 userInfo:@{ NSLocalizedDescriptionKey: reason }];
	}
	return nil;
}

/*
 * The rest of what MTLDevice and MTLDeviceSPI require.
 *
 * Measured on the phone: 44 of the methods those two protocols require are answered by
 * nothing in the _MTLDevice chain (docs/metal.md 10.8). Until they are answered, any
 * application that asks one gets "unrecognized selector" and dies -- and that matters more
 * than it looks, because the gates inside Metal are assertions, so a process that asks a
 * question this device cannot answer does not degrade, it aborts, and launchd restarts it.
 *
 * So each of these answers, and each answers HONESTLY: a capability we have not got is
 * declined rather than claimed, a quantity we do not track says so in its comment rather
 * than inventing a number that something would then rely on. Where a number is a placeholder
 * it is marked as one; where the right authority is the compiler library rather than this
 * file, that is written down, because the next person to touch it will want to know.
 */

- (uint64_t)registryID
{
	/* Not invented: the registry's own identifier for the service Metal handed us. */
	uint64_t id = 0;

	if (_acceleratorService != IO_OBJECT_NULL &&
	    IORegistryEntryGetRegistryEntryID(_acceleratorService, &id) == KERN_SUCCESS) {
		return id;
	}
	return 0;
}

/* The a635 runs up to 1024 threads in a threadgroup, which is also what every pipeline the
 * compiler library has compiled reports back (10.15). The per-dimension limits are Metal's
 * usual shape for that; the authority should become the library itself once the contract has
 * a call for it. */
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 64); }

/* The number _MTLDevice's own limits table was filled with for this feature profile. Also a
 * placeholder in the same sense: the hardware's figure belongs to Mesa, not here. */
- (NSUInteger)maxThreadgroupMemoryLength { return 16384; }

- (BOOL)hasUnifiedMemory { return YES; }   /* one memory on this SoC, shared with the CPU */

/* Nothing here tracks allocation yet, and two of these are read by applications deciding how
 * much to allocate, so they answer a figure that is plausible for this phone rather than 0,
 * which would read as "allocate nothing". That they are not measured is the point of this
 * comment. */
- (uint64_t)recommendedMaxWorkingSetSize { return 512ULL << 20; }
- (uint64_t)currentAllocatedSize         { return 0; }
- (uint64_t)sharedMemorySize             { return 512ULL << 20; }
- (uint64_t)dedicatedMemorySize          { return 0; }   /* none: the memory is shared */
- (uint64_t)maxBufferLengthInternal      { return 256ULL << 20; }

- (BOOL)isLowPower      { return NO; }
- (BOOL)isHeadless      { return NO; }
- (BOOL)isRemovable     { return NO; }
- (BOOL)supportPriorityBand { return NO; }

/* Declined rather than claimed: no sparse textures, and a yes here would be acted on.
 *
 * Depth24Unorm_Stencil8 stays NO now that there IS a depth path, and it is not a leftover:
 * Metal's own header marks that format API_UNAVAILABLE(ios), the compiler library refuses it
 * by name for the same reason, and iOS depth is Depth16Unorm, Depth32Float or
 * Depth32Float_Stencil8 -- which this device does answer for. A yes here would offer a
 * format no texture could be made of. */
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }

/*
 * One sample, STILL -- and this one needs its reason written down, because it is now the
 * narrower statement of the two this driver could make.
 *
 * The compiler library does make multisample textures since docs/metal.md 10.34: the
 * Adreno gives 2 and 4 samples, a render pass of that count fills one, and a shader reads
 * one with read(coord, sample). So "count == 1" is no longer what this device CAN do with
 * a multisample texture.
 *
 * It is what this device can do with one for an APPLICATION, and that is what the
 * selector asks. An application that is told four samples makes a four-sample attachment
 * and then asks for its result in a one-sample texture -- MTLStoreActionMultisampleResolve,
 * or MTLRenderPassColorAttachmentDescriptor.resolveTexture -- and this driver has neither:
 * the contract stores what it has and resolves nothing. AppleMetalGLRenderer is exactly
 * such an application (its own six multisample fragment shaders are the ones 10.34 is
 * about), and a GL context given an MSAA config it cannot resolve is worse off than one
 * given none. So the yes waits for the resolve, and the measurement of the texture does
 * not wait for the yes: a test binds one without asking.
 *
 * Note also that -supportsTextureSampleCount: is not a second answer to keep in step. It
 * is one instruction in _MTLDevice (0x181a17890) that tail-calls -supportsSampleCount:,
 * which _MTLDevice itself does not implement at all -- so this method is the whole of
 * both, and without it that forward would die on an unrecognized selector naming OUR
 * class (docs/metal.md 10.1's trap).
 */
- (BOOL)supportsSampleCount:(NSUInteger)count { return count == 1; }
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
{
	(void)type; (void)format; (void)samples;
	return MTLSizeMake(0, 0, 0);
}
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
			  sparsePageSize:(NSInteger)page
{
	(void)page;
	return [self sparseTileSizeWithTextureType:type pixelFormat:format sampleCount:samples];
}

/* These are bitfields of floating-point behaviours a device claims. Claiming none is the
 * honest answer while nothing has verified any of them on this hardware -- and docs/air.md
 * is precisely the document that would have to grow the evidence first. */
- (uint64_t)singleFPConfig { return 0; }
- (uint64_t)halfFPConfig   { return 0; }
- (uint64_t)doubleFPConfig { return 0; }

- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(NSUInteger)format
{
	(void)format;
	return 16;   /* a placeholder: the real figure is per format and belongs to Mesa */
}

/* Heaps are suballocation of one big allocation, and nothing here suballocates yet. Metal's
 * contract allows nil and a zero size-and-align, so these decline rather than lie. */
- (id)newHeapWithDescriptor:(id)descriptor { (void)descriptor; return nil; }
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)length; (void)options;
	return (MTLSizeAndAlign){ 0, 0 };
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(id)descriptor
{
	(void)descriptor;
	return (MTLSizeAndAlign){ 0, 0 };
}

/* Everything else the two protocols require and this driver has not built. Each returns the
 * API's own failure value, so a caller finds out by the rule Metal already has, rather than
 * by a selector that does not exist. */
- (id)newFence                                      { return nil; }
/*
 * A library from a URL that is not there: Metal's own -[_MTLDevice newLibraryWithURL:error:] asserts
 * ("url must not be nil") and the process aborts. AppleCVAPhoto, which mediaserverd's capture stack
 * starts at launch, asks for its metallib by a URL its bundle lookup could not make here, and
 * mediaserverd -- the one that decodes HEIC for everybody, the wallpaper's layers among them -- died
 * at every start (run 269). Nil gets an error, as a missing file does; the rest is Metal's.
 */
- (id)newLibraryWithURL:(id)url error:(NSError **)error
{
	if (url == nil) {
		RW_ONCE("newLibraryWithURL: with no URL: refused with an error, not Metal's assertion\n");
		if (error != NULL) {
			*error = [NSError errorWithDomain:@"MTLLibraryErrorDomain" code:6 /* MTLLibraryErrorFileNotFound */
						 userInfo:@{ NSLocalizedDescriptionKey: @"no URL for the library" }];
		}
		return nil;
	}
	return [super newLibraryWithURL:url error:error];
}
/*
 * The one method -[CAMetalLayer nextDrawable] asks of a device (docs/metal.md 10.46):
 * allocate_drawable_texture builds an ordinary MTLTextureDescriptor, sets its private
 * rotation/isDrawable/framebufferOnly/usage and sends this. The surface, its geometry and
 * every refusal live in src/metal/drawable.cpp, because they answer CoreAnimation rather than
 * Metal; what is left here is the object, which has to be of this class.
 *
 * The refusal is printed rather than returned silently, and that is not a style choice:
 * CoreAnimation does not test what it gets back, so a bare nil becomes a CAMetalDrawable
 * whose -texture is nil, and the application finds out later and somewhere else.
 */
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)s plane:(NSUInteger)p
{
	/* the road is the core's (src/metal/import.cpp); here are Metal's descriptor and the object made */
	char why[512] = { 0 };
	struct rwm_import road;
	struct rwa_texture_shape shape = {
		.pixel_format = rw_known_format((unsigned)[d pixelFormat]),
		.type = (unsigned)[d textureType],
		.width = (unsigned)[d width],
		.height = (unsigned)[d height],
		.depth = (unsigned)[d depth],
		.array_length = (unsigned)[d arrayLength],
		.mip_levels = (unsigned)[d mipmapLevelCount],
		.sample_count = (unsigned)[d sampleCount],
		.usage = (unsigned)[d usage],
	};
	struct rwm_texture *core = rwm_surface_import(_mesa, (IOSurfaceRef)s, (unsigned)p, &shape, rw_in_place, NULL,
						      &road, why, sizeof(why));

	if (core == NULL) {
		return nil;
	}

	RedwoodMetalTexture *made = [[RedwoodMetalTexture alloc] initWithRedwoodDevice:self core:core];

	if (made != nil && road.kind == RWM_IMPORT_COPY_IN) {
		[made redwoodCopyIn];   /* (now, or at -commit on the driver's thread) */
	}
	return made;
}
- (id)newBufferWithIOSurface:(void *)surface        { (void)surface; return nil; }
- (id)newBufferWithBytesNoCopy:(void *)bytes
			length:(NSUInteger)length
		       options:(NSUInteger)options
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	(void)bytes; (void)length; (void)options; (void)deallocator;
	return nil;   /* wrapping the caller's own pages needs the kernel to map them: not yet */
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
			      maxCommandCount:(NSUInteger)count
				      options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
				    maxCount:(NSUInteger)count
				     options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectComputeCommandEncoderWithBuffer:(id)b { (void)b; return nil; }
- (id)newIndirectRenderCommandEncoderWithBuffer:(id)b  { (void)b; return nil; }
- (id)newIndirectArgumentBufferLayoutWithStructType:(id)t { (void)t; return nil; }
- (id)newVisibleFunctionTableWithDescriptor:(id)d      { (void)d; return nil; }
- (id)newIntersectionFunctionTableWithDescriptor:(id)d { (void)d; return nil; }

/*
 * The last paths in the chain that would still abort.
 *
 * Asking the device whether it responds to these says yes -- they are real entries in
 * _MTLDevice's method list -- and calling one aborts, because each body is
 * [self doesNotRecognizeSelector:_cmd]. Seven methods of _MTLDevice are abstract in that
 * way (tools/metal/absmeth.py), one of them is -initWithAcceleratorPort: which this driver
 * implements, and the other six are these: ray tracing. Overriding them to decline turns the
 * last aborting paths into ordinary nil, which is what the "0 unanswered" measurement needs
 * in order to mean anything.
 */
- (id)newAccelerationStructureWithSize:(NSUInteger)size { (void)size; return nil; }
- (id)newAccelerationStructureWithSize:(NSUInteger)size resourceIndex:(NSUInteger)index
{
	(void)size; (void)index;
	return nil;
}
- (id)newAccelerationStructureWithSize:(NSUInteger)size withDescriptor:(id)descriptor
{
	(void)size; (void)descriptor;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer offset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer
				  offset:(NSUInteger)offset
			   resourceIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	return nil;
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)size
{
	(void)size;
	return (MTLSizeAndAlign){ 0, 0 };
}

- (BOOL)metalAssertionsEnabled { return _assertions; }
- (void)setMetalAssertionsEnabled:(BOOL)enabled { _assertions = enabled; }

/* Without this the device inherits _MTLDevice's own answer, which is "Apple GPU" -- true of
 * the code and false of the hardware. The name is Mesa's renderer string for this chip
 * (docs/air.md: `Adreno 7c+ Gen 3, chip 0x06030500`), and the identifier beside it is asked
 * of the kernel rather than written down, so a different phone says something different.
 * Nothing in Metal's registration reads this; a person reading a log does. */
- (NSString *)name
{
	uint64_t chip = 0;

	if (gpu_param(_gpu, 0x03, &chip)) {
		return [NSString stringWithFormat:@"Adreno 7c+ Gen 3 (chip %#x, Redwood)",
				 (unsigned)(chip & 0xffffffffu)];
	}
	return @"Adreno (Redwood; the GPU is not started)";
}

/*
 * The answer that could not be "nothing", and the first place in this driver where that is
 * true. logs/210311-run.log is backboardd dying every ten seconds on
 *
 *   signal 11 at 0x10, pc 0x181a3ca3c, called from 0x18847f6dc
 *
 * which is MTLPipelineLibraryBuilder::newLibraryWithFile + 836, reached from
 * CA::OGL::new_metal_context + 1808 -- QuartzCore asking this device for the precompiled
 * pipelines of its own default.pipelinelib. Nothing of ours is on that stack:
 * -[_MTLDevice newPipelineLibraryWithFilePath:error:] is three instructions (0x181a3d9b0,
 * `mov x1, x0; ldr x0, [x0, #0x180]; b`) that hand the device and its own builder ivar to
 * Apple's C++, which gets as far as parsing pipelines.desc out of the image and then asks us
 * one question. The three instructions that matter:
 *
 *   0x181a3c9ec  objc_opt_respondsToSelector(device, "targetDeviceInfo")   -> YES
 *   0x181a3c9f8  objc_msgSend$targetDeviceInfo                             -> NULL
 *   0x181a3ca3c  ldr x8, [x23, #0x10]                                      -> 0x10
 *
 * Both halves of that are Apple's. `_MTLDevice` HAS an entry for -targetDeviceInfo and its
 * body is `mov x0, #0; ret` (0x181aa8d98 -- tools/metal/absmeth.py calls that a nil-stub),
 * so the runtime answers the question YES on our behalf and the pointer is NULL; and the
 * caller treats a YES as a promise and dereferences what comes back. Declining is therefore
 * not on the table: returning nil IS what crashes, and the only way to make the runtime say
 * NO would be to lie in -respondsToSelector:, which would also lie to every caller that
 * does not ask first. Of the eight objc_opt_respondsToSelector sites in all of
 * Metal.framework this is the only one whose answer is a raw pointer rather than a scalar or
 * an object -- the other seven ask about -shaderDebugInfoCaching, -maximumComputeSubstreams
 * (twice), -presentWithOptions: (twice), -boolValue and one selector held in a register.
 *
 * The shape can recur, and the population is counted rather than feared: of `_MTLDevice`'s
 * 522 instance methods 74 are nil-stubs, and 14 of those are declared to return a raw
 * pointer (absmeth.py joined to protometh.py by implementation address). Thirteen of the
 * fourteen are the pipeline-cache key family -- `*FunctionKeyWith...:keySize:` and
 * `get*FunctionId:compilerOptions:compilerOptionsSize:` -- which is reached only once a
 * pipeline cache actually holds something; this is the fourteenth, and the first one
 * anything asked for.
 *
 * So the struct is real, and its contents are the ones Apple's own code writes in the branch
 * it takes for a device that does not respond (0x181a3ca04): zero, zero, and the device's
 * -name with the spaces removed. Its shape is known twice over, from two sources that agree
 * -- that branch, which builds one on its own stack, and the type MTLDeviceSPI records for
 * this method, `r^{MTLTargetDeviceArch=QI*}16@0:8`. Each field by its reader:
 *
 *   +0x00  nobody on this path reads it, and Apple's branch writes 0.
 *   +0x08  read by -[_MTLPipelineCache initWithFilePath:readOnly:deviceInfo:] (0x181a3b720),
 *          which pairs it with 1 into an MTLCompilerCache::Version for
 *          MTLCompilerCache::createPipelineDataCache. That version decides whether a cache
 *          FILE is stale; this cache is opened read-only (w3=1 at 0x181a3ca64) over a file
 *          no image here has, so there is nothing for it to invalidate, and 0 is the number
 *          Apple's own branch uses. It becomes a real question the day this driver WRITES a
 *          pipeline cache, and not before.
 *   +0x10  formatted into "%s/pipelines.%s" (0x181a3ca48) -- the per-GPU cache file inside
 *          QuartzCore.framework/default.pipelinelib. It must be a C string and not NULL:
 *          Metal's second cache, the one for `functions`, it builds with a zeroed struct and
 *          an EMPTY name itself (0x181a3ca7c), which is the measurement that says a zeroed
 *          struct is acceptable to _MTLPipelineCache.
 *
 * What this buys is a refusal with a reason instead of a crash, and the refusal is the
 * honest one: that image ships `functions` and `pipelines.desc` and no pipelines.* for any
 * GPU at all (not even the A11 this phone was born with), so the cache comes back nil,
 * Metal fills the NSError with "Could not open pipeline cache file: %s" (code 6,
 * 0x181a3cb2c), and QuartzCore logs "Metal failed to load pipeline library: %s" and carries
 * on -- 0x18847f6f0 is a `cbnz`, not an assert. Nothing is lost by it: a precompiled
 * pipeline in that cache would be A11 machine code, which this driver could not run anyway.
 * The pipelines CoreAnimation needs get compiled the ordinary way, through
 * -newRenderPipelineStateWithDescriptor: and AIR.
 */
- (const struct MTLTargetDeviceArch *)targetDeviceInfo
{
	/* The whole point of this method is that a NULL here is fatal, so it must not depend on
	 * an initialiser having run: an object built with plain -init rather than Metal's
	 * -initWithAcceleratorPort: has a zeroed ivar, and an empty name is what Apple's own
	 * second cache passes for itself anyway. The array is zeroed by the runtime, so it
	 * already spells "". */
	if (_target.name == NULL) {
		_target.name = _targetName;
	}
	RW_ONCE("Metal asked for targetDeviceInfo (pipelines.%s); it is answered "
		"with a real struct because Apple's own answer here is a NULL this driver does "
		"not get to decline\n", _target.name);
	return &_target;
}

/* The two buffer entry points of MTLDevice that nothing in the chain answers. Anything
 * Metal would ask about storage modes is answered by the buffer itself. */
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)options;   /* every allocation here is CPU-visible; the buffer says so honestly */
	return [[RedwoodMetalBuffer alloc] initWithDevice:self gpu:_gpu length:length];
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(NSUInteger)options
{
	RedwoodMetalBuffer *buffer = [self newBufferWithLength:length options:options];

	if (buffer != nil && bytes != NULL && [buffer contents] != NULL) {
		memcpy([buffer contents], bytes, length);
	}
	return buffer;
}

/* Profile 0's own table says maxVertexAmplificationCount is 1, so anything above one is
 * not supported, and saying otherwise would be a lie Metal would act on. */
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count
{
	return count <= 1;
}

@end

@implementation RedwoodMetalBuffer

+ (void)load
{
	Protocol *p = objc_getProtocol("MTLBuffer");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLBuffer) refused\n");
	}
}

- (instancetype)initWithDevice:(RedwoodMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length
{
	char why[256] = { 0 };

	self = [super init];
	if (self == nil) {
		return nil;
	}
	/* Mesa's memory when the compiler library is loaded (only such a buffer can be handed to
	 * a dispatch), the kernel's GEM directly without it: which one is -mesaBuffer */
	_core = rwm_buffer_new(mesa_available() ? [device mesaDevice] : NULL, gpu, length, why,
			       sizeof(why));
	if (_core == NULL) {
		rw_print("no buffer of %lu bytes: %s\n", (unsigned long)length, why);
		[self release];
		return nil;
	}
	_owner = device;
	return self;
}

- (struct rwa_buffer *)mesaBuffer
{
	return _core != NULL ? rwm_buffer_mesa(_core) : NULL;
}

- (void)dealloc
{
	if (_core != NULL) {
		rwm_buffer_free(_core);
	}
	[super dealloc];
}

- (void *)contents          { return rwm_buffer_contents(_core); }
- (NSUInteger)length        { return rwm_buffer_length(_core); }
- (id)device                { return _owner; }
- (uint64_t)gpuAddress      { return rwm_buffer_gpu_address(_core); }
- (NSUInteger)allocatedSize { return rwm_buffer_length(_core); }

/* The memory is write-combined and the GPU reads it through no cache of ours, so there is
 * nothing to push out; saying so is not the same as doing nothing by accident. */
- (void)didModifyRange:(NSRange)range    { (void)range; }

/* What was asked for does not change what was given: every allocation here is one kind of
 * memory, and the honest answer is the kind it is. */
- (NSUInteger)storageMode        { return 0; }   /* MTLStorageModeShared */
- (NSUInteger)cpuCacheMode       { return 1; }   /* MTLCPUCacheModeWriteCombined */
- (NSUInteger)resourceOptions    { return 0; }
- (NSUInteger)hazardTrackingMode { return 0; }   /* MTLHazardTrackingModeDefault */

/* MTLPurgeableStateNonVolatile (2), whatever was asked for, and that is the answer and not
 * a refusal: this memory is never volatile -- the kernel holds it from kMsmGemNew to
 * kDrmGemClose -- so there is no state for a caller to change and none to report but this
 * one. (Zero, which this used to answer, is not an MTLPurgeableState at all, and Metal has
 * a path that then believes the resource was purged: see -addPurgedResource: on the command
 * buffer.) */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }
- (id)heap                      { return nil; }
- (NSUInteger)heapOffset        { return 0; }
- (BOOL)isAliasable             { return NO; }
- (void)makeAliasable           { }

/*
 * What <MTLResourceSPI> and <MTLBuffer> require and nothing in this chain answered: ten
 * methods, every one of them an abort in whatever daemon asked first. They are answers, and
 * most of them are true rather than evasive -- which is the point of counting what a
 * protocol demands instead of what our own tests happen to send.
 *
 * Aliasing: nothing here aliases anything. Every buffer is its own GEM allocation (or its
 * own Mesa allocation), there are no heaps and -makeAliasable does nothing, so "does this
 * overlap that" is NO for every pair, for certain.
 *
 * Completion: this driver's -commit runs the recorded work and -waitUntilCompleted fences
 * it, and nothing hands a resource back to a caller before the submission it was part of
 * has been made. There is no per-resource completion to wait for, so YES and a return are
 * the honest pair -- and if a second ring ever appears, these two are where it has to be
 * answered again.
 */
- (BOOL)doesAliasResource:(id)resource { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)isComplete        { return YES; }
- (void)waitUntilComplete { }
- (BOOL)isPurgeable       { return NO; }
/* Protected (DRM) memory is a kernel capability this port does not have; zero is "none of
 * the protection options", which is what this memory has. */
- (NSUInteger)protectionOptions { return 0; }
/* The options before the device filtered them: the device filters nothing, so this is the
 * same answer -resourceOptions gives. */
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Whose allocation this is for the kernel's accounting. We do not tell the kernel, so the
 * only true answer is this process, and a setter that remembers what it was told. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }
/*
 * A texture laid over this buffer's memory. Textures exist now -- the comment here used to say
 * they did not -- but the contract still has no call that lays one over a buffer we already own,
 * so this is nil with a reason rather than nil with a stale excuse.
 */
- (id)newTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
		   bytesPerRow:(NSUInteger)bytesPerRow
{
	(void)descriptor; (void)offset; (void)bytesPerRow;
	RW_ONCE("redwoodmetal: a texture over a buffer was asked for; the contract lays a texture "
		"over an IOSurface and over memory of its own, but not over a buffer this driver "
		"already holds\n");
	return nil;
}

- (id)newLinearTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
			 bytesPerRow:(NSUInteger)bytesPerRow
		       bytesPerImage:(NSUInteger)bytesPerImage
{
	(void)bytesPerImage;
	return [self newTextureWithDescriptor:descriptor offset:offset bytesPerRow:bytesPerRow];
}
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range { (void)marker; (void)range; }
- (void)removeAllDebugMarkers { }

@end

@implementation RedwoodMetalTexture

+ (void)load
{
	/* MTLTexture is what an application sees. MTLTextureImplementation is what Metal
	 * itself checks: -[MTLRenderPassColorAttachmentDescriptorInternal setTexture:]
	 * (0x181a350a0) asks conformsToProtocol: for it and reports "texture is not a
	 * MTLTextureImplementation." Only the conformance is tested on that path -- nothing
	 * sends its 68 members -- so declaring it is honest and is what lets a texture of ours
	 * be a render target at all. */
	const char *names[] = { "MTLTexture", "MTLTextureImplementation" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *p = objc_getProtocol(names[i]);

		if (p == NULL) {
			continue;
		}
		if (!class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
			rw_print("class_addProtocol(%s) refused\n", names[i]);
		}
	}
}

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			      texture:(struct rwa_texture *)texture
				shape:(const struct rwa_texture_shape *)shape
{
	self = [super init];
	if (self == nil) {
		mesa.texture_destroy(texture);
		return nil;
	}
	_core = rwm_texture_new(texture, shape);     /* (takes the texture, also when it fails) */
	if (_core == NULL) {
		[self release];
		return nil;
	}
	_owner = device;
	return self;
}

- (void)dealloc
{
	if (_core != NULL) {
		rwm_texture_free(_core);
	}
	[super dealloc];
}

- (struct rwa_texture *)mesaTexture { return rwm_texture_mesa(_core); }
- (struct rwm_texture *)redwoodCore { return _core; }

/* over a texture of the core's that is made already (src/metal/import.cpp), which it takes: freed with
 * this object, and at once when this object cannot be made */
- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device core:(struct rwm_texture *)core
{
	self = [super init];
	if (self == nil) {
		rwm_texture_free(core);
		return nil;
	}
	_core = core;
	_owner = device;
	return self;
}


/* (on the driver's own thread the copy was made at -commit already, unless rwm_copyin_late=1) */
- (void)redwoodCopyIn
{
	if (rw_on_worker() && !rwm_copyin_late()) {
		return;
	}
	[self redwoodCopyInNow];
}

- (void)redwoodCopyInNow
{
	rwm_copy_in(_core);     /* (src/metal/run.cpp: the copy, counted in rwm_perf) */
}


- (id)device                       { return _owner; }
- (NSUInteger)width                { return rwm_texture_shape(_core)->width; }
- (NSUInteger)height               { return rwm_texture_shape(_core)->height; }
- (NSUInteger)depth                { return rwm_texture_shape(_core)->depth; }
- (NSUInteger)mipmapLevelCount     { return rwm_texture_shape(_core)->mip_levels; }
- (NSUInteger)arrayLength          { return rwm_texture_shape(_core)->array_length; }
- (NSUInteger)sampleCount          { return rwm_texture_shape(_core)->sample_count; }
- (NSUInteger)pixelFormat          { return rwm_texture_shape(_core)->pixel_format; }
- (NSUInteger)textureType          { return rwm_texture_shape(_core)->type; }
- (NSUInteger)usage                { return rwm_texture_shape(_core)->usage; }
- (BOOL)isFramebufferOnly          { return NO; }
- (BOOL)allowGPUOptimizedContents  { return NO; }
- (id)parentTexture                { return nil; }
- (NSUInteger)parentRelativeLevel  { return 0; }
- (NSUInteger)parentRelativeSlice  { return 0; }
- (id)buffer                       { return nil; }
- (NSUInteger)bufferOffset         { return 0; }
- (NSUInteger)bufferBytesPerRow    { return 0; }
- (id)iosurface                    { return nil; }
- (NSUInteger)iosurfacePlane       { return 0; }
/* MTLStorageModeShared, except for a multisample texture, which Metal does not allow to be
 * one: "MTLTextureDescriptor: Multisample textures cannot be allocated with
 * MTLStorageModeShared on this device" is Metal's own sentence, in the framework's own
 * __cstring and not in the validation layer. Answering Private for one is not bookkeeping
 * -- Shared is what makes -replaceRegion: and -getBytes: legal, and those are exactly the
 * two calls the compiler library refuses on a multisample texture (and refuses because
 * neither side can give the samples of a texel a defined order through a map). So the
 * storage mode this reports and the calls that work on it say the same thing. */
- (NSUInteger)storageMode
{
	return rwm_texture_shape(_core)->sample_count > 1 ? 2 : 0;   /* MTLStorageModePrivate : Shared */
}
- (NSUInteger)cpuCacheMode         { return 1; }
- (NSUInteger)resourceOptions      { return 0; }
- (NSUInteger)hazardTrackingMode   { return 0; }
- (NSUInteger)allocatedSize        { return 0; }   /* not accounted for yet, and says so */
/* MTLPurgeableStateNonVolatile, for the reason written over the buffer's own: a texture of
 * the compiler library's is held until it is destroyed, so there is no volatile state to
 * report and zero was not a state at all. */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }
- (id)heap                         { return nil; }
- (NSUInteger)heapOffset           { return 0; }
- (BOOL)isAliasable                { return NO; }
- (void)makeAliasable              { }

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
		slice:(NSUInteger)slice
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
	bytesPerImage:(NSUInteger)image
{
	const unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
				     (unsigned)region.origin.z };
	const unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
				   (unsigned)region.size.depth };

	(void)image;
	rwm_texture_write(_core, (unsigned)level, (unsigned)slice, origin, size, bytes, stride);
}

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
{
	[self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:stride
	      bytesPerImage:0];
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
   bytesPerImage:(NSUInteger)image
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
	   slice:(NSUInteger)slice
{
	const unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
				     (unsigned)region.origin.z };
	const unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
				   (unsigned)region.size.depth };

	(void)image;
	rwm_texture_read(_core, (unsigned)level, (unsigned)slice, origin, size, bytes, stride);
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:stride bytesPerImage:0 fromRegion:region
	   mipmapLevel:level slice:0];
}

- (BOOL)redwoodReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region
{
	const unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
				     (unsigned)region.origin.z };
	const unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
				   (unsigned)region.size.depth };

	return rwm_texture_read_depth(_core, origin, size, depth, stencil);
}

/* A view of a texture is another texture over the same memory, and the compiler library has
 * no call for that yet; nil is the honest answer, and it is better than a view that aliases
 * nothing. */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format { (void)format; return nil; }

/*
 * THE REST OF WHAT <MTLTexture>, <MTLTextureSPI>, <MTLResource> AND <MTLResourceSPI>
 * REQUIRE: 27 methods with no answer anywhere in this chain before this section. The same
 * three kinds as everywhere else -- served, true by construction, or refused out loud.
 *
 * The two view calls are the only ones here that hand nothing back, and they say so: a view
 * is a second texture over the first one's memory, and rwa has no call that makes one. A
 * view that aliased nothing would be found much later, by a picture that did not change.
 */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
{
	(void)format; (void)type; (void)levels; (void)slices;
	RW_ONCE("a texture view is not implemented (the compiler library has no "
		"call that makes a second texture over the same memory); nil\n");
	return nil;
}
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
			    swizzle:(MTLTextureSwizzleChannels)swizzle
{
	(void)swizzle;
	return [self newTextureViewWithPixelFormat:format textureType:type levels:levels
					    slices:slices];
}
- (id)newSharedTextureHandle { return nil; }   /* nothing here is shared between processes */

/* Served out of the shape this texture was made with, which is the only place that
 * knowledge exists: a cube is six faces (MTLTextureTypeCube is 3, CubeArray is 4),
 * everything else is one. */
- (NSUInteger)numFaces
{
	return (rwm_texture_shape(_core)->type == 3 || rwm_texture_shape(_core)->type == 4) ? 6 : 1;
}
/* The identity swizzle, and it is the truth rather than a default: a texture of this driver
 * is made from a descriptor's format alone and nothing reorders its channels, so red reads
 * as red. MTLTextureSwizzleChannelsDefault is that same quadruple. */
- (MTLTextureSwizzleChannels)swizzle
{
	return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
					     MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
}
- (unsigned int)swizzleKey { return 0; }   /* the key of "nothing is swizzled" */
/* Nothing here is a view, so every texture is its own root, and nil is what Apple's own
 * resource answers for one that was allocated rather than derived. */
- (id)rootResource { return nil; }

/*
 * True by construction, and each is a sentence about this driver rather than a shrug:
 *
 *   - nothing aliases: each texture is its own allocation of the compiler library's, there
 *     are no heaps, and -makeAliasable does nothing, so "does this overlap that" is NO for
 *     every pair, for certain;
 *   - nothing is compressed: the library allocates surfaces with no lossless-compression
 *     metadata, so there is no compression and nothing to feed back;
 *   - nothing is sparse: there is no tail, so its size and its first mipmap are zero;
 *   - nothing is a drawable: a texture of this driver never comes from a CAMetalLayer, it is
 *     the attachment a pass draws into and -getBytes: reads back;
 *   - nothing is purgeable or incomplete, for the reason written over the buffer's pair.
 */
- (BOOL)doesAliasResource:(id)resource { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)isCompressed             { return NO; }
- (NSInteger)compressionFeedback { return 0; }
- (BOOL)isSparse                 { return NO; }
- (NSUInteger)firstMipmapInTail  { return 0; }
- (NSUInteger)tailSizeInBytes    { return 0; }
- (BOOL)isShareable              { return NO; }
- (BOOL)isDrawable               { return NO; }
- (BOOL)isComplete               { return YES; }
- (void)waitUntilComplete        { }
- (BOOL)isPurgeable              { return NO; }
- (NSUInteger)protectionOptions  { return 0; }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Zero is "not rotated": this texture is not a rotated view of a display surface, which is
 * the only thing in Metal that sets this. */
- (NSUInteger)rotation { return 0; }
/*
 * The identifier an argument buffer would hold for this texture. This driver has no
 * argument buffers -- the device answers no to every tier of them -- and the compiler
 * library hands out no bindless handle, so there is no identifier to give; zero is the one
 * value that cannot be mistaken for a resource, and it is said once because a caller that
 * asks is a caller that was about to write it into a buffer a shader reads.
 */
- (MTLResourceID)gpuResourceID
{
	MTLResourceID resourceID = { 0 };

	RW_ONCE("a GPU resource ID was asked of a texture; this driver has no "
		"argument buffers and no bindless handles, so it answers zero\n");
	return resourceID;
}
- (NSUInteger)gpuHandle { return 0; }
/* Whose allocation this is for the kernel's accounting. We never tell the kernel, so the
 * true answer is this process until somebody says otherwise, and then it is what they said. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }

@end

@implementation RedwoodMetalSampler

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			      sampler:(struct rwa_sampler *)sampler
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilySampler sends to super; the descriptor is passed on
	 * because the base keeps it and the pipeline-state assertions taught us not to send
	 * nil to one of these */
	self = [super initWithDevice:device samplerDescriptor:descriptor];
	if (self == nil) {
		mesa.sampler_destroy(sampler);
		return nil;
	}
	_sampler = sampler;
	return self;
}

- (void)dealloc
{
	if (_sampler != NULL) {
		mesa.sampler_destroy(_sampler);
	}
	[super dealloc];
}

- (struct rwa_sampler *)mesaSampler { return _sampler; }

@end

@implementation RedwoodMetalDepthStencilState

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor
{
	/* The base class's own designated initialiser, which validates the descriptor,
	 * sends `init` to super and fills _device and _label (0x181a29b38). The descriptor
	 * is handed on for the same reason the sampler's is: the base reads it, and a nil
	 * there is reported by Apple's own validation. */
	self = [super initWithDevice:device depthStencilDescriptor:descriptor];
	if (self == nil) {
		mesa.depth_stencil_destroy(state);
		return nil;
	}
	_state = state;

	/*
	 * The four answers <MTLDepthStencilStateSPI> requires and the base class gives as
	 * NO (nil-stubs at 0x181b1b5e0 and its neighbours). They are the only methods of
	 * this object that are a statement about what it will do, so they are derived from
	 * the descriptor rather than left at a default that would be false for every state
	 * that writes anything.
	 *
	 * "Reads" means the test consults the buffer: MTLCompareFunctionAlways consults
	 * nothing, anything else does. For the stencil an operation other than Keep also
	 * reads it, because incrementing or inverting a value means fetching it first.
	 */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };

	_writesDepth = [descriptor isDepthWriteEnabled];
	_readsDepth = [descriptor depthCompareFunction] != MTLCompareFunctionAlways;
	for (unsigned i = 0; i < 2; i++) {
		MTLStencilDescriptor *f = faces[i];
		BOOL keeps;

		if (f == nil) {
			continue;   /* null_resettable, so this is Metal's default: no stencil */
		}
		keeps = [f stencilFailureOperation] == MTLStencilOperationKeep &&
			[f depthFailureOperation] == MTLStencilOperationKeep &&
			[f depthStencilPassOperation] == MTLStencilOperationKeep;
		if ([f stencilCompareFunction] != MTLCompareFunctionAlways || !keeps) {
			_readsStencil = YES;
		}
		if (!keeps && [f writeMask] != 0) {
			_writesStencil = YES;
		}
	}
	return self;
}

- (void)dealloc
{
	if (_state != NULL) {
		mesa.depth_stencil_destroy(_state);
	}
	[super dealloc];
}

- (struct rwa_depth_stencil *)mesaDepthStencil { return _state; }

- (BOOL)readsDepth    { return _readsDepth; }
- (BOOL)writesDepth   { return _writesDepth; }
- (BOOL)readsStencil  { return _readsStencil; }
- (BOOL)writesStencil { return _writesStencil; }

@end

@implementation RedwoodMetalComputePipeline

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor
{
	/* _MTLComputePipelineState's designated initialiser is the one AGX sends to super
	 * (0x2092477e0 -> 0x181a2e068). The descriptor must not be nil: the base asserts on
	 * that at its own line 1504, and the first version of this passed nil and was killed
	 * by it. */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		mesa.compute_destroy(compute);
		return nil;
	}
	_owner = device;
	_compute = compute;
	_info = *info;
	/* and the same for a kernel with a set, for the same reason */
	if (_info.argbuf_mask != 0) {
		RW_ONCE("a compute pipeline declares argument buffers at Metal buffer indices "
			"%#x; their members are bound from what an MTLArgumentEncoder "
			"recorded\n", _info.argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_compute != NULL) {
		mesa.compute_destroy(_compute);
	}
	[super dealloc];
}

- (struct rwa_compute *)compute
{
	return _compute;
}

/* The three the base class answers with nothing at all (docs/metal.md 10.8), so they can
 * only come from whoever compiled the shader -- which is why the contract carries them. */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _info.max_threads_per_group; }
- (NSUInteger)threadExecutionWidth          { return _info.thread_execution_width; }
- (NSUInteger)staticThreadgroupMemoryLength { return _info.static_shared_bytes; }

- (id)device { return _owner; }

@end

@implementation RedwoodMetalRenderPipeline

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			       render:(struct rwa_render *)render
				 info:(const struct rwa_render_info *)info
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilyRenderPipeline sends to super (0x181a294dc), and a real
	 * descriptor, for the reason the compute one learned at its own line 1504 */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		mesa.render_destroy(render);
		return nil;
	}
	_owner = device;
	_render = render;
	_info = *info;
	if (info->generated_vertex_stage) {
		rw_print("this pipeline's vertex stage was generated, so what it "
		         "draws is the fragment shader over the whole attachment\n");
	}
	/* The first pipeline of this process whose shaders declare an ARGUMENT BUFFER, named
	 * once: every measurement this port has of argument buffers is on the host, and this
	 * line is how a boot says a pipeline with a set was built at all. The indices matter
	 * because the draw binds nothing of that set out of the buffer table -- it binds what
	 * the encoder recorded for whatever buffer is bound there. */
	if (info->vertex_argbuf_mask != 0 || info->fragment_argbuf_mask != 0) {
		RW_ONCE("a render pipeline declares argument buffers: the vertex stage at "
			"Metal buffer indices %#x and the fragment stage at %#x. Their members "
			"are bound from what an MTLArgumentEncoder recorded, and a buffer "
			"bound there that nobody encoded is refused by name\n",
			info->vertex_argbuf_mask, info->fragment_argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_render != NULL) {
		mesa.render_destroy(_render);
	}
	[super dealloc];
}

- (struct rwa_render *)render { return _render; }
- (BOOL)writesDepth           { return _info.fragment_writes_depth != 0; }
- (unsigned)vertexStreamMask  { return _info.vertex_stream_mask; }
- (unsigned)vertexStreamCount { return _info.vertex_streams; }
- (unsigned)vertexElementCount { return _info.vertex_attribute_count; }
- (unsigned)vertexElementAttribute:(unsigned)index
{
	unsigned n = sizeof(_info.vertex_attribute_index) /
		     sizeof(_info.vertex_attribute_index[0]);

	/* ~0u and not 0 for an index past the end: attribute 0 is a real answer, and a
	 * caller that asked for element 5 of a pipeline with two would otherwise read the
	 * mapping as if it had five. */
	return index < _info.vertex_attribute_count && index < n
	    ? _info.vertex_attribute_index[index] : ~0u;
}
- (id)device                  { return _owner; }

/* The base class answers this one with `mov x0, #0; ret` (0x181b0eee0), and a zero would read
 * as "no threads"; the number is the one the compiler library reports for a compiled stage. */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (BOOL)threadgroupSizeMatchesTileSize      { return NO; }
- (NSUInteger)imageblockSampleLength        { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions
{
	(void)dimensions;
	return 0;
}

/* The last two of <MTLRenderPipelineStateSPI> that nothing in this chain answered. Shader
 * debug information is what Metal's own debugger reads out of a pipeline built with it;
 * there is none here, and nil is the answer Apple's driver gives for a pipeline that was
 * built without it, so the caller already has this path. */
- (id)newVertexShaderDebugInfo   { return nil; }
- (id)newFragmentShaderDebugInfo { return nil; }

@end

/*
 * What a recorded draw or dispatch names is held until the command buffer is gone, as Metal's
 * own command buffers hold it (retainedReferences): CoreAnimation lets go of a texture it has
 * drawn into once the encoder is ended, and leaving an application did so before -commit drew
 * into it -- objc_msgSend on freed memory in -runDraw:, and the window server gone with every
 * application (24 October).
 */
@implementation RedwoodMetalWork
- (void)retainResources
{
	for (unsigned i = 0; i < core.keep_count; i++) {
		[(id)core.keep[i] retain];
	}
	held = YES;
}

- (void)dealloc
{
	for (unsigned i = 0; held && i < core.keep_count; i++) {
		[(id)core.keep[i] release];
	}
	[super dealloc];
}
@end

/*
 * THE SLOT EVERY COMMAND BUFFER TAKES FROM THE QUEUE, AND WHY IT HAS TO BE GIVEN BACK.
 *
 * -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] -- the
 * designated initialiser this driver's command buffer calls, because docs/metal.md 10.13
 * says to call the real one -- does this, at 0x181a1ea14 and on:
 *
 *     ldr  x8, [x19, _MTLCommandBuffer._queue]
 *     ldr  x0, [x8, _MTLCommandQueue._commandBufferSemaphore]
 *     mov  x1, #-1                       ; DISPATCH_TIME_FOREVER
 *     bl   dispatch_semaphore_wait
 *
 * and just before it sets _MTLCommandBuffer._needsCommandBufferSemaphoreSignal to 1. The
 * semaphore is made in -[_MTLCommandQueue initWithDevice:descriptor:] (0x181a1d1fc), which
 * reads maxCommandBufferCount off the descriptor (0x181a1d268) and refuses zero; the
 * default for MTLCommandQueueDescriptor is 64. Every ivar name above is the cache's own
 * (`ipsw dyld a2s`: OBJC_IVAR_$__MTLCommandQueue._commandBufferSemaphore,
 * OBJC_IVAR_$__MTLCommandBuffer._needsCommandBufferSemaphoreSignal), not a guess.
 *
 * So each command buffer takes one of 64 slots AND OWES A SIGNAL -- that ivar is Apple's
 * own name for the debt. In Apple's driver something on the completion path pays it. This
 * driver has no completion path at all: it overrides -commit and -waitUntilCompleted and
 * never calls -enqueueCommandBuffer:, -commitCommandBuffer:wake: or any of the rest, so
 * nothing in Metal will ever pay it for us. The 65th -commandBuffer then waits for ever.
 *
 * MEASURED, not reasoned: logs/114823-run.log. A test that made 64 command buffers (one
 * per shader of an 8x8 mosaic) blocked on the 65th inside -[MTLCommandQueue commandBuffer]
 * and stayed blocked for four minutes, with a heartbeat thread proving the process alive.
 * It is not a test bug: an application makes a command buffer per frame, so this driver
 * would hang anything that drew for a couple of seconds.
 *
 * The debt is paid at -commit, and that is a choice with a reason. Metal holds the slot
 * until the buffer completes; here -commit IS the completion -- the recorded work runs
 * inside it and -waitUntilCompleted only fences -- so holding it any longer would throttle
 * an application for a wait this driver does not have. A buffer that is never committed
 * pays at -dealloc instead, which is the case Apple's flag exists for.
 *
 * Both are done through the runtime by ivar NAME, and a name that is not there is said out
 * loud rather than worked around: on an iOS whose Metal keeps this differently, the right
 * answer is to find out, not to run with a queue that silently blocks at 64.
 */
static dispatch_semaphore_t
redwood_queue_semaphore(id queue)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_commandBufferSemaphore")
				   : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no "
			         "_commandBufferSemaphore in this Metal, so the slot every "
			         "command buffer takes cannot be given back and this queue "
			         "will block on its maxCommandBufferCount'th buffer\n");
		}
	}
	return ivar != NULL ? (dispatch_semaphore_t)object_getIvar(queue, ivar) : nil;
}

/* Where a queue's completion queue lives: Apple's own _MTLCommandQueue._completionQueue,
 * found by NAME for the same reason the semaphore above is -- the base class already has
 * the getter for it, and a second copy in this file would mean a queue built from a
 * descriptor that carried one reading back as nil. A Metal without the ivar is said out
 * loud once and falls back to this driver's own field. */
static Ivar
redwood_completion_ivar(void)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_completionQueue") : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no _completionQueue in this "
			         "Metal, so -setCompletionQueue: keeps the queue in this driver's "
			         "own field instead of the one Apple's getter reads\n");
		}
	}
	return ivar;
}

/* Apple's own "this buffer still owes a signal" flag, cleared when we pay, so that its
 * bookkeeping and ours cannot disagree about a debt that has been settled. */
static void
redwood_clear_semaphore_debt(id commandBuffer)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandBuffer");

		looked = YES;
		ivar = base != Nil ?
		    class_getInstanceVariable(base, "_needsCommandBufferSemaphoreSignal") : NULL;
	}
	if (ivar != NULL) {
		*(unsigned char *)((char *)commandBuffer + ivar_getOffset(ivar)) = 0;
	}
}

@implementation RedwoodMetalCommandQueue {
	RedwoodMetalDevice *_owner;
	NSUInteger _outstanding;        /* buffers of this queue that still hold a slot */
	NSUInteger _made, _returned;
	/* What <MTLCommandQueueSPI> keeps about a queue and this driver cannot act on: the
	 * two priorities are remembered so that a caller reading back what it set is not
	 * lied to, and -setCompletionQueue: explains the third below. */
	NSUInteger _gpuPriority, _backgroundGPUPriority;
	dispatch_queue_t _completionQueue;   /* only used when Apple's own ivar is missing */
}

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device descriptor:(id)descriptor
{
	/* The initialiser IOGPUMetalCommandQueue itself sends to super (0x1ec16dabc). The
	 * descriptor is not optional: the base validates it at its own line 137 and the first
	 * version of this passed nil and was killed by that assertion. */
	self = [super initWithDevice:device descriptor:descriptor];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	/* Said at birth and once: this number is how many command buffers may exist at a
	 * time before -commandBuffer blocks, and until this driver returned the slot it was
	 * also how many this process could ever make. */
	rw_print("this queue takes %lu command buffers at a time "
	         "(maxCommandBufferCount), and each one waits on the queue's semaphore in "
	         "-[_MTLCommandBuffer initWithQueue:...]; this driver gives the slot back at "
	         "-commit%s\n", (unsigned long)[self maxCommandBufferCount],
	         redwood_queue_semaphore(self) != nil ? "" :
	         ", EXCEPT that the semaphore was not found and it cannot");
	return self;
}

- (id)device { return _owner; }

/* What the queue itself thinks, for a test that wants the number rather than the
 * inference: how many command buffers of this queue still hold one of those slots. */
- (NSUInteger)redwoodOutstandingCommandBuffers { return _outstanding; }
- (NSUInteger)redwoodCommandBuffersMade        { return _made; }

- (void)redwoodTookSlot
{
	_outstanding++;
	_made++;
}

- (void)redwoodReturnedSlot
{
	if (_outstanding > 0) {
		_outstanding--;
	}
	_returned++;
}

- (id)commandBuffer
{
	return [[[RedwoodMetalCommandBuffer alloc] initWithRedwoodQueue:self] autorelease];
}

- (id)commandBufferWithUnretainedReferences
{
	return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary { }

/*
 * THE TWO METHODS THE SYSTEM NAMED, AND WHAT THEY REALLY ARE.
 *
 * With metal=1 and launchd running, backboardd and mediaserverd sent this queue
 * -setGPUPriority: 58 times and -setCompletionQueue: 30 times in a hundred seconds, and
 * every one of them killed the daemon that sent it (logs/130132-run.log). Neither is in any
 * public header: both are required members of <MTLCommandQueueSPI>, which is declared in
 * AGXMetalA11.bundle and therefore does not exist in any process on this phone -- so
 * objc_getProtocol cannot be asked what this class owes, and the list was read out of the
 * cache instead (tools/metal/protometh.py, tools/metal/owes.py).
 *
 * -setGPUPriority: (B24@0:8Q16 -- NSUInteger in, BOOL out) is NOT bookkeeping.
 * -[IOGPUMetalCommandQueue setGPUPriority:] (0x1ec174cb8) calls _validateGPUPriority
 * (0x1ec178f74) and then -_setGPUPriority:backgroundPriority: (0x1ec178eec), which packs
 * the foreground and background numbers into a three-word request and hands it to the
 * kernel's own command-queue object (_IOGPUCommandQueueSetPriorityAndBackground); only if
 * the kernel accepts does it write the two ivars that -getGPUPriority reads back. So the
 * number is read by the GPU's scheduler, and the ivar is just a cache of what the scheduler
 * agreed to. The validation is worth copying exactly: anything above 5 is refused and 3 is
 * refused for a foreground queue -- "priority (%d) is not valid for foreground" is the
 * cache's own string -- and it only REPORTS, it does not stop the call, which is why
 * Apple's BOOL is the kernel's answer and not the validator's.
 *
 * This driver has one ring and one priority. Every command buffer goes to the kernel's
 * single msm ring in the order it was committed, and there is no second queue to be ahead
 * of; so there is nothing here to tell, and the honest answer is to remember the number,
 * report it back, and say once that it changes nothing.
 */
- (BOOL)setGPUPriority:(NSUInteger)priority
{
	if (priority > 5 || priority == 3) {
		RW_ONCE("GPU priority %lu is outside what Apple's own "
			"_validateGPUPriority allows (0..5, and 3 is the background-only "
			"one); it is remembered as asked, like Apple's own path, which "
			"reports such a value and carries on\n", (unsigned long)priority);
	}
	RW_ONCE("a GPU priority of %lu was asked for; this driver has one ring "
		"and one priority, so the number is remembered and reported back but nothing "
		"schedules by it\n", (unsigned long)priority);
	_gpuPriority = priority;
	return YES;
}

/* The offset is a place WITHIN a priority band (Apple's second argument is an unsigned
 * short, S24 in the encoding). With one ring there is no band to be placed in. */
- (BOOL)setGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	if (offset != 0) {
		RW_ONCE("a GPU priority offset of %u is ignored: this driver has "
			"one ring, so a place inside a priority band means nothing here\n",
			(unsigned)offset);
	}
	return [self setGPUPriority:priority];
}

/* The background priority is the one a queue gets while its process is not frontmost. Same
 * answer, kept separately so that the two reads do not lie about each other. */
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
	RW_ONCE("a background GPU priority of %lu was asked for; remembered, "
		"not honoured, for the same reason as the foreground one\n",
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

/*
 * -setCompletionQueue: (v24@0:8@16) names the dispatch queue on which this command queue
 * delivers what finishes. -[IOGPUMetalCommandQueue setCompletionQueue:] (0x1ec178e88)
 * releases the queue it held, retains the new one into _MTLCommandQueue._completionQueue --
 * the ivar the base class already has a getter for -- and re-targets the queue's own
 * dispatch source at it (0x1ec1765e8), so that every command buffer's completion handler
 * runs there instead of on whichever thread the kernel's notification arrived on. That is
 * exactly why backboardd sets one: a completion handler that runs on a daemon's render
 * thread is a completion handler that can stall a frame.
 *
 * So this one is SERVED, not declined: the queue is kept, and -[RedwoodMetalCommandBuffer
 * commit] delivers the scheduled and completed handlers on it (see the note there -- before
 * this they ran inline, at the moment they were added, which is before the work they are
 * about). It is kept in Apple's ivar rather than in one of ours so that a queue built from
 * a descriptor that already carried a completion queue (there is a
 * -[MTLCommandQueueDescriptorInternal setCompletionQueue:]) reads back as what it is; the
 * retain is balanced by -[_MTLCommandQueue dealloc], the ivar's owner.
 */
- (void)setCompletionQueue:(dispatch_queue_t)queue
{
	Ivar ivar = redwood_completion_ivar();
	dispatch_queue_t held = ivar != NULL ?
	    (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;

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

/* What this driver reads when it has something to deliver. Not an override of -completionQueue
 * (the base class's getter reads the same ivar): this is here for the Metal where that ivar
 * is not, so that one missing ivar does not quietly send every handler to the wrong thread. */
- (dispatch_queue_t)redwoodCompletionQueue
{
	Ivar ivar = redwood_completion_ivar();

	return ivar != NULL ? (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;
}

- (void)dealloc
{
	/* Only the copy this driver keeps itself: the one in Apple's ivar belongs to
	 * -[_MTLCommandQueue dealloc], and releasing it twice would be worse than leaking
	 * one dispatch queue in the Metal where that dealloc does not. */
	[_completionQueue release];
	[super dealloc];
}

@end

/*
 * MTLCommandBufferStatus, AND WHAT A LOG FROM BEFORE THIS CHANGE SAYS.
 *
 * These are Metal's own numbers (MTLCommandBuffer.h): NotEnqueued 0, Enqueued 1, Committed 2,
 * Scheduled 3, Completed 4, Error 5. -[MTLCommandBuffer status] is how real code decides that a
 * buffer is done, so they are not cosmetic.
 *
 * Until 2026-10-21 this file answered 3 for committed, 4 for scheduled and 6 for completed -- and
 * 6 is not a status at all. Under those numbers an application comparing with
 * MTLCommandBufferStatusCompleted (4) saw completion the moment -commit returned, BEFORE the GPU
 * fence of -waitUntilCompleted was paid, and after the fence it saw a number Metal does not
 * define. Error was 5 by coincidence and has not moved.
 *
 * Every boot log under logs/ was written by the old numbering and none of them is going to be
 * regenerated, so those numbers have to be DATED rather than reinterpreted. That is what the
 * banner in -initWithRedwoodQueue: below is for: it names the numbering of the log that carries
 * it, and it is printed out of these constants, so the line cannot drift from what -status
 * answers. A log without that line is older than this change, and in it "status 6" is this
 * completion, "status 4" is a buffer that was only submitted, and 2 and 3 do not occur at all.
 * That is the rule tools/air/metalcheck.py reads a log by, and the measurement that makes it
 * safe: over all 141 logs in logs/ the only statuses the test ever printed are 5 (128 times) and
 * 6 (990 times) -- never 4.
 *
 * One file more keys on the old numbers and is NOT part of this change: userland/src/
 * metal_drv_test.m compares [cmd status] with the literal 6 in five places (its lines 1200, 1526,
 * 2041, 2293, 2795) and prints the literal "status 6" in a sixth (2308). Until those move, a boot
 * with this driver has the test calling every completed buffer a failure: the sweep prints
 * "drvtest: drawall <name> status 4" with nothing after it and reads back no pixel, and the single
 * draw returns before its own readback. metalcheck.py names that shape when it sees it.
 */
/* (the numbers are src/metal/rwm.h's RWM_CB_*) */

@implementation RedwoodMetalCommandBuffer {
	RedwoodMetalCommandQueue *_queue;
	NSMutableArray *_work;
	NSError *_error;
	BOOL _owesSlot;                 /* this buffer holds one of the queue's slots */
	struct rwm_command _core;       /* its status, its times, its GPU job, its surfaces: src/metal/cmdbuf.cpp */
	NSMutableArray *_scheduled, *_completed;   /* handler blocks, copied to the heap */
	NSUInteger _protectionOptions;  /* what the window server asked for; see -setProtectionOptions: */
}

/*
 * The window server sends this to every command buffer it makes, and until it was answered the
 * boot of 22 October 01:51 ended it 78 times with
 *
 *   NSInvalidArgumentException: -[RedwoodMetalCommandBuffer setProtectionOptions:]:
 *   unrecognized selector sent to instance
 *
 * -- an uncaught Objective-C exception out of CA::WindowServer::IOMFBServer::thread_body, which
 * is terminate() and not a refusal we could read.
 *
 * It was missed by owes.py for a reason worth keeping: that tool counts what the PROTOCOLS
 * require, and this pair is declared on a CLASS -- IOGPUMetalCommandBuffer, whose own
 * _protectionOptions ivar it is. Our command buffer descends from _MTLCommandBuffer and not
 * from IOGPU's, so nothing in the chain answers it and nothing in the protocol lists says it is
 * missing. Every selector Apple's own counterpart class declares is a selector a caller may
 * send, and the protocols are only part of that list.
 *
 * The value is kept and not honoured. Protected content means memory the GPU can read and the
 * processor cannot, which this port has nothing to provide: a buffer of ours is ordinary memory
 * on both sides. So a non-zero request says so once rather than being refused -- the window
 * server asks for it on buffers that carry no protected content at all, and refusing would stop
 * a composite that is in no danger.
 */
- (void)setProtectionOptions:(NSUInteger)options
{
	if (options != 0 && _protectionOptions != options) {
		RW_ONCE("a command buffer was asked for protection options %lu, and this "
			"driver has no protected memory to give: the request is kept and not honoured, "
			"because nothing it would protect is protected here either\n",
			(unsigned long)options);
	}
	_protectionOptions = options;
}

- (NSUInteger)protectionOptions
{
	return _protectionOptions;
}

- (instancetype)initWithRedwoodQueue:(RedwoodMetalCommandQueue *)queue
{
	/* what IOGPUMetalCommandBuffer sends to super, retained references and no debug mode.
	 * THIS CALL BLOCKS when the queue's semaphore is empty -- see the long note above
	 * RedwoodMetalCommandQueue -- so by the time it returns, a slot is held and owed. */
	self = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];
	if (self == nil) {
		return nil;
	}
	_queue = [queue retain];
	_work = [[NSMutableArray alloc] init];
	rwm_command_begin(&_core);
	/* The numbering this log is written in, said once per process and printed out of the
	 * constants that answer -status: a log that does NOT carry this line was written before
	 * the numbers became Metal's, and says committed 3, scheduled 4, completed 6 (see the
	 * note above this class). */
	RW_ONCE("MTLCommandBufferStatus: committed %u, scheduled %u, completed %u, "
		"error %u -- Metal's own numbers; a log without this line was written by the old "
		"ones (committed 3, scheduled 4, completed 6)\n",
		(unsigned)RWM_CB_COMMITTED, (unsigned)RWM_CB_SCHEDULED,
		(unsigned)RWM_CB_COMPLETED, (unsigned)RWM_CB_ERROR);
	_owesSlot = YES;
	[queue redwoodTookSlot];
	return self;
}

/*
 * Give the queue's slot back, exactly once. Called at the end of -commit, where this
 * driver's work is already done, and at -dealloc for a buffer that was never committed --
 * which is the case Apple's _needsCommandBufferSemaphoreSignal exists for and the one that
 * would otherwise lose a slot for the life of the process.
 */
- (void)redwoodReturnSlot
{
	dispatch_semaphore_t semaphore;

	if (!_owesSlot) {
		return;
	}
	_owesSlot = NO;
	redwood_clear_semaphore_debt(self);
	[_queue redwoodReturnedSlot];
	semaphore = redwood_queue_semaphore(_queue);
	if (semaphore != nil) {
		dispatch_semaphore_signal(semaphore);
	}
}

- (void)dealloc
{
	[self redwoodReturnSlot];
	[self redwoodReleaseSampled];
	[_work release];
	[_error release];
	[_scheduled release];
	[_completed release];
	[_queue release];
	[super dealloc];
}

- (id)device       { return [_queue device]; }
- (id)commandQueue { return _queue; }
- (NSUInteger)status { return _core.status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }

- (id)computeCommandEncoder
{
	return [[[RedwoodMetalComputeEncoder alloc] initWithRedwoodCommandBuffer:self]
		   autorelease];
}

- (id)renderCommandEncoderWithDescriptor:(id)descriptor
{
	if (descriptor == nil) {
		rw_print("a render pass needs a descriptor\n");
		return nil;
	}
	return [[[RedwoodMetalRenderEncoder alloc] initWithRedwoodCommandBuffer:self
								    descriptor:descriptor]
		   autorelease];
}

- (id)parallelRenderCommandEncoderWithDescriptor:(id)descriptor
{
	(void)descriptor;
	return nil;   /* one encoder at a time here; a parallel pass needs the split */
}

/* The rest of what MTLCommandBuffer requires of a driver. Each declines rather than being
 * absent, for the reason the device's own 44 were answered: a missing selector aborts the
 * caller, and these are the four that were still missing. */
- (id)blitCommandEncoder
{
	static int told;

	if (told++ < 6) {
		char line[80];

		snprintf(line, sizeof(line), "!rwm blit encoder made (%d) in %s", told, getprogname());
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}
	return [[[RedwoodMetalBlitEncoder alloc] initWithRedwoodCommandBuffer:self] autorelease];
}
- (id)resourceStateCommandEncoder { return nil; }
- (id)accelerationStructureCommandEncoder { return nil; }
- (void)encodeSignalEvent:(id)event value:(uint64_t)value
{
	RW_ONCE("NOT DONE: -encodeSignalEvent:%s value:%llu\n", class_getName(object_getClass(event)),
	         (unsigned long long)value);
}
- (void)encodeWaitForEvent:(id)event value:(uint64_t)value
{
	RW_ONCE("NOT DONE: -encodeWaitForEvent:%s value:%llu\n", class_getName(object_getClass(event)),
	         (unsigned long long)value);
}

- (id)computeCommandEncoderWithDispatchType:(NSUInteger)type
{
	(void)type;   /* serial and concurrent dispatch are the same here: one queue, in order */
	return [self computeCommandEncoder];
}

- (void)recordWork:(RedwoodMetalWork *)work
{
	[work retainResources];
	[_work addObject:work];
}

- (void)enqueue { }

/* Metal's own rule, kept: the recorded dispatches run here and not when they were encoded. */
/* (the driver's own thread, its queue and its waits are src/metal/worker.cpp's: rwm_worker_*) */

/* a pipeline's label, for the core's "first drawn" line (NULL: none) */
static const char *
rw_label_of(void *pipeline)
{
	return [[(id)pipeline label] UTF8String];
}

/* a command buffer's works as the core's, in order: in `stack` (64) when they fit, else in memory of their own that the
 * caller frees (none when there is none to be had: then no work, as before) */
static struct rwm_work **
rw_works_of(NSArray *list, struct rwm_work **stack, unsigned *count)
{
	NSUInteger total = [list count];
	struct rwm_work **works = total <= 64 ? stack : malloc(total * sizeof(*works));
	unsigned n = 0;

	for (RedwoodMetalWork *work in list) {
		if (works != NULL) {
			works[n++] = &work->core;
		}
	}
	*count = n;
	return works != NULL ? works : stack;
}

/* once the GPU has done with them (-redwoodDeliverHandlers), or the buffer goes without being run */
- (void)redwoodReleaseSampled
{
	rwm_sampled_release(&_core.sampled);
}

/* a command buffer on the driver's thread: its work, inside its own autorelease pool, then let go */
static void
rw_commit_job(void *context)
{
	RedwoodMetalCommandBuffer *buffer = context;

	@autoreleasepool {
		[buffer redwoodCommitWorkFromThread];
	}
	[buffer release];
}

- (void)commit
{
	struct rwm_work *stack[64];
	unsigned n;
	struct rwm_work **works = rw_works_of(_work, stack, &n);
	BOOL to_worker = rwm_command_commit(&_core, works, n, rw_label_of);     /* src/metal/cmdbuf.cpp */

	if (works != stack) {
		free(works);
	}
	if (!to_worker) {
		/* the draws compile too (panfrost builds a shader's variants when it is bound), so the
		   whole of the work runs where rw_on_big_stack puts it */
		rw_on_big_stack(^{
			[self redwoodCommitWork];
		});
		return;
	}
	/* to the driver's thread (src/metal/worker.cpp), kept alive until it is done there */
	[self retain];
	rwm_worker_submit(rw_commit_job, self);
}

/* on the driver's thread: the work, then the surfaces' last job for the display */
- (void)redwoodCommitWorkFromThread
{
	[self redwoodCommitWork];
	rwm_command_processed(&_core);
}

/* until the driver's thread has done this buffer's work (-commit) */
- (void)redwoodWaitProcessed
{
	rwm_command_wait_processed(&_core);
}


- (void)redwoodCommitWork
{
	char why[512] = { 0 };
	struct rwm_work *stack[64];
	unsigned n;
	struct rwm_work **works = rw_works_of(_work, stack, &n);
	int ran = rwm_command_run(&_core, [_queue device] != nil ? [[_queue device] mesaDevice] : NULL, works, n, why,
				  sizeof(why));        /* src/metal/cmdbuf.cpp */

	if (works != stack) {
		free(works);
	}
	if (ran == RWM_RUN_DRAW_FAILED) {
		[self failWith:why];
	} else if (ran == RWM_RUN_DISPATCH_FAILED) {
		_error = [[NSError errorWithDomain:@"RedwoodMetal" code:2
					  userInfo:@{ NSLocalizedDescriptionKey:
						      [NSString stringWithUTF8String:why] }]
			     retain];
	}
	/* The queue's slot goes back here and on every way out of this method, a failed draw
	 * included: a buffer that errored is as finished as one that drew, and a slot kept
	 * because of an error would turn one bad draw into a queue that blocks for ever. */
	[self redwoodReturnSlot];
	/* and only then the handlers, so that a handler which makes the next command buffer
	 * is not waiting for a slot this one still holds */
	[self redwoodDeliverHandlers];
}

/* the first 12 copies and clears of a process, said: which kind, how big, from and into what */
static struct rwm_texture *rw_core_of(id texture);

static void
rw_say_blit(unsigned kind, const unsigned size[3], id from, id to)
{
	static int told;

	if (told++ < 12) {
		char line[160];
		struct rwm_texture *core = rw_core_of(to);

		snprintf(line, sizeof(line), "!rwm blit %u: %ux%u from %s to %s (surface %u)", kind, size[0], size[1],
		         object_getClassName(from), object_getClassName(to), core != NULL ? rwm_texture_in_place(core) : 0);
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}
}

/* a texture's core, which the encoders hand the core with each texture bound (NULL: not one of ours) */
static struct rwm_texture *
rw_core_of(id texture)
{
	return [texture isKindOfClass:[RedwoodMetalTexture class]] ? [(RedwoodMetalTexture *)texture redwoodCore] : NULL;
}

- (void)failWith:(const char *)reason
{
	_error = [[NSError errorWithDomain:@"RedwoodMetal" code:4
				  userInfo:@{ NSLocalizedDescriptionKey:
					      [NSString stringWithUTF8String:reason] }] retain];
	_core.status = RWM_CB_ERROR;
}

/* (the fence, and why it is the whole point: src/metal/cmdbuf.cpp) */
- (void)waitUntilCompleted
{
	char why[512] = { 0 };

	if (rwm_command_wait_completed(&_core, [[_queue device] mesaDevice], why, sizeof(why)) != 0) {
		_error = [[NSError errorWithDomain:@"RedwoodMetal" code:3
					  userInfo:@{ NSLocalizedDescriptionKey:
						      [NSString stringWithUTF8String:why] }] retain];
	}
}

- (void)waitUntilScheduled
{
	rwm_command_wait_scheduled(&_core);     /* (src/metal/cmdbuf.cpp says why it does not wait for the worker) */
}

/*
 * HANDLERS, AND WHERE THEY RUN.
 *
 * Both of these used to call the block the moment it was added -- which is before the work
 * the block is about, on whichever thread encoded it. That was defensible while the only
 * caller was this port's own test; it is not once backboardd is the caller, and it is the
 * other half of why the system sends -[MTLCommandQueueSPI setCompletionQueue:]: a daemon
 * says WHERE its completion handlers may run precisely because it must not have them on its
 * own thread.
 *
 * So handlers are kept and delivered where the work happens: -commit. A scheduled handler
 * is delivered as soon as the recorded work has been submitted, a completed handler after
 * the GPU fence has been paid for -- the fence is only taken when somebody is actually
 * waiting on one, because this driver's -commit is otherwise asynchronous and a fence
 * nobody asked for would cost every frame. A handler added after the fact is delivered at
 * once rather than dropped: Metal calls that undefined, and losing it would be the kind of
 * silence this driver is trying to stop having.
 */
- (void)redwoodDeliver:(void (^)(id))handler
{
	dispatch_queue_t queue = [_queue redwoodCompletionQueue];

	if (queue != nil) {
		/* dispatch_async copies the block, which retains both this buffer and the
		 * handler it carries, so the buffer outlives its own completion. */
		dispatch_async(queue, ^{ handler(self); });
		return;
	}
	handler(self);
}

- (void)addScheduledHandler:(void (^)(id))handler
{
	if (handler == NULL) {
		return;
	}
	/* scheduled or further, an error included (3, 4, 5): the work this handler is about has
	 * already run, so it is delivered rather than kept */
	if (_core.status >= RWM_CB_SCHEDULED) {
		[self redwoodDeliver:handler];
		return;
	}
	if (_scheduled == nil) {
		_scheduled = [[NSMutableArray alloc] init];
	}
	[_scheduled addObject:[[handler copy] autorelease]];
}

- (void)addCompletedHandler:(void (^)(id))handler
{
	if (handler == NULL) {
		return;
	}
	if (_core.status >= RWM_CB_SCHEDULED) {
		[self redwoodDeliver:handler];
		return;
	}
	if (_completed == nil) {
		_completed = [[NSMutableArray alloc] init];
	}
	[_completed addObject:[[handler copy] autorelease]];
}

/* Called from the one place the work is done. The order is Metal's: scheduled first, then
 * the fence, then completed. */
- (void)redwoodDeliverHandlers
{
	NSArray *scheduled = _scheduled;
	NSArray *completed = _completed;

	_scheduled = nil;
	_completed = nil;
	for (id handler in scheduled) {
		[self redwoodDeliver:(void (^)(id))handler];
	}
	if (_core.seq != 0 && _core.status != RWM_CB_ERROR) {
		/* the GPU still drawing: the completed handlers when it is done, from a queue of
		 * their own and in the order the buffers were committed (one job queue, in order) */
		static dispatch_queue_t completions;
		static dispatch_once_t once;

		dispatch_once(&once, ^{
			completions = dispatch_queue_create("redwood.metal.completed", DISPATCH_QUEUE_SERIAL);
		});
		[self retain];
		[scheduled release];
		dispatch_async(completions, ^{
			[self waitUntilCompleted];
			[self redwoodReleaseSampled];
			for (id handler in completed) {
				[self redwoodDeliver:(void (^)(id))handler];
			}
			[completed release];
			[self release];
		});
		return;
	}
	if ([completed count] > 0 && _core.status != RWM_CB_ERROR) {
		/* A completed handler is a promise about the GPU, so it is made true before it
		 * is kept: -waitUntilCompleted is this driver's fence and sets the status the
		 * handler will read. */
		[self waitUntilCompleted];
	}
	/* (no job left to wait for: the GPU finished it in -redwoodCommitWork, or it failed) */
	[self redwoodReleaseSampled];
	for (id handler in completed) {
		[self redwoodDeliver:(void (^)(id))handler];
	}
	[scheduled release];
	[completed release];
}

/* -commitWithDeadline: is an abstract entry of _MTLCommandBuffer, so inheriting it aborts
 * rather than defaulting. The deadline is for work that is worthless if it is late (a frame
 * that misses its vsync); Apple's drivers hand it to the GPU's scheduler. There is one
 * scheduler here and one priority, so the deadline cannot be honoured -- but the COMMIT
 * must happen, and a deadline that is merely unmet is a late frame while a commit that
 * never happens is a daemon waiting for ever. */
- (void)commitWithDeadline:(NSUInteger)deadline
{
	(void)deadline;
	RW_ONCE("a command buffer was committed with a deadline; this driver "
		"has one ring and no scheduler to tell, so the work is committed and the "
		"deadline is not promised\n");
	[self commit];
}

- (void)addPurgedResource:(id)resource { (void)resource; [self redwoodPurgedNote]; }
- (void)addPurgedHeap:(id)heap         { (void)heap; [self redwoodPurgedNote]; }

/* Metal records on a command buffer every resource of its that was purged while the buffer
 * referenced it, so that -commit can fail the buffer instead of reading pages that are
 * gone. Nothing of this driver's is ever volatile -- -setPurgeableState: answers
 * MTLPurgeableStateNonVolatile for every resource, which is what they are -- so a report
 * here means Metal and this driver disagree about a resource, and that is worth a line
 * rather than a silent buffer that fails later for no stated reason. */
- (void)redwoodPurgedNote
{
	RW_ONCE("Metal reported a purged resource on a command buffer, but "
		"nothing this driver allocates is volatile; the report is noted and the "
		"buffer is not failed\n");
}


@end

@implementation RedwoodMetalRenderEncoder {
	RedwoodMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder */
	struct rwm_render_state _state;              /* the pass and what is bound: src/metal/render.cpp */
}

- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer
				  descriptor:(MTLRenderPassDescriptor *)descriptor
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	rwm_render_begin(&_state, [[commandBuffer device] mesaDevice]);

	/* what the pass draws into, whether it begins by clearing, and a resolve it cannot do (the core says why) */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		id texture = attachment != nil ? [attachment texture] : nil;
		MTLClearColor c;

		if (texture == nil) {
			continue;
		}
		c = [attachment clearColor];
		rwm_render_color(&_state, i, texture, rw_core_of(texture), [attachment loadAction] == MTLLoadActionClear,
				 (const double[4]){ c.red, c.green, c.blue, c.alpha }, [attachment storeAction],
				 [attachment resolveTexture] != nil);
	}
	/* the depth/stencil attachment: two descriptors over one texture, each aspect its own load action */
	MTLRenderPassDepthAttachmentDescriptor *za = [descriptor depthAttachment];
	MTLRenderPassStencilAttachmentDescriptor *sa = [descriptor stencilAttachment];
	id ztex = [za texture];
	id stex = [sa texture];

	rwm_render_depth(&_state, ztex, rw_core_of(ztex), [za loadAction] == MTLLoadActionClear, [za clearDepth], stex,
			 rw_core_of(stex), [sa loadAction] == MTLLoadActionClear, (unsigned)[sa clearStencil]);
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setRenderPipelineState:(id)state
{
	RedwoodMetalRenderPipeline *p = state;

	rwm_render_set_pipeline(&_state, p, [p render], [p vertexStreamMask], [p vertexStreamCount], [p writesDepth]);
}

/*
 * One table, two meanings, and the offset is no longer thrown away.
 *
 * Metal keeps vertex streams and the vertex stage's [[buffer(n)]] arguments in the SAME
 * table, and which an index is depends on the pipeline's vertex descriptor -- which may
 * not even be set yet when this is called. So the offset is RECORDED here and judged at
 * the draw: for a stream it is Metal's own `offset:` and goes to the library, and for a
 * buffer argument it is still unimplemented and the draw says so by name, having the
 * pipeline's own mask to tell the two apart. Dropping it here is what would draw the
 * wrong mesh out of a buffer that holds several.
 */
- (void)setVertexBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	rwm_render_set_vertex_buffer(&_state, index, buffer, [buffer mesaBuffer], offset);
}

/*
 * And the fragment stage's table, which is now the same shape as the vertex one: the
 * offset is RECORDED and judged at the draw rather than refused here.
 *
 * It used to be refused on the spot, because the contract bound whole buffers and
 * ignoring an offset would read the wrong bytes quietly. An ARGUMENT BUFFER is bound
 * through this very call with an offset of its own -- Metal lets one allocation hold many
 * encodings and the offset is which one -- and whether index n is a set or a plain buffer
 * argument is the pipeline's answer and may not be known yet when this is called. So the
 * draw decides: a set takes the offset, a buffer argument with a non-zero offset is still
 * refused there, by name.
 */
- (void)setFragmentBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	rwm_render_set_fragment_buffer(&_state, index, buffer, [buffer mesaBuffer], offset);
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setVertexBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setFragmentBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)setFragmentTexture:(id)texture atIndex:(NSUInteger)index
{
	rwm_render_set_texture(&_state, index, texture, rw_core_of(texture));
}

- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	rwm_render_set_sampler(&_state, index, sampler, [sampler mesaSampler]);
}

/*
 * The viewport, of which exactly one half is served and the other half is declined, and the
 * split is the contract's: rwa_draw carries znear and zfar and carries no rectangle, because
 * the library's draw always covers the whole attachment.
 *
 * The depth range half is not a convenience either. It is the only thing in Metal that moves
 * a quad in z through this driver -- the generated vertex stage puts all four corners at
 * clip z = 0, and Metal's NDC z starts at 0 on the near plane, so the window depth of what
 * is drawn is znear exactly. Without it every draw here would be at depth 0 and no depth
 * test could ever be measured on the device.
 *
 * The rectangle half says so once, as it did before: a viewport silently ignored draws the
 * right triangle in the wrong place.
 */
- (void)setViewport:(MTLViewport)viewport
{
	rwm_render_set_viewport(&_state, (const double[6]){ viewport.originX, viewport.originY, viewport.width,
						 viewport.height, viewport.znear, viewport.zfar });
}
/* -setScissorRect:, ignored until 27 October: CoreAnimation draws a dirty region with every layer
 * that touches it under a scissor of that region, and without it those layers covered the frame
 * beyond the region -- the rectangles of older content in the user's videos */
- (void)setScissorRect:(MTLScissorRect)rect
{
	rwm_render_set_scissor(&_state, rect.x, rect.y, rect.width, rect.height);
}
/* Not carried to the draw (stage 3 of docs/metal-x6826b-plan.md): said, the first time an
 * argument asks for something that drawing everything as filled two-sided triangles is not. */
- (void)setCullMode:(NSUInteger)mode
{
	if (mode != MTLCullModeNone) {
		RW_ONCE("NOT DONE: -setCullMode:%lu; both faces are drawn\n", (unsigned long)mode);
	}
}
- (void)setFrontFacingWinding:(NSUInteger)winding     { (void)winding; }
- (void)setTriangleFillMode:(NSUInteger)mode
{
	if (mode != MTLTriangleFillModeFill) {
		RW_ONCE("NOT DONE: -setTriangleFillMode:%lu; triangles are filled\n", (unsigned long)mode);
	}
}

/* The depth/stencil state, recorded like the pipeline state: nothing runs at encode time,
 * so what a draw tests against is whatever was bound when that draw was encoded. A state
 * from another driver -- or nil, which is Metal's "no test, no write" -- is passed on as
 * nil, and the library draws with its own zeroed state, which is the same thing. */
- (void)setDepthStencilState:(id)state
{
	if (state != nil && ![state isKindOfClass:[RedwoodMetalDepthStencilState class]]) {
		rw_print("this depth/stencil state is a %s and not one of ours, so "
		         "it cannot be bound; this pass draws with no depth test\n",
		         class_getName(object_getClass(state)));
		state = nil;
	}
	rwm_render_set_depth_stencil(&_state, state, [state mesaDepthStencil]);
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	(void)r; (void)g; (void)b; (void)a;
}

- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance
{
	/* the state as it is, recorded by the core (src/metal/render.cpp: what is dropped and why, the pass's load
	 * actions with the first draw); the work keeps the objects it was recorded from */
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];
	struct rwm_draw_call call = {
		.primitive = primitive, .start = start, .count = count,
		.index_object = indexBuffer, .index_buffer = [indexBuffer mesaBuffer],
		.index_type = indexType, .index_offset = indexOffset, .base_vertex = baseVertex,
		.instances = instances, .base_instance = baseInstance,
	};

	if (rwm_render_draw(&_state, &call, &work->core)) {
		[_commandBuffer recordWork:work];
	}
	[work release];
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:1 baseInstance:0];
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:0];
}

/*
 * The rest of what MTLRenderCommandEncoder requires. Each is here for the reason the
 * device's own 44 were: a selector that is absent aborts the caller, and an application's
 * encoder sends a good many of these without thinking about it.
 *
 * The division is deliberate. A form that can be served by the single-binding path forwards
 * to it. A form that cannot be served -- the INDIRECT draws, whose whole argument list is
 * read out of a buffer the GPU may be writing -- DECLINES out loud, because drawing the
 * wrong primitives is worse than drawing none. A form that is merely state we do not vary
 * yet (store actions: we always store; residency: everything is resident) is a no-op, and
 * that is said here rather than guessed at by the next reader.
 */
- (void)setVertexBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

- (void)setFragmentBuffers:(const id *)buffers
		   offsets:(const NSUInteger *)offsets
		 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

/* The offset alone, which an application changes between draws out of one buffer. It is
 * the same table entry -setVertexBuffer:offset:atIndex: filled, so it is the same field:
 * for a stream the library honours it, and for a buffer argument the draw says it cannot
 * (it has the pipeline's mask and this method has not). */
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	rwm_render_set_vertex_offset(&_state, index, offset);
}

/* The same for the fragment stage, and now with the same answer as the vertex one: the
 * offset is recorded against the index already bound there, and the draw judges it. An
 * application that moves an argument buffer's offset between draws -- which is the
 * ordinary way to walk a table of encodings -- is doing nothing this driver has to
 * refuse. */
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	rwm_render_set_fragment_offset(&_state, index, offset);
}

- (void)setVertexTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;   /* the contract binds textures to the fragment stage only */
}
- (void)setVertexTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
}
- (void)setVertexSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
}
- (void)setVertexSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
}
- (void)setVertexSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
}
- (void)setFragmentTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentTexture:textures[i] atIndex:range.location + i];
	}
}
- (void)setFragmentSamplerState:(id)sampler
		    lodMinClamp:(float)lodMin
		    lodMaxClamp:(float)lodMax
			atIndex:(NSUInteger)index
{
	(void)lodMin; (void)lodMax;
	[self setFragmentSamplerState:sampler atIndex:index];
}
- (void)setFragmentSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] atIndex:range.location + i];
	}
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
	  baseInstance:(NSUInteger)base
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:base];
}

/*
 * Indexed drawing, which is what every drawing call in a real application is. All three
 * direct forms are one call here, because that is what they are in Metal: the shorter two
 * are the longest with instanceCount 1, baseVertex 0 and baseInstance 0, and Metal's own
 * header documents them that way (the parameter lists are identical as far as they go).
 *
 * Nothing is checked here except what the LIBRARY cannot check: that there is an index
 * buffer at all, and that it is one of ours (its Gallium resource is what the draw needs,
 * and a buffer allocated on the kernel path has none -- -mesaBuffer is nil for it, and
 * nil in the draw would read as "not indexed", which is the vertices in the wrong order).
 * The index type, the offset against the index size, the count against the buffer's
 * length and the indices against the quad are all refused BY NAME in air_metal.c, and
 * that refusal reaches the application as the command buffer's NSError, with the numbers
 * in it. Two readers of the same rule is how the two drift apart.
 */
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
		   baseVertex:(NSInteger)baseVertex
		 baseInstance:(NSUInteger)baseInstance
{
	if (buffer == nil) {
		rw_print("an indexed draw with no index buffer is refused rather "
		         "than drawn as if the indices were 0, 1, 2, ...\n");
		return;
	}
	[self recordDraw:primitive vertexStart:0 count:count indexBuffer:buffer
		indexType:type indexOffset:offset baseVertex:baseVertex
	    instanceCount:instances baseInstance:baseInstance];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset instanceCount:1
			 baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset
		      instanceCount:instances baseVertex:0 baseInstance:0];
}

/*
 * The INDIRECT forms, which were absent from this class altogether until now -- and
 * absent is the one answer that aborts the application, with our class's name on the
 * selector (10.1). They are the fourth -drawIndexedPrimitives: overload and the second
 * -drawPrimitives:, and what they ask for is the draw's whole argument list out of a
 * buffer: MTLDrawPrimitivesIndirectArguments and MTLDrawIndexedPrimitivesIndirect-
 * Arguments, five 32-bit words at an offset.
 *
 * They decline, and the reason is not that the words are hard to read -- the processor
 * can read them, this buffer is mapped. It is that the point of an indirect draw is a
 * count the GPU writes, and this library validates an indexed draw ON THE PROCESSOR
 * before it makes it: the index count against the buffer's length, the indices against
 * the quad. A count read here would be the count at encode time and not the one the GPU
 * will have written, so what came out would be a picture of the wrong number of
 * primitives -- and it would read as a result.
 *
 * Nothing in Metal makes this unreachable for an application, so this says so out loud
 * rather than drawing nothing silently.
 */
- (void)drawPrimitives:(NSUInteger)primitive
	indirectBuffer:(id)indirectBuffer
  indirectBufferOffset:(NSUInteger)offset
{
	rw_print("an indirect draw is not implemented (the argument list would "
	         "be read on the processor at encode time, and the GPU may not have written it "
	         "yet), so this draw is refused rather than drawn from the wrong count\n");
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
	       indirectBuffer:(id)indirectBuffer
	 indirectBufferOffset:(NSUInteger)indirectOffset
{
	[self drawPrimitives:primitive indirectBuffer:indirectBuffer
	       indirectBufferOffset:indirectOffset];
}

/* Raster state the contract does not carry yet. Collected here with one line each so that
 * the next person can see at a glance what the draw ignores. */
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count
{
	if (count > 1 && !_state.complained) {
		rw_print("%lu viewports are not implemented; the first one is "
		         "taken\n", (unsigned long)count);
		_state.complained = true;
	}
	if (count >= 1 && viewports != NULL) {
		[self setViewport:viewports[0]];
	}
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count
{
	if (count > 0) {
		[self setScissorRect:rects[0]];
	}
}
/* MTLDepthClipModeClip is what the library's rasterizer state does (depth_clip_near and
 * _far are both on); MTLDepthClipModeClamp, which keeps a primitive outside the depth range
 * and clamps its z instead, is a different picture and the contract has nowhere to ask for
 * it. Said once rather than ignored. */
- (void)setDepthClipMode:(NSUInteger)mode
{
	if (mode != MTLDepthClipModeClip && !_state.complained) {
		rw_print("MTLDepthClipModeClamp is not implemented; this pass clips "
		         "against the depth range instead of clamping to it\n");
		_state.complained = true;
	}
}
- (void)setDepthBias:(float)bias slopeScale:(float)scale clamp:(float)clamp
{
	if (bias != 0 || scale != 0) {
		RW_ONCE("NOT DONE: -setDepthBias:%g slopeScale:%g clamp:%g\n", bias, scale, clamp);
	}
}
/* The stencil reference, which Metal keeps on the encoder and not in the state object, so
 * it is recorded with the draw like the clear colour. */
- (void)setStencilReferenceValue:(uint32_t)value
{
	rwm_render_set_stencil_reference(&_state, value, value);
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	rwm_render_set_stencil_reference(&_state, front, back);
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset
{
	if (mode != MTLVisibilityResultModeDisabled) {
		RW_ONCE("NOT DONE: -setVisibilityResultMode:%lu; nothing is counted\n", (unsigned long)mode);
	}
	(void)offset;
}
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const void *)mappings
{
	(void)count; (void)mappings;
}

/*
 * We always store what was drawn, so these record nothing; saying it here is cheaper than
 * leaving the next reader to wonder whether a discarded attachment would be honoured.
 *
 * That is now also true of the depth attachment, and there it is more than tidiness: the
 * only thing MTLStoreActionDontCare permits that storing does not is leaving the contents
 * undefined, so keeping them is a superset of every store action Metal has. A pass whose
 * depth the next pass reads back therefore works whichever action it asked for -- and the
 * depth test inside one pass never depended on this at all.
 */
- (void)setColorStoreAction:(NSUInteger)action atIndex:(NSUInteger)index
{
	if (action == MTLStoreActionMultisampleResolve || action == MTLStoreActionStoreAndMultisampleResolve) {
		RW_ONCE("NOT DONE: -setColorStoreAction:%lu atIndex:%lu, a resolve\n", (unsigned long)action,
		         (unsigned long)index);
	}
}
- (void)setDepthStoreAction:(NSUInteger)action                 { (void)action; }
- (void)setStencilStoreAction:(NSUInteger)action               { (void)action; }
- (void)setColorStoreActionOptions:(NSUInteger)options atIndex:(NSUInteger)index
{
	(void)options; (void)index;
}
- (void)setDepthStoreActionOptions:(NSUInteger)options         { (void)options; }
- (void)setStencilStoreActionOptions:(NSUInteger)options       { (void)options; }

/* Residency: every allocation here is resident for its lifetime, so there is nothing to
 * make resident. */
/*
 * RESIDENCY, which on this driver is nothing to arrange, and that is worth saying rather
 * than implementing in silence.
 *
 * In Metal these calls exist because an argument buffer holds raw handles: the GPU reads
 * them, so the resources they name have to be made resident separately. Here every member
 * of every set is bound explicitly from the encoder's record (air_metal.h), so a resource
 * that is encoded is bound and a resource that is not encoded is a refusal at the draw --
 * there is no third state for residency to be about. What the application still owes is
 * what Metal makes it owe: keeping the resource alive, and this driver turns a resource
 * destroyed after it was encoded into a sentence rather than a read of freed memory.
 */
- (void)useResource:(id)resource usage:(NSUInteger)usage stages:(NSUInteger)stages
{
	(void)resource; (void)usage; (void)stages;
}
- (void)useResource:(id)resource usage:(NSUInteger)usage
{
	(void)resource; (void)usage;
	RW_ONCE("-useResource: is a no-op here: every member of an argument "
		"buffer is bound explicitly from what the encoder recorded, so there is no "
		"residency to arrange and no handle for the GPU to follow\n");
}
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)useResources:(const id *)resources
	       count:(NSUInteger)count
	       usage:(NSUInteger)usage
	      stages:(NSUInteger)stages
{
	(void)resources; (void)count; (void)usage; (void)stages;
}
- (void)useHeap:(id)heap
{
	(void)heap;
	RW_ONCE("-useHeap: is a no-op here, for -useResource:'s reason, and "
		"this driver has no heaps to make resident in any case\n");
}
- (void)useHeap:(id)heap stages:(NSUInteger)stages             { (void)heap; (void)stages; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count     { (void)heaps; (void)count; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count stages:(NSUInteger)stages
{
	(void)heaps; (void)count; (void)stages;
}

/*
 * THE REST OF WHAT <MTLRenderCommandEncoder> AND <MTLRenderCommandEncoderSPI> REQUIRE.
 *
 * 125 + 34 required methods between them, read out of the cache rather than the runtime
 * (MTLRenderCommandEncoderSPI is declared in AGXMetalA11.bundle, so no process on this
 * phone has it; tools/metal/owes.py does the subtraction). 48 of them had no answer
 * anywhere in this chain before this section, and an answer is the difference between a
 * daemon that keeps running and one launchd restarts for ever.
 *
 * Each one either serves, or refuses and says so once. Nothing here claims.
 */

/* Served, by forwarding: Metal's own shorthands for what this encoder already does. */
- (void)setFragmentTexture:(id)texture
	   atTextureIndex:(NSUInteger)textureIndex
	     samplerState:(id)sampler
	   atSamplerIndex:(NSUInteger)samplerIndex
{
	[self setFragmentTexture:texture atIndex:textureIndex];
	[self setFragmentSamplerState:sampler atIndex:samplerIndex];
}
- (void)setFragmentSamplerStates:(const id *)samplers
		    lodMinClamps:(const float *)lodMin
		    lodMaxClamps:(const float *)lodMax
		       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] lodMinClamp:lodMin[i]
				  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
- (void)setVertexSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexSamplerState:samplers[i] lodMinClamp:lodMin[i]
				lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
/* The SPI spelling of -setDepthClipMode:, which already says what it cannot do. */
- (void)setDepthClipModeSPI:(NSUInteger)mode { [self setDepthClipMode:mode]; }

/*
 * Served by construction, which is a different thing from ignored.
 *
 * A barrier and a fence both say "do not start this until that has finished". Everything
 * this driver submits goes to the kernel's single msm ring in the order it was committed,
 * and a command buffer's recorded work runs inside one -commit; there is no second queue
 * and no reordering, so the ordering a barrier asks for is already the only ordering there
 * is. If that ever stops being true -- a second ring, or work submitted from -endEncoding
 * -- these stop being correct, which is why it is written down here and not assumed.
 */
- (void)memoryBarrierWithScope:(NSUInteger)scope
		   afterStages:(NSUInteger)after
		  beforeStages:(NSUInteger)before
{
	(void)scope; (void)after; (void)before;
}
- (void)memoryBarrierWithResources:(const id *)resources
			     count:(NSUInteger)count
		       afterStages:(NSUInteger)after
		      beforeStages:(NSUInteger)before
{
	(void)resources; (void)count; (void)after; (void)before;
}
- (void)textureBarrier
{
	RW_ONCE("NOT DONE: -textureBarrier; a draw after it reads what is still in the tile\n");
}
- (void)updateFence:(id)fence afterStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}
- (void)waitForFence:(id)fence beforeStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}

/* Answers about this pass, and they are answers rather than placeholders: the attachments
 * are real textures in real memory (nothing memoryless), and there is no tile pipeline at
 * all, so there is no tile size to report. Zero is the only honest number for a feature
 * that does not exist -- a plausible 32x32 would be a claim, and the device already answers
 * no to every tile-shading query, so a caller that asks this has already been told. */
- (BOOL)isMemorylessRender { return NO; }
- (NSUInteger)tileWidth    { return 0; }
- (NSUInteger)tileHeight   { return 0; }

/* Refused, each with its name in the log once.
 *
 * Tessellation: the library has no tessellator, so a patch draw records nothing. Said out
 * loud because a draw that silently does nothing is the one failure this driver has already
 * paid for twice (docs/metal.md 10.21, 10.26). */
- (void)redwoodNoTessellation
{
	RW_ONCE("tessellation is not implemented; this patch draw records "
		"nothing\n");
}
- (void)drawPatches:(NSUInteger)controlPoints
	 patchStart:(NSUInteger)start
	 patchCount:(NSUInteger)count
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
      instanceCount:(NSUInteger)instances
       baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)instances; (void)baseInstance;
	[self redwoodNoTessellation];
}
- (void)drawPatches:(NSUInteger)controlPoints
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
     indirectBuffer:(id)indirectBuffer
indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)indirectBuffer;
	(void)indirectOffset;
	[self redwoodNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
		patchStart:(NSUInteger)start
		patchCount:(NSUInteger)count
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	     instanceCount:(NSUInteger)instances
	      baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)controlPointBuffer; (void)controlPointOffset; (void)instances;
	(void)baseInstance;
	[self redwoodNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	    indirectBuffer:(id)indirectBuffer
      indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)controlPointBuffer;
	(void)controlPointOffset; (void)indirectBuffer; (void)indirectOffset;
	[self redwoodNoTessellation];
}
- (void)setTessellationFactorBuffer:(id)buffer
			     offset:(NSUInteger)offset
		     instanceStride:(NSUInteger)stride
{
	(void)buffer; (void)offset; (void)stride;
	[self redwoodNoTessellation];
}
- (void)setTessellationFactorScale:(float)scale
{
	(void)scale;
	[self redwoodNoTessellation];
}

/* Indirect command buffers: a buffer of commands the GPU reads for itself. The device
 * answers no to every indirect-command-buffer query, so this is a path a correct caller
 * does not take; the draws in it would be lost silently if it did. */
- (void)redwoodNoIndirectCommands
{
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	[self redwoodNoIndirectCommands];
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	[self redwoodNoIndirectCommands];
}

/* Tile shading and imageblocks: a whole second pipeline kind (a tile function that runs over
 * the pass's imageblock), which this driver does not have. Every binding of it is refused
 * together, because one of them answering and the rest not would be worse than all of them
 * refusing: a tile dispatch would then run with half its bindings. */
- (void)redwoodNoTileShading
{
	RW_ONCE("tile shading and imageblocks are not implemented; this tile "
		"binding or dispatch is dropped\n");
}
- (void)dispatchThreadsPerTile:(MTLSize)size
{
	(void)size;
	[self redwoodNoTileShading];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length
			    offset:(NSUInteger)offset
			   atIndex:(NSUInteger)index
{
	(void)length; (void)offset; (void)index;
	[self redwoodNoTileShading];
}
- (void)setTileBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self redwoodNoTileShading];
}
- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self redwoodNoTileShading];
}
- (void)setTileBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self redwoodNoTileShading];
}
- (void)setTileBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self redwoodNoTileShading];
}
- (void)setTileTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self redwoodNoTileShading];
}
- (void)setTileTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self redwoodNoTileShading];
}
- (void)setTileSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self redwoodNoTileShading];
}
- (void)setTileSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self redwoodNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self redwoodNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self redwoodNoTileShading];
}

/* Function tables: pointers to shader functions, which an AIR module of ours never names
 * and the device answers no to (no ray tracing, no function pointers). A table bound here
 * would be a table a shader cannot call. */
- (void)redwoodNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVertexVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setVertexVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}
- (void)setTileVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setTileVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTables:(const id *)tables
			      withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}
- (void)setTileIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setTileIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}

/* A line width other than 1 would change the picture, so it is said; the rasterizer this
 * driver drives has one width. */
- (void)setLineWidth:(float)width
{
	if (width != 1.0f) {
		RW_ONCE("a line width of %g is not implemented; lines are one "
			"pixel wide\n", (double)width);
	}
}

/* Vertex amplification: one vertex shader run writing several positions (what stereo
 * rendering uses). The device answers 1 to -supportsVertexAmplificationCount:, so the only
 * mode a correct caller sets is the one that amplifies nothing. */
- (void)setVertexAmplificationMode:(NSUInteger)mode value:(NSUInteger)value
{
	if (value > 1) {
		RW_ONCE("vertex amplification to %lu views is not implemented; "
			"this pass draws one\n", (unsigned long)value);
	}
	(void)mode;
}

/* Counter sampling: the device has no counter sets (every query answers nil or zero), so
 * there is no sample buffer this could write into. */
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;
}

/*
 * MESH AND OBJECT STAGE BINDINGS, AND WHY THEY ARE HERE AT ALL.
 *
 * These 26 are not missing from the chain: _MTLCommandEncoder has an entry for every one of
 * them. Each entry is one of Apple's ABSTRACT methods -- a body that calls
 * doesNotRecognizeSelector: -- and that is the trap docs/metal.md 10.1 was written about:
 * `respondsToSelector:` answers YES, the method list lists it, and the abort when it is
 * called names the RECEIVER, so the log would read "-[RedwoodMetalRenderEncoder
 * setMeshTexture:atIndex:]: unrecognized selector" and send the next reader looking for a
 * method that is not the problem. An entry in a method table is not behaviour, and the only
 * way to tell these apart from working methods is to read the bodies out of the cache
 * (tools/metal/protometh.py --verdicts, which found exactly 28 of them under this driver's
 * classes: these, -commitWithDeadline: and -setThreadgroupPackingDisabled:).
 *
 * A mesh pipeline is two stages before the fragment one -- an object stage that decides how
 * much geometry to make and a mesh stage that makes it. This driver compiles a vertex and a
 * fragment function and nothing else, and the device answers no when asked whether mesh
 * shaders are supported, so there is no stage for any of this to bind to. They are refused
 * together for the same reason the tile bindings are: half of a stage's bindings arriving
 * would be worse than none.
 */
- (void)redwoodNoMeshStages
{
	RW_ONCE("mesh and object shader stages are not implemented (the device "
		"answers no to them); this binding is dropped\n");
}
- (void)setObjectBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setObjectBytes:(const void *)bytes
		length:(NSUInteger)length
	       atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)length; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setObjectIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setMeshBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setMeshBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setMeshVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoMeshStages];
}
- (void)setMeshIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoMeshStages];
}
/*
 * The last four of the mesh group are OPTIONAL members of <MTLRenderCommandEncoderSPI>, and
 * optional is the worse case rather than the lighter one: an optional method is the one
 * Apple asks about with respondsToSelector: before sending it, and an abstract entry in
 * _MTLCommandEncoder makes that question answer YES. So the probe passes, the call goes
 * out, and the abort names our class for a method we were never asked to have. Overriding
 * them turns the yes into a yes that is true.
 */
- (void)setMeshIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setMeshAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self redwoodNoMeshStages];
}
- (void)setObjectAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self redwoodNoMeshStages];
}

/* A split handler is called when Metal splits one render pass into several submissions,
 * which this driver never does: a pass is one draw list and one submission. */
- (void)addSplitHandler:(void (^)(id, NSUInteger, NSUInteger))handler
{
	(void)handler;
	RW_ONCE("a render-pass split handler was installed; this driver never "
		"splits a pass, so it will not be called\n");
}

/*
 * Ending a pass is Apple's bookkeeping, and this override used to skip it.
 * `_MTLCommandEncoder` RETAINS the command buffer in -initWithCommandBuffer: -- the
 * designated initialiser this driver sends to super -- and releases it in exactly one place,
 * the tail of its own -endEncoding (0x181a1f4a8: `objc_release(_commandBuffer);
 * _commandBuffer = nil`). An override that does nothing leaves that ivar set, and Apple's
 * -dealloc checks it:
 *
 *   0x181a1e5f4  cbz x8, +124      ; _commandBuffer nil -> nothing to say
 *   0x181a1e610  MTLReportFailure(0, "-[_MTLCommandEncoder dealloc]", 131,
 *                                 @"Command encoder released without endEncoding")
 *
 * and MTLReportFailure here is an abort(). That is the whole of logs/214841-run.log: that
 * line 86 times and 43 aborts of backboardd, one every ten seconds, all from
 * CA::OGL::MetalContext::warmup_shaders -- which for each attachment combination it warms
 * opens a render encoder, ends it immediately, and pops the autorelease pool it made it in
 * (0x188476304, 0x188476308, then the pop whose return address is +1064). So every warmed
 * pass cost one abort. It also leaked a retain on every command buffer any encoder touched,
 * which is the quieter half of the same mistake.
 *
 * So the superclass is called, and what that runs for us is measured rather than hoped --
 * every step is Apple's own code, and the long middle of the function is skipped:
 *
 *   [_commandBuffer executeSynchronizationNotifications:1]   real; our list is empty
 *   [_commandBuffer setCurrentCommandEncoder:nil]            real; an ivar write
 *   _isProgressTrackingEncoder is 0, so no short cut through, and then
 *   [_commandBuffer errorOptions] with bit 0 clear jumps straight to the tail
 *   objc_release(_commandBuffer); _commandBuffer = nil
 *
 * Calling it twice is safe: the second time every message goes to a nil ivar and the release
 * is a release of nil. The one branch not taken is framework-assisted error tracking
 * (`errorOptions` bit 0, MTLCommandBufferErrorOptionEncoderExecutionStatus): it would build
 * an `_MTLCommandBufferEncoderInfo` and ask this encoder -getDriverUniqueID, which is a
 * one-instruction forward to -globalTraceObjectID. Nothing here turns that option on, and if
 * something ever does, that selector is where to look.
 *
 * What this deliberately does NOT do is seal the encoder. Metal calls encoding after
 * -endEncoding undefined, and this driver would still record it; refusing that is separate
 * work and nothing has asked for it.
 *
 * A pass that clears and draws nothing still clears (26 October): recorded as a piece of work of
 * its own (a clear, RWM_BLIT_CLEAR: src/metal/render.cpp), in its place among the others.
 */
- (void)endEncoding
{
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];

	if (rwm_render_end(&_state, &work->core)) {
		rw_say_blit(RWM_BLIT_CLEAR, (const unsigned[3]){ 0, 0, 0 }, nil,
			    _state.color_count != 0 ? (id)_state.color[0].object : nil);
		[_commandBuffer recordWork:work];
	}
	[work release];
	[super endEncoding];
}

@end

@implementation RedwoodMetalComputeEncoder {
	RedwoodMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
	struct rwm_compute_state _state;             /* what is bound: src/metal/compute.cpp */
}

- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer
{
	/* the initialiser IOGPUMetalCommandEncoder sends to super (0x1ec171d14 -> 0x181a1ee6c) */
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setComputePipelineState:(id)state
{
	rwm_compute_set_pipeline(&_state, state, [(RedwoodMetalComputePipeline *)state compute]);
}

- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	rwm_compute_set_buffer(&_state, index, buffer, [buffer mesaBuffer], offset);
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	rwm_compute_set_texture(&_state, index, texture, rw_core_of(texture));
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	rwm_compute_set_sampler(&_state, index, sampler, [sampler mesaSampler]);
}

- (void)setSamplerState:(id)sampler
	    lodMinClamp:(float)lodMin
	    lodMaxClamp:(float)lodMax
		atIndex:(NSUInteger)index
{
	rwm_compute_lod_clamp(&_state, lodMin, lodMax);
	[self setSamplerState:sampler atIndex:index];
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/* Metal's own shorthand for small constant data; it is a buffer here, because that is what
 * the shader's binding is. */
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

/* the state as it is, recorded by the core (src/metal/compute.cpp: what is dropped and why) */
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)size
{
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];

	if (rwm_compute_dispatch(&_state, (const unsigned long[3]){ groups.width, groups.height, groups.depth },
				 (const unsigned long[3]){ size.width, size.height, size.depth }, &work->core)) {
		[_commandBuffer recordWork:work];
	}
	[work release];
}

- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)size
{
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];

	if (rwm_compute_dispatch_threads(&_state, (const unsigned long[3]){ threads.width, threads.height, threads.depth },
					 (const unsigned long[3]){ size.width, size.height, size.depth }, &work->core)) {
		[_commandBuffer recordWork:work];
	}
	[work release];
}

- (void)useResource:(id)resource usage:(NSUInteger)usage { (void)resource; (void)usage; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)memoryBarrierWithScope:(NSUInteger)scope { (void)scope; }
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)length; (void)index;   /* only what the shader declares is given, for now */
}

/*
 * THE REST OF WHAT <MTLComputeCommandEncoder> AND <MTLComputeCommandEncoderSPI> REQUIRE:
 * 20 methods that had no answer in this chain, each of which was an abort waiting for the
 * first daemon to encode a kernel. Counted by tools/metal/owes.py off the cache, because
 * MTLComputeCommandEncoderSPI is declared in AGXMetalA11.bundle and is in no process here.
 */

/* Served, by forwarding to the singular form -- which is where this driver's refusal of a
 * non-zero buffer offset already lives, so a plural bind inherits it instead of slipping
 * past it. */
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	rwm_compute_set_buffer_offset(&_state, index, offset);
}
- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}
- (void)setSamplerStates:(const id *)samplers
	    lodMinClamps:(const float *)lodMin
	    lodMaxClamps:(const float *)lodMax
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] lodMinClamp:lodMin[i]
			  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}

/* Served by construction: one ring, one submission per command buffer, in commit order, so
 * the ordering a barrier or a fence asks for is the only ordering there is. Residency is
 * the same answer as on the render encoder: every allocation is resident for its lifetime. */
- (void)memoryBarrierWithResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
}
- (void)updateFence:(id)fence  { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }
- (void)useHeap:(id)heap       { (void)heap; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count { (void)heaps; (void)count; }
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;   /* the device has no counter sets */
}

/* Declined: there is no debug information for a kernel of ours. nil is what Metal's own
 * driver returns when a pipeline was not built with it, so the caller has this path. */
- (id)newKernelDebugInfo { return nil; }

/* Declined, and each says its name once. An indirect dispatch reads its grid out of GPU
 * memory, which means the size is not known when the command is recorded -- this driver
 * records sizes, so there is nothing to record. A dispatch that vanishes without a word is
 * the worst outcome available, hence the line. */
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer
			  indirectBufferOffset:(NSUInteger)offset
			 threadsPerThreadgroup:(MTLSize)size
{
	(void)buffer; (void)offset; (void)size;
	RW_ONCE("an indirect dispatch is not implemented (the grid lives in GPU "
		"memory and this driver records the grid); this dispatch is dropped\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
/* The stage-in region is for a kernel whose arguments arrive as [[stage_in]] -- the
 * non-uniform-grid feature. Nothing in the AIR this driver compiles declares one. */
- (void)redwoodNoStageIn
{
	RW_ONCE("a kernel stage-in region is not implemented; the dispatch "
		"covers the whole grid it was given\n");
}
- (void)setStageInRegion:(MTLRegion)region
{
	(void)region;
	[self redwoodNoStageIn];
}
- (void)setStageInRegionWithIndirectBuffer:(id)buffer
		      indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	[self redwoodNoStageIn];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
	(void)width; (void)height;
	RW_ONCE("imageblocks are not implemented; this kernel's imageblock "
		"size is ignored\n");
}
/* An entry for this exists in _MTLCommandEncoder and it is one of Apple's abstract ones
 * (tools/metal/protometh.py --verdicts), so inheriting it is an abort rather than a
 * default. It is a hint about how the hardware packs threadgroups together; the compiler
 * library decides that for itself and there is nothing here to turn off. */
- (void)setThreadgroupPackingDisabled:(BOOL)disabled { (void)disabled; }

- (void)redwoodNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}
- (void)setIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self redwoodNoFunctionTables];
}
- (void)setIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self redwoodNoFunctionTables];
}

/* The same as the render encoder's, for the same measured reason: the superclass retains the
 * command buffer in the initialiser and releases it only here, and its -dealloc aborts on an
 * encoder released without this. Nothing has deallocated a compute encoder of ours on the
 * phone yet -- the one that aborted 43 times was the render one, out of warmup_shaders -- so
 * this half is the same fix made before it is measured, not after. */
- (void)endEncoding
{
	[super endEncoding];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * MTLBlitCommandEncoder (26 October): until then -blitCommandEncoder answered nil and every copy
 * made through one was silently not made -- the snapshot of an application among them, black or
 * stale in SpringBoard's closing animation. Each copy is recorded as a piece of work (src/metal/blit.cpp),
 * run in its place among the draws, by the GPU. Mipmaps, fences, synchronization
 * and the rest need nothing here: one queue, run in order, the memory shared.
 */
@implementation RedwoodMetalBlitEncoder {
	RedwoodMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
}

- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

/* a copy recorded by the core (src/metal/blit.cpp): `blit` names the library's objects, from and to are Metal's,
 * kept with it */
- (void)redwoodRecord:(const struct rwm_blit *)blit from:(id)from to:(id)to
{
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];

	rw_say_blit(blit->kind, blit->size, from, to);
	rwm_blit_record(&work->core, blit, from, rw_core_of(from), to, rw_core_of(to));
	[_commandBuffer recordWork:work];
	[work release];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
           sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toTexture:(id)destination
       destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
      destinationOrigin:(MTLOrigin)destinationOrigin
{
	struct rwm_blit blit = {
		.kind = RWM_BLIT_TEXTURE_TO_TEXTURE,
		.from_texture = [source mesaTexture],
		.to_texture = [destination mesaTexture],
		.from_slice = (unsigned)sourceSlice,
		.from_level = (unsigned)sourceLevel,
		.from_origin = { (unsigned)sourceOrigin.x, (unsigned)sourceOrigin.y, (unsigned)sourceOrigin.z },
		.to_slice = (unsigned)destinationSlice,
		.to_level = (unsigned)destinationLevel,
		.to_origin = { (unsigned)destinationOrigin.x, (unsigned)destinationOrigin.y, (unsigned)destinationOrigin.z },
		.size = { (unsigned)sourceSize.width, (unsigned)sourceSize.height, (unsigned)sourceSize.depth },
	};

	[self redwoodRecord:&blit from:source to:destination];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
              toTexture:(id)destination destinationSlice:(NSUInteger)destinationSlice
       destinationLevel:(NSUInteger)destinationLevel sliceCount:(NSUInteger)sliceCount
             levelCount:(NSUInteger)levelCount
{
	for (NSUInteger l = 0; l < levelCount; l++) {
		NSUInteger w = MAX([source width] >> (sourceLevel + l), 1u), h = MAX([source height] >> (sourceLevel + l), 1u);

		for (NSUInteger sl = 0; sl < sliceCount; sl++) {
			[self copyFromTexture:source sourceSlice:sourceSlice + sl sourceLevel:sourceLevel + l
			         sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toTexture:destination
			     destinationSlice:destinationSlice + sl destinationLevel:destinationLevel + l
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
		}
	}
}

- (void)copyFromTexture:(id)source toTexture:(id)destination
{
	NSUInteger levels = MIN([source mipmapLevelCount], [destination mipmapLevelCount]);
	NSUInteger slices = MAX([source arrayLength], 1u) * ([source textureType] == MTLTextureTypeCube ? 6 : 1);

	[self copyFromTexture:source sourceSlice:0 sourceLevel:0 toTexture:destination destinationSlice:0
	     destinationLevel:0 sliceCount:slices levelCount:MAX(levels, 1u)];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset toBuffer:(id)destination
     destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size
{
	struct rwm_blit blit = {
		.kind = RWM_BLIT_BUFFER_TO_BUFFER,
		.from_buffer = [source mesaBuffer],
		.to_buffer = [destination mesaBuffer],
		.from_offset = sourceOffset,
		.to_offset = destinationOffset,
		.bytes = size,
	};

	[self redwoodRecord:&blit from:source to:destination];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)bytesPerRow
     sourceBytesPerImage:(NSUInteger)bytesPerImage sourceSize:(MTLSize)size toTexture:(id)destination
        destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
{
	struct rwm_blit blit = {
		.kind = RWM_BLIT_BUFFER_TO_TEXTURE,
		.to_texture = [destination mesaTexture],
		.from_contents = (const uint8_t *)[source contents],
		.from_offset = sourceOffset,
		.row_bytes = bytesPerRow,
		.to_slice = (unsigned)slice,
		.to_level = (unsigned)level,
		.to_origin = { (unsigned)origin.x, (unsigned)origin.y, (unsigned)origin.z },
		.size = { (unsigned)size.width, (unsigned)size.height, (unsigned)MAX(size.depth, 1u) },
	};

	(void)bytesPerImage;
	[self redwoodRecord:&blit from:source to:destination];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)bytesPerRow
     sourceBytesPerImage:(NSUInteger)bytesPerImage sourceSize:(MTLSize)size toTexture:(id)destination
        destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
                 options:(NSUInteger)options
{
	(void)options;
	[self copyFromBuffer:source sourceOffset:sourceOffset sourceBytesPerRow:bytesPerRow sourceBytesPerImage:bytesPerImage
	          sourceSize:size toTexture:destination destinationSlice:slice destinationLevel:level destinationOrigin:origin];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
           sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id)destination
      destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)bytesPerRow
    destinationBytesPerImage:(NSUInteger)bytesPerImage
{
	struct rwm_blit blit = {
		.kind = RWM_BLIT_TEXTURE_TO_BUFFER,
		.from_texture = [source mesaTexture],
		.to_contents = (uint8_t *)[destination contents],
		.from_slice = (unsigned)slice,
		.from_level = (unsigned)level,
		.from_origin = { (unsigned)origin.x, (unsigned)origin.y, (unsigned)origin.z },
		.size = { (unsigned)size.width, (unsigned)size.height, (unsigned)MAX(size.depth, 1u) },
		.to_offset = offset,
		.row_bytes = bytesPerRow,
	};

	(void)bytesPerImage;
	[self redwoodRecord:&blit from:source to:destination];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
           sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id)destination
      destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)bytesPerRow
    destinationBytesPerImage:(NSUInteger)bytesPerImage options:(NSUInteger)options
{
	(void)options;
	[self copyFromTexture:source sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size
	             toBuffer:destination destinationOffset:offset destinationBytesPerRow:bytesPerRow
	  destinationBytesPerImage:bytesPerImage];
}

- (void)fillBuffer:(id)buffer range:(NSRange)range value:(uint8_t)value
{
	struct rwm_blit blit = {
		.kind = RWM_BLIT_FILL,
		.to_buffer = [buffer mesaBuffer],
		.to_offset = range.location,
		.bytes = range.length,
		.fill_value = value,
	};

	[self redwoodRecord:&blit from:nil to:buffer];
}

/* the levels below the base, by the GPU in their place among the draws (RWM_BLIT_MIPMAPS) */
- (void)generateMipmapsForTexture:(id)texture
{
	if (![texture isKindOfClass:[RedwoodMetalTexture class]] || [texture mipmapLevelCount] <= 1) {
		return;
	}
	struct rwm_blit blit = {
		.kind = RWM_BLIT_MIPMAPS,
		.to_texture = [texture mesaTexture],
	};

	[self redwoodRecord:&blit from:nil to:texture];
}
- (void)synchronizeResource:(id)resource { (void)resource; }
- (void)synchronizeTexture:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}
- (void)optimizeContentsForGPUAccess:(id)texture { (void)texture; }
- (void)optimizeContentsForCPUAccess:(id)texture { (void)texture; }
- (void)updateFence:(id)fence { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }
- (void)setLabel:(NSString *)label { (void)label; }

/* Any other selector of MTLBlitCommandEncoder's: nothing done, said once, rather than the abort
 * an unrecognized selector is. */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	NSMethodSignature *signature = [super methodSignatureForSelector:selector];

	return signature != nil ? signature : [NSMethodSignature signatureWithObjCTypes:"v@:"];
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	RW_ONCE("blit encoder: %s not done\n", sel_getName([invocation selector]));
}

- (void)endEncoding
{
	[super endEncoding];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * MTLArgumentEncoder, which on this driver is a RECORDER and not a writer.
 *
 * Metal's encoder writes one 8-byte handle per member into a plain buffer, and those
 * handles are an Apple GPU's descriptors: a Gallium driver, which binds by index, cannot
 * read them. It also does not need to -- the AIR frontend resolves every member of every
 * set to a binding while translating (docs/air.md 8.2) -- so each -setBuffer:atIndex:
 * here RECORDS (resource, Metal id) against (the destination buffer, the destination
 * offset) in the compiler library, and the draw binds every member the pipeline's shaders
 * declare from those records. The buffer's contents are never read, and an argument
 * buffer the application filled by any other means is REFUSED BY NAME at the draw.
 *
 * Two things are deliberate about this object:
 *
 *   - IT HOLDS NO RECORDS ITSELF. Its state is (buffer, offset), and the records live
 *     with the destination buffer in the library, because that is what Metal's own object
 *     model allows: one buffer holds several encodings at several offsets, each reused
 *     across many draws, and an application may release the encoder and go on drawing
 *     with what it encoded.
 *   - -setArgumentBuffer:offset: DOES NOT CLEAR the encoding at that offset, which is
 *     Metal's behaviour: the bytes an earlier encoding wrote there are still there, so a
 *     second encoding that sets three of five members leaves the other two as they were.
 *
 * The layout comes from the module (`air.indirect_buffer`), so `encodedLength` and
 * `alignment` are Apple's own numbers -- 8 bytes per member the set declares and 8 --
 * which matters because the application allocates its buffer from them and then encodes
 * at multiples of them.
 */
@implementation RedwoodMetalArgumentEncoder {
	RedwoodMetalDevice *_owner;         /* not retained: the device owns everything here */
	NSString *_label;
	struct rwm_argenc *_core;           /* the layout, the destination, the records: src/metal/argenc.cpp */
	RedwoodMetalBuffer *_destination;   /* what -setArgumentBuffer:offset: named, kept alive */
}

/* Conformance declared at runtime, as the device's MTLDeviceSPI is and for the same
 * reason: an application (or Metal's own validation) may ask conformsToProtocol: before
 * it sends anything, and the protocol is only in the process once Metal is. Every method
 * it requires is answered below -- some of them by declining out loud, which is an answer
 * and not an absence. */
+ (void)load
{
	Protocol *p = objc_getProtocol("MTLArgumentEncoder");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLArgumentEncoder) refused\n");
	}
}

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_core = rwm_argenc_new(layout, fromModule);
	if (_core == NULL) {
		[self release];
		return nil;
	}
	_owner = device;
	return self;
}

- (void)dealloc
{
	[_label release];
	[_destination release];
	rwm_argenc_free(_core);
	[super dealloc];
}

- (id)device       { return _owner; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label
{
	[_label release];
	_label = [label copy];
}

/* The two numbers an application allocates from, and they are the module's own: 8 bytes
 * per member the set declares, used by this shader or not (air_metal.h). A length of ours
 * that was too small would have the application overlapping its own encodings. */
- (NSUInteger)encodedLength { return rwm_argenc_layout(_core)->encoded_length; }
- (NSUInteger)alignment     { return rwm_argenc_layout(_core)->alignment; }

/* Not part of MTLArgumentEncoder: what this encoder is for, so a test can read back the
 * mapping between Metal's [[id(n)]] and the index the GPU is programmed with instead of
 * trusting it. Metal's own pipeline reflection would report the same thing. */
- (NSUInteger)redwoodMemberCount { return rwm_argenc_layout(_core)->member_count; }
- (NSUInteger)redwoodMetalIndex  { return rwm_argenc_layout(_core)->metal_index; }
- (BOOL)redwoodMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index
{
	if (i >= rwm_argenc_layout(_core)->member_count) {
		return NO;
	}
	if (mid != NULL) {
		*mid = rwm_argenc_layout(_core)->members[i].id;
	}
	if (kind != NULL) {
		*kind = rwm_argenc_layout(_core)->members[i].kind;
	}
	if (index != NULL) {
		*index = rwm_argenc_layout(_core)->members[i].index;
	}
	return YES;
}

/*
 * The destination. Metal checks the offset against the buffer's length and the encoder's
 * alignment, and so does this -- an application that encodes at an unaligned offset has
 * allocated its buffer from something other than -encodedLength, and every record made
 * afterwards would be keyed by an offset the draw will never look up.
 */
- (void)setArgumentBuffer:(id)buffer offset:(NSUInteger)offset
{
	if (buffer != nil && ![buffer isKindOfClass:[RedwoodMetalBuffer class]]) {
		rw_print("the argument buffer is a %s and not one of ours, so "
		         "nothing can be encoded into it\n",
		         class_getName(object_getClass(buffer)));
		rwm_argenc_set_destination(_core, false, NULL, NULL, 0);
		[_destination release];
		_destination = nil;
		return;
	}
	if (!rwm_argenc_set_destination(_core, buffer != nil, [buffer mesaBuffer], [buffer contents], offset)) {
		[_destination release];
		_destination = nil;
		return;
	}
	/* Retained, unlike the other back-pointers in this file, and for a measured
	 * reason: the records live in the compiler library WITH that buffer, so an
	 * application that released it while this encoder still pointed at it would have
	 * rwa_buffer_destroy free the records (and scrub them out of everyone else's) and
	 * leave this object holding a freed MTLBuffer. Metal's own encoder outliving its
	 * argument buffer is an application's mistake; a crash in the driver is ours. */
	[buffer retain];
	[_destination release];
	_destination = buffer;
}

/* Metal's array form: the same destination with an element's worth of offset added. The
 * arithmetic is Metal's own -- encodedLength per element -- and it is here rather than
 * declined because an application that encodes an ARRAY of sets is doing nothing this
 * driver cannot record. */
- (void)setArgumentBuffer:(id)buffer startOffset:(NSUInteger)offset
	     arrayElement:(NSUInteger)element
{
	[self setArgumentBuffer:buffer offset:offset + element * rwm_argenc_layout(_core)->encoded_length];
}

/* Whether this encoder is in a state to record, said once per site: an id past the set's
 * own members is the mistake worth naming, because Metal would write a handle into the
 * buffer and the shader would read a descriptor that is not there. */
- (BOOL)redwoodReady:(NSUInteger)index for:(const char *)what
{
	return rwm_argenc_ready(_core, index, what);
}

/*
 * The three that record. The KIND is the call, as it is in Metal; a member encoded with
 * the wrong one is refused at the draw, by name, where the shader's own declaration is in
 * hand -- not here, because one buffer may be encoded for a set this driver has never
 * seen a shader for.
 *
 * The library's sentence is passed through rather than replaced: it names the id and what
 * went wrong with it.
 */
- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (![self redwoodReady:index for:"a buffer"]) {
		return;
	}
	if (buffer != nil && ![buffer isKindOfClass:[RedwoodMetalBuffer class]]) {
		rw_print("the buffer encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(buffer)));
		return;
	}
	rwm_argenc_encode_buffer(_core, index, [buffer mesaBuffer], offset);
}

- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets ? offsets[i] : 0
			atIndex:range.location + i];
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	if (![self redwoodReady:index for:"a texture"]) {
		return;
	}
	if (texture != nil && ![texture isKindOfClass:[RedwoodMetalTexture class]]) {
		rw_print("the texture encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(texture)));
		return;
	}
	rwm_argenc_encode_texture(_core, index, [texture mesaTexture]);
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (![self redwoodReady:index for:"a sampler"]) {
		return;
	}
	if (sampler != nil && ![sampler isKindOfClass:[RedwoodMetalSampler class]]) {
		rw_print("the sampler encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(sampler)));
		return;
	}
	rwm_argenc_encode_sampler(_core, index, [sampler mesaSampler]);
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/*
 * -constantDataAtIndex: is where an application writes a VALUE member -- an `int` or a
 * `float4` inside the set rather than a descriptor -- and it is answered with the real
 * bytes of the argument buffer at that member's place, which is what Metal answers.
 *
 * Nothing of this driver ever reads those bytes, and that is said here because it is the
 * kind of silence that costs a day: a shader that reads a value out of a set is refused
 * when it is TRANSLATED ("a value read out of the set instead of a descriptor",
 * docs/air.md 8.2.1), so an application writing here is writing into memory no shader of
 * ours will be compiled to read. Answering a real pointer is still the right answer --
 * the write lands in the application's own buffer and nothing is corrupted -- while
 * answering NULL would be a crash in the application's memcpy.
 */
- (void *)constantDataAtIndex:(NSUInteger)index
{
	return rwm_argenc_constant_data(_core, index);
}

/*
 * A SET INSIDE A SET, declined by name. The frontend refuses a nested `air.indirect_buffer`
 * when it translates the shader -- one module of Resident Evil 4's 7 620 has one, the
 * kernel that encodes draws on the GPU -- so an encoder for one would be an object whose
 * records no pipeline could ever ask for.
 */
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
	rw_print("-newArgumentEncoderForBufferAtIndex:%lu asks for an encoder for "
	         "a set INSIDE a set, and this port's frontend refuses a nested argument buffer "
	         "when it translates the shader, so there is nothing to encode for\n",
	         (unsigned long)index);
	return nil;
}

/*
 * The members that are not a buffer, a texture or a sampler. Every one of them is declined
 * BY NAME and for the same reason: the frontend refuses such a member when it translates
 * the shader ("a member that is not a buffer, a texture or a sampler"), so a record here
 * would be a record no draw could use -- and a silent no-op would leave the application
 * believing its set was encoded.
 */
- (void)redwoodNoSuchMember:(const char *)what atIndex:(NSUInteger)index
{
	rw_print("%s was encoded at id %lu of an argument buffer, and this port "
	         "binds a buffer, a texture or a sampler member and refuses the rest when the "
	         "shader is translated; this encoding is dropped\n", what, (unsigned long)index);
}

- (void)setRenderPipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self redwoodNoSuchMember:"a render pipeline state" atIndex:index];
}
- (void)setRenderPipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self redwoodNoSuchMember:"render pipeline states" atIndex:range.location];
}
- (void)setComputePipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self redwoodNoSuchMember:"a compute pipeline state" atIndex:index];
}
- (void)setComputePipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self redwoodNoSuchMember:"compute pipeline states" atIndex:range.location];
}
- (void)setIndirectCommandBuffer:(id)buffer atIndex:(NSUInteger)index
{
	(void)buffer;
	[self redwoodNoSuchMember:"an indirect command buffer" atIndex:index];
}
- (void)setIndirectCommandBuffers:(const id *)buffers withRange:(NSRange)range
{
	(void)buffers;
	[self redwoodNoSuchMember:"indirect command buffers" atIndex:range.location];
}
- (void)setAccelerationStructure:(id)structure atIndex:(NSUInteger)index
{
	(void)structure;
	[self redwoodNoSuchMember:"an acceleration structure" atIndex:index];
}
- (void)setVisibleFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self redwoodNoSuchMember:"a visible function table" atIndex:index];
}
- (void)setVisibleFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self redwoodNoSuchMember:"visible function tables" atIndex:range.location];
}
- (void)setIntersectionFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self redwoodNoSuchMember:"an intersection function table" atIndex:index];
}
- (void)setIntersectionFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self redwoodNoSuchMember:"intersection function tables" atIndex:range.location];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * -[MTLFunction newArgumentEncoderWithBufferIndex:], which is a method of METAL'S OWN
 * class and is therefore installed at RUNTIME here rather than written as a category.
 *
 * Two decisions, and the first one is measured. Metal's own implementation cannot work on
 * this port:
 *
 *   -[_MTLFunction newArgumentEncoderWithBufferIndex:] (0x181aef7f4) forwards to
 *   ...reflection:binaryArchives: (0x181aef85c), which asks
 *   -[_MTLFunction reflectionWithOptions:binaryArchives:] (0x181aef458) for the
 *   function's reflection -- and that one calls -compiler, which on this port is
 *   -[_MTLDevice compiler] (0x181aac128), a two-instruction stub answering nil
 *   (docs/metal.md 10.7: everything behind Metal's own compiler is nil here). The
 *   builder then walks `[reflection arguments]`, finds a count of zero, and takes its
 *   failure path (0x181aef924):
 *
 *     MTLReportFailure(0, "-[_MTLFunction newArgumentEncoderWithBufferIndex:reflection:"
 *                      "functionReflection:]", 0x22fc,
 *                      @"bufferIndex %lu does not identify an argument buffer")
 *
 *   and returns nil.
 *
 * So the choice is not between Apple's implementation and ours; it is between nil and an
 * encoder. And what Metal was missing is what this port HAS: -[_MTLFunction bitcodeData]
 * works (docs/metal.md 10.12, checked over all eleven functions of AV1SW.metallib), and
 * the frontend reads the set's layout straight out of those bytes. Resident Evil 4 sends
 * this selector; so does any application that writes `[[id(n)]]` structs in MSL instead
 * of building MTLArgumentDescriptors.
 *
 * The second decision is HOW, and a category is the obvious way and the wrong one: a
 * category on `_MTLFunction` makes this dylib import `_OBJC_CLASS_$__MTLFunction`, and a
 * private class symbol that does not bind is a dylib that does not LOAD -- which would
 * take the whole driver down for every Metal client in the process, not just this
 * selector. The one private class symbol this driver does import,
 * `_OBJC_CLASS_$__MTLDevice`, is imported by IOGPU.framework too and has been measured
 * loading on the phone; `ipsw dyld symaddr -b` finds nothing in the cache that imports
 * _MTLFunction's, so nothing says it would bind. class_replaceMethod needs no symbol at
 * all, and it is the same move ensure_spi already makes for MTLDeviceSPI.
 *
 * What it costs, said plainly: the method is replaced for every MTLFunction in the
 * process, Apple's daemons included. Nothing is lost by that -- the implementation it
 * replaces returns nil for all of them, for the reason above -- and nothing else of
 * _MTLFunction is touched.
 */
static id
rw_function_argument_encoder(id self, SEL cmd, NSUInteger index)
{
	id device = ((id (*)(id, SEL))objc_msgSend)(self, sel_getUid("device"));

	(void)cmd;
	if (![device isKindOfClass:[RedwoodMetalDevice class]]) {
		rw_print("-newArgumentEncoderWithBufferIndex: on a function whose device is "
		         "a %s and not ours\n", class_getName(object_getClass(device)));
		return nil;
	}
	return [(RedwoodMetalDevice *)device redwoodArgumentEncoderForFunction:self
								  bufferIndex:index];
}

/* The same with Metal's out-parameter, which is answered nil: a reflection object needs
 * Metal's own compiler, and every pipeline path of this driver already says so. */
static id
rw_function_argument_encoder_reflection(id self, SEL cmd, NSUInteger index, id *reflection)
{
	if (reflection != NULL) {
		*reflection = nil;
	}
	return rw_function_argument_encoder(self, cmd, index);
}

/* Installed once per process, from ensure_spi -- which runs at +load (Metal loaded us, so
 * Metal is there) and again before the first device is made (a library can also be
 * brought in by hand, before Metal). A process without Metal's class simply has no
 * MTLFunction to answer for. */
static void
ensure_function_encoder(void)
{
	static BOOL installed;
	Class fn = objc_lookUpClass("_MTLFunction");

	if (installed || fn == NULL) {
		return;
	}
	installed = YES;
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:"),
			    (IMP)rw_function_argument_encoder, "@@:Q");
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:reflection:"),
			    (IMP)rw_function_argument_encoder_reflection, "@@:Q^@");
}
