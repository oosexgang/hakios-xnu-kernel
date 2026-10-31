/*
 * glassref: one Metal draw, on Apple's GPU and through our AIR -> NIR path, compared pixel by pixel.
 *
 * The phone's driver (userland/src/redwoodmetal.m) hands each pipeline's bitcode -- -[MTLFunction
 * bitcodeData], function constants registered beside it -- to libredwoodair, which translates it and
 * draws with Mesa. This machine's Metal loads the very same iOS 26 libraries (QuartzCore's
 * default.metallib, AIR slice), so the same draw can be made twice: once by Apple's compiler on the
 * M-series GPU, which is the answer, and once by the host build of libredwoodair on softpipe
 * (RWA_SOFTWARE=1), which is ours. Whatever differs is a translation bug, found without the phone.
 *
 * Inputs are made up but plausible: a full-target quad from the library's own vertex shader (positions
 * in clip space, texture coordinates 0..1, white colour; an identity mvp_matrix), each fragment texture a
 * pattern, each uniform member a value chosen by its name (opacity 1, radius 8, colour-matrix rows the
 * identity...), all overridable. The same bytes go to both sides.
 *
 * usage: glassref [options] LIB.metallib VERTEX FRAGMENT
 *   --size WxH            target size (default 64x64)
 *   --fc INDEX=VALUE      a function constant, typed by the library's declaration (repeatable)
 *   --set NAME=V[,V...]   a uniform member's value(s), by name, in every buffer that has it (repeatable)
 *   --tex INDEX=KIND      fragment texture: grad (default), checker, sdf, solid:R,G,B,A
 *   --out DIR             write metal.ppm, ours.ppm, diff.ppm there
 *   --tol T               a pixel differs when a channel differs by more than T (default 0.02)
 *   --show X,Y            print both pixels at X,Y
 *
 * Build: tools/air/glassref.sh
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal.h"
#include "glasscase.h"

#define STREAM 20               /* the vertex buffer's Metal index: above anything the libraries use */
#define SLOTS 31
#define STRIDE GC_STRIDE

static unsigned W = 64, H = 64;
static float tolerance = 0.02f;

struct setting {
	const char *name;
	float v[16];
	int n;
};
static struct setting settings[64];
static int setting_count;

static int fail(const char *what) { fprintf(stderr, "glassref: %s\n", what); return 1; }

/* ---- plausible values for a uniform member, by its name ---------------------------------------- */

static int
override(NSString *name, float *out, int max)
{
	for (int i = 0; i < setting_count; i++) {
		if ([name isEqualToString:@(settings[i].name)]) {
			int n = settings[i].n < max ? settings[i].n : max;
			memcpy(out, settings[i].v, (size_t)n * sizeof(float));
			for (int k = n; k < max; k++) {
				out[k] = settings[i].v[settings[i].n - 1];
			}
			return 1;
		}
	}
	return 0;
}

static void
plausible(NSString *name, int count, float *out)
{
	NSString *n = [name lowercaseString];
	float v = 0.5f;

	if (override(name, out, count)) {
		return;
	}
	if ([n containsString:@"opacity"] || [n containsString:@"alpha"] || [n hasSuffix:@"scale"] ||
	    [n containsString:@"white"] || [n containsString:@"amount"] || [n containsString:@"contribution"]) {
		v = 1.0f;
	} else if ([n containsString:@"radius"] || [n containsString:@"dist"]) {
		v = 8.0f;
	} else if ([n containsString:@"inv_height"] || [n containsString:@"inv_radius"]) {
		v = 0.125f;
	} else if ([n containsString:@"threshold"]) {
		v = 0.25f;
	} else if ([n containsString:@"workaround"] || [n containsString:@"offset"] || [n containsString:@"preserve"] ||
		   [n containsString:@"complex"] || [n containsString:@"darken"]) {
		v = 0.0f;
	} else if ([n containsString:@"limit"]) {
		v = 4.0f;
	}
	for (int k = 0; k < count; k++) {
		out[k] = v;
	}
	/* a colour matrix row (..._cm0, cm1, cm2) or a matrix: the identity */
	if ([n containsString:@"cm"] && count == 4) {
		int row = [n characterAtIndex:[n length] - 1] - '0';
		for (int k = 0; k < 4; k++) {
			out[k] = (k == row) ? 1.0f : 0.0f;
		}
	}
	if (([n containsString:@"mat"] || [n containsString:@"matrix"]) && count == 16) {
		for (int k = 0; k < 16; k++) {
			out[k] = (k % 5 == 0) ? 1.0f : 0.0f;
		}
	}
	if ([n containsString:@"displacement_mat"] && count == 4) {
		out[0] = 1, out[1] = 0, out[2] = 0, out[3] = 1;
	}
}

