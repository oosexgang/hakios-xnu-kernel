/*
 * What a Metal driver needs from Mesa, and nothing else.
 *
 * Subsystem (1) made an AIR front end inside Mesa; subsystem (2) is making the Metal object
 * graph in userland/src/redwoodmetal.m. They meet here. The measurement that decided the
 * shape of this header is in docs/metal.md 10.12: Metal hands a driver one function's LLVM
 * module through -[_MTLFunction bitcodeData], and those bytes are -- checked over all eleven
 * functions of AV1SW.metallib -- exactly what this front end already reads. So the contract
 * is "AIR in, a pipeline out", and nothing in it mentions NIR, Gallium or ir3: the Metal
 * side must not grow a dependency on Mesa's headers, and this side must stay free to change
 * how it compiles.
 *
 * air_run (tools/mesa/air/air_run.c) does all of this already as a test program -- opens a
 * freedreno screen, translates a module, builds a compute state and launches a grid on the
 * phone's Adreno. This is that, as a library, so a driver can do it in process.
 *
 * Every call that can fail says why in a caller's buffer rather than returning a code to be
 * looked up: a driver has to put something in an NSError, and "the GPU did not finish" and
 * "the shader binds a texture, which is not implemented" are different sentences.
 */

#ifndef AIR_METAL_H
#define AIR_METAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
struct rwa_compute;

/* What Metal must be told about a compiled compute pipeline. The first three are protocol
 * requirements of MTLComputePipelineState that Metal's own base class answers with nothing
 * (docs/metal.md 10.8), so they have to come from whoever compiled the shader. */
struct rwa_compute_info {
	unsigned max_threads_per_group;    /* -maxTotalThreadsPerThreadgroup */
	unsigned thread_execution_width;   /* -threadExecutionWidth */
	unsigned static_shared_bytes;      /* -staticThreadgroupMemoryLength */
	unsigned buffers;                  /* how many buffer bindings the shader reads */
	unsigned textures;                 /* and textures, and samplers: a driver that cannot */
	unsigned samplers;                 /* bind them yet needs to know before it dispatches */

	/* Which of those buffer indices are ARGUMENT BUFFERS rather than buffer
	 * arguments, one bit each (`air.indirect_buffer`, see below). A driver needs
	 * this before it dispatches for the reason it needs vertex_stream_mask before
	 * it draws: Metal has ONE buffer table and an index in it is either a set or an
	 * argument, and which it is nobody but the shader knows. Appended, so every
	 * field a driver already fills keeps its offset.
	 */
	unsigned argbuf_mask;
};

/* The device. One per Metal device object; it owns the screen and one context.
 * Returns NULL and fills `why` when there is no GPU to open -- which on this port is the
 * ordinary case when the kernel was booted without gpu=1. */
struct rwa_device *rwa_device_open(char *why, size_t why_size);
void rwa_device_close(struct rwa_device *device);

/* What the driver should call the GPU, as Mesa knows it ("Adreno 7c+ Gen 3"). Never NULL. */
const char *rwa_device_name(struct rwa_device *device);

/* Memory a shader can read and write, and the processor can too. The mapping is write
 * combined, as the kernel makes it, so a driver has nothing to flush.
 *
 * It is also what an index buffer is (rwa_draw.index_buffer): Metal has one buffer
 * object and the application decides at the draw what the bytes in it are, so this makes
 * memory that can be any of them rather than asking what it will be used for. */
struct rwa_buffer *rwa_buffer_create(struct rwa_device *device, size_t size,
				     char *why, size_t why_size);
void *rwa_buffer_map(struct rwa_buffer *buffer);
size_t rwa_buffer_size(struct rwa_buffer *buffer);
void rwa_buffer_destroy(struct rwa_buffer *buffer);

/*
 * AIR in, a compiled pipeline out.
 *
 * `bitcode`/`size` is one function's module as Metal hands it over: the 20-byte 0x0B17C0DE
 * wrapper may be in front of it and is accepted either way. `entry` names the function
 * inside the module; it may be NULL when the module holds exactly one.
 *
 * On failure returns NULL and writes a sentence into `why` -- and the sentences matter, as
 * docs/air.md says: 135 of the corpus's modules are refused for reasons a driver should
 * repeat to the application rather than swallow.
 */
struct rwa_compute *rwa_compute_create(struct rwa_device *device, const void *bitcode,
				       size_t size, const char *entry,
				       struct rwa_compute_info *info, char *why, size_t why_size);
void rwa_compute_destroy(struct rwa_compute *compute);

/*
 * A texture. The shape comes from Metal's own descriptor, in Metal's own terms, because that
 * is what a driver has in its hand when newTextureWithDescriptor: is called; translating it
 * into Gallium's terms is this library's job, and a format it cannot translate must be
 * refused BY NAME rather than approximated -- docs/air.md is full of reasons to distrust a
 * pixel format that was almost right.
 *
 * `pixel_format` is MTLPixelFormat as Metal defines it. `type` is MTLTextureType. `usage` is
 * MTLTextureUsage, and the only bit that changes what the GPU is told is whether the shader
 * writes the texture.
 *
 * A DEPTH TEXTURE is made here and nowhere else, and it is a texture in exactly these
 * terms: one whose `pixel_format` is one of Metal's depth or stencil formats
 * (Depth16Unorm, Depth32Float, Stencil8, Depth32Float_Stencil8). Three things follow from
 * the format alone, and they are the whole of what makes it different:
 *
 *   - MTLTextureUsageRenderTarget on it means the depth/stencil attachment of a render
 *     pass and not a colour one, because Gallium has two bindings where Metal has one
 *     usage bit, and the two are not interchangeable: freedreno answers for a Z format
 *     only when asked about PIPE_BIND_DEPTH_STENCIL (a6xx/fd6_screen.cc, fd6_pipe2depth).
 *   - MTLTextureUsageShaderWrite on it is refused by name. Metal has no depth format a
 *     shader stores to, and Gallium has no image binding for one either, so a texture
 *     made that way could only be bound to something that would not write it.
 *   - its texels do not come back through rwa_texture_read, which refuses it by name and
 *     says so: a texel of Depth32Float_Stencil8 holds two values of two kinds, and the
 *     one answer rwa_texture_read is able to give would be whichever of them the caller
 *     did not ask for. rwa_depth_read, below, asks for an aspect.
 *
 * Everything else about it is a texture's: the width, the mip chain, the region of a
 * write, MTLTextureUsageShaderRead for a shader that samples it (which is what Metal's
 * depth2d<float> parameter is, and what Apple's own texturedFragment2DDepth declares).
 */
struct rwa_texture_shape {
	unsigned pixel_format;
	unsigned type;
	unsigned width, height, depth;
	unsigned array_length;
	unsigned mip_levels;
	unsigned sample_count;
	unsigned usage;
};

struct rwa_texture *rwa_texture_create(struct rwa_device *device,
				       const struct rwa_texture_shape *shape,
				       char *why, size_t why_size);
void rwa_texture_destroy(struct rwa_texture *texture);

/*
 * The same texture, over memory somebody else allocated: an IOSurface of CoreAnimation's.
 *
 * This is what a drawable is (docs/metal.md 10.46). -[CAMetalLayer nextDrawable] makes an
 * IOSurface, builds an ordinary MTLTextureDescriptor and sends a device exactly one message,
 * -newTextureWithDescriptor:iosurface:plane:, and the texture that comes back has to BE that
 * surface's memory rather than a copy of it -- because the point of a drawable is that the
 * display controller already scans out what the pass wrote. Without this call the driver
 * reads the finished attachment back to the processor and hands it to the kernel, which is
 * measured at 15.46 ms a frame and caps the whole port at 64.7 frames a second with nothing
 * drawn (docs/metal.md 10.43).
 *
 * `surface_id` is the IOSurface's ID, and it goes where Linux puts a dmabuf file descriptor:
 * the DRM shim of this port takes the ID there already (tools/mesa/redwood-drm: the kernel's
 * kRedwoodSurface answers it), so nothing new is asked of the layers underneath. `offset`
 * and `row_bytes` are the kernel's own answer about that surface and are NOT asked again
 * here, because freedreno takes both from the caller.
 *
 * Narrower than rwa_texture_create on purpose, and every narrowing is a refusal by name: a
 * plain 2D texture, one mip level, one sample, one slice. That is what a drawable is and all
 * that an imported row-major surface can be; a mip chain or an array would need a layout
 * agreement with whoever allocated the memory, and there is none.
 *
 * Ownership, which is the part that is easy to get wrong twice: the buffer the kernel makes
 * for that surface belongs to FREEDRENO's table of buffers from here on, and
 * rwa_texture_destroy gives it back. A caller that also takes the surface itself gets a
 * second buffer over the same memory on its own connection, which is a waste rather than a
 * fault -- but it must not then close freedreno's.
 *
 * NULL and a sentence on anything. A screen that imports nothing says so by name, which is
 * what the software screen on a build machine does.
 */
struct rwa_texture *rwa_texture_from_surface(struct rwa_device *device,
					     const struct rwa_texture_shape *shape,
					     int surface_id, unsigned offset,
					     unsigned row_bytes, char *why, size_t why_size);

/*
 * Pixels in and out, which is -replaceRegion:... and -getBytes:... on the Metal side. The
 * region is given in texels and `stride` in bytes per row of the caller's memory; a stride
 * of 0 means tightly packed. 0, or -1 and a sentence.
 */
int rwa_texture_write(struct rwa_texture *texture, unsigned level, unsigned layer,
		      const unsigned origin[3], const unsigned size[3], const void *bytes,
		      size_t stride, char *why, size_t why_size);
