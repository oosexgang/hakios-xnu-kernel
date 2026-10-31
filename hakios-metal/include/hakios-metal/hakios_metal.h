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
/* A function constant the pipeline supplies (MTLFunctionConstantValues): INDEX the [[function_constant(n)]], BITS the
 * value as the bits of its type -- a bool 0 or 1, an integer or float its own bits in the low end, a vector's
 * component i at bit i * its width. A constant the module declares and the array does not name is not defined to it
 * (is_function_constant_defined) and reads as zero; an index it does not declare is ignored. */
typedef struct hakios_metal_constant {
    unsigned index;
    uint64_t bits;
} hakios_metal_constant;
hakios_metal_kernel *hakios_metal_kernel_compile_bitcode_constants(const void *bitcode, size_t size,
    const hakios_metal_constant *constants, unsigned count);
void hakios_metal_kernel_free(hakios_metal_kernel *kernel);
const char *hakios_metal_kernel_name(hakios_metal_kernel *kernel);
/* the argument buffer at [[buffer(SET)]] of the function in BITCODE, as MTLFunction's -newArgumentEncoderWithBufferIndex:
 * lays it out: every member the set declares, in id order, its [[id(n)]] into IDS and its byte offset into OFFSETS (at
 * most MAX of them). Returns how many members the set has -- more than MAX means the arrays were too short -- or -1 with
 * hakios_metal_error(): the front end refused the function, or there is no set at that index. Only the front end runs. */
int hakios_metal_argument_buffer_layout(const void *bitcode, size_t size, unsigned set, unsigned *ids, unsigned *offsets, unsigned max);
/* the bytes of GPU memory DEVICE has allocated and not given back: buffers, textures and the driver's own (command
 * streams, tables); not an IOSurface's, which is its owner's. MTLDevice's currentAllocatedSize. */
uint64_t hakios_metal_device_allocated_bytes(hakios_metal_device *device);
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
/* ...of LEVELS mip levels (MTLTextureDescriptor.mipmapLevelCount: 1 to floor(log2(max(width, height))) + 1) */
hakios_metal_texture *hakios_metal_texture_new_levels(hakios_metal_device *device, unsigned type, unsigned pixel_format,
    unsigned width, unsigned height, unsigned levels, unsigned usage);
unsigned hakios_metal_texture_levels(hakios_metal_texture *texture);
/* a 2D multisample texture (MTLTextureType2DMultisample) of SAMPLES (2 or 4) samples a pixel, a render target; it is
 * drawn into by a program of the same count (hakios_metal_program_set_samples) and resolved by hakios_metal_cb_blit
 * into a texture of one sample, pixel for pixel */
hakios_metal_texture *hakios_metal_texture_new_multisample(hakios_metal_device *device, unsigned pixel_format, unsigned width,
    unsigned height, unsigned samples, unsigned usage);
unsigned hakios_metal_texture_samples(hakios_metal_texture *texture);   /* 1, or a multisample texture's count */
/* -newTextureWithDescriptor:iosurface:plane: -- the texture's rows are IOSurface SURFACE_ID's pixels (the kernel's
 * surface buffer: drawing into it draws into what the display or another program reads), PIXEL_FORMAT, WIDTH, HEIGHT
 * and USAGE as for hakios_metal_texture_new. NULL with the reason: no such surface, a plane other than 0, a size past
 * the surface's, rows the GPU cannot take (pitch or start not a multiple of 64 bytes), a FourCC that is not the format's,
 * a depth format. */
hakios_metal_texture *hakios_metal_texture_new_iosurface(hakios_metal_device *device, unsigned surface_id, unsigned plane,
    unsigned pixel_format, unsigned width, unsigned height, unsigned usage);
/* the same for a two-plane format (YCbCr8_420_2P, 500) over a '420f' or '420v' surface: CHROMA_OFFSET where the surface's
 * second plane starts in its memory (IOSurfaceGetBaseAddressOfPlane(s, 1) less IOSurfaceGetBaseAddress(s)) and
 * CHROMA_ROW_BYTES its rows; both planes are sampled as one texture. */
