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
 *   - -initWithAcceleratorPort: opens this port's OWN gpu service, `HakiosGPU`, the way
 *     Mesa's shim does (tools/mesa/darwin-drm/darwin_drm.c: IOServiceOpen with type 0,
 *     then msm-shaped requests through IOConnectCallStructMethod). That is the road the
 *     resource layer will take, and it already carries Mesa's own dispatches today. The
 *     IOGPU road was measured shut: IOServiceOpen on our IOAcceleratorES service with
 *     Apple's type 1 answers kIOReturnUnsupported.
 *
 * The first resource is here too. A Metal buffer on this port is a GEM allocation of the
 * kernel's own gpu driver, made and mapped exactly the way Mesa's shim makes and maps one
 * (tools/mesa/darwin-drm/darwin_drm.c): kMsmGemNew (0x42) for the memory, MSM_INFO_GET_IOVA
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
 * /hakios/lib/libhakios-air.dylib through the contract in tools/mesa/air/air_metal.h.
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
 * (`hakiosmetal[progname pid]:`, see rw_print): a driver is a library, four or five
 * Metal clients are alive in one boot of this port, and a refusal nobody can attribute
 * is half a refusal.
 */

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <unistd.h>

/* A pipeline that took long to make, said with its function and the time: a process that
 * makes several at launch can miss its check-in (glass19: MercuryPoster's extension,
 * XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT, its wallpaper never shown). */
static void rw_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
@class HakiosMetalWork;
static void rw_say_sync_inputs(HakiosMetalWork *work);
static void rw_say_surface_colour(IOSurfaceRef surface, const uint8_t *base, size_t row, size_t width,
				  size_t height, bool in);

static void
rw_say_slow(const char *what, const char *name, uint64_t t0)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t ms = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000000ull;
	if (ms >= 50) {
		rw_print("%s %s took %llu ms\n", what, name ? name : "?", (unsigned long long)ms);
	}
}
/* A command buffer that took long to run, said with what was in it: under QEMU, where the
 * shaders run on the processor, how long a frame of the window server's takes and whether it
 * waited for another thread's work first. The first 40 of a process, then one in 50. */
/* the command buffers run so far: what a no-copy buffer looks at its caller's pages again once per */
static unsigned rw_commit_epoch;

static void
rw_say_slow_buffer(unsigned draws, unsigned dispatches, unsigned blits, uint64_t t_wait,
                   uint64_t t0)
{
	static unsigned said;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t now = mach_absolute_time();
	uint64_t ms = (now - t0) * tb.numer / tb.denom / 1000000ull;
	uint64_t waited = (t0 - t_wait) * tb.numer / tb.denom / 1000000ull;
	if (ms < 1000 && waited < 1000) {
		return;
	}
	unsigned n = said++;
	if (n < 40 || n % 50 == 0) {
		rw_print("a command buffer of %u draws, %u dispatches and %u blits took %llu ms, after "
		         "waiting %llu ms for the context\n", draws, dispatches, blits,
		         (unsigned long long)ms, (unsigned long long)waited);
	}
}
#import <Metal/Metal.h>          /* for MTLSize and the public enumerations, nothing private */
#include <fcntl.h>
#include <unistd.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <pthread.h>

/*
 * The compiler library's one Gallium context, which is not thread-safe: the window server writes textures
 * (-replaceRegion:) and reads them from other threads than the one that commits, and two threads in the context at
 * once corrupt its state -- textures with other textures' texels in them, blocks of colour. Every call that reaches the
 * context holds this (recursive: a commit refreshes textures from surfaces through -replaceRegion:).
 */
static pthread_mutex_t rw_pipe_lock;
static pthread_once_t rw_pipe_once = PTHREAD_ONCE_INIT;

static void
rw_pipe_init(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&rw_pipe_lock, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void
rw_pipe_enter(void)
{
	pthread_once(&rw_pipe_once, rw_pipe_init);
	pthread_mutex_lock(&rw_pipe_lock);
}

static void
rw_pipe_leave(void)
{
	pthread_mutex_unlock(&rw_pipe_lock);
}

/* the keys of a specialised function's associated objects (rw_specialize, at the end of this file) */
static char rw_key_base, rw_key_values, rw_key_air;
/* CoreAnimation's generic uber shader and what is done with it (at the end of this file) */
static int rw_generic_mode(void);
static bool rw_is_generic(id base, id values);

/* every call into the compiler library, under rw_pipe_lock */
#define MESA(fn, ...) ({ rw_pipe_enter(); __typeof__(mesa.fn(__VA_ARGS__)) rw_r_ = mesa.fn(__VA_ARGS__); rw_pipe_leave(); rw_r_; })
#define MESA_VOID(fn, ...) do { rw_pipe_enter(); mesa.fn(__VA_ARGS__); rw_pipe_leave(); } while (0)
#include <os/lock.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>

#include "../../../userland/userland/src/board/drawable.h"
#include "rw_null_air.h"            /* src/rw_null.metal: the generic uber shader's stand-in (rw_generic_mode) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* The contract with Mesa's side, copied here rather than included: this file is built by
 * ../../userland/userland/build.sh, which knows nothing of Mesa's tree, and the declarations are few.
 * tools/mesa/air/air_metal.h is the original and explains each call. */
struct rwa_device;
/* libhakios-air's own (tools/mesa/air/air_metal.h): one function constant's value */
/* air_metal.h's struct rwa_stage_in: a compute pipeline's stageInputDescriptor in Metal's numbers */
struct rw_stage_in {
	struct { unsigned format, offset, buffer_index; } attribute[31];
	struct { unsigned stride, step_function, step_rate; } layout[31];
	unsigned index_buffer_index, index_type;
};

/* air_metal.h's struct rwa_linked_function */
struct rw_linked_function {
	const char *name;
	const void *bitcode;
	size_t size;
	const char *entry;
};

struct rwa_constant {
	unsigned index;
	unsigned size;
	uint8_t bytes[32];
};
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
struct rwa_compute;
struct rwa_compute_info {
	unsigned max_threads_per_group, thread_execution_width, static_shared_bytes;
	unsigned buffers, textures, samplers;
	/* which of those buffer indices are ARGUMENT BUFFERS, one bit each: Metal has one
	 * buffer table and an index in it is either a set or an argument. Appended, so
	 * every field above keeps the offset this file already reads -- and this file and
	 * the library still have to be rebuilt together. */
	unsigned argbuf_mask;
};
struct rwa_texture_shape {
	unsigned pixel_format, type, width, height, depth;
	unsigned array_length, mip_levels, sample_count, usage;
};
struct rwa_sampler_state {
	unsigned address[3];
	unsigned min_filter, mag_filter, mip_filter, compare_function, max_anisotropy;
	unsigned normalized_coordinates;
	float lod_min, lod_max;
};
struct rwa_bindings {
	struct rwa_buffer *const *buffers;
	unsigned buffer_count;
	struct rwa_texture *const *textures;
	unsigned texture_count;
	struct rwa_sampler *const *samplers;
	unsigned sampler_count;
};
/* ARGUMENT BUFFERS. There is no argument buffer to write on this driver: the frontend
 * resolves every member of a set to a binding while translating, and Metal's 8-byte
 * handles are an Apple GPU's descriptors, which a Gallium driver cannot read. So
 * MTLArgumentEncoder RECORDS (resource, Metal id) against (destination buffer,
 * destination offset) and the draw binds what it finds -- air_metal.h carries the whole
 * of why, including why an argument buffer nobody encoded has to be refused by name. */
#define RWA_MAX_ARGBUF_MEMBERS 64
enum rwa_argbuf_kind {
	RWA_ARGBUF_BUFFER = 0,
	RWA_ARGBUF_TEXTURE = 1,
	RWA_ARGBUF_SAMPLER = 2,
	RWA_ARGBUF_VALUE = 3,    /* bytes in the set itself: -constantDataAtIndex: */
};
struct rwa_argbuf_member {
	unsigned id;             /* Metal's [[id(n)]], which the application names */
	unsigned kind;           /* enum rwa_argbuf_kind */
	unsigned index;          /* the flat index the frontend allocated */
	unsigned write;
	char name[48];
	unsigned offset, size;   /* RWA_ARGBUF_VALUE: its bytes in an encoding */
};
struct rwa_argbuf_layout {
	unsigned metal_index;    /* the [[buffer(n)]] the set is bound at */
	unsigned encoded_length; /* the set's air.buffer_size: Apple's own number */
	unsigned alignment;
	unsigned member_count;
	struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];
};
/* Metal's buffer table for one stage: what is bound at each index AND AT WHAT OFFSET,
 * which `rwa_bindings.buffers` has no room for. One array for both of the things an
 * offset means, because Metal has one -setFragmentBuffer:offset:atIndex: behind both: for
 * an argument buffer it is WHICH ENCODING in that buffer to read, and for a buffer
 * argument it is which block of it the shader's pointer starts at. The library serves a
 * `constant` one at any offset (uploading when the GPU cannot fetch from it) and refuses
 * a `device` one the GPU cannot fetch from, by name -- air_metal.h says why. */
struct rwa_buffer_bind {
	struct rwa_buffer *buffer;   /* what the application bound at this Metal index */
	size_t offset;               /* Metal's own `offset:` */
};
struct rwa_render;
struct rwa_depth_stencil;
/* MTLVertexDescriptor's two arrays, indexed by Metal's own indices: attributes[n] by the
 * shader's [[attribute(n)]] and layouts[k] by the buffer index an attribute names. A
 * format of MTLVertexFormatInvalid (0) is "not configured", which is what a fresh
 * MTLVertexDescriptor holds everywhere, so a zeroed shape carries no layout at all. */
#define RWA_MAX_VERTEX_ATTRIBUTES 31
#define RWA_MAX_VERTEX_BUFFERS    31
struct rwa_vertex_attribute {
	unsigned format, offset, buffer_index;
};
struct rwa_vertex_layout {
	unsigned stride, step_function, step_rate;
};
/* MTLRenderPipelineColorAttachmentDescriptor's blend half, per colour attachment, as
 * Metal's own numbers: the library maps MTLBlendFactor, MTLBlendOperation and
 * MTLColorWriteMask by table and refuses what it cannot serve by name. `write_mask` is
 * MTLColorWriteMask, whose bits run the OTHER WAY from Gallium's colormask (Metal's Red is
 * 1 << 3) -- which is the library's business and not this driver's: this passes the number
 * Metal gave it and reverses nothing. */
struct rwa_color_blend {
	unsigned blending_enabled;
	unsigned rgb_blend_operation, alpha_blend_operation;
	unsigned source_rgb_blend_factor, destination_rgb_blend_factor;
	unsigned source_alpha_blend_factor, destination_alpha_blend_factor;
	unsigned write_mask;
};
struct rwa_render_shape {
	unsigned color_format[8];
	unsigned color_count;
	unsigned depth_format;
	unsigned stencil_format;
	unsigned sample_count;
	struct rwa_vertex_attribute vertex_attribute[RWA_MAX_VERTEX_ATTRIBUTES];
	struct rwa_vertex_layout vertex_layout[RWA_MAX_VERTEX_BUFFERS];
	/* APPENDED, 260 bytes at the END: every field above keeps the offset this driver
	 * already writes. A field inserted in the MIDDLE of a struct shared with the library
	 * has cost this port twice -- a primitive type that arrived as 1871292816, and
	 * struct air_resource growing by eight bytes so that every resource after the first
	 * was read eight bytes early. `color_blend_count` of 0 means this shape carries NO
	 * blend state, which is blending off and all four channels written; a zeroed entry
	 * cannot stand in for that, because MTLColorWriteMaskNone is 0 and Metal's default
	 * writeMask is All. */
	struct rwa_color_blend color_blend[8];
	unsigned color_blend_count;
};
struct rwa_render_info {
	unsigned vertex_buffers, fragment_buffers, fragment_textures, fragment_samplers;
	unsigned generated_vertex_stage;
	unsigned fragment_writes_depth;
	/* what the vertex descriptor became: how many elements were built, the Metal
	 * attribute index of each (in driver_location order, which is the shader's and not
	 * Metal's), how many stream slots a draw must cover and which of them are fetched
	 * from. The mask is what splits Metal's one buffer table at the draw. */
	unsigned vertex_attribute_count;
	unsigned vertex_attribute_index[16];
	unsigned vertex_streams;
	unsigned vertex_stream_mask;
	/* and which buffer indices of each stage are argument buffers. Metal's one table per
	 * stage now holds three things -- a vertex stream, a buffer argument and a set --
	 * and only the shader and the vertex descriptor together say which an index is. */
	unsigned vertex_argbuf_mask;
	unsigned fragment_argbuf_mask;
	/* and what the blend state became: one bit per attachment that ends up blending, and
	 * four bits per attachment of colour write mask in GALLIUM's order (R 0x1 .. A 0x8),
	 * which is MTLColorWriteMask's reversed. Printed rather than trusted: for any mask
	 * but All, None, green|blue and red|alpha these four bits MUST differ from the
	 * writeMask this driver passed, so a log line where they agree is a log line saying
	 * the bits were never reversed. Filled in BY the library, so this struct must not be
	 * shorter than the library's. */
	unsigned blend_enabled_mask;
	unsigned color_write_masks;
};
struct rwa_vertex_stream {
	struct rwa_buffer *buffer;
	size_t offset;
};
struct rwa_stencil_face {
	unsigned compare_function;
	unsigned stencil_fail_operation;
	unsigned depth_fail_operation;
	unsigned depth_stencil_pass_operation;
	unsigned read_mask, write_mask;
};
struct rwa_depth_stencil_state {
	unsigned depth_compare_function;
	unsigned depth_write_enabled;
	struct rwa_stencil_face front, back;
};
struct rwa_draw {
	struct rwa_texture *const *color;
	unsigned color_count;
	const float *clear_color;
	struct rwa_texture *depth;
	const float *clear_depth;
	const unsigned *clear_stencil;
	const struct rwa_depth_stencil *depth_stencil;
	unsigned stencil_reference[2];
	const float *depth_range;
	struct rwa_bindings vertex;
	struct rwa_bindings fragment;
	unsigned primitive;
	unsigned vertex_start, vertex_count;
	/* the indexed forms: -drawIndexedPrimitives: and its instanced ones. Appended, as
	 * air_metal.h says, so that every field above keeps the offset this driver already
	 * reads -- but the library and this file still have to be rebuilt together, and
	 * `vertex_count` is the INDEX count for an indexed draw (there is deliberately no
	 * second count field for the two to disagree in). */
	struct rwa_buffer *index_buffer;
	unsigned index_type;
	size_t index_buffer_offset;
	int base_vertex;
	unsigned instance_count;
	unsigned base_instance;
	/* the vertex streams: -setVertexBuffer:offset:atIndex:, indexed by that index, and
	 * a second array beside `vertex.buffers` because a stream carries an OFFSET and is
	 * checked against the layout rather than against the shader's [[buffer(n)]]. Both
	 * come off the one table Metal has, and the pipeline's vertex_stream_mask says
	 * which index belongs to which side. */
	const struct rwa_vertex_stream *vertex_streams;
	unsigned vertex_stream_count;
	/* Metal's buffer table for each stage: the offsets `vertex.buffers` and
	 * `fragment.buffers` have nowhere to put -- an argument buffer's encoding offset
	 * and a buffer argument's own offset alike. APPENDED rather than put inside
	 * rwa_bindings, which is embedded by value above: a field added there would move
	 * `primitive` and every index field after it, which is how a primitive type once
	 * arrived as 1871292816. */
	const struct rwa_buffer_bind *vertex_buffer_table;
	unsigned vertex_buffer_table_count;
	const struct rwa_buffer_bind *fragment_buffer_table;
	unsigned fragment_buffer_table_count;
	/* MTLViewport's rectangle and MTLScissorRect, x y w h; NULL: the whole attachment
	 * (air_metal.h; at the end, so nothing before them moves) */
	const float *viewport;
	const unsigned *scissor;
	/* each colour attachment's own clear (air_metal.h) */
	const float (*clear_colors)[4];
	unsigned clear_mask;
	/* each colour attachment's mip level and slice; NULL: 0 (air_metal.h) */
	const unsigned *color_level;
	const unsigned *color_layer;
	/* the encoder's raster state: MTLCullMode, MTLWinding, MTLTriangleFillMode,
	 * MTLDepthClipMode and the depth bias (air_metal.h; at the end, as above) */
	unsigned cull_mode;
	unsigned winding;
	unsigned fill_mode;
	unsigned depth_clamp;
	float depth_bias, depth_slope_scale, depth_bias_clamp;
};

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
 *     hakiosmetal: fragment buffer 2 at offset 1164: ...
 *     hakiosmetal: no drawable: pixel format 500 is not one ...
 *
 * one after the other, and four or five Metal clients were alive in that boot (backboardd,
 * SpringBoard, chronod and this port's own application). The drawable refusal was pinned on
 * backboardd only by reading QuartzCore's own os_log line beside it, which is a guess, and
 * the offset refusals could not be attributed at all. A driver is a library: its lines are
 * written by somebody else's process, and the process is the first thing a reader needs.
 *
 * So the prefix is `hakiosmetal[progname pid]:`, built once per process -- getprogname()
 * is the name launchd or the shell gave it, which is what the log's other lines carry too.
 *
 * It flushes, for the reason RW_ONCE below flushes: stdio to a console is block-buffered,
 * the processes that reach these sites are the ones that then abort, and a line written and
 * not flushed dies with the process. One flush per sentence costs nothing next to the draw
 * that provoked it.
 */
static void rw_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* and the one method this driver installs into Metal's own function class, whose reason
 * is a measurement and is written where it is done (the end of this file) */
static void ensure_function_encoder(void);

static void
rw_print(const char *fmt, ...)
{
	static char who[96];
	va_list ap;

	static int console = -2;
	char line[512];
	int at;

	if (who[0] == '\0') {
		snprintf(who, sizeof(who), "hakiosmetal[%s %d]", getprogname(), getpid());
	}
	/* to the console as well as stdout: a daemon's stdout (backboardd's, where CoreAnimation draws)
	 * goes nowhere under launchd, and the console is the kernel's log */
	if (console == -2) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	}
	at = snprintf(line, sizeof(line), "%s: ", who);
	va_start(ap, fmt);
	vsnprintf(line + at, sizeof(line) - (size_t)at, fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fflush(stdout);
	if (console >= 0 && !isatty(STDOUT_FILENO)) {
		(void)write(console, line, strlen(line));
	}
}

/*
 * A selector this driver does not know, answered instead of thrown. Metal's own classes
 * grow private methods with every release, and CoreAnimation calls them without asking
 * -respondsToSelector: first: Darwin 26's -[MTLCommandBuffer setResponsibleTaskIDs:count:]
 * ended backboardd 29 times in one boot, each one a reboot's worth of finding out. So every
 * class of this driver takes an unknown message, says once which one it was, and returns 0
 * (nil, NO). A method that matters will show up in the log as the first thing to implement;
 * one that does not costs nothing.
 */
static NSMethodSignature *
rw_tolerant_signature(id self, SEL _cmd, SEL selector)
{
	struct objc_super up = { self, class_getSuperclass(object_getClass(self)) };
	NSMethodSignature *(*super_sig)(struct objc_super *, SEL, SEL) = (void *)objc_msgSendSuper;
	NSMethodSignature *sig = super_sig(&up, _cmd, selector);
	char types[64] = "q@:";
	const char *name = sel_getName(selector);

	if (sig != nil) {
		return sig;
	}
	for (const char *at = name; *at != '\0' && strlen(types) < sizeof(types) - 1; at++) {
		if (*at == ':') {
			strcat(types, "q");
		}
	}
	return [NSMethodSignature signatureWithObjCTypes:types];
}

static void
rw_tolerant_forward(id self, SEL _cmd, NSInvocation *invocation)
{
	static NSMutableSet *said;
	static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
	NSString *name = [NSString stringWithFormat:@"%s %s", class_getName(object_getClass(self)),
	    sel_getName(invocation.selector)];
	long long zero = 0;
	BOOL first;

	os_unfair_lock_lock(&lock);
	if (said == nil) {
		said = [NSMutableSet new];
	}
	first = ![said containsObject:name];
	[said addObject:name];
	os_unfair_lock_unlock(&lock);
	if (first) {
		rw_print("not implemented: -[%s], answered with 0 (the first time it was sent)\n",
		    name.UTF8String);
	}
	if (invocation.methodSignature.methodReturnLength <= sizeof(zero)) {
		[invocation setReturnValue:&zero];
	}
}

__attribute__((constructor)) static void
rw_make_tolerant(void)
{
	static const char *names[] = { "HakiosMetalDevice", "HakiosMetalBuffer", "HakiosMetalTexture",
		"HakiosMetalSampler", "HakiosMetalDepthStencilState", "HakiosMetalComputePipeline",
		"HakiosMetalRenderPipeline", "HakiosMetalCommandQueue", "HakiosMetalCommandBuffer",
		"HakiosMetalRenderEncoder", "HakiosMetalComputeEncoder", "HakiosMetalArgumentEncoder" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Class c = objc_getClass(names[i]);

		if (c == Nil) {
			continue;
		}
		class_addMethod(c, @selector(methodSignatureForSelector:), (IMP)rw_tolerant_signature, "@@::");
		class_addMethod(c, @selector(forwardInvocation:), (IMP)rw_tolerant_forward, "v@:@");
	}
}

static struct {
	void *handle;
	struct rwa_device *(*device_open)(char *, size_t);
	void (*device_close)(struct rwa_device *);
	const char *(*device_name)(struct rwa_device *);
	struct rwa_buffer *(*buffer_create)(struct rwa_device *, size_t, char *, size_t);
	void *(*buffer_map)(struct rwa_buffer *);
	size_t (*buffer_size)(struct rwa_buffer *);
	void (*buffer_destroy)(struct rwa_buffer *);
	struct rwa_compute *(*compute_create)(struct rwa_device *, const void *, size_t,
					      const char *, struct rwa_compute_info *, char *,
					      size_t);
	void (*compute_destroy)(struct rwa_compute *);
	struct rwa_texture *(*texture_view)(struct rwa_texture *, unsigned, char *, size_t);
	struct rwa_texture *(*texture_create)(struct rwa_device *,
					      const struct rwa_texture_shape *, char *, size_t);
	int (*texture_write)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			     const unsigned[3], const void *, size_t, char *, size_t);
	int (*texture_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			    const unsigned[3], void *, size_t, char *, size_t);
	int (*depth_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			  const unsigned[3], float *, uint8_t *, char *, size_t);
	void (*texture_destroy)(struct rwa_texture *);
	struct rwa_sampler *(*sampler_create)(struct rwa_device *,
					      const struct rwa_sampler_state *, char *, size_t);
	void (*sampler_destroy)(struct rwa_sampler *);
	int (*dispatch)(struct rwa_compute *, const struct rwa_bindings *,
			const unsigned[3], const unsigned[3], char *, size_t);
	int (*dispatch_buffers)(struct rwa_compute *, const struct rwa_bindings *,
				const struct rwa_buffer_bind *, unsigned, const unsigned[3],
				const unsigned[3], char *, size_t);
	void (*threadgroup_memory)(struct rwa_compute *, unsigned);
	void (*threadgroup_memory_at)(struct rwa_compute *, unsigned, unsigned);
	void (*grid_threads)(struct rwa_compute *, const unsigned[3]);
	unsigned (*render_patch_vertices)(const struct rwa_render *);
	int (*clear_pass)(struct rwa_texture *const *, unsigned, unsigned, const float (*)[4],
			  struct rwa_texture *, const float *, const unsigned *, char *, size_t);
	int (*clear_pass_levels)(struct rwa_texture *const *, const unsigned *, const unsigned *, unsigned,
				 unsigned, const float (*)[4], struct rwa_texture *, const float *,
				 const unsigned *, char *, size_t);
	int (*texture_copy)(struct rwa_texture *, unsigned, unsigned, const unsigned[3], const unsigned[3],
			    struct rwa_texture *, unsigned, unsigned, const unsigned[3], char *, size_t);
	int (*buffer_copy)(struct rwa_buffer *, size_t, struct rwa_buffer *, size_t, size_t, char *, size_t);
	int (*buffer_fill)(struct rwa_buffer *, size_t, size_t, uint8_t, char *, size_t);
	int (*buffer_texture_copy)(struct rwa_buffer *, size_t, size_t, size_t, struct rwa_texture *, unsigned,
				   unsigned, const unsigned[3], const unsigned[3], int, char *, size_t);
	int (*generate_mipmaps)(struct rwa_texture *, char *, size_t);
	int (*module_argbufs)(struct rwa_device *, const void *, size_t, const char *,
			      struct rwa_argbuf_layout *, unsigned, unsigned *, char *,
			      size_t);
	int (*argbuf_encode_buffer)(struct rwa_buffer *, size_t, unsigned,
				    struct rwa_buffer *, size_t, char *, size_t);
	int (*argbuf_encode_texture)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_texture *, char *, size_t);
	int (*argbuf_encode_sampler)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_sampler *, char *, size_t);
	unsigned (*argbuf_encoded_members)(const struct rwa_buffer *, size_t);
	struct rwa_render *(*render_create)(struct rwa_device *, const void *, size_t,
					    const char *, const void *, size_t, const char *,
					    const struct rwa_render_shape *,
					    struct rwa_render_info *, char *, size_t);
	void (*render_destroy)(struct rwa_render *);
	void (*set_pipe_lock)(void (*)(void), void (*)(void));
	void (*stage_in_register)(const void *, size_t, const struct rw_stage_in *);
	void (*render_times)(const struct rwa_render *, double *);
	int (*render_draw)(struct rwa_render *, const struct rwa_draw *, char *, size_t);
	struct rwa_depth_stencil *(*depth_stencil_create)(struct rwa_device *,
							  const struct rwa_depth_stencil_state *,
							  char *, size_t);
	void (*depth_stencil_destroy)(struct rwa_depth_stencil *);
	int (*finish)(struct rwa_device *, char *, size_t);
	void (*draw_wait)(int);
	void (*constants_register)(const void *, size_t, const struct rwa_constant *, unsigned);
	/* rwa_linked_register: what a pipeline links into a stage (MTLLinkedFunctions) */
	void (*linked_register)(const void *, size_t, const struct rw_linked_function *, unsigned);
} mesa;

/* Loaded once, and its absence is an answer rather than a crash: without the GPU there is
 * no library worth loading, and the device object still has to exist. */