int rwa_texture_read(struct rwa_texture *texture, unsigned level, unsigned layer,
		     const unsigned origin[3], const unsigned size[3], void *bytes,
		     size_t stride, char *why, size_t why_size);

/*
 * MTLBlitCommandEncoder's copies, done by the GPU in the order of the draws around them: a
 * region of one texture into another (the same format, or one of the same texel size), a
 * range of one buffer into another, and a range of a buffer filled with one byte. 0, or -1
 * and a sentence.
 */
int rwa_texture_copy(struct rwa_texture *dst, unsigned dst_level, unsigned dst_slice,
		     const unsigned dst_origin[3], struct rwa_texture *src, unsigned src_level,
		     unsigned src_slice, const unsigned src_origin[3], const unsigned size[3],
		     char *why, size_t why_size);
int rwa_render_clear(struct rwa_device *device, struct rwa_texture *const *color,
		     unsigned color_count, const float *clear_color, struct rwa_texture *depth,
		     const float *clear_depth, const unsigned *clear_stencil, char *why,
		     size_t why_size);
int rwa_buffer_copy(struct rwa_buffer *dst, size_t dst_offset, struct rwa_buffer *src,
		    size_t src_offset, size_t size, char *why, size_t why_size);
/* -generateMipmapsForTexture:: every level below the base, filtered from the one above it */
int rwa_texture_mipmaps(struct rwa_texture *texture, char *why, size_t why_size);
int rwa_buffer_fill(struct rwa_buffer *buffer, size_t offset, size_t size, uint8_t value,
		    char *why, size_t why_size);

/*
 * What a depth/stencil texture holds, by aspect, which is the reading rwa_texture_read
 * cannot give.
 *
 * `depth` takes one float per texel and `stencil` one byte per texel, both tightly
 * packed in the region's own row-major order, and either may be NULL. An aspect the
 * format does not have is refused BY NAME rather than filled with something: asking the
 * stencil of a Depth32Float texture is a caller that believes it allocated a different
 * texture, and a buffer of zeros would confirm that belief.
 *
 * The depth comes back as a float in [0, 1] whatever the format stores -- Depth16Unorm's
 * 65535 is 1.0 here -- because that is the number a depth test was made of and the only
 * one two formats can be compared in. The decoding is Mesa's own for the format
 * (util_format's unpack_z_float / unpack_s_8uint), not arithmetic written here, for the
 * reason the colour formats are a table of identities: a depth format packed by hand is
 * a wrong number that still looks like a depth.
 *
 * 0, or -1 and a sentence.
 */
int rwa_depth_read(struct rwa_texture *texture, unsigned level, unsigned layer,
		   const unsigned origin[3], const unsigned size[3], float *depth,
		   uint8_t *stencil, char *why, size_t why_size);

/*
 * What textures a module declares, slot by slot, in Metal's own terms.
 *
 * Everything above takes a texture's shape FROM the caller, because a driver has the
 * application's MTLTextureDescriptor in hand and never has to ask. Two callers do have
 * to ask, and neither has a descriptor to read it from:
 *
 *   - Metal's own pipeline reflection. -newRenderPipelineStateWithDescriptor:options:
 *     reflection: has to report MTLArgument.textureType and .textureDataType for every
 *     binding, and only whoever read the shader knows them.
 *   - a test standing in for an application. userland/src/metal_drv_test.m --draw-all
 *     sweeps every fragment function of a .metallib, and with one kind and one format
 *     bound for the whole library 13 of Apple's 271 were refused -- by this library, and
 *     rightly: they declare a texture3d, a texture2d<int> or a multisample texture where
 *     a texture2d<float> was bound. Thirteen shaders went unmeasured for a reason that
 *     has nothing to do with the hardware.
 *
 * What comes back is read off the same slots rwa_dispatch and rwa_render_draw judge a
 * binding by, through the same two helpers, so the two cannot drift: a caller that binds
 * what this reports is not refused for the shape, and a caller that binds something else
 * is refused BY NAME -- which is what keeps a deliberate mismatch testable.
 */
enum rwa_element {
	RWA_ELEMENT_UNKNOWN = 0,  /* the module states none: any format is accepted */
	RWA_ELEMENT_FLOAT,        /* float, half: a floating-point or normalised format */
	RWA_ELEMENT_INT,          /* int, short: a signed integer format */
	RWA_ELEMENT_UINT,         /* uint, ushort: an unsigned integer format */
};

struct rwa_texture_decl {
	unsigned index;          /* Metal's [[texture(n)]] */
	unsigned type;           /* MTLTextureType, as rwa_texture_shape.type takes it */
	unsigned type_known;     /* 0 when the shader indexes this slot with a computed
				  * value and no instruction declared a kind: `type` says
				  * nothing then, and any texture is accepted */
	unsigned usage;          /* the MTLTextureUsage bits the shader needs. Honoured and
				  * not advisory: a texture made without ShaderWrite is
				  * refused for a shader that writes it */
	unsigned element;        /* enum rwa_element */
	unsigned element_bits;   /* 32 for float/int/uint, 16 for half/short/ushort; 0 when
				  * Apple's metadata carries no type name for the slot and
				  * the element type came from the instructions, which say
				  * the class and not the width */
	char metal_type[64];     /* air.arg_type_name as Apple wrote it, e.g.
				  * "texture2d<half, write>"; "" when the module has none.
				  * Copied, not pointed at: this call frees everything it
				  * read before it returns */
};

/*
 * Fills `out` with one entry per texture slot the module declares, in slot order, and
 * writes how many into *count. Returns 0, or -1 with a sentence -- including when there
 * are more than `max` or when a slot is past the sixteen this library binds, because a
 * caller that quietly bound the first few would be dispatching a shader with a slot
 * nothing was bound to, which is the one outcome worse than an error.
 *
 * `bitcode`/`size`/`entry` are rwa_compute_create's, wrapper or not, and every stage is
 * accepted: a fragment shader's textures are as much a declaration as a kernel's. The
 * module is translated to answer, so a module this library cannot translate is refused
 * here with the same sentence rwa_compute_create would give.
 */
int rwa_module_textures(struct rwa_device *device, const void *bitcode, size_t size,
			const char *entry, struct rwa_texture_decl *out, unsigned max,
			unsigned *count, char *why, size_t why_size);

/*
 * A sampler, as Metal describes one. These fields are deliberately the same ones the front
 * end already decodes out of a shader's `constexpr sampler` word (docs/air.md section 3.3),
 * whose behaviour was checked against Apple's own rules in 15 cases: a driver that builds a
 * sampler from MTLSamplerDescriptor and a shader that declares its own must end up at the
 * same place, and keeping one vocabulary is what makes that checkable.
 *
 * The address modes are Metal's MTLSamplerAddressMode, the filters MTLSamplerMinMagFilter,
 * the mip filter MTLSamplerMipFilter and the compare function MTLCompareFunction.
 *
 * NOTHING SAYS HERE WHETHER A COMPARISON HAPPENS, and that is Metal's own design, not an
 * omission: sample() and sample_compare() are different instructions over one sampler,
 * so the descriptor only says WHICH comparison and the shader says whether. The library
 * builds both Gallium states a sampler needs for that -- Gallium puts the decision in
 * the sampler -- and binds the comparing one into exactly the slots the translated
 * shader samples with a comparison. A driver has nothing to do for it, and this struct
 * did not have to grow for it either.
 *
 * compare_function is MTLCompareFunction's eight and is not the shader word's nine: that
 * word has a `none` MTLCompareFunction has no name for, with `never` at 8 and in a
 * different order throughout. The translation is a table in air_metal.c's
 * sampler_state_from_air, with the three sources it was read from.
 */
struct rwa_sampler_state {
	unsigned address[3];       /* s, t, r */
	unsigned min_filter, mag_filter, mip_filter;
	unsigned compare_function;         /* MTLCompareFunction: 0 Never .. 7 Always */
	unsigned max_anisotropy;
	unsigned normalized_coordinates;   /* 0: in pixels, as Metal's unnormalized sampler */
	float lod_min, lod_max;
};

struct rwa_sampler *rwa_sampler_create(struct rwa_device *device,
				       const struct rwa_sampler_state *state,
				       char *why, size_t why_size);
void rwa_sampler_destroy(struct rwa_sampler *sampler);

/*
 * ---------------------------------------------------------------------------------------
 * ARGUMENT BUFFERS: MTLArgumentEncoder, and the one thing to read before the rest.
 *
 * THERE IS NO ARGUMENT BUFFER TO WRITE. Metal's encoder writes an 8-byte handle per
 * member into a plain buffer -- 8 bytes per member is what the metadata says, a
 * two-member set declaring `air.arg_type_size 16` and a ten-member set 80 -- and those
 * handles are an Apple GPU's descriptors. They mean nothing to a Gallium driver, which
 * binds by index; and this frontend has already resolved every member of every set to a
 * binding while translating (docs/air.md 8.2), so the handles are not needed either.
 *
 * So the encoder cannot be a writer here. It is a RECORDER: -setBuffer:offset:atIndex:,
 * -setTexture:atIndex: and -setSamplerState:atIndex: record (resource, Metal id) against
 * (destination buffer, destination offset), and at the draw or dispatch this library
 * binds each member of each set the pipeline's shaders declare to the Gallium slot the
 * frontend gave it. THE BUFFER'S CONTENTS ARE NEVER READ.
 *
 * Three consequences, each of which is a decision rather than a side effect:
 *
 *   - The records live WITH THE DESTINATION BUFFER and are keyed by the offset, because
 *     that is what Metal's own object model allows: one buffer holds several encodings at
 *     several offsets, and each is reused across many draws. A record keyed by the
 *     encoder would be wrong twice over -- an encoder is reset onto another buffer and
 *     another offset and keeps encoding, and an application may destroy it and draw with
 *     what it encoded for the rest of the frame.
 *   - An encoding is not cleared by -setArgumentBuffer:offset:, and that is Metal's
 *     behaviour and not a shortcut: the bytes an earlier encoding wrote into that buffer
 *     at that offset are still there, so a second encoding that sets three of five
 *     members leaves the other two as they were. A record that vanished would refuse a
 *     draw Metal would have made.
 *   - An argument buffer the application did NOT fill through an encoder is REFUSED BY
 *     NAME, and this is the most important refusal in this header. A buffer written by
 *     the processor, by a compute kernel or by a blit holds handles this library cannot
 *     interpret, and nothing distinguishes it from a correctly encoded one except the
 *     presence of a record. There is no guess available that is not "draw over whatever
 *     resource index 0 happens to be".
 *
 * `useResource:`/`useHeap:` are therefore no-ops on this driver, and saying so out loud
 * is better than implementing nothing in silence: every member is bound explicitly, so
 * there is no residency for the application to arrange.
 */