hakios_metal_texture *hakios_metal_texture_new_iosurface2(hakios_metal_device *device, unsigned surface_id, unsigned plane,
    unsigned pixel_format, unsigned width, unsigned height, unsigned usage, uint64_t chroma_offset, unsigned chroma_row_bytes);
/* -[MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:]: a texture over BUFFER's bytes from OFFSET, BYTES_PER_ROW apart
 * (the same memory). NULL with the reason: a pitch or start that is not a multiple of 64 bytes, rows past the buffer, a
 * format, type or level count a buffer does not take. */
hakios_metal_texture *hakios_metal_texture_new_buffer(hakios_metal_buffer *buffer, unsigned type, unsigned pixel_format,
    unsigned width, unsigned height, unsigned usage, uint64_t offset, uint64_t bytes_per_row);
/* -newTextureViewWithPixelFormat:textureType:levels:slices:: a texture over TEXTURE's memory, of PIXEL_FORMAT and TYPE,
 * TEXTURE's levels BASE_LEVEL .. + LEVELS - 1 and slices BASE_SLICE .. + SLICES - 1 (texture.hpp: Texture::view says
 * which are served). Freed on its own; the memory lives while either does. NULL with the reason. */
/* TEXTURE sampled through a swizzle: RED..ALPHA are MTLTextureSwizzle numbers (0 zero, 1 one, 2 red, 3 green, 4 blue,
 * 5 alpha), over what TEXTURE already reads. NULL with the reason for a number past 5 */
hakios_metal_texture *hakios_metal_texture_new_swizzled(hakios_metal_texture *texture, unsigned red, unsigned green, unsigned blue, unsigned alpha);
hakios_metal_texture *hakios_metal_texture_new_view(hakios_metal_texture *texture, unsigned pixel_format, unsigned type, unsigned base_level,
    unsigned levels, unsigned base_slice, unsigned slices);
/* -heapTextureSizeAndAlignWithDescriptor:: the bytes a texture of these fields would take (TYPE and PIXEL_FORMAT
 * Metal's numbers, DEPTH a 3D texture's slices or an array's layers, 1 otherwise), allocating nothing -- the same
 * plan the texture's creation follows, so a heap sized by it holds the texture. -1 with the reason creation gives. */
int hakios_metal_texture_plan_bytes(unsigned type, unsigned pixel_format, unsigned width, unsigned height, unsigned depth, unsigned levels,
    unsigned samples, unsigned usage, uint64_t *bytes);
/* the bytes of memory TEXTURE's layout takes (MTLResource.allocatedSize); a view answers with its parent's memory */
uint64_t hakios_metal_texture_allocated_bytes(hakios_metal_texture *texture);
unsigned hakios_metal_texture_iosurface(hakios_metal_texture *texture);   /* its IOSurface's ID, 0 for a texture of its own */
/* what an argument buffer holds for TEXTURE or SAMPLER (MTLTexture/MTLSamplerState.gpuResourceID): never 0, and found
 * again by a draw or dispatch reading the argument buffer while the object lives. A buffer's is its GPU address. */
uint64_t hakios_metal_texture_resource_id(hakios_metal_texture *texture);
uint64_t hakios_metal_sampler_resource_id(hakios_metal_sampler *sampler);
void hakios_metal_texture_free(hakios_metal_texture *texture);
void *hakios_metal_texture_contents(hakios_metal_texture *texture);        /* the GPU's rows, write-combined */
uint64_t hakios_metal_texture_bytes_per_row(hakios_metal_texture *texture); /* the GPU's pitch (MTLTexture.bufferBytesPerRow) */
uint64_t hakios_metal_texture_gpu_address(hakios_metal_texture *texture);
unsigned hakios_metal_texture_width(hakios_metal_texture *texture);
unsigned hakios_metal_texture_height(hakios_metal_texture *texture);
unsigned hakios_metal_texture_format(hakios_metal_texture *texture);
unsigned hakios_metal_texture_type(hakios_metal_texture *texture);
unsigned hakios_metal_texture_usage(hakios_metal_texture *texture);
/* one pixel's bytes in its rows (the luma plane's for a two-plane format): the bytes a copy of W pixels of a row moves */
unsigned hakios_metal_texture_bytes_per_pixel(hakios_metal_texture *texture);
/* PIXEL_FORMAT's element as the core lays it out: BLOCK_W x BLOCK_H texels (1 x 1 but for a compressed format) in BYTES
 * bytes. 0 with the reason when the format is not served. */