static bool
mesa_available(void)
{
	static bool tried;

	if (tried) {
		return mesa.handle != NULL;
	}
	tried = true;
	mesa.handle = dlopen("/hakios/lib/libhakios-air.dylib", RTLD_NOW);
	if (mesa.handle == NULL) {
		rw_print("no compiler library: %s\n", dlerror());
		return false;
	}
	*(void **)&mesa.device_open = dlsym(mesa.handle, "rwa_device_open");
	*(void **)&mesa.device_close = dlsym(mesa.handle, "rwa_device_close");
	*(void **)&mesa.device_name = dlsym(mesa.handle, "rwa_device_name");
	*(void **)&mesa.buffer_create = dlsym(mesa.handle, "rwa_buffer_create");
	*(void **)&mesa.buffer_map = dlsym(mesa.handle, "rwa_buffer_map");
	*(void **)&mesa.buffer_size = dlsym(mesa.handle, "rwa_buffer_size");
	*(void **)&mesa.buffer_destroy = dlsym(mesa.handle, "rwa_buffer_destroy");
	*(void **)&mesa.compute_create = dlsym(mesa.handle, "rwa_compute_create");
	*(void **)&mesa.compute_destroy = dlsym(mesa.handle, "rwa_compute_destroy");
	*(void **)&mesa.texture_create = dlsym(mesa.handle, "rwa_texture_create");
	*(void **)&mesa.texture_view = dlsym(mesa.handle, "rwa_texture_view");
	*(void **)&mesa.texture_write = dlsym(mesa.handle, "rwa_texture_write");
	*(void **)&mesa.texture_read = dlsym(mesa.handle, "rwa_texture_read");
	*(void **)&mesa.depth_read = dlsym(mesa.handle, "rwa_depth_read");
	*(void **)&mesa.texture_destroy = dlsym(mesa.handle, "rwa_texture_destroy");
	*(void **)&mesa.sampler_create = dlsym(mesa.handle, "rwa_sampler_create");
	*(void **)&mesa.sampler_destroy = dlsym(mesa.handle, "rwa_sampler_destroy");
	*(void **)&mesa.dispatch = dlsym(mesa.handle, "rwa_dispatch");
	/* The argument-buffer calls, looked up but NOT required, for the reason drawing is
	 * not required: a library without them is a real state of this work, and the
	 * encoder says so when an application asks for one rather than the driver refusing
	 * to load. What must not happen is answering an encoder and then binding nothing,
	 * so -newArgumentEncoder... checks for these before it makes one. */
	*(void **)&mesa.dispatch_buffers = dlsym(mesa.handle, "rwa_dispatch_buffers");
	*(void **)&mesa.threadgroup_memory = dlsym(mesa.handle, "rwa_threadgroup_memory");
	*(void **)&mesa.threadgroup_memory_at = dlsym(mesa.handle, "rwa_threadgroup_memory_at");
	*(void **)&mesa.grid_threads = dlsym(mesa.handle, "rwa_grid_threads");
	*(void **)&mesa.render_patch_vertices = dlsym(mesa.handle, "rwa_render_patch_vertices");
	*(void **)&mesa.clear_pass = dlsym(mesa.handle, "rwa_clear_pass");
	*(void **)&mesa.clear_pass_levels = dlsym(mesa.handle, "rwa_clear_pass_levels");
	*(void **)&mesa.texture_copy = dlsym(mesa.handle, "rwa_texture_copy");
	*(void **)&mesa.buffer_copy = dlsym(mesa.handle, "rwa_buffer_copy");
	*(void **)&mesa.buffer_fill = dlsym(mesa.handle, "rwa_buffer_fill");
	*(void **)&mesa.buffer_texture_copy = dlsym(mesa.handle, "rwa_buffer_texture_copy");
	*(void **)&mesa.generate_mipmaps = dlsym(mesa.handle, "rwa_texture_generate_mipmaps");
	*(void **)&mesa.module_argbufs = dlsym(mesa.handle, "rwa_module_argbufs");
	*(void **)&mesa.argbuf_encode_buffer = dlsym(mesa.handle, "rwa_argbuf_encode_buffer");
	*(void **)&mesa.argbuf_encode_texture = dlsym(mesa.handle, "rwa_argbuf_encode_texture");
	*(void **)&mesa.argbuf_encode_sampler = dlsym(mesa.handle, "rwa_argbuf_encode_sampler");
	*(void **)&mesa.argbuf_encoded_members = dlsym(mesa.handle,
						       "rwa_argbuf_encoded_members");
	/* Drawing is looked up but not required: a compiler library that can compute and not
	 * yet draw is a real state of this work, and the driver says so when asked to draw
	 * rather than refusing to exist. */
	*(void **)&mesa.render_create = dlsym(mesa.handle, "rwa_render_create");
	*(void **)&mesa.render_destroy = dlsym(mesa.handle, "rwa_render_destroy");
	*(void **)&mesa.render_draw = dlsym(mesa.handle, "rwa_render_draw");
	/* Depth likewise: a library that can draw and not test depth is a real state of
	 * this work, and -newDepthStencilStateWithDescriptor: says so when it is asked
	 * rather than the driver refusing to load. */
	*(void **)&mesa.depth_stencil_create = dlsym(mesa.handle, "rwa_depth_stencil_create");
	*(void **)&mesa.depth_stencil_destroy = dlsym(mesa.handle, "rwa_depth_stencil_destroy");
	*(void **)&mesa.finish = dlsym(mesa.handle, "rwa_finish");
	*(void **)&mesa.draw_wait = dlsym(mesa.handle, "rwa_draw_wait");
	*(void **)&mesa.constants_register = dlsym(mesa.handle, "rwa_constants_register");
	*(void **)&mesa.linked_register = dlsym(mesa.handle, "rwa_linked_register");
	/* the lock on the library's Gallium context, handed to it: with it a render pipeline is
	 * made outside the lock and the translation does not hold up the draws (air_metal.h) */
	*(void **)&mesa.set_pipe_lock = dlsym(mesa.handle, "rwa_set_pipe_lock");
	*(void **)&mesa.render_times = dlsym(mesa.handle, "rwa_render_times");
	*(void **)&mesa.stage_in_register = dlsym(mesa.handle, "rwa_stage_in_register");
	if (mesa.set_pipe_lock != NULL) {
		mesa.set_pipe_lock(rw_pipe_enter, rw_pipe_leave);
	}
	/* draws are waited for once a command buffer (-commit), not one by one */
	if (mesa.draw_wait != NULL && mesa.finish != NULL) {
		MESA_VOID(draw_wait, false);
	}
	if (mesa.device_open == NULL || mesa.compute_create == NULL || mesa.dispatch == NULL ||
	    mesa.texture_create == NULL || mesa.sampler_create == NULL) {
		/* rwa_dispatch's arguments changed when textures were added to the contract, so
		 * an older library would be called with the wrong shape of argument and do
		 * something arbitrary. Refusing it outright is the only safe reading of a
		 * missing rwa_texture_create. */
		rw_print("the compiler library is missing calls the contract names, "
		         "so it is older than this driver and will not be used\n");
		dlclose(mesa.handle);
		mesa.handle = NULL;
		return false;
	}
	return true;
}

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

/*
 * A BLEND LINE, deduplicated the way a repeated refusal is -- but on the CONTENT and not on
 * the call site, which is the difference between this and RW_AGAIN above.
 *
 * These lines are the measurement this driver exists to make, and they repeat enormously:
 * the window server built 127 pipelines in logs/080121-run.log and printed 240 attachment
 * lines, and there were THIRTEEN DISTINCT ONES among them. One boot of Resident Evil 4
 * would be a different order of magnitude -- 7 879 modules in its library -- and the
 * loader keeps the first 256 KB of a log and the last megabyte and drops the middle, which
 * is how 862 identical drawable refusals once pushed out the lines saying whether the game
 * had launched at all.
 *
 * So a line is printed the first time its exact content is seen and then on powers of two
 * of that content's own count, carrying the count. RW_AGAIN cannot do this: its counter is
 * per call site, so it would have suppressed twelve of those thirteen as if they were
 * repetitions of the first.
 *
 * Returns whether it printed, so the caller can attach the pipeline's LABEL to a line that
 * is new -- the label is what names the blend mode, and one per distinct configuration is
 * the whole of what it has to say.
 *
 * 32 entries is more than twice the thirteen a whole boot of the compositor produced, and
 * past it every further distinct line shares one counter rather than being dropped: a table
 * that silently stopped reporting would be the same instrument failure as the flood.
 */
static bool
rw_blend_line(unsigned attachment, unsigned format, const struct rwa_color_blend *b)
{
	static struct {
		unsigned attachment, format;
		struct rwa_color_blend b;
		unsigned long times;
	} seen[32];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = NULL;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].attachment == attachment && seen[i].format == format &&
		    memcmp(&seen[i].b, b, sizeof(*b)) == 0) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].attachment = attachment;
		seen[nseen].format = format;
		seen[nseen].b = *b;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == NULL) {
		times = &beyond;
	}
	if ((++*times & (*times - 1)) != 0) {
		return false;
	}
	rw_print("blend: attachment %u format %u blend %u rgb %u/%u op %u alpha %u/%u op %u "
	         "writeMask 0x%x\n", attachment, format, b->blending_enabled,
	         b->source_rgb_blend_factor, b->destination_rgb_blend_factor,
	         b->rgb_blend_operation, b->source_alpha_blend_factor,
	         b->destination_alpha_blend_factor, b->alpha_blend_operation, b->write_mask);
	if (*times > 1) {
		rw_print("  ... and that is the %lu'th pipeline asking for exactly that%s\n",
		         *times, times == &beyond ? ", counting every configuration past the "
		                                    "thirty-two this keeps apart" : "");
	}
	return true;
}

/* The same, for what the LIBRARY made of it: four distinct lines in that whole boot. */
static void
rw_blend_result(unsigned enabled_mask, unsigned write_masks)
{
	static struct { unsigned enabled, masks; unsigned long times; } seen[16];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = NULL;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].enabled == enabled_mask && seen[i].masks == write_masks) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].enabled = enabled_mask;
		seen[nseen].masks = write_masks;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == NULL) {
		times = &beyond;
	}
	if ((++*times & (*times - 1)) != 0) {
		return;
	}
	rw_print("blend: the library blends attachments 0x%x and writes 0x%08x (four Gallium "
	         "bits per attachment, R 0x1 .. A 0x8)\n", enabled_mask, write_masks);
	if (*times > 1) {
		rw_print("  ... and that is the %lu'th pipeline it made that of\n", *times);
	}
}

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
 * only thing that gives it back (see -[HakiosMetalRenderEncoder endEncoding]). */
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

@interface HakiosMetalDevice : _MTLDevice {
	io_service_t _acceleratorService;   /* the IOAcceleratorES service Metal handed us */
	io_connect_t _gpu;                  /* this port's own gpu client, or IO_OBJECT_NULL */
	struct rwa_device *_mesa;           /* Mesa's screen and context, or NULL */
	BOOL _assertions;                   /* Metal's own assertion switch, stored for it */
	struct MTLTargetDeviceArch _target;     /* answered by pointer; see -targetDeviceInfo */
	char _targetName[96];               /* the bytes _target.name points at, inside the object */
	id _architecture;                   /* Darwin 26: -targetDeviceArchitecture's object, made once */
}
/* Not part of any Metal protocol: how this driver's own objects reach the compiler. */
- (struct rwa_device *)mesaDevice;
/* and the argument encoder of one function's `[[buffer(n)]]` set, which the method this
 * driver installs into Metal's own _MTLFunction forwards to (the end of this file says
 * why it has to): the layout is read out of the function's own bitcode, which is the one
 * thing Metal has here and its reflection has not. */
- (id)hakiosArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index;
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
 * (iokit/Kernel/BoardGPU.cpp: kMsmGetParam = 0x40, struct msm_param). The parameters
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

/* The kernel's gpu interface, by the numbers its own header gives
 * (iokit/Kernel/BoardGPU.cpp). */
enum {
	kDrmGemClose  = 0x09,
	kMsmGetParam  = 0x40,
	kMsmGemNew    = 0x42,
	kMsmGemInfo   = 0x43,
};
#define MSM_INFO_GET_IOVA  1

struct gem_new   { uint64_t size; uint32_t flags, handle; };
struct gem_info  { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct gem_close { uint32_t handle, pad; };

#define RW_MAX_BUFFERS 32

@class HakiosMetalCommandQueue;
@class HakiosMetalWork;

@interface HakiosMetalComputePipeline : _MTLComputePipelineState {
	HakiosMetalDevice *_owner;
	struct rwa_compute *_compute;
	struct rwa_compute_info _info;
	NSArray *_linked;   /* the linked functions, in the library's order (rw_register_linked) */
}
- (void)hakiosSetLinked:(NSArray *)linked;
@end

/*
 * VISIBLE FUNCTION TABLES. A function handle is a linked function's place in its pipeline's
 * list, plus one (rw_register_linked); a table is a buffer of eight-byte handles, which an encoder
 * binds at the table's index as it binds a buffer, and which the library reads where the shader
 * takes a function out of it (nir_air.h, function_tables). CoreImage's ubershader calls its
 * kernels so.
 */
@interface HakiosMetalFunctionHandle : NSObject {
	id _device, _function;
	uint64_t _handle;
}
- (instancetype)initWithDevice:(id)device function:(id)function handle:(uint64_t)handle;
- (uint64_t)hakiosHandle;
@end

@interface HakiosMetalVisibleFunctionTable : NSObject {
	id _device, _buffer;
	NSUInteger _count;
	NSString *_label;
}
- (instancetype)initWithDevice:(id)device count:(NSUInteger)count;
- (id)hakiosBuffer;
@end

/* the handle of `function` among `linked`, as functionHandleWithFunction: answers it */
static id
rw_function_handle(id device, NSArray *linked, id function)
{
	NSUInteger k = [linked indexOfObjectIdenticalTo:function];
	if (k == NSNotFound && [function respondsToSelector:@selector(name)]) {
		for (NSUInteger i = 0; i < [linked count]; i++) {
			if ([[linked[i] name] isEqualToString:[function name]]) {
				k = i;
				break;
			}
		}
	}
	if (k == NSNotFound) {
		return nil;   /* a function the pipeline does not link: Metal answers nil as well */
	}
	return [[[HakiosMetalFunctionHandle alloc] initWithDevice:device function:function
							   handle:k + 1] autorelease];
}

/* One recorded dispatch. Metal's rule is that nothing runs until the command buffer is
 * committed, so an encoder records and the command buffer runs; dispatching at encode time
 * would be less code and would quietly reorder work. */
@interface HakiosMetalComputePipeline (HakiosMetalInternal)
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor;
- (struct rwa_compute *)compute;
@end

@interface HakiosMetalCommandQueue : _MTLCommandQueue
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device descriptor:(id)descriptor;
@end

@interface HakiosMetalCommandBuffer : _MTLCommandBuffer
- (instancetype)initWithHakiosQueue:(HakiosMetalCommandQueue *)queue;
- (void)recordWork:(HakiosMetalWork *)work;
- (id)device;
@end

@interface HakiosMetalComputeEncoder : _MTLCommandEncoder
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer;
@end

@interface HakiosMetalBlitEncoder : _MTLCommandEncoder
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer;
@end

@interface HakiosMetalRenderEncoder : _MTLCommandEncoder
- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer
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

#define RW_BISECT_GROUPS 6
#define RW_BISECT_PHASE 5

static uint64_t rw_bisect_start(void);     /* boot-arg metal_bisect (see -runDraw:) */
/* the names of metal_bisect_names, or of /hakios/bisect.txt (one a line), as "a,b,c"; "" if none */
static const char *
rw_bisect_names(void)
{
	static char names[4096];
	static int known;

	if (!known) {
		char args[1024] = { 0 }, *at;
		size_t size = sizeof(args) - 1;
		FILE *f;

		known = 1;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_bisect_names=")) != NULL) {
			at += strlen("metal_bisect_names=");
			for (size_t i = 0; at[i] && at[i] != ' ' && i < sizeof(names) - 1; i++) {
				names[i] = at[i];
			}
		} else if (strstr(args, "metal_bisect=") != NULL && (f = fopen("/hakios/bisect.txt", "r")) != NULL) {
			char line[256];
			size_t n = 0;

			while (fgets(line, sizeof(line), f) != NULL) {
				size_t l = strcspn(line, "\r\n ");

				if (l == 0 || n + l + 2 >= sizeof(names)) {
					continue;
				}
				if (n) {
					names[n++] = ',';
				}
				memcpy(names + n, line, l);
				n += l;
			}
			fclose(f);
		}
	}
	return names;
}
/* how many phases: RW_BISECT_GROUPS, or the number of names in metal_bisect_names */
static int
rw_bisect_phases(void)
{
	static int phases;

	if (phases == 0) {
		char args[1024] = { 0 }, *at;
		size_t size = sizeof(args) - 1;

		phases = RW_BISECT_GROUPS;
		(void)args; (void)at; (void)size;
		if (*rw_bisect_names()) {
			phases = 1;
			for (const char *c = rw_bisect_names(); *c; c++) {
				phases += *c == ',';
			}
		}
	}
	return phases;
}
static int
rw_bisect_within(void)
{
	static int known, group = -1;

	if (!known) {
		char args[1024] = { 0 }, *at;
		size_t size = sizeof(args) - 1;

		known = 1;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_bisect_group=")) != NULL) {
			group = atoi(at + strlen("metal_bisect_group="));
		}
	}
	return group;
}

@interface HakiosMetalRenderPipeline : _MTLRenderPipelineState {
	HakiosMetalDevice *_owner;
	struct rwa_render *_render;
	struct rwa_render_info _info;
	NSArray *_linked;   /* the fragment stage's linked functions (rw_register_linked) */
@public
	unsigned _bisectGroup;          /* boot-arg metal_bisect: which group of pipelines it is in (its label's hash) */
	char _names[128];               /* "vertex/fragment" function names, for boot-arg metal_sync's log */
}
- (struct rwa_render *)render;
- (void)hakiosSetLinked:(NSArray *)linked;
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

@interface HakiosMetalWork : NSObject {
@public
	HakiosMetalComputePipeline *pipeline;
	HakiosMetalRenderPipeline *render;      /* set instead for a draw */
	id color[8];
	unsigned colorCount;
	unsigned colorLevel[8], colorSlice[8];  /* each attachment's mip level and slice */
	float clearColor[8][4];
	unsigned clearMask;
	BOOL clearOnly;                 /* a pass that drew nothing: its load actions alone */
	BOOL clears;
	/* the depth/stencil attachment of the pass, its two load actions and the state
	 * bound when the draw was encoded. Separate flags per aspect because Metal keeps
	 * two attachment descriptors over one texture and a pass that clears the depth and
	 * keeps the stencil is ordinary. */
	id depth;
	id depthStencilState;
	float clearDepth;
	unsigned clearStencil;
	BOOL clearsDepth, clearsStencil;
	unsigned stencilReference[2];
	float depthRange[2];
	BOOL hasDepthRange;
	float viewport[4];              /* x, y, width, height; hasViewport NO: the whole attachment */
	BOOL hasViewport;
	unsigned scissor[4];
	BOOL hasScissor;
	unsigned primitive, vertexStart, vertexCount;
	/* the index buffer of an indexed draw and the three numbers that come with it in
	 * the same Metal call; indexBuffer nil is -drawPrimitives:, and then vertexCount
	 * counts vertices rather than indices */
	id indexBuffer;
	unsigned indexType;
	size_t indexBufferOffset;
	int baseVertex;
	unsigned instanceCount, baseInstance;
	id vertexBuffers[RW_MAX_BUFFERS];
	size_t vertexBufferOffsets[RW_MAX_BUFFERS];
	unsigned vertexBufferCount;
	id buffers[RW_MAX_BUFFERS];
	/* Metal's own `offset:` for each of them, which until argument buffers existed was
	 * refused at the encoder as unimplemented. It is RECORDED here and judged at the
	 * draw instead, because which indices may carry one is the pipeline's answer: a
	 * member of an argument buffer has its own offset and a buffer argument still has
	 * none. */
	size_t bufferOffsets[RW_MAX_BUFFERS];
	id textures[RW_MAX_BUFFERS];
	id samplers[RW_MAX_BUFFERS];
	float samplerLod[RW_MAX_BUFFERS][2];    /* an encoder's LOD clamp for that sampler ... */
	BOOL samplerClamped[RW_MAX_BUFFERS];    /* ... when it set one */
	BOOL retained;                  /* -retainResources has run: dealloc gives them back */
	unsigned count, textureCount, samplerCount;
	unsigned groups[3], size[3];
	unsigned threads[3];            /* -dispatchThreads:'s count when the groups do not divide it, or 0 */
	/* An indirect draw or dispatch: where its arguments are, read when the work runs and not
	 * when it is recorded -- a kernel earlier in the same command buffer may be what writes
	 * them, and the work runs in order, each dispatch waited for (rwa_dispatch). */
	id indirectBuffer;
	size_t indirectOffset;
	/* the encoder's raster state at the draw (rwa_draw.cull_mode and after) */
	unsigned cullMode, winding, fillMode, depthClamp;
	float depthBias[3];
	unsigned threadgroupBytes[32];  /* -setThreadgroupMemoryLength:atIndex:, by index */
	/* a blit (HakiosMetalBlitEncoder): which kind, from what to what, where */
	unsigned blit;
	id blitFrom, blitTo;
	unsigned fromSlice, fromLevel, fromOrigin[3], blitSize[3], toSlice, toLevel, toOrigin[3];
	size_t fromOffset, toOffset, blitBytes, bytesPerRow, bytesPerImage;
	uint8_t fillValue;
}
- (void)retainResources;
@end

@interface HakiosMetalTexture : _MTLResource {
	HakiosMetalDevice *_owner;
	struct rwa_texture *_texture;
	struct rwa_texture_shape _shape;
	int _responsibleProcess;   /* as on the buffer: accounting, not behaviour */
	/* a copy of a surface the GPU could not take (newTextureWithDescriptor:iosurface:plane:): the surface, and
	 * what is needed to copy it again while its owner is still drawing into it */
	IOSurfaceRef _copiedFrom;
	/* ... or of part of a buffer (-[HakiosMetalBuffer newTextureWithDescriptor:offset:bytesPerRow:]) */
	id _copiedFromBuffer;
	NSUInteger _bufferOffset, _bufferRow;
	uint64_t _copiedAt, _copiedHash;
	unsigned _copiedStable;
	uint32_t _copiedSeed;      /* the surface's seed when last copied: a writer that locked it changes it */
}
- (void)copyFromSurface:(IOSurfaceRef)surface;
- (void)copyFromBuffer:(id)buffer offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row;
- (void)writeBackToSurface;
@property (readonly) struct rwa_texture *mesaTexture;
/* Not part of MTLTexture: the depth or stencil aspect of this texture, one float and one
 * byte per texel. Metal's own -getBytes: has no aspect to name and the compiler library
 * refuses a depth texture through it by name, so a caller that wants to know what a depth
 * buffer holds asks for an aspect here instead. Either pointer may be NULL. */
- (BOOL)hakiosReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region;
@end

@interface HakiosMetalSampler : _MTLSamplerState {
	struct rwa_sampler *_sampler;
	struct rwa_sampler_state _state;
	/* the same state with an encoder's LOD clamps (-mesaSamplerWithLodMin:max:), made once each */
	struct { float min, max; struct rwa_sampler *sampler; } _clamped[4];
	unsigned _clampedCount;
}
@property (readonly) struct rwa_sampler *mesaSampler;
- (struct rwa_sampler *)mesaSamplerWithLodMin:(float)lodMin max:(float)lodMax;
- (void)setHakiosState:(const struct rwa_sampler_state *)state;
@end

@interface HakiosMetalDepthStencilState : _MTLDepthStencilState {
	struct rwa_depth_stencil *_state;
	BOOL _readsDepth, _writesDepth, _readsStencil, _writesStencil;
}
@property (readonly) struct rwa_depth_stencil *mesaDepthStencil;
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor;
@end

/*
 * MTLArgumentEncoder. Not a writer: the records live in the compiler library with the
 * destination buffer, because the frontend has already resolved every member of every set
 * to a binding and Metal's 8-byte handles are an Apple GPU's descriptors (air_metal.h).
 * The implementation below carries the whole of why.
 */
@interface HakiosMetalArgumentEncoder : NSObject
- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule;
/* Not part of MTLArgumentEncoder: the mapping between Metal's [[id(n)]] and the index the
 * GPU is programmed with, so a test can read it back instead of trusting it. */
- (NSUInteger)hakiosMemberCount;
- (NSUInteger)hakiosMetalIndex;
- (BOOL)hakiosMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index;
@end

