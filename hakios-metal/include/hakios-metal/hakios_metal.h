/*
 * The driver's core as C, for hakios's Metal plug-in (userland/src/HakiosMetal.m), which is C and Objective-C and does
 * not take C++ classes. Every object is an opaque pointer, every call says by its result whether it worked, and a
 * failure leaves its reason for hakios_metal_error().
 *
 *   device          the GPU, or NULL when the kernel has none to give (no gpu=1, no firmware, QEMU)
 *   buffer          GPU memory with its CPU mapping (write-combined) and its GPU address
 *   library         a .metallib taken apart: its functions by name, each one's AIR
 *   kernel          a compute function compiled for this GPU, in this process (the compiler is built in with MESA=1)
 *   queue           command buffers complete in commit order; their callbacks run on the queue's own thread
 *   command buffer  copy and fill, done by the GPU's command processor; a kernel dispatched over a grid; commit; wait
 */
#ifndef HAKIOS_METAL_H
#define HAKIOS_METAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hakios_metal_device hakios_metal_device;
typedef struct hakios_metal_buffer hakios_metal_buffer;
typedef struct hakios_metal_library hakios_metal_library;
typedef struct hakios_metal_kernel hakios_metal_kernel;
typedef struct hakios_metal_queue hakios_metal_queue;
typedef struct hakios_metal_cb hakios_metal_cb;

enum hakios_metal_status {
	HAKIOS_METAL_NOT_ENQUEUED = 0,  /* as MTLCommandBufferStatus numbers them */
	HAKIOS_METAL_ENQUEUED = 1,
	HAKIOS_METAL_COMMITTED = 2,
	HAKIOS_METAL_SCHEDULED = 3,
	HAKIOS_METAL_COMPLETED = 4,
	HAKIOS_METAL_ERROR = 5,
};

/* a function's type in a .metallib, as the container's TYPE tag numbers them (MTLFunctionType is these plus one) */
enum hakios_metal_function_type {
	HAKIOS_METAL_FUNCTION_VERTEX = 0,
	HAKIOS_METAL_FUNCTION_FRAGMENT = 1,
	HAKIOS_METAL_FUNCTION_KERNEL = 2,
};

/* what a compiled kernel binds (the plug-in's reflection, and what the encoder must be given) */
enum hakios_metal_resource_kind {
	HAKIOS_METAL_RESOURCE_CONSTANT_BUFFER = 0,   /* [[buffer(n)]] of constant address space */
	HAKIOS_METAL_RESOURCE_DEVICE_BUFFER = 1,     /* [[buffer(n)]] of device address space */
	HAKIOS_METAL_RESOURCE_THREADGROUP_BUFFER = 2,
	HAKIOS_METAL_RESOURCE_TEXTURE = 3,           /* [[texture(n)]] declared read or sample: sampled through the texture table */
	HAKIOS_METAL_RESOURCE_IMAGE = 4,             /* [[texture(n)]] declared write or read_write: stored through an IBO slot */
	HAKIOS_METAL_RESOURCE_SAMPLER = 5,           /* [[sampler(m)]] */
};

/* Textures: MTLTextureType, MTLPixelFormat, MTLTextureUsage and MTLSamplerDescriptor's fields are passed as Metal's own
 * numbers (MTLTexture.h, MTLPixelFormat.h, MTLSampler.h, MTLDepthStencil.h), so the plug-in hands them through. What
 * is served: type 2 (2D), one level, one slice, formats 10 (R8Unorm), 30 (RG8Unorm), 53 (R32Uint), 55 (R32Float),
 * 70 (RGBA8Unorm), 71 (RGBA8Unorm_sRGB), 80 (BGRA8Unorm), 81 (BGRA8Unorm_sRGB), 115 (RGBA16Float), 123 (RGBA32Uint),
 * 125 (RGBA32Float); usage bits 1 (shader read) and 2 (shader write). Anything else: NULL with the reason. */