int hakios_metal_format_block(unsigned pixel_format, unsigned *block_w, unsigned *block_h, unsigned *bytes);
/* a 3D texture (MTLTextureType3D) of DEPTH slices, one level, sampled alone (usage 1); its slices written and read as
 * -replaceRegion:...slice:withBytes:bytesPerRow:bytesPerImage: has them, Z to Z + D - 1 */
hakios_metal_texture *hakios_metal_texture_new_3d(hakios_metal_device *device, unsigned pixel_format, unsigned width, unsigned height,
    unsigned depth, unsigned usage);
/* a 2D array (MTLTextureType2DArray) of LAYERS, one level, sampled alone; its layers are the slices the _3d copies move,
 * Z the layer (-replaceRegion:...slice:'s slice) */
hakios_metal_texture *hakios_metal_texture_new_array(hakios_metal_device *device, unsigned pixel_format, unsigned width, unsigned height,
    unsigned layers, unsigned usage);
/* a layered texture of TYPE (MTLTextureType: 1DArray 1, 2DArray 3, Cube 5, CubeArray 6, 3D 7) and LAYERS slices --
 * a cube array's are its faces, six a cube, in Metal's slice order; a 1D array is HEIGHT 1. One level. */
hakios_metal_texture *hakios_metal_texture_new_layers(hakios_metal_device *device, unsigned type, unsigned pixel_format, unsigned width,
    unsigned height, unsigned layers, unsigned usage);
/* a cube (MTLTextureTypeCube) of SIZE x SIZE faces, one level, sampled alone; face f (Metal's slice) is the slice copies' Z */
hakios_metal_texture *hakios_metal_texture_new_cube(hakios_metal_device *device, unsigned pixel_format, unsigned size, unsigned usage);
/* a layered texture of TYPE (MTLTextureType: 1DArray 1, 2DArray 3, Cube 5, CubeArray 6) with LEVELS mip levels: each
 * layer the chain of them, as fdl6 lays it (hakios_metal_texture_new_layers is LEVELS 1); LAYERS counts faces for a
 * cube or cube array, six a cube */
hakios_metal_texture *hakios_metal_texture_new_layered(hakios_metal_device *device, unsigned type, unsigned pixel_format, unsigned width,
    unsigned height, unsigned layers, unsigned levels, unsigned usage);
unsigned hakios_metal_texture_depth(hakios_metal_texture *texture);   /* a 3D texture's slices, a 2D array's layers, else 1 */
/* slices Z to Z + D - 1 of mip level LEVEL (a layered texture's levels are each layer's own; a 3D texture has one) */
int hakios_metal_texture_replace_3d(hakios_metal_texture *texture, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w, unsigned h,
    unsigned d, const void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image);
int hakios_metal_texture_read_3d(hakios_metal_texture *texture, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w, unsigned h,
    unsigned d, void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image);
/* -replaceRegion:mipmapLevel:withBytes:bytesPerRow: for level 0: H rows of W pixels from BYTES, the caller's rows
 * BYTES_PER_ROW apart, into the texture at (X, Y); the pitch conversion happens here. -1 with the reason: a region outside
 * the texture, a stride shorter than W pixels. */
int hakios_metal_texture_replace(hakios_metal_texture *texture, unsigned x, unsigned y, unsigned w, unsigned h,
    const void *bytes, uint64_t bytes_per_row);