@interface HakiosMetalBuffer : _MTLResource {
	HakiosMetalDevice *_owner;
	io_connect_t _gpu;        /* not owned: the device holds it */
	uint32_t _handle;         /* set on the kernel path only */
	uint64_t _iova;
	void *_contents;
	NSUInteger _length;
	struct rwa_buffer *_mesa; /* set instead when the memory came from Mesa */
	int _responsibleProcess;  /* what <MTLResourceSPI> calls the owner, for accounting */
	const void *_callerBytes; /* newBufferWithBytesNoCopy: the caller's pages this is a copy of */
	uint64_t _callerHash;     /* ... as they were when last copied in */
	unsigned _callerEpoch;    /* ... and the command buffer that last looked */
}
@property (readonly) struct rwa_buffer *mesaBuffer;
- (void)followCallerBytes:(const void *)bytes;
- (instancetype)initWithDevice:(HakiosMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length;
@end

@interface HakiosMetalDeallocNote : NSObject {
@public
	void (^_block)(void *, NSUInteger);
	void *_bytes;
	NSUInteger _length;
}
@end

/* Whether boot-arg KEY (e.g. "metal_skip=") names this draw: a comma-separated list of substrings of the
 * pipeline's "vertex/fragment" function names, each with an optional ":N" for its draws of more than N vertices. */
static BOOL
rw_draw_chosen(const char *key, const char *names, unsigned vertices)
{
	static struct { const char *key; char list[256]; } seen[4];
	const char *list = NULL;

	for (unsigned i = 0; i < 4; i++) {
		if (seen[i].key == key) {
			list = seen[i].list;
			break;
		}
		if (seen[i].key == NULL) {
			char args[1024] = "";
			size_t sz = sizeof(args) - 1;
			const char *at;

			if (sysctlbyname("kern.bootargs", args, &sz, NULL, 0) == 0 && (at = strstr(args, key)) != NULL) {
				at += strlen(key);
				memcpy(seen[i].list, at, MIN(strcspn(at, " "), sizeof(seen[i].list) - 1));
			} else if (!strcmp(key, "metal_skip=")) {
				/* by default, RenderBox's paths drawn by its uber shader: uber_fragment stops the
				 * Adreno 830 in them, every time and for the rest of the boot (runs glass25-32;
				 * the GPU's not-yet-understood half of it). Text and shapes drawn that way are
				 * missing instead. metal_skip=none draws them. */
				strlcpy(seen[i].list, "uber_vertex:4", sizeof(seen[i].list));
			}
			seen[i].key = key;
			list = seen[i].list;
			break;
		}
	}
	for (const char *name = list ?: ""; *name;) {
		size_t n = strcspn(name, ",");
		char want[96] = { 0 };
		unsigned most = ~0u;

		memcpy(want, name, MIN(n, sizeof(want) - 1));
		char *colon = strchr(want, ':');
		if (colon != NULL) {
			*colon = 0;
			most = (unsigned)strtoul(colon + 1, NULL, 10);
		}
		if (want[0] && strstr(names, want) != NULL && (most == ~0u || vertices > most)) {
			return YES;
		}
		name += n + (name[n] == ',');
	}
	return NO;
}

@implementation HakiosMetalDeallocNote
- (void)dealloc
{
	if (_block != NULL) {
		_block(_bytes, _length);
		[_block release];
	}
	[super dealloc];
}
@end

/*
 * MTLLinkedFunctions registered against a stage's bytes for the library to translate in
 * (rwa_linked_register): `groups` name them as the stage's calls do, `functions` and
 * `privateFunctions` by their own names. Answers the functions in the order they were
 * registered, which is the order the library numbers them in for a visible function table
 * (nir_air.h, function_tables: a handle is one more than the place here), or nil.
 */
static NSData *rw_function_air(id function);

static NSArray *
rw_register_linked(id linked, NSData *stage_air, NSString *label)
{
	if (linked == nil || mesa.linked_register == NULL || stage_air == nil) {
		return nil;
	}
	NSMutableArray *fns = [NSMutableArray array], *names = [NSMutableArray array];
	for (id fn in ([linked respondsToSelector:@selector(functions)] ? [linked functions] : nil)) {
		[fns addObject:fn];
		[names addObject:[fn name] ?: @""];
	}
	for (id fn in ([linked respondsToSelector:@selector(privateFunctions)] ? [linked privateFunctions] : nil)) {
		[fns addObject:fn];
		[names addObject:[fn name] ?: @""];
	}
	NSDictionary *groups = [linked respondsToSelector:@selector(groups)] ? [linked groups] : nil;
	for (NSString *group in groups) {
		for (id fn in groups[group]) {
			[fns addObject:fn];
			[names addObject:group];
		}
	}
	struct rw_linked_function list[8];
	NSData *keep[8];
	NSMutableArray *registered = [NSMutableArray array];
	unsigned count = 0;
	for (NSUInteger k = 0; k < [fns count] && count < 8; k++) {
		NSData *air = rw_function_air(fns[k]);
		if (air == nil) {
			continue;
		}
		keep[count] = air;
		list[count] = (struct rw_linked_function){
			.name = strdup([names[k] UTF8String] ?: ""),
			.bitcode = [air bytes], .size = [air length],
			.entry = strdup([[fns[k] name] UTF8String] ?: ""),
		};
		rw_print("pipeline %s: links %s as %s (%lu bytes)\n", [label UTF8String] ?: "(no label)",
			 list[count].entry, list[count].name, (unsigned long)[air length]);
		[registered addObject:fns[k]];
		count++;
	}
	if (count > 0) {
		mesa.linked_register([stage_air bytes], [stage_air length], list, count);
	}
	for (unsigned i = 0; i < count; i++) {
		free((void *)list[i].name);
		free((void *)list[i].entry);
		keep[i] = nil;
	}
	return count > 0 ? registered : nil;
}

@implementation HakiosMetalDevice

+ (void)load
{
	ensure_spi(self);
}

/*
 * Boot-arg metal_procs="name name ...": the programs this device exists in; elsewhere Metal
 * finds no device, and a framework that has a way without one takes it. The window server's
 * compositor is what this driver serves well; RenderBox (SwiftUI's renderer) wants visible
 * function tables, vector function constants and framebuffer reads it does not have yet, and
 * the home screen came out black through it while its CPU path draws it whole. Unset: every
 * program.
 */
static BOOL
rw_metal_wanted_here(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at, *me = getprogname();
	size_t n = strlen(me);

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 ||
	    (at = strstr(args, "metal_procs=")) == NULL) {
		return YES;
	}
	at += strlen("metal_procs=");
	if (*at == '"') {
		at++;
	}
	/* the names, separated by commas, up to the next space */
	while (*at != '\0' && *at != ' ' && *at != '"') {
		const char *end = at;

		while (*end != '\0' && *end != ',' && *end != ' ' && *end != '"') {
			end++;
		}
		if ((size_t)(end - at) == n && strncmp(at, me, n) == 0) {
			return YES;
		}
		at = *end == ',' ? end + 1 : end;
	}
	return NO;
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	ensure_spi([self class]);

	if (!rw_metal_wanted_here()) {
		RW_ONCE("not a program named in metal_procs=: no Metal device here\n");
		/* not released: -[_MTLDevice dealloc] of a device its -init never ran on
		 * dispatch_release()s a NULL queue */
		return nil;
	}
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
	io_service_t gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));

	_gpu = IO_OBJECT_NULL;
	if (gpu == IO_OBJECT_NULL) {
		rw_print("no HakiosGPU service (the gpu was not started)\n");
	} else {
		kern_return_t kr = IOServiceOpen(gpu, mach_task_self(), 0, &_gpu);

		rw_print("HakiosGPU client %s (%#x)\n",
		         kr == KERN_SUCCESS ? "opened" : "refused", kr);
		IOObjectRelease(gpu);
	}

	/* And the compiler, if it is there. Its absence is not fatal: the device still has to
	 * exist and answer, it just cannot make a pipeline (and says so when asked). */
	if (mesa_available()) {
		char why[256] = { 0 };

		_mesa = MESA(device_open, why, sizeof(why));
		rw_print("the compiler library %s\n",
		         _mesa != NULL ? MESA(device_name, _mesa) : why);
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
		MESA_VOID(device_close, _mesa);
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
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		profile = 0;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "metal_profile=")) != NULL && (at == args || at[-1] == ' ')) {
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

static NSData *rw_function_air(id function);

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
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
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
					     @"was not started (gpu=1) or libhakios-air.dylib is "
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
	NSData *fragment_air = rw_function_air(fragment);
	const char *fragment_entry = [[fragment name] UTF8String];
	if (rw_generic_mode() == 2 &&
	    rw_is_generic(objc_getAssociatedObject(fragment, &rw_key_base), objc_getAssociatedObject(fragment, &rw_key_values))) {
		static unsigned stood_in;
		if (++stood_in <= 4 || stood_in % 50 == 0) {
			rw_print("pipeline %s: %s is the generic uber shader; drawn with the stand-in that draws nothing "
			         "(%u so far, boot-arg metal_generic)\n", [[descriptor label] UTF8String] ?: "(no label)",
			         fragment_entry, stood_in);
		}
		fragment_air = [NSData dataWithBytesNoCopy:(void *)rw_null_air length:rw_null_air_len freeWhenDone:NO];
		fragment_entry = "rw_null_fragment";
	}
	NSData *vertex_air = vertex != nil ? rw_function_air(vertex) : nil;

	if (fragment_air == nil || [fragment_air length] == 0) {
		return [self fail:error with:@"the fragment function has no bitcode"];
	}

	/* MTLLinkedFunctions: the functions a stage calls through NAME.MTL_VISIBLE_FN_REF (RenderBox's
	 * custom effects -- SwiftUI shaders such as MercuryPoster's wallpaper -- as custom_fn), registered
	 * against the fragment's bytes for the library to translate in (rw_register_linked) */
	NSArray *fragmentLinked = nil;
	if ([descriptor respondsToSelector:@selector(fragmentLinkedFunctions)]) {
		fragmentLinked = rw_register_linked([descriptor fragmentLinkedFunctions], fragment_air,
						    [descriptor label]);
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
		unsigned format = attachment != nil ? (unsigned)[attachment pixelFormat] : 0;

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
	bool blend_is_new = false;

	for (unsigned i = 0; i < shape.color_count; i++) {
		blend_is_new |= rw_blend_line(i, shape.color_format[i],
		                              &shape.color_blend[i]);
	}
	/* and the label, on a pipeline whose blend configuration has not been seen before:
	 * one per distinct configuration is the whole of what the label has to say, and the
	 * compositor has over a hundred of them per boot */
	if (blend_is_new) {
		NSString *label = [descriptor label];

		rw_print("blend: ... and the first such pipeline's label is %s\n",
		         label != nil ? [label UTF8String] : "(none)");
	}
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
		unsigned configured = 0, highest = 0;

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
			configured++;
			if ((unsigned)[a bufferIndex] > highest) {
				highest = (unsigned)[a bufferIndex];
			}
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
		if (configured > 0) {
			/* the layout itself, once for every different one: which format at which
			 * offset of which buffer, and each buffer's stride -- what a picture whose
			 * vertex colours come out wrong has to be checked against */
			static NSMutableSet *seen;
			NSMutableString *what = [NSMutableString stringWithFormat:@"%s:",
			    vertex != nil ? [[vertex name] UTF8String] : "-"];

			for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
				if (shape.vertex_attribute[i].format != 0) {
					[what appendFormat:@" a%u=fmt%u@%u/b%u", i, shape.vertex_attribute[i].format,
					    shape.vertex_attribute[i].offset, shape.vertex_attribute[i].buffer_index];
				}
			}
			for (unsigned k = 0; k < RWA_MAX_VERTEX_BUFFERS; k++) {
				if (shape.vertex_layout[k].stride != 0) {
					[what appendFormat:@" b%u stride %u step %u", k, shape.vertex_layout[k].stride,
					    shape.vertex_layout[k].step_function];
				}
			}
			@synchronized ([HakiosMetalDevice class]) {
				if (seen == nil) {
					seen = [NSMutableSet new];
				}
				if (![seen containsObject:what]) {
					[seen addObject:what];
					rw_print("vertex layout %s\n", what.UTF8String);
				}
			}
		}
	}

	/* Which functions each of CoreAnimation's pipelines is made of, once per label: its labels name the
	 * uber shader's configuration (PBGRABsovXm_TvrbA2Xhfn_... is a variable blur), and whether that is a
	 * specialised fixed_frag, an unspecialised one, or one of the plain shaders decides what it draws. */
	{
		static NSMutableSet *named;
		NSString *label = [descriptor label] ?: @"(no label)";
		BOOL first = NO;
		@synchronized ([HakiosMetalDevice class]) {
			if (named == nil) {
				named = [NSMutableSet new];
			}
			if (![named containsObject:label] && [named count] < 400) {
				[named addObject:label];
				first = YES;
			}
		}
		if (first) {
			rw_print("pipeline %s: %s + %s%s\n", label.UTF8String,
			         vertex != nil ? [[vertex name] UTF8String] : "(generated)", [[fragment name] UTF8String],
			         objc_getAssociatedObject(fragment, &rw_key_values) != nil ? " (specialised)" : "");
		}
	}

	char why[512] = { 0 };
	struct rwa_render_info info = { 0 };
	uint64_t t0 = mach_absolute_time();
	struct rwa_render *render;
	if (mesa.set_pipe_lock != NULL) {
		/* the library takes the lock itself, around the part that uses the context */
		render = mesa.render_create(_mesa,
		    vertex_air != nil ? [vertex_air bytes] : NULL,
		    vertex_air != nil ? [vertex_air length] : 0,
		    vertex != nil ? [[vertex name] UTF8String] : NULL,
		    [fragment_air bytes], [fragment_air length], fragment_entry,
		    &shape, &info, why, sizeof(why));
	} else {
		render = MESA(render_create, _mesa,
		    vertex_air != nil ? [vertex_air bytes] : NULL,
		    vertex_air != nil ? [vertex_air length] : 0,
		    vertex != nil ? [[vertex name] UTF8String] : NULL,
		    [fragment_air bytes], [fragment_air length], fragment_entry,
		    &shape, &info, why, sizeof(why));
	}
	rw_say_slow("render pipeline", [[fragment name] UTF8String], t0);
	/* and for one that took long, which part (under QEMU the question was whether it was the
	 * translation, the compiling, or waiting for the lock: what is not in the parts is the wait) */
	if (render != NULL && mesa.render_times != NULL) {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		uint64_t ms = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000000ull;
		if (ms >= 1000) {
			double part[5];
			mesa.render_times(render, part);
			rw_print("  ... of which: fragment %.0f ms, vertex %.0f, link %.0f, compiled %.0f + %.0f, "
			         "and the rest waiting\n", part[0], part[1], part[2], part[3], part[4]);
		}
	}

	if (render == NULL) {
		/* PRINTED as well as returned, and RW_AGAIN because the same specification
		 * is asked for over and over: -fail: puts the sentence in an NSError and
		 * nothing more, and whether anyone ever sees it is then the caller's
		 * business -- CoreAnimation discards this error and retries. A refusal that
		 * reaches no log is a refusal nobody can grep for, which makes the whole
		 * "refused by name" discipline unobservable from a boot: one pipeline
		 * specification was declined 80 times over 42 lives of backboardd before the
		 * vertex layout existed, and that number had to be counted by hand. */
		RW_AGAIN("this render pipeline is refused: %s\n", why);
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	/* and what the library made of it, in GALLIUM's bits: for any mask but All, None,
	 * green|blue and red|alpha these nibbles must DIFFER from the writeMask printed
	 * above, because the two enumerations number their channels in opposite directions.
	 * A log where 0xe went in and 0xe came back is a log saying nothing reversed them. */
	if (shape.color_count > 0) {
		rw_blend_result(info.blend_enabled_mask, info.color_write_masks);
	}
	HakiosMetalRenderPipeline *made = [[HakiosMetalRenderPipeline alloc] initWithHakiosDevice:self
										      render:render
											info:&info
										  descriptor:descriptor];
	[made hakiosSetLinked:fragmentLinked];
	return made;
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
	RW_ONCE("hakiosmetal: a texture layout was asked for and this driver has none to give; "
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
 * an IOSurface, which the kernel hands over as a buffer (kHakiosSurface); a bare pointer into
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
	RW_ONCE("hakiosmetal: a texture over the caller's own memory was asked for, and the "
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
		.pixel_format = (unsigned)[descriptor pixelFormat],
		.type = (unsigned)[descriptor textureType],
		.width = (unsigned)[descriptor width],
		.height = (unsigned)[descriptor height],
		.depth = (unsigned)[descriptor depth],
		.array_length = (unsigned)[descriptor arrayLength],
		.mip_levels = (unsigned)[descriptor mipmapLevelCount],
		.sample_count = (unsigned)[descriptor sampleCount],
		.usage = (unsigned)[descriptor usage],
	};
	struct rwa_texture *texture = MESA(texture_create, _mesa, &shape, why, sizeof(why));

	if (texture == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[HakiosMetalTexture alloc] initWithHakiosDevice:self
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
	struct rwa_sampler *sampler = MESA(sampler_create, _mesa, &state, why, sizeof(why));

	if (sampler == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	HakiosMetalSampler *object = [[HakiosMetalSampler alloc] initWithHakiosDevice:self
										  sampler:sampler
									       descriptor:descriptor];
	[object setHakiosState:&state];
	return object;
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

	struct rwa_depth_stencil *made = MESA(depth_stencil_create, _mesa, &state, why,
								   sizeof(why));

	if (made == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[HakiosMetalDepthStencilState alloc] initWithHakiosDevice:self
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
	struct rwa_argbuf_layout layout = { 0 };
	NSUInteger count = [arguments count];

	if (mesa.argbuf_encode_buffer == NULL || mesa.module_argbufs == NULL) {
		rw_print("this compiler library has no argument buffers in it, so "
		         "an encoder would record what no draw could read\n");
		return nil;
	}
	if (count == 0 || count > RWA_MAX_ARGBUF_MEMBERS) {
		rw_print("an argument encoder of %lu members: this driver records "
		         "1 to %u\n", (unsigned long)count, RWA_MAX_ARGBUF_MEMBERS);
		return nil;
	}
	layout.metal_index = ~0u;      /* the application binds it where it likes */
	layout.alignment = 8;
	layout.member_count = (unsigned)count;
	layout.encoded_length = 8 * (unsigned)count;

	uint64_t seen = 0;
	for (NSUInteger i = 0; i < count; i++) {
		MTLArgumentDescriptor *d = [arguments objectAtIndex:i];
		NSUInteger index = [d index];
		NSUInteger type = [d dataType];
		NSUInteger array = [d arrayLength];
		unsigned kind;

		switch (type) {
		case MTLDataTypePointer: kind = RWA_ARGBUF_BUFFER; break;
		case MTLDataTypeTexture: kind = RWA_ARGBUF_TEXTURE; break;
		case MTLDataTypeSampler: kind = RWA_ARGBUF_SAMPLER; break;
		default:
			rw_print("member %lu of this argument encoder is "
			         "MTLDataType %lu, and this port binds a buffer, a texture or a "
			         "sampler member: a value inside a set is refused when the "
			         "shader is translated\n", (unsigned long)index,
			         (unsigned long)type);
			return nil;
		}
		if (array > 1) {
			rw_print("member %lu of this argument encoder is an array "
			         "of %lu descriptors, and this port's frontend refuses a member "
			         "spanning more than one descriptor index\n",
			         (unsigned long)index, (unsigned long)array);
			return nil;
		}
		if (index >= count || (seen & (1ull << index)) != 0) {
			rw_print("this argument encoder has %lu members and one of "
			         "them is index %lu: Metal's encodedLength is over the highest "
			         "index, so this driver will not answer a length for indices "
			         "with a gap or a repeat in them\n", (unsigned long)count,
			         (unsigned long)index);
			return nil;
		}
		seen |= 1ull << index;
		layout.members[i].id = (unsigned)index;
		layout.members[i].kind = kind;
		/* The flat index is the SHADER's to allocate, and there is no shader here:
		 * the draw takes it from the pipeline's own resource list, where the member
		 * is named by (set, id) exactly as the application named it. */
		layout.members[i].index = ~0u;
	}
	return [[HakiosMetalArgumentEncoder alloc] initWithHakiosDevice:self layout:&layout
							      fromModule:NO];
}

/* The Darwin 16 form, which takes a reflection object (MTLBufferBinding) instead of
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
- (id)hakiosArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (_mesa == NULL || mesa.module_argbufs == NULL) {
		rw_print("no compiler library, so the argument buffer layout of "
		         "this function cannot be read\n");
		return nil;
	}
	if (function == nil || ![function respondsToSelector:bitcode]) {
		rw_print("this function does not answer bitcodeData, so its "
		         "argument buffers cannot be read\n");
		return nil;
	}
	NSData *air = rw_function_air(function);

	if (air == nil || [air length] == 0) {
		rw_print("this function has no bitcode\n");
		return nil;
	}

	char why[512] = { 0 };
	struct rwa_argbuf_layout sets[8];
	unsigned count = 0;

	if (MESA(module_argbufs, _mesa, [air bytes], [air length],
				[[function name] UTF8String], sets,
				(unsigned)(sizeof(sets) / sizeof(sets[0])), &count, why,
				sizeof(why)) != 0) {
		/* the frontend's own sentence, passed through: it names what it refused */
		rw_print("%s\n", why);
		return nil;
	}
	for (unsigned i = 0; i < count; i++) {
		if (sets[i].metal_index != (unsigned)index) {
			continue;
		}
		return [[HakiosMetalArgumentEncoder alloc] initWithHakiosDevice:self
									  layout:&sets[i]
								      fromModule:YES];
	}
	/* Metal's own words for this, and its own reason: there is no argument buffer at
	 * that index. Ours says which indices there ARE, because an application that asked
	 * for the wrong one has no other way to find out. */
	char which[96] = "";

	for (unsigned i = 0; i < count; i++) {
		char one[16];

		snprintf(one, sizeof(one), "%s%u", i ? ", " : " at ", sets[i].metal_index);
		strlcat(which, one, sizeof(which));
	}
	rw_print("bufferIndex %lu does not identify an argument buffer: this function "
	         "declares %u%s\n", (unsigned long)index, count, which);
	return nil;
}

- (id)newCommandQueueWithDescriptor:(id)descriptor
{
	/* Apple's own -newCommandQueue and -newCommandQueueWithMaxCommandBufferCount: build a
	 * descriptor and end here, so one usually arrives; when this is called directly with
	 * nil, one is made, because the base class validates it and refuses nil. */
	id use = descriptor != nil ? [[descriptor retain] autorelease] :
	    [[[MTLCommandQueueDescriptor alloc] init] autorelease];

	return [[HakiosMetalCommandQueue alloc] initWithHakiosDevice:self descriptor:use];
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
					     @"was not started (gpu=1) or libhakios-air.dylib "
					     @"is missing"];
	}
	if (![function respondsToSelector:bitcode]) {
		return [self fail:error with:@"this function does not answer bitcodeData, so its "
					     @"AIR cannot be read"];
	}
	NSData *air = rw_function_air(function);

	if (air == nil || [air length] == 0) {
		return [self fail:error with:@"the function has no bitcode"];
	}

	/* The descriptor's stage-in, for a kernel with [[stage_in]] attributes: what the library reads
	 * them out of (rwa_stage_in_register). VFX's particle kernels and CoreRE3DGS's take theirs so. */
	id sid = [descriptor respondsToSelector:@selector(stageInputDescriptor)] ? [descriptor stageInputDescriptor] : nil;
	if (sid != nil && mesa.stage_in_register != NULL) {
		struct rw_stage_in si;
		bool any = false;

		memset(&si, 0, sizeof(si));
		for (unsigned i = 0; i < 31; i++) {
			MTLAttributeDescriptor *a = [[sid attributes] objectAtIndexedSubscript:i];
			MTLBufferLayoutDescriptor *l = [[sid layouts] objectAtIndexedSubscript:i];

			si.attribute[i].format = (unsigned)[a format];
			si.attribute[i].offset = (unsigned)[a offset];
			si.attribute[i].buffer_index = (unsigned)[a bufferIndex];
			si.layout[i].stride = (unsigned)[l stride];
			si.layout[i].step_function = (unsigned)[l stepFunction];
			si.layout[i].step_rate = (unsigned)[l stepRate];
			any |= si.attribute[i].format != 0;
		}
		si.index_buffer_index = (unsigned)[sid indexBufferIndex];
		si.index_type = (unsigned)[sid indexType];
		if (any) {
			MESA_VOID(stage_in_register, [air bytes], [air length], &si);
		}
	}

	/* MTLComputePipelineDescriptor.linkedFunctions: what the kernel calls by name or out of a
	 * visible function table (CoreImage's ubershader), registered as a render pipeline's are */
	NSArray *linked = [descriptor respondsToSelector:@selector(linkedFunctions)]
				? rw_register_linked([descriptor linkedFunctions], air, [descriptor label]) : nil;

	char why[512] = { 0 };
	struct rwa_compute_info info = { 0 };
	uint64_t t0 = mach_absolute_time();
	struct rwa_compute *compute = MESA(compute_create, _mesa, [air bytes], [air length],
							  [[function name] UTF8String], &info,
							  why, sizeof(why));
	rw_say_slow("compute pipeline", [[function name] UTF8String], t0);
	if (compute == NULL) {
		/* The front end's own sentence, passed through rather than replaced: it says
		 * which construct it refused, and an application can act on that. */
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	HakiosMetalComputePipeline *made = [[HakiosMetalComputePipeline alloc] initWithHakiosDevice:self
											    compute:compute
											       info:&info
											 descriptor:descriptor];
	[made hakiosSetLinked:linked];
	return made;
}

- (id)fail:(NSError **)error with:(NSString *)reason
{
	if (error != NULL) {
		*error = [NSError errorWithDomain:@"HakiosMetal" code:1
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
 * Metal's own header marks that format unavailable on this platform, the compiler library refuses it
 * by name for the same reason, and Darwin depth is Depth16Unorm, Depth32Float or
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
 * The one method -[CAMetalLayer nextDrawable] asks of a device (docs/metal.md 10.46):
 * allocate_drawable_texture builds an ordinary MTLTextureDescriptor, sets its private
 * rotation/isDrawable/framebufferOnly/usage and sends this. The surface, its geometry and
 * every refusal live in src/drawable.m, because they answer CoreAnimation rather than
 * Metal; what is left here is the object, which has to be of this class.
 *
 * The refusal is printed rather than returned silently, and that is not a style choice:
 * CoreAnimation does not test what it gets back, so a bare nil becomes a CAMetalDrawable
 * whose -texture is nil, and the application finds out later and somewhere else.
 */
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)s plane:(NSUInteger)p
{
	char why[512] = { 0 };
	struct rwd_surface took;
	struct rwa_texture_shape shape;   /* this file's own declaration; rwd_texture_shape is the
					   * same layout under the other file's name, and the cast
					   * below is the one place the two meet */
	struct rwa_texture *texture;

	if (_mesa == NULL) {
		rw_print("a drawable's texture needs the compiler library\n");
		return nil;
	}
	memset(&shape, 0, sizeof(shape));
	shape.pixel_format = (unsigned)[d pixelFormat];
	shape.type = (unsigned)[d textureType];
	shape.width = (unsigned)[d width];
	shape.height = (unsigned)[d height];
	shape.depth = (unsigned)[d depth];
	shape.array_length = (unsigned)[d arrayLength];
	shape.mip_levels = (unsigned)[d mipmapLevelCount];
	shape.sample_count = (unsigned)[d sampleCount];
	shape.usage = (unsigned)[d usage];
	texture = rwd_texture_for_surface(_mesa, s, p, (const struct rwd_texture_shape *)&shape,
					  &took, why, sizeof(why));
	if (texture == NULL) {
		/*
		 * A surface the GPU cannot take as it is -- rows the Adreno cannot step by (480
		 * bytes), a format or a pitch its import refuses -- is still a picture somebody
		 * wants drawn: a texture of this driver's own with the surface's pixels copied in.
		 * What that loses is the sharing: a later change to the surface is not seen. The
		 * surfaces this happens to are icons and other pictures made once and shown.
		 */
		id copy = (p == 0 && [d textureType] == MTLTextureType2D) ? [self newTextureWithDescriptor:d] : nil;

		if (copy != nil && [copy isKindOfClass:[HakiosMetalTexture class]]) {
			const void *base = IOSurfaceGetBaseAddress((IOSurfaceRef)s);
			size_t row = IOSurfaceGetBytesPerRow((IOSurfaceRef)s);

			/* copied now, and again while its owner is still drawing into it (-mesaTexture) */
			[(HakiosMetalTexture *)copy copyFromSurface:(IOSurfaceRef)s];
			if (base != NULL && row != 0) {
				RW_AGAIN("a surface the GPU cannot take as it is, copied instead (%s)\n", why);
				return copy;
			}
		}
		[copy release];
		RW_AGAIN("no drawable: %s\n", why);
		return nil;
	}
	return [[HakiosMetalTexture alloc] initWithHakiosDevice:self
							 texture:texture
							   shape:&shape];
}
- (id)newBufferWithIOSurface:(void *)surface        { (void)surface; return nil; }
/*
 * The caller's own pages, wrapped -- which needs the kernel to map them for the GPU, and it
 * cannot yet. A COPY instead: RenderBox (SwiftUI's renderer, in SpringBoard) makes its shader
 * tables this way, and a nil here was a null dereference in RB::RenderQueue::encode the moment
 * the home screen was drawn. What is lost is the sharing: a write the caller makes to its pages
 * afterwards is not seen. Tables and other constant data -- what this is used for -- never are.
 * The deallocator still runs when the buffer goes, as Metal's contract says, and not before:
 * the pages are the caller's until then.
 */

- (id)newBufferWithBytesNoCopy:(void *)bytes
			length:(NSUInteger)length
		       options:(NSUInteger)options
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	HakiosMetalBuffer *buffer = [self newBufferWithBytes:bytes length:length options:options];

	RW_ONCE("newBufferWithBytesNoCopy: the caller's pages are copied, not shared (the kernel "
		"cannot give a process's own memory to the GPU yet), and copied again when they change\n");
	if (buffer != nil) {
		[buffer followCallerBytes:bytes];
	}
	if (buffer != nil && deallocator != NULL) {
		HakiosMetalDeallocNote *note = [HakiosMetalDeallocNote new];

		note->_block = [deallocator copy];
		note->_bytes = bytes;
		note->_length = length;
		objc_setAssociatedObject(buffer, @selector(newBufferWithBytesNoCopy:length:options:deallocator:),
		    note, OBJC_ASSOCIATION_RETAIN);
		[note release];
	}
	return buffer;
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
 * (docs/air.md: `Adreno 7c+ Gen 3, chip 0x06030500`) where the chip is known, and the identifier is
 * asked of the kernel rather than written down, so a different phone says something different.
 * Nothing in Metal's registration reads this; a person reading a log does. */
/*
 * What MetalPerformanceShaders recognises a GPU by. MPSDevice::GetMPSDevice (MPSCore) maps a known
 * Apple GPU to its own device class and otherwise asserts "Failed to get MPSDevice" -- which killed
 * SpringBoard at its first Gaussian blur (the Siri sphere) -- unless -productName is
 * "Paravirtual device": then it makes an MPSParavirtDevice, the generic one Apple uses for a
 * virtual machine's GPU, which runs its kernels as ordinary Metal compute pipelines. That is what
 * this driver can give it.
 */
- (NSString *)productName
{
	return @"Paravirtual device";
}

/* ... and the one question MPSParavirtDevice -initWithDevice: (MPSCore) asks of such a device before
 * anything else: it looks up "hostSupportsFamily:" and asserts "Virtual device doesn't support
 * supportsFamily" when the device does not answer it. In a virtual machine it is the host GPU's
 * family; here the host is this GPU, so the answer is this device's own. */
- (BOOL)hostSupportsFamily:(MTLGPUFamily)family
{
	return [self supportsFamily:family];
}

/* Mesa's renderer names for the chips this port has run on; any other is named by its number */
static const struct { uint32_t chip; const char *name; } known_chips[] = {
	{ 0x06030500, "Adreno 7c+ Gen 3" },     /* SM7325: docs/air.md */
	{ 0x06060010, "Adreno 642" },           /* SM7350: the kernel's GPU profile */
};

- (NSString *)name
{
	uint64_t chip = 0;
	uint32_t id;

	if (!gpu_param(_gpu, 0x03, &chip)) {
		return @"Adreno (the GPU is not started)";
	}
	id = (uint32_t)(chip & 0xffffffffu);
	for (unsigned i = 0; i < sizeof(known_chips) / sizeof(known_chips[0]); i++) {
		if (known_chips[i].chip == id) {
			return [NSString stringWithFormat:@"%s (chip %#x)", known_chips[i].name, id];
		}
	}
	return [NSString stringWithFormat:@"Adreno (chip %#x)", id];
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
/*
 * Darwin 26's Metal asks this in the place where 16.5 asked -targetDeviceInfo: +[MTLLoader
 * sliceIDForDevice:legacyDriverVersion:airntDriverVersion:] sends -targetDeviceArchitecture and
 * asserts "Target device architecture is nil" when it gets nil -- the first thing backboardd's
 * CoreAnimation did with this device, 117 ms into its life. The answer is Metal's own
 * MTLTargetDeviceArchitecture, of which the loader reads cpuType and subType (the slice of a
 * binary archive to take), driverVersion and versionCombined. A cpu type of 0 is no Apple GPU's,
 * so every precompiled slice misses and pipelines are compiled from their AIR, as this driver
 * does anyway (see -targetDeviceInfo below for the same reasoning on 16.5).
 */
- (id)targetDeviceArchitecture
{
	if (_architecture == nil) {
		Class arch = NSClassFromString(@"MTLTargetDeviceArchitecture");
		id a = arch != Nil ? [[arch alloc] init] : nil;

		if (a != nil) {
			if ([a respondsToSelector:@selector(setCpuType:)]) {
				((void (*)(id, SEL, uint32_t))objc_msgSend)(a, @selector(setCpuType:), 0);
			}
			if ([a respondsToSelector:@selector(setSubType:)]) {
				((void (*)(id, SEL, uint32_t))objc_msgSend)(a, @selector(setSubType:), 0);
			}
			if ([a respondsToSelector:@selector(setDriverVersion:)]) {
				((void (*)(id, SEL, uint64_t))objc_msgSend)(a, @selector(setDriverVersion:), 0);
			}
		}
		_architecture = a;
		rw_print("targetDeviceArchitecture answers %s (cpu type 0: no precompiled slice is this device's)\n",
		         a != nil ? "a MTLTargetDeviceArchitecture" : "NIL: Metal has no such class here");
	}
	return _architecture;
}

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
	return [[HakiosMetalBuffer alloc] initWithDevice:self gpu:_gpu length:length];
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(NSUInteger)options
{
	HakiosMetalBuffer *buffer = [self newBufferWithLength:length options:options];

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

/* No rasterization rate maps: every pass is rasterised at one rate, and the compiler library takes a
 * shader's map_screen_to_physical_coordinates as the coordinate it was given (air_intrinsics.c). */
- (BOOL)supportsRasterizationRateMapWithLayerCount:(NSUInteger)count
{
	(void)count;
	return NO;
}

- (id)newRasterizationRateMapWithDescriptor:(id)descriptor
{
	(void)descriptor;
	return nil;
}

@end

@implementation HakiosMetalBuffer

+ (void)load
{
	Protocol *p = objc_getProtocol("MTLBuffer");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLBuffer) refused\n");
	}
}

- (instancetype)initWithDevice:(HakiosMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length
{
	struct gem_new request = { .size = length, .flags = 0, .handle = 0 };
	size_t size = sizeof(request);

	self = [super init];
	if (self == nil) {
		return nil;
	}

	/* Two ways to the same kernel. When the compiler library is loaded, the memory comes
	 * from Mesa, because only such a buffer can be handed to a dispatch; without it the
	 * kernel's GEM interface is used directly, so that a device with no GPU library still
	 * gives memory a program can write. Which one happened is visible in -mesaBuffer. */
	if (mesa_available() && [device mesaDevice] != NULL) {
		char why[256] = { 0 };

		_mesa = MESA(buffer_create, [device mesaDevice], length, why, sizeof(why));
		if (_mesa == NULL) {
			rw_print("Mesa would not allocate %lu bytes: %s\n",
			         (unsigned long)length, why);
			[self release];
			return nil;
		}
		_owner = device;
		_length = length;
		_contents = MESA(buffer_map, _mesa);
		return self;
	}
	if (gpu == IO_OBJECT_NULL || length == 0) {
		rw_print("a buffer of %lu bytes needs a gpu client\n",
		         (unsigned long)length);
		[self release];
		return nil;
	}
	if (IOConnectCallStructMethod(gpu, kMsmGemNew, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS || request.handle == 0) {
		rw_print("the kernel would not allocate %lu bytes\n",
		         (unsigned long)length);
		[self release];
		return nil;
	}
	_owner = device;
	_gpu = gpu;
	_handle = request.handle;
	_length = length;

	/* the address the GPU will see, which is what a command stream carries */
	struct gem_info info = { .handle = _handle, .info = MSM_INFO_GET_IOVA };

	size = sizeof(info);
	if (IOConnectCallStructMethod(gpu, kMsmGemInfo, &info, sizeof(info), &info,
				      &size) == KERN_SUCCESS) {
		_iova = info.value;
	}

	/* and the address this processor will see: the mapping is as the memory is made,
	 * write-combined, exactly as Mesa takes it */
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	if (IOConnectMapMemory64(gpu, _handle, mach_task_self(), &address, &mapped,
				 kIOMapAnywhere) == KERN_SUCCESS) {
		_contents = (void *)(uintptr_t)address;
	} else {
		rw_print("buffer %u is allocated but could not be mapped\n", _handle);
	}
	return self;
}

/* every word, for a no-copy buffer's pages: a write the sampled hash steps over would be lost */
static uint64_t
rw_bytes_hash_all(const uint8_t *bytes, size_t length)
{
	uint64_t h = 1469598103934665603ull;
	size_t x;

	for (x = 0; x + 8 <= length; x += 8) {
		uint64_t w;

		memcpy(&w, bytes + x, 8);
		h = (h ^ w) * 1099511628211ull;
	}
	for (; x < length; x++) {
		h = (h ^ bytes[x]) * 1099511628211ull;
	}
	return h;
}

- (void)followCallerBytes:(const void *)bytes
{
	_callerBytes = bytes;
	_callerHash = bytes != NULL ? rw_bytes_hash_all(bytes, _length) : 0;
	_callerEpoch = rw_commit_epoch;
}

/*
 * The buffer as the GPU is to read it. A no-copy buffer is a copy of its caller's pages (the kernel
 * cannot give them to the GPU), and Metal's contract is that the GPU sees what the caller writes to
 * them: so once a command buffer, the pages are looked at again and copied in when they changed.
 * CoreAnimation writes the lock screen's wallpaper colours into one after making it: copied once,
 * the ground of Stripe Yellow came out black. A write the GPU made to the copy is kept as long as
 * the caller's pages have not changed since.
 */
- (struct rwa_buffer *)mesaBuffer
{
	if (_callerBytes != NULL && _contents != NULL && _callerEpoch != rw_commit_epoch) {
		uint64_t h = rw_bytes_hash_all(_callerBytes, _length);

		_callerEpoch = rw_commit_epoch;
		if (h != _callerHash) {
			memcpy(_contents, _callerBytes, _length);
			_callerHash = h;
		}
	}
	return _mesa;
}

- (void)dealloc
{
	if (_mesa != NULL) {
		MESA_VOID(buffer_destroy, _mesa);
		[super dealloc];
		return;
	}
	if (_contents != NULL) {
		IOConnectUnmapMemory64(_gpu, _handle, mach_task_self(),
				       (mach_vm_address_t)(uintptr_t)_contents);
	}
	if (_handle != 0) {
		struct gem_close request = { .handle = _handle, .pad = 0 };
		size_t size = sizeof(request);

		IOConnectCallStructMethod(_gpu, kDrmGemClose, &request, sizeof(request), &request,
					  &size);
	}
	[super dealloc];
}

- (void *)contents          { return _contents; }
- (NSUInteger)length        { return _length; }
- (id)device                { return _owner; }
- (uint64_t)gpuAddress      { return _iova; }
- (NSUInteger)allocatedSize { return _length; }

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
	/*
	 * A texture laid over this buffer, which the contract has no call for: a texture of the device's
	 * own instead, the buffer's bytes copied in now and again whenever they have changed, and what the
	 * GPU draws into it written back (-[HakiosMetalTexture copyFromBuffer:...], the way a surface the
	 * GPU cannot take is served). backboardd asks for one at its first frames under QEMU, and had nil.
	 */
	NSUInteger height = [descriptor height] ? [descriptor height] : 1;

	if (_contents == NULL || bytesPerRow == 0 || [descriptor textureType] != MTLTextureType2D ||
	    offset + bytesPerRow * (height - 1) > _length) {
		RW_ONCE("hakiosmetal: a texture over a buffer was asked for that this driver cannot copy "
			"(a 2D one, inside the buffer, with its rows given, is what it can)\n");
		return nil;
	}
	id texture = [_owner newTextureWithDescriptor:descriptor];
	if (![texture isKindOfClass:[HakiosMetalTexture class]]) {
		[texture release];
		return nil;
	}
	RW_ONCE("hakiosmetal: a texture over a buffer is a copy of the buffer's bytes, taken again when "
		"they change, and written back when the GPU draws into it\n");
	[(HakiosMetalTexture *)texture copyFromBuffer:self offset:offset bytesPerRow:bytesPerRow];
	return texture;
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

@implementation HakiosMetalTexture

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

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
			      texture:(struct rwa_texture *)texture
				shape:(const struct rwa_texture_shape *)shape
{
	self = [super init];
	if (self == nil) {
		MESA_VOID(texture_destroy, texture);
		return nil;
	}
	_owner = device;
	_texture = texture;
	_shape = *shape;
	return self;
}

- (void)dealloc
{
	if (_texture != NULL) {
		MESA_VOID(texture_destroy, _texture);
	}
	[_copiedFromBuffer release];
	if (_copiedFrom != NULL) {
		CFRelease(_copiedFrom);
	}
	[super dealloc];
}

/*
 * A copied surface is looked at again when it is used (a sampled hash of its pixels) and copied again when it
 * changed: a surface copied once was copied before its owner had drawn it (black).
 */
static uint64_t
rw_surface_hash(IOSurfaceRef surface, size_t rows, size_t bytes)
{
	const uint8_t *base = IOSurfaceGetBaseAddress(surface);
	size_t row = IOSurfaceGetBytesPerRow(surface);
	uint64_t h = 1469598103934665603ull;

	if (base == NULL) {
		return 0;
	}
	for (size_t y = 0; y < rows; y += 3) {
		const uint64_t *w = (const uint64_t *)(base + y * row);

		for (size_t x = 0; x < bytes / 8; x += 5) {
			h = (h ^ w[x]) * 1099511628211ull;
		}
	}
	return h;
}

- (void)copyFromSurface:(IOSurfaceRef)surface
{
	BOOL locked = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess;
	const void *base = IOSurfaceGetBaseAddress(surface);
	size_t row = IOSurfaceGetBytesPerRow(surface);

	if (base != NULL && row != 0) {
		uint64_t h = rw_surface_hash(surface, _shape.height, MIN(row, (size_t)_shape.width * 4));

		if (_copiedFrom != NULL && h == _copiedHash) {
			_copiedStable++;
		} else {
			[self replaceRegion:MTLRegionMake2D(0, 0, _shape.width, _shape.height) mipmapLevel:0
				  withBytes:base bytesPerRow:row];
			rw_say_surface_colour(surface, base, row, _shape.width, _shape.height, true);
			_copiedStable = 0;
			_copiedHash = h;
		}
	}
	if (locked) {
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	}
	if (_copiedFrom == NULL) {
		_copiedFrom = (IOSurfaceRef)CFRetain(surface);
	}
	_copiedAt = mach_absolute_time();
	_copiedSeed = IOSurfaceGetSeed(surface);
}

/*
 * And the other way: what the GPU drew into the copy, written to the surface it is a copy of. Without
 * it a pass that renders into a surface the GPU could not take drew into this driver's copy alone --
 * under QEMU, with no HakiosGPU to give a surface's memory to, that is every surface, the display's
 * own included, and not one frame CoreAnimation drew through Metal reached the screen. Called by the
 * command buffer once its work has finished, for each copy it drew or blitted into. The surface's
 * new seed and contents are taken as the copy's, so the copy is not taken back in again.
 */
- (void)writeBackToSurface
{
	if (_copiedFromBuffer != nil) {
		uint8_t *base = [_copiedFromBuffer contents];
		size_t rows = _shape.height ? _shape.height : 1;

		if (base != NULL) {
			[self getBytes:base + _bufferOffset bytesPerRow:_bufferRow
			    fromRegion:MTLRegionMake2D(0, 0, _shape.width, rows) mipmapLevel:0];
			_copiedHash = rw_bytes_hash(base + _bufferOffset, _bufferRow * rows);
		}
		return;
	}
	if (_copiedFrom == NULL) {
		return;
	}
	BOOL locked = IOSurfaceLock(_copiedFrom, 0, NULL) == kIOReturnSuccess;
	void *base = IOSurfaceGetBaseAddress(_copiedFrom);
	size_t row = IOSurfaceGetBytesPerRow(_copiedFrom);
	size_t width = MIN((size_t)_shape.width, IOSurfaceGetWidth(_copiedFrom));
	size_t height = MIN((size_t)_shape.height, IOSurfaceGetHeight(_copiedFrom));

	if (base != NULL && row != 0 && width != 0 && height != 0) {
		[self getBytes:base bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
		rw_say_surface_colour(_copiedFrom, base, row, width, height, false);
	}
	if (locked) {
		IOSurfaceUnlock(_copiedFrom, 0, NULL);
	}
	if (base != NULL && row != 0) {
		_copiedHash = rw_surface_hash(_copiedFrom, _shape.height, MIN(row, (size_t)_shape.width * 4));
	}
	_copiedSeed = IOSurfaceGetSeed(_copiedFrom);
	_copiedAt = mach_absolute_time();
}

/*
 * What the first forty large surfaces of each kind were drawn as, written back, and what the first few taken in
 * held (`in`): the mean of each byte of a 4-byte pixel over a grid of them, and how many are not
 * black. A display that shows black after a frame is a window server that drew black, a driver
 * that wrote nothing, or layers that came in black, and these lines tell the three apart without
 * a screenshot of every layer.
 */
static void
rw_say_surface_colour(IOSurfaceRef surface, const uint8_t *base, size_t row, size_t width, size_t height,
		      bool in)
{
	static unsigned said[2];
	uint64_t sum[4] = { 0, 0, 0, 0 }, n = 0, lit = 0;

	if (said[in] >= 40 || width < 256 || height < 256 || IOSurfaceGetBytesPerElement(surface) != 4) {
		return;
	}
	said[in]++;
	for (size_t y = 0; y < height; y += height / 32) {
		for (size_t x = 0; x < width; x += width / 32) {
			const uint8_t *p = base + y * row + x * 4;

			for (unsigned k = 0; k < 4; k++) {
				sum[k] += p[k];
			}
			lit += (p[0] | p[1] | p[2]) != 0;
			n++;
		}
	}
	rw_print("surface %u (%zux%zu) %s: bytes on average %llu %llu %llu %llu, %llu of %llu "
	         "sampled pixels not black\n", IOSurfaceGetID(surface), width, height,
	         in ? "taken in" : "written back",
	         (unsigned long long)(sum[0] / n), (unsigned long long)(sum[1] / n),
	         (unsigned long long)(sum[2] / n), (unsigned long long)(sum[3] / n),
	         (unsigned long long)lit, (unsigned long long)n);
}

/* the FNV-1a of a run of bytes, every fifth word: what tells a changed buffer from one that has not */
static uint64_t
rw_bytes_hash(const uint8_t *bytes, size_t length)
{
	const uint64_t *w = (const uint64_t *)bytes;
	uint64_t h = 1469598103934665603ull;

	for (size_t x = 0; x < length / 8; x += 5) {
		h = (h ^ w[x]) * 1099511628211ull;
	}
	return h;
}

- (void)copyFromBuffer:(id)buffer offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row
{
	const uint8_t *base = [buffer contents];

	if (base == NULL) {
		return;
	}
	if (_copiedFromBuffer == nil) {
		_copiedFromBuffer = [buffer retain];
		_bufferOffset = offset;
		_bufferRow = row;
	}
	uint64_t h = rw_bytes_hash(base + offset, row * (_shape.height ? _shape.height : 1));
	if (_copiedAt != 0 && h == _copiedHash) {
		return;
	}
	[self replaceRegion:MTLRegionMake2D(0, 0, _shape.width, _shape.height ? _shape.height : 1) mipmapLevel:0
		  withBytes:base + offset bytesPerRow:row];
	_copiedHash = h;
	_copiedAt = mach_absolute_time();
}

- (struct rwa_texture *)mesaTexture
{
	if (_copiedFromBuffer != nil) {
		[self copyFromBuffer:_copiedFromBuffer offset:_bufferOffset bytesPerRow:_bufferRow];
	}
	if (_copiedFrom != NULL) {
		static mach_timebase_info_data_t tb;

		if (tb.denom == 0) {
			mach_timebase_info(&tb);
		}
		/* copied again at once when its writer has said so (the seed: who writes a surface locks it), and otherwise looked
		 * at every 100 ms, every 250 ms once it has not changed for two seconds (a second left text and masks stale long
		 * enough to be seen); copied only when it has changed */
		if (IOSurfaceGetSeed(_copiedFrom) != _copiedSeed ||
		    (mach_absolute_time() - _copiedAt) * tb.numer / tb.denom > (_copiedStable < 20 ? 100000000ull : 250000000ull)) {
			[self copyFromSurface:_copiedFrom];
		}
	}
	return _texture;
}

- (id)device                       { return _owner; }
- (NSUInteger)width                { return _shape.width; }
- (NSUInteger)height               { return _shape.height; }
- (NSUInteger)depth                { return _shape.depth; }
- (NSUInteger)mipmapLevelCount     { return _shape.mip_levels; }
- (NSUInteger)arrayLength          { return _shape.array_length; }
- (NSUInteger)sampleCount          { return _shape.sample_count; }
- (NSUInteger)pixelFormat          { return _shape.pixel_format; }
- (NSUInteger)textureType          { return _shape.type; }
- (NSUInteger)usage                { return _shape.usage; }
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
	return _shape.sample_count > 1 ? 2 : 0;   /* MTLStorageModePrivate : Shared */
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
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	(void)image;
	rw_pipe_enter();
	if (MESA(texture_write, _texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			       stride, why, sizeof(why)) != 0) {
		rw_print("writing texels: %s\n", why);
	}
	rw_pipe_leave();
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
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	(void)image;
	rw_pipe_enter();
	if (MESA(texture_read, _texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			      stride, why, sizeof(why)) != 0) {
		rw_print("reading texels: %s\n", why);
	}
	rw_pipe_leave();
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:stride bytesPerImage:0 fromRegion:region
	   mipmapLevel:level slice:0];
}

- (BOOL)hakiosReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	if (mesa.depth_read == NULL) {
		rw_print("this compiler library has no rwa_depth_read, so it is "
		         "older than this driver and cannot say what a depth buffer holds\n");
		return NO;
	}
	rw_pipe_enter();
	if (MESA(depth_read, _texture, 0, 0, origin, size, depth, stencil, why,
			    sizeof(why)) != 0) {
		rw_print("reading the depth of a texture: %s\n", why);
		return NO;
	}
	rw_pipe_leave();
	return YES;
}

/* A view of a texture is another texture over the same memory, and the compiler library has
 * no call for that yet; nil is the honest answer, and it is better than a view that aliases
 * nothing. */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
{
	return [self newTextureViewWithPixelFormat:format textureType:_shape.type
					    levels:NSMakeRange(0, MAX(_shape.mip_levels, 1u))
					    slices:NSMakeRange(0, MAX(_shape.array_length, 1u))];
}

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
	/* The whole texture in another format (the window server's _sRGB views of its BGRA8
	 * layers); a view of some of its levels or slices, or as another type, is not one this
	 * library makes yet, and is said. */
	char why[256] = "";
	struct rwa_texture *view;

	if (type != _shape.type || levels.location != 0 || slices.location != 0) {
		RW_ONCE("a texture view of part of a texture, or as another type, is not "
			"implemented; the whole texture is viewed\n");
	}
	if (mesa.texture_view == NULL ||
	    (view = MESA(texture_view, _texture, (unsigned)format, why, sizeof(why))) == NULL) {
		RW_AGAIN("no texture view: %s\n", why);
		return nil;
	}
	struct rwa_texture_shape shape = _shape;

	shape.pixel_format = (unsigned)format;
	return [[HakiosMetalTexture alloc] initWithHakiosDevice:_owner texture:view shape:&shape];
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
	return (_shape.type == 3 || _shape.type == 4) ? 6 : 1;
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

@implementation HakiosMetalSampler

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
			      sampler:(struct rwa_sampler *)sampler
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilySampler sends to super; the descriptor is passed on
	 * because the base keeps it and the pipeline-state assertions taught us not to send
	 * nil to one of these */
	self = [super initWithDevice:device samplerDescriptor:descriptor];
	if (self == nil) {
		MESA_VOID(sampler_destroy, sampler);
		return nil;
	}
	_sampler = sampler;
	return self;
}

- (void)dealloc
{
	if (_sampler != NULL) {
		MESA_VOID(sampler_destroy, _sampler);
	}
	for (unsigned i = 0; i < _clampedCount; i++) {
		MESA_VOID(sampler_destroy, _clamped[i].sampler);
	}
	[super dealloc];
}

- (struct rwa_sampler *)mesaSampler { return _sampler; }

- (void)setHakiosState:(const struct rwa_sampler_state *)state { _state = *state; }

/*
 * This sampler with an encoder's own LOD range, -setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:,
 * which used to be dropped. It is how a pass reads one level of a texture while it renders into the next:
 * the window server's variable blur downsamples level n-1 into level n of one texture with the clamp at
 * n-1, and without it the sample took its level from the derivatives -- the level being written -- and
 * the blur under Darwin 26's navigation bars came out in stair-stepped blocks (runs glass3, glass4).
 */
- (struct rwa_sampler *)mesaSamplerWithLodMin:(float)lodMin max:(float)lodMax
{
	char why[256] = "";

	if (lodMin == _state.lod_min && lodMax == _state.lod_max) {
		return _sampler;
	}
	for (unsigned i = 0; i < _clampedCount; i++) {
		if (_clamped[i].min == lodMin && _clamped[i].max == lodMax) {
			return _clamped[i].sampler;
		}
	}
	struct rwa_sampler_state state = _state;
	state.lod_min = lodMin;
	state.lod_max = lodMax;
	struct rwa_sampler *sampler = MESA(sampler_create, [(HakiosMetalDevice *)[self device] mesaDevice], &state,
					   why, sizeof(why));
	if (sampler == NULL) {
		RW_ONCE("a sampler with an encoder's LOD clamps: %s; the sampler's own range is used\n", why);
		return _sampler;
	}
	if (_clampedCount == sizeof(_clamped) / sizeof(_clamped[0])) {
		MESA_VOID(sampler_destroy, _clamped[0].sampler);
		memmove(&_clamped[0], &_clamped[1], sizeof(_clamped[0]) * (_clampedCount - 1));
		_clampedCount--;
	}
	_clamped[_clampedCount].min = lodMin;
	_clamped[_clampedCount].max = lodMax;
	_clamped[_clampedCount].sampler = sampler;
	_clampedCount++;
	return sampler;
}

@end

@implementation HakiosMetalDepthStencilState

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor
{
	/* The base class's own designated initialiser, which validates the descriptor,
	 * sends `init` to super and fills _device and _label (0x181a29b38). The descriptor
	 * is handed on for the same reason the sampler's is: the base reads it, and a nil
	 * there is reported by Apple's own validation. */
	self = [super initWithDevice:device depthStencilDescriptor:descriptor];
	if (self == nil) {
		MESA_VOID(depth_stencil_destroy, state);
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
		MESA_VOID(depth_stencil_destroy, _state);
	}
	[super dealloc];
}

- (struct rwa_depth_stencil *)mesaDepthStencil { return _state; }

- (BOOL)readsDepth    { return _readsDepth; }
- (BOOL)writesDepth   { return _writesDepth; }
- (BOOL)readsStencil  { return _readsStencil; }
- (BOOL)writesStencil { return _writesStencil; }

@end

@implementation HakiosMetalComputePipeline

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
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
		MESA_VOID(compute_destroy, compute);
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
		MESA_VOID(compute_destroy, _compute);
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

- (void)hakiosSetLinked:(NSArray *)linked
{
	[_linked release];
	_linked = [linked retain];
}

- (id)functionHandleWithFunction:(id)function
{
	return rw_function_handle(_owner, _linked, function);
}

- (id)newVisibleFunctionTableWithDescriptor:(id)descriptor
{
	NSUInteger count = [descriptor respondsToSelector:@selector(functionCount)] ? [descriptor functionCount] : 0;
	return [[HakiosMetalVisibleFunctionTable alloc] initWithDevice:_owner count:count];
}

@end

@implementation HakiosMetalFunctionHandle

- (instancetype)initWithDevice:(id)device function:(id)function handle:(uint64_t)handle
{
	self = [super init];
	if (self != nil) {
		_device = [device retain];
		_function = [function retain];
		_handle = handle;
	}
	return self;
}

- (void)dealloc
{
	[_device release];
	[_function release];
	[super dealloc];
}

- (uint64_t)hakiosHandle             { return _handle; }
- (id)device                         { return _device; }
- (NSString *)name                   { return [_function name]; }
- (NSUInteger)functionType           { return [_function functionType]; }
- (MTLResourceID)gpuResourceID       { return (MTLResourceID){ ._impl = _handle }; }

@end

@implementation HakiosMetalVisibleFunctionTable

- (instancetype)initWithDevice:(id)device count:(NSUInteger)count
{
	self = [super init];
	if (self != nil) {
		_device = [device retain];
		_count = count;
		/* eight bytes a handle, zero (no function) until one is set */
		_buffer = [device newBufferWithLength:MAX(count, 1) * 8 options:0];
		if (_buffer != nil) {
			memset([_buffer contents], 0, MAX(count, 1) * 8);
		}
	}
	return self;
}

- (void)dealloc
{
	[_buffer release];
	[_device release];
	[_label release];
	[super dealloc];
}

- (id)hakiosBuffer                   { return _buffer; }
- (id)device                         { return _device; }
- (NSString *)label                  { return _label; }
- (void)setLabel:(NSString *)label   { [_label release]; _label = [label copy]; }
- (MTLResourceID)gpuResourceID       { return (MTLResourceID){ ._impl = (uint64_t)(uintptr_t)self }; }
- (NSUInteger)allocatedSize          { return _count * 8; }

- (void)setFunction:(id)function atIndex:(NSUInteger)index
{
	if (index >= _count || _buffer == nil) {
		return;
	}
	uint64_t handle = [function respondsToSelector:@selector(hakiosHandle)] ? [function hakiosHandle] : 0;
	((uint64_t *)[_buffer contents])[index] = handle;
}

- (void)setFunctions:(const id *)functions withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFunction:functions[i] atIndex:range.location + i];
	}
}