typedef struct hakios_metal_texture hakios_metal_texture;
typedef struct hakios_metal_sampler hakios_metal_sampler;

/* the last failure on this thread, in words */
const char *hakios_metal_error(void);

hakios_metal_device *hakios_metal_open(void);
/* (for the tests: a device on the fake GPU, which runs the packets in this process) */
hakios_metal_device *hakios_metal_open_fake(void);
void hakios_metal_close(hakios_metal_device *device);
/* the chip id (0x06030500 on the phone: the kernel's GPU_ID is 0, the chip id is what names the GPU) */
uint64_t hakios_metal_gpu_id(hakios_metal_device *device);
/* whether kernels can be compiled in this build (MESA=1); without it hakios_metal_kernel_compile* refuse by name */
int hakios_metal_has_compiler(void);

hakios_metal_buffer *hakios_metal_buffer_new(hakios_metal_device *device, uint64_t length);
void hakios_metal_buffer_free(hakios_metal_buffer *buffer);
void *hakios_metal_buffer_contents(hakios_metal_buffer *buffer);
uint64_t hakios_metal_buffer_length(hakios_metal_buffer *buffer);
uint64_t hakios_metal_buffer_gpu_address(hakios_metal_buffer *buffer);

/* a .metallib's bytes (thin or universal), copied; NULL when the file does not hold together, with the reason */
hakios_metal_library *hakios_metal_library_open(const void *data, size_t size);
void hakios_metal_library_free(hakios_metal_library *library);
unsigned hakios_metal_library_count(hakios_metal_library *library);
const char *hakios_metal_library_name(hakios_metal_library *library, unsigned i);
int hakios_metal_library_type(hakios_metal_library *library, unsigned i);   /* enum hakios_metal_function_type */

/* FUNCTION of LIBRARY compiled for this GPU (AIR to NIR to ir3, here); NULL with the reason: no such function, not a
 * compute kernel, the front end's or ir3's refusal by name, or no compiler in this build */
hakios_metal_kernel *hakios_metal_kernel_compile(hakios_metal_library *library, const char *function);
/* the same from a module's bitcode (a .bc, or a function's AIR as Metal hands it out: -[_MTLFunction bitcodeData]) */
hakios_metal_kernel *hakios_metal_kernel_compile_bitcode(const void *bitcode, size_t size);
void hakios_metal_kernel_free(hakios_metal_kernel *kernel);
const char *hakios_metal_kernel_name(hakios_metal_kernel *kernel);
unsigned hakios_metal_kernel_resource_count(hakios_metal_kernel *kernel);
/* resource I: its kind, Metal index and declared size in bytes (0 when the metadata gives none); -1 past the count */
int hakios_metal_kernel_resource(hakios_metal_kernel *kernel, unsigned i, unsigned *kind, unsigned *index, unsigned *size_bytes);
/* the threadgroup size the kernel fixed, or 0 0 0 when it is the dispatch's to choose (every Metal kernel so far) */
void hakios_metal_kernel_threadgroup_size(hakios_metal_kernel *kernel, unsigned size[3]);
/* MTLComputePipelineState's numbers: the most threads a threadgroup may have, and a SIMD group's width */
unsigned hakios_metal_kernel_max_threads(hakios_metal_kernel *kernel);
unsigned hakios_metal_kernel_execution_width(hakios_metal_kernel *kernel);
unsigned hakios_metal_kernel_threadgroup_memory(hakios_metal_kernel *kernel);   /* bytes it declares statically */

/* a texture of the GPU's own layout (linear rows of hakios_metal_texture_bytes_per_row bytes, the memory behind them a
 * buffer the command buffers keep alive while they use it) */
hakios_metal_texture *hakios_metal_texture_new(hakios_metal_device *device, unsigned type, unsigned pixel_format,
    unsigned width, unsigned height, unsigned usage);