/* -getBytes:bytesPerRow:fromRegion:mipmapLevel:, the other way */
int hakios_metal_texture_read(hakios_metal_texture *texture, unsigned x, unsigned y, unsigned w, unsigned h,
    void *bytes, uint64_t bytes_per_row);
/* the two of mip level LEVEL (the region inside that level's size) */
int hakios_metal_texture_replace_level(hakios_metal_texture *texture, unsigned level, unsigned x, unsigned y, unsigned w,
    unsigned h, const void *bytes, uint64_t bytes_per_row);
int hakios_metal_texture_read_level(hakios_metal_texture *texture, unsigned level, unsigned x, unsigned y, unsigned w,
    unsigned h, void *bytes, uint64_t bytes_per_row);

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
 * draws of them into it, in SYSMEM mode, with no depth yet. Metal's numbers are passed through as everywhere in this
 * header. */
typedef struct hakios_metal_program hakios_metal_program;

#define HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET 4   /* MTLTextureUsageRenderTarget: hakios_metal_texture_new accepts it; a draw needs it */

/* VS of VS_LIBRARY and FS of FS_LIBRARY (the same library is fine) compiled here and linked, for a colour attachment of
 * COLOR_PIXEL_FORMAT (MTLPixelFormat, one of the served formats). NULL with the reason: not a vertex / fragment
 * function, the front end's or ir3's refusal by name, a fragment stage with more than one colour output, a depth/stencil/sample-mask output, framebuffer fetch, a varying the
 * vertex stage does not write, a vertex stage with textures, an image in either stage, a format whose element class
 * the fragment output does not match, no compiler in this build. */
hakios_metal_program *hakios_metal_program_new(hakios_metal_library *vs_library, const char *vs,
    hakios_metal_library *fs_library, const char *fs, unsigned color_pixel_format);
/* the same from two modules' bitcode (-[_MTLFunction bitcodeData] of each) */
hakios_metal_program *hakios_metal_program_new_bitcode(const void *vs_bitcode, size_t vs_size,
    const void *fs_bitcode, size_t fs_size, unsigned color_pixel_format);
/* ...each function specialised by its own constants, as each MTLFunction of a pipeline carries its own. FS_BITCODE
 * NULL (or FS_SIZE 0) is a pipeline with no fragment function: a built-in one that writes nothing is drawn, so only
 * depth and stencil come out (a depth-only pass's pipeline). */
hakios_metal_program *hakios_metal_program_new_bitcode_constants(const void *vs_bitcode, size_t vs_size,
    const hakios_metal_constant *vs_constants, unsigned vs_count, const void *fs_bitcode, size_t fs_size,
    const hakios_metal_constant *fs_constants, unsigned fs_count, unsigned color_pixel_format);
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
/* colour attachment 0's blend (MTLRenderPipelineColorAttachmentDescriptor), Metal's numbers: ENABLED (blendingEnabled),
 * RGB_OP / ALPHA_OP (MTLBlendOperation), the four factors (MTLBlendFactor; the Source1 ones refused at the draw: no
 * dual-source blending), WRITE_MASK (MTLColorWriteMask: Red 8, Green 4, Blue 2, Alpha 1). Off with the mask 0xf until
 * this is called. What a draw cannot serve (blending into an integer format, say) it refuses by name. */
void hakios_metal_program_set_blend(hakios_metal_program *program, int enabled, unsigned rgb_op, unsigned alpha_op,
    unsigned src_rgb, unsigned dst_rgb, unsigned src_alpha, unsigned dst_alpha, unsigned write_mask);
/* MTLRenderPipelineDescriptor.rasterSampleCount: 1 (the default), 2 or 4; a draw refuses attachments of another count.
 * -1 for any other count */
int hakios_metal_program_set_samples(hakios_metal_program *program, unsigned samples);
/* ...the same for colour attachment INDEX (0 to 7; 0 is the call above): -1 past 7 */
int hakios_metal_program_set_blend_at(hakios_metal_program *program, unsigned index, int enabled, unsigned rgb_op, unsigned alpha_op,
    unsigned src_rgb, unsigned dst_rgb, unsigned src_alpha, unsigned dst_alpha, unsigned write_mask);