@end

@implementation HakiosMetalRenderPipeline

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
			       render:(struct rwa_render *)render
				 info:(const struct rwa_render_info *)info
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilyRenderPipeline sends to super (0x181a294dc), and a real
	 * descriptor, for the reason the compute one learned at its own line 1504 */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		MESA_VOID(render_destroy, render);
		return nil;
	}
	_owner = device;
	_render = render;
	_info = *info;
	snprintf(_names, sizeof(_names), "%s/%s", [[[descriptor vertexFunction] name] UTF8String] ?: "-",
	         [[[descriptor fragmentFunction] name] UTF8String] ?: "-");
	{
		NSString *label = [descriptor label];
		const char *text = label != nil ? [label UTF8String] : "";
		uint32_t h = 2166136261u;

		for (const char *c = text; *c; c++) {
			h = (h ^ (uint8_t)*c) * 16777619u;
		}
		_bisectGroup = h % RW_BISECT_GROUPS;
		/* with boot-arg metal_bisect_group=G, only group G's pipelines, split again by the hash's next digit */
		if (rw_bisect_within() >= 0) {
			_bisectGroup = (unsigned)rw_bisect_within() == _bisectGroup ? (h / RW_BISECT_GROUPS) % RW_BISECT_GROUPS :
			    RW_BISECT_GROUPS;
		}
		/* with boot-arg metal_bisect_names=a,b,c: phase k is the pipelines whose label contains the k-th name */
		{
			char names[1024] = { 0 }, *at;
			size_t size = sizeof(names) - 1;

			(void)size; (void)at; (void)names;
			if (*rw_bisect_names()) {
				unsigned k = 0;
				const char *name = rw_bisect_names(), *end;

				_bisectGroup = 1000;
				for (; *name && *name != ' '; k++) {
					for (end = name; *end && *end != ',' && *end != ' '; end++) {
					}
					if (end > name) {
						char want[128] = { 0 };

						memcpy(want, name, MIN((size_t)(end - name), sizeof(want) - 1));
						if (strstr(text, want) != NULL) {
							_bisectGroup = k;
							break;
						}
					}
					name = *end == ',' ? end + 1 : end;
				}
			}
		}
		if (rw_bisect_start() != 0) {
			rw_print("bisect: group %u: %s\n", _bisectGroup, text);
		}
	}
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
		MESA_VOID(render_destroy, _render);
	}
	[super dealloc];
}

- (struct rwa_render *)render { return _render; }

- (void)hakiosSetLinked:(NSArray *)linked
{
	[_linked release];
	_linked = [linked retain];
}

/* a render pipeline's handles and tables are its fragment stage's (the stage that links) */
- (id)functionHandleWithFunction:(id)function stage:(NSUInteger)stage
{
	(void)stage;
	return rw_function_handle(_owner, _linked, function);
}

- (id)newVisibleFunctionTableWithDescriptor:(id)descriptor stage:(NSUInteger)stage
{
	(void)stage;
	NSUInteger count = [descriptor respondsToSelector:@selector(functionCount)] ? [descriptor functionCount] : 0;
	return [[HakiosMetalVisibleFunctionTable alloc] initWithDevice:_owner count:count];
}
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