void hakios_metal_texture_free(hakios_metal_texture *texture);
void *hakios_metal_texture_contents(hakios_metal_texture *texture);        /* the GPU's rows, write-combined */
uint64_t hakios_metal_texture_bytes_per_row(hakios_metal_texture *texture); /* the GPU's pitch (MTLTexture.bufferBytesPerRow) */
uint64_t hakios_metal_texture_gpu_address(hakios_metal_texture *texture);
unsigned hakios_metal_texture_width(hakios_metal_texture *texture);
unsigned hakios_metal_texture_height(hakios_metal_texture *texture);
unsigned hakios_metal_texture_format(hakios_metal_texture *texture);
unsigned hakios_metal_texture_type(hakios_metal_texture *texture);
unsigned hakios_metal_texture_usage(hakios_metal_texture *texture);
/* -replaceRegion:mipmapLevel:withBytes:bytesPerRow: for level 0: H rows of W pixels from BYTES, the caller's rows
 * BYTES_PER_ROW apart, into the texture at (X, Y); the pitch conversion happens here. -1 with the reason: a region outside
 * the texture, a stride shorter than W pixels. */
int hakios_metal_texture_replace(hakios_metal_texture *texture, unsigned x, unsigned y, unsigned w, unsigned h,
    const void *bytes, uint64_t bytes_per_row);
/* -getBytes:bytesPerRow:fromRegion:mipmapLevel:, the other way */
int hakios_metal_texture_read(hakios_metal_texture *texture, unsigned x, unsigned y, unsigned w, unsigned h,
    void *bytes, uint64_t bytes_per_row);

/* MTLSamplerDescriptor's fields, Metal's numbering and defaults (zero everywhere but normalized_coordinates 1,
 * lod_max_clamp FLT_MAX, max_anisotropy 1) */
struct hakios_metal_sampler_desc {
	unsigned min_filter, mag_filter;      /* MTLSamplerMinMagFilter: Nearest 0, Linear 1 */
	unsigned mip_filter;                  /* MTLSamplerMipFilter: NotMipmapped 0, Nearest 1, Linear 2 */
	unsigned s_address, t_address, r_address;   /* MTLSamplerAddressMode: ClampToEdge 0, MirrorClampToEdge 1, Repeat 2, MirrorRepeat 3, ClampToZero 4, ClampToBorderColor 5 */
	int normalized_coordinates;
	float lod_min_clamp, lod_max_clamp;
	unsigned max_anisotropy;
	unsigned compare_function;            /* MTLCompareFunction: Never 0 .. Always 7 */
	unsigned border_color;                /* MTLSamplerBorderColor: TransparentBlack 0, OpaqueBlack 1, OpaqueWhite 2 */
};
/* NULL with the reason for a mode not served (texture.hpp: makeSampler) */
hakios_metal_sampler *hakios_metal_sampler_new(hakios_metal_device *device, const struct hakios_metal_sampler_desc *desc);
void hakios_metal_sampler_free(hakios_metal_sampler *sampler);
const uint32_t *hakios_metal_sampler_words(hakios_metal_sampler *sampler);   /* the four TEX_SAMP dwords, for the tests */

/* resource I of a kernel when it is a texture, image or sampler: the dimension (MTLTextureType numbering, 2 for 2D),
 * the element class the kernel addresses it as (0 float, 1 uint, 2 int) and the access it makes (bit 1 read, bit 2 write);
 * -1 for a buffer */
int hakios_metal_kernel_resource_texture(hakios_metal_kernel *kernel, unsigned i, unsigned *texture_type, unsigned *element,
    unsigned *access);

/* Rendering (render.hpp): a vertex function and a fragment function compiled and linked for one colour attachment, and
 * one draw of them into it, in SYSMEM mode, with no depth, no blending, no vertex descriptor yet. Metal's numbers are
 * passed through as everywhere in this header. */
typedef struct hakios_metal_program hakios_metal_program;

#define HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET 4   /* MTLTextureUsageRenderTarget: hakios_metal_texture_new accepts it; a draw needs it */