struct hakios_metal_viewport { double x, y, width, height, znear, zfar; };   /* MTLViewport: origin top left, NDC y up */
struct hakios_metal_scissor { unsigned x, y, width, height; };              /* MTLScissorRect */

/* MTLRenderPassDescriptor's colour attachment 0, as the plug-in reads it */
struct hakios_metal_render_pass {
	hakios_metal_texture *color0;     /* made with usage bit 4 */
	unsigned load_action;             /* MTLLoadAction: DontCare 0, Load 1, Clear 2 (the whole attachment filled before the draw) */
	unsigned store_action;            /* MTLStoreAction: DontCare 0, Store 1; others refused */
	double clear_color[4];            /* MTLClearColor, read only for Clear: RGBA; whole numbers 0..2^32-1 for a Uint attachment */
	float blend_color[4];             /* -setBlendColorRed:green:blue:alpha:, the encoder's state each draw reads (0 by default) */
	/* the depth attachment (a Depth32Float texture, 252, made with render-target usage alone) or NULL, its MTLLoadAction
	 * and MTLStoreAction, and the clear depth (0 to 1) read for a Clear */
	hakios_metal_texture *depth;
	unsigned depth_load_action, depth_store_action;
	double clear_depth;
	/* -setDepthStencilState:'s depth half, each draw reads it: when DEPTH_STATE_SET is 0 Metal's default (compare Always,
	 * no write: no test), so that a zeroed struct is Metal's default; else DEPTH_COMPARE (MTLCompareFunction, Never 0 ..
	 * Always 7) and DEPTH_WRITE */
	int depth_state_set;
	unsigned depth_compare;
	int depth_write;
	/* colour attachments 1 to 7 (colorAttachments[1..7]), each the size of color0, TEXTURE NULL when not set, with
	 * color0's rules for the actions and the clear colour. One the fragment function writes ([[color(n)]]) is drawn
	 * into; one it does not write is cleared by a Clear and otherwise left as it is. */
	struct hakios_metal_color_attachment {
		hakios_metal_texture *texture;
		unsigned load_action, store_action;
		double clear_color[4];
		unsigned level;                   /* the mip level drawn into */
		unsigned slice;                   /* the 2D array's layer, cube's face or 3D texture's plane drawn into */
	} more[7];
	unsigned color0_level;                    /* color0's mip level drawn into (MTLRenderPassAttachmentDescriptor.level) */
	/* the stencil attachment: the depth texture itself when it is Depth32Float_Stencil8 (260), or NULL; its MTLLoadAction,
	 * MTLStoreAction and clear value (read for a Clear) */
	hakios_metal_texture *stencil;
	unsigned stencil_load_action, stencil_store_action;
	unsigned clear_stencil;
	/* -setDepthStencilState:'s stencil half and -setStencilReferenceValue:, each draw reads them: STENCIL_STATE_SET 0 is
	 * no test (Metal's default, a descriptor with no stencil faces); each face an MTLCompareFunction, three
	 * MTLStencilOperations (stencil fail, depth fail, both pass) and the read and write masks */
	int stencil_state_set;
	struct hakios_metal_stencil_face {
		unsigned compare, stencil_fail, depth_fail, pass, read_mask, write_mask;
	} stencil_front, stencil_back;
	unsigned stencil_front_reference, stencil_back_reference;
	/* the pass's visibilityResultBuffer and -setVisibilityResultMode:offset:, each draw reads them: VISIBILITY_MODE an
	 * MTLVisibilityResultMode (Disabled 0, Boolean 1, Counting 2); a kept mode adds the samples the draw lets through to
	 * the 64-bit word at VISIBILITY_OFFSET, zeroed first when VISIBILITY_RESET (the encoder's first draw into it) */
	hakios_metal_buffer *visibility;
	unsigned visibility_mode;
	uint64_t visibility_offset;
	int visibility_reset;
	/* -setCullMode: and -setFrontFacingWinding:, each draw reads them: CULL_MODE an MTLCullMode (None 0, Front 1, Back 2),
	 * WINDING the front face's MTLWinding (Clockwise 0, Metal's default; CounterClockwise 1). Zero is Metal's default for both */
	unsigned cull_mode, winding;
	unsigned color0_slice;                    /* color0's layer, face or plane drawn into (MTLRenderPassAttachmentDescriptor.slice) */
	unsigned depth_level;                     /* the depth attachment's mip level drawn into */
	unsigned depth_slice;                     /* a depth 2D array's layer drawn into (MTLRenderPassDepthAttachmentDescriptor.slice) */
};