@implementation HakiosMetalWork
/*
 * Metal's command buffers keep what was encoded alive until it has run ("retained references"): CoreAnimation
 * releases a texture as soon as its draw is encoded. Unretained, such a texture was freed before the commit ran
 * the draw, and backboardd died sending -mesaTexture to whatever took its memory (an OS_dispatch_data).
 */
#define RW_EACH_ID(f) do { \
	f(pipeline); f(render); f(depth); f(depthStencilState); f(indexBuffer); f(blitFrom); f(blitTo); \
	f(indirectBuffer); \
	for (unsigned k_ = 0; k_ < 8; k_++) { f(color[k_]); } \
	for (unsigned k_ = 0; k_ < RW_MAX_BUFFERS; k_++) { f(vertexBuffers[k_]); f(buffers[k_]); f(textures[k_]); f(samplers[k_]); } \
} while (0)
#define RW_RETAIN(x) [x retain]
#define RW_RELEASE(x) [x release]
- (void)retainResources
{
	if (!retained) {
		RW_EACH_ID(RW_RETAIN);
		retained = YES;
	}
}
- (void)dealloc
{
	if (retained) {
		RW_EACH_ID(RW_RELEASE);
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
 * loud rather than worked around: on a Darwin whose Metal keeps this differently, the right
 * answer is to find out, not to run with a queue that silently blocks at 64.
 */
static dispatch_semaphore_t
hakios_queue_semaphore(id queue)
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
hakios_completion_ivar(void)
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
hakios_clear_semaphore_debt(id commandBuffer)
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

@implementation HakiosMetalCommandQueue {
	HakiosMetalDevice *_owner;
	NSUInteger _outstanding;        /* buffers of this queue that still hold a slot */
	NSUInteger _made, _returned;
	/* What <MTLCommandQueueSPI> keeps about a queue and this driver cannot act on: the
	 * two priorities are remembered so that a caller reading back what it set is not
	 * lied to, and -setCompletionQueue: explains the third below. */
	NSUInteger _gpuPriority, _backgroundGPUPriority;
	dispatch_queue_t _completionQueue;   /* only used when Apple's own ivar is missing */
}

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device descriptor:(id)descriptor
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
	         hakios_queue_semaphore(self) != nil ? "" :
	         ", EXCEPT that the semaphore was not found and it cannot");
	return self;
}

- (id)device { return _owner; }

/* What the queue itself thinks, for a test that wants the number rather than the
 * inference: how many command buffers of this queue still hold one of those slots. */
- (NSUInteger)hakiosOutstandingCommandBuffers { return _outstanding; }
- (NSUInteger)hakiosCommandBuffersMade        { return _made; }

- (void)hakiosTookSlot
{
	_outstanding++;
	_made++;
}

- (void)hakiosReturnedSlot
{
	if (_outstanding > 0) {
		_outstanding--;
	}
	_returned++;
}

- (id)commandBuffer
{
	return [[[HakiosMetalCommandBuffer alloc] initWithHakiosQueue:self] autorelease];
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
 * So this one is SERVED, not declined: the queue is kept, and -[HakiosMetalCommandBuffer
 * commit] delivers the scheduled and completed handlers on it (see the note there -- before
 * this they ran inline, at the moment they were added, which is before the work they are
 * about). It is kept in Apple's ivar rather than in one of ours so that a queue built from
 * a descriptor that already carried a completion queue (there is a
 * -[MTLCommandQueueDescriptorInternal setCompletionQueue:]) reads back as what it is; the
 * retain is balanced by -[_MTLCommandQueue dealloc], the ivar's owner.
 */
- (void)setCompletionQueue:(dispatch_queue_t)queue
{
	Ivar ivar = hakios_completion_ivar();
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
- (dispatch_queue_t)hakiosCompletionQueue
{
	Ivar ivar = hakios_completion_ivar();

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
 * banner in -initWithHakiosQueue: below is for: it names the numbering of the log that carries
 * it, and it is printed out of these constants, so the line cannot drift from what -status
 * answers. A log without that line is older than this change, and in it "status 6" is this
 * completion, "status 4" is a buffer that was only submitted, and 2 and 3 do not occur at all.
 * That is the rule tools/air/metalcheck.py reads a log by, and the measurement that makes it
 * safe: over all 141 logs in logs/ the only statuses the test ever printed are 5 (128 times) and
 * 6 (990 times) -- never 4.
 *
 * One file more keys on the old numbers and is NOT part of this change: ../../userland/userland/src/
 * metal_drv_test.m compares [cmd status] with the literal 6 in five places (its lines 1200, 1526,
 * 2041, 2293, 2795) and prints the literal "status 6" in a sixth (2308). Until those move, a boot
 * with this driver has the test calling every completed buffer a failure: the sweep prints
 * "drvtest: drawall <name> status 4" with nothing after it and reads back no pixel, and the single
 * draw returns before its own readback. metalcheck.py names that shape when it sees it.
 */
enum {
	RW_CB_NOT_ENQUEUED = 0,
	RW_CB_ENQUEUED     = 1,
	RW_CB_COMMITTED    = 2,
	RW_CB_SCHEDULED    = 3,
	RW_CB_COMPLETED    = 4,
	RW_CB_ERROR        = 5,
};

@implementation HakiosMetalCommandBuffer {
	HakiosMetalCommandQueue *_queue;
	NSMutableArray *_work;
	NSError *_error;
	NSUInteger _status;
	BOOL _owesSlot;                 /* this buffer holds one of the queue's slots */
	NSMutableArray *_scheduled, *_completed;   /* handler blocks, copied to the heap */
	NSMutableArray *_signals;       /* { event, value } pairs, set once the work is done */
	NSUInteger _protectionOptions;  /* what the window server asked for; see -setProtectionOptions: */
}

/*
 * Darwin 26: CA::OGL::MetalContext::start_command_buffer names the tasks a command buffer's
 * GPU time is to be billed to (memory and energy accounting, Apple's GPU driver). There is
 * no accounting here; every command buffer of the window server gets this.
 */
- (void)setResponsibleTaskIDs:(const unsigned int *)ids count:(NSUInteger)count
{
	(void)ids;
	(void)count;
}

/*
 * The window server sends this to every command buffer it makes, and until it was answered the
 * boot of 22 October 01:51 ended it 78 times with
 *
 *   NSInvalidArgumentException: -[HakiosMetalCommandBuffer setProtectionOptions:]:
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

- (instancetype)initWithHakiosQueue:(HakiosMetalCommandQueue *)queue
{
	/* what IOGPUMetalCommandBuffer sends to super, retained references and no debug mode.
	 * THIS CALL BLOCKS when the queue's semaphore is empty -- see the long note above
	 * HakiosMetalCommandQueue -- so by the time it returns, a slot is held and owed. */
	self = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];
	if (self == nil) {
		return nil;
	}
	_queue = [queue retain];
	_work = [[NSMutableArray alloc] init];
	/* The numbering this log is written in, said once per process and printed out of the
	 * constants that answer -status: a log that does NOT carry this line was written before
	 * the numbers became Metal's, and says committed 3, scheduled 4, completed 6 (see the
	 * note above this class). */
	RW_ONCE("MTLCommandBufferStatus: committed %u, scheduled %u, completed %u, "
		"error %u -- Metal's own numbers; a log without this line was written by the old "
		"ones (committed 3, scheduled 4, completed 6)\n",
		(unsigned)RW_CB_COMMITTED, (unsigned)RW_CB_SCHEDULED,
		(unsigned)RW_CB_COMPLETED, (unsigned)RW_CB_ERROR);
	_status = RW_CB_ENQUEUED;   /* nothing is pending yet */
	_owesSlot = YES;
	[queue hakiosTookSlot];
	return self;
}

/*
 * Give the queue's slot back, exactly once. Called at the end of -commit, where this
 * driver's work is already done, and at -dealloc for a buffer that was never committed --
 * which is the case Apple's _needsCommandBufferSemaphoreSignal exists for and the one that
 * would otherwise lose a slot for the life of the process.
 */
- (void)hakiosReturnSlot
{
	dispatch_semaphore_t semaphore;

	if (!_owesSlot) {
		return;
	}
	_owesSlot = NO;
	hakios_clear_semaphore_debt(self);
	[_queue hakiosReturnedSlot];
	semaphore = hakios_queue_semaphore(_queue);
	if (semaphore != nil) {
		dispatch_semaphore_signal(semaphore);
	}
}

- (void)dealloc
{
	[self hakiosReturnSlot];
	[_work release];
	[_error release];
	[_scheduled release];
	[_completed release];
	[_signals release];
	[_queue release];
	[super dealloc];
}

- (id)device       { return [_queue device]; }
- (id)commandQueue { return _queue; }
- (NSUInteger)status { return _status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }

- (id)computeCommandEncoder
{
	return [[[HakiosMetalComputeEncoder alloc] initWithHakiosCommandBuffer:self]
		   autorelease];
}

- (id)renderCommandEncoderWithDescriptor:(id)descriptor
{
	if (descriptor == nil) {
		rw_print("a render pass needs a descriptor\n");
		return nil;
	}
	return [[[HakiosMetalRenderEncoder alloc] initWithHakiosCommandBuffer:self
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
	return [[[HakiosMetalBlitEncoder alloc] initWithHakiosCommandBuffer:self] autorelease];
}
- (id)blitCommandEncoderWithDescriptor:(id)descriptor
{
	(void)descriptor;               /* (its sample buffer attachments: counters, not kept here) */
	return [self blitCommandEncoder];
}
- (id)resourceStateCommandEncoder { return nil; }
- (id)accelerationStructureCommandEncoder { return nil; }
/*
 * An event the GPU sets when it reaches this point. The window server's renderer signals a
 * shared event at the end of every frame and swaps the frame to the display only once that
 * value is reached; ignored, the frames were drawn (384 submissions in five seconds) and none
 * was ever shown. There is one ring here and the work of a command buffer is done in order,
 * so "reached this point" is "this command buffer is done": the values are set then.
 */
- (void)encodeSignalEvent:(id)event value:(uint64_t)value
{
	if (event == nil) {
		return;
	}
	RW_ONCE("a command buffer signals an event (a %s): its value is set when the buffer is done\n",
		class_getName(object_getClass(event)));
	if (_signals == nil) {
		_signals = [[NSMutableArray alloc] init];
	}
	[_signals addObject:@[ event, @(value) ]];
}

/* waiting is ordering, and a single in-order ring is ordered already */
- (void)encodeWaitForEvent:(id)event value:(uint64_t)value { (void)event; (void)value; }

- (id)computeCommandEncoderWithDispatchType:(NSUInteger)type
{
	(void)type;   /* serial and concurrent dispatch are the same here: one queue, in order */
	return [self computeCommandEncoder];
}

- (void)recordWork:(HakiosMetalWork *)work
{
	[work retainResources];
	[_work addObject:work];
}

- (void)enqueue { }

/* One blit (HakiosMetalBlitEncoder), at the commit, in its place among the draws and dispatches. */
enum { RW_BLIT_TEXTURE = 1, RW_BLIT_BUFFER, RW_BLIT_FILL, RW_BLIT_TO_TEXTURE, RW_BLIT_TO_BUFFER, RW_BLIT_MIPMAPS };

static BOOL
rw_run_blit(HakiosMetalWork *w, char *why, size_t size)
{
	static unsigned said[8];

	/* the first few of each kind, with what they move: which of them the window server uses, and on what */
	if (w->blit < 8 && said[w->blit]++ < 6) {
		id from = w->blitFrom, to = w->blitTo;
		BOOL ft = [from respondsToSelector:@selector(pixelFormat)], tt = [to respondsToSelector:@selector(pixelFormat)];

		rw_print("blit %u: from %s %lux%lu format %lu (level %u slice %u at %u,%u, %ux%u) to %s %lux%lu format %lu "
		         "(level %u slice %u at %u,%u); %zu bytes, offsets %zu %zu, row %zu\n", w->blit,
		         ft ? "texture" : from ? "buffer" : "-", ft ? (unsigned long)[from width] : 0ul,
		         ft ? (unsigned long)[from height] : 0ul, ft ? (unsigned long)[from pixelFormat] : 0ul, w->fromLevel,
		         w->fromSlice, w->fromOrigin[0], w->fromOrigin[1], w->blitSize[0], w->blitSize[1],
		         tt ? "texture" : to ? "buffer" : "-", tt ? (unsigned long)[to width] : 0ul,
		         tt ? (unsigned long)[to height] : 0ul, tt ? (unsigned long)[to pixelFormat] : 0ul, w->toLevel, w->toSlice,
		         w->toOrigin[0], w->toOrigin[1], w->blitBytes, w->fromOffset, w->toOffset, w->bytesPerRow);
	}
	switch (w->blit) {
	case RW_BLIT_TEXTURE:
		return mesa.texture_copy != NULL &&
		       MESA(texture_copy, [w->blitFrom mesaTexture], w->fromSlice, w->fromLevel, w->fromOrigin, w->blitSize,
					 [w->blitTo mesaTexture], w->toSlice, w->toLevel, w->toOrigin, why, size) == 0;
	case RW_BLIT_BUFFER:
		return mesa.buffer_copy != NULL &&
		       MESA(buffer_copy, [w->blitFrom mesaBuffer], w->fromOffset, [w->blitTo mesaBuffer], w->toOffset,
					w->blitBytes, why, size) == 0;
	case RW_BLIT_FILL:
		return mesa.buffer_fill != NULL &&
		       MESA(buffer_fill, [w->blitTo mesaBuffer], w->toOffset, w->blitBytes, w->fillValue, why, size) == 0;
	case RW_BLIT_TO_TEXTURE:
		return mesa.buffer_texture_copy != NULL &&
		       MESA(buffer_texture_copy, [w->blitFrom mesaBuffer], w->fromOffset, w->bytesPerRow, w->bytesPerImage,
						[w->blitTo mesaTexture], w->toSlice, w->toLevel, w->toOrigin, w->blitSize, 1,
						why, size) == 0;
	case RW_BLIT_TO_BUFFER:
		return mesa.buffer_texture_copy != NULL &&
		       MESA(buffer_texture_copy, [w->blitTo mesaBuffer], w->toOffset, w->bytesPerRow, w->bytesPerImage,
						[w->blitFrom mesaTexture], w->fromSlice, w->fromLevel, w->fromOrigin,
						w->blitSize, 0, why, size) == 0;
	case RW_BLIT_MIPMAPS:
		return mesa.generate_mipmaps != NULL && MESA(generate_mipmaps, [w->blitTo mesaTexture], why, size) == 0;
	}
	snprintf(why, size, "a blit of an unknown kind");
	return NO;
}

/* Metal's own rule, kept: the recorded dispatches run here and not when they were encoded. */
- (void)commit
{
	char why[512] = { 0 };
	uint64_t t_wait = mach_absolute_time();

	rw_pipe_enter();
	rw_commit_epoch++;
	/* the time from here on, and how long the lock kept this buffer waiting first (another
	 * thread's draw: the context is one, rw_pipe_lock) */
	uint64_t t0 = mach_absolute_time();
	unsigned draws = 0, dispatches = 0, blits = 0;
	for (HakiosMetalWork *work in _work) {
		if (work->blit != 0) {
			blits++;
		} else if (work->render != nil) {
			draws++;
		} else if (!work->clearOnly) {
			dispatches++;
		}
	}
	_status = RW_CB_COMMITTED;
	for (HakiosMetalWork *work in _work) {
		if (work->blit != 0) {
			if (!rw_run_blit(work, why, sizeof(why))) {
				rw_print("a blit (kind %u) failed: %s\n", work->blit, why);
			}
			continue;
		}
		if (work->clearOnly) {
			struct rwa_texture *color[8] = { NULL };

			for (unsigned i = 0; i < work->colorCount && i < 8; i++) {
				color[i] = [work->color[i] mesaTexture];
			}
			if (mesa.clear_pass_levels != NULL) {
				if (MESA(clear_pass_levels, color, work->colorLevel, work->colorSlice, work->colorCount,
					 work->clearMask, work->clearColor, [work->depth mesaTexture],
					 work->clearsDepth ? &work->clearDepth : NULL,
					 work->clearsStencil ? &work->clearStencil : NULL, why, sizeof(why)) != 0) {
					rw_print("a pass's clear failed: %s\n", why);
				}
			} else if (mesa.clear_pass != NULL &&
			    MESA(clear_pass, color, work->colorCount, work->clearMask, work->clearColor,
					    [work->depth mesaTexture], work->clearsDepth ? &work->clearDepth : NULL,
					    work->clearsStencil ? &work->clearStencil : NULL, why, sizeof(why)) != 0) {
				rw_print("a pass's clear failed: %s\n", why);
			}
			continue;
		}
		if (work->render != nil) {
			(void)[self runDraw:work why:why size:sizeof(why)];   /* (a draw that fails is said and skipped) */
			continue;
		}
		if (work->indirectBuffer != nil && !rw_resolve_indirect_dispatch(work)) {
			continue;
		}
		struct rwa_buffer *buffers[RW_MAX_BUFFERS] = { NULL };
		struct rwa_texture *textures[RW_MAX_BUFFERS] = { NULL };
		struct rwa_sampler *samplers[RW_MAX_BUFFERS] = { NULL };
		/* Metal's one [[buffer(n)]] table, passed BOTH ways: `buffers` is what is
		 * bound at each index, `table` is the same thing with Metal's own offset
		 * beside it, and the library takes from each side what that side owns -- an
		 * argument buffer's encoding offset and a buffer argument's own offset come
		 * from the table, and which an index is nobody but the shader knows. */
		struct rwa_buffer_bind table[RW_MAX_BUFFERS] = { { NULL, 0 } };

		for (unsigned i = 0; i < work->count; i++) {
			buffers[i] = [work->buffers[i] mesaBuffer];
			table[i].buffer = buffers[i];
			table[i].offset = work->bufferOffsets[i];
			if (buffers[i] == NULL && work->buffers[i] != nil) {
				rw_print("buffer %u of this dispatch did not come "
				         "from the compiler library, so it cannot be bound\n", i);
			}
		}
		for (unsigned i = 0; i < work->textureCount; i++) {
			textures[i] = [work->textures[i] mesaTexture];
		}
		for (unsigned i = 0; i < work->samplerCount; i++) {
			samplers[i] = [work->samplers[i] mesaSampler];
		}
		struct rwa_bindings bindings = {
			.buffers = buffers, .buffer_count = work->count,
			.textures = textures, .texture_count = work->textureCount,
			.samplers = samplers, .sampler_count = work->samplerCount,
		};

		/* rwa_dispatch_buffers when the library has it, which is the same call with
		 * Metal's offsets passed as well; a library too old to have it is used
		 * through rwa_dispatch, and then a kernel with a set is refused by name
		 * there and every offset is read as zero -- which is what an older library
		 * did with one in any case, out loud. */
		if (mesa.grid_threads != NULL) {
			MESA_VOID(grid_threads, [work->pipeline compute],
				  work->threads[0] != 0 ? work->threads : NULL);
		}
		if (mesa.threadgroup_memory_at != NULL) {
			for (unsigned k = 0; k < 32; k++) {
				MESA_VOID(threadgroup_memory_at, [work->pipeline compute], k,
					  work->threadgroupBytes[k]);
			}
		} else if (mesa.threadgroup_memory != NULL) {
			MESA_VOID(threadgroup_memory, [work->pipeline compute], work->threadgroupBytes[0]);
		}
		int rc = mesa.dispatch_buffers != NULL
			    ? MESA(dispatch_buffers, [work->pipeline compute], &bindings,
						    table, work->count, work->groups,
						    work->size, why, sizeof(why))
			    : MESA(dispatch, [work->pipeline compute], &bindings, work->groups,
					    work->size, why, sizeof(why));
		if (rc != 0) {
			static unsigned said;

			/* the one dispatch is lost, not the rest of the frame (see -failWith:) */
			if (said++ < 64) {
				rw_print("the dispatch failed: %s\n", why);
			}
		}
	}
	/* the draws of this command buffer, submitted together and waited for once (rwa_draw_wait(false)) */
	if (mesa.draw_wait != NULL && mesa.finish != NULL && [_work count] != 0 &&
	    MESA(finish, [[_queue device] mesaDevice], why, sizeof(why)) != 0) {
		rw_print("the command buffer's work did not finish: %s\n", why);
		_error = [[NSError errorWithDomain:@"HakiosMetal" code:3
					  userInfo:@{ NSLocalizedDescriptionKey: [NSString stringWithUTF8String:why] }] retain];
		_status = RW_CB_ERROR;
		goto done;
	}
	/* the surfaces this buffer drew into through copies of them, written back (-writeBackToSurface) */
	for (HakiosMetalWork *work in _work) {
		if (work->blit != 0) {
			if ([work->blitTo isKindOfClass:[HakiosMetalTexture class]]) {
				[(HakiosMetalTexture *)work->blitTo writeBackToSurface];
			}
			continue;
		}
		for (unsigned i = 0; i < work->colorCount && i < 8; i++) {
			if ([work->color[i] isKindOfClass:[HakiosMetalTexture class]]) {
				[(HakiosMetalTexture *)work->color[i] writeBackToSurface];
			}
		}
	}
	_status = RW_CB_SCHEDULED;   /* submitted; the fence below is what makes it completed */
done:
	rw_pipe_leave();
	rw_say_slow_buffer(draws, dispatches, blits, t_wait, t0);
	/* The queue's slot goes back here and on every way out of this method, a failed draw
	 * included: a buffer that errored is as finished as one that drew, and a slot kept
	 * because of an error would turn one bad draw into a queue that blocks for ever. */
	[self hakiosReturnSlot];
	/* and only then the handlers, so that a handler which makes the next command buffer
	 * is not waiting for a slot this one still holds */
	[self hakiosDeliverHandlers];
}

/* A draw, built out of what the encoder recorded. Separate from the dispatch loop because the
 * two have different bindings -- a draw has them per stage -- and folding them together would
 * mean a struct whose fields mean different things depending on a flag. */
/*
 * Boot-arg metal_bisect=S (seconds of uptime): from then on, for RW_BISECT_PHASE seconds each, the draws of one group
 * of render pipelines after another (RW_BISECT_GROUPS groups, by a hash of the pipeline's label, each named once in
 * the log) are skipped -- with a screen recording (rec_secs) the phase in which something black goes away names the
 * group, and the log names its pipelines. Then everything is drawn again.
 */
/*
 * The arguments of an indirect draw or dispatch, out of the buffer the application named, at the
 * moment the work runs: Metal's MTLDrawPrimitivesIndirectArguments (vertexCount, instanceCount,
 * vertexStart, baseInstance), MTLDrawIndexedPrimitivesIndirectArguments (indexCount,
 * instanceCount, indexStart, baseVertex, baseInstance) and MTLDispatchThreadgroupsIndirectArguments
 * (threadgroupsPerGrid), all 32-bit words. The buffer is shared memory, so the processor reads
 * what the GPU wrote once the work before has been waited for -- which it has, the command buffer's
 * work running in order. NO, said, for arguments that do not fit the buffer.
 */
static const uint32_t *
rw_indirect_words(HakiosMetalWork *work, unsigned words)
{
	const uint8_t *bytes = [work->indirectBuffer contents];
	NSUInteger length = [work->indirectBuffer length];

	if (bytes == NULL || work->indirectOffset % 4 != 0 ||
	    work->indirectOffset + 4 * (size_t)words > length) {
		rw_print("the arguments of an indirect %s are %u words at byte %zu of a buffer of %lu "
		         "bytes%s, and are not read\n", work->render != nil ? "draw" : "dispatch", words,
		         work->indirectOffset, (unsigned long)length,
		         bytes == NULL ? " the processor cannot read" : "");
		return NULL;
	}
	return (const uint32_t *)(bytes + work->indirectOffset);
}

static BOOL
rw_resolve_indirect_draw(HakiosMetalWork *work)
{
	const uint32_t *w = rw_indirect_words(work, work->indexBuffer != nil ? 5 : 4);

	if (w == NULL) {
		return NO;
	}
	if (work->indexBuffer == nil) {
		work->vertexCount = w[0];
		work->instanceCount = w[1];
		work->vertexStart = w[2];
		work->baseInstance = w[3];
	} else {
		work->vertexCount = w[0];
		work->instanceCount = w[1];
		/* the first index is an offset into the index buffer, in indices */
		work->indexBufferOffset += (size_t)w[2] * (work->indexType == 1 ? 4 : 2);
		work->baseVertex = (int32_t)w[3];
		work->baseInstance = w[4];
	}
	return work->vertexCount != 0 && work->instanceCount != 0;
}

static BOOL
rw_resolve_indirect_dispatch(HakiosMetalWork *work)
{
	const uint32_t *w = rw_indirect_words(work, 3);

	if (w == NULL) {
		return NO;
	}
	work->groups[0] = w[0];
	work->groups[1] = w[1];
	work->groups[2] = w[2];
	/* an empty grid is a dispatch of nothing, which Metal runs as nothing */
	return w[0] != 0 && w[1] != 0 && w[2] != 0;
}

static uint64_t
rw_bisect_start(void)
{
	static int known;
	static uint64_t start;

	if (!known) {
		char args[1024] = { 0 }, *at;
		size_t size = sizeof(args) - 1;

		known = 1;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "metal_bisect=")) != NULL) {
			start = strtoull(at + strlen("metal_bisect="), NULL, 10);
		}
	}
	return start;
}

static int
rw_bisect_skipped_group(void)
{
	static mach_timebase_info_data_t tb;
	static int said = -1;
	uint64_t start = rw_bisect_start(), up;
	int phase;

	if (start == 0) {
		return -1;
	}
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	up = mach_absolute_time() * tb.numer / tb.denom / 1000000000ull;
	if (up < start) {
		return -1;
	}
	phase = (int)((up - start) / RW_BISECT_PHASE);
	if (phase >= rw_bisect_phases()) {
		phase = -1;
	}
	if (phase != said) {
		said = phase;
		rw_print("bisect: at %llu s of uptime, %s%d\n", up, phase < 0 ? "every group drawn again" : "skipping group ",
		         phase < 0 ? 0 : phase);
	}
	return phase;
}

