/*
 * An argument encoder: a RECORDER. Metal's encoder writes 8-byte handles into a buffer and those are an Apple
 * GPU's descriptors, so this records (resource, Metal id) against (destination buffer, destination offset) in the
 * compiler library instead, and the draw binds every member the pipeline's shaders declare (docs/air.md 8.2).
 * The layout is the set's -- a shader's, or one the application described -- and the destination the buffer
 * -setArgumentBuffer:offset: named; which objects are the driver's own, and keeping the buffer alive, is the
 * plug-in's (RedwoodMetalArgumentEncoder).
 */
#include "rwm.h"

#include <stdio.h>
#include <string.h>

#include <new>

struct rwm_argenc {
	struct rwa_argbuf_layout layout;
	bool from_module;               /* the layout came from a shader, not descriptors */
	struct rwa_buffer *destination; /* the library's buffer -setArgumentBuffer:offset: named */
	uint8_t *contents;              /* ... and its memory */
	size_t offset;
	bool complained;
};

/*
 * A set the application describes rather than a shader declares: its members by Metal's index, each a buffer, a
 * texture or a sampler, 8 bytes each in Metal's encoding, bound wherever the application likes. What this port's
 * frontend cannot bind -- a value inside a set, an array of descriptors, indices with a gap or a repeat (Metal's
 * encodedLength is over the highest index) -- is said and refused. (MTLDataTypeTexture 58, Sampler 59, Pointer 60.)
 */
extern "C" bool
rwm_argenc_layout_of(const struct rwm_argument *arguments, unsigned long count, struct rwa_argbuf_layout *layout)
{
	const struct rwm_air *air = rwm_air();
	uint64_t seen = 0;

	if (air->argbuf_encode_buffer == nullptr || air->module_argbufs == nullptr) {
		rwm_print("this compiler library has no argument buffers in it, so "
			  "an encoder would record what no draw could read\n");
		return false;
	}
	if (count == 0 || count > RWA_MAX_ARGBUF_MEMBERS) {
		rwm_print("an argument encoder of %lu members: this driver records "
			  "1 to %u\n", count, RWA_MAX_ARGBUF_MEMBERS);
		return false;
	}
	*layout = rwa_argbuf_layout{};
	layout->metal_index = ~0u;      /* the application binds it where it likes */
	layout->alignment = 8;
	layout->member_count = (unsigned)count;
	layout->encoded_length = 8 * (unsigned)count;
	for (unsigned long i = 0; i < count; i++) {
		const unsigned long index = arguments[i].index, type = arguments[i].data_type;
		unsigned kind;

		switch (type) {
		case 60: kind = RWA_ARGBUF_BUFFER; break;
		case 58: kind = RWA_ARGBUF_TEXTURE; break;
		case 59: kind = RWA_ARGBUF_SAMPLER; break;
		default:
			rwm_print("member %lu of this argument encoder is "
				  "MTLDataType %lu, and this port binds a buffer, a texture or a "
				  "sampler member: a value inside a set is refused when the "
				  "shader is translated\n", index, type);
			return false;
		}
		if (arguments[i].array_length > 1) {
			rwm_print("member %lu of this argument encoder is an array "
				  "of %lu descriptors, and this port's frontend refuses a member "
				  "spanning more than one descriptor index\n", index, arguments[i].array_length);
			return false;
		}
		if (index >= count || (seen & (1ull << index)) != 0) {
			rwm_print("this argument encoder has %lu members and one of "
				  "them is index %lu: Metal's encodedLength is over the highest "
				  "index, so this driver will not answer a length for indices "
				  "with a gap or a repeat in them\n", count, index);
			return false;
		}
		seen |= 1ull << index;
		layout->members[i].id = (unsigned)index;
		layout->members[i].kind = kind;
		layout->members[i].index = ~0u;
	}
	return true;
}

/* -[MTLFunction newArgumentEncoderWithBufferIndex:]: the sets the module declares, read by the frontend, and the one at
 * `index` -- or Metal's own answer, that there is no argument buffer there, with which indices there ARE, because an
 * application that asked for the wrong one has no other way to find out */