/* RE4's largest set has 43 members (docs/air.md 8.2) and Apple's own limit on an
 * argument buffer's entries is higher than this; what this number bounds is the record,
 * and a set with more members than this is refused by name rather than recorded in part.
 */
#define RWA_MAX_ARGBUF_MEMBERS 64

enum rwa_argbuf_kind {
	RWA_ARGBUF_BUFFER = 0,     /* `constant` or `device`: which it is, the shader decides */
	RWA_ARGBUF_TEXTURE = 1,
	RWA_ARGBUF_SAMPLER = 2,
};

/*
 * One member of one set, as the module declares it, in both namings -- and it takes two
 * because neither can be derived from the other:
 *
 *   - `id` is Metal's `[[id(n)]]`, which is what the APPLICATION passes to
 *     -setTexture:atIndex: and what this library's record is keyed by. It is per set, so
 *     set 0's member 0 and set 1's member 0 both say 0.
 *   - `index` is the flat per-kind binding the FRONTEND allocated, which is what the GPU
 *     is programmed with. A driver never has to use it: it is here so that the mapping
 *     can be read back and checked rather than trusted.
 */
struct rwa_argbuf_member {
	unsigned id;
	unsigned kind;             /* enum rwa_argbuf_kind */
	unsigned index;
	unsigned write;            /* 1 when the shader writes this member */
	char name[48];             /* air.arg_name, as Apple's compiler wrote it; "" if none */
};

/*
 * A set, and the two numbers an application allocates its buffer from.
 *
 * `encoded_length` and `alignment` are -[MTLArgumentEncoder encodedLength] and
 * -alignment, and they must be APPLE'S OWN NUMBERS rather than this library's idea of
 * what it needs: the application calls -newBufferWithLength:encodedLength and then
 * encodes at multiples of it, so a number of ours that was too small would have the
 * application overlapping its own encodings, and one too large would waste memory in a
 * way that is invisible until something runs out. They are 8 bytes per member the set
 * DECLARES -- used or not, which is why the member list below is never compacted -- and
 * 8 bytes, which is what `air.arg_type_size` and `air.arg_type_align_size` say in the
 * metadata of every one of Resident Evil 4's 7 857 sets (docs/air.md 8.2.1).
 */
struct rwa_argbuf_layout {
	unsigned metal_index;      /* the [[buffer(n)]] the SET is bound at */
	unsigned encoded_length;
	unsigned alignment;
	unsigned member_count;
	struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];
};

/*
 * What argument buffers a module declares, set by set, in Metal's own terms:
 * rwa_module_textures' shape, for rwa_module_textures' reason. A driver asked for
 * -[MTLFunction newArgumentEncoderWithBufferIndex:] has a function's bitcode and nothing
 * else -- no pipeline yet, and no descriptor to read the layout from -- and the numbers
 * it must answer with are in the module.
 *
 * Returns 0 and fills `out` with the sets in the order the module declares them, or -1
 * and a sentence. A module with no argument buffer is 0 sets and not an error.
 *
 * REFUSED BY NAME here rather than at the draw, because each would otherwise be a
 * length the application allocates against: a set with more members than this library
 * records; two sets at one Metal buffer index; a member whose id is past the number of
 * members the set declares (Apple's encodedLength is over the highest id, and a set with
 * a gap in its ids would be allocated too short); and anything the module cannot be
 * translated at all for, with the sentence rwa_compute_create would give.
 */
int rwa_module_argbufs(struct rwa_device *device, const void *bitcode, size_t size,
		       const char *entry, struct rwa_argbuf_layout *out, unsigned max,
		       unsigned *count, char *why, size_t why_size);

/*
 * THE ENCODER, which is these three calls and no object: an encoder's whole state is
 * (destination buffer, destination offset), which the caller passes to each one, so
 * -setArgumentBuffer:offset: is a driver-side assignment and nothing happens here until
 * a member is set.
 *
 * `id` is Metal's `[[id(n)]]` as the application gave it. The KIND is the call, as it is
 * in Metal, and a member set with the wrong one is refused at the draw by name -- not
 * here, where the set this buffer is being encoded for is not known: one buffer may be
 * encoded for a set declared by a shader this library has never seen.
 *
 * `offset` on a buffer member is Metal's own `offset:` and is honoured, by the same rule
 * a DIRECT buffer argument's offset follows -- struct rwa_buffer_bind has it, and the
 * short of it is that a `constant` binding is served at any offset and a `device` one at
 * an offset this GPU cannot fetch from is refused by name. It is the ordinary case of an
 * application with one allocation and many blocks in it, and it is checked at the draw
 * rather than here, where the set this buffer is being encoded for is not known.
 *
 * 0, or -1 and a sentence: a destination that is not a buffer, an id past
 * RWA_MAX_ARGBUF_MEMBERS, a resource of another device, or an offset past the end of the
 * buffer being encoded.
 *
 * The records hold the resources, and keeping them alive while the encoding is drawn
 * with is the application's business, exactly as it is in Metal -- an argument buffer
 * there holds raw handles, which is what -useResource: exists for. What this library
 * does guarantee is that a resource destroyed after it was encoded is a REFUSAL at the
 * next draw and not a read of freed memory: rwa_buffer_destroy, rwa_texture_destroy and
 * rwa_sampler_destroy scrub the member they were encoded into and the draw then names
 * it.
 */
int rwa_argbuf_encode_buffer(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
			     struct rwa_buffer *buffer, size_t offset, char *why,
			     size_t why_size);
int rwa_argbuf_encode_texture(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
			      struct rwa_texture *texture, char *why, size_t why_size);
int rwa_argbuf_encode_sampler(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
			      struct rwa_sampler *sampler, char *why, size_t why_size);

/*
 * How many members are recorded at (dest, dest_offset), which is what -[MTLArgumentEncoder
 * constantDataAtIndex:] and a test need and what the refusal at the draw turns on: zero
 * is "this buffer was never encoded", and nothing else about a buffer says that. Not part
 * of any Metal protocol.
 */
unsigned rwa_argbuf_encoded_members(const struct rwa_buffer *dest, size_t dest_offset);

/*
 * METAL'S BUFFER TABLE FOR ONE STAGE: what is bound at each index AND AT WHAT OFFSET.
 *
 * `rwa_bindings.buffers` says which buffer is at index n; this says where in it, which
 * rwa_bindings has no room for. One array covers both of the things an offset means,
 * because Metal has one table and one -setFragmentBuffer:offset:atIndex: behind both:
 *
 *   - for an ARGUMENT BUFFER, the offset is WHICH ENCODING in that buffer the draw is to
 *     read: one allocation holds many sets at multiples of encodedLength, and the records
 *     this library binds from are keyed by it.
 *   - for a BUFFER ARGUMENT, it is which block of a buffer the shader's `constant` or
 *     `device` pointer starts at: the ordinary case of an application that packs several
 *     uniform blocks into one allocation and moves the offset per draw. The window server
 *     of this port does exactly that -- 55 bindings of one boot arrived as
 *     "fragment buffer 2 at offset 1164" -- and until this field existed each of them was
 *     a refused binding.
 *
 * What is done with a buffer argument's offset, which is not the same on both bindings
 * because Gallium's two bindings are not the same:
 *
 *   - a `constant` member or argument (a UBO) is ALWAYS served. An offset the GPU can
 *     fetch from directly is passed as the binding's own offset and nothing is copied;
 *     one it cannot -- a6xx wants multiples of 64 and softpipe 16, and Metal allows any
 *     multiple of 4 -- is bound as a `user_buffer`, which both back ends handle by
 *     uploading the bytes themselves (freedreno's upload_user_buffer aligns to 64,
 *     softpipe wraps the pointer). What is copied is `air.buffer_size` where the metadata
 *     carries one, which is all the shader can read, and the rest of the buffer
 *     otherwise.
 *   - a `device` member or argument (an SSBO) at an offset the GPU cannot fetch from is
 *     REFUSED BY NAME, and the reason is not symmetry: the shader may WRITE it, and the
 *     copy that serves a UBO would take the writes with it when it was thrown away.
 *     Gallium has no user_buffer on a shader buffer for the same reason.
 *
 * It is a SECOND array beside `rwa_bindings.buffers` rather than a field in it, and the
 * reason is this project's own scar tissue: `rwa_bindings` is embedded BY VALUE in
 * rwa_draw, twice, so a field added to it moves `primitive`, `vertex_start` and every
 * index field after them for a driver built against the other header -- which is exactly
 * how a primitive type came back as 1871292816 once. Nothing in rwa_bindings moves for
 * this feature.
 *
 * A driver fills this for every index it binds a buffer at, and `rwa_bindings.buffers`
 * exactly as before. Where both name a buffer they must name the SAME one: two tables
 * that disagree about index n are a driver filling one of them from the wrong place, and
 * that is refused by name rather than resolved in favour of either.
 */