- (BOOL)runDraw:(HakiosMetalWork *)work why:(char *)why size:(size_t)size
{
	if (work->indirectBuffer != nil && !rw_resolve_indirect_draw(work)) {
		return YES;
	}
	struct rwa_texture *color[8] = { NULL };
	struct rwa_buffer *vertex_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_vertex_stream vertex_streams[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer *fragment_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_texture *fragment_textures[RW_MAX_BUFFERS] = { NULL };
	struct rwa_sampler *fragment_samplers[RW_MAX_BUFFERS] = { NULL };
	/* and Metal's own table for each stage: the same entries as the arrays above with
	 * the OFFSET beside each, which rwa_bindings has nowhere to put. One array for
	 * both of the things an offset means -- which encoding of an argument buffer, and
	 * which block of a buffer argument -- because Metal has one call behind both. */
	struct rwa_buffer_bind vertex_table[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer_bind fragment_table[RW_MAX_BUFFERS] = { { NULL, 0 } };

	{
		int skipped = rw_bisect_skipped_group();

		if (skipped >= 0 && work->render != nil && ((HakiosMetalRenderPipeline *)work->render)->_bisectGroup == (unsigned)skipped) {
			return YES;
		}
	}
	/* boot-arg metal_skip=a,b:N: the draws of every pipeline whose "vertex/fragment" function names contain
	 * one of these (with ":N", only its draws of more than N vertices) are not made (run glass29: skipping
	 * uber_vertex's draws of more than 4 vertices -- RenderBox's paths -- is what kept the GPU going).
	 * boot-arg metal_tiny=...: the same choice of draws, made with a 1x1 scissor -- every vertex shaded,
	 * next to no fragments -- which says whether the vertex or the fragment stage stops the GPU. */
	BOOL tiny = NO;
	if (work->render != nil) {
		const char *names = ((HakiosMetalRenderPipeline *)work->render)->_names;
		if (rw_draw_chosen("metal_skip=", names, work->vertexCount)) {
			return YES;
		}
		tiny = rw_draw_chosen("metal_tiny=", names, work->vertexCount);
	}
	if (mesa.render_draw == NULL) {
		snprintf(why, size, "this compiler library can compute but not yet draw");
		[self failWith:why];
		return NO;
	}
	for (unsigned i = 0; i < work->colorCount; i++) {
		color[i] = [work->color[i] mesaTexture];
	}
	/*
	 * Metal's one vertex buffer table, split in two by the pipeline's own mask: an index
	 * the vertex descriptor fetches attributes from is a STREAM, with its offset, and
	 * every other index is a [[buffer(n)]] ARGUMENT of the vertex stage. The library
	 * refuses a descriptor that claims an index the module also reads as a buffer
	 * (air_metal.h), so the two sets cannot overlap and this split cannot drop a
	 * binding.
	 *
	 * The offset is honoured on the stream side and still unimplemented on the argument
	 * side, and that asymmetry is said out loud rather than left in a comment: the
	 * library takes a stream's offset as Metal's own `offset:` and rwa_bindings has
	 * nowhere to put one.
	 */
	unsigned stream_mask = [work->render vertexStreamMask];

	for (unsigned i = 0; i < work->vertexBufferCount; i++) {
		/* Metal's own offset, for whichever of the other two things index i is: an
		 * argument buffer (which encoding to read) or a buffer argument (which
		 * block). The library takes it from here and judges it there, where the
		 * shader's own address space is in hand; a stream's offset is the stream
		 * array's below and an index is never both. */
		if (!(stream_mask & (1u << i))) {
			vertex_table[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_table[i].offset = work->vertexBufferOffsets[i];
		}
		if (i < RWA_MAX_VERTEX_BUFFERS && (stream_mask & (1u << i))) {
			vertex_streams[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_streams[i].offset = work->vertexBufferOffsets[i];
			if (work->vertexBuffers[i] != nil &&
			    vertex_streams[i].buffer == NULL) {
				snprintf(why, size, "the buffer bound as vertex stream %u did "
						    "not come from the compiler library, so "
						    "there is nothing to fetch vertices out "
						    "of", i);
				[self failWith:why];
				return NO;
			}
			continue;
		}
		vertex_buffers[i] = [work->vertexBuffers[i] mesaBuffer];
	}
	if ([work->render vertexStreamCount] > RW_MAX_BUFFERS) {
		snprintf(why, size, "this pipeline fetches from %u vertex stream slots and "
				    "this driver's table holds %u", [work->render
				    vertexStreamCount], RW_MAX_BUFFERS);
		[self failWith:why];
		return NO;
	}
	for (unsigned i = 0; i < work->count; i++) {
		fragment_buffers[i] = [work->buffers[i] mesaBuffer];
		fragment_table[i].buffer = fragment_buffers[i];
		fragment_table[i].offset = work->bufferOffsets[i];
		if (work->buffers[i] != nil && fragment_buffers[i] == NULL) {
			snprintf(why, size, "the buffer bound at fragment buffer index %u did "
					    "not come from the compiler library, so there is "
					    "nothing to bind", i);
			[self failWith:why];
			return NO;
		}
	}
	for (unsigned i = 0; i < work->textureCount; i++) {
		fragment_textures[i] = [work->textures[i] mesaTexture];
	}
	for (unsigned i = 0; i < work->samplerCount; i++) {
		fragment_samplers[i] = work->samplerClamped[i] &&
		    [work->samplers[i] respondsToSelector:@selector(mesaSamplerWithLodMin:max:)]
		    ? [work->samplers[i] mesaSamplerWithLodMin:work->samplerLod[i][0] max:work->samplerLod[i][1]]
		    : [work->samplers[i] mesaSampler];
	}
	struct rwa_draw draw = {
		.color = color,
		.color_count = work->colorCount,
		.color_level = work->colorLevel,
		.color_layer = work->colorSlice,
		.clear_colors = work->clearColor,
		.clear_mask = work->clearMask,
		/* The depth attachment, its two load actions, the state that was bound and
		 * the depth range of the viewport -- which is where the quad's own depth
		 * comes from, the generated vertex stage putting every corner at clip z = 0
		 * (-setViewport: has the whole of it). Without a viewport the contract's
		 * default [0, 1] applies and the quad sits at 0, the near plane. */
		.depth = [work->depth mesaTexture],
		.clear_depth = work->clearsDepth ? &work->clearDepth : NULL,
		.clear_stencil = work->clearsStencil ? &work->clearStencil : NULL,
		.depth_stencil = [work->depthStencilState mesaDepthStencil],
		.stencil_reference = { work->stencilReference[0], work->stencilReference[1] },
		.depth_range = work->hasDepthRange ? work->depthRange : NULL,
		.viewport = work->hasViewport ? work->viewport : NULL,
		.scissor = tiny ? (const unsigned[4]){ 0, 0, 1, 1 } : work->hasScissor ? work->scissor : NULL,
		.vertex = { .buffers = vertex_buffers, .buffer_count = work->vertexBufferCount },
		/* and the streams the layout fetches from, at Metal's own indices with
		 * Metal's own offsets; the count is what the library needs covered, which
		 * is the highest index the pipeline fetches from plus one. */
		.vertex_streams = vertex_streams,
		.vertex_stream_count = [work->render vertexStreamCount],
		.fragment = {
			.buffers = fragment_buffers, .buffer_count = work->count,
			.textures = fragment_textures, .texture_count = work->textureCount,
			.samplers = fragment_samplers, .sampler_count = work->samplerCount,
		},
		.primitive = work->primitive,
		.vertex_start = work->vertexStart,
		.vertex_count = work->vertexCount,
		/* The index buffer, by the same rule as every other resource here: it has to
		 * be one of ours, because what the library needs is the Gallium resource
		 * behind it and a buffer from the kernel path (or from another driver) has
		 * none. -mesaBuffer is nil for such a buffer, and nil here would be a draw
		 * the library reads as non-indexed -- the vertices in the wrong order, which
		 * is the one outcome worse than an error. So it is caught where the mapping
		 * fails and not left to the library, which has no way to tell the two apart. */
		.index_buffer = [work->indexBuffer mesaBuffer],
		.index_type = work->indexType,
		.index_buffer_offset = work->indexBufferOffset,
		.base_vertex = work->baseVertex,
		.instance_count = work->instanceCount,
		.base_instance = work->baseInstance,
		/* Metal's own table for each stage, at Metal's own indices: the offsets the
		 * arrays above have nowhere to put. The library takes an argument buffer's
		 * whole binding from here and a buffer argument's offset only, so the two
		 * readings cannot disagree -- and where both name a buffer it checks that
		 * they name the same one. */
		.vertex_buffer_table = vertex_table,
		.vertex_buffer_table_count = work->vertexBufferCount,
		.fragment_buffer_table = fragment_table,
		.fragment_buffer_table_count = work->count,
		.cull_mode = work->cullMode,
		.winding = work->winding,
		.fill_mode = work->fillMode,
		.depth_clamp = work->depthClamp,
		.depth_bias = work->depthBias[0],
		.depth_slope_scale = work->depthBias[1],
		.depth_bias_clamp = work->depthBias[2],
	};

	if (work->indexBuffer != nil && draw.index_buffer == NULL) {
		snprintf(why, size, "the index buffer of this draw did not come from the "
		         "compiler library, so there is nothing to read the indices out of");
		[self failWith:why];
		return NO;
	}

	/* boot-arg metal_sync=1: each draw said before it goes and waited for after, so that the
	 * draw a GPU hang starts at is the last one said (run glass23/24: the GPU stopped once
	 * RenderBox's uber_fragment drew, with nothing to say which draw) */
	static int sync = -1;
	if (sync < 0) {
		char args[1024] = "";
		size_t sz = sizeof(args) - 1;
		sync = sysctlbyname("kern.bootargs", args, &sz, NULL, 0) == 0 && strstr(args, "metal_sync=1") != NULL;
	}
	if (sync) {
		rw_print("sync draw %s (%s): %u colours (formats %lu %lu %lu, the first %lux%lu), %u vertices "
		         "x %u, clears 0x%x (%.2f %.2f %.2f %.2f), viewport %s%.0f,%.0f %.0fx%.0f, scissor %s%u,%u %ux%u, "
		         "depth %s%s\n",
		         [[work->pipeline label] UTF8String] ?: "?",
		         work->render != nil ? ((HakiosMetalRenderPipeline *)work->render)->_names : "-", work->colorCount,
		         work->colorCount > 0 ? (unsigned long)[work->color[0] pixelFormat] : 0ul,
		         work->colorCount > 1 ? (unsigned long)[work->color[1] pixelFormat] : 0ul,
		         work->colorCount > 2 ? (unsigned long)[work->color[2] pixelFormat] : 0ul,
		         work->colorCount > 0 ? (unsigned long)[work->color[0] width] : 0ul,
		         work->colorCount > 0 ? (unsigned long)[work->color[0] height] : 0ul,
		         work->vertexCount, work->instanceCount, work->clearMask, work->clearColor[0][0],
		         work->clearColor[0][1], work->clearColor[0][2], work->clearColor[0][3],
		         work->hasViewport ? "" : "(none) ", work->viewport[0], work->viewport[1], work->viewport[2],
		         work->viewport[3], work->hasScissor ? "" : "(none) ", work->scissor[0], work->scissor[1],
		         work->scissor[2], work->scissor[3], work->depth != nil ? "yes" : "no",
		         work->depthStencilState != nil ? " with a state" : "");
	}
	if (MESA(render_draw, [work->render render], &draw, why, size) != 0) {
		rw_print("the draw failed: %s\n", why);
		[self failWith:why];
		return NO;
	}
	if (sync && mesa.finish != NULL && MESA(finish, [[_queue device] mesaDevice], why, size) != 0) {
		rw_print("sync: the draw did not finish: %s\n", why);
	}
	if (sync) {
		rw_say_sync_inputs(work);
	}
	return YES;
}

/*
 * metal_sync=1, for the first draws into a screen-sized attachment: the first words of the buffers
 * each stage was given and, the draw finished, the pixel at the middle of where it may land. A frame
 * that comes out all zero is a vertex stage that put everything nowhere, a fragment stage that
 * wrote zero or a target nothing reached, and these numbers tell which.
 */
static void
rw_say_sync_inputs(HakiosMetalWork *work)
{
	static unsigned said;

	if (work->colorCount == 0 || [work->color[0] width] < 1000 || said >= 16) {
		return;
	}
	said++;
	for (int stage = 0; stage < 2; stage++) {
		unsigned n = stage == 0 ? work->vertexBufferCount : work->count;

		for (unsigned i = 0; i < n && i < 6; i++) {
			id b = stage == 0 ? work->vertexBuffers[i] : work->buffers[i];
			size_t off = stage == 0 ? work->vertexBufferOffsets[i] : work->bufferOffsets[i];
			const float *f;

			if (b == nil || ![b respondsToSelector:@selector(contents)] ||
			    (f = (const float *)((const uint8_t *)[b contents] + off)) == NULL ||
			    [b length] < off + 32) {
				continue;
			}
			rw_print("sync   %s buffer %u (%lu bytes at %zu): %g %g %g %g %g %g %g %g\n",
			         stage == 0 ? "vertex" : "fragment", i, (unsigned long)[b length], off,
			         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
		}
	}
	unsigned x = work->hasScissor ? work->scissor[0] + work->scissor[2] / 2 : (unsigned)[work->color[0] width] / 2;
	unsigned y = work->hasScissor ? work->scissor[1] + work->scissor[3] / 2 : (unsigned)[work->color[0] height] / 2;
	uint8_t px[16] = { 0 };

	if ([work->color[0] respondsToSelector:@selector(getBytes:bytesPerRow:fromRegion:mipmapLevel:)]) {
		[work->color[0] getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(x, y, 1, 1)
			     mipmapLevel:work->colorLevel[0]];
	}
	rw_print("sync   after it, attachment 0 at %u,%u: %02x %02x %02x %02x\n", x, y, px[0], px[1], px[2], px[3]);
}

/*
 * One draw that could not be made: said (the first few of each run), and only that draw is lost. It used to put the
 * whole command buffer in error and end it there: every draw after it in the frame was dropped, and the window server,
 * told the frame had failed, did not show it -- black blocks and whole black frames that came and went.
 */
- (void)failWith:(const char *)reason
{
	static unsigned said;

	if (said++ < 64) {
		rw_print("a draw is skipped: %s\n", reason);
	}
}

/* The fence is the whole point: a dispatch that the GPU does not finish must not read as a
 * result. air_run learned that the expensive way (docs/air.md section 5). */
- (void)waitUntilCompleted
{
	char why[512] = { 0 };

	if (_status == RW_CB_ERROR) {
		return;
	}
	rw_pipe_enter();
	int unfinished = MESA(finish, [[_queue device] mesaDevice], why, sizeof(why));
	rw_pipe_leave();
	if (unfinished != 0) {
		rw_print("THE GPU DID NOT FINISH: %s\n", why);
		_error = [[NSError errorWithDomain:@"HakiosMetal" code:3
					  userInfo:@{ NSLocalizedDescriptionKey:
						      [NSString stringWithUTF8String:why] }] retain];
		_status = RW_CB_ERROR;
		return;
	}
	_status = RW_CB_COMPLETED;   /* the fence has been paid, and only now */
}

- (void)waitUntilScheduled { }

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
- (void)hakiosDeliver:(void (^)(id))handler
{
	dispatch_queue_t queue = [_queue hakiosCompletionQueue];

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
	if (_status >= RW_CB_SCHEDULED) {
		[self hakiosDeliver:handler];
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
	if (_status >= RW_CB_SCHEDULED) {
		[self hakiosDeliver:handler];
		return;
	}
	if (_completed == nil) {
		_completed = [[NSMutableArray alloc] init];
	}
	[_completed addObject:[[handler copy] autorelease]];
}

/* Called from the one place the work is done. The order is Metal's: scheduled first, then
 * the fence, then completed. */
- (void)hakiosDeliverHandlers
{
	NSArray *scheduled = _scheduled;
	NSArray *completed = _completed;
	NSArray *signals = _signals;

	_scheduled = nil;
	_completed = nil;
	_signals = nil;
	for (id handler in scheduled) {
		[self hakiosDeliver:(void (^)(id))handler];
	}
	if ([signals count] > 0) {
		[self waitUntilCompleted];
		for (NSArray *pair in signals) {
			id event = pair[0];
			uint64_t value = [pair[1] unsignedLongLongValue];

			if ([event respondsToSelector:@selector(setSignaledValue:)] &&
			    (![event respondsToSelector:@selector(signaledValue)] ||
			     [(id<MTLSharedEvent>)event signaledValue] < value)) {
				[(id<MTLSharedEvent>)event setSignaledValue:value];
			}
		}
		[signals release];
	}
	if ([completed count] > 0 && _status != RW_CB_ERROR) {
		/* A completed handler is a promise about the GPU, so it is made true before it
		 * is kept: -waitUntilCompleted is this driver's fence and sets the status the
		 * handler will read. */
		[self waitUntilCompleted];
	}
	for (id handler in completed) {
		[self hakiosDeliver:(void (^)(id))handler];
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

- (void)addPurgedResource:(id)resource { (void)resource; [self hakiosPurgedNote]; }
- (void)addPurgedHeap:(id)heap         { (void)heap; [self hakiosPurgedNote]; }

/* Metal records on a command buffer every resource of its that was purged while the buffer
 * referenced it, so that -commit can fail the buffer instead of reading pages that are
 * gone. Nothing of this driver's is ever volatile -- -setPurgeableState: answers
 * MTLPurgeableStateNonVolatile for every resource, which is what they are -- so a report
 * here means Metal and this driver disagree about a resource, and that is worth a line
 * rather than a silent buffer that fails later for no stated reason. */
- (void)hakiosPurgedNote
{
	RW_ONCE("Metal reported a purged resource on a command buffer, but "
		"nothing this driver allocates is volatile; the report is noted and the "
		"buffer is not failed\n");
}


@end

@implementation HakiosMetalRenderEncoder {
	HakiosMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder */
	HakiosMetalRenderPipeline *_pipeline;
	id _color[8];
	unsigned _colorCount;
	/* each attachment's MTLRenderPassColorAttachmentDescriptor level and slice: the window server's
	 * variable blur renders its pyramid into levels 1, 2, ... of one texture (rwa_draw.color_level) */
	unsigned _colorLevel[8], _colorSlice[8];
	/* where each attachment is resolved to at the end of the pass (endEncoding), or nil */
	id _resolveTo[8];
	unsigned _resolveLevel[8], _resolveSlice[8];
	float _clearColor[8][4];
	unsigned _clearMask;
	BOOL _drew;                     /* a draw has been recorded in this pass */
	id _indirect;                   /* the next draw's arguments are here (an indirect draw) */
	NSUInteger _indirectOffset;
	unsigned _cullMode, _winding, _fillMode, _depthClamp;   /* raster state, Metal's enums */
	float _depthBias[3];
	id _depth;
	id _depthStencilState;
	float _clearDepth;
	unsigned _clearStencil;
	BOOL _clearsDepth, _clearsStencil;
	unsigned _stencilReference[2];
	float _depthRange[2];
	BOOL _hasDepthRange;
	float _viewport[4];
	BOOL _hasViewport;
	unsigned _scissor[4];
	BOOL _hasScissor;
	id _vertexBuffers[RW_MAX_BUFFERS];
	NSUInteger _vertexBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentBuffers[RW_MAX_BUFFERS];
	NSUInteger _fragmentBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentTextures[RW_MAX_BUFFERS];
	id _fragmentSamplers[RW_MAX_BUFFERS];
	/* -setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:'s range per slot; _fragmentSamplerClamped[i] says
	 * it is one */
	float _fragmentSamplerLod[RW_MAX_BUFFERS][2];
	BOOL _fragmentSamplerClamped[RW_MAX_BUFFERS];
	unsigned _vertexBufferCount, _fragmentBufferCount;
	unsigned _fragmentTextureCount, _fragmentSamplerCount;
	BOOL _complained;
}

- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer
				  descriptor:(MTLRenderPassDescriptor *)descriptor
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;

	/* What the pass is drawing into, and whether it begins by clearing. Metal keeps this in
	 * the pass descriptor rather than in the pipeline, so it belongs to the encoder. */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		id texture = attachment != nil ? [attachment texture] : nil;

		if (texture == nil) {
			continue;
		}
		_color[i] = texture;
		_colorCount = i + 1;
		_colorLevel[i] = (unsigned)[attachment level];
		_colorSlice[i] = (unsigned)[attachment slice];
		/*
		 * A resolve is the one thing a multisample pass asks for that this driver
		 * cannot do, and it is said out loud rather than ignored. The compiler
		 * library makes multisample attachments and renders into them
		 * (docs/metal.md 10.34), but it stores what it has and resolves nothing --
		 * so for an application the one-sample texture it means to READ is the
		 * resolveTexture, and that one would come back holding whatever it held
		 * before. Silence here would read as a pass that drew nothing. The same
		 * goes for MTLStoreActionMultisampleResolve on the depth attachment, which
		 * nothing has asked for yet.
		 */
		NSUInteger store = [attachment storeAction];

		/* THE RESOLVE, as a copy into the resolveTexture once the pass is drawn (endEncoding):
		 * the library's texture copy blits a multisample source, which is Gallium's resolve.
		 * RenderBox (SwiftUI, SpringBoard's icons and widgets) draws into an attachment and
		 * asks for its resolve, and with none the texture it shows held nothing (run glass21). */
		if ((store == MTLStoreActionMultisampleResolve ||
		     store == MTLStoreActionStoreAndMultisampleResolve) &&
		    [attachment resolveTexture] != nil) {
			_resolveTo[i] = [[attachment resolveTexture] retain];
			_resolveLevel[i] = (unsigned)[attachment resolveLevel];
			_resolveSlice[i] = (unsigned)[attachment resolveSlice];
		}
		if (i < 8 && [attachment loadAction] == MTLLoadActionClear) {
			MTLClearColor c = [attachment clearColor];

			_clearColor[i][0] = (float)c.red;
			_clearColor[i][1] = (float)c.green;
			_clearColor[i][2] = (float)c.blue;
			_clearColor[i][3] = (float)c.alpha;
			_clearMask |= 1u << i;
		}
	}
	/*
	 * The depth/stencil attachment, which is two descriptors over one texture: for
	 * MTLPixelFormatDepth32Float_Stencil8 an application sets the same texture on
	 * both, and for a depth-only format it sets only the first. So whichever of the
	 * two carries a texture is the attachment, and both carrying DIFFERENT textures is
	 * refused out loud -- a Gallium framebuffer has one zsbuf, and quietly taking the
	 * depth one would drop the stencil half of what was asked for.
	 *
	 * Each aspect keeps its own load action, because the one struct Metal has behind
	 * both descriptors holds clearDepth and clearStencil separately
	 * (MTLRenderPassAttachmentDescriptorPrivate) and a pass that clears the depth and
	 * loads the stencil is an ordinary pass.
	 */
	MTLRenderPassDepthAttachmentDescriptor *za = [descriptor depthAttachment];
	MTLRenderPassStencilAttachmentDescriptor *sa = [descriptor stencilAttachment];
	id ztex = [za texture];
	id stex = [sa texture];

	if (ztex != nil && stex != nil && ztex != stex) {
		rw_print("the depth attachment and the stencil attachment are two "
		         "different textures; one texture carries both aspects here, so this "
		         "pass is encoded with neither\n");
	} else {
		_depth = ztex != nil ? ztex : stex;
		if (ztex != nil && [za loadAction] == MTLLoadActionClear) {
			_clearDepth = (float)[za clearDepth];
			_clearsDepth = YES;
		}
		if (stex != nil && [sa loadAction] == MTLLoadActionClear) {
			_clearStencil = (unsigned)[sa clearStencil];
			_clearsStencil = YES;
		}
	}
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setRenderPipelineState:(id)state { _pipeline = state; }

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
	if (index >= RW_MAX_BUFFERS) {
		rw_print("vertex buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_vertexBuffers[index] = buffer;
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
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
	if (index >= RW_MAX_BUFFERS) {
		rw_print("fragment buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_fragmentBuffers[index] = buffer;
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
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
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentTextures[index] = texture;
	if (index + 1 > _fragmentTextureCount) {
		_fragmentTextureCount = (unsigned)index + 1;
	}
}

- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentSamplers[index] = sampler;
	_fragmentSamplerClamped[index] = NO;
	if (index + 1 > _fragmentSamplerCount) {
		_fragmentSamplerCount = (unsigned)index + 1;
	}
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
	id first = _colorCount > 0 ? _color[0] : _depth;
	NSUInteger w = [first width], h = [first height];

	_hasViewport = viewport.originX != 0.0 || viewport.originY != 0.0 ||
	    (NSUInteger)viewport.width != w || (NSUInteger)viewport.height != h;
	_viewport[0] = (float)viewport.originX;
	_viewport[1] = (float)viewport.originY;
	_viewport[2] = (float)viewport.width;
	_viewport[3] = (float)viewport.height;
	_depthRange[0] = (float)viewport.znear;
	_depthRange[1] = (float)viewport.zfar;
	_hasDepthRange = YES;
}
- (void)setScissorRect:(MTLScissorRect)rect
{
	id first = _colorCount > 0 ? _color[0] : _depth;

	_hasScissor = rect.x != 0 || rect.y != 0 || rect.width != [first width] || rect.height != [first height];
	_scissor[0] = (unsigned)rect.x;
	_scissor[1] = (unsigned)rect.y;
	_scissor[2] = (unsigned)rect.width;
	_scissor[3] = (unsigned)rect.height;
}
/* Raster state, kept on the encoder as Metal keeps it and handed to each draw (rwa_draw): every
 * draw was two-sided, filled, clipped and unbiased whatever was set here. */
- (void)setCullMode:(NSUInteger)mode                  { _cullMode = mode <= 2 ? (unsigned)mode : 0; }
- (void)setFrontFacingWinding:(NSUInteger)winding     { _winding = winding == 1; }
- (void)setTriangleFillMode:(NSUInteger)mode          { _fillMode = mode == 1; }

/* The depth/stencil state, recorded like the pipeline state: nothing runs at encode time,
 * so what a draw tests against is whatever was bound when that draw was encoded. A state
 * from another driver -- or nil, which is Metal's "no test, no write" -- is passed on as
 * nil, and the library draws with its own zeroed state, which is the same thing. */
- (void)setDepthStencilState:(id)state
{
	if (state != nil && ![state isKindOfClass:[HakiosMetalDepthStencilState class]]) {
		rw_print("this depth/stencil state is a %s and not one of ours, so "
		         "it cannot be bound; this pass draws with no depth test\n",
		         class_getName(object_getClass(state)));
		state = nil;
	}
	_depthStencilState = state;
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
	HakiosMetalWork *work = [[HakiosMetalWork alloc] init];

	if (_pipeline == nil || (_colorCount == 0 && _depth == nil)) {
		rw_print("this draw has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "nothing to draw into");
		[work release];
		return;
	}
	/*
	 * What the contract carries in 32 bits and Metal hands over in 64. These are not
	 * limits of this driver's choosing: `index_bias` in Gallium is a signed int and
	 * instance counts there are unsigned, so a number that does not fit is a number
	 * that would be recorded as a DIFFERENT number -- a base vertex of 2^32 as 0, and
	 * an instance count of 2^32 + 3 as 3. Saying so is the only answer that is not a
	 * wrong picture.
	 */
	if (baseVertex < INT32_MIN || baseVertex > INT32_MAX || instances > UINT32_MAX ||
	    baseInstance > UINT32_MAX || count > UINT32_MAX || start > UINT32_MAX) {
		rw_print("this draw does not fit the contract's 32 bits (count %lu, "
		         "start %lu, base vertex %ld, %lu instances from %lu) and is dropped "
		         "rather than wrapped\n", (unsigned long)count, (unsigned long)start,
		         (long)baseVertex, (unsigned long)instances, (unsigned long)baseInstance);
		[work release];
		return;
	}
	/* Said at the draw and not at the pass, because it is the pipeline that knows:
	 * Metal drops a [[depth]] write when there is no depth attachment, and so does
	 * this -- but a caller measuring such a shader would otherwise be left reading a
	 * colour attachment that holds its clear value and nothing else. */
	if ([_pipeline writesDepth] && _depth == nil && !_complained) {
		rw_print("this pipeline's fragment shader stores to [[depth]] and "
		         "this pass has no depth attachment, so those writes go nowhere\n");
		_complained = YES;
	}
	work->render = _pipeline;
	work->colorCount = _colorCount;
	for (unsigned i = 0; i < _colorCount; i++) {
		work->color[i] = _color[i];
		work->colorLevel[i] = _colorLevel[i];
		work->colorSlice[i] = _colorSlice[i];
	}
	work->clearMask = _clearMask;
	memcpy(work->clearColor, _clearColor, sizeof(work->clearColor));
	work->depth = _depth;
	work->depthStencilState = _depthStencilState;
	work->clearDepth = _clearDepth;
	work->clearStencil = _clearStencil;
	work->clearsDepth = _clearsDepth;
	work->clearsStencil = _clearsStencil;
	work->stencilReference[0] = _stencilReference[0];
	work->stencilReference[1] = _stencilReference[1];
	work->depthRange[0] = _depthRange[0];
	work->depthRange[1] = _depthRange[1];
	work->hasDepthRange = _hasDepthRange;
	memcpy(work->viewport, _viewport, sizeof(_viewport));
	work->hasViewport = _hasViewport;
	memcpy(work->scissor, _scissor, sizeof(_scissor));
	work->hasScissor = _hasScissor;
	work->primitive = (unsigned)primitive;
	work->vertexStart = (unsigned)start;
	work->vertexCount = (unsigned)count;
	work->indexBuffer = indexBuffer;
	work->indexType = (unsigned)indexType;
	work->indexBufferOffset = indexOffset;
	work->baseVertex = (int)baseVertex;
	work->instanceCount = (unsigned)instances;
	work->baseInstance = (unsigned)baseInstance;
	work->indirectBuffer = _indirect;
	work->indirectOffset = (size_t)_indirectOffset;
	work->cullMode = _cullMode;
	work->winding = _winding;
	work->fillMode = _fillMode;
	work->depthClamp = _depthClamp;
	memcpy(work->depthBias, _depthBias, sizeof(_depthBias));
	work->vertexBufferCount = _vertexBufferCount;
	for (unsigned i = 0; i < _vertexBufferCount; i++) {
		work->vertexBuffers[i] = _vertexBuffers[i];
		work->vertexBufferOffsets[i] = (size_t)_vertexBufferOffsets[i];
	}
	work->count = _fragmentBufferCount;
	for (unsigned i = 0; i < _fragmentBufferCount; i++) {
		work->buffers[i] = _fragmentBuffers[i];
		work->bufferOffsets[i] = (size_t)_fragmentBufferOffsets[i];
	}
	work->textureCount = _fragmentTextureCount;
	for (unsigned i = 0; i < _fragmentTextureCount; i++) {
		work->textures[i] = _fragmentTextures[i];
	}
	work->samplerCount = _fragmentSamplerCount;
	for (unsigned i = 0; i < _fragmentSamplerCount; i++) {
		work->samplers[i] = _fragmentSamplers[i];
		work->samplerClamped[i] = _fragmentSamplerClamped[i];
		work->samplerLod[i][0] = _fragmentSamplerLod[i][0];
		work->samplerLod[i][1] = _fragmentSamplerLod[i][1];
	}
	[_commandBuffer recordWork:work];
	[work release];
	/* the load actions happen once, before the pass's first draw: a later draw that cleared
	 * again wiped everything drawn before it in the pass (black where it had drawn) */
	_clearMask = 0;
	_clearsDepth = NO;
	_clearsStencil = NO;
	_drew = YES;
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
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
}

/* The same for the fragment stage, and now with the same answer as the vertex one: the
 * offset is recorded against the index already bound there, and the draw judges it. An
 * application that moves an argument buffer's offset between draws -- which is the
 * ordinary way to walk a table of encodings -- is doing nothing this driver has to
 * refuse. */
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
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
	[self setFragmentSamplerState:sampler atIndex:index];
	if (index < RW_MAX_BUFFERS) {
		_fragmentSamplerLod[index][0] = lodMin;
		_fragmentSamplerLod[index][1] = lodMax;
		_fragmentSamplerClamped[index] = YES;
	}
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
	if (![indirectBuffer isKindOfClass:[HakiosMetalBuffer class]]) {
		rw_print("an indirect draw's arguments are in a %s and not one of our buffers, "
		         "and this draw is dropped\n",
		         indirectBuffer ? class_getName(object_getClass(indirectBuffer)) : "nil");
		return;
	}
	/* recorded with the counts to come: they are read when the draw runs (runDraw) */
	_indirect = indirectBuffer;
	_indirectOffset = offset;
	[self recordDraw:primitive vertexStart:0 count:1 indexBuffer:nil indexType:0 indexOffset:0
	      baseVertex:0 instanceCount:1 baseInstance:0];
	_indirect = nil;
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
	       indirectBuffer:(id)indirectBuffer
	 indirectBufferOffset:(NSUInteger)indirectOffset
{
	if (![indirectBuffer isKindOfClass:[HakiosMetalBuffer class]]) {
		rw_print("an indirect draw's arguments are in a %s and not one of our buffers, "
		         "and this draw is dropped\n",
		         indirectBuffer ? class_getName(object_getClass(indirectBuffer)) : "nil");
		return;
	}
	_indirect = indirectBuffer;
	_indirectOffset = indirectOffset;
	[self recordDraw:primitive vertexStart:0 count:1 indexBuffer:buffer indexType:type
	     indexOffset:offset baseVertex:0 instanceCount:1 baseInstance:0];
	_indirect = nil;
}

/* Raster state the contract does not carry yet. Collected here with one line each so that
 * the next person can see at a glance what the draw ignores. */
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count
{
	if (count > 1 && !_complained) {
		rw_print("%lu viewports are not implemented; the first one is "
		         "taken\n", (unsigned long)count);
		_complained = YES;
	}
	if (count >= 1 && viewports != NULL) {
		[self setViewport:viewports[0]];
	}
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count
{
	if (count >= 1 && rects != NULL) {
		[self setScissorRect:rects[0]];   /* (one viewport here, so one scissor) */
	}
}
/* MTLDepthClipModeClip is what the library's rasterizer state does (depth_clip_near and
 * _far are both on); MTLDepthClipModeClamp, which keeps a primitive outside the depth range
 * and clamps its z instead, is a different picture and the contract has nowhere to ask for
 * it. Said once rather than ignored. */
- (void)setDepthClipMode:(NSUInteger)mode
{
	_depthClamp = mode == MTLDepthClipModeClamp;
}
- (void)setDepthBias:(float)bias slopeScale:(float)scale clamp:(float)clamp
{
	_depthBias[0] = bias;
	_depthBias[1] = scale;
	_depthBias[2] = clamp;
}
/* The stencil reference, which Metal keeps on the encoder and not in the state object, so
 * it is recorded with the draw like the clear colour. */
- (void)setStencilReferenceValue:(uint32_t)value
{
	_stencilReference[0] = value;
	_stencilReference[1] = value;
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	_stencilReference[0] = front;
	_stencilReference[1] = back;
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset
{
	(void)mode; (void)offset;
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
	(void)action; (void)index;
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
- (void)textureBarrier { }
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
 * Tessellation, in the forms the library cannot draw: it has no tessellator, so a patch is a
 * grid its post-tessellation vertex function is run over (rwa_render_patch_vertices), and a
 * patch draw it cannot make that way records nothing. Said out loud because a draw that
 * silently does nothing is the one failure this driver has already paid for twice
 * (docs/metal.md 10.21, 10.26). */
- (void)hakiosNoTessellation
{
	RW_ONCE("this form of tessellation is not implemented; this patch draw records "
		"nothing\n");
}

/* A patch draw, as the library draws one: each patch an instance of a grid of triangles, the
 * first patch the base instance, so [[patch_id]] counts from patchStart. A patch index buffer
 * (which patch an instance is) and more than one instance of the patches are not drawn: both
 * would need the patch count in the shader to tell [[patch_id]] from [[instance_id]]. */
- (void)drawPatches:(NSUInteger)controlPoints
	 patchStart:(NSUInteger)start
	 patchCount:(NSUInteger)count
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
      instanceCount:(NSUInteger)instances
       baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)offset; (void)baseInstance;
	unsigned vertices = 0;

	if (mesa.render_patch_vertices != NULL && _pipeline != nil) {
		vertices = MESA(render_patch_vertices, [_pipeline render]);
	}
	if (vertices == 0) {
		RW_ONCE("a patch draw with a pipeline whose vertex function is not a "
			"post-tessellation one (or a library that draws no patches) records "
			"nothing\n");
		return;
	}
	if (indexBuffer != nil || instances > 1) {
		RW_ONCE("a patch draw with %s records nothing: the patches are drawn as "
			"instances, and that leaves no instance to tell apart\n",
			indexBuffer != nil ? "a patch index buffer" : "more than one instance");
		return;
	}
	if (count == 0) {
		return;
	}
	[self recordDraw:MTLPrimitiveTypeTriangle vertexStart:0 count:vertices indexBuffer:nil
	       indexType:0 indexOffset:0 baseVertex:0 instanceCount:count baseInstance:start];
}
- (void)drawPatches:(NSUInteger)controlPoints
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
     indirectBuffer:(id)indirectBuffer
indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)indirectBuffer;
	(void)indirectOffset;
	[self hakiosNoTessellation];
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
	/* The control-point index buffer says where a patch's control points are for a function
	 * that takes them through [[stage_in]], which the library refuses when it translates one;
	 * a function this library draws reads its control points itself (OpenSubdiv's, by
	 * [[patch_id]]), so for it this is -drawPatches:. */
	(void)controlPointBuffer; (void)controlPointOffset;
	[self drawPatches:controlPoints patchStart:start patchCount:count patchIndexBuffer:indexBuffer
	   patchIndexBufferOffset:offset instanceCount:instances baseInstance:baseInstance];
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
	[self hakiosNoTessellation];
}
/* The factors a tessellation kernel wrote, and their scale: taken and not read, every patch
 * being drawn at the library's one level (RWA_TESS_LEVEL). Said once, because an application
 * that varies them per patch gets the same subdivision everywhere. */
- (void)setTessellationFactorBuffer:(id)buffer
			     offset:(NSUInteger)offset
		     instanceStride:(NSUInteger)stride
{
	(void)buffer; (void)offset; (void)stride;
	RW_ONCE("tessellation factors are not read: every patch is drawn at one level\n");
}
- (void)setTessellationFactorScale:(float)scale
{
	(void)scale;
}

/* Indirect command buffers: a buffer of commands the GPU reads for itself. The device
 * answers no to every indirect-command-buffer query, so this is a path a correct caller
 * does not take; the draws in it would be lost silently if it did. */
- (void)hakiosNoIndirectCommands
{
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	[self hakiosNoIndirectCommands];
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	[self hakiosNoIndirectCommands];
}

/* Tile shading and imageblocks: a whole second pipeline kind (a tile function that runs over
 * the pass's imageblock), which this driver does not have. Every binding of it is refused
 * together, because one of them answering and the rest not would be worse than all of them
 * refusing: a tile dispatch would then run with half its bindings. */
- (void)hakiosNoTileShading
{
	RW_ONCE("tile shading and imageblocks are not implemented; this tile "
		"binding or dispatch is dropped\n");
}
- (void)dispatchThreadsPerTile:(MTLSize)size
{
	(void)size;
	[self hakiosNoTileShading];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length
			    offset:(NSUInteger)offset
			   atIndex:(NSUInteger)index
{
	(void)length; (void)offset; (void)index;
	[self hakiosNoTileShading];
}
- (void)setTileBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self hakiosNoTileShading];
}
- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self hakiosNoTileShading];
}
- (void)setTileBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self hakiosNoTileShading];
}
- (void)setTileBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self hakiosNoTileShading];
}
- (void)setTileTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self hakiosNoTileShading];
}
- (void)setTileTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self hakiosNoTileShading];
}
- (void)setTileSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self hakiosNoTileShading];
}
- (void)setTileSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self hakiosNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self hakiosNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self hakiosNoTileShading];
}