extern "C" bool
rwm_argenc_layout_for(struct rwa_device *device, const void *module, size_t module_size, const char *name,
		      unsigned long index, struct rwa_argbuf_layout *layout)
{
	const struct rwm_air *air = rwm_air();
	char why[512] = { 0 }, which[96] = "";
	struct rwa_argbuf_layout sets[8];
	unsigned count = 0;

	if (device == nullptr || air->module_argbufs == nullptr) {
		rwm_print("no compiler library, so the argument buffer layout of "
			  "this function cannot be read\n");
		return false;
	}
	if (air->module_argbufs(device, module, module_size, name, sets, (unsigned)(sizeof(sets) / sizeof(sets[0])),
				&count, why, sizeof(why)) != 0) {
		rwm_print("%s\n", why);     /* the frontend's own sentence: it names what it refused */
		return false;
	}
	for (unsigned i = 0; i < count; i++) {
		if (sets[i].metal_index == (unsigned)index) {
			*layout = sets[i];
			return true;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		char one[16];

		snprintf(one, sizeof(one), "%s%u", i ? ", " : " at ", sets[i].metal_index);
		strlcat(which, one, sizeof(which));
	}
	rwm_print("bufferIndex %lu does not identify an argument buffer: this function "
		  "declares %u%s\n", index, count, which);
	return false;
}

extern "C" struct rwm_argenc *
rwm_argenc_new(const struct rwa_argbuf_layout *layout, bool from_module)
{
	rwm_argenc *e = new (std::nothrow) rwm_argenc{};

	if (e != nullptr) {
		e->layout = *layout;
		e->from_module = from_module;
	}
	return e;
}

extern "C" void
rwm_argenc_free(struct rwm_argenc *e)
{
	delete e;
}

extern "C" const struct rwa_argbuf_layout *
rwm_argenc_layout(const struct rwm_argenc *e)
{
	return &e->layout;
}

/*
 * The destination. Metal checks the offset against the buffer's length and the encoder's alignment, and so does
 * this -- an application that encodes at an unaligned offset has allocated its buffer from something other than
 * -encodedLength, and every record made afterwards would be keyed by an offset the draw will never look up.
 */
extern "C" bool
rwm_argenc_set_destination(struct rwm_argenc *e, bool given, struct rwa_buffer *buffer, void *contents, size_t offset)
{
	e->destination = nullptr;
	e->contents = nullptr;
	e->offset = 0;
	if (!given) {
		return true;
	}
	if (buffer == nullptr) {
		rwm_print("the argument buffer did not come from the compiler library, so nothing can be encoded into it\n");
		return false;
	}
	if (e->layout.alignment != 0 && offset % e->layout.alignment != 0) {
		rwm_print("an argument buffer offset of %lu is not a multiple of this encoder's alignment (%u), so the draw "
			  "would look for the encoding somewhere else\n", (unsigned long)offset, e->layout.alignment);
		return false;
	}
	e->destination = buffer;
	e->contents = (uint8_t *)contents;
	e->offset = offset;
	return true;
}

/* whether this encoder is in a state to record, said once per encoder for a missing destination: an id past the
 * set's own members is the mistake worth naming, because Metal would write a handle into the buffer and the shader
 * would read a descriptor that is not there */
extern "C" bool
rwm_argenc_ready(struct rwm_argenc *e, size_t index, const char *what)
{
	if (e->destination == nullptr) {
		if (!e->complained) {
			rwm_print("%s was encoded at id %lu with no argument buffer set (Metal's -setArgumentBuffer:offset: has "
				  "to name one first)\n", what, (unsigned long)index);
			e->complained = true;
		}
		return false;
	}
	if (index >= e->layout.member_count) {
		/* which of the two layouts this is matters to whoever reads the line: a shader's set is the shader's
		 * business, a set described with MTLArgumentDescriptors is the application's own */
		rwm_print("%s was encoded at id %lu of %s that declares %u member%s\n", what, (unsigned long)index,
			  e->from_module ? "a set a shader declares" : "an encoder this application described",
			  e->layout.member_count, e->layout.member_count == 1 ? "" : "s");
		return false;
	}
	return true;
}

/*
 * The three that record. The KIND is the call, as it is in Metal; a member encoded with the wrong one is refused
 * at the draw, by name, where the shader's own declaration is in hand. The library's sentence is passed through.
 */
extern "C" void
rwm_argenc_encode_buffer(struct rwm_argenc *e, size_t index, struct rwa_buffer *buffer, size_t offset)
{
	char why[512] = { 0 };

	if (rwm_air()->argbuf_encode_buffer(e->destination, e->offset, (unsigned)index, buffer, offset, why,
					    sizeof(why)) != 0) {
		rwm_print("%s\n", why);
	}
}

extern "C" void
rwm_argenc_encode_texture(struct rwm_argenc *e, size_t index, struct rwa_texture *texture)
{
	char why[512] = { 0 };

	if (rwm_air()->argbuf_encode_texture(e->destination, e->offset, (unsigned)index, texture, why, sizeof(why)) != 0) {
		rwm_print("%s\n", why);
	}
}

extern "C" void
rwm_argenc_encode_sampler(struct rwm_argenc *e, size_t index, struct rwa_sampler *sampler)
{
	char why[512] = { 0 };

	if (rwm_air()->argbuf_encode_sampler(e->destination, e->offset, (unsigned)index, sampler, why, sizeof(why)) != 0) {
		rwm_print("%s\n", why);
	}
}

/*
 * A VALUE member's place in the argument buffer -- an `int` or a `float4` inside the set -- which is what Metal
 * answers. Nothing of this driver reads those bytes (a shader reading a value out of a set is refused when it is
 * translated, docs/air.md 8.2.1), and that is said once; a real pointer is still the right answer, where NULL
 * would be a crash in the application's memcpy.
 */
extern "C" void *
rwm_argenc_constant_data(struct rwm_argenc *e, size_t index)
{
	static bool said;

	if (!rwm_argenc_ready(e, index, "constant data") || e->contents == nullptr) {
		return nullptr;
	}
	if (!said) {
		said = true;
		rwm_print("-constantDataAtIndex: hands back the argument buffer's own bytes, and nothing in this driver reads "
			  "them: a shader that reads a VALUE out of an argument buffer is refused when it is translated, because "
			  "every member this frontend binds is a descriptor\n");
	}
	return e->contents + e->offset + 8 * index;
}