static uint16_t
half_bits(float f)
{
	__fp16 h = (__fp16)f;
	uint16_t b;
	memcpy(&b, &h, 2);
	return b;
}

static void
store(uint8_t *p, MTLDataType type, const float *v)
{
	switch (type) {
	case MTLDataTypeFloat: case MTLDataTypeFloat2: case MTLDataTypeFloat3: case MTLDataTypeFloat4: {
		int n = type == MTLDataTypeFloat ? 1 : type == MTLDataTypeFloat2 ? 2 : type == MTLDataTypeFloat3 ? 3 : 4;
		memcpy(p, v, (size_t)n * 4);
		break;
	}
	case MTLDataTypeHalf: case MTLDataTypeHalf2: case MTLDataTypeHalf3: case MTLDataTypeHalf4: {
		int n = type == MTLDataTypeHalf ? 1 : type == MTLDataTypeHalf2 ? 2 : type == MTLDataTypeHalf3 ? 3 : 4;
		for (int k = 0; k < n; k++) {
			uint16_t b = half_bits(v[k]);
			memcpy(p + 2 * k, &b, 2);
		}
		break;
	}
	case MTLDataTypeFloat4x4: case MTLDataTypeFloat3x3: case MTLDataTypeFloat2x2:
		memcpy(p, v, type == MTLDataTypeFloat4x4 ? 64 : type == MTLDataTypeFloat3x3 ? 48 : 16);
		break;
	case MTLDataTypeInt: case MTLDataTypeUInt: case MTLDataTypeBool: {
		int32_t i = (int32_t)v[0];
		memcpy(p, &i, type == MTLDataTypeBool ? 1 : 4);
		break;
	}
	default:
		break;
	}
}

static int
components(MTLDataType t)
{
	switch (t) {
	case MTLDataTypeFloat4x4: return 16;
	case MTLDataTypeFloat3x3: return 12;
	case MTLDataTypeFloat2x2: return 4;
	case MTLDataTypeFloat4: case MTLDataTypeHalf4: return 4;
	case MTLDataTypeFloat3: case MTLDataTypeHalf3: return 3;
	case MTLDataTypeFloat2: case MTLDataTypeHalf2: return 2;
	default: return 1;
	}
}

static void
fill_struct(MTLStructType *st, uint8_t *base, size_t size, int depth)
{
	for (MTLStructMember *m in st.members) {
		if (m.offset >= size) {
			continue;
		}
		if (m.dataType == MTLDataTypeStruct) {
			fill_struct(m.structType, base + m.offset, size - m.offset, depth + 1);
			continue;
		}
		float v[16];
		plausible(m.name, components(m.dataType), v);
		store(base + m.offset, m.dataType, v);
		if (getenv("GLASSREF_VERBOSE")) {
			printf("  %*s%-28s @%3lu = %g %g %g %g\n", depth * 2, "", m.name.UTF8String,
			       (unsigned long)m.offset, v[0], v[1], v[2], v[3]);
		}
	}
}

static NSData *
buffer_bytes(id<MTLBufferBinding> b)
{
	size_t size = b.bufferDataSize ? b.bufferDataSize : 16;
	NSMutableData *d = [NSMutableData dataWithLength:size < 16 ? 16 : size];
	uint8_t *p = d.mutableBytes;

	if (b.bufferDataType == MTLDataTypeStruct && b.bufferStructType != nil) {
		fill_struct(b.bufferStructType, p, size, 0);
	} else {
		float v[16];
		int n = components(b.bufferDataType);
		plausible(b.name, n, v);
		if ([b.name isEqualToString:@"mvp_matrix"]) {
			for (int k = 0; k < 16; k++) {
				v[k] = (k % 5 == 0) ? 1.0f : 0.0f;
			}
		}
		if (([b.name containsString:@"texmat"] || [b.name containsString:@"texture_matrix"]) && n == 4) {
			v[0] = 1, v[1] = 1, v[2] = 0, v[3] = 0;          /* scale (1, 1), offset (0, 0) */
		}
		store(p, b.bufferDataType, v);
	}
	return d;
}