/* One draw: PROGRAM over VERTEX_COUNT vertices from VERTEX_START, PRIMITIVE an MTLPrimitiveType (Point 0, Line 1,
 * LineStrip 2, Triangle 3, TriangleStrip 4; 3 and 4 are served), INSTANCE_COUNT (1 or more), into PASS's
 * attachment through VIEWPORT and SCISSOR (NULL: the whole attachment), with the two stages' [[buffer(n)]] tables (step
 * one: counts of 0 only). -1 with the reason when the program, the pass and the draw do not fit together; everything
 * stays alive until the command buffer completes. Each call is one complete pass (begin, state, draw, end). */
int hakios_metal_cb_draw(hakios_metal_cb *cb, hakios_metal_program *program,
    const struct hakios_metal_render_pass *pass, const struct hakios_metal_viewport *viewport,
    const struct hakios_metal_scissor *scissor, unsigned primitive, unsigned vertex_start, unsigned vertex_count,
    unsigned instance_count,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count);
/* the same with the fragment stage's FS_TEXTURES[n] at [[texture(n)]] for n < FS_TEXTURE_COUNT and FS_SAMPLERS[m] at
 * [[sampler(m)]] (NULL: nothing there). -1 with the reason: a texture or sampler the fragment function samples and
 * nothing bound there, a texture whose element class is not the function's, one made without read usage.
 * hakios_metal_cb_draw is this with neither. */
int hakios_metal_cb_draw2(hakios_metal_cb *cb, hakios_metal_program *program,
    const struct hakios_metal_render_pass *pass, const struct hakios_metal_viewport *viewport,
    const struct hakios_metal_scissor *scissor, unsigned primitive, unsigned vertex_start, unsigned vertex_count,
    unsigned instance_count,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count,
    hakios_metal_texture *const *fs_textures, unsigned fs_texture_count,
    hakios_metal_sampler *const *fs_samplers, unsigned fs_sampler_count);
/* -drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:: hakios_metal_cb_draw2 with BASE_INSTANCE, the first
 * instance [[instance_id]] counts from and per-instance attributes are fetched from */
int hakios_metal_cb_draw3(hakios_metal_cb *cb, hakios_metal_program *program,
    const struct hakios_metal_render_pass *pass, const struct hakios_metal_viewport *viewport,
    const struct hakios_metal_scissor *scissor, unsigned primitive, unsigned vertex_start, unsigned vertex_count,
    unsigned instance_count, unsigned base_instance,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count,
    hakios_metal_texture *const *fs_textures, unsigned fs_texture_count,
    hakios_metal_sampler *const *fs_samplers, unsigned fs_sampler_count);
/* -drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:, the
 * tables as hakios_metal_cb_draw2's: INDEX_COUNT indices of INDEX_TYPE (MTLIndexType: UInt16 0, UInt32 1) from
 * INDEX_BUFFER at INDEX_OFFSET, BASE_VERTEX added to each (a negative one refused), primitive restart at the type's
 * all-ones as Metal has it; BASE_INSTANCE the first instance [[instance_id]] counts from. The index buffer stays alive until the command buffer completes. */