/* VS of VS_LIBRARY and FS of FS_LIBRARY (the same library is fine) compiled here and linked, for a colour attachment of
 * COLOR_PIXEL_FORMAT (MTLPixelFormat, one of the served formats). NULL with the reason: not a vertex / fragment
 * function, the front end's or ir3's refusal by name, a vertex stage with [[stage_in]] attributes (not served yet), a
 * fragment stage with more than one colour output, a depth/stencil/sample-mask output, framebuffer fetch, a varying the
 * vertex stage does not write, a stage that binds buffers, textures or samplers (step two), a format whose element class
 * the fragment output does not match, no compiler in this build. */
hakios_metal_program *hakios_metal_program_new(hakios_metal_library *vs_library, const char *vs,
    hakios_metal_library *fs_library, const char *fs, unsigned color_pixel_format);
/* the same from two modules' bitcode (-[_MTLFunction bitcodeData] of each) */
hakios_metal_program *hakios_metal_program_new_bitcode(const void *vs_bitcode, size_t vs_size,
    const void *fs_bitcode, size_t fs_size, unsigned color_pixel_format);
void hakios_metal_program_free(hakios_metal_program *program);
const char *hakios_metal_program_name(hakios_metal_program *program);   /* "vs+fs" */
/* STAGE 0 = vertex, 1 = fragment: the resources that stage binds, as hakios_metal_kernel_resource reports a kernel's */
unsigned hakios_metal_program_resource_count(hakios_metal_program *program, unsigned stage);
int hakios_metal_program_resource(hakios_metal_program *program, unsigned stage, unsigned i,
    unsigned *kind, unsigned *index, unsigned *size_bytes);

/* the program's vertex descriptor (MTLRenderPipelineDescriptor.vertexDescriptor), Metal's numbers: [[attribute(INDEX)]]
 * is FORMAT (MTLVertexFormat; Float..Float4, Half2, Half4, UChar4Normalized served) at OFFSET bytes into each vertex of
 * [[buffer(BUFFER_INDEX)]]; that buffer's layout is STRIDE bytes a vertex and STEP_FUNCTION (MTLVertexStepFunction;
 * PerVertex 1 served). -1 with the reason for an index past 30. What is not served is refused by the draw, by name. */
int hakios_metal_program_set_vertex_attribute(hakios_metal_program *program, unsigned index, unsigned format, unsigned offset,
    unsigned buffer_index);
int hakios_metal_program_set_vertex_layout(hakios_metal_program *program, unsigned buffer_index, unsigned stride,
    unsigned step_function, unsigned step_rate);

struct hakios_metal_viewport { double x, y, width, height, znear, zfar; };   /* MTLViewport: origin top left, NDC y up */
struct hakios_metal_scissor { unsigned x, y, width, height; };              /* MTLScissorRect */

/* MTLRenderPassDescriptor's colour attachment 0, as the plug-in reads it */
struct hakios_metal_render_pass {
	hakios_metal_texture *color0;     /* made with usage bit 4 */
	unsigned load_action;             /* MTLLoadAction: DontCare 0, Load 1, Clear 2 (the whole attachment filled before the draw) */
	unsigned store_action;            /* MTLStoreAction: DontCare 0, Store 1; others refused */
	double clear_color[4];            /* MTLClearColor, read only for Clear: RGBA; whole numbers 0..2^32-1 for a Uint attachment */
};

/* One draw: PROGRAM over VERTEX_COUNT vertices from VERTEX_START, PRIMITIVE an MTLPrimitiveType (Point 0, Line 1,
 * LineStrip 2, Triangle 3, TriangleStrip 4; step one serves 3 and 4), INSTANCE_COUNT (step one: 1), into PASS's
 * attachment through VIEWPORT and SCISSOR (NULL: the whole attachment), with the two stages' [[buffer(n)]] tables (step
 * one: counts of 0 only). -1 with the reason when the program, the pass and the draw do not fit together; everything
 * stays alive until the command buffer completes. Each call is one complete pass (begin, state, draw, end). */