/* ---- textures ---------------------------------------------------------------------------------- */

static NSString *tex_kind[SLOTS];

/* ---- output ------------------------------------------------------------------------------------ */

static void
ppm(NSString *path, const float *rgba, int diff_of_two, const float *other)
{
	FILE *f = fopen(path.UTF8String, "wb");
	if (f == NULL) {
		return;
	}
	fprintf(f, "P6\n%u %u\n255\n", W, H);
	for (unsigned i = 0; i < W * H; i++) {
		for (int c = 0; c < 3; c++) {
			float v = diff_of_two ? fabsf(rgba[4 * i + c] - other[4 * i + c]) * 4 : rgba[4 * i + c];
			v = isnan(v) ? 1.0f : fminf(fmaxf(v, 0.0f), 1.0f);
			fputc((int)(v * 255 + 0.5f), f);
		}
	}
	fclose(f);
}

static unsigned
channels_of(MTLPixelFormat fmt)
{
	switch (fmt) {
	case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float: return 1;
	case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: return 2;
	default: return 4;
	}
}

/* bytes a texel of the target takes */
static unsigned
texel_bytes(MTLPixelFormat fmt)
{
	return fmt == MTLPixelFormatRGBA16Float ? 8 : 4 * channels_of(fmt);
}

static float
half_value(uint16_t h)
{
	unsigned e = (h >> 10) & 31, m = h & 1023;
	float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024), (int)e - 25);
	return h & 0x8000 ? -v : v;
}

static int
is_int(MTLPixelFormat fmt)
{
	return fmt == MTLPixelFormatR32Uint || fmt == MTLPixelFormatRG32Uint || fmt == MTLPixelFormatRGBA32Uint ? 1 :
	       fmt == MTLPixelFormatR32Sint || fmt == MTLPixelFormatRG32Sint || fmt == MTLPixelFormatRGBA32Sint ? -1 : 0;
}

/* texels as read back (`channels_of` words each) into RGBA floats, an integer's value as a number */
static void
as_floats(float *texels, MTLPixelFormat fmt)
{
	unsigned n = channels_of(fmt);
	float *out = calloc((size_t)W * H, 16);
	if (fmt == MTLPixelFormatRGBA16Float) {
		const uint16_t *h = (const uint16_t *)texels;
		for (unsigned i = 0; i < W * H * 4; i++) {
			out[i] = half_value(h[i]);
		}
		memcpy(texels, out, (size_t)W * H * 16);
		free(out);
		return;
	}
	for (unsigned i = 0; i < W * H; i++) {
		for (unsigned c = 0; c < 4; c++) {
			if (c >= n) {
				out[4 * i + c] = c == 3 ? 1.0f : 0.0f;
				continue;
			}
			uint32_t w;
			memcpy(&w, &texels[n * i + c], 4);
			out[4 * i + c] = is_int(fmt) > 0 ? (float)w : is_int(fmt) < 0 ? (float)(int32_t)w : texels[n * i + c];
		}
	}
	memcpy(texels, out, (size_t)W * H * 16);
	free(out);
}

/* ---- the two sides ----------------------------------------------------------------------------- */