int hakios_metal_cb_draw_indexed(hakios_metal_cb *cb, hakios_metal_program *program,
    const struct hakios_metal_render_pass *pass, const struct hakios_metal_viewport *viewport,
    const struct hakios_metal_scissor *scissor, unsigned primitive, unsigned index_count, unsigned index_type,
    hakios_metal_buffer *index_buffer, uint64_t index_offset, unsigned instance_count, int base_vertex, unsigned base_instance,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count,
    hakios_metal_texture *const *fs_textures, unsigned fs_texture_count,
    hakios_metal_sampler *const *fs_samplers, unsigned fs_sampler_count);
/* The vertex stage's [[texture(n)]] and [[sampler(m)]] for the draws that follow on CB (setVertexTexture /
 * setVertexSamplerState): TEXTURES[n] for n < TEXTURE_COUNT and SAMPLERS[m] for m < SAMPLER_COUNT, NULL nothing
 * there; the tables are copied, and stay until the next call (counts of 0 clear them). The draws judge them as they
 * judge the fragment stage's. Every draw call above takes them. */
void hakios_metal_cb_set_vertex_textures(hakios_metal_cb *cb, hakios_metal_texture *const *textures, unsigned texture_count,
    hakios_metal_sampler *const *samplers, unsigned sampler_count);
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
/* -copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:...: SW x SH at (SX, SY) of FROM's level
 * FROM_LEVEL into DW x DH at (DX, DY) of TO's level TO_LEVEL, one pixel format; another size is a scale, LINEAR filtered.
 * -1 with the reason (the command buffer ends in error). */
int hakios_metal_cb_blit(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned from_level, unsigned sx, unsigned sy,
    unsigned sw, unsigned sh, hakios_metal_texture *to, unsigned to_level, unsigned dx, unsigned dy, unsigned dw,
    unsigned dh, int linear);
/* FROM's level 0 into TO's level 0 turned by the 2D engine (a6xx_rotation ROTATION: 1 and 3 quarter turns, which take
 * a TO of FROM's sides swapped, 2 a half turn, 4 and 5 the flips): one pixel format, copied pixel for pixel. A
 * framebuffer CoreAnimation renders landscape over the portrait display surface goes into it so. -1 with the reason. */
int hakios_metal_cb_blit_rotated(hakios_metal_cb *cb, hakios_metal_texture *from, hakios_metal_texture *to, unsigned rotation);
/* What a refused draw, dispatch, copy or clear does to CB (CommandBuffer::setSkipRefused): 0, the default, ends CB in
 * error with the reason; 1 leaves that piece of work out and runs the rest, as Apple's driver runs a frame whatever is
 * wrong in it. hakios_metal_cb_refuse is the caller's own refusal of a piece of work, by the same rule; it returns -1,
 * and hakios_metal_cb_skipped how many were left out and the last reason. */
void hakios_metal_cb_skip_refused(hakios_metal_cb *cb, int on);
/* CB's passes timed by the GPU's own counter (CommandBuffer::setProfile, before anything is recorded) and, once it has
 * completed, pass I's kind (0 draw, 1 copy, 2 clear, 3 dispatch), target size, draws and microseconds; the count of
 * passes timed is returned, -1 for an I past them (any pointer may be NULL) */
void hakios_metal_cb_set_profile(hakios_metal_cb *cb, int on);
int hakios_metal_cb_pass_time(hakios_metal_cb *cb, unsigned i, unsigned *kind, unsigned *width, unsigned *height, unsigned *draws, double *us);
/* what CB holds so far (CommandBuffer::counts): passes opened, draws, dispatches, copies and clears; any pointer NULL */
void hakios_metal_cb_counts(hakios_metal_cb *cb, unsigned *passes, unsigned *draws, unsigned *dispatches, unsigned *copies, unsigned *clears);
/* tiled rendering (render::Options::gmem) for CB's passes from now on, on by default; and of what CB holds, the passes
 * drawn in GMEM and their tiles, and pass I's tiles as profiled (0: in sysmem, -1 past them) */