struct rwa_buffer_bind {
	struct rwa_buffer *buffer;   /* NULL: nothing bound at this index */
	size_t offset;               /* Metal's own `offset:` */
};

/*
 * What a dispatch is given. Each array is bound at indices 0..count-1, which is the
 * convention the front end already uses for a kernel's arguments.
 *
 * A shader that declares a texture the caller did not bind must not be dispatched: it would
 * read a slot nothing was bound to, and the answer would look like a result. This library
 * refuses that, by name, which is why rwa_compute_info reports the counts a shader wants.
 */
struct rwa_bindings {
	struct rwa_buffer *const *buffers;
	unsigned buffer_count;
	struct rwa_texture *const *textures;
	unsigned texture_count;
	struct rwa_sampler *const *samplers;
	unsigned sampler_count;
};

/*
 * One dispatch. `groups` is the grid in threadgroups and `group_size` the threads in one of
 * them, both as Metal counts them.
 *
 * Returns 0, or -1 with a sentence in `why`. A dispatch that the GPU does not finish is a
 * failure here and says so: air_run learned that the hard way (docs/air.md section 5) --
 * before it waited on a fence it printed its own fill pattern as a result, and 21 of 47
 * kernels in one run were vacuous.
 */
int rwa_dispatch(struct rwa_compute *compute, const struct rwa_bindings *bindings,
		 const unsigned groups[3], const unsigned group_size[3],
		 char *why, size_t why_size);

/*
 * The same dispatch, with the kernel's BUFFER TABLE: `table` is indexed by Metal's own
 * buffer index, as rwa_bindings.buffers is, and covers `table_count` slots from 0. It is
 * what carries an argument buffer's encoding offset and a buffer argument's own offset --
 * see struct rwa_buffer_bind, which has the whole of what each means.
 *
 * rwa_dispatch is this with no table at all: every offset is then zero, and a kernel that
 * declares an argument buffer is refused through it by name.
 *
 * It is a second entry point and not two more fields in rwa_bindings, and the reason is
 * the one rwa_buffer_bind gives: rwa_bindings is embedded by value in rwa_draw, so
 * growing it would move half of that struct for every driver built against the other
 * header. A new symbol, by contrast, is simply absent in an older library -- which the
 * driver's own dlsym already treats as a library too old to use.
 */
int rwa_dispatch_buffers(struct rwa_compute *compute, const struct rwa_bindings *bindings,
			 const struct rwa_buffer_bind *table, unsigned table_count,
			 const unsigned groups[3], const unsigned group_size[3],
			 char *why, size_t why_size);

/* Wait for everything dispatched so far, with a real fence. 0, or -1 and a sentence. */
int rwa_finish(struct rwa_device *device, char *why, size_t why_size);
/* where the draws' time went since the last call, in ns: validation, binding checks, fixed
 * state, the stages' resources, draw_vbo, unbinding; then the number of draws */
void rwa_draw_profile(uint64_t out[7]);

/* The work so far sent off without waiting, and the number of its last job; a wait for it. */
uint64_t rwa_flush_seq(struct rwa_device *device);
int rwa_wait_seq(uint64_t seq, int64_t timeout_ns);
void rwa_submit_profile(uint64_t out[5]);
int rwa_surface_seq(uint32_t surface, uint64_t seq);

/*
 * ---------------------------------------------------------------------------------------
 * Drawing.
 *
 * The same shape of contract as compute, and for the same reason: AIR in, a pipeline out,
 * and nothing of NIR or Gallium in the caller's view.
 *
 * One thing here is not symmetrical, and it is deliberate. `vertex_bitcode` may be NULL,
 * and then this library supplies a pass-through vertex stage covering the whole attachment.
 * That is not a convenience: it is the road that already has evidence behind it. air_run
 * draws every fragment shader of the corpus that way -- a generated quad vertex shader plus
 * the real fragment shader -- and 255 of them have been drawn and compared pixel by pixel
 * (docs/air.md). A driver asked for a pipeline whose vertex function is one of Apple's own
 * three vertex shaders passes both modules instead, and then nothing is generated.
 */
struct rwa_render;
struct rwa_depth_stencil;

/*
 * Depth and stencil, which are ONE attachment of two aspects and not two attachments,
 * and both sides of the translation force that:
 *
 *   - Metal keeps two descriptors, MTLRenderPassDescriptor.depthAttachment and
 *     .stencilAttachment, and they are two views of one resource: for
 *     MTLPixelFormatDepth32Float_Stencil8 an application sets THE SAME texture on both,
 *     because there is one texture. The two pipeline properties are the same shape --
 *     depthAttachmentPixelFormat and stencilAttachmentPixelFormat, both set to the
 *     combined format. (Read off the cache: both attachment descriptors are backed by
 *     one MTLRenderPassAttachmentDescriptorPrivate that holds clearColor, clearDepth AND
 *     clearStencil together, and both -setTexture: make the same
 *     conformsToProtocol:MTLTextureImplementation check as the colour one --
 *     0x181a4245c and 0x181a40e54.)
 *   - Gallium has one `zsbuf` in a framebuffer and one resource behind it, and
 *     PIPE_FORMAT_Z24_UNORM_S8_UINT or PIPE_FORMAT_Z32_FLOAT_S8X24_UINT is one format
 *     with two aspects, cleared through two bits of one mask.
 *
 * So `depth_format` and `stencil_format` are both here, as the pipeline descriptor has
 * them, and this library requires them to name one texture: equal, or one of them zero.
 * A pipeline that gives two different formats is refused by name, because what it is
 * asking for -- a separate depth surface and stencil surface -- is a thing neither
 * Metal's own descriptor nor a Gallium framebuffer can hold, and guessing which of the
 * two to honour would silently drop the other.
 */
/*
 * THE VERTEX ATTRIBUTE LAYOUT: MTLVertexDescriptor, and the one thing this contract was
 * missing when the phone's own window server first reached it. ONE of its render
 * pipeline specifications was refused by name, 80 times over 42 lives of backboardd --
 * "reads vertex attributes ([[stage_in]]), and this
 * contract carries no vertex attribute layout (rwa_render_shape has no formats, strides
 * or offsets)" -- because the shader's half of that match is all AIR carries
 * (docs/air.md 3.1: Apple's compiler flattens the stage-in struct and the format, the
 * offset, the stride and the buffer are nowhere in the module) and the API's half had
 * nowhere to arrive.
 *
 * It is TWO arrays because Metal's descriptor is two, and each is indexed by Metal's own
 * index rather than packed, because Metal's are:
 *
 *   - `attribute[n]` is MTLVertexAttributeDescriptor at `vertexDescriptor.attributes[n]`,
 *     where n is the shader's [[attribute(n)]]. The indices a real shader uses are not
 *     contiguous -- QuartzCore's std_vert0_lpf declares 0, 1 and 3 -- so a packed array
 *     with a count would need a second field to say which index each entry is, and that
 *     is a field to disagree with the entry.
 *   - `layout[k]` is MTLVertexBufferLayoutDescriptor at `vertexDescriptor.layouts[k]`,
 *     where k is the buffer index an attribute names and the index the stream is bound
 *     at (rwa_draw.vertex_streams).
 *
 * There is no count for either, for the same reason Metal has none: an attribute is
 * configured when its `format` is not MTLVertexFormatInvalid (0), which is exactly what
 * a freshly made MTLVertexDescriptor holds everywhere, and a layout is read only when an
 * attribute names its index. So a zeroed rwa_render_shape is a pipeline with no vertex
 * layout at all, which is every pipeline this library drew before now.
 *
 * In Gallium this becomes one `pipe_vertex_element` per attribute the shader FETCHES, in
 * driver_location order, plus one `pipe_vertex_buffer` per buffer index at the draw --
 * see rwa_draw.vertex_streams, and air_metal.c for why that order is the only one that
 * feeds the right attribute.
 */
struct rwa_vertex_attribute {
	unsigned format;         /* MTLVertexFormat; 0 (Invalid) is "not configured" */
	unsigned offset;         /* bytes from the start of the element, as Metal's is */
	unsigned buffer_index;   /* which stream it is fetched from: layout[] and
				  * rwa_draw.vertex_streams are indexed by this */
};

struct rwa_vertex_layout {
	unsigned stride;         /* bytes between one vertex's element and the next */
	unsigned step_function;  /* MTLVertexStepFunction: Constant 0, PerVertex 1,
				  * PerInstance 2, PerPatch 3, PerPatchControlPoint 4 */
	/* Metal's stepRate: with PerInstance, how many consecutive instances share one
	 * element, which is Gallium's instance_divisor exactly. ZERO IS READ AS ONE,
	 * because Metal's own default is 1 and a caller's struct filled in only as far
	 * as it cared about leaves this at 0 -- the same reading rwa_draw.instance_count
	 * gets, and for the same reason. With Constant there is no rate to honour (every
	 * vertex and every instance reads element 0) and this field is not read at all.
	 */
	unsigned step_rate;
};