static MTLVertexFormat
vertex_format(MTLDataType t)
{
	switch (t) {
	case MTLDataTypeFloat: return MTLVertexFormatFloat;
	case MTLDataTypeFloat2: return MTLVertexFormatFloat2;
	case MTLDataTypeFloat3: return MTLVertexFormatFloat3;
	case MTLDataTypeFloat4: return MTLVertexFormatFloat4;
	case MTLDataTypeHalf2: return MTLVertexFormatHalf2;
	case MTLDataTypeHalf4: return MTLVertexFormatHalf4;
	default: return MTLVertexFormatFloat4;
	}
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		const char *out_dir = NULL, *case_path = NULL;
		int show_x = -1, show_y = -1;
		NSMutableDictionary<NSNumber *, NSString *> *fc = [NSMutableDictionary dictionary];
		int a = 1;

		for (; a < argc && argv[a][0] == '-'; a++) {
			if (!strcmp(argv[a], "--size") && a + 1 < argc) {
				sscanf(argv[++a], "%ux%u", &W, &H);
			} else if (!strcmp(argv[a], "--fc") && a + 1 < argc) {
				char *eq = strchr(argv[++a], '=');
				if (eq == NULL) {
					return fail("--fc INDEX=VALUE");
				}
				fc[@(atoi(argv[a]))] = @(eq + 1);
			} else if (!strcmp(argv[a], "--set") && a + 1 < argc && setting_count < 64) {
				char *eq = strchr(argv[++a], '=');
				if (eq == NULL) {
					return fail("--set NAME=V[,V...]");
				}
				*eq = 0;
				settings[setting_count].name = argv[a];
				for (char *s = eq + 1; s && *s && settings[setting_count].n < 16; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL) {
					settings[setting_count].v[settings[setting_count].n++] = strtof(s, NULL);
				}
				setting_count++;
			} else if (!strcmp(argv[a], "--tex") && a + 1 < argc) {
				char *eq = strchr(argv[++a], '=');
				if (eq == NULL) {
					return fail("--tex INDEX=KIND");
				}
				tex_kind[atoi(argv[a]) % SLOTS] = @(eq + 1);
			} else if (!strcmp(argv[a], "--case") && a + 1 < argc) {
				case_path = argv[++a];          /* the draw kept as a file, for tools/air/glasscase.c */
			} else if (!strcmp(argv[a], "--out") && a + 1 < argc) {
				out_dir = argv[++a];
			} else if (!strcmp(argv[a], "--tol") && a + 1 < argc) {
				tolerance = strtof(argv[++a], NULL);
			} else if (!strcmp(argv[a], "--show") && a + 1 < argc) {
				sscanf(argv[++a], "%d,%d", &show_x, &show_y);
			} else {
				return fail("unknown option");
			}
		}
		if (argc - a != 3) {
			return fail("usage: glassref [options] LIB.metallib VERTEX FRAGMENT");
		}

		/* ---- Apple's side ---- */
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		NSError *err = nil;
		NSData *libdata = [NSData dataWithContentsOfFile:@(argv[a])];
		if (libdata == nil) {
			return fail("cannot read the library");
		}
		dispatch_data_t dd = dispatch_data_create(libdata.bytes, libdata.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		id<MTLLibrary> lib = [dev newLibraryWithData:dd error:&err];
		if (lib == nil) {
			return fail(err.description.UTF8String);
		}
		id<MTLFunction> vbase = [lib newFunctionWithName:@(argv[a + 1])];
		id<MTLFunction> fbase = [lib newFunctionWithName:@(argv[a + 2])];
		if (vbase == nil || fbase == nil) {
			return fail("no such function");
		}

		/* function constants: typed by the declaration, the same values to both sides */
		MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
		struct rwa_constant rconst[64];
		unsigned rcount = 0;
		NSMutableDictionary *declared = [NSMutableDictionary dictionary];
		[declared addEntriesFromDictionary:vbase.functionConstantsDictionary];
		[declared addEntriesFromDictionary:fbase.functionConstantsDictionary];
		for (NSString *name in declared) {
			MTLFunctionConstant *c = declared[name];
			NSString *given = fc[@(c.index)];
			if (given == nil) {
				continue;
			}
			uint8_t bytes[32] = { 0 };
			unsigned size = 4;
			/* "x" and raw bytes in hex, as redwoodmetal logs them ("fcvals NAME: I=HEX ..."):
			 * any type, a uint4 of RenderBox's _shader_state included */
			if ([given hasPrefix:@"x"]) {
				const char *h = given.UTF8String + 1;
				size = 0;
				while (h[0] && h[1] && size < sizeof(bytes)) {
					unsigned v;
					sscanf(h, "%2x", &v);
					bytes[size++] = (uint8_t)v;
					h += 2;
				}
				[values setConstantValue:bytes type:c.type atIndex:c.index];
				if (rcount < 64) {
					rconst[rcount].index = (unsigned)c.index;
					rconst[rcount].size = size;
					memcpy(rconst[rcount].bytes, bytes, sizeof(bytes));
					rcount++;
				}
				continue;
			}
			switch (c.type) {
			case MTLDataTypeBool: bytes[0] = (uint8_t)(given.intValue != 0); size = 1; break;
			case MTLDataTypeFloat: { float f = given.floatValue; memcpy(bytes, &f, 4); break; }
			case MTLDataTypeHalf: { uint16_t h = half_bits(given.floatValue); memcpy(bytes, &h, 2); size = 2; break; }
			case MTLDataTypeShort: case MTLDataTypeUShort: { int16_t s = (int16_t)given.intValue; memcpy(bytes, &s, 2); size = 2; break; }
			case MTLDataTypeChar: case MTLDataTypeUChar: bytes[0] = (uint8_t)given.intValue; size = 1; break;
			default: { int32_t i = given.intValue; memcpy(bytes, &i, 4); break; }
			}
			[values setConstantValue:bytes type:c.type atIndex:c.index];
			if (rcount < 64) {
				rconst[rcount].index = (unsigned)c.index;
				rconst[rcount].size = size;
				memcpy(rconst[rcount].bytes, bytes, sizeof(bytes));
				rcount++;
			}
		}
		id<MTLFunction> vfn = vbase, ffn = fbase;
		if (vbase.functionConstantsDictionary.count) {
			vfn = [lib newFunctionWithName:@(argv[a + 1]) constantValues:values error:&err];
		}
		if (fbase.functionConstantsDictionary.count) {
			ffn = [lib newFunctionWithName:@(argv[a + 2]) constantValues:values error:&err];
		}
		if (vfn == nil || ffn == nil) {
			return fail(err.localizedDescription.UTF8String);
		}

		/* the quad: two triangles, every attribute in one interleaved buffer at index STREAM */
		MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
		struct rwa_render_shape shape;
		memset(&shape, 0, sizeof(shape));
		float verts[6][STRIDE / 4];
		memset(verts, 0, sizeof(verts));
		static const float corner[6][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		int slot = 0;
		for (MTLVertexAttribute *attr in vbase.vertexAttributes) {
			if (!attr.active && !attr.isActive) {
				/* still described: the module declares it */
			}
			unsigned off = (unsigned)(16 * slot++);
			vd.attributes[attr.attributeIndex].format = vertex_format(attr.attributeType);
			vd.attributes[attr.attributeIndex].offset = off;
			vd.attributes[attr.attributeIndex].bufferIndex = STREAM;
			shape.vertex_attribute[attr.attributeIndex].format = (unsigned)vertex_format(attr.attributeType);
			shape.vertex_attribute[attr.attributeIndex].offset = off;
			shape.vertex_attribute[attr.attributeIndex].buffer_index = STREAM;
			NSString *n = [attr.name lowercaseString];
			for (int v = 0; v < 6; v++) {
				float *p = &verts[v][off / 4];
				float x = corner[v][0], y = corner[v][1];
				if ([n containsString:@"position"]) {
					p[0] = x, p[1] = y, p[2] = 0, p[3] = 1;
				} else if ([n containsString:@"color"]) {
					p[0] = p[1] = p[2] = p[3] = 1;
				} else {
					/* texture coordinates: Metal's top-left origin, v down */
					p[0] = (x + 1) / 2, p[1] = (1 - y) / 2, p[2] = 0, p[3] = 1;
				}
			}
		}
		vd.layouts[STREAM].stride = STRIDE;
		shape.vertex_layout[STREAM].stride = STRIDE;
		shape.vertex_layout[STREAM].step_function = 1;
		shape.vertex_layout[STREAM].step_rate = 1;

		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = vfn;
		pd.fragmentFunction = ffn;
		pd.vertexDescriptor = vd;
		/* the target: 32-bit float, unless the shader writes integers */
		MTLPixelFormat target_fmt = MTLPixelFormatRGBA32Float;
		/* GLASSREF_TARGET=rgba16f: a target of halves, what Core Animation's EDR layers draw into (a half4 colour
		 * output keeps its 16 bits in our translation, for such a target) */
		if (getenv("GLASSREF_TARGET") && strcmp(getenv("GLASSREF_TARGET"), "rgba16f") == 0) {
			target_fmt = MTLPixelFormatRGBA16Float;
		}
		MTLRenderPipelineReflection *refl = nil;
		id<MTLRenderPipelineState> ps = nil;
		for (int attempt = 0; attempt < 2 && ps == nil; attempt++) {
			pd.colorAttachments[0].pixelFormat = target_fmt;
			ps = [dev newRenderPipelineStateWithDescriptor:pd
							       options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
							    reflection:&refl error:&err];
			if (ps == nil && attempt == 0) {
				NSString *e = err.localizedDescription;
				static const struct { const char *type; MTLPixelFormat fmt; } outs[] = {
					{ "of type uint4", MTLPixelFormatRGBA32Uint }, { "of type uint2", MTLPixelFormatRG32Uint },
					{ "of type uint ", MTLPixelFormatR32Uint }, { "of type int4", MTLPixelFormatRGBA32Sint },
					{ "of type int2", MTLPixelFormatRG32Sint }, { "of type int ", MTLPixelFormatR32Sint },
					{ "of type ushort4", MTLPixelFormatRGBA32Uint }, { "of type ushort2", MTLPixelFormatRG32Uint },
					{ "of type float2", MTLPixelFormatRG32Float }, { "of type half2", MTLPixelFormatRG32Float },
				};
				MTLPixelFormat pick = MTLPixelFormatInvalid;
				for (unsigned k = 0; k < sizeof(outs) / sizeof(outs[0]) && pick == MTLPixelFormatInvalid; k++) {
					if ([e containsString:@(outs[k].type)]) {
						pick = outs[k].fmt;
					}
				}
				if (pick == MTLPixelFormatInvalid) {
					break;
				}
				target_fmt = pick;
			}
		}
		if (ps == nil) {
			return fail(err.localizedDescription.UTF8String);
		}

		/* the inputs, made once and given to both */
		NSData *vbytes[SLOTS] = { nil }, *fbytes[SLOTS] = { nil };
		NSData *texels[SLOTS] = { nil };
		int fsampler[SLOTS] = { 0 };
		MTLPixelFormat tex_fmt[SLOTS] = { 0 };
		const char *kinds[SLOTS] = { NULL };
		for (id<MTLBinding> b in refl.vertexBindings) {
			if (b.type == MTLBindingTypeBuffer && b.index < SLOTS && b.index != STREAM) {
				vbytes[b.index] = buffer_bytes((id<MTLBufferBinding>)b);
			}
		}
		for (id<MTLBinding> b in refl.fragmentBindings) {
			if (b.type == MTLBindingTypeBuffer && b.index < SLOTS) {
				fbytes[b.index] = buffer_bytes((id<MTLBufferBinding>)b);
			} else if (b.type == MTLBindingTypeSampler && b.index < SLOTS) {
				fsampler[b.index] = 1;
			} else if (b.type == MTLBindingTypeTexture && b.index < SLOTS) {
				NSMutableData *d = [NSMutableData dataWithLength:(size_t)W * H * 16];
				MTLDataType et = ((id<MTLTextureBinding>)b).textureDataType;
				if (et == MTLDataTypeUInt || et == MTLDataTypeInt || et == MTLDataTypeUShort || et == MTLDataTypeShort) {
					tex_fmt[b.index] = et == MTLDataTypeInt || et == MTLDataTypeShort ? MTLPixelFormatRGBA32Sint
													    : MTLPixelFormatRGBA32Uint;
					kinds[b.index] = "seeds";
				} else {
					tex_fmt[b.index] = MTLPixelFormatRGBA32Float;
					kinds[b.index] = tex_kind[b.index] != nil ? tex_kind[b.index].UTF8String : "grad";
				}
				gc_pattern(kinds[b.index], W, H, d.mutableBytes);
				texels[b.index] = d;
			}
		}

		id<MTLTexture> target;
		{
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target_fmt
												    width:W height:H mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			td.storageMode = MTLStorageModeShared;
			target = [dev newTextureWithDescriptor:td];
		}
		id<MTLCommandQueue> q = [dev newCommandQueue];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = target;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:ps];
		[enc setVertexBytes:verts length:sizeof(verts) atIndex:STREAM];
		for (int i = 0; i < SLOTS; i++) {
			if (vbytes[i]) {
				[enc setVertexBuffer:[dev newBufferWithBytes:vbytes[i].bytes length:vbytes[i].length
								      options:MTLResourceStorageModeShared] offset:0 atIndex:i];
			}
			if (fbytes[i]) {
				[enc setFragmentBuffer:[dev newBufferWithBytes:fbytes[i].bytes length:fbytes[i].length
									options:MTLResourceStorageModeShared] offset:0 atIndex:i];
			}
			if (fsampler[i]) {
				MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
				sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
				sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
				[enc setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:i];
			}
			if (texels[i]) {
				MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex_fmt[i]
													    width:W height:H mipmapped:NO];
				td.usage = MTLTextureUsageShaderRead;
				td.storageMode = MTLStorageModeShared;
				id<MTLTexture> t = [dev newTextureWithDescriptor:td];
				[t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:texels[i].bytes bytesPerRow:W * 16];
				[enc setFragmentTexture:t atIndex:i];
			}
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
		[enc endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		float *apple = calloc((size_t)W * H, 16);
		[target getBytes:apple bytesPerRow:W * texel_bytes(target_fmt) fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
		as_floats(apple, target_fmt);

		/* ---- our side: the same bitcode the phone's driver gets, drawn by tools/air/glasscase.c ---- */
		SEL bc = sel_getUid("bitcodeData");
		NSData *vair = ((NSData *(*)(id, SEL))objc_msgSend)(vbase, bc);
		NSData *fair = ((NSData *(*)(id, SEL))objc_msgSend)(fbase, bc);
		if (vair == nil || fair == nil) {
			return fail("no bitcodeData");
		}
		shape.color_format[0] = target_fmt;
		shape.color_count = 1;
		shape.sample_count = 1;
		shape.color_blend[0].write_mask = 0xf;
		shape.color_blend_count = 1;
		static struct gc_case gc;
		memset(&gc, 0, sizeof(gc));
		gc.w = W, gc.h = H, gc.target_format = (uint32_t)target_fmt, gc.tolerance = tolerance;
		strlcpy(gc.vertex_name, argv[a + 1], sizeof(gc.vertex_name));
		strlcpy(gc.fragment_name, argv[a + 2], sizeof(gc.fragment_name));
		gc.vertex_air = (struct gc_blob) { (uint32_t)vair.length, (void *)vair.bytes };
		gc.fragment_air = (struct gc_blob) { (uint32_t)fair.length, (void *)fair.bytes };
		gc.vertex_constants = vbase.functionConstantsDictionary.count != 0;
		gc.fragment_constants = fbase.functionConstantsDictionary.count != 0;
		gc.constant_count = rcount;
		memcpy(gc.constants, rconst, sizeof(rconst[0]) * rcount);
		gc.shape = shape;
		memcpy(gc.vertices, verts, sizeof(gc.vertices));
		for (int i = 0; i < SLOTS; i++) {
			if (vbytes[i]) {
				gc.vertex_buffer[i] = (struct gc_blob) { (uint32_t)vbytes[i].length, (void *)vbytes[i].bytes };
			}
			if (fbytes[i]) {
				gc.fragment_buffer[i] = (struct gc_blob) { (uint32_t)fbytes[i].length, (void *)fbytes[i].bytes };
			}
			gc.fragment_sampler[i] = (uint32_t)fsampler[i];
			if (texels[i]) {
				gc.texture_format[i] = (uint32_t)tex_fmt[i];
				strlcpy(gc.texture_kind[i], kinds[i], sizeof(gc.texture_kind[i]));
			}
		}
		gc.apple = (struct gc_blob) { W * H * 16, apple };
		if (case_path != NULL && gc_write(case_path, &gc)) {
			return fail("cannot write the case");
		}

		setenv("RWA_SOFTWARE", "1", 1);
		char why[512] = "";
		struct rwa_device *rdev = rwa_device_open(why, sizeof(why));
		if (rdev == NULL) {
			return fail(why);
		}
		float *ours = calloc((size_t)W * H, 16);
		if (gc_draw(rdev, &gc, ours, getenv("GLASSREF_VERBOSE") != NULL, why, sizeof(why))) {
			printf("OURS: %s + %s: %s\n", argv[a + 1], argv[a + 2], why);
			return 2;
		}
		char name[300];
		snprintf(name, sizeof(name), "%s + %s", argv[a + 1], argv[a + 2]);
		unsigned bad = gc_compare(name, &gc, ours, show_x, show_y, stdout);
		if (out_dir) {
			NSString *dir = @(out_dir);
			[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
			ppm([dir stringByAppendingPathComponent:@"metal.ppm"], apple, 0, NULL);
			ppm([dir stringByAppendingPathComponent:@"ours.ppm"], ours, 0, NULL);
			ppm([dir stringByAppendingPathComponent:@"diff.ppm"], apple, 1, ours);
		}
		return bad ? 3 : 0;
	}
}