int hakios_metal_cb_draw(hakios_metal_cb *cb, hakios_metal_program *program,
    const struct hakios_metal_render_pass *pass, const struct hakios_metal_viewport *viewport,
    const struct hakios_metal_scissor *scissor, unsigned primitive, unsigned vertex_start, unsigned vertex_count,
    unsigned instance_count,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count);
/* A pass with no draw, as an encoder that recorded none ends: only a Clear writes anything then (PASS's load action
 * must be 2), and the attachment is filled with its clear colour. -1 with the reason otherwise. With draws, each
 * hakios_metal_cb_draw is a whole pass of its own: the caller passes the Clear to the first and Load to the rest. */
int hakios_metal_cb_clear(hakios_metal_cb *cb, const struct hakios_metal_render_pass *pass);

hakios_metal_queue *hakios_metal_queue_new(hakios_metal_device *device);
void hakios_metal_queue_free(hakios_metal_queue *queue);

hakios_metal_cb *hakios_metal_cb_new(hakios_metal_queue *queue);
/* the command buffer is released by its owner; one still in flight is waited for first. It may be freed inside its
 * own callback (the callback is the last thing to use the pointer). */
void hakios_metal_cb_free(hakios_metal_cb *cb);
int hakios_metal_cb_copy(hakios_metal_cb *cb, hakios_metal_buffer *from, uint64_t from_offset, hakios_metal_buffer *to,
    uint64_t to_offset, uint64_t length);
int hakios_metal_cb_fill(hakios_metal_cb *cb, hakios_metal_buffer *buffer, uint64_t offset, uint64_t length, uint8_t value);
/* KERNEL over GROUPS threadgroups of THREADS threads each (MTLComputeCommandEncoder's dispatchThreadgroups:
 * threadsPerThreadgroup:), with BUFFERS[i] at OFFSETS[i] bound at Metal index i for i < COUNT (NULL: nothing there).
 * -1 with the reason when the kernel and the bindings do not fit together; the kernel and the buffers stay alive
 * until the command buffer completes, whatever the caller frees meanwhile. */
int hakios_metal_cb_dispatch(hakios_metal_cb *cb, hakios_metal_kernel *kernel, const unsigned groups[3], const unsigned threads[3],
    hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count);
/* the same with TEXTURES[n] at Metal texture index n for n < TEXTURE_COUNT (a sampled texture or an image, whichever the
 * kernel declared at n; NULL: nothing there) and SAMPLERS[m] at sampler index m for m < SAMPLER_COUNT. -1 with the reason:
 * a declared texture or sampler missing, a texture whose dimension or element class is not the kernel's, one made without
 * write usage where the kernel writes. hakios_metal_cb_dispatch is this with no textures and no samplers. */
int hakios_metal_cb_dispatch2(hakios_metal_cb *cb, hakios_metal_kernel *kernel, const unsigned groups[3], const unsigned threads[3],
    hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count,
    hakios_metal_texture *const *textures, unsigned texture_count,
    hakios_metal_sampler *const *samplers, unsigned sampler_count);
/* something the caller could not record: the command buffer will end in HAKIOS_METAL_ERROR, saying WHY */
void hakios_metal_cb_fail(hakios_metal_cb *cb, const char *why);
/* CALLBACK(CB, CONTEXT) runs once, after completion or error, on the queue's thread; may be NULL */
void hakios_metal_cb_commit(hakios_metal_cb *cb, void (*callback)(hakios_metal_cb *cb, void *context), void *context);
void hakios_metal_cb_wait(hakios_metal_cb *cb);
enum hakios_metal_status hakios_metal_cb_status(hakios_metal_cb *cb);
/* why it ended in HAKIOS_METAL_ERROR, or "" */
const char *hakios_metal_cb_error(hakios_metal_cb *cb);
/* how long the GPU had it, in nanoseconds (commit to fence), once completed */
int64_t hakios_metal_cb_gpu_nanoseconds(hakios_metal_cb *cb);

#ifdef __cplusplus
}
#endif

#endif