/* Metal's own limits, from Apple's feature tables: 31 attributes and 31 buffer indices.
 * The modules corroborate the buffer half on this device -- every one of them carries
 * `air.max_device_buffers i32 31` -- and Gallium has room for both (PIPE_MAX_ATTRIBS is
 * 32 for elements and for buffers alike). What the SHADER side can use is narrower and
 * is the frontend's business: NIR has sixteen VERT_ATTRIB_GENERIC slots, so an
 * attribute index of 16 or more is refused when the module is translated, long before
 * one of these entries is read.
 */
#define RWA_MAX_VERTEX_ATTRIBUTES 31
#define RWA_MAX_VERTEX_BUFFERS    31

/*
 * THE BLEND STATE: MTLRenderPipelineColorAttachmentDescriptor's other half, and the
 * second thing this contract did not have at all.
 *
 * What the absence looked like. iOS's own window server composites the home screen
 * through this library, and CoreAnimation composites its layers with PREMULTIPLIED
 * ALPHA: a layer's texel holds colour already multiplied by its own coverage, and the
 * pipeline is asked for source-over -- source times one, plus destination times one
 * minus the source's alpha. With no blend state in the shape, every pipeline this
 * library made was
 *
 *     create_blend_state(pipe, &(struct pipe_blend_state){ .rt[0].colormask = RGBA })
 *
 * which is blending OFF: the source replaces the destination. A label layer whose
 * background is transparent black then lands as OPAQUE BLACK, which is exactly what the
 * phone showed -- every icon label in a black box, the Search pill a black rectangle,
 * the dock edged in dark red, the widgets flat white or black slabs. Not a refusal and
 * not a crash: a picture, wrong in a way only a photograph could report.
 *
 * WHAT IS CARRIED, and it is Metal's own eight properties per colour attachment, as
 * Metal's own numbers. The library maps each enumeration BY TABLE, refuses what it
 * cannot serve BY NAME, and derives nothing from a cast -- MTLBlendFactor and
 * pipe_blendfactor agree on exactly two of nineteen values, MTLColorWriteMask's bits run
 * in the OPPOSITE ORDER to Gallium's colormask, and air_metal.c has both tables with the
 * arithmetic of every disagreement written out.
 *
 * WHAT THE COMPOSITOR ACTUALLY ASKS FOR, measured and not assumed. CoreAnimation builds
 * every one of its pipelines in one function
 * (CA::OGL::MetalContext::create_pipeline_state, 0x188378090 of this device's
 * QuartzCore), out of a 55-entry table of blend modes at 0x18855c092, and what it sends
 * is readable instruction by instruction:
 *
 *     mode 3  "sov"  blendingEnabled YES, One / OneMinusSourceAlpha, Add, both halves,
 *                    and writeMask NEVER SENT -- so All, Apple's default
 *     mode 9  "dot"  Zero / OneMinusSourceAlpha      mode 12 "add"  One / One
 *     mode 15 "mul"  DestinationColor / Zero         mode 8  "din"  Zero / SourceAlpha
 *     mode 6  "sap"  DestinationAlpha / OneMinusSourceAlpha
 *     mode 13 "sbs"  One / One, ReverseSubtract      mode 16 "scn"  One / OneMinusSourceColor
 *
 * Of the 1079 pipelines in this device's own precompiled archive
 * (QuartzCore.framework/default.pipelinelib/pipelines.desc, whose names carry the mode),
 * 472 are source-over, 33 "dot", 31 "add", 19 "mul", 13 "din", 3 "sap", 2 "sbs", 1
 * "scn", 1 "ldg" -- and NOT ONE uses a factor this library refuses. The refusals below
 * cost the home screen nothing; they are there for the applications.
 *
 * AND 504 OF THOSE 1079 CARRY NO BLEND STATE AT ALL, which is the trap. Those layers
 * blend INSIDE THE FRAGMENT SHADER -- the mode is lifted into the fixed-function blender
 * only when it is one of the first 26 and some other conditions hold
 * (Pipeline::Spec::lift_blend_function, 0x18834859c), and the twenty-eight Photoshop-like
 * modes from "pld" onwards never are -- so their descriptor arrives UNTOUCHED, at
 * Apple's own defaults. Those defaults were read off
 * -[MTLRenderPipelineColorAttachmentDescriptorInternal init] (0x181a2e9a8), which writes
 * the word 0x78001080:
 *
 *     blendingEnabled NO, both operations Add, sourceRGB One, sourceAlpha One,
 *     destinationRGB Zero, destinationAlpha Zero, writeMask All (0xf)
 *
 * SO: THE SOURCE FACTOR OF A PIPELINE THAT DOES NOT BLEND IS ONE, AND ITS WRITE MASK IS
 * 0xF. A driver that inferred "this descriptor carries blending" from any field being
 * non-zero would turn the blender on for 504 of 1079 pipelines that have already blended
 * in their shader, and double-blend the home screen. `blendingEnabled` is the only field
 * that says, and when it is false this library reads NOTHING ELSE of the entry but the
 * write mask -- which is Metal's own rule.
 *
 * `color_blend_count` IS NOT DECORATION, and it is the one field here that is not
 * Metal's. It says how many entries of `color_blend[]` the caller filled in, and ZERO
 * MEANS NONE AT ALL -- blending off, all four channels written, which is the state every
 * draw this library ever made used and what a shape built before this field existed
 * still asks for. It has to exist because every field of a blend descriptor has a
 * meaningful zero and NOT ONE of those zeros is Metal's default, as the word above
 * shows:
 *
 *     blendingEnabled    false     -- which IS Metal's default, so zero is safe here
 *     writeMask          None (0)  -- Metal's default is All (0xf), and a zeroed entry
 *                                     would write NOTHING: a pipeline that draws
 *                                     nothing at all, and no refusal to say why
 *     the four factors    Zero     -- and Metal's defaults are One, One, Zero, Zero
 *     the two operations  Add      -- the one field whose zero IS the default
 *
 * So there is no value of `write_mask` that can mean "the caller never said", and the
 * project has already paid once for reading a meaningful zero as an absence: compare
 * function 8 is `never`, which is what Apple's own metal_stdlib writes into an inline
 * sampler, and a test for "not zero" refused 653 of 653 draws. The count is how this
 * contract tells None from never-said, and it is one number about the array rather than
 * a second opinion inside each entry.
 *
 * THE ENTRIES REALLY DO DIFFER PER ATTACHMENT on this device, so independent blending is
 * not a hypothetical: create_pipeline_state's own multiple-attachment loop sends
 * `setWriteMask: 0` to every attachment that is not the primary one and sends
 * `setBlendingEnabled:` to none of them, while the primary one gets the blend. That is
 * exactly the case Gallium's `independent_blend_enable` exists for, and exactly the case
 * where leaving it clear makes rt[0] govern them all.
 *
 * A caller that gives FEWER entries than `color_count` attachments is refused by name:
 * filling in attachment 0 and leaving attachment 1 to the zeros would turn the second
 * attachment's blending silently off, which is the failure this whole field exists to
 * end.
 *
 * It is in the SHAPE because that is where Metal puts it -- blend state is compiled into
 * the pipeline, not set on the encoder -- with ONE exception that is therefore refused:
 * the four factors that read Metal's BLEND COLOUR
 * (-setBlendColorRed:green:blue:alpha:) are encoder state, this contract has nowhere to
 * carry it, and the driver's own setter is a stub. See the refusal list at
 * rwa_render_create.
 */
struct rwa_color_blend {
	unsigned blending_enabled;               /* isBlendingEnabled */
	unsigned rgb_blend_operation;            /* MTLBlendOperation */
	unsigned alpha_blend_operation;          /* MTLBlendOperation */
	unsigned source_rgb_blend_factor;        /* MTLBlendFactor */
	unsigned destination_rgb_blend_factor;   /* MTLBlendFactor */
	unsigned source_alpha_blend_factor;      /* MTLBlendFactor */
	unsigned destination_alpha_blend_factor; /* MTLBlendFactor */
	/* MTLColorWriteMask, which is FOUR BITS IN THE OTHER ORDER FROM GALLIUM'S: Metal
	 * has Red at 1 << 3 and Alpha at 1 << 0 where PIPE_MASK_R is 0x1 and PIPE_MASK_A
	 * is 0x8. All (0xf) and None (0) are the same number either way, and so are
	 * green|blue and red|alpha, so twelve of the sixteen masks are exchanged by a
	 * cast and four are not -- which is why a test that only ever asks for All would
	 * pass with the bits backwards. Anything above 0xf is refused by name.
	 */
	unsigned write_mask;
};

struct rwa_render_shape {
	unsigned color_format[8];      /* MTLPixelFormat per colour attachment */
	unsigned color_count;
	unsigned depth_format;         /* MTLPixelFormat, or 0 for none */
	unsigned stencil_format;       /* the same format again, or 0: see above */
	unsigned sample_count;

	/*
	 * MTLRenderPipelineDescriptor.vertexDescriptor, appended for the reason the
	 * indexed fields of rwa_draw were appended: userland/src/redwoodmetal.m holds a
	 * copy of this struct typed out by hand, and appending leaves every field that
	 * driver already fills at the offset it already reads. Both still have to be
	 * rebuilt together.
	 *
	 * It is in the SHAPE and not in the draw because that is where Metal puts it: the
	 * vertex descriptor is pipeline state, compiled once, and -setVertexBuffer: is
	 * encoder state. The two halves of one fetch therefore arrive in two calls, and
	 * what each is checked against is different -- the layout against the shader at
	 * creation, the buffers against the layout at the draw.
	 */
	struct rwa_vertex_attribute vertex_attribute[RWA_MAX_VERTEX_ATTRIBUTES];
	struct rwa_vertex_layout vertex_layout[RWA_MAX_VERTEX_BUFFERS];