void hakios_metal_cb_set_gmem(hakios_metal_cb *cb, int on);
/* GMEM's refinements turned off (CommandBuffer::setGmemOff's bits: 1 whole area, 2 every attachment, 4 fetch barrier) */
void hakios_metal_cb_set_gmem_off(hakios_metal_cb *cb, unsigned bits);
/* GMEM or sysmem for each of CB's passes by what such passes cost the queue before (CommandBuffer::setGmemTuned) */
void hakios_metal_cb_set_gmem_tuned(hakios_metal_cb *cb, int on);
/* GMEM only for a pass whose like had at least DRAWS draws the last time (CommandBuffer::setGmemMinDraws); 0: off */
void hakios_metal_cb_set_gmem_min_draws(hakios_metal_cb *cb, unsigned draws);
/* each draw of CB's sysmem passes timed alone (CommandBuffer::setProfileDraws, before anything is recorded) and, once it
 * has completed, draw I's program name (valid while CB lives) and microseconds; the count is returned, -1 past them */
void hakios_metal_cb_set_profile_draws(hakios_metal_cb *cb, int on);
int hakios_metal_cb_draw_time(hakios_metal_cb *cb, unsigned i, const char **name, double *us);
/* draw I's pass (an index of hakios_metal_cb_pass_time's) and the pixels it can touch, X0 Y0 X1 Y1; -1 past them */
int hakios_metal_cb_draw_place(hakios_metal_cb *cb, unsigned i, unsigned *pass, unsigned area[4]);
void hakios_metal_cb_gmem_counts(hakios_metal_cb *cb, unsigned *passes, unsigned *tiles);
int hakios_metal_cb_pass_tiles(hakios_metal_cb *cb, unsigned i);
/* why CB's last pass refused for GMEM was refused ("" when none was); valid while CB lives */
const char *hakios_metal_cb_gmem_refusal(hakios_metal_cb *cb);
int hakios_metal_cb_refuse(hakios_metal_cb *cb, const char *why);
unsigned hakios_metal_cb_skipped(hakios_metal_cb *cb, const char **last);
/* -generateMipmapsForTexture: */
int hakios_metal_cb_generate_mipmaps(hakios_metal_cb *cb, hakios_metal_texture *texture);
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
/* dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:: as hakios_metal_cb_dispatch2
 * with the group counts three 32-bit words in ARGS at ARGS_OFFSET (a multiple of 4), read by the GPU when the dispatch
 * runs -- an earlier dispatch on the queue may write them. -1 with the reason, as the direct form. */
int hakios_metal_cb_dispatch_indirect(hakios_metal_cb *cb, hakios_metal_kernel *kernel, hakios_metal_buffer *args, uint64_t args_offset,
    const unsigned threads[3], hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count,
    hakios_metal_texture *const *textures, unsigned texture_count, hakios_metal_sampler *const *samplers, unsigned sampler_count);
/* the next draw on CB (hakios_metal_cb_draw2/3 or _draw_indexed) is indirect: its counts, starts and bases are the
 * words at OFFSET of ARGS when the GPU runs it (MTLDrawPrimitivesIndirectArguments, or the indexed form for an indexed
 * draw), and the call's own counts are not read; the index buffer, type and offset still are. That draw alone. */
void hakios_metal_cb_set_indirect(hakios_metal_cb *cb, hakios_metal_buffer *args, uint64_t offset);
/* OTHER's recorded work after CB's, as if recorded on CB (a parallel render encoder's sub-encoders, appended in the
 * order they were made): OTHER is left empty, to be freed uncommitted. -1 with the reason, the error OTHER ended in
 * among them (CB ends in it too). */
int hakios_metal_cb_append(hakios_metal_cb *cb, hakios_metal_cb *other);
/* setThreadgroupMemoryLength:atIndex: 0 for the dispatches that follow on CB: LENGTH bytes for a kernel's
 * [[threadgroup(0)]] argument, given past its static threadgroup arrays (a kernel without the argument ignores it) */
void hakios_metal_cb_set_threadgroup_length(hakios_metal_cb *cb, unsigned length);
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