/* Function tables: pointers to shader functions, which an AIR module of ours never names
 * and the device answers no to (no ray tracing, no function pointers). A table bound here
 * would be a table a shader cannot call. */
- (void)hakiosNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
/* a visible function table is a buffer of handles (HakiosMetalVisibleFunctionTable), bound at its
 * index as a buffer is; anything else is dropped as before */
- (void)setVertexVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	if ([table respondsToSelector:@selector(hakiosBuffer)]) {
		[self setVertexBuffer:[table hakiosBuffer] offset:0 atIndex:index];
		return;
	}
	[self hakiosNoFunctionTables];
}
- (void)setVertexVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexVisibleFunctionTable:tables[i] atBufferIndex:range.location + i];
	}
}
- (void)setFragmentVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	if ([table respondsToSelector:@selector(hakiosBuffer)]) {
		[self setFragmentBuffer:[table hakiosBuffer] offset:0 atIndex:index];
		return;
	}
	[self hakiosNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentVisibleFunctionTable:tables[i] atBufferIndex:range.location + i];
	}
}
- (void)setTileVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoFunctionTables];
}
- (void)setTileVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTables:(const id *)tables
			      withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoFunctionTables];
}
- (void)setTileIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoFunctionTables];
}
- (void)setTileIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoFunctionTables];
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
 * called names the RECEIVER, so the log would read "-[HakiosMetalRenderEncoder
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
- (void)hakiosNoMeshStages
{
	RW_ONCE("mesh and object shader stages are not implemented (the device "
		"answers no to them); this binding is dropped\n");
}
- (void)setObjectBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setObjectBytes:(const void *)bytes
		length:(NSUInteger)length
	       atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)length; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setObjectIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setMeshBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setMeshBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setMeshVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoMeshStages];
}
- (void)setMeshIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoMeshStages];
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
	[self hakiosNoMeshStages];
}
- (void)setObjectIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setMeshAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self hakiosNoMeshStages];
}
- (void)setObjectAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self hakiosNoMeshStages];
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
 */
- (void)endEncoding
{
	/* a pass that drew nothing still clears what it said it clears */
	if (!_drew && (_clearMask != 0 || _clearsDepth || _clearsStencil)) {
		HakiosMetalWork *work = [[HakiosMetalWork alloc] init];

		work->clearOnly = YES;
		work->colorCount = _colorCount;
		for (unsigned i = 0; i < _colorCount; i++) {
			work->color[i] = _color[i];
			work->colorLevel[i] = _colorLevel[i];
			work->colorSlice[i] = _colorSlice[i];
		}
		work->clearMask = _clearMask;
		memcpy(work->clearColor, _clearColor, sizeof(work->clearColor));
		work->depth = _depth;
		work->clearDepth = _clearDepth;
		work->clearStencil = _clearStencil;
		work->clearsDepth = _clearsDepth;
		work->clearsStencil = _clearsStencil;
		[_commandBuffer recordWork:work];
		[work release];
		_clearMask = 0;
		_clearsDepth = _clearsStencil = NO;
	}
	for (unsigned i = 0; i < _colorCount && i < 8; i++) {
		id from = _color[i], to = _resolveTo[i];

		if (from == nil || to == nil) {
			[to release];
			_resolveTo[i] = nil;
			continue;
		}
		HakiosMetalWork *w = [[HakiosMetalWork alloc] init];
		unsigned fw = (unsigned)MAX([from width] >> _colorLevel[i], 1ul);
		unsigned fh = (unsigned)MAX([from height] >> _colorLevel[i], 1ul);
		unsigned tw = (unsigned)MAX([to width] >> _resolveLevel[i], 1ul);
		unsigned th = (unsigned)MAX([to height] >> _resolveLevel[i], 1ul);

		w->blit = RW_BLIT_TEXTURE;
		w->blitFrom = from;
		w->blitTo = to;
		w->fromSlice = _colorSlice[i];
		w->fromLevel = _colorLevel[i];
		w->blitSize[0] = MIN(fw, tw);
		w->blitSize[1] = MIN(fh, th);
		w->blitSize[2] = 1;
		w->toSlice = _resolveSlice[i];
		w->toLevel = _resolveLevel[i];
		[_commandBuffer recordWork:w];
		[w release];
		[to release];
		_resolveTo[i] = nil;
	}
	[super endEncoding];
}

@end

/*
 * MTLBlitCommandEncoder. The window server copies with it -- the frame behind a frosted-glass layer into a texture of
 * its own, to downsample and blur -- and for as long as the command buffer answered nil every one of those copies was
 * a message to nil: the materials were dark bands with debris in them. Each copy is recorded and run at the commit in
 * its place (rw_run_blit, the compiler library's rwa_texture_copy and the rest). What MTLBlitCommandEncoder has that is
 * not here is answered by +resolveInstanceMethod: below, said once by name, rather than aborting the caller.
 */
@implementation HakiosMetalBlitEncoder {
	HakiosMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
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

- (id)device { return [_commandBuffer device]; }

- (void)record:(HakiosMetalWork *)work
{
	[_commandBuffer recordWork:work];
	[work release];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
	   sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toTexture:(id)destination
       destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
      destinationOrigin:(MTLOrigin)destinationOrigin
{
	HakiosMetalWork *w = [[HakiosMetalWork alloc] init];

	w->blit = RW_BLIT_TEXTURE;
	w->blitFrom = source;
	w->blitTo = destination;
	w->fromSlice = (unsigned)sourceSlice;
	w->fromLevel = (unsigned)sourceLevel;
	w->fromOrigin[0] = (unsigned)sourceOrigin.x;
	w->fromOrigin[1] = (unsigned)sourceOrigin.y;
	w->fromOrigin[2] = (unsigned)sourceOrigin.z;
	w->blitSize[0] = (unsigned)sourceSize.width;
	w->blitSize[1] = (unsigned)sourceSize.height;
	w->blitSize[2] = (unsigned)sourceSize.depth;
	w->toSlice = (unsigned)destinationSlice;
	w->toLevel = (unsigned)destinationLevel;
	w->toOrigin[0] = (unsigned)destinationOrigin.x;
	w->toOrigin[1] = (unsigned)destinationOrigin.y;
	w->toOrigin[2] = (unsigned)destinationOrigin.z;
	[self record:w];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
	   sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toTexture:(id)destination
       destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
      destinationOrigin:(MTLOrigin)destinationOrigin options:(NSUInteger)options
{
	(void)options;
	[self copyFromTexture:source sourceSlice:sourceSlice sourceLevel:sourceLevel sourceOrigin:sourceOrigin
		   sourceSize:sourceSize toTexture:destination destinationSlice:destinationSlice
	     destinationLevel:destinationLevel destinationOrigin:destinationOrigin];
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
	NSUInteger slices = MIN(MAX([source arrayLength], 1u), MAX([destination arrayLength], 1u));

	[self copyFromTexture:source sourceSlice:0 sourceLevel:0 toTexture:destination destinationSlice:0
	     destinationLevel:0 sliceCount:slices levelCount:MAX(levels, 1u)];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset toBuffer:(id)destination
     destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size
{
	HakiosMetalWork *w = [[HakiosMetalWork alloc] init];

	w->blit = RW_BLIT_BUFFER;
	w->blitFrom = source;
	w->blitTo = destination;
	w->fromOffset = sourceOffset;
	w->toOffset = destinationOffset;
	w->blitBytes = size;
	[self record:w];
}

- (void)fillBuffer:(id)buffer range:(NSRange)range value:(uint8_t)value
{
	HakiosMetalWork *w = [[HakiosMetalWork alloc] init];

	w->blit = RW_BLIT_FILL;
	w->blitTo = buffer;
	w->toOffset = range.location;
	w->blitBytes = range.length;
	w->fillValue = value;
	[self record:w];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)bytesPerRow
    sourceBytesPerImage:(NSUInteger)bytesPerImage sourceSize:(MTLSize)size toTexture:(id)destination
       destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
{
	HakiosMetalWork *w = [[HakiosMetalWork alloc] init];

	w->blit = RW_BLIT_TO_TEXTURE;
	w->blitFrom = source;
	w->blitTo = destination;
	w->fromOffset = sourceOffset;
	w->bytesPerRow = bytesPerRow;
	w->bytesPerImage = bytesPerImage;
	w->blitSize[0] = (unsigned)size.width;
	w->blitSize[1] = (unsigned)size.height;
	w->blitSize[2] = (unsigned)size.depth;
	w->toSlice = (unsigned)slice;
	w->toLevel = (unsigned)level;
	w->toOrigin[0] = (unsigned)origin.x;
	w->toOrigin[1] = (unsigned)origin.y;
	w->toOrigin[2] = (unsigned)origin.z;
	[self record:w];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)bytesPerRow
    sourceBytesPerImage:(NSUInteger)bytesPerImage sourceSize:(MTLSize)size toTexture:(id)destination
       destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
		options:(NSUInteger)options
{
	(void)options;
	[self copyFromBuffer:source sourceOffset:sourceOffset sourceBytesPerRow:bytesPerRow sourceBytesPerImage:bytesPerImage
		  sourceSize:size toTexture:destination destinationSlice:slice destinationLevel:level
	   destinationOrigin:origin];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
	   sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id)destination
      destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)bytesPerRow
destinationBytesPerImage:(NSUInteger)bytesPerImage
{
	HakiosMetalWork *w = [[HakiosMetalWork alloc] init];

	w->blit = RW_BLIT_TO_BUFFER;
	w->blitFrom = source;
	w->blitTo = destination;
	w->fromSlice = (unsigned)slice;
	w->fromLevel = (unsigned)level;
	w->fromOrigin[0] = (unsigned)origin.x;
	w->fromOrigin[1] = (unsigned)origin.y;
	w->fromOrigin[2] = (unsigned)origin.z;
	w->blitSize[0] = (unsigned)size.width;
	w->blitSize[1] = (unsigned)size.height;
	w->blitSize[2] = (unsigned)size.depth;
	w->toOffset = offset;
	w->bytesPerRow = bytesPerRow;
	w->bytesPerImage = bytesPerImage;
	[self record:w];
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

- (void)generateMipmapsForTexture:(id)texture
{
	HakiosMetalWork *w = [[HakiosMetalWork alloc] init];

	w->blit = RW_BLIT_MIPMAPS;
	w->blitTo = texture;
	[self record:w];
}

/* one ring, work in order, memory shared with the processor: these have nothing to do */
- (void)synchronizeResource:(id)resource { (void)resource; }
- (void)synchronizeTexture:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}
- (void)optimizeContentsForGPUAccess:(id)texture { (void)texture; }
- (void)optimizeContentsForCPUAccess:(id)texture { (void)texture; }
- (void)updateFence:(id)fence { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }

/* anything else MTLBlitCommandEncoder has: answered with nothing, and said once by name */
static id
rw_blit_unknown(id self, SEL sel, ...)
{
	(void)self;
	rw_print("blit encoder: -%s is not done here; it is skipped\n", sel_getName(sel));
	return nil;
}

+ (BOOL)resolveInstanceMethod:(SEL)sel
{
	const char *name = sel_getName(sel);

	if (strstr(name, "copy") != NULL || strstr(name, "fill") != NULL || strstr(name, "Mipmap") != NULL ||
	    strstr(name, "optimize") != NULL || strstr(name, "reset") != NULL || strstr(name, "Counter") != NULL) {
		class_addMethod(self, sel, (IMP)rw_blit_unknown, "@@:");
		return YES;
	}
	return [super resolveInstanceMethod:sel];
}

- (void)endEncoding
{
	[super endEncoding];
}

@end

@implementation HakiosMetalComputeEncoder {
	HakiosMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
	HakiosMetalComputePipeline *_pipeline;
	id _buffers[RW_MAX_BUFFERS];
	NSUInteger _offsets[RW_MAX_BUFFERS];
	id _textures[RW_MAX_BUFFERS];
	id _samplers[RW_MAX_BUFFERS];
	unsigned _highest, _highestTexture, _highestSampler;
	unsigned _threadgroupBytes[32];
	BOOL _complained;
	id _indirect;                   /* the next dispatch's grid is here (an indirect dispatch) */
	NSUInteger _indirectOffset;
}

- (instancetype)initWithHakiosCommandBuffer:(HakiosMetalCommandBuffer *)commandBuffer
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
	_pipeline = state;
}

- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* The offset is RECORDED and judged at the dispatch, as the render encoder's is.
	 * An argument buffer is bound through this call with an offset of its own -- which
	 * encoding in that buffer to read -- and a plain buffer argument with one is an
	 * application packing several blocks into an allocation, which is served: the
	 * library takes a `constant` binding at any offset (uploading the block when the
	 * GPU cannot fetch from that offset) and refuses a `device` one the GPU cannot
	 * fetch from, by name, where the shader's own address space is in hand. */
	_buffers[index] = buffer;
	_offsets[index] = offset;
	if (index + 1 > _highest) {
		_highest = (unsigned)index + 1;
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("texture index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	_textures[index] = texture;
	if (index + 1 > _highestTexture) {
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
	if (index >= RW_MAX_BUFFERS) {
		rw_print("sampler index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	_samplers[index] = sampler;
	if (index + 1 > _highestSampler) {
		_highestSampler = (unsigned)index + 1;
	}
}

/* Metal lets a binding narrow the sampler's lod range. The compiler library takes the range
 * when the sampler is made, so honouring it here would need a second sampler object; until
 * it does, saying nothing would silently sample the wrong level. */
- (void)setSamplerState:(id)sampler
	    lodMinClamp:(float)lodMin
	    lodMaxClamp:(float)lodMax
		atIndex:(NSUInteger)index
{
	if (!_complained && (lodMin != 0.0f || lodMax < 1000.0f)) {
		rw_print("a per-binding lod clamp (%g..%g) is not implemented; the "
		         "sampler's own range is used instead\n", (double)lodMin, (double)lodMax);
		_complained = YES;
	}
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

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)size
{
	[self hakiosDispatch:groups size:size threads:(MTLSize){ 0, 0, 0 }];
}

/* Both dispatches: `threads` zero for whole groups, or the count -dispatchThreads: asked for,
 * which the library runs with the last groups partial (rwa_grid_threads). */
- (void)hakiosDispatch:(MTLSize)groups size:(MTLSize)size threads:(MTLSize)threads
{
	HakiosMetalWork *work = [[HakiosMetalWork alloc] init];

	if (_pipeline == nil || _complained) {
		rw_print("this dispatch has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "a binding this driver refused");
		[work release];
		return;
	}
	work->pipeline = _pipeline;
	work->count = _highest;
	work->textureCount = _highestTexture;
	work->samplerCount = _highestSampler;
	for (unsigned i = 0; i < _highest; i++) {
		work->buffers[i] = _buffers[i];
		work->bufferOffsets[i] = (size_t)_offsets[i];
	}
	for (unsigned i = 0; i < _highestTexture; i++) {
		work->textures[i] = _textures[i];
	}
	for (unsigned i = 0; i < _highestSampler; i++) {
		work->samplers[i] = _samplers[i];
	}
	memcpy(work->threadgroupBytes, _threadgroupBytes, sizeof(_threadgroupBytes));
	work->groups[0] = (unsigned)groups.width;
	work->groups[1] = (unsigned)groups.height;
	work->groups[2] = (unsigned)groups.depth;
	work->size[0] = (unsigned)size.width;
	work->size[1] = (unsigned)size.height;
	work->size[2] = (unsigned)size.depth;
	work->threads[0] = (unsigned)threads.width;
	work->threads[1] = (unsigned)threads.height;
	work->threads[2] = (unsigned)threads.depth;
	work->indirectBuffer = _indirect;
	work->indirectOffset = (size_t)_indirectOffset;
	[_commandBuffer recordWork:work];
	[work release];
}

/* Metal counts threads here and threadgroups above. A count the groups do not divide runs
 * with the last groups partial -- Metal's non-uniform threadgroups, which the library has as
 * a variant of the kernel guarded on the count (rwa_grid_threads); a library without it
 * cannot, and then the dispatch is refused rather than rounded, because rounding runs threads
 * the application did not ask for. */
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)size
{
	MTLSize groups;

	if (size.width == 0 || size.height == 0 || size.depth == 0) {
		rw_print("a dispatch of threads in groups of %lux%lux%lu has no threads in a "
		         "group\n", (unsigned long)size.width, (unsigned long)size.height,
		         (unsigned long)size.depth);
		return;
	}
	groups.width = (threads.width + size.width - 1) / size.width;
	groups.height = (threads.height + size.height - 1) / size.height;
	groups.depth = (threads.depth + size.depth - 1) / size.depth;
	BOOL whole = threads.width % size.width == 0 && threads.height % size.height == 0 &&
		     threads.depth % size.depth == 0;

	if (!whole && mesa.grid_threads == NULL) {
		rw_print("%lux%lux%lu threads do not divide into groups of "
		         "%lux%lux%lu, and this compiler library has no partial groups\n",
		         (unsigned long)threads.width, (unsigned long)threads.height,
		         (unsigned long)threads.depth, (unsigned long)size.width,
		         (unsigned long)size.height, (unsigned long)size.depth);
		return;
	}
	[self hakiosDispatch:groups size:size threads:whole ? (MTLSize){ 0, 0, 0 } : threads];
}

- (void)useResource:(id)resource usage:(NSUInteger)usage { (void)resource; (void)usage; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)memoryBarrierWithScope:(NSUInteger)scope { (void)scope; }
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	/* any index: buffer n starts where 0..n-1 end, so the library compiles a kernel with
	 * more than one from all their lengths (rwa_threadgroup_memory_at) */
	if (index < 32) {
		_threadgroupBytes[index] = (unsigned)length;
	} else {
		rw_print("a threadgroup buffer at index %lu; Metal has 31\n", (unsigned long)index);
	}
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
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* Metal's rule is that this moves a buffer that is already bound at that index, so
	 * with nothing bound there is nothing to move -- and binding nil here would add an
	 * empty slot to the dispatch's count and make it complain about a buffer nobody
	 * asked for. */
	if (_buffers[index] != nil) {
		[self setBuffer:_buffers[index] offset:offset atIndex:index];
	}
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

/* An indirect dispatch: its grid is read out of the buffer when the dispatch runs, the work
 * before it having been waited for (rw_resolve_indirect_dispatch). Declined below, each saying its
 * name once: the indirect command buffers, which are commands and not a grid. */
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer
			  indirectBufferOffset:(NSUInteger)offset
			 threadsPerThreadgroup:(MTLSize)size
{
	if (![buffer isKindOfClass:[HakiosMetalBuffer class]]) {
		rw_print("an indirect dispatch's grid is in a %s and not one of our buffers, and "
		         "this dispatch is dropped\n",
		         buffer ? class_getName(object_getClass(buffer)) : "nil");
		return;
	}
	_indirect = buffer;
	_indirectOffset = offset;
	[self hakiosDispatch:(MTLSize){ 1, 1, 1 } size:size threads:(MTLSize){ 0, 0, 0 }];
	_indirect = nil;
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
- (void)hakiosNoStageIn
{
	RW_ONCE("a kernel stage-in region is not implemented; the dispatch "
		"covers the whole grid it was given\n");
}
- (void)setStageInRegion:(MTLRegion)region
{
	(void)region;
	[self hakiosNoStageIn];
}
- (void)setStageInRegionWithIndirectBuffer:(id)buffer
		      indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	[self hakiosNoStageIn];
}
/* A kernel's explicit imageblock is a pixel per thread of the group's x by y (the library's,
 * nir_air.h): a block of another size than the group is not kept, and is said once. */
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
	(void)width; (void)height;
	RW_ONCE("an imageblock of %lux%lu was asked for; a kernel's imageblock here is the size "
		"of its threadgroup, whatever this says\n", (unsigned long)width,
		(unsigned long)height);
}
/* An entry for this exists in _MTLCommandEncoder and it is one of Apple's abstract ones
 * (tools/metal/protometh.py --verdicts), so inheriting it is an abort rather than a
 * default. It is a hint about how the hardware packs threadgroups together; the compiler
 * library decides that for itself and there is nothing here to turn off. */
- (void)setThreadgroupPackingDisabled:(BOOL)disabled { (void)disabled; }

- (void)hakiosNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
/* a visible function table is a buffer of handles (HakiosMetalVisibleFunctionTable), bound at its
 * index as a buffer is (CoreImage's ubershader); anything else is dropped as before */
- (void)setVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	if ([table respondsToSelector:@selector(hakiosBuffer)]) {
		[self setBuffer:[table hakiosBuffer] offset:0 atIndex:index];
		return;
	}
	[self hakiosNoFunctionTables];
}
- (void)setVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVisibleFunctionTable:tables[i] atBufferIndex:range.location + i];
	}
}
- (void)setIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hakiosNoFunctionTables];
}
- (void)setIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hakiosNoFunctionTables];
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
@implementation HakiosMetalArgumentEncoder {
	HakiosMetalDevice *_owner;         /* not retained: the device owns everything here */
	NSString *_label;
	struct rwa_argbuf_layout _layout;
	BOOL _fromModule;                   /* the layout came from a shader, not descriptors */
	HakiosMetalBuffer *_destination;   /* what -setArgumentBuffer:offset: named */
	NSUInteger _offset;
	BOOL _complained;
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

- (instancetype)initWithHakiosDevice:(HakiosMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	_layout = *layout;
	_fromModule = fromModule;
	return self;
}

- (void)dealloc
{
	[_label release];
	[_destination release];
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
- (NSUInteger)encodedLength { return _layout.encoded_length; }
- (NSUInteger)alignment     { return _layout.alignment; }

/* Not part of MTLArgumentEncoder: what this encoder is for, so a test can read back the
 * mapping between Metal's [[id(n)]] and the index the GPU is programmed with instead of
 * trusting it. Metal's own pipeline reflection would report the same thing. */
- (NSUInteger)hakiosMemberCount { return _layout.member_count; }
- (NSUInteger)hakiosMetalIndex  { return _layout.metal_index; }
- (BOOL)hakiosMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index
{
	if (i >= _layout.member_count) {
		return NO;
	}
	if (mid != NULL) {
		*mid = _layout.members[i].id;
	}
	if (kind != NULL) {
		*kind = _layout.members[i].kind;
	}
	if (index != NULL) {
		*index = _layout.members[i].index;
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
	if (buffer != nil && ![buffer isKindOfClass:[HakiosMetalBuffer class]]) {
		rw_print("the argument buffer is a %s and not one of ours, so "
		         "nothing can be encoded into it\n",
		         class_getName(object_getClass(buffer)));
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && [buffer mesaBuffer] == NULL) {
		rw_print("the argument buffer did not come from the compiler "
		         "library, so nothing can be encoded into it\n");
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && _layout.alignment != 0 && offset % _layout.alignment != 0) {
		rw_print("an argument buffer offset of %lu is not a multiple of "
		         "this encoder's alignment (%u), so the draw would look for the "
		         "encoding somewhere else\n", (unsigned long)offset, _layout.alignment);
		[_destination release];
		_destination = nil;
		return;
	}
	/* Retained, unlike the other back-pointers in this file, and for a measured
	 * reason: the records live in the compiler library WITH that buffer, so an
	 * application that released it while this encoder still pointed at it would have
	 * rwa_buffer_destroy free the records (and scrub them out of everyone else's) and
	 * leave this object holding a freed MTLBuffer to send -mesaBuffer to. Metal's own
	 * encoder outliving its argument buffer is an application's mistake; a crash in
	 * the driver is ours. */
	[buffer retain];
	[_destination release];
	_destination = buffer;
	_offset = offset;
}

/* Metal's array form: the same destination with an element's worth of offset added. The
 * arithmetic is Metal's own -- encodedLength per element -- and it is here rather than
 * declined because an application that encodes an ARRAY of sets is doing nothing this
 * driver cannot record. */
- (void)setArgumentBuffer:(id)buffer startOffset:(NSUInteger)offset
	     arrayElement:(NSUInteger)element
{
	[self setArgumentBuffer:buffer offset:offset + element * _layout.encoded_length];
}

/* The member an id names: a set's ids are not 0..n-1 once it has a struct in it (Metal
 * numbers a struct's members from the struct's own id), so it is looked up. NULL for an id
 * the set does not declare. */
- (const struct rwa_argbuf_member *)hakiosMemberWithId:(NSUInteger)index
{
	for (unsigned i = 0; i < _layout.member_count; i++) {
		if (_layout.members[i].id == index) {
			return &_layout.members[i];
		}
	}
	return NULL;
}

/* Whether this encoder is in a state to record, said once per site: an id the set does
 * not declare is the mistake worth naming, because Metal would write a handle into the
 * buffer and the shader would read a descriptor that is not there. A descriptor set at a
 * VALUE's id is the other one: the shader reads those bytes as a number. */
- (BOOL)hakiosReady:(NSUInteger)index for:(const char *)what
{
	if (_destination == nil) {
		if (!_complained) {
			rw_print("%s was encoded at id %lu with no argument buffer "
			         "set (Metal's -setArgumentBuffer:offset: has to name one "
			         "first)\n", what, (unsigned long)index);
			_complained = YES;
		}
		return NO;
	}
	const struct rwa_argbuf_member *m = [self hakiosMemberWithId:index];

	if (m == NULL) {
		/* Which of the two layouts this is matters to whoever reads the line: a
		 * shader's set is the shader's business and an application cannot change
		 * it, while a set described with MTLArgumentDescriptors is the
		 * application's own and the id it just used is one it did not describe. */
		rw_print("%s was encoded at id %lu of %s that declares no such id (%u "
		         "member%s)\n", what, (unsigned long)index,
		         _fromModule ? "a set a shader declares" : "an encoder this "
		                       "application described",
		         _layout.member_count, _layout.member_count == 1 ? "" : "s");
		return NO;
	}
	BOOL value = strcmp(what, "constant data") == 0;

	if ((m->kind == RWA_ARGBUF_VALUE) != value) {
		rw_print("%s was encoded at id %lu, which the set declares as %s\n", what,
		         (unsigned long)index,
		         m->kind == RWA_ARGBUF_VALUE ? "a value (-constantDataAtIndex:)"
		                                     : "a descriptor");
		return NO;
	}
	if (_offset + _layout.encoded_length > [_destination length]) {
		rw_print("%s was encoded at offset %lu of an argument buffer of %lu "
		         "bytes, and the encoding is %u\n", what, (unsigned long)_offset,
		         (unsigned long)[_destination length], _layout.encoded_length);
		return NO;
	}
	return YES;
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
	char why[512] = { 0 };

	if (![self hakiosReady:index for:"a buffer"]) {
		return;
	}
	if (buffer != nil && ![buffer isKindOfClass:[HakiosMetalBuffer class]]) {
		rw_print("the buffer encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(buffer)));
		return;
	}
	if (MESA(argbuf_encode_buffer, [_destination mesaBuffer], _offset, (unsigned)index,
				      [buffer mesaBuffer], offset, why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
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
	char why[512] = { 0 };

	if (![self hakiosReady:index for:"a texture"]) {
		return;
	}
	if (texture != nil && ![texture isKindOfClass:[HakiosMetalTexture class]]) {
		rw_print("the texture encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(texture)));
		return;
	}
	if (MESA(argbuf_encode_texture, [_destination mesaBuffer], _offset, (unsigned)index,
				       [texture mesaTexture], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
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
	char why[512] = { 0 };

	if (![self hakiosReady:index for:"a sampler"]) {
		return;
	}
	if (sampler != nil && ![sampler isKindOfClass:[HakiosMetalSampler class]]) {
		rw_print("the sampler encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(sampler)));
		return;
	}
	if (MESA(argbuf_encode_sampler, [_destination mesaBuffer], _offset, (unsigned)index,
				       [sampler mesaSampler], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
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
 * bytes of the argument buffer at that member's place, which is what Metal answers. The
 * place is the set's own layout (the struct's, from the module), and the shader reads the
 * value there: a set with values in it is bound as a buffer too, at its own index.
 */
- (void *)constantDataAtIndex:(NSUInteger)index
{
	if (![self hakiosReady:index for:"constant data"]) {
		return NULL;
	}
	void *bytes = [_destination contents];

	if (bytes == NULL) {
		return NULL;
	}
	return (char *)bytes + _offset + [self hakiosMemberWithId:index]->offset;
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
- (void)hakiosNoSuchMember:(const char *)what atIndex:(NSUInteger)index
{
	rw_print("%s was encoded at id %lu of an argument buffer, and this port "
	         "binds a buffer, a texture or a sampler member and refuses the rest when the "
	         "shader is translated; this encoding is dropped\n", what, (unsigned long)index);
}

- (void)setRenderPipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self hakiosNoSuchMember:"a render pipeline state" atIndex:index];
}
- (void)setRenderPipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self hakiosNoSuchMember:"render pipeline states" atIndex:range.location];
}
- (void)setComputePipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self hakiosNoSuchMember:"a compute pipeline state" atIndex:index];
}
- (void)setComputePipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self hakiosNoSuchMember:"compute pipeline states" atIndex:range.location];
}
- (void)setIndirectCommandBuffer:(id)buffer atIndex:(NSUInteger)index
{
	(void)buffer;
	[self hakiosNoSuchMember:"an indirect command buffer" atIndex:index];
}
- (void)setIndirectCommandBuffers:(const id *)buffers withRange:(NSRange)range
{
	(void)buffers;
	[self hakiosNoSuchMember:"indirect command buffers" atIndex:range.location];
}
- (void)setAccelerationStructure:(id)structure atIndex:(NSUInteger)index
{
	(void)structure;
	[self hakiosNoSuchMember:"an acceleration structure" atIndex:index];
}
- (void)setVisibleFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self hakiosNoSuchMember:"a visible function table" atIndex:index];
}
- (void)setVisibleFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self hakiosNoSuchMember:"visible function tables" atIndex:range.location];
}
- (void)setIntersectionFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self hakiosNoSuchMember:"an intersection function table" atIndex:index];
}
- (void)setIntersectionFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self hakiosNoSuchMember:"intersection function tables" atIndex:range.location];
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
	if (![device isKindOfClass:[HakiosMetalDevice class]]) {
		rw_print("-newArgumentEncoderWithBufferIndex: on a function whose device is "
		         "a %s and not ours\n", class_getName(object_getClass(device)));
		return nil;
	}
	return [(HakiosMetalDevice *)device hakiosArgumentEncoderForFunction:self
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
static void ensure_specialization(void);

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
			    (IMP)rw_function_argument_encoder_reflection, "@@:Q^@");	ensure_specialization();
}