	/*
	 * MTLRenderPipelineColorAttachmentDescriptor's blend state, one entry per colour
	 * attachment, and APPENDED -- 8 * 32 + 4 = 260 bytes at the END of this struct,
	 * with every field above it left at the offset it already had.
	 *
	 * That is not tidiness. userland/src/redwoodmetal.m holds a copy of this struct
	 * typed out by hand, and this port has paid twice for a field inserted in the
	 * MIDDLE of a shared struct: once with a primitive type arriving as 1871292816
	 * (rwa_draw, see there), and once when struct air_resource grew by eight bytes
	 * and every resource after the first was read eight bytes early. Appending is
	 * what keeps the damage of a half-rebuilt pair to the new field alone. Both
	 * copies still have to be rebuilt together.
	 *
	 * `color_blend_count` of 0 is "this shape carries no blend state", which is
	 * blending off and all four channels written -- see struct rwa_color_blend for
	 * why a count is needed and a zeroed entry cannot stand in for one.
	 */
	struct rwa_color_blend color_blend[8];
	unsigned color_blend_count;
};

/*
 * ONE THING ABOUT THE FOUR COUNTS BELOW, now that an argument buffer's members are
 * bindings of their own: they count the Gallium slots the stage READS, members included,
 * because that is what they have always meant and what a mismatch would be refused
 * against. A member is not a slot the driver fills, though -- it is filled from the
 * encoder's record (rwa_argbuf_bind) -- so a driver that bound `fragment_textures`
 * textures of its own would be binding over the members' slots. What it binds directly
 * is what the application bound at Metal's own indices, and nothing else; the masks
 * below say which buffer indices are sets.
 */
struct rwa_render_info {
	unsigned vertex_buffers;       /* what each stage binds, so a driver can refuse a */
	unsigned fragment_buffers;     /* mismatch before it draws rather than read a slot */
	unsigned fragment_textures;    /* nothing was bound to */
	unsigned fragment_samplers;
	unsigned generated_vertex_stage;   /* 1 when this library supplied the vertex stage */

	/* 1 when the fragment shader stores to [[depth]], and then the depth buffer holds
	 * what the shader computed rather than the interpolated z. Reported rather than
	 * made a requirement, because Metal does not require a depth attachment for such a
	 * shader -- it drops the writes -- and a caller that measures a shader needs to
	 * know which of the two it is looking at. Apple's own texturedFragment2DDepth and
	 * texturedFragment3DDepth are the two in the corpus: each declares a colour output
	 * and stores nothing to it, so with no depth attachment they draw and produce
	 * nothing anyone can read.
	 */
	unsigned fragment_writes_depth;

	/*
	 * What the vertex descriptor turned into, which a driver needs for two things it
	 * cannot work out on its own: how many stream slots a draw has to fill, and what
	 * Metal's pipeline reflection would have to report.
	 *
	 * `vertex_attribute_count` is how many attributes the vertex stage FETCHES -- the
	 * number of pipe_vertex_elements built, which is the used attributes of
	 * air_shader_info and not the configured entries of the descriptor: an attribute
	 * the module marks air.arg_unused costs no fetch, and a configured attribute no
	 * module reads is ignored exactly as Metal ignores it.
	 *
	 * `vertex_attribute_index[i]` is the Metal [[attribute(n)]] of element i, so the
	 * mapping this library built can be read back and compared with the shader's own
	 * interface instead of being taken on trust. The order is driver_location order,
	 * which is the order AIR lists the attributes with the unused ones skipped -- for
	 * std_vert0_lpf (attributes 0, 1 unused, 3) the two entries are 0 and 3.
	 *
	 * `vertex_streams` is the number of slots rwa_draw.vertex_streams must cover
	 * (the highest buffer index any fetched attribute names, plus one) and
	 * `vertex_stream_mask` is which of them are actually fetched from. A slot in the
	 * count but not in the mask needs nothing bound: Gallium's set_vertex_buffers
	 * takes a count from slot 0 where Metal's -setVertexBuffer:atIndex: takes an
	 * index, so a descriptor that uses index 5 alone is still an array of six with
	 * five empty entries.
	 */
	unsigned vertex_attribute_count;
	unsigned vertex_attribute_index[16];
	unsigned vertex_streams;
	unsigned vertex_stream_mask;

	/* And which Metal buffer indices of each stage are ARGUMENT BUFFERS, one bit
	 * each. Metal's one table per stage holds three things now -- a vertex stream,
	 * a buffer argument and a set -- and only the shader and the vertex descriptor
	 * together say which an index is: the descriptor's layouts are
	 * vertex_stream_mask above, the module's sets are these, and what is in neither
	 * mask is an ordinary [[buffer(n)]] argument. A driver that cannot tell them
	 * apart would hand a set's buffer to a shader expecting a constant block.
	 *
	 * Appended for the reason the attribute fields above were appended, and the
	 * same warning applies: this struct is filled in by the library, so a driver
	 * built against the shorter one is handed more than it has room for. Both still
	 * have to be rebuilt together.
	 */
	unsigned vertex_argbuf_mask;
	unsigned fragment_argbuf_mask;

	/*
	 * WHAT THE BLEND STATE BECAME, which is here for the reason
	 * `vertex_attribute_index` is: it is the only way a caller can read back the
	 * mapping this library made instead of trusting it.
	 *
	 * `blend_enabled_mask` is one bit per colour attachment, set when that attachment
	 * ends up blending.
	 *
	 * `color_write_masks` is four bits per attachment, attachment i at bits 4*i, and
	 * they are GALLIUM's bits -- PIPE_MASK_R 0x1 .. PIPE_MASK_A 0x8 -- and NOT
	 * MTLColorWriteMask's, which run the other way round. That is the whole point of
	 * reporting it: for any mask but All, None, green|blue and red|alpha the number
	 * here MUST differ from the writeMask the caller passed, and a log line where the
	 * two agree on, say, 0xe is a log line saying the bits were never reversed.
	 *
	 * Appended, with the warning the two fields above carry: this struct is filled in
	 * BY the library, so a driver built against the shorter one is handed more than it
	 * has room for. Both still have to be rebuilt together.
	 */
	unsigned blend_enabled_mask;
	unsigned color_write_masks;
};

/*
 * MTLDepthStencilDescriptor, which is a device object in Metal and a created state here
 * for the same reason a sampler is: it is compiled once and bound at every draw, and a
 * compare function Metal does not have must be refused when the state is made rather
 * than at the draw that was going to use it.
 *
 * The fields are MTLDepthStencilDescriptor's own, and the two faces are its
 * frontFaceStencil and backFaceStencil. Nothing here is a "stencil enabled" flag,
 * because Metal has none: its defaults -- compare Always, every operation Keep -- ARE
 * the off state, and this library derives Gallium's `enabled` bit from them rather than
 * taking a flag a Metal driver has nothing to fill in with.
 *
 * `read_mask` and `write_mask` are Metal's uint32 masks of an eight-bit buffer; only the
 * low eight bits can select anything and that is what is used.
 */
struct rwa_stencil_face {
	unsigned compare_function;              /* MTLCompareFunction */
	unsigned stencil_fail_operation;        /* MTLStencilOperation */
	unsigned depth_fail_operation;          /* MTLStencilOperation */
	unsigned depth_stencil_pass_operation;  /* MTLStencilOperation */
	unsigned read_mask, write_mask;
};

struct rwa_depth_stencil_state {
	unsigned depth_compare_function;   /* MTLCompareFunction */
	unsigned depth_write_enabled;      /* isDepthWriteEnabled */
	struct rwa_stencil_face front, back;
};

struct rwa_depth_stencil *rwa_depth_stencil_create(struct rwa_device *device,
						   const struct rwa_depth_stencil_state *state,
						   char *why, size_t why_size);
void rwa_depth_stencil_destroy(struct rwa_depth_stencil *state);

/*
 * WHAT A VERTEX LAYOUT IS REFUSED FOR, each refusal naming what was asked for, and every
 * one of them is THIS LIBRARY'S refusal rather than freedreno's for a reason that was
 * read in freedreno's own source: fd6_vertex_state_create does
 * `assert(fmt != FMT6_NONE)` and then programs the register with whatever it has, and
 * A6XX_VFD_DECODE_INSTR_OFFSET() is a masking setter over a 12-bit field. A driver that
 * hands freedreno a layout it cannot fetch gets an assertion in a debug build and a
 * wrong fetch in a release one -- so the sentence has to be written here, where the
 * caller's own words are still in hand.
 *
 *   - an attribute of air_shader_info.vertex_attributes_read with no configured
 *     attribute in the descriptor, UNLESS the module gates it with a function-constant
 *     predicate. The fetch would read a VFD slot nothing was set up for; the gated case
 *     is legal and is the one exception, because the parameter exists whatever the
 *     predicate says and a pipeline whose constants make it false may configure nothing
 *     there (docs/air.md 3.1, and six of QuartzCore's 46 attributes are such).
 *   - a format whose converted type is not the type the shader declared: an integer
 *     format for a `float` member or a normalised one for a `uint` member. This is what
 *     air_vertex_attribute.base_type is for, and why the frontend keeps INT and UINT
 *     apart although `int4` and `uint4` are the same <4 x i32>.
 *   - MTLVertexFormatInt1010102Normalized, UInt1010102Normalized, and the two formats
 *     the iOS 16.5 SDK has not got at all (FloatRG11B10 = 54 and FloatRGB9E5 = 55):
 *     every one of those is several channels packed into one word, and the pixel format
 *     table in air_metal.c declines packed formats for the reason spelled out there --
 *     Metal's channel order and Gallium's are two conventions and a mistake between them
 *     does not fail, it swaps two channels of a picture that still looks like a picture.
 *     FloatRGB9E5 has a second reason that is not a convention: PIPE_FORMAT_R9G9B9E5_FLOAT
 *     is texture-only in fd6_format_table.c, so fd6_vertex_format() answers FMT6_NONE
 *     and the VFD cannot fetch it however the channels are ordered.
 *   - MTLVertexStepFunctionPerPatch and PerPatchControlPoint: tessellation, which has no
 *     pipe_vertex_element field and no stage in this port.
 *   - PerVertex (or Constant) with a step rate other than 1: Gallium has a divisor for
 *     instances and nothing to express a rate over vertices with.
 *   - an attribute offset or a layout stride above 4095 bytes. Metal allows more, and
 *     this is the refusal that matters most because the alternative is silent:
 *     A6XX_VFD_DECODE_INSTR_OFFSET__MASK is 0x0001ffe0 at shift 5, twelve bits, and the
 *     setter ANDs rather than asserts, so offset 4096 would fetch offset 0.
 *   - a buffer index that is also a [[buffer(n)]] the vertex module reads. Metal has one
 *     table behind -setVertexBuffer:offset:atIndex: and both the layouts and the stage's
 *     buffer arguments read out of it, so an index is either a stream or an argument and
 *     never both; an application that configured both wants two things in one slot.
 *   - a vertex descriptor on a pipeline whose vertex stage this library GENERATED. That
 *     stage draws the library's own quad out of the library's own buffer; a descriptor
 *     for it would describe memory the caller does not own.
 *
 * AND WHAT A BLEND STATE IS REFUSED FOR. Every one of these would otherwise DRAW
 * SOMETHING -- a picture with the wrong arithmetic in it -- which is the failure the
 * blend fields were added to end, so each is a sentence with the caller's own words in
 * it:
 *
 *   - MTLBlendFactorBlendColor, OneMinusBlendColor, BlendAlpha and OneMinusBlendAlpha
 *     (11-14). Gallium has all four (PIPE_BLENDFACTOR_CONST_COLOR and friends) and so
 *     does the hardware; what nothing here has is the COLOUR. Metal keeps it on the
 *     encoder, -setBlendColorRed:green:blue:alpha:, so it is not pipeline state and not
 *     in this shape -- and the driver's own setter is a stub that drops it. Honouring
 *     them would blend against the zero a Gallium context starts with, which is
 *     "multiply by nothing": the layer would vanish instead of failing. Serving them is
 *     one float[4] in rwa_draw and one call to set_blend_color, on the day something
 *     asks.
 *   - MTLBlendFactorSource1Color, OneMinusSource1Color, Source1Alpha and
 *     OneMinusSource1Alpha (15-18): dual-source blending, where the second factor comes
 *     from a SECOND fragment output at [[color(0), index(1)]]. Neither this contract nor
 *     this frontend carries a second output -- air_shader_info has one colour per target
 *     and nothing names an index -- so the factor would read whatever register happened
 *     to be there.
 *   - MTLBlendFactorSourceAlphaSaturated as the sourceAlphaBlendFactor or the
 *     destinationAlphaBlendFactor -- and only there. For the three colour channels the
 *     factor is min(As, 1 - Ad) in Metal and in GL alike and it is served; for the ALPHA
 *     channel GL defines it as 1 and Metal's one equation does not, and softpipe
 *     implements GL's ("multiply alpha by 1.0", sp_quad_blend.c). Serving the alpha half
 *     would mean choosing one of two readings of a number the NEXT composite reads.
 *   - MTLBlendOperationMin or Max with any factor other than MTLBlendFactorOne. Metal's
 *     equation multiplies by the factors and then takes the minimum; GL ignores the
 *     factors for these two operations entirely. One is the identity under both readings
 *     and is served, and it is the only form a maximum-blend is ever written in --
 *     anything else draws correctly on one back end and wrongly on the other, with
 *     nothing in the picture to say which.
 *   - a factor above 18 or an operation above 4: not a value of Metal's enumeration at
 *     all, and the number is printed, because that is what a half-rebuilt pair of
 *     structs looks like from this side.
 *   - a writeMask above 0xf. MTLColorWriteMask is four bits; anything else is a field
 *     nobody set or a struct read at the wrong offset.
 *   - blending enabled on a colour attachment whose MTLPixelFormat is a PURE INTEGER
 *     format (RGBA8Uint, R32Sint and the rest). Metal forbids it, GL and Vulkan forbid
 *     it, and there is no arithmetic to do: a blend equation over integers has no
 *     agreed meaning and freedreno would program the blender anyway.
 *   - blend state for fewer attachments than the shape has colour formats: see
 *     rwa_render_shape.color_blend_count.
 *   - colour attachments whose blend states DIFFER on a screen that cannot blend them
 *     independently (pipe_caps.indep_blend_enable / indep_blend_func). Gallium's rt[0]
 *     governs them all when independent blending is off, so the second attachment would
 *     silently get the first one's equation.
 */
/*
 * The function constants of the NEXT rwa_render_create on this thread, per stage: Metal's index
 * and the value's bytes as one little-endian word, which is the packing the translator reads
 * (component i at bit i * width). Without a call every constant is zero, as before. A runtime
 * that specializes nothing itself -- the window server's, where Apple's compiler service cannot
 * read the library -- hands the values here instead of a specialized module.
 */
struct rwa_constant {
	unsigned index;
	uint64_t bits;
};
void rwa_render_constants(const struct rwa_constant *vertex, unsigned vertex_count,
			  const struct rwa_constant *fragment, unsigned fragment_count);

struct rwa_render *rwa_render_create(struct rwa_device *device,
				     const void *vertex_bitcode, size_t vertex_size,
				     const char *vertex_entry,
				     const void *fragment_bitcode, size_t fragment_size,
				     const char *fragment_entry,
				     const struct rwa_render_shape *shape,
				     struct rwa_render_info *info, char *why, size_t why_size);
void rwa_render_destroy(struct rwa_render *render);

/*
 * One draw into one render pass, which is what MTLRenderPassDescriptor plus one
 * -drawPrimitives: amounts to. The colour attachments are textures made by this library.
 *
 * `clear_color` is NULL when the pass is to keep what the attachment already holds, which is
 * Metal's MTLLoadActionLoad; a pointer to four floats is MTLLoadActionClear with that colour.
 * `primitive` is MTLPrimitiveType as Metal defines it.
 *
 * The depth/stencil attachment and its clears are the same shape -- the texture or NULL, and
 * a pointer to a value or NULL for load -- because a pass clears depth and keeps stencil often
 * enough that one load action for both would be a lie about one of them. `depth_stencil` NULL
 * is Metal's state when an application has set none: the test always passes and nothing is
 * written, which is also what every draw this library has made until now did, there having
 * been no depth attachment to test against.
 *
 * `stencil_reference` is front and back, and it is in the DRAW and not in the state object
 * because that is where Metal puts it: -setStencilReferenceValue: is a method of the render
 * command encoder, not a property of MTLDepthStencilDescriptor.
 *
 * `depth_range` is MTLViewport's znear and zfar, the only part of a viewport this contract
 * carries, and it is here because without it a depth test has nothing to test: the vertex
 * stage this library generates puts all four corners at clip z = 0, and Metal's NDC z runs
 * from 0 at the near plane, so the window depth of that quad is exactly `znear`. Two draws
 * that differ only in `depth_range` are the same picture at two depths. NULL is { 0, 1 },
 * which is what every draw before this one used.
 *
 * Returns 0, or -1 with a sentence. A pass the GPU does not finish is a failure here, on the
 * same grounds as a dispatch: a picture that was never drawn must not read as a result.
 *
 * ---------------------------------------------------------------------------------------
 * INDEXED DRAWING, which is what every draw in a real application is.
 *
 * `index_buffer` NULL is -drawPrimitives:vertexStart:vertexCount:, the only form this
 * contract had, and everything below is then zero. A buffer is one of the three direct
 * -drawIndexedPrimitives: forms, and the fields are those calls' arguments one for one:
 *
 *   -drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:
 *                        [:instanceCount:[baseVertex:baseInstance:]]
 *
 * It is an `rwa_buffer` because Metal's index buffer is an `id <MTLBuffer>` and nothing
 * else: a driver has the application's buffer object in hand, and on the device that is
 * one this library made (-newBufferWithLength:options:). A fourth form takes the whole
 * argument list out of a buffer the GPU may write (MTLDrawIndexedPrimitivesIndirect-
 * Arguments) and is NOT here: there is nothing in this contract a driver could put those
 * five words into, and a draw whose count is read on the GPU cannot be checked on the
 * processor at all -- which is the half of this contract that keeps a wrong picture from
 * reading as a result. The driver declines that form by name.
 *
 * THE COUNT IS `vertex_count`, and there is deliberately no second count field. Metal
 * has two names for one role (`vertexCount` without an index buffer, `indexCount` with
 * one) and Gallium has one: `pipe_draw_start_count_bias.count` is however many elements
 * the draw walks, whichever kind they are. A second field would be a second thing to
 * disagree with the first, and this library's whole job at a draw is to refuse the
 * disagreements -- so an indexed draw of three indices sets vertex_count = 3, and
 * vertex_count = 0 is still "a draw of no vertices" and still refused.
 *
 * `vertex_start` must be ZERO for an indexed draw and is refused otherwise, because
 * Metal's indexed forms have no vertexStart: where in the index buffer to start is
 * `index_buffer_offset` and nothing else. The two are the same field on the Gallium side
 * and so cannot both be honoured -- and a caller that filled in both wants something
 * Metal cannot express.
 *
 * `index_buffer_offset` is in BYTES, as Metal's own argument is, and Apple's header
 * states the rule this library enforces by name: "indexBufferOffset must be a multiple
 * of the index size". That rule is not a formality here, it is the reason the conversion
 * exists at all: Gallium counts the start in ELEMENTS (both back ends do --
 * a6xx emits `first_indx = draw->start` against the resource's base, and softpipe hands
 * the draw module the resource's base with `eltSize`), so the byte offset becomes
 * offset / index size, exactly when Metal's own rule holds. An odd offset with
 * MTLIndexTypeUInt16 is that refusal's commonest case and it names the type.
 *
 * `index_type` is MTLIndexType, which has exactly two values -- UInt16 = 0, UInt32 = 1
 * (MTLStageInputOutputDescriptor.h) -- and anything else is refused by name rather than
 * read as one of them. There is no 8-bit index type in Metal, although both back ends
 * have one.
 *
 * `base_vertex` is added to every index before the vertex is fetched, and it is signed
 * because Metal says so in as many words: "baseVertex Offset for vertex_id. NOTE: this
 * can be negative". Gallium's `index_bias` is the same signed int in the same place, and
 * both back ends honour it (fd6_draw.cc's `index_start`, the draw module's `eltBias`).
 * With no index buffer it must be zero: Metal's non-indexed forms have no baseVertex,
 * and a non-zero one there is a caller inventing an argument.
 *
 * `instance_count` and `base_instance` belong in this same struct because that is where
 * Metal puts them -- one call carries indexCount, instanceCount, baseVertex and
 * baseInstance together, and there is no "set the instance count" method on the encoder
 * to carry them separately. ZERO IS READ AS ONE: every caller written before this field
 * existed leaves it zeroed, and Metal's non-instanced forms draw exactly one instance,
 * so the two readings agree. `base_instance` is Metal's "Offset for instance_id", which
 * is what the frontend already assumes of it -- air.instance_id translates to
 * instance_id + base_instance, because Metal's [[instance_id]] counts from the base and
 * GL's gl_InstanceID does not.
 *
 * What this library CHECKS before it draws, each refusal naming what was asked for: the
 * index type; the offset against the index size; that the indices asked for are inside
 * the buffer (offset + count * size <= its length), because both back ends answer an
 * index past the end with something that looks like a picture -- softpipe substitutes
 * index 0 ("if the index buffer would overflow we return index 0", draw_private.h) and
 * a6xx clamps against `max_indices` computed from the resource's width; and, when the
 * vertex stage was GENERATED, that every index lands on one of that quad's four corners
 * once base_vertex is added, which is the indexed form of the vertex_start check that
 * was already here. For a SUPPLIED vertex stage an index is a vertex id and there is
 * nothing to bound it against: the shader reads its own buffer with it, and reading past
 * that buffer is the application's business, exactly as it is in Metal.
 *
 * Primitive restart is not in this contract because it is not in Metal: there is no
 * restart index anywhere in MTLRenderCommandEncoder, so 0xFFFF is an ordinary index
 * here, and Gallium's `primitive_restart` stays off. A driver ported from GL would get
 * that backwards.
 */