/*
 * FUNCTION CONSTANTS, which no pipeline of this driver has had until now.
 *
 * Metal specialises a function in -[_MTLFunctionInternal newSpecializedFunctionWithDescriptor:
 * destinationArchive:functionCache:...]: it hashes the constants and sends them to
 * -[device compiler] (specializedFunctionHash:requestData:airScript:constants:...), which
 * on this port is nil. So every -newFunctionWithName:constantValues: in the process came
 * to nothing, and the AIR this driver translated was always specialised with zeros
 * (tools/mesa/air/air_metal.c said so: "Function constants are taken as zero"). That is
 * CoreAnimation's uber shaders with every fc_has_* false and RenderBox's
 * `RB::Shader::Constant::_shader_state` -- the uint4 every one of its shaders branches on --
 * all zero.
 *
 * With boot-arg metal_fc=1 those methods are this driver's: the answer is a function of
 * the same class holding the same state as the unspecialised one, plus the constant values,
 * and when a pipeline is made from it (rw_function_air) the values go to libhakios-air with
 * a copy of the bitcode they belong to (rwa_constants_register).
 *
 * The function is a COPY because it has to be its own object: -newFunctionWithName: hands
 * out ONE cached function per name (-[_MTLLibrary newFunctionWithNameInternal:] looks it up
 * in a dictionary), and two specialisations of one function are two pipelines. The copy is
 * an instance of a subclass made at run time whose -dealloc does nothing: its ivars are the
 * original's, retained once more for it, and the original is kept alive by it, so nothing
 * it points at can be freed twice. What that costs is that a specialisation is never
 * freed -- one small object and one bitcode copy per variant, of which CoreAnimation makes
 * tens and keeps them all anyway.
 */
static char rw_key_base, rw_key_values, rw_key_air;

static bool
rw_function_constants_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		enabled = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "metal_fc=")) != NULL && (at == args || at[-1] == ' ') &&
		    at[strlen("metal_fc=")] == '1';
	}
	return enabled;
}

static void
rw_specialized_dealloc(id self, SEL cmd)
{
	(void)self; (void)cmd;   /* see above: a specialisation lives as long as the process */
}

static BOOL
rw_specialized_no(id self, SEL cmd)
{
	(void)self; (void)cmd;
	return NO;
}

static Class
rw_specialized_class(Class base)
{
	char name[256];

	snprintf(name, sizeof(name), "HakiosSpecialized_%s", class_getName(base));
	Class sub = objc_getClass(name);
	if (sub != Nil) {
		return sub;
	}
	sub = objc_allocateClassPair(base, name, 0);
	if (sub == Nil) {
		return objc_getClass(name);   /* another thread made it first */
	}
	class_addMethod(sub, sel_getUid("dealloc"), (IMP)rw_specialized_dealloc, "v@:");
	/* specialised: Metal must not ask for constants again */
	class_addMethod(sub, sel_getUid("needsFunctionConstantValues"), (IMP)rw_specialized_no,
			"B@:");
	objc_registerClassPair(sub);
	return sub;
}

/* the specialisation of `base` with `values`, retained (a -new... method's answer) */
static id
rw_specialize(id base, id values)
{
	Class cls = object_getClass(base);
	Class sub = rw_specialized_class(cls);
	size_t size = class_getInstanceSize(cls);
	id copy = class_createInstance(sub, 0);

	if (copy == nil) {
		return nil;
	}
	/* every ivar, after the isa: the copy has its own isa and its own reference count */
	memcpy((char *)copy + sizeof(Class), (char *)base + sizeof(Class), size - sizeof(Class));
	for (Class c = cls; c != Nil && c != [NSObject class]; c = class_getSuperclass(c)) {
		unsigned count = 0;
		Ivar *ivars = class_copyIvarList(c, &count);

		for (unsigned i = 0; i < count; i++) {
			const char *type = ivar_getTypeEncoding(ivars[i]);

			if (type != NULL && type[0] == '@') {
				[*(id *)((char *)copy + ivar_getOffset(ivars[i])) retain];
			}
		}
		free(ivars);
	}
	{
		static unsigned made;

		made++;
		if (made <= 4 || made % 100 == 0) {
			rw_print("specialisation %u: %s\n", made,
			         [[((id (*)(id, SEL))objc_msgSend)(base, sel_getUid("name")) description] UTF8String]);
		}
	}
	objc_setAssociatedObject(copy, &rw_key_base, base, OBJC_ASSOCIATION_RETAIN);
	objc_setAssociatedObject(copy, &rw_key_values, [[values copy] autorelease],
				 OBJC_ASSOCIATION_RETAIN);
	return copy;
}

/*
 * CoreAnimation's GENERIC uber shader: fixed_frag_lph_* with fc_generic true, every feature on and the
 * configuration read from a buffer at run time. Nothing of it folds away -- 189,000 lines of NIR and 1,344 loops --
 * and on the phone backboardd asked for it nine times before its first frame and was killed by jetsam compiling
 * it (run glass1); compiled once and shared (libhakios-air's stage cache) it still froze the lock screen and
 * starved the system (glass6). Refused, CoreAnimation presented nothing at all (glass2). CoreAnimation draws with it
 * only until a configuration's specialised pipeline is ready, and those fold to a few seconds' work each.
 *
 * Boot-arg metal_generic: 0 refuses it; 1 compiles it; 2, the default, makes its pipelines with a stand-in
 * fragment shader that draws nothing (src/rw_null.metal, rw_null_air.h), so a layer is missing for its first
 * frames and then drawn by its specialised pipeline.
 */
static int
rw_generic_mode(void)
{
	static int mode = -1;

	if (mode < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		mode = 2;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "metal_generic=")) != NULL && (at == args || at[-1] == ' ')) {
			mode = at[strlen("metal_generic=")] - '0';
			if (mode < 0 || mode > 2) {
				mode = 2;
			}
		}
	}
	return mode;
}

/* whether `values`, given to `base`, are the generic uber shader's (fc_generic true) */
static bool
rw_is_generic(id base, id values)
{
	SEL value_of = sel_getUid("constantValueWithFunctionConstant:");

	if (base == nil || values == nil || ![values respondsToSelector:value_of]) {
		return false;
	}
	NSDictionary *declared = ((id (*)(id, SEL))objc_msgSend)(base, sel_getUid("functionConstantsDictionary"));
	id fc = declared != nil ? [declared objectForKey:@"fc_generic"] : nil;
	const uint8_t *p = fc != nil ? ((const void *(*)(id, SEL, id))objc_msgSend)(values, value_of, fc) : NULL;

	return p != NULL && *p != 0;
}

static id
rw_specialized_from_descriptor(id self, id descriptor, NSError **error)
{
	id values = descriptor != nil ?
	    ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_getUid("constantValues")) : nil;

	if (error != NULL) {
		*error = nil;
	}
	if (values == nil) {
		return [self retain];
	}
	if (rw_generic_mode() == 0 && rw_is_generic(self, values)) {
		static unsigned refused;
		if (++refused <= 4 || refused % 50 == 0) {
			rw_print("specialisation refused (%u so far): %s with fc_generic, CoreAnimation's generic uber "
			         "shader, which this driver cannot compile in reasonable time or memory\n", refused,
			         [[((id (*)(id, SEL))objc_msgSend)(self, sel_getUid("name")) description] UTF8String]);
		}
		if (error != NULL) {
			*error = [NSError errorWithDomain:@"HakiosMetal" code:2 userInfo:@{
			    NSLocalizedDescriptionKey: @"the generic uber shader is not compiled on this device" }];
		}
		return nil;
	}
	id copy = rw_specialize(self, values);
	if (copy == nil && error != NULL) {
		*error = [NSError errorWithDomain:@"HakiosMetal" code:1 userInfo:@{
		    NSLocalizedDescriptionKey: @"the function could not be specialised" }];
	}
	return copy;
}

typedef id (*rw_spec4_t)(id, SEL, id, id, id, NSError **);
typedef id (*rw_spec5_t)(id, SEL, id, id, id, id, NSError **);
typedef void (*rw_spec_async_t)(id, SEL, id, id, id, BOOL, id);
typedef void (*rw_spec_async_task_t)(id, SEL, id, id, id, BOOL, id, id);
static rw_spec4_t rw_metal_spec4;
static rw_spec5_t rw_metal_spec5;
static rw_spec_async_t rw_metal_spec_async;
static rw_spec_async_task_t rw_metal_spec_async_task;

/* -newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:error: */
static id
rw_spec4(id self, SEL cmd, id descriptor, id archive, id cache, NSError **error)
{
	if (!rw_function_constants_enabled()) {
		return rw_metal_spec4(self, cmd, descriptor, archive, cache, error);
	}
	return rw_specialized_from_descriptor(self, descriptor, error);
}

/* ...functionCache:compilerTask:error: */
static id
rw_spec5(id self, SEL cmd, id descriptor, id archive, id cache, id task, NSError **error)
{
	if (!rw_function_constants_enabled()) {
		return rw_metal_spec5(self, cmd, descriptor, archive, cache, task, error);
	}
	return rw_specialized_from_descriptor(self, descriptor, error);
}

/* the asynchronous two, whose handler takes (id<MTLFunction>, NSError *) */
static void
rw_spec_async(id self, SEL cmd, id descriptor, id archive, id cache, BOOL sync, id handler)
{
	if (!rw_function_constants_enabled()) {
		rw_metal_spec_async(self, cmd, descriptor, archive, cache, sync, handler);
		return;
	}
	NSError *error = nil;
	id fn = rw_specialized_from_descriptor(self, descriptor, &error);
	if (handler != nil) {
		((void (^)(id, NSError *))handler)(fn, error);
	}
	[fn release];
}

static void
rw_spec_async_task(id self, SEL cmd, id descriptor, id archive, id cache, BOOL sync, id task,
		   id handler)
{
	if (!rw_function_constants_enabled()) {
		rw_metal_spec_async_task(self, cmd, descriptor, archive, cache, sync, task, handler);
		return;
	}
	rw_spec_async(self, cmd, descriptor, archive, cache, sync, handler);
}

static void
ensure_specialization(void)
{
	Class fn = objc_lookUpClass("_MTLFunctionInternal");
	struct { const char *sel; IMP imp; void **orig; const char *types; } hooks[] = {
		{ "newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:error:",
		  (IMP)rw_spec4, (void **)&rw_metal_spec4, "@@:@@@^@" },
		{ "newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:compilerTask:error:",
		  (IMP)rw_spec5, (void **)&rw_metal_spec5, "@@:@@@@^@" },
		{ "newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:sync:completionHandler:",
		  (IMP)rw_spec_async, (void **)&rw_metal_spec_async, "v@:@@@B@?" },
		{ "newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:sync:compilerTask:completionHandler:",
		  (IMP)rw_spec_async_task, (void **)&rw_metal_spec_async_task, "v@:@@@B@@?" },
	};

	if (fn == Nil) {
		rw_print("no _MTLFunctionInternal in this process: function constants stay zero\n");
		return;
	}
	for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		SEL sel = sel_getUid(hooks[i].sel);
		Method m = class_getInstanceMethod(fn, sel);

		if (m == NULL) {
			rw_print("-[_MTLFunctionInternal %s] is not there: that road to a "
			         "specialised function is left as Metal has it\n", hooks[i].sel);
			continue;
		}
		*hooks[i].orig = (void *)method_getImplementation(m);
		class_replaceMethod(fn, sel, hooks[i].imp, hooks[i].types);
	}
}

/* How many bytes Metal keeps for a constant of MTLDataType `type`: the scalar's size times
 * the component count, a 3-vector packed (component i at i * scalar). 0: not a type a
 * function constant can have. */
static unsigned
rw_data_type_size(NSUInteger type)
{
	struct { unsigned first, last, scalar; } runs[] = {
		{ 3, 6, 4 },      /* Float .. Float4 */
		{ 16, 19, 2 },    /* Half .. Half4 */
		{ 29, 32, 4 },    /* Int .. Int4 */
		{ 33, 36, 4 },    /* UInt .. UInt4 */
		{ 37, 40, 2 },    /* Short .. Short4 */
		{ 41, 44, 2 },    /* UShort .. UShort4 */
		{ 45, 48, 1 },    /* Char .. Char4 */
		{ 49, 52, 1 },    /* UChar .. UChar4 */
		{ 53, 56, 1 },    /* Bool .. Bool4 */
		{ 81, 84, 8 },    /* Long .. Long4 */
		{ 85, 88, 8 },    /* ULong .. ULong4 */
	};

	for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
		if (type >= runs[i].first && type <= runs[i].last) {
			return runs[i].scalar * (unsigned)(type - runs[i].first + 1);
		}
	}
	return 0;
}

/*
 * The AIR a pipeline is made from. For a function this driver specialised it is a copy of
 * the bytes -- one per specialisation, so that libhakios-air can tell two specialisations of
 * one function apart by address -- registered with the values of every constant the
 * function declares. Metal's own -functionConstantsDictionary (read from the library's
 * metadata, no compiler involved) says which constants there are, and the values object
 * answers for each one, by index or by name, whichever way the application set it.
 */
static NSData *
rw_function_air(id function)
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);
	id values = objc_getAssociatedObject(function, &rw_key_values);

	if (values == nil || air == nil || [air length] == 0) {
		return air;
	}
	NSData *own = objc_getAssociatedObject(function, &rw_key_air);
	if (own != nil) {
		return own;
	}
	if (mesa.constants_register == NULL) {
		RW_ONCE("libhakios-air.dylib has no rwa_constants_register: it is older than this "
		        "driver, and every function constant stays zero\n");
		return air;
	}
	id base = objc_getAssociatedObject(function, &rw_key_base);
	NSDictionary *declared = ((id (*)(id, SEL))objc_msgSend)(base != nil ? base : function,
							 sel_getUid("functionConstantsDictionary"));
	SEL value_of = sel_getUid("constantValueWithFunctionConstant:");

	if (declared == nil || ![values respondsToSelector:value_of]) {
		RW_ONCE("a specialised function whose constants cannot be read (%s): it is "
		        "translated with zeros, as before\n",
		        declared == nil ? "no functionConstantsDictionary" :
		        "the values do not answer constantValueWithFunctionConstant:");
		return air;
	}
	unsigned count = 0, cap = (unsigned)[declared count];
	struct rwa_constant *list = calloc(cap ? cap : 1, sizeof(*list));
	for (id name in declared) {
		id fc = [declared objectForKey:name];
		NSUInteger type = ((NSUInteger (*)(id, SEL))objc_msgSend)(fc, sel_getUid("type"));
		NSUInteger index = ((NSUInteger (*)(id, SEL))objc_msgSend)(fc, sel_getUid("index"));
		const void *p = ((const void *(*)(id, SEL, id))objc_msgSend)(values, value_of, fc);
		unsigned size = rw_data_type_size(type);

		if (p == NULL || count >= cap) {
			continue;   /* not given: undefined, which the module may ask about */
		}
		if (size == 0 || size > sizeof(list[count].bytes)) {
			RW_ONCE("function constant %s has MTLDataType %lu, which this driver has no "
			        "size for: it is left undefined\n", [[name description] UTF8String],
			        (unsigned long)type);
			continue;
		}
		list[count].index = (unsigned)index;
		list[count].size = size;
		memcpy(list[count].bytes, p, size);
		count++;
	}
	own = [NSData dataWithBytes:[air bytes] length:[air length]];
	mesa.constants_register([own bytes], [own length], list, count);
	/* every value, as bytes: what a specialisation of RenderBox's uber shaders was given is the one
	 * thing the Mac cannot produce for itself (glass23: uber_fragment hung the GPU), and with it
	 * glassref replays the same specialisation against Apple's GPU */
	{
		static unsigned said;
		if (said++ < 400) {
			char line[1024];
			int at = snprintf(line, sizeof(line), "fcvals %s:", [[((id (*)(id, SEL))objc_msgSend)(function,
			                  sel_getUid("name")) description] UTF8String]);
			for (unsigned i = 0; i < count && at < (int)sizeof(line) - 48; i++) {
				at += snprintf(line + at, sizeof(line) - at, " %u=", list[i].index);
				for (unsigned k = 0; k < list[i].size && at < (int)sizeof(line) - 4; k++)
					at += snprintf(line + at, sizeof(line) - at, "%02x", list[i].bytes[k]);
			}
			rw_print("%s\n", line);
		}
	}
	{
		static unsigned specialised;
		static unsigned long long bytes;

		specialised++;
		bytes += [own length];
		if (specialised <= 8 || specialised % 32 == 0) {
			rw_print("function constants: %s, %u of the %u it declares given (%u specialised so "
			         "far, %llu KB of bitcode kept)\n",
			         [[((id (*)(id, SEL))objc_msgSend)(function, sel_getUid("name")) description]
			             UTF8String], count, cap, specialised, bytes >> 10);
		}
	}
	free(list);
	objc_setAssociatedObject(function, &rw_key_air, own, OBJC_ASSOCIATION_RETAIN);
	return own;
}