struct rwa_vertex_stream {
	struct rwa_buffer *buffer;   /* NULL: nothing bound at this index */
	size_t offset;               /* bytes, Metal's own `offset:` argument */
};

struct rwa_draw {
	struct rwa_texture *const *color;
	unsigned color_count;
	const float *clear_color;      /* NULL, or four floats */
	struct rwa_texture *depth;     /* the depth/stencil attachment, or NULL for none */
	const float *clear_depth;      /* NULL, or one float: MTLClearDepth */
	const unsigned *clear_stencil; /* NULL, or one value: MTLClearStencil */
	const struct rwa_depth_stencil *depth_stencil;   /* NULL: no test, no write */
	unsigned stencil_reference[2];                   /* front, back */
	const float *depth_range;      /* NULL, or { znear, zfar } */
	struct rwa_bindings vertex;
	struct rwa_bindings fragment;
	unsigned primitive;
	unsigned vertex_start, vertex_count;

	/* The indexed forms. Appended rather than placed beside `primitive` on purpose:
	 * userland/src/redwoodmetal.m holds a copy of this struct typed out by hand, and
	 * appending leaves every field that driver already fills at the offset it already
	 * reads. Both still have to be rebuilt together -- a driver compiled against one
	 * shape of this struct calling a library built with the other read a primitive
	 * type back as 1871292816 -- and the refusals above are what turns that mismatch
	 * into a sentence instead of a picture: an index type of 1871292816, an offset no
	 * index size divides and a count no buffer holds are all refused by name.
	 */
	struct rwa_buffer *index_buffer;   /* NULL: the non-indexed -drawPrimitives: */
	unsigned index_type;               /* MTLIndexType: UInt16 = 0, UInt32 = 1 */
	size_t index_buffer_offset;        /* bytes, and a multiple of the index size */
	int base_vertex;                   /* added to every index; may be negative */
	unsigned instance_count;           /* 0 is read as one */
	unsigned base_instance;            /* Metal's offset for [[instance_id]] */

	/*
	 * THE VERTEX STREAMS: -setVertexBuffer:offset:atIndex:, indexed by that index,
	 * which is the index rwa_render_shape.vertex_layout is indexed by as well.
	 *
	 * These are the memory the vertex fetch reads, and they are in the DRAW because
	 * that is where Metal keeps them -- the descriptor is pipeline state and the
	 * buffer is encoder state, and the same pipeline draws two meshes out of two
	 * buffers.
	 *
	 * It is a second array beside `vertex.buffers` rather than the same one, and both
	 * come off the same table in the driver, because of two things that differ:
	 *
	 *   - the OFFSET. rwa_bindings has no room for one, and a stream's offset is not
	 *     the same thing as a buffer argument's: this one is which MESH to draw, it
	 *     is checked against the layout's stride and the draw's own vertex range, and
	 *     dropping it would draw the wrong mesh rather than fail. A buffer argument's
	 *     offset arrives in `*_buffer_table` below, where an argument buffer's does.
	 *   - WHAT IS CHECKED. A slot in `vertex.buffers` has to satisfy what the module
	 *     declares at [[buffer(n)]]; a slot here has to satisfy the layout. The two
	 *     sets must not overlap (rwa_render_create refuses it), so a driver may pass
	 *     its one table as both and this library takes each index from the side that
	 *     owns it.
	 *
	 * `vertex_stream_count` is how many slots this array covers, as Gallium's
	 * set_vertex_buffers counts them: from slot 0, empty entries and all. A slot the
	 * pipeline fetches from and this array leaves NULL is refused by name.
	 *
	 * What is CHECKED about a stream, besides its presence: that the last element the
	 * draw walks is inside the buffer -- offset + stride * (highest vertex index) +
	 * the format's own size, against the buffer's length less the stream offset.
	 * Reading past a vertex buffer is bounded rather than wild on both of these back
	 * ends (freedreno programs VFD_FETCH_SIZE from the resource, softpipe clamps in
	 * the draw module), so what it produces is a picture made partly of zeros, which
	 * is the kind of wrong answer this contract exists to turn into a sentence. A
	 * stream whose layout steps per INSTANCE is not checked that way and says so:
	 * which element an instance reads is the hardware's arithmetic over the base
	 * instance and the divisor, and a bound computed from the wrong formula would
	 * refuse draws that are perfectly legal.
	 */
	const struct rwa_vertex_stream *vertex_streams;
	unsigned vertex_stream_count;

	/*
	 * METAL'S BUFFER TABLE for each stage, at Metal's own indices and with Metal's
	 * own offsets, covering `*_buffer_table_count` slots from 0: an argument
	 * buffer's encoding offset and a buffer argument's own offset, which
	 * `vertex.buffers` and `fragment.buffers` have nowhere to put. struct
	 * rwa_buffer_bind says what each means and what is done with it.
	 *
	 * A driver fills these from the same table it fills `vertex.buffers` and
	 * `fragment.buffers` from -- the one Metal has -- and this library takes from
	 * each side what that side owns. A stream's offset is NOT here: a vertex stream
	 * is `vertex_streams` above, which has its own, and a buffer index is never both
	 * (rwa_render_create refuses a descriptor that claims an index the module reads).
	 *
	 * APPENDED, and that is the whole reason they are four fields here instead of
	 * two in rwa_bindings: `vertex` and `fragment` are embedded by value above, so a
	 * field added inside rwa_bindings moves `primitive` and everything after it for
	 * any driver built against the other shape of this header -- which is how a
	 * primitive type once arrived as 1871292816. These four move nothing.
	 *
	 * A set the pipeline declares with nothing bound at its index is refused by
	 * name, and so is one whose buffer holds no encoding: see rwa_buffer_bind.
	 */
	const struct rwa_buffer_bind *vertex_buffer_table;
	unsigned vertex_buffer_table_count;
	const struct rwa_buffer_bind *fragment_buffer_table;
	unsigned fragment_buffer_table_count;

	/* Appended 27 October, in both copies: -setViewport:'s rectangle (x, y, width, height in
	 * pixels; NULL: the whole attachment) and -setScissorRect: (x, y, width, height; NULL: none).
	 * Both were ignored, and CoreAnimation draws a dirty region or part of an atlas through them:
	 * a layer meant for one rectangle drew over the whole frame. */
	const float *viewport;
	const unsigned *scissor;
};

int rwa_render_draw(struct rwa_render *render, const struct rwa_draw *draw,
		    char *why, size_t why_size);

#ifdef __cplusplus
}
#endif

#endif /* AIR_METAL_H */
