/*
 * Does the driver's device object, built as an ordinary library, get through Metal?
 *
 * metal_stubdev proved the contract with classes assembled at runtime. This asks the same
 * question of the real thing: /redwood/lib/libredwoodmetal.dylib, whose RedwoodMetalDevice
 * is declared in source and linked against the private `_OBJC_CLASS_$__MTLDevice`. Whether
 * that symbol binds at all is the first thing this program measures -- if dyld cannot find
 * it, the library does not load and the driver has to go back to building its class at
 * runtime.
 *
 * Then the same three steps as before, and in the same order Metal uses them:
 *   step 4   [class isSubclassOfClass:_MTLDevice]
 *   step 5   [[class alloc] initWithAcceleratorPort:service]
 *   MTLAddDevice (0x181a1875c), then MTLCreateSystemDefaultDevice()
 *
 * Discovery is still bypassed, deliberately: no bundle, and the service still has neither
 * MetalPluginName nor MetalPluginClassName, because docs/metal.md section 7 measured what
 * happens when the whole system takes the Metal road with nothing behind it.
 *
 * With --bundle it loads the driver the way Metal's own discovery would -- through NSBundle,
 * out of /redwood/lib/RedwoodMetal.bundle -- instead of dlopen'ing the dylib. That tests the
 * one part of discovery that cannot be reasoned about: whether a flat, unsigned bundle of
 * ours gives up its class to -[NSBundle classNamed:], whose identity test
 * ([NSBundle bundleForClass:cls] == self, 0x1811e0dac) is the thing most likely to refuse.
 * The service is still not given MetalPluginName/MetalPluginClassName, so Metal is not asked
 * to find anything: only the steps are walked, in its order.
 *
 * Run: /redwood/bin/metal_drv_test [--bundle] [LIBRARY [FUNCTION]]   (RC_JOBS=rc.jobs.metaldev)
 *
 * With no arguments it reads /System/Library/VideoDecoders/AV1SW.metallib and offers every
 * function in it to the compiler. With a FUNCTION it compiles and dispatches just that one,
 * which is how a kernel binding nothing but buffers can be picked: the driver has no
 * textures yet, and most of the system's shaders want them.
 *
 * With --picture [SECONDS] it draws the panel instead of a pixel: 64 of Apple's own
 * fragment shaders and Apple's own textured blit, at 1080x2400, put on the screen through
 * the kernel's GPU service so that `./xl screenshot` can take it. See picture_path below;
 * RC_JOBS=rc.jobs.metalpicture runs it.
 *
 * With --time [REPEATS] it measures what all of that COSTS -- a compile, a draw at the
 * panel's own size, a texture, a command buffer, a frame on the screen -- which no mode
 * above it has ever done. See time_path below; RC_JOBS=rc.jobs.metalperf runs it.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach_time.h>

#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define MTL_ADD_DEVICE_UNSLID    0x181a1875cUL
extern id MTLCreateSystemDefaultDevice(void);

/*
 * One call of the compiler library's own contract, declared here as redwoodmetal.m
 * declares the rest of it (tools/mesa/air/air_metal.h is in the Mesa tree and this
 * program must not grow a dependency on it): what textures a module declares.
 *
 * It is asked of the same /redwood/lib/libredwoodair.dylib the driver loaded -- dyld
 * uniques images by install_name, so this dlopen gets that image and not a second one --
 * and of the same `struct rwa_device` the driver holds, which RedwoodMetalDevice hands
 * out through -mesaDevice. So the answer here is the answer the driver will judge a
 * binding by at the draw, and not a second opinion about the same shader.
 *
 * Why this program has to ask at all: it is in the position of an application that did
 * not write the shader and has no MTLTextureDescriptor to read the shape off. Before
 * this, --draw-all bound ONE kind and ONE format for a whole library, and 13 of the 271
 * fragment shaders of AppleMetalGLRenderer were refused by name -- they declare a
 * texture3d, a texture2d<int> or a multisample texture. That refusal was honest and it
 * was about this harness, which means 13 shaders went unmeasured for a reason that has
 * nothing to do with the GPU.
 */
struct rwa_device;

enum rwa_element {
	RWA_ELEMENT_UNKNOWN = 0,
	RWA_ELEMENT_FLOAT,
	RWA_ELEMENT_INT,
	RWA_ELEMENT_UINT,
};

struct rwa_texture_decl {
	unsigned index;
	unsigned type;           /* MTLTextureType */
	unsigned type_known;
	unsigned usage;          /* MTLTextureUsage */
	unsigned element;        /* enum rwa_element */
	unsigned element_bits;
	char metal_type[64];
};

static int (*rwa_module_textures)(struct rwa_device *, const void *, size_t,
				  const char *, struct rwa_texture_decl *, unsigned,
				  unsigned *, char *, size_t);
static struct rwa_device *air_device;

/*
 * How many members an MTLArgumentEncoder has recorded against (buffer, offset), asked of
 * the compiler library itself.
 *
 * It is the one thing about an encoder that cannot be seen from Metal's side: there is no
 * argument buffer to read back, because the frontend resolved every member of every set
 * to a binding and Metal's 8-byte handles mean nothing to a Gallium driver (docs/air.md
 * 8.2), so what an encoder leaves behind is a RECORD in the library and not bytes. This
 * reaches it the way -mesaDevice is reached above: the driver's own accessor for the
 * `struct rwa_buffer` behind an MTLBuffer, and then the library's own count.
 */
static unsigned (*rwa_argbuf_encoded_members)(const void *, size_t);

static unsigned
argbuf_records(id<MTLBuffer> buffer, size_t offset)
{
	SEL mesa = sel_getUid("mesaBuffer");

	if (rwa_argbuf_encoded_members == NULL || buffer == nil ||
	    ![buffer respondsToSelector:mesa]) {
		return 0;
	}
	const void *b = ((const void *(*)(id, SEL))objc_msgSend)(buffer, mesa);

	return b != NULL ? rwa_argbuf_encoded_members(b, offset) : 0;
}

/* Both halves of the query, or neither: a table filled from a device that is not the
 * driver's would describe a shader nobody is about to draw. Prints what is missing and
 * leaves the sweep to its old single binding, which is a smaller measurement and not a
 * wrong one. */
static void
load_texture_query(id<MTLDevice> dev)
{
	void *lib = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW);

	if (lib == NULL) {
		printf("drvtest: no compiler library to ask what a shader declares: %s\n",
		       dlerror());
		return;
	}
	*(void **)&rwa_module_textures = dlsym(lib, "rwa_module_textures");
	/* and the argument-buffer record count, which --argbuf asks for and nothing else
	 * needs: a library without it is older than that mode and says so there. */
	*(void **)&rwa_argbuf_encoded_members = dlsym(lib, "rwa_argbuf_encoded_members");
	if (rwa_module_textures == NULL) {
		printf("drvtest: that compiler library has no rwa_module_textures, so it is "
		       "older than this test\n");
		return;
	}
	SEL mesa = sel_getUid("mesaDevice");

	if (![dev respondsToSelector:mesa]) {
		printf("drvtest: this device object does not answer -mesaDevice, so there is "
		       "no screen to read a module with\n");
		rwa_module_textures = NULL;
		return;
	}
	air_device = ((struct rwa_device *(*)(id, SEL))objc_msgSend)(dev, mesa);
	if (air_device == NULL) {
		printf("drvtest: the device has no Mesa screen (a boot without gpu=1), so "
		       "nothing can be read out of a module\n");
		rwa_module_textures = NULL;
	}
}

/* One function's module, as -[_MTLFunction bitcodeData] hands it over: the 0x0B17C0DE
 * wrapper and all, which the compiler library accepts either way. nil when this
 * function does not answer the selector, which is the one thing that would make the
 * whole query moot. */
static NSData *
function_bitcode(id<MTLFunction> fn)
{
	SEL bitcode = sel_getUid("bitcodeData");

	if (fn == nil || ![fn respondsToSelector:bitcode]) {
		return nil;
	}
	NSData *data = ((NSData *(*)(id, SEL))objc_msgSend)(fn, bitcode);

	return data != nil && [data length] >= 4 ? data : nil;
}

/* Steps 3 and 4 of Metal's discovery, by hand, against a bundle of ours. Returns the class
 * Metal would have ended up with, or Nil with the reason printed. */
static Class
class_from_bundle(const char *path)
{
	NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:path]];

	if (bundle == nil) {
		printf("drvtest: NSBundle would not open %s\n", path);
		return Nil;
	}
	printf("drvtest: bundle %s, executable %s\n", path,
	       [bundle executablePath] != nil ? [[bundle executablePath] UTF8String] : "(none)");

	/* what step 4 does first, and the test that can refuse it */
	Class named = [bundle classNamed:@"RedwoodMetalDevice"];

	printf("drvtest: [bundle classNamed:] gave %s\n",
	       named != Nil ? class_getName(named) : "nil -- the identity test refused");

	/* and the road Metal takes when that answers nil, which makes no such test */
	Class principal = [bundle principalClass];

	printf("drvtest: [bundle principalClass] gave %s\n",
	       principal != Nil ? class_getName(principal) : "nil");

	Class cls = named != Nil ? named : principal;
	if (cls == Nil) {
		return Nil;
	}
	if ([NSBundle bundleForClass:cls] == bundle) {
		printf("drvtest: and the runtime agrees the class came out of that bundle\n");
	}
	return cls;
}

/* One number over a run of bytes, computed the same way here and on the host, so that two
 * readers of the same module can be compared without carrying the module anywhere. */
static uint64_t
fnv1a(const unsigned char *bytes, size_t length)
{
	uint64_t hash = 0xcbf29ce484222325ULL;

	for (size_t i = 0; i < length; i++) {
		hash ^= bytes[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/* Where the AIR of one function actually comes from.
 *
 * -[_MTLFunction bitcodeData] (0x181aef9e0) is @required on <MTLFunctionSPI> and returns the
 * function's module with LLVM's 0x0B17C0DE wrapper in front of it: Metal has already read
 * the container, found this function in it and kept the whole metallib mapped, so a driver
 * asks one declared selector and gets bytes. That is the junction with subsystem (1): the
 * frontend in docs/air.md reads exactly such a module.
 *
 * So this prints, for every function of the library, the wrapper's own idea of where the
 * module is and a checksum over it -- which tools/metallib.py computes over the same
 * function of the same file on the host. Equal numbers mean Metal and our own reader hand
 * the compiler the same bytes; different numbers would mean the frontend has been compiling
 * something other than what Metal would give it. */
static void
air_of_each_function(id<MTLLibrary> library)
{
	SEL bitcode = sel_getUid("bitcodeData");

	for (NSString *name in [library functionNames]) {
		id<MTLFunction> fn = [library newFunctionWithName:name];

		if (fn == nil) {
			printf("drvtest: air %-34s no function object\n", [name UTF8String]);
			continue;
		}
		if (![fn respondsToSelector:bitcode]) {
			printf("drvtest: air %-34s does not answer bitcodeData\n",
			       [name UTF8String]);
			continue;
		}
		NSData *data = ((NSData *(*)(id, SEL))objc_msgSend)(fn, bitcode);

		if (data == nil || [data length] < 20) {
			printf("drvtest: air %-34s bitcodeData gave %s\n", [name UTF8String],
			       data == nil ? "nil" : "less than a wrapper");
			continue;
		}
		const unsigned char *bytes = [data bytes];
		uint32_t magic = 0, offset = 0, size = 0;

		memcpy(&magic, bytes + 0, 4);
		memcpy(&offset, bytes + 8, 4);
		memcpy(&size, bytes + 12, 4);
		if (magic != 0x0b17c0deU || (size_t)offset + size > [data length]) {
			printf("drvtest: air %-34s wrapper %#x, offset %u, size %u of %lu -- "
			       "not what the format says\n", [name UTF8String], magic, offset,
			       size, (unsigned long)[data length]);
			continue;
		}
		printf("drvtest: air %-34s module %u bytes at %u of %lu, fnv1a %016llx\n",
		       [name UTF8String], size, offset, (unsigned long)[data length],
		       (unsigned long long)fnv1a(bytes + offset, size));
	}
	fflush(stdout);
}

/* The compute path, asked of the driver exactly as an application would ask.
 *
 * Every function of the library is offered to newComputePipelineStateWithFunction:, because
 * the interesting number is how many of them a driver on this port can compile at all, and
 * because a refusal carries the front end's own sentence about what it could not translate.
 * The first one that compiles and binds nothing but buffers is then dispatched for real:
 * queue, command buffer, encoder, commit, wait, and a look at the memory afterwards. */
/* What kind of texture to bind, and of what format, when nothing else says. A kernel
 * declaring `texture3d<half, sample>` is refused when a 2D texture is bound, and the refusal
 * names the kind -- which is useful once and useless twice, so --tex/--texfmt let the kind be
 * given on the command line after it has been read out of the shader's own air.texture
 * metadata on the machine.
 *
 * The draw sweep no longer needs that: rwa_module_textures answers what each shader declares
 * and --draw-all binds it per shader. These remain the compute path's binding and the draw
 * sweep's fallback for a slot no shader named, and --texslots below remains an override. */
static NSUInteger texture_kind = MTLTextureType2D;
static MTLPixelFormat texture_format = MTLPixelFormatRGBA32Float;

/*
 * And per slot, because a kernel can declare a different kind and element type in each:
 * one of the corpus's reads texture 0 as texture2d<uint> and texture 3 as texture2d<int>,
 * and a harness that binds one format everywhere is refused by name for the others. The
 * maps are filled from the shader's own air.texture entries, read on the machine; a slot
 * nobody named falls back to the single values above.
 *
 * Where --draw-all now reads the declaration itself, these are the deliberate mismatch: a
 * slot named here is bound what the command line says and not what the shader asked for,
 * which is the only way the driver's refusal can be tested at all.
 */
#define RW_TEX_SLOTS 16
static MTLPixelFormat slot_format[RW_TEX_SLOTS];
static NSUInteger slot_kind[RW_TEX_SLOTS];

static MTLPixelFormat
format_named(const char *name)
{
	static const struct { const char *name; MTLPixelFormat format; } formats[] = {
		{ "rgba8unorm", MTLPixelFormatRGBA8Unorm },
		{ "rgba16float", MTLPixelFormatRGBA16Float },
		{ "rgba16uint", MTLPixelFormatRGBA16Uint },
		{ "rgba16sint", MTLPixelFormatRGBA16Sint },
		{ "rgba32float", MTLPixelFormatRGBA32Float },
		{ "rgba32uint", MTLPixelFormatRGBA32Uint },
		{ "rgba32sint", MTLPixelFormatRGBA32Sint },
		{ "r32float", MTLPixelFormatR32Float },
		{ "r32uint", MTLPixelFormatR32Uint },
		/* the depth attachment formats iOS has, for --draw-depth. Depth24Unorm_Stencil8
		 * is absent because Metal's own header marks it API_UNAVAILABLE(ios) and the
		 * device answers NO to -isDepth24Stencil8PixelFormatSupported. */
		{ "depth16unorm", MTLPixelFormatDepth16Unorm },
		{ "depth32float", MTLPixelFormatDepth32Float },
		{ "depth32float_stencil8", MTLPixelFormatDepth32Float_Stencil8 },
		{ "stencil8", MTLPixelFormatStencil8 },
	};

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (strcmp(name, formats[i].name) == 0) {
			return formats[i].format;
		}
	}
	printf("drvtest: unknown texture format \"%s\"\n", name);
	return 0;
}

static NSUInteger
kind_named(const char *name)
{
	return strcmp(name, "3d") == 0 ? MTLTextureType3D :
	    strcmp(name, "cube") == 0 ? MTLTextureTypeCube :
	    strcmp(name, "1d") == 0 ? MTLTextureType1D :
	    strcmp(name, "2darray") == 0 ? MTLTextureType2DArray : MTLTextureType2D;
}

/* "0=2d:rgba32uint,3=2d:rgba32sint" -- one entry per slot the shader declares. */
static void
parse_slots(const char *spec)
{
	char buf[512];

	snprintf(buf, sizeof(buf), "%s", spec);
	for (char *tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
		unsigned index = 0;
		char kind[16] = { 0 }, fmt[32] = { 0 };

		if (sscanf(tok, "%u=%15[^:]:%31s", &index, kind, fmt) != 3 ||
		    index >= RW_TEX_SLOTS) {
			printf("drvtest: cannot read the slot spec \"%s\"\n", tok);
			continue;
		}
		slot_kind[index] = kind_named(kind);
		slot_format[index] = format_named(fmt);
	}
}

/* One texel, in whatever the texture's format is. __fp16 does the half conversion with the
 * hardware's own rounding rather than a hand-rolled one, which is the sort of thing that is
 * wrong in the last bit and then blamed on the GPU. */
static void
write_texel(unsigned char *at, MTLPixelFormat format, const float v[4])
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
		for (int c = 0; c < 4; c++) {
			float x = v[c] / 1000.0f;   /* the values run to 1400; scale to [0,1] */

			at[c] = (unsigned char)(x < 0 ? 0 : x > 1 ? 255 : x * 255.0f + 0.5f);
		}
		return;
	case MTLPixelFormatRGBA16Float: {
		__fp16 h[4];

		for (int c = 0; c < 4; c++) {
			h[c] = (__fp16)v[c];
		}
		memcpy(at, h, sizeof(h));
		return;
	}
	case MTLPixelFormatRGBA16Uint: {
		unsigned short u[4];

		for (int c = 0; c < 4; c++) {
			u[c] = (unsigned short)v[c];
		}
		memcpy(at, u, sizeof(u));
		return;
	}
	case MTLPixelFormatRGBA16Sint: {
		short s[4];

		for (int c = 0; c < 4; c++) {
			s[c] = (short)v[c];
		}
		memcpy(at, s, sizeof(s));
		return;
	}
	case MTLPixelFormatRGBA32Uint: {
		unsigned u[4];

		for (int c = 0; c < 4; c++) {
			u[c] = (unsigned)v[c];
		}
		memcpy(at, u, sizeof(u));
		return;
	}
	/*
	 * An integer format takes the NUMBER and not the bits of the float that held it,
	 * which is the convention air_run and air_metal_test both write by
	 * (`written.i[k] = (int32_t)v[c]`, air_metal_test.c) -- and the only one under
	 * which a shader's answer here and its answer there are the same measurement.
	 * Writing 100.0f's bits into a texture2d<int> would have the shader read
	 * 1120403456, a number that looks like a compiler bug.
	 *
	 * RGBA32Sint fell into the four-float case below until a slot could ask for it by
	 * name, which is why the case is here now and not in the first version of this.
	 */
	case MTLPixelFormatRGBA32Sint: {
		int s[4];

		for (int c = 0; c < 4; c++) {
			s[c] = (int)v[c];
		}
		memcpy(at, s, sizeof(s));
		return;
	}
	case MTLPixelFormatR32Uint: {
		unsigned u = (unsigned)v[0];

		memcpy(at, &u, sizeof(u));
		return;
	}
	case MTLPixelFormatR32Float:
		memcpy(at, v, sizeof(float));
		return;
	default:                            /* RGBA32Float and anything else four-float wide */
		memcpy(at, v, 4 * sizeof(float));
		return;
	}
}

/* write_texel backwards, for the one case where the test has to say what landed in a texture
 * rather than what it put there. Only the formats write_texel makes are read: a format it
 * cannot write cannot be in a texture this test seeded.
 */
static void
read_texel(const unsigned char *at, MTLPixelFormat format, float v[4])
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
		for (int c = 0; c < 4; c++) {
			v[c] = at[c] / 255.0f * 1000.0f;   /* write_texel's scale, undone */
		}
		return;
	case MTLPixelFormatRGBA16Float: {
		__fp16 h[4];

		memcpy(h, at, sizeof(h));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)h[c];
		}
		return;
	}
	case MTLPixelFormatRGBA16Uint: {
		unsigned short u[4];

		memcpy(u, at, sizeof(u));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)u[c];
		}
		return;
	}
	case MTLPixelFormatRGBA16Sint: {
		short t[4];

		memcpy(t, at, sizeof(t));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)t[c];
		}
		return;
	}
	case MTLPixelFormatRGBA32Uint: {
		unsigned u[4];

		memcpy(u, at, sizeof(u));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)u[c];
		}
		return;
	}
	case MTLPixelFormatRGBA32Sint: {
		int t[4];

		memcpy(t, at, sizeof(t));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)t[c];
		}
		return;
	}
	case MTLPixelFormatR32Uint: {
		unsigned u;

		memcpy(&u, at, sizeof(u));
		v[0] = (float)u;
		return;
	}
	case MTLPixelFormatR32Float:
		memcpy(v, at, sizeof(float));
		return;
	default:
		memcpy(v, at, 4 * sizeof(float));
		return;
	}
}

/* How wide one texel of a format is, which is what a row's stride is counted in. It was
 * an expression inside the compute path, right for the three formats that path asked
 * for and wrong for R32Float (4 bytes read as 16) the moment a slot could name one. */
static unsigned
texel_bytes_of(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatR32Float:
	case MTLPixelFormatR32Uint:
		return 4;
	case MTLPixelFormatRGBA16Float:
	case MTLPixelFormatRGBA16Uint:
	case MTLPixelFormatRGBA16Sint:
		return 8;
	default:                            /* the four-word RGBA32 formats */
		return 16;
	}
}

static MTLTextureDescriptor *
texture_descriptor(MTLPixelFormat format, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [[[MTLTextureDescriptor alloc] init] autorelease];

	[td setPixelFormat:format];
	[td setTextureType:texture_kind];
	[td setWidth:64];
	[td setHeight:texture_kind == MTLTextureType1D ? 1 : 64];
	[td setDepth:texture_kind == MTLTextureType3D ? 16 : 1];
	[td setArrayLength:1];
	[td setMipmapLevelCount:1];
	[td setSampleCount:1];
	[td setUsage:usage];
	return td;
}

static void
compute_path(id<MTLDevice> dev, id<MTLLibrary> library, const char *only)
{
	NSArray<NSString *> *names = only != NULL ?
	    @[ [NSString stringWithUTF8String:only] ] : [library functionNames];
	unsigned compiled = 0, refused = 0;
	id<MTLComputePipelineState> first = nil;
	NSString *firstName = nil;

	for (NSString *name in names) {
		id<MTLFunction> fn = [library newFunctionWithName:name];
		NSError *err = nil;
		id<MTLComputePipelineState> ps = fn != nil ?
		    [dev newComputePipelineStateWithFunction:fn error:&err] : nil;

		if (ps == nil) {
			refused++;
			printf("drvtest: compute %-32s refused: %s\n", [name UTF8String],
			       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
			continue;
		}
		compiled++;
		printf("drvtest: compute %-32s compiled: %lu threads per group, width %lu, "
		       "%lu bytes of threadgroup memory\n", [name UTF8String],
		       (unsigned long)[ps maxTotalThreadsPerThreadgroup],
		       (unsigned long)[ps threadExecutionWidth],
		       (unsigned long)[ps staticThreadgroupMemoryLength]);
		if (first == nil) {
			first = [ps retain];
			firstName = [name retain];
		}
	}
	printf("drvtest: of %lu functions, %u compiled and %u were refused\n",
	       (unsigned long)[names count], compiled, refused);
	if (first == nil) {
		return;
	}

	/* and one real dispatch, through the queue and the command buffer */
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	printf("drvtest: newCommandQueue gave %s\n",
	       queue != nil ? class_getName(object_getClass(queue)) : "nil");
	if (queue == nil) {
		return;
	}
	/* Eight buffers, all bound. How many a kernel reads is its own business -- the corpus's
	 * compute kernels take between one and five -- and binding fewer than it declares would
	 * make it read a slot nothing was bound to. Metal's own rule is that a shader sees only
	 * what it declared, so binding more is harmless. */
	enum { RW_TEST_BUFFERS = 16 };
	id<MTLBuffer> bufs[RW_TEST_BUFFERS];

	for (int i = 0; i < RW_TEST_BUFFERS; i++) {
		bufs[i] = [dev newBufferWithLength:4096 options:0];
		if (bufs[i] == nil) {
			printf("drvtest: buffer %d could not be made\n", i);
			return;
		}
		if ([bufs[i] contents] != NULL) {
			memset([bufs[i] contents], 0, 4096);
		}
	}
	/* Textures and a sampler, for the kernels that declare them. Eight of each, the same
	 * reasoning as the buffers: a shader sees only what it declared, and binding fewer than
	 * it declares makes it read a slot nothing was bound to. 64x64 RGBA8 is a shape the
	 * corpus's kernels work in; a format the compiler library cannot translate is refused by
	 * name, which is the answer this test wants either way. */
	id<MTLTexture> texs[RW_TEST_BUFFERS] = { nil };
	id<MTLSamplerState> sampler = nil;
	int textures_made = 0;

	/*
	 * Which format to ask for is not a free choice, and the first run of this taught it:
	 * asking for RGBA8Unorm that the shader writes was refused by name -- "Adreno 7c+ Gen 3
	 * cannot give MTLPixelFormatRGBA8Unorm as a MTLTextureType2D the shader writes" -- and
	 * with no texture made every texture-reading kernel then failed with "reads texture 0
	 * and 0 textures were bound", which is a true sentence about the harness and says
	 * nothing about the kernel.
	 *
	 * So the candidates are tried in order and the first that the hardware will give as a
	 * writable image wins. A kernel whose declaration wants another element type is still
	 * refused, and that refusal names what it wanted -- which is the measurement, because
	 * this test is in the position of an application that did not write the shader.
	 */
	const MTLPixelFormat candidates[] = {
		texture_format, MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint,
		MTLPixelFormatRGBA16Float, MTLPixelFormatR32Float, MTLPixelFormatRGBA8Unorm,
	};
	MTLTextureDescriptor *td = nil;

	/* Read and write first, then read alone. The first run of this found that NOTHING is
	 * given as a texture a shader writes -- all five candidates refused -- and with no
	 * texture made every reading kernel failed with "reads texture 0 and 0 textures were
	 * bound", which is a sentence about this harness and not about any kernel. Falling back
	 * to read-only measures the eight that only read; the one that writes is then refused
	 * by the library, which is the honest state of things until that question is settled. */
	static const MTLTextureUsage usages[] = {
		MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
		MTLTextureUsageShaderRead,
	};

	for (size_t u = 0; u < sizeof(usages) / sizeof(usages[0]) && td == nil; u++) {
		for (size_t f = 0; f < sizeof(candidates) / sizeof(candidates[0]) && td == nil;
		     f++) {
			MTLTextureDescriptor *try = texture_descriptor(candidates[f], usages[u]);
			id<MTLTexture> probe;

			probe = [dev newTextureWithDescriptor:try];
			if (probe != nil) {
				[probe release];
				td = try;
				printf("drvtest: textures are format %lu, usage %lu (the first "
				       "this hardware gives)\n", (unsigned long)candidates[f],
				       (unsigned long)usages[u]);
			}
		}
	}
	if (td == nil) {
		printf("drvtest: no candidate pixel format can be made at all here\n");
	}
	for (int i = 0; td != nil && i < RW_TEST_BUFFERS; i++) {
		MTLTextureDescriptor *own = td;

		/* A slot the shader named gets what it asked for; the rest get what the probe
		 * above found the hardware will give. */
		if (i < RW_TEX_SLOTS && slot_format[i] != 0) {
			NSUInteger keep = texture_kind;

			texture_kind = slot_kind[i];
			own = texture_descriptor(slot_format[i], [td usage]);
			texture_kind = keep;
		}
		texs[i] = [dev newTextureWithDescriptor:own];
		if (texs[i] == nil) {
			printf("drvtest: slot %d (format %lu) was refused\n", i,
			       (unsigned long)[own pixelFormat]);
			break;
		}
		textures_made++;
		/*
		 * The texels are air_run's own (`texel_value`, air_run.c:92):
		 *
		 *     r = 100 + x + 1000 * slot,  g = 200 + y,  b = 300 + x + y + z,  a = 400
		 *
		 * and they are copied rather than invented for two reasons. The thousand per slot
		 * is deliberate there -- it makes a shader that reads the wrong slot return a
		 * visibly wrong texel instead of a plausible one, which air_run's comment records
		 * as the third mistake of that shape it met. And using the same values is what
		 * lets a texture-reading kernel's result be compared between softpipe here and
		 * the Adreno at all: the same shader over different texels is not a comparison.
		 */
		static unsigned char row[64 * 16];
		MTLPixelFormat pf = [own pixelFormat];
		unsigned texel_bytes = texel_bytes_of(pf);
		NSUInteger stride = 64 * texel_bytes;

		for (NSUInteger slice = 0; slice < [texs[i] arrayLength] *
		     (texture_kind == MTLTextureTypeCube ? 6 : 1); slice++) {
			for (NSUInteger z = 0; z < [texs[i] depth]; z++) {
				for (unsigned y = 0; y < 64; y++) {
					for (unsigned x = 0; x < 64; x++) {
						float v[4];

						v[0] = 100.0f + x + 1000.0f * i;
						v[1] = 200.0f + y;
						v[2] = 300.0f + x + y + (float)z;
						v[3] = 400.0f;
						write_texel(row + x * texel_bytes, pf, v);
					}
					[texs[i] replaceRegion:MTLRegionMake3D(0, y, z, 64, 1, 1)
						   mipmapLevel:0
							 slice:slice
						     withBytes:row
						   bytesPerRow:stride
						 bytesPerImage:0];
				}
			}
		}
	}
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

	sampler = [dev newSamplerStateWithDescriptor:sd];
	[sd release];
	printf("drvtest: %d of %d textures made, sampler %s\n", textures_made, RW_TEST_BUFFERS,
	       sampler != nil ? class_getName(object_getClass(sampler)) : "nil");

	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

	printf("drvtest: commandBuffer %s, computeCommandEncoder %s\n",
	       cmd != nil ? class_getName(object_getClass(cmd)) : "nil",
	       enc != nil ? class_getName(object_getClass(enc)) : "nil");
	if (cmd == nil || enc == nil) {
		return;
	}
	[enc setComputePipelineState:first];
	for (int i = 0; i < RW_TEST_BUFFERS; i++) {
		[enc setBuffer:bufs[i] offset:0 atIndex:i];
		if (texs[i] != nil) {
			[enc setTexture:texs[i] atIndex:i];
		}
		if (sampler != nil) {
			[enc setSamplerState:sampler atIndex:i];
		}
	}
	/* The same grid air_run uses -- 4x4x1 threads in one threadgroup -- so that the words
	 * this prints can be compared with what softpipe computes for the same kernel on the
	 * same (zero) input. A different thread count is a different computation for any kernel
	 * that writes per-thread, and then "the values agree" would mean nothing. */
	[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	printf("drvtest: dispatched %s: status %lu%s\n", [firstName UTF8String],
	       (unsigned long)[cmd status],
	       [cmd error] != nil ? [[NSString stringWithFormat:@", error: %@",
				      [[cmd error] localizedDescription]] UTF8String] : "");
	int written = 0;

	for (int which = 0; which < RW_TEST_BUFFERS; which++) {
		const unsigned char *bytes = [bufs[which] contents];
		int nonzero = 0;

		if (bytes == NULL) {
			continue;
		}
		for (int i = 0; i < 4096; i++) {
			if (bytes[i] != 0) {
				nonzero++;
			}
		}
		written += nonzero;
		if (nonzero == 0) {
			continue;   /* only the buffers that changed are worth a line */
		}
		printf("drvtest: buffer %d: %d non-zero bytes of 4096, first words %08x %08x "
		       "%08x %08x\n", which, nonzero, ((const unsigned *)bytes)[0],
		       ((const unsigned *)bytes)[1], ((const unsigned *)bytes)[2],
		       ((const unsigned *)bytes)[3]);
	}
	if (written == 0) {
		printf("drvtest: nothing was written to either buffer, so nothing is proven: this "
		       "kernel writes its buffer unconditionally\n");
	}
}

/*
 * One triangle, with one of Apple's own fragment shaders.
 *
 * `clearFragment20` out of AppleMetalGLRenderer's library writes render target 2 (int4) and
 * reads a 48-byte constant buffer at index 14. It is one of the 255 fragment shaders air_run
 * has drawn and compared pixel by pixel (docs/air.md), so there is a reference to compare
 * against -- and these four words ARE that reference, computed by air_run on softpipe with
 * the same (i * 7 + 13) & 0xff fill this function uses:
 *
 *     air_run: the pixel at (0, 0) of render target 2 came back as:
 *       2458616957 2930221209 3401825461 3873429713
 *     air_run: drew 4x4 into r32g32b32a32_uint, 3 render targets
 *
 * They are written down here rather than computed, which is a real weakness: if the shader's
 * meaning ever changes the constant stays and the test lies. tools/air/metalcheck.py is being
 * written to take the comparison off hardcoded numbers; until it does, the provenance above is
 * the only thing making this honest.
 */
/* Apple's own vertex shader, or the library's generated stage. `clearVertex` reads four
 * float4 corners out of a constant buffer at index 14 and indexes them by air.vertex_id --
 * no vertex attributes, which is why it can be served at all: the contract carries no
 * attribute layout, and all three of the corpus's vertex shaders turn out to read a buffer
 * instead (docs/metal.md 10.24). The corners written below are the ones the generated stage
 * covers, so the two roads draw the same pixels and a difference is a shader difference. */
static const char *vertex_function;
static const char *fragment_function = "clearFragment20";
static bool draw_all;
/* --draw-depth, which is the draw with a depth attachment under it and so with the first
 * depth test this port has ever run (depth_path). */
static bool draw_depth;
/* --draw-index, which is the draw that goes through an index buffer -- what every drawing
 * call in a real application is, and what this driver declined until now. It runs AFTER the
 * ordinary draw and over the same pipeline and attachments (index_draws), because the
 * measurement is a comparison: the same three indices drawn from two places in one buffer
 * must put the triangle in two different halves of the attachment. */
static bool draw_index;
/* --draw-attrib: the vertex attribute layout through Metal's own MTLVertexDescriptor, on the
 * hardware. Its own flag and not an option of --draw because what it draws is a different
 * pipeline out of two libraries, and because its verdicts are compared with each other and
 * not with the one picture --draw makes. */
static bool draw_attrib;
/* --argbuf: the argument encoder and the buffer offset, on the device. */
static bool argbuf;
/*
 * Which slice of the library to draw. Not a convenience: the loader keeps the first 256 KB
 * of a boot's log and the last 1 MB, dropping the middle, and the boot's own noise fills
 * most of that first 256 KB -- so about seventy lines of sweep survive per boot. Two runs
 * stopped at exactly 64 shaders and the limit looked like the driver's until the line number
 * gave it away: 2572 of 11480. A range turns that into five honest boots instead of one
 * number that is quietly a quarter of the answer.
 */
static unsigned draw_from, draw_count = 60;
/* --seed-rows writes the draw's 4x4 texture one row at a time, at a non-zero region origin,
 * which is how this test used to do it always. It is a flag now because that made every
 * measurement of that texture depend on the driver's origin.y, and because four rows landing
 * in one place is the second explanation of the flip in docs/metal.md 10.26.
 */
/* Metal's own MTLCommandBufferStatus: NotEnqueued 0, Enqueued 1, Committed 2, Scheduled 3,
 * Completed 4, Error 5. This test used to compare against the literal 6, which is what the
 * driver answered until it was corrected to Metal's numbering -- and 6 is not a status at
 * all, so an application comparing with MTLCommandBufferStatusCompleted saw completion the
 * moment -commit returned, before the GPU fence was paid. The driver prints its numbering
 * once per process; a log without that line was written by the old one.
 */
#define RW_CB_COMPLETED 4

static bool seed_rows;
/* --no-texdecl makes the sweep bind its single texture set to every shader, as it did
 * before each shader was given what it declares. It exists because the first sweep with
 * per-shader textures stopped at its third shader, and a sweep that cannot finish measures
 * nothing: with this, the 256 shaders that declare no texture are measurable again while
 * the 15 that do are measured one process at a time (userland/rc.jobs.metaltex).
 */
static bool use_decl = true;

/* 4x4, and 4x4x4 for a 3D slot, because that is air_run's TEX_SIZE and the shape it
 * draws against (air_run.c: a 2D texture 4x4, a 3D one 4x4x4). A texture of another
 * size here would make the device's pixel and the host's reference two different
 * measurements with one name. */
enum { DRAW_TEX = 4 };

/* A descriptor for one fragment-stage texture of the kind a slot asks for. Separate from
 * texture_descriptor() above, which is the compute path's 64x64 and reads the one global
 * kind: here the kind is per slot and comes from the shader. */
static MTLTextureDescriptor *
draw_texture_descriptor(NSUInteger type, MTLPixelFormat format, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [[[MTLTextureDescriptor alloc] init] autorelease];

	[td setPixelFormat:format];
	[td setTextureType:type];
	[td setWidth:DRAW_TEX];
	[td setHeight:type == MTLTextureType1D ? 1 : DRAW_TEX];
	[td setDepth:type == MTLTextureType3D ? DRAW_TEX : 1];
	[td setArrayLength:1];
	[td setMipmapLevelCount:1];
	[td setSampleCount:1];
	[td setUsage:usage];
	return td;
}

/*
 * air_run's texels into one texture, every slice and every depth of it:
 *
 *     r = 100 + x + 1000 * slot,  g = 200 + y,  b = 300 + x + y + z,  a = 400
 *
 * The thousand per slot is why a shader that reads the wrong slot comes back visibly
 * wrong instead of plausibly right, and using air_run's own numbers is what lets the
 * pixel this draw produces be compared with the one softpipe produces at all.
 */
static void
fill_draw_texture(id<MTLTexture> tex, unsigned slot)
{
	MTLPixelFormat pf = [tex pixelFormat];
	unsigned bytes = texel_bytes_of(pf);
	unsigned char row[DRAW_TEX * 16];
	NSUInteger slices = [tex textureType] == MTLTextureTypeCube ? 6 : [tex arrayLength];

	for (NSUInteger slice = 0; slice < slices; slice++) {
		for (NSUInteger z = 0; z < [tex depth]; z++) {
			for (NSUInteger y = 0; y < [tex height]; y++) {
				for (NSUInteger x = 0; x < [tex width]; x++) {
					float v[4] = { 100.0f + x + 1000.0f * slot,
						       200.0f + y, 300.0f + x + y + z, 400.0f };

					write_texel(row + x * bytes, pf, v);
				}
				[tex replaceRegion:MTLRegionMake3D(0, y, z, [tex width], 1, 1)
				       mipmapLevel:0
					     slice:slice
					 withBytes:row
				       bytesPerRow:[tex width] * bytes
				     bytesPerImage:0];
			}
		}
	}
}

/*
 * The pixel format to bind for a declared element type: the 32-bit RGBA format of the
 * declared class, always.
 *
 * The width is deliberately not followed. What a binding is refused for is the CLASS --
 * "reads texture 0 as a texture2d<uint> and MTLPixelFormatRGBA32Float is a
 * floating-point or normalised format" -- and the three 32-bit formats are the ones
 * air_run and air_metal_test bind, so a pixel measured here and a pixel computed there
 * are the same measurement. Every texel value in the pattern (100 to 1400) is exact in
 * half and fits in a ushort, so a shader declaring `half` or `ushort` reads the same
 * numbers out of a 32-bit texture as out of a 16-bit one. --texslots still asks for a
 * 16-bit format by name when the point is the format rather than the shader.
 */
static MTLPixelFormat
format_for_element(unsigned element)
{
	switch (element) {
	case RWA_ELEMENT_INT:
		return MTLPixelFormatRGBA32Sint;
	case RWA_ELEMENT_UINT:
		return MTLPixelFormatRGBA32Uint;
	case RWA_ELEMENT_FLOAT:
		return MTLPixelFormatRGBA32Float;
	default:
		/* Nothing states an element type, so the slot is bound what this sweep bound
		 * every slot before it could ask -- draw_path's own dtex[] format. Not
		 * texture_format, which is the COMPUTE path's default: a pixel that changed
		 * because a fallback moved would look like a shader that changed. */
		return MTLPixelFormatRGBA32Uint;
	}
}

static void
release_slots(id<MTLTexture> *tex, unsigned slots)
{
	for (unsigned i = 0; i < slots; i++) {
		[tex[i] release];
		tex[i] = nil;
	}
}

/*
 * The textures one fragment shader declares: made, filled with air_run's texels, and
 * left in out[slot] for the slots it named. False and a sentence when any of that
 * cannot be done, and then the sentence IS the measurement for that shader -- "it
 * declares texture 0 as texture2d_ms<float, read> and no MTLTextureType 4 ... could be
 * made" is a limit of the driver (it binds no multisample texture), where the old
 * "declares texture 0 as a texture2d_ms and the texture bound there is a 2d" was a
 * limit of this harness.
 *
 * Without the query (an older compiler library, or a boot with no GPU) it does nothing
 * and says so by leaving every slot nil: the caller's single --texslots binding then
 * stands, which is exactly the behaviour this sweep had before.
 */
static bool
textures_for_shader(id<MTLDevice> dev, id<MTLFunction> fn, id<MTLTexture> *out,
		    unsigned slots, unsigned *declared, char *why, size_t why_size)
{
	*declared = 0;
	if (rwa_module_textures == NULL) {
		return true;
	}
	NSData *bc = function_bitcode(fn);

	if (bc == nil) {
		snprintf(why, why_size, "it does not answer bitcodeData, so there is no "
			 "module to read a declaration out of");
		return false;
	}
	struct rwa_texture_decl decl[RW_TEX_SLOTS];
	unsigned n = 0;
	char reason[256] = "";

	if (rwa_module_textures(air_device, [bc bytes], [bc length], NULL, decl,
				RW_TEX_SLOTS, &n, reason, sizeof(reason)) != 0) {
		snprintf(why, why_size, "%s", reason);
		return false;
	}
	*declared = n;
	for (unsigned k = 0; k < n; k++) {
		unsigned slot = decl[k].index;

		if (slot >= slots) {
			snprintf(why, why_size, "it declares texture %u and this harness binds "
				 "%u slots", slot, slots);
			return false;
		}
		/* --texslots stays an override, deliberately: binding the wrong thing on
		 * purpose is how the driver's refusal gets tested, and that can only be on
		 * purpose if a person can still ask for it. */
		bool overridden = slot_format[slot] != 0;

		/* A slot the module describes in no way at all -- a computed binding with no
		 * instruction and no air.arg_type_name -- is left to the caller's own
		 * binding rather than replaced by a guess of the same shape. No shader of the
		 * corpus is in this case; making a texture for it anyway would change what
		 * such a shader is bound without anything having asked.
		 */
		if (!overridden && !decl[k].type_known &&
		    decl[k].element == RWA_ELEMENT_UNKNOWN) {
			continue;
		}
		MTLPixelFormat format = overridden ? slot_format[slot]
						   : format_for_element(decl[k].element);
		NSUInteger type = overridden ? slot_kind[slot]
			: (decl[k].type_known ? (NSUInteger)decl[k].type : texture_kind);
		MTLTextureDescriptor *td = draw_texture_descriptor(type, format,
		    (MTLTextureUsage)decl[k].usage);

		out[slot] = [dev newTextureWithDescriptor:td];
		if (out[slot] == nil) {
			/* The driver has already printed the compiler library's own sentence
			 * about what it would not make; this one says which slot of which
			 * shader asked, which the library cannot know. */
			snprintf(why, why_size, "it declares texture %u as %s and no "
				 "MTLTextureType %lu of MTLPixelFormat %lu could be made for "
				 "it", slot,
				 decl[k].metal_type[0] != '\0' ? decl[k].metal_type :
				 "a texture whose metadata carries no type name",
				 (unsigned long)type, (unsigned long)format);
			return false;
		}
		fill_draw_texture(out[slot], slot);
	}
	return true;
}

static void draw_every(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
		       MTLRenderPassDescriptor *rp, id<MTLBuffer> uniform, id<MTLTexture> *dtex,
		       id<MTLSamplerState> dsamp, id<MTLTexture> *targets);

/*
 * The indexed draws, run after the ordinary one and over its pipeline, attachments and
 * bindings -- so that the only thing that differs between the reference picture and these
 * is the index buffer. docs/metal.md 10.35 is the experiment and air_metal_test
 * --draw-index is its reference implementation on softpipe, step for step.
 *
 * The index buffer holds 0 1 2 3 1 2 3. Three indices out of it make one triangle of the
 * quad: 0,1,2 is the half of the attachment below the diagonal, whose right angle is the
 * pixel at (0, 0), and 1,2,3 is the other half, whose right angle is (3, 3). Those two
 * draws differ in NOTHING but indexBufferOffset, so a driver that ignored the index buffer
 * or the offset would draw the first triangle twice and (3, 3) would come back as the clear
 * colour both times. The four pixels on the diagonal have their centres exactly on the
 * shared edge and belong to whichever triangle the rasteriser's fill rule gives them, so
 * the covered count is printed and nothing is concluded from it -- the two right angles are
 * the verdict.
 */
static const uint16_t index_list[7] = { 0, 1, 2, 3, 1, 2, 3 };

/* Whether pixel (x, y) of a 4x4 readback was drawn at all. The clear colour is zero and
 * clearFragment20's first word is 2458616957, so a non-zero first word is a covered pixel
 * -- which the caller's own four words have already confirmed before this is used. */
static bool
index_at(const unsigned grid[16][4], unsigned x, unsigned y)
{
	return grid[y * 4 + x][0] != 0;
}

/* How many of the 64 words of two pictures differ: the whole attachment and not one pixel,
 * because two triangles that differ only along the diagonal differ in four pixels. */
static unsigned
index_diff(const unsigned a[16][4], const unsigned b[16][4])
{
	unsigned n = 0;

	for (unsigned i = 0; i < 16; i++) {
		for (unsigned c = 0; c < 4; c++) {
			n += a[i][c] != b[i][c];
		}
	}
	return n;
}

/* One picture, as four rows of four, with the two corners the verdicts are made of. */
static void
index_print(unsigned n, const char *what, const unsigned grid[16][4])
{
	unsigned covered = 0;

	printf("drvtest: index: %u. %s: ", n, what);
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			printf("%c", index_at(grid, x, y) ? '#' : '.');
			covered += index_at(grid, x, y);
		}
		printf("%s", y < 3 ? "/" : "");
	}
	printf("  %u of 16 pixels, (0,0) %s, (3,3) %s\n", covered,
	       index_at(grid, 0, 0) ? "drawn" : "clear",
	       index_at(grid, 3, 3) ? "drawn" : "clear");
}

static void
index_draws(id<MTLDevice> dev, id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	    id<MTLRenderPipelineState> pipeline, id<MTLBuffer> uniform,
	    id<MTLBuffer> corners, id<MTLBuffer> coords, id<MTLTexture> *dtex,
	    id<MTLSamplerState> dsamp, id<MTLTexture> *targets)
{
	/* Step 0 is the draw the caller has just made and whose pixels are still on the
	 * attachment: the non-indexed strip of four. Everything below is compared with it or
	 * with another step of it. */
	static const struct {
		const char *what;
		bool wide;                /* MTLIndexTypeUInt32 rather than UInt16 */
		NSUInteger count, offset;
		NSInteger baseVertex;
		bool must_fail;
	} step[] = {
		{ "the indexed strip of 4 [0 1 2 3]",             false, 4, 0,     0, false },
		{ "indices [0 1 2]: the half below the diagonal", false, 3, 0,     0, false },
		{ "indices [1 2 3]: the half above it",           false, 3, 4 * 2, 0, false },
		{ "indices [0 1 2] with baseVertex 1",            false, 3, 0,     1, false },
		{ "indices [0 1 2] as UInt32",                    true,  3, 0,     0, false },
		{ "indices [1 2 3] as UInt32",                    true,  3, 4 * 4, 0, false },
		{ "UInt16 at an odd offset, which must be refused",   false, 3, 1, 0, true },
		{ "8 indices of a buffer that holds 7, refused too",  false, 8, 0, 0, true },
	};
	enum { STEPS = sizeof(step) / sizeof(step[0]) };
	unsigned grid[STEPS + 1][16][4];
	uint32_t wide[7];
	unsigned drew = 0, refused = 0;

	memset(grid, 0, sizeof(grid));
	[targets[2] getBytes:grid[0]
		 bytesPerRow:4 * 16
		  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		 mipmapLevel:0];
	for (unsigned i = 0; i < 7; i++) {
		wide[i] = index_list[i];
	}
	id<MTLBuffer> ib16 = [dev newBufferWithBytes:index_list length:sizeof(index_list)
					     options:0];
	id<MTLBuffer> ib32 = [dev newBufferWithBytes:wide length:sizeof(wide) options:0];

	if (ib16 == nil || ib32 == nil) {
		printf("drvtest: index: no index buffer: %s\n",
		       ib16 == nil ? "newBufferWithBytes: gave nil" : "the UInt32 one is nil");
		return;
	}
	printf("drvtest: index: the index buffer is a %s of %lu bytes, holding "
	       "0 1 2 3 1 2 3\n", class_getName(object_getClass(ib16)),
	       (unsigned long)[ib16 length]);
	printf("drvtest: index: step 0 is the non-indexed strip of 4 already drawn above\n");
	index_print(0, "the non-indexed strip of 4", grid[0]);

	for (unsigned s = 0; s < STEPS; s++) {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: index: %s: no encoder\n", step[s].what);
			return;
		}
		[enc setRenderPipelineState:pipeline];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			[enc setVertexBuffer:i == 1 ? coords : corners offset:0 atIndex:i];
			if (i < 4 && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		/* The longest form for every step, because the shorter two ARE this one with
		 * instanceCount 1, baseVertex 0 and baseInstance 0 -- Metal's own header says
		 * as much -- and one call site is one thing to read. */
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
				indexCount:step[s].count
				 indexType:step[s].wide ? MTLIndexTypeUInt32
							: MTLIndexTypeUInt16
			       indexBuffer:step[s].wide ? ib32 : ib16
			 indexBufferOffset:step[s].offset
			     instanceCount:1
				baseVertex:step[s].baseVertex
			      baseInstance:0];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		if ([cmd status] != RW_CB_COMPLETED) {
			printf("drvtest: index: %u. %s: status %lu%s%s\n", s + 1, step[s].what,
			       (unsigned long)[cmd status], [cmd error] != nil ? " -- " : "",
			       [cmd error] != nil
				   ? [[[cmd error] localizedDescription] UTF8String] : "");
			refused += step[s].must_fail;
			if (!step[s].must_fail) {
				printf("drvtest: index:    and that step was meant to draw\n");
			}
			continue;
		}
		if (step[s].must_fail) {
			printf("drvtest: index: %u. %s: DREW, and it should not have\n", s + 1,
			       step[s].what);
			continue;
		}
		drew++;
		[targets[2] getBytes:grid[s + 1]
			 bytesPerRow:4 * 16
			  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			 mipmapLevel:0];
		index_print(s + 1, step[s].what, grid[s + 1]);
	}

	/*
	 * And the comparisons, which are the measurement. Each names what the other answer
	 * would have meant, so a wrong run does not need this file to be read to be
	 * understood.
	 */
	printf("drvtest: index: the indexed strip of 4 differs from the non-indexed one in "
	       "%u of 64 words -- the CONTROL, which a draw ignoring the index buffer passes "
	       "as well\n", index_diff(grid[0], grid[1]));
	printf("drvtest: index: the two halves differ in %u of 64 words, (0,0) is %s in the "
	       "first and %s in the second, (3,3) is %s and %s: %s\n",
	       index_diff(grid[2], grid[3]),
	       index_at(grid[2], 0, 0) ? "drawn" : "clear",
	       index_at(grid[3], 0, 0) ? "drawn" : "clear",
	       index_at(grid[2], 3, 3) ? "drawn" : "clear",
	       index_at(grid[3], 3, 3) ? "drawn" : "clear",
	       (index_at(grid[2], 0, 0) && !index_at(grid[2], 3, 3) &&
		index_at(grid[3], 3, 3) && !index_at(grid[3], 0, 0))
		   ? "THE INDEX BUFFER AND THE OFFSET ARE READ ON THE ADRENO"
		   : "a right angle came back on the wrong side, which is what an ignored "
		     "index buffer or offset gives: the first triangle twice");
	printf("drvtest: index: baseVertex 1 over [0 1 2] differs from [1 2 3] in %u of 64 "
	       "words and from [0 1 2] in %u: %s\n", index_diff(grid[4], grid[3]),
	       index_diff(grid[4], grid[2]),
	       index_diff(grid[4], grid[3]) == 0
		   ? "baseVertex is added to every index"
		   : index_diff(grid[4], grid[2]) == 0 ? "baseVertex was DROPPED"
						       : "neither picture, which wants reading");
	printf("drvtest: index: the UInt32 halves differ from the UInt16 ones in %u and %u of "
	       "64 words: %s\n", index_diff(grid[5], grid[2]), index_diff(grid[6], grid[3]),
	       index_diff(grid[5], grid[2]) == 0 && index_diff(grid[6], grid[3]) == 0
		   ? "the index type is honoured"
		   : "a 32-bit index read two bytes at a time is 0, 0, 1 -- a triangle of no "
		     "area, which covers nothing");
	printf("drvtest: index: %u of 6 drew and %u of 2 were refused by name\n", drew,
	       refused);
	fflush(stdout);
}

/*
 * --draw-attrib: the vertex attribute layout ON THE HARDWARE, through Metal's own API.
 *
 * docs/metal.md 10.44 put MTLVertexDescriptor into the contract and measured it on this
 * machine's softpipe: the offsets, the stride, the formats, the buffer index, the step
 * function and its rate each changed what came back, and the element order proved to be the
 * shader's driver_location order and not Metal's attribute order. What that measurement
 * could NOT touch is the only part made of hardware: the VFD -- Adreno's vertex fetch unit
 * -- which softpipe does not have at all. So three things in it were left as honest
 * negatives, and this mode is here to settle them on the phone:
 *
 *   - the SWAP. MTLVertexFormatUChar4Normalized_BGRA becomes PIPE_FORMAT_B8G8R8A8_UNORM, and
 *     nothing about the byte order is in the fetch's format field: freedreno puts it in
 *     VFD_DECODE_INSTR.swap, which fd6_vertex_swap() reads off the pipe_format. Measured by
 *     storing the coordinate so that it is only right if the swap happens: the first byte of
 *     the element is ZERO and the coordinate lives in bytes 2 and 1, which the BGRA order
 *     delivers as x and y. Without the swap the shader's x is that zero, every pixel samples
 *     column 0 of the texture, and the x gradient of the picture disappears.
 *   - the INSTANCED bit and STEP_RATE, which are two more fields of the same register
 *     (VFD_DECODE_INSTR.instanced and VFD_DECODE[k].STEP_RATE) and have never been
 *     programmed by this driver. Measured in the picture, because no colour attachment can
 *     show an instance count: the coordinate comes out of a SECOND stream that steps per
 *     instance, so all four vertices of one instance share one coordinate and every pixel
 *     holds ONE texel; per vertex instead, the four vertices get four coordinates and the
 *     picture is a gradient of four texels.
 *   - the PLACEHOLDER element for an attribute gated by a function constant, which 10.44
 *     proved accepted and not fetched: on the host the pipeline for such a module dies
 *     inside softpipe before anything can be drawn with it (the `_lph` variants on the
 *     16-bit narrowing of a `half4` attribute, the `_lpf` ones on `store_scratch`). Here ir3 compiles it, so the question becomes
 *     the one that matters: does the GPU come back. An unconfigured attribute that got NO
 *     element at all would leave an empty VFD slot, and an empty slot fetches from address 0
 *     -- which is the shape of the fault that cost this port a boot in 10.18.
 *
 * And it carries over the thing that makes the host measurement evidence rather than a
 * picture: THE DECOY. The module's own [[attribute(1)]] is air.arg_unused -- nothing reads
 * it -- and this mode configures it anyway, pointing at bytes that hold a different texture
 * coordinate. An element array built in Metal's attribute order, or over all three
 * attributes in order, feeds that layout to attribute 3's fetch: the geometry stays
 * perfectly right and every pixel comes back (100, 200, 300, 400). That is the only wrong
 * answer here that a coverage check could not see.
 *
 * The pipeline is Apple's own code on both sides and out of two libraries, which is a legal
 * Metal pipeline and the shape a real one has: QuartzCore's std_vert0_lpf (position is
 * `mvp_matrix * attribute(0)`, so an IDENTITY matrix in its [[buffer(2)]] makes the position
 * the attribute verbatim, and its [[attribute(3)]] `color` becomes the first varying) over
 * AppleMetalGLRenderer's texturedFragment2DUInt (samples texture 0 at that varying and
 * writes the texel to four RGBA32Uint targets). So the COVERAGE measures attribute 0 and
 * every WORD measures attribute 3: pixel (x, y) must hold the texel its own interpolated
 * coordinate lands in, (100 + x, 200 + y, 300 + x + y, 400), up to the vertical orientation
 * of 10.32 -- both orientations are counted and the one that matched is printed, because
 * which attribute fed the fetch has the same answer either way.
 *
 *   metal_drv_test --draw-attrib [--vertexlib PATH] [--vertex NAME] [--gated NAME]
 *                  [--fragment NAME] FRAGMENTLIB
 */

#define ATTR_N 8          /* elements in the stream */
#define ATTR_STREAM 5     /* Metal buffer index of the interleaved stream */
#define ATTR_INSTANCE 6   /* ... of the per-instance stream */
#define ATTR_OTHER 7      /* ... and of the same stream bound somewhere else */

static const char *attrib_vertex_lib =
    "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
static const char *attrib_gated_function = "fixed_vert_lpf_spc";

/*
 * The eight elements of the stream: the four corners of the full-screen quad with a texture
 * coordinate running the whole [0, 1] square, then four at half the distance from the origin
 * whose coordinate is the far corner. The second four exist for one step -- the same data
 * read at twice the stride, where the fetch walks 0, 2, 4, 6 and must produce a picture
 * nothing else here produces.
 */
static const float attrib_pos[ATTR_N][4] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f }, {  1.0f, -1.0f, 0.0f, 1.0f },
	{ -1.0f,  1.0f, 0.0f, 1.0f }, {  1.0f,  1.0f, 0.0f, 1.0f },
	{ -0.5f, -0.5f, 0.0f, 1.0f }, {  0.5f, -0.5f, 0.0f, 1.0f },
	{ -0.5f,  0.5f, 0.0f, 1.0f }, {  0.5f,  0.5f, 0.0f, 1.0f },
};
static const float attrib_uv[ATTR_N][2] = {
	{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f },
	{ 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f },
};

/* What the DECOY points at, in every element: the centre of texel (0, 0), so an
 * implementation that fed this layout to attribute 3's fetch comes back with
 * (100, 200, 300, 400) in all sixteen pixels -- one number, named in advance, and not a
 * plausible gradient. */
static const float attrib_decoy_uv[2] = { 0.125f, 0.125f };

/* The per-instance stream: four coordinates, two in opposite corners of the texture and two
 * in the other two. Instance 1's is the far corner, which is the texel the instanced step
 * must leave in every pixel; a stream that stepped per vertex would hand these four to the
 * four vertices of ONE instance and the picture would be a gradient instead. */
static const float attrib_instance_uv[4][2] = {
	{ 0.125f, 0.125f }, { 0.875f, 0.875f }, { 0.125f, 0.875f }, { 0.875f, 0.125f },
};

struct attrib_slot {
	NSUInteger format;   /* MTLVertexFormat */
	NSUInteger offset;
	NSUInteger buffer;
};

struct attrib_layout {
	const char *what;
	NSUInteger stride;
	struct attrib_slot pos, uv, decoy;
};

/* Metal's Short4Normalized is value / 32767, so 1.0 is 32767 exactly and the quad's corners
 * are exact; the half-size ones are not, and nothing here compares them. */
static int16_t
attrib_snorm16(float v)
{
	float s = v * 32767.0f;

	return (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

/* UChar4Normalized: 255 is 1.0 exactly, so a coordinate of 0 or 1 survives the encoding. */
static unsigned char
attrib_unorm8(float v)
{
	return (unsigned char)(v * 255.0f + 0.5f);
}

/*
 * The stream, written in whatever encoding the layout names. Always ATTR_N elements of the
 * layout's own stride, so a step that reads it at twice the stride has something defined to
 * read instead of running off the end (and being refused for it, which is a different
 * measurement and has its own step).
 *
 * The BGRA case is the swap measurement and is the only place here where the bytes are NOT
 * in the order the components are: byte 0 is zero, byte 1 is v and byte 2 is u, because
 * MTLVertexFormatUChar4Normalized_BGRA delivers (x, y, z, w) = (byte2, byte1, byte0, byte3).
 * If the swap is not programmed the shader's x is byte 0 -- the zero -- and the picture
 * loses its x gradient entirely.
 */
static void
attrib_fill(unsigned char *mem, size_t size, const struct attrib_layout *l)
{
	memset(mem, 0, size);
	for (unsigned i = 0; i < ATTR_N; i++) {
		unsigned char *e = mem + (size_t)i * l->stride;

		if (l->pos.format == MTLVertexFormatFloat4) {
			float v[4] = { attrib_pos[i][0], attrib_pos[i][1], attrib_pos[i][2],
				       attrib_pos[i][3] };

			memcpy(e + l->pos.offset, v, sizeof(v));
		} else {
			int16_t v[4] = { attrib_snorm16(attrib_pos[i][0]),
					 attrib_snorm16(attrib_pos[i][1]),
					 attrib_snorm16(attrib_pos[i][2]),
					 attrib_snorm16(attrib_pos[i][3]) };

			memcpy(e + l->pos.offset, v, sizeof(v));
		}
		if (l->uv.format == MTLVertexFormatFloat4) {
			float v[4] = { attrib_uv[i][0], attrib_uv[i][1], 0.0f, 1.0f };

			memcpy(e + l->uv.offset, v, sizeof(v));
		} else if (l->uv.format == MTLVertexFormatUChar4Normalized_BGRA) {
			unsigned char v[4] = { 0, attrib_unorm8(attrib_uv[i][1]),
					       attrib_unorm8(attrib_uv[i][0]), 255 };

			memcpy(e + l->uv.offset, v, sizeof(v));
		} else {
			unsigned char v[4] = { attrib_unorm8(attrib_uv[i][0]),
					       attrib_unorm8(attrib_uv[i][1]), 0, 255 };

			memcpy(e + l->uv.offset, v, sizeof(v));
		}
		if (l->decoy.format == MTLVertexFormatFloat2) {
			float v[2] = { attrib_decoy_uv[0], attrib_decoy_uv[1] };

			memcpy(e + l->decoy.offset, v, sizeof(v));
		} else {
			/* 0.125 as a half: 2^-3 is exponent field 15 - 3 = 12 with a zero
			 * mantissa, which is 0x3000. Written out because this file has no
			 * float-to-half of its own and the value is a constant. */
			uint16_t v[2] = { 0x3000, 0x3000 };

			memcpy(e + l->decoy.offset, v, sizeof(v));
		}
	}
}

/*
 * The layout as Metal's own object. Both of its arrays are filled at Metal's own indices,
 * and attribute 1 -- the one the module marks air.arg_unused -- is configured on purpose:
 * that is the decoy, and a descriptor configuring an attribute no shader reads is legal and
 * ignored by Metal.
 */
static MTLVertexDescriptor *
attrib_descriptor(const struct attrib_layout *l, NSUInteger step_function,
		  NSUInteger step_rate, NSInteger instance_buffer)
{
	MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
	const struct attrib_slot *slot[3] = { &l->pos, &l->decoy, &l->uv };
	const unsigned at[3] = { 0, 1, 3 };

	for (unsigned i = 0; i < 3; i++) {
		MTLVertexAttributeDescriptor *a =
		    [[vd attributes] objectAtIndexedSubscript:at[i]];

		[a setFormat:(MTLVertexFormat)slot[i]->format];
		[a setOffset:slot[i]->offset];
		[a setBufferIndex:slot[i]->buffer];
	}
	MTLVertexBufferLayoutDescriptor *bl =
	    [[vd layouts] objectAtIndexedSubscript:l->pos.buffer];

	[bl setStride:l->stride];
	[bl setStepFunction:MTLVertexStepFunctionPerVertex];
	[bl setStepRate:1];
	if (instance_buffer >= 0) {
		MTLVertexBufferLayoutDescriptor *il =
		    [[vd layouts] objectAtIndexedSubscript:(NSUInteger)instance_buffer];

		[il setStride:8];        /* one float2 coordinate per element */
		[il setStepFunction:(MTLVertexStepFunction)step_function];
		[il setStepRate:step_rate];
	}
	return vd;
}

/* One pipeline over four RGBA32Uint attachments, which is what texturedFragment2DUInt
 * writes. The reason for a sentence rather than a nil is the whole of 10.44: every refusal
 * in this path is the compiler library's own and names what was asked for. */
static id<MTLRenderPipelineState>
attrib_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
		MTLVertexDescriptor *vd, const char *what, bool quiet)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	[pd setVertexDescriptor:vd];
	for (unsigned i = 0; i < 4; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];

	[pd release];
	if (ps == nil && !quiet) {
		printf("drvtest: attrib: %s got no pipeline: %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	if (ps == nil && quiet) {
		printf("drvtest: attrib:   %s: refused -- %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/* Everything a draw of this mode binds, so that eight draws differ in the layout and in
 * nothing else. The vertex ARGUMENTS (the matrices) and the vertex STREAMS come off one
 * table, as Metal has one; the pipeline's own mask is what splits them in the driver. */
struct attrib_bind {
	id<MTLCommandQueue> queue;
	MTLRenderPassDescriptor *rp;
	id<MTLTexture> target;                /* the attachment read back */
	id<MTLBuffer> argument[8];            /* by Metal buffer index: the matrices */
	id<MTLBuffer> stream[8];              /* by Metal buffer index */
	NSUInteger stream_offset[8];
	id<MTLTexture> tex;
	id<MTLSamplerState> samp;
};

/*
 * One draw and the picture it left, into attachments cleared to zero by the pass. Returns
 * false and says where it stopped: this driver RECORDS the draw and performs it on -commit
 * (10.13), so "no encoder", "status" and "the GPU did not finish" are three different
 * answers and the log has to tell them apart.
 */
static bool
attrib_draw(const struct attrib_bind *b, id<MTLRenderPipelineState> ps,
	    NSUInteger instances, id<MTLBuffer> indices, NSUInteger index_count,
	    NSUInteger index_offset, unsigned grid[16][4], const char *what)
{
	id<MTLCommandBuffer> cmd = [b->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:b->rp];

	if (enc == nil) {
		printf("drvtest: attrib: %s got no encoder\n", what);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < 8; i++) {
		if (b->argument[i] != nil) {
			[enc setVertexBuffer:b->argument[i] offset:0 atIndex:i];
		}
		if (b->stream[i] != nil) {
			[enc setVertexBuffer:b->stream[i] offset:b->stream_offset[i] atIndex:i];
		}
	}
	[enc setFragmentTexture:b->tex atIndex:0];
	[enc setFragmentSamplerState:b->samp atIndex:0];
	if (indices != nil) {
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
				indexCount:index_count
				 indexType:MTLIndexTypeUInt16
			       indexBuffer:indices
			 indexBufferOffset:index_offset
			     instanceCount:instances
				baseVertex:0
			      baseInstance:0];
	} else {
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
			vertexStart:0
			vertexCount:4
		      instanceCount:instances];
	}
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: attrib: %s: status %lu%s%s\n", what, (unsigned long)[cmd status],
		       [cmd error] != nil ? " -- " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
		return false;
	}
	memset(grid, 0, sizeof(unsigned[16][4]));
	[b->target getBytes:grid
		bytesPerRow:4 * 16
		 fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		mipmapLevel:0];
	return true;
}

/*
 * What the picture says, counted rather than eyeballed. 10.32 established that the texture
 * comes back mirrored vertically against a naive reading of the coordinate, so BOTH
 * orientations are counted and the one that matched is printed: what this mode is about is
 * WHICH attribute fed the fetch, and that question has the same answer either way.
 */
struct attrib_verdict {
	unsigned x_gradient;    /* pixels whose first word is 100 + x */
	unsigned mirrored;      /* ... and whose second is 200 + (3 - y) */
	unsigned upright;       /* ... or 200 + y */
	unsigned decoy_texel;   /* pixels holding texel (0, 0): the decoy's own answer */
	unsigned column0;       /* pixels holding texel (0, y): what a dropped BGRA swap gives */
	unsigned uniform;       /* pixels equal to the pixel at (0, 0) */
	unsigned covered;
};

static void
attrib_judge(const unsigned grid[16][4], struct attrib_verdict *v)
{
	memset(v, 0, sizeof(*v));
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			const unsigned *w = grid[y * 4 + x];

			v->covered += w[0] != 0;
			v->x_gradient += w[0] == 100 + x;
			v->mirrored += w[0] == 100 + x && w[1] == 200 + (3 - y);
			v->upright += w[0] == 100 + x && w[1] == 200 + y;
			v->decoy_texel += w[0] == 100 && w[1] == 200 && w[2] == 300 &&
					  w[3] == 400;
			v->column0 += w[0] == 100;
			v->uniform += w[0] == grid[0][0] && w[1] == grid[0][1] &&
				      w[2] == grid[0][2] && w[3] == grid[0][3];
		}
	}
}

static void
attrib_print(const char *what, const unsigned grid[16][4], const struct attrib_verdict *v)
{
	printf("drvtest: attrib: %s\n", what);
	printf("drvtest: attrib:   coverage ");
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			printf("%c", index_at(grid, x, y) ? '#' : '.');
		}
		printf("%s", y < 3 ? "/" : "");
	}
	printf("  %u of 16; (0,0) = %u %u %u %u, (3,3) = %u %u %u %u\n", v->covered,
	       grid[0][0], grid[0][1], grid[0][2], grid[0][3],
	       grid[15][0], grid[15][1], grid[15][2], grid[15][3]);
	printf("drvtest: attrib:   %u of 16 hold texel x, %u hold (x, 3-y), %u hold (x, y), "
	       "%u hold texel (0,0), %u hold column 0, %u are the same as (0,0)\n",
	       v->x_gradient, v->mirrored, v->upright, v->decoy_texel, v->column0, v->uniform);
}

/* Whether a picture is the texel grid the layout describes: every pixel its own texel, in
 * one of the two vertical orientations, over the whole attachment. */
static bool
attrib_is_grid(const struct attrib_verdict *v)
{
	return v->covered == 16 && v->x_gradient == 16 && (v->mirrored == 16 || v->upright == 16);
}

static void
attrib_path(id<MTLDevice> dev, const char *fragment_lib)
{
	NSError *err = nil;
	/* Two libraries, two MTLLibrary objects, one pipeline -- which Metal allows and a real
	 * pipeline does: the attributes are QuartzCore's and the shader that turns their
	 * varying into a readable texel is the GL renderer's. */
	NSData *vdata = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:attrib_vertex_lib]];
	NSData *fdata = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:fragment_lib]];

	if (vdata == nil || fdata == nil) {
		printf("drvtest: attrib: cannot read %s\n", vdata == nil ? attrib_vertex_lib
								         : fragment_lib);
		return;
	}
	dispatch_data_t vdd = dispatch_data_create([vdata bytes], [vdata length], NULL,
						   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	dispatch_data_t fdd = dispatch_data_create([fdata bytes], [fdata length], NULL,
						   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> vlib = [dev newLibraryWithData:vdd error:&err];
	id<MTLLibrary> flib = [dev newLibraryWithData:fdd error:&err];

	if (vlib == nil || flib == nil) {
		printf("drvtest: attrib: newLibraryWithData: gave nil for %s\n",
		       vlib == nil ? attrib_vertex_lib : fragment_lib);
		return;
	}
	/* The vertex function's default is the module 10.44 measured on this machine, and the
	 * fragment function's is the only shader in reach that turns a varying into a word a
	 * pixel can be read as. --vertex and --fragment override either; the fragment default
	 * is spelled out rather than taken from `fragment_function`, whose own default
	 * (clearFragment20) writes a constant and would make every picture here identical. */
	const char *vname = vertex_function != NULL ? vertex_function : "std_vert0_lpf";
	const char *fname = (fragment_function != NULL &&
			     strcmp(fragment_function, "clearFragment20") != 0)
	    ? fragment_function : "texturedFragment2DUInt";
	id<MTLFunction> vertex = [vlib newFunctionWithName:
	    [NSString stringWithUTF8String:vname]];
	id<MTLFunction> fragment = [flib newFunctionWithName:
	    [NSString stringWithUTF8String:fname]];

	if (vertex == nil || fragment == nil) {
		printf("drvtest: attrib: no %s in %s\n", vertex == nil ? vname : fname,
		       vertex == nil ? attrib_vertex_lib : fragment_lib);
		return;
	}
	printf("drvtest: attrib: %s (%s) over %s (%s)\n", vname, attrib_vertex_lib, fname,
	       fragment_lib);

	/* Four attachments, because that shader writes four, and 4x4 as everywhere else in
	 * this program: a pixel here, a pixel out of air_metal_test and a pixel out of air_run
	 * are the same measurement. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *rp = [[MTLRenderPassDescriptor renderPassDescriptor] retain];

	for (unsigned i = 0; i < 4; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: attrib: attachment %u could not be made\n", i);
			return;
		}
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:targets[i]];
		[a setLoadAction:MTLLoadActionClear];
		[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
		[a setStoreAction:MTLStoreActionStore];
	}

	/* The sampled texture, air_run's texels, and the sampler Metal gives by default:
	 * nearest, clamp to edge, normalised coordinates -- so the texel the coordinate falls
	 * in is the texel that comes back, with no weights to round. */
	MTLTextureDescriptor *std_ =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[std_ setUsage:MTLTextureUsageShaderRead];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:std_];

	if (tex == nil) {
		printf("drvtest: attrib: the sampled texture could not be made\n");
		return;
	}
	/*
	 * Seeded in ONE call and read back before anything is drawn, which is draw_path's own
	 * rule and for its reason: per row it would be four calls with a non-zero region
	 * origin, and then every picture below would depend on the driver getting origin.y
	 * right -- a thing this mode is not about. And a draw that samples a texture nobody
	 * has looked at cannot tell a wrong sample from a wrong seed.
	 */
	{
		unsigned char all[4 * 4 * 16], back[4 * 4 * 16];
		unsigned stride_bytes = texel_bytes_of(MTLPixelFormatRGBA32Uint);

		for (unsigned y = 0; y < 4; y++) {
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 100.0f + x, 200.0f + y, 300.0f + x + y, 400.0f };

				write_texel(all + (y * 4 + x) * stride_bytes,
					    MTLPixelFormatRGBA32Uint, v);
			}
		}
		[tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
		       mipmapLevel:0
			 withBytes:all
		       bytesPerRow:4 * stride_bytes];
		memset(back, 0, sizeof(back));
		[tex getBytes:back
		  bytesPerRow:4 * stride_bytes
		   fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		  mipmapLevel:0];
		for (unsigned y = 0; y < 4; y++) {
			printf("drvtest: attrib: seeded row %u reads back", y);
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 0, 0, 0, 0 };

				read_texel(back + (y * 4 + x) * stride_bytes,
					   MTLPixelFormatRGBA32Uint, v);
				printf(" %g/%g", (double)v[0], (double)v[1]);
			}
			printf("   (x/y of each texel; row %u must read y = %u)\n", y, 200 + y);
		}
	}
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
	id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

	[sd release];

	/*
	 * The vertex stage's own buffer arguments: mvp_matrix at [[buffer(2)]] gets the
	 * IDENTITY, which is what makes the position the attribute verbatim and the picture
	 * computable; texmat at [[buffer(3)]] is air.arg_unused in this module and is bound
	 * because the module declares it; [[buffer(4)]] is bound for the gated module, which
	 * declares a chroma_matrix there. The identity is the identity in either column order,
	 * which is the whole reason this harness can fill a matrix it did not define.
	 */
	struct attrib_bind b;

	memset(&b, 0, sizeof(b));
	b.queue = [dev newCommandQueue];
	b.rp = rp;
	b.target = targets[3];
	b.tex = tex;
	b.samp = samp;
	for (unsigned i = 2; i <= 4; i++) {
		b.argument[i] = [dev newBufferWithLength:64 options:0];
		if (b.argument[i] == nil || [b.argument[i] contents] == NULL) {
			printf("drvtest: attrib: buffer %u could not be made\n", i);
			return;
		}
		float *f = [b.argument[i] contents];

		memset(f, 0, 64);
		for (unsigned k = 0; k < 4; k++) {
			f[k * 4 + k] = 1.0f;
		}
	}

	/* The two shapes of the stream, and everything about them differs except the numbers
	 * they encode: the stride, all three offsets and all three formats. */
	static const struct attrib_layout wide = {
		"1. Float4 at offset 16 of a 48-byte element, stream at buffer index 5",
		48,
		{ MTLVertexFormatFloat4, 16, ATTR_STREAM },
		{ MTLVertexFormatFloat4, 0, ATTR_STREAM },
		{ MTLVertexFormatFloat2, 32, ATTR_STREAM },
	};
	static const struct attrib_layout narrow = {
		"2. Short4Normalized at offset 4 of a 16-byte element, the same four vertices",
		16,
		{ MTLVertexFormatShort4Normalized, 4, ATTR_STREAM },
		{ MTLVertexFormatUChar4Normalized, 0, ATTR_STREAM },
		{ MTLVertexFormatHalf2, 12, ATTR_STREAM },
	};

	id<MTLBuffer> wide_stream = [dev newBufferWithLength:ATTR_N * wide.stride options:0];
	id<MTLBuffer> narrow_stream = [dev newBufferWithLength:ATTR_N * narrow.stride options:0];
	id<MTLBuffer> bgra_stream = [dev newBufferWithLength:ATTR_N * narrow.stride options:0];
	id<MTLBuffer> instance_stream =
	    [dev newBufferWithLength:sizeof(attrib_instance_uv) options:0];
	id<MTLBuffer> indices = [dev newBufferWithLength:sizeof(index_list) options:0];

	if (wide_stream == nil || narrow_stream == nil || bgra_stream == nil ||
	    instance_stream == nil || indices == nil) {
		printf("drvtest: attrib: a vertex stream buffer could not be made\n");
		return;
	}
	struct attrib_layout bgra = narrow;

	bgra.what = "3. the coordinate as UChar4Normalized_BGRA: the SWAP";
	bgra.uv.format = MTLVertexFormatUChar4Normalized_BGRA;
	attrib_fill([wide_stream contents], [wide_stream length], &wide);
	attrib_fill([narrow_stream contents], [narrow_stream length], &narrow);
	attrib_fill([bgra_stream contents], [bgra_stream length], &bgra);
	memcpy([instance_stream contents], attrib_instance_uv, sizeof(attrib_instance_uv));
	memcpy([indices contents], index_list, sizeof(index_list));
	printf("drvtest: attrib: the stream is 8 elements: four corners of the quad with the "
	       "coordinate running the whole [0,1] square, then four at half the distance with "
	       "the coordinate (1,1). The module's UNUSED attribute 1 is configured too and "
	       "points at (%g, %g), the centre of texel (0,0)\n", (double)attrib_decoy_uv[0],
	       (double)attrib_decoy_uv[1]);

	/* 1. The wide layout, which is the reference every step below is compared with. */
	MTLVertexDescriptor *vd = attrib_descriptor(&wide, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> wide_ps = attrib_pipeline(dev, vertex, fragment, vd,
							     wide.what, false);

	[vd release];
	if (wide_ps == nil) {
		return;
	}
	/* What the compiler library built, read off the driver's own pipeline object rather
	 * than assumed: the elements are in the shader's driver_location order, so for
	 * std_vert0_lpf (attributes 0, 1 unused, 3) there must be TWO of them and they must be
	 * attribute 0 and attribute 3. A module of another shape is said so rather than
	 * measured against the wrong expectation. */
	unsigned elements = 0, mask = 0, slots = 0;
	SEL sel_count = sel_getUid("vertexElementCount");
	SEL sel_attr = sel_getUid("vertexElementAttribute:");
	SEL sel_mask = sel_getUid("vertexStreamMask");
	SEL sel_slots = sel_getUid("vertexStreamCount");

	if ([wide_ps respondsToSelector:sel_count] && [wide_ps respondsToSelector:sel_attr]) {
		elements = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_count);
		mask = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_mask);
		slots = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_slots);
		printf("drvtest: attrib: the pipeline holds %u vertex element%s, %u stream "
		       "slot%s, mask 0x%x; elements are", elements, elements == 1 ? "" : "s",
		       slots, slots == 1 ? "" : "s", mask);
		for (unsigned i = 0; i < elements; i++) {
			printf(" [%u]=attribute(%u)", i,
			       ((unsigned (*)(id, SEL, unsigned))objc_msgSend)(wide_ps, sel_attr,
									       i));
		}
		printf("\n");
	} else {
		printf("drvtest: attrib: this pipeline does not report its elements, so the "
		       "element ORDER is measured only by the decoy below\n");
	}

	unsigned g_wide[16][4], g_narrow[16][4], g_bgra[16][4], g_stride[16][4];
	unsigned g_other[16][4], g_idx[16][4], g_half[16][4], g_inst[16][4], g_rate[16][4];
	struct attrib_verdict v;
	bool ok = true;

	b.stream[ATTR_STREAM] = wide_stream;
	if (!attrib_draw(&b, wide_ps, 1, nil, 0, 0, g_wide, wide.what)) {
		return;
	}
	attrib_judge(g_wide, &v);
	attrib_print(wide.what, g_wide, &v);
	if (!attrib_is_grid(&v)) {
		printf("drvtest: attrib:   NOT the picture the layout describes%s\n",
		       v.decoy_texel == 16
			   ? ": every pixel holds texel (0,0), which is where the module's "
			     "UNUSED attribute 1 points -- the elements were not built in "
			     "driver_location order"
			   : "");
		ok = false;
	} else {
		printf("drvtest: attrib:   the coordinate of [[attribute(3)]] reached every "
		       "pixel and the quad of [[attribute(0)]] covered all 16, ON THE ADRENO: "
		       "the offsets, the stride and buffer index 5 are READ, and element 1 is "
		       "attribute 3 and not the unused attribute 1 (%s orientation, 10.32)\n",
		       v.mirrored == 16 ? "mirrored" : "upright");
	}

	/* 2. The same numbers in other formats at another stride and other offsets: equal to
	 *    the bit, or the formats are not what decoded the bytes. 0 and 255 read as a float
	 *    are 0.0 and 3.57e-43, and a Short4Normalized position read as Float4 is four
	 *    denormals -- a quad collapsed into the origin, covering nothing. */
	vd = attrib_descriptor(&narrow, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> narrow_ps = attrib_pipeline(dev, vertex, fragment, vd,
							       narrow.what, false);
	[vd release];
	if (narrow_ps == nil) {
		return;
	}
	b.stream[ATTR_STREAM] = narrow_stream;
	if (!attrib_draw(&b, narrow_ps, 1, nil, 0, 0, g_narrow, narrow.what)) {
		return;
	}
	attrib_judge(g_narrow, &v);
	attrib_print(narrow.what, g_narrow, &v);
	{
		unsigned d = index_diff(g_wide, g_narrow);

		printf("drvtest: attrib:   %u of 64 words differ from step 1: %s\n", d,
		       d == 0 ? "two encodings, two strides, three different offsets, ONE "
				"PICTURE -- the VFD decoded the bytes"
			      : "the same numbers in another format came back as another picture");
		ok = ok && d == 0;
	}

	/* 3. THE SWAP, which is the first of the three things softpipe could not answer. */
	vd = attrib_descriptor(&bgra, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> bgra_ps = attrib_pipeline(dev, vertex, fragment, vd,
							     bgra.what, false);
	[vd release];
	if (bgra_ps != nil) {
		b.stream[ATTR_STREAM] = bgra_stream;
		if (attrib_draw(&b, bgra_ps, 1, nil, 0, 0, g_bgra, bgra.what)) {
			unsigned d = index_diff(g_narrow, g_bgra);

			attrib_judge(g_bgra, &v);
			attrib_print(bgra.what, g_bgra, &v);
			printf("drvtest: attrib:   %u of 64 words differ from step 2, and the "
			       "coordinate's own byte 0 is ZERO: %s\n", d,
			       d == 0 ? "the BGRA order IS programmed -- VFD_DECODE_INSTR.swap "
					"delivers bytes 2 and 1 as x and y"
				      : v.column0 == 16
					    ? "every pixel holds column 0, which is what reading "
					      "the bytes in memory order gives: the swap was NOT "
					      "programmed"
					    : "neither picture, which is a third answer and wants "
					      "reading");
			ok = ok && d == 0;
		} else {
			ok = false;
		}
		[bgra_ps release];
	} else {
		ok = false;
	}

	/* 4. The stride, with a picture nothing else here produces: the narrow layout read at
	 *    32 bytes instead of 16 over a buffer of eight elements, so the fetch walks
	 *    elements 0, 2, 4 and 6 -- two corners of the full quad and two of the half-size
	 *    one. */
	struct attrib_layout stride2 = narrow;

	stride2.what = "4. the narrow layout at twice its stride: the fetch walks 0, 2, 4, 6";
	stride2.stride = 32;
	vd = attrib_descriptor(&stride2, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> stride_ps = attrib_pipeline(dev, vertex, fragment, vd,
							       stride2.what, false);
	[vd release];
	if (stride_ps != nil) {
		b.stream[ATTR_STREAM] = narrow_stream;
		if (attrib_draw(&b, stride_ps, 1, nil, 0, 0, g_stride, stride2.what)) {
			unsigned d = index_diff(g_narrow, g_stride);

			attrib_judge(g_stride, &v);
			attrib_print(stride2.what, g_stride, &v);
			printf("drvtest: attrib:   %u of 64 words differ from step 2: %s\n", d,
			       d ? "the stride decides which element a vertex reads"
				 : "A STRIDE OF 32 DREW WHAT A STRIDE OF 16 DREW, so the stride "
				   "is not read");
			ok = ok && d != 0;
		} else {
			ok = false;
		}
		[stride_ps release];
	} else {
		ok = false;
	}

	/* 5. The buffer index from the other side: the same layout fetched from index 7, with
	 *    the stream bound at 7 and nowhere else. A driver that bound every stream at slot 0
	 *    would fetch from an empty slot here and draw nothing. */
	struct attrib_layout other = narrow;

	other.what = "5. the narrow layout fetched from buffer index 7";
	other.pos.buffer = other.uv.buffer = other.decoy.buffer = ATTR_OTHER;
	vd = attrib_descriptor(&other, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> other_ps = attrib_pipeline(dev, vertex, fragment, vd,
							      other.what, false);
	[vd release];
	if (other_ps != nil) {
		b.stream[ATTR_STREAM] = nil;
		b.stream[ATTR_OTHER] = narrow_stream;
		if (attrib_draw(&b, other_ps, 1, nil, 0, 0, g_other, other.what)) {
			unsigned d = index_diff(g_narrow, g_other);

			printf("drvtest: attrib:   %s: %u of 64 words differ from step 2, %s\n",
			       other.what, d,
			       d == 0 ? "so Metal's buffer index is carried all the way down to "
					"VFD_FETCH[k]"
				      : "so the index moved the picture, which it must not");
			ok = ok && d == 0;
		} else {
			ok = false;
		}
		[other_ps release];
		b.stream[ATTR_OTHER] = nil;
		b.stream[ATTR_STREAM] = narrow_stream;
	} else {
		ok = false;
	}

	/* 6 and 7. The indexed forms over a real layout, which is what every draw in a real
	 *          application is -- and here an index is the ELEMENT THE FETCH READS, where in
	 *          10.35 it was the [[vertex_id]] a shader indexed its own buffer with. Four
	 *          indices must give step 2's picture; the last three must give one triangle of
	 *          it. */
	if (attrib_draw(&b, narrow_ps, 1, indices, 4, 0, g_idx,
			"6. -drawIndexedPrimitives: 4 indices [0 1 2 3] over the narrow layout")) {
		unsigned d = index_diff(g_narrow, g_idx);

		printf("drvtest: attrib:   6. indices [0 1 2 3]: %u of 64 words differ from step "
		       "2, %s\n", d,
		       d == 0 ? "so the index buffer selects the element the fetch reads"
			      : "so an index fed the fetch the wrong element");
		ok = ok && d == 0;
	} else {
		ok = false;
	}
	if (attrib_draw(&b, narrow_ps, 1, indices, 3, 4 * 2, g_half,
			"7. the same buffer's last three indices [1 2 3], UInt16 at byte 8")) {
		attrib_judge(g_half, &v);
		attrib_print("7. indices [1 2 3] at byte 8", g_half, &v);
		printf("drvtest: attrib:   %u of 64 words differ from step 6, covering %u of 16: "
		       "%s\n", index_diff(g_idx, g_half), v.covered,
		       v.covered > 0 && v.covered < 16
			   ? "one triangle of the quad out of three of its four elements"
			   : "NOT one triangle, which is what an offset that was not applied or "
			     "an index that was not read would give");
		ok = ok && v.covered > 0 && v.covered < 16;
	} else {
		ok = false;
	}

	/*
	 * 8 and 9. THE INSTANCED BIT and THE STEP RATE, the second thing softpipe could not
	 *          answer about the hardware. The coordinate comes out of a second stream that
	 *          steps per instance; two instances of this geometry cover the same pixels, so
	 *          the second is drawn over the first and every pixel must hold ONE texel --
	 *          instance 1's element, (0.875, 0.875), which is texel (3, 3) = 103 203 306
	 *          400. A stream that stepped per VERTEX would hand the four coordinates to the
	 *          four vertices of one instance and the picture would be a gradient. Then the
	 *          same at a step rate of 2, where both instances share element 0 and the
	 *          picture must be texel (0, 0) = 100 200 300 400 instead -- a different number
	 *          and not a different-looking picture.
	 */
	struct attrib_layout split = narrow;

	split.what = "8. the coordinate from a second stream stepping PER INSTANCE, 2 instances";
	split.uv.format = MTLVertexFormatFloat2;
	split.uv.offset = 0;
	split.uv.buffer = ATTR_INSTANCE;
	for (unsigned step = 0; step < 2; step++) {
		NSUInteger rate = step == 0 ? 1 : 2;
		const char *what = step == 0 ? split.what
		    : "9. the same at a step rate of 2: both instances share element 0";
		unsigned (*grid)[4] = step == 0 ? g_inst : g_rate;
		unsigned want_x = step == 0 ? 3 : 0, want_y = step == 0 ? 3 : 0;

		vd = attrib_descriptor(&split, MTLVertexStepFunctionPerInstance, rate,
				       ATTR_INSTANCE);
		id<MTLRenderPipelineState> ps = attrib_pipeline(dev, vertex, fragment, vd, what,
								false);
		[vd release];
		if (ps == nil) {
			ok = false;
			continue;
		}
		b.stream[ATTR_INSTANCE] = instance_stream;
		if (attrib_draw(&b, ps, 2, nil, 0, 0, grid, what)) {
			unsigned w0 = 100 + want_x, w1 = 200 + want_y;

			attrib_judge(grid, &v);
			attrib_print(what, grid, &v);
			bool flat = v.uniform == 16 && grid[0][0] == w0 && grid[0][1] == w1;

			printf("drvtest: attrib:   expecting %u %u %u %u in all 16 pixels (texel "
			       "(%u,%u)): %s\n", w0, w1, 300 + want_x + want_y, 400, want_x,
			       want_y,
			       flat ? (step == 0 ? "the stream stepped PER INSTANCE on the "
						   "hardware"
						 : "the step rate IS the instance divisor")
				    : v.uniform != 16
					  ? "the picture is a gradient, so that stream stepped "
					    "per VERTEX"
					  : "flat, but not that texel");
			ok = ok && flat;
		} else {
			ok = false;
		}
		[ps release];
		b.stream[ATTR_INSTANCE] = nil;
	}

	/*
	 * 10. THE PLACEHOLDER for an attribute gated by a function constant, which is the third
	 *     thing this machine could not answer: on softpipe the pipeline for such a module
	 *     dies in nir_to_tgsi before any draw. QuartzCore's fixed_vert_*_spc has five
	 *     attributes -- listed 0, 4, 1, 2, 3, so attribute 4 is at driver_location 1 -- and
	 *     three of them (point_size, texcoord0, texcoord1) are gated. This configures the
	 *     two that are NOT gated and leaves the three alone, which is what a pipeline whose
	 *     constants make the predicate false legally does.
	 *
	 *     What is claimed about it is exactly two things, and no more: the pipeline BUILDS,
	 *     and the draw COMES BACK. The picture is printed and nothing is concluded from it
	 *     -- no function constants are supplied here, so what the shader reads when the
	 *     predicate is false is the pipeline's business and not this test's. The claim that
	 *     matters is the second one: an unconfigured attribute with no element at all would
	 *     leave an empty VFD slot, and an empty slot fetches from address 0 (10.18).
	 */
	if (attrib_gated_function != NULL && *attrib_gated_function != '\0') {
		id<MTLFunction> gated = [vlib newFunctionWithName:
		    [NSString stringWithUTF8String:attrib_gated_function]];

		if (gated == nil) {
			printf("drvtest: attrib: 10. no %s in %s, so the function-constant gate "
			       "was not measured\n", attrib_gated_function, attrib_vertex_lib);
		} else {
			struct attrib_layout gate = narrow;

			gate.what = "10. a module whose attributes are gated by function "
				    "constants, with only the two ungated ones configured";
			gate.decoy.format = MTLVertexFormatInvalid;   /* attribute 1 is gated */
			vd = attrib_descriptor(&gate, MTLVertexStepFunctionPerVertex, 1, -1);
			id<MTLRenderPipelineState> ps = attrib_pipeline(dev, gated, fragment, vd,
									gate.what, false);
			[vd release];
			if (ps == nil) {
				printf("drvtest: attrib:   and that refusal is the measurement "
				       "failing: a gated attribute needs no configuration, so "
				       "this pipeline had to build\n");
				ok = false;
			} else {
				unsigned g_gate[16][4];

				b.stream[ATTR_STREAM] = narrow_stream;
				if (attrib_draw(&b, ps, 1, nil, 0, 0, g_gate, gate.what)) {
					attrib_judge(g_gate, &v);
					attrib_print(gate.what, g_gate, &v);
					printf("drvtest: attrib:   the pipeline built and the GPU "
					       "CAME BACK, so the placeholder element for a gated "
					       "attribute fetches memory this library owns and not "
					       "an empty VFD slot. What the pixels hold is NOT a "
					       "claim: no function constants were supplied, so the "
					       "predicate's value -- and what the shader reads "
					       "under it -- is the pipeline's and not this test's\n");
				} else {
					printf("drvtest: attrib:   the pipeline built and the draw "
					       "did NOT come back, which is what an empty VFD slot "
					       "looks like\n");
					ok = false;
				}
				[ps release];
			}
			[gated release];
		}
	}

	/*
	 * And three layouts that must be REFUSED on the device, by name, so the sentences are
	 * read in the log of the machine they are about rather than only on the host: the
	 * twelve-bit offset field, a packed format whose channel order against Gallium's is
	 * unmeasured, and the one buffer table Metal has behind -setVertexBuffer:atIndex:.
	 */
	printf("drvtest: attrib: and the layouts that must be refused, each by name:\n");
	unsigned refused = 0;
	for (unsigned i = 0; i < 3; i++) {
		struct attrib_layout bad = narrow;
		const char *what = i == 0 ? "an attribute at offset 4096"
		    : i == 1 ? "MTLVertexFormatUInt1010102Normalized, a packed word"
			     : "a stream at the buffer index the module reads mvp_matrix from";

		bad.what = what;
		if (i == 0) {
			bad.pos.offset = 4096;
		} else if (i == 1) {
			bad.pos.format = MTLVertexFormatUInt1010102Normalized;
		} else {
			bad.pos.buffer = bad.uv.buffer = bad.decoy.buffer = 2;
		}
		vd = attrib_descriptor(&bad, MTLVertexStepFunctionPerVertex, 1, -1);
		id<MTLRenderPipelineState> ps = attrib_pipeline(dev, vertex, fragment, vd, what,
								true);
		[vd release];
		if (ps != nil) {
			printf("drvtest: attrib:   %s: BUILT, and it should not have\n", what);
			[ps release];
			ok = false;
		} else {
			refused++;
		}
	}
	printf("drvtest: attrib: %u of 3 refused by name\n", refused);
	ok = ok && refused == 3;

	printf("drvtest: attrib: %s\n",
	       ok ? "THE VERTEX ATTRIBUTE LAYOUT IS READ ON THE ADRENO -- the offsets, the "
		    "stride, the formats, the BGRA swap, the buffer index, the index buffer, the "
		    "instanced bit and its step rate each changed what came back, and the element "
		    "order is the shader's driver_location order and not Metal's attribute order"
		  : "SOMETHING ABOVE DISAGREES, and a layout that is nearly right draws a picture "
		    "of the wrong vertices");

	[narrow_ps release];
	[wide_ps release];
	[indices release];
	[instance_stream release];
	[bgra_stream release];
	[narrow_stream release];
	[wide_stream release];
	for (unsigned i = 2; i <= 4; i++) {
		[b.argument[i] release];
	}
	[samp release];
	[tex release];
	for (unsigned i = 0; i < 4; i++) {
		[targets[i] release];
	}
	[rp release];
	fflush(stdout);
}

static void
draw_path(id<MTLDevice> dev, const char *metallib)
{
	static const unsigned expected[4] = { 2458616957u, 2930221209u, 3401825461u, 3873429713u };
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: draw: newLibraryWithData: gave nil\n");
		return;
	}
	const char *shader = fragment_function;
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:shader]];

	if (fragment == nil) {
		printf("drvtest: draw: no %s in that library\n", shader);
		return;
	}
	/* Named in the log on purpose: tools/air/metalcheck.py can then produce the host
	 * reference itself, rather than the comparison resting on the four words written into
	 * this file. */
	printf("drvtest: draw: %s %s\n", metallib, shader);

	/* What a shader declares, from the compiler library the driver is already using. The
	 * sweep below binds per shader with it; this single draw keeps its one binding, whose
	 * reference is hand-checked. */
	load_texture_query(dev);

	/* Three attachments, because the shader writes target 2 and Metal numbers them from 0;
	 * the format is the one air_run drew into, so the comparison is of the same thing. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: draw: attachment %d could not be made\n", i);
			return;
		}
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	id<MTLFunction> vertex = nil;

	if (vertex_function != NULL) {
		vertex = [library newFunctionWithName:
		    [NSString stringWithUTF8String:vertex_function]];
		if (vertex == nil) {
			printf("drvtest: draw: no %s in that library\n", vertex_function);
			return;
		}
		printf("drvtest: draw: vertex stage is Apple's own %s\n", vertex_function);
	}
	[pd setVertexFunction:vertex];   /* nil: the library generates the stage, as air_run does */
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];

	[pd release];
	printf("drvtest: draw: newRenderPipelineStateWithDescriptor: gave %s%s%s\n",
	       pipeline != nil ? class_getName(object_getClass(pipeline)) : "nil",
	       err != nil ? ", error: " : "",
	       err != nil ? [[err localizedDescription] UTF8String] : "");
	if (pipeline == nil) {
		return;
	}

	/* The shader's constant buffer, filled the way air_run fills it, and bound at every
	 * index up to 16 because this one is declared at location 14 and a driver that bound
	 * only index 0 would leave it reading nothing. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform != nil && [uniform contents] != NULL) {
		unsigned char *bytes = [uniform contents];

		for (int i = 0; i < 256; i++) {
			bytes[i] = (unsigned char)((i * 7 + 13) & 0xff);
		}
	}

	/*
	 * The vertex stage's own buffers. Two of them, because that is what the shaders declare,
	 * and the first version of this bound ONE buffer of corners at every index -- so
	 * texturedVertex2D, which reads positions from buffer 0 and texture coordinates from
	 * buffer 1, read positions as its coordinates and returned an honest answer to an input
	 * nobody meant (103 200 303 400 on the device, docs/metal.md 10.25). A harness that
	 * binds the same thing everywhere cannot be compared with anything.
	 *
	 * Positions go to index 0 and to index 14, because clearVertex declares its buffer at
	 * 14 and texturedVertex2D at 0; coordinates go to 1. The corners are the geometry the
	 * generated stage covers, so the two roads draw the same pixels.
	 */
	id<MTLBuffer> corners = [dev newBufferWithLength:256 options:0];
	id<MTLBuffer> coords = [dev newBufferWithLength:256 options:0];

	if (vertex_function != NULL && corners != nil && [corners contents] != NULL) {
		static const float quad[4][4] = {
			{ -1.0f, -1.0f, 0.0f, 1.0f },
			{  1.0f, -1.0f, 0.0f, 1.0f },
			{ -1.0f,  1.0f, 0.0f, 1.0f },
			{  1.0f,  1.0f, 0.0f, 1.0f },
		};

		memcpy([corners contents], quad, sizeof(quad));
	}
	if (vertex_function != NULL && coords != nil && [coords contents] != NULL) {
		/* the same corners in texture space, so the far corner of the picture reads the
		 * far texel -- which is the half of the comparison that a dropped varying cannot
		 * fake */
		static const float uv[4][2] = {
			{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f },
		};

		memcpy([coords contents], uv, sizeof(uv));
	}

	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

	for (int i = 0; i < 3; i++) {
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:targets[i]];
		[a setLoadAction:MTLLoadActionClear];
		[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
		[a setStoreAction:MTLStoreActionStore];
	}

	/* Textures for the fragment stage, when the shader reads any. The first attempt at
	 * drawing with Apple's textured vertex shader never reached the draw at all: it was
	 * refused with "reads texture 0 and 0 textures were bound", which is a sentence about
	 * this harness. The texels are air_run's own, so the picture can be compared. */
	id<MTLTexture> dtex[4] = { nil, nil, nil, nil };
	id<MTLSamplerState> dsamp = nil;
	MTLTextureDescriptor *dtd = texture_descriptor(
	    slot_format[0] != 0 ? slot_format[0] : MTLPixelFormatRGBA32Uint,
	    MTLTextureUsageShaderRead);

	[dtd setWidth:4];
	[dtd setHeight:4];
	for (int i = 0; i < 4; i++) {
		dtex[i] = [dev newTextureWithDescriptor:dtd];
		if (dtex[i] == nil) {
			break;
		}
		unsigned stride_bytes = texel_bytes_of([dtd pixelFormat]);
		unsigned char all[4 * 4 * 16];

		for (unsigned y = 0; y < 4; y++) {
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 100.0f + x + 1000.0f * i, 200.0f + y,
					       300.0f + x + y, 400.0f };

				write_texel(all + (y * 4 + x) * stride_bytes,
					    [dtd pixelFormat], v);
			}
		}
		/* One call for the whole picture, and not one per row.
		 *
		 * Per row it was four calls with a non-zero region origin, which made every
		 * measurement of this texture depend on the driver getting origin.y right --
		 * a thing this test is not about. It is also the second explanation of the
		 * vertical flip in docs/metal.md 10.26, which one pixel could not separate
		 * from the first: four rows landing in one place would leave every texel
		 * carrying the last row's y = 203, and pixel (0, 0) would then read
		 * 100 203 303 400 with no flip anywhere.
		 */
		if (seed_rows) {
			for (unsigned y = 0; y < 4; y++) {
				[dtex[i] replaceRegion:MTLRegionMake2D(0, y, 4, 1)
					  mipmapLevel:0
					    withBytes:all + y * 4 * stride_bytes
					  bytesPerRow:4 * stride_bytes];
			}
		} else {
			[dtex[i] replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
				  mipmapLevel:0
				    withBytes:all
				  bytesPerRow:4 * stride_bytes];
		}
		/* and read it back, because a draw that samples a texture nobody has looked
		 * at cannot tell a wrong sample from a wrong seed
		 */
		if (i == 0) {
			unsigned char back[4 * 4 * 16];

			memset(back, 0, sizeof(back));
			[dtex[i] getBytes:back
			      bytesPerRow:4 * stride_bytes
			       fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			      mipmapLevel:0];
			for (unsigned y = 0; y < 4; y++) {
				printf("drvtest: draw: seeded row %u reads back", y);
				for (unsigned x = 0; x < 4; x++) {
					float v[4] = { 0, 0, 0, 0 };

					read_texel(back + (y * 4 + x) * stride_bytes,
						   [dtd pixelFormat], v);
					printf(" %g/%g", v[0], v[1]);
				}
				printf("   (x/y of each texel; row %u must read y = %u)\n",
				       y, 200 + y);
			}
		}
	}
	MTLSamplerDescriptor *dsd = [[MTLSamplerDescriptor alloc] init];

	dsamp = [dev newSamplerStateWithDescriptor:dsd];
	[dsd release];
	printf("drvtest: draw: %d textures of format %lu for the fragment stage\n",
	       dtex[0] != nil ? (dtex[3] != nil ? 4 : 1) : 0, (unsigned long)[dtd pixelFormat]);

	id<MTLCommandQueue> queue = [dev newCommandQueue];
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	printf("drvtest: draw: renderCommandEncoderWithDescriptor: gave %s\n",
	       enc != nil ? class_getName(object_getClass(enc)) : "nil");
	if (enc == nil) {
		return;
	}
	[enc setRenderPipelineState:pipeline];
	for (int i = 0; i < 16; i++) {
		[enc setFragmentBuffer:uniform offset:0 atIndex:i];
		/* what each index holds is what some shader declares there: 0 and 14 are
		 * positions, 1 is texture coordinates */
		[enc setVertexBuffer:i == 1 ? coords : corners offset:0 atIndex:i];
		if (i < 4 && dtex[i] != nil) {
			[enc setFragmentTexture:dtex[i] atIndex:i];
		}
		if (dsamp != nil) {
			[enc setFragmentSamplerState:dsamp atIndex:i];
		}
	}
	/* four vertices as a strip: the quad air_run draws over the whole attachment */
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	printf("drvtest: draw: status %lu%s%s\n", (unsigned long)[cmd status],
	       [cmd error] != nil ? ", error: " : "",
	       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
	if ([cmd status] != RW_CB_COMPLETED) {
		return;
	}

	/* What the render path still owes, counted on the device the same way the device's own
	 * protocols were: the encoder and the pipeline state are where the remaining work is,
	 * and a number beats an estimate. */
	for (int which = 0; which < 2; which++) {
		const char *pname = which == 0 ? "MTLRenderCommandEncoder" :
		    "MTLRenderPipelineState";
		id object = which == 0 ? (id)enc : (id)pipeline;
		Protocol *proto = objc_getProtocol(pname);
		unsigned n = 0, missing = 0, shown = 0;

		if (proto == NULL) {
			continue;
		}
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n; i++) {
			if ([object respondsToSelector:req[i].name]) {
				continue;
			}
			missing++;
			if (shown < 6) {
				printf("drvtest: draw:   %s still owes %s\n", pname,
				       sel_getName(req[i].name));
				shown++;
			}
		}
		free(req);
		printf("drvtest: draw: of %u methods %s requires, %u are unanswered\n", n, pname,
		       missing);
	}

	if (draw_all) {
		draw_every(dev, library, queue, rp, uniform, dtex, dsamp, targets);
		return;
	}

	unsigned pixel[4] = { 0 };

	[targets[2] getBytes:pixel
		 bytesPerRow:4 * 4 * 4
		  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
		 mipmapLevel:0];
	/* All sixteen pixels, not one.
	 *
	 * One pixel is what made a flat varying look like a vertical flip for three boots:
	 * pixel (0,0) lies in the triangle whose provoking vertex carries the texture
	 * coordinate (0,1), so a FLAT varying puts texel (0,3) there -- exactly what a
	 * mirrored picture would put there. And pixels (3,3) and (0,3) read the same under
	 * flat and under smooth, so of the sixteen only fourteen can tell the two apart.
	 * docs/metal.md 10.32 is the measurement that followed from printing them all.
	 */
	{
		unsigned grid[4 * 4 * 4];

		memset(grid, 0, sizeof(grid));
		[targets[2] getBytes:grid
			 bytesPerRow:4 * 16
			  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			 mipmapLevel:0];
		for (unsigned gy = 0; gy < 4; gy++) {
			for (unsigned gx = 0; gx < 4; gx++) {
				const unsigned *t = grid + (gy * 4 + gx) * 4;

				printf("drvtest: draw: attachment 2 at (%u, %u) = %u %u %u %u\n",
				       gx, gy, t[0], t[1], t[2], t[3]);
			}
		}
	}
	printf("drvtest: draw: the pixel at (0, 0) of attachment 2 is %u %u %u %u\n",
	       pixel[0], pixel[1], pixel[2], pixel[3]);
	printf("drvtest: draw: softpipe computed                      %u %u %u %u\n",
	       expected[0], expected[1], expected[2], expected[3]);
	if (strcmp(shader, "clearFragment20") != 0) {
		/* The four words above are clearFragment20's; another shader has no constant
		 * here, and metalcheck.py produces its reference by running air_run. */
		printf("drvtest: draw: no constant for %s in this test; metalcheck compares it\n",
		       shader);
	} else {
		printf("drvtest: draw: %s\n",
		       memcmp(pixel, expected, sizeof(pixel)) == 0 ?
		       "THE SAME -- a Metal fragment shader drew on the Adreno" :
		       "DIFFERENT, and a different picture is a failure, not a variation");
	}
	/* The indexed draws go LAST, over this pipeline and these attachments, so that the
	 * picture above is their reference and the only thing that differs between it and
	 * them is the index buffer. */
	if (draw_index) {
		index_draws(dev, queue, rp, pipeline, uniform, corners, coords, dtex, dsamp,
			    targets);
	}
	fflush(stdout);
}

/*
 * --draw-depth: whether a depth test discards anything on the Adreno.
 *
 * Every draw this driver has made ran with no depth attachment and so with depth testing
 * off, which is not a choice anyone made -- it is an absence. This is the measurement that
 * closes it, and it is the same experiment air_metal_test runs on softpipe
 * (tools/mesa/air/air_metal_test.c, --draw-depth), so the two can be put side by side:
 *
 *   - a fragment shader that writes COLOUR, drawn four times. Three with
 *     MTLCompareFunctionLess and depth writes on, at depths 0.5, 0.75 and 0.25, and then
 *     once more at 0.75 with MTLCompareFunctionAlways. The quad is the same quad; what
 *     changes is the depth (through MTLViewport's znear, which is the only thing in Metal
 *     that moves this driver's quad in z) and the bytes of its constant buffer, so a draw
 *     that is not discarded leaves a colour the previous one did not. The colour after the
 *     far draw is the test: unchanged means discarded, and the Always control at that same
 *     depth afterwards says the compare function did it rather than the draw.
 *   - a fragment shader that writes DEPTH -- Apple's texturedFragment2DDepth and
 *     texturedFragment3DDepth -- where the buffer must come back holding what the shader
 *     computed. Those two sample a texture and store channel x to [[depth]], so their
 *     texture is seeded in [0, 1] instead of with air_run's pattern, whose 100.0 would
 *     clamp to the far plane and read exactly like the clear value.
 *
 * Which of the two runs is the pipeline's own answer (-writesDepth, out of the compiler
 * library's rwa_render_info) and not a flag here.
 */
static MTLPixelFormat depth_attachment_format = MTLPixelFormatDepth32Float;

/* 64 values in (0, 1), each exact as a float, so the one that comes back names the texel it
 * came from -- and none of them is 0 or 1, which a cleared or untouched buffer could hold. */
static float
depth_texel_value(unsigned x, unsigned y, unsigned z)
{
	return ((float)(x + 4 * y + 16 * z) + 0.5f) / 64.0f;
}

static void
fill_depth_texture(id<MTLTexture> tex)
{
	MTLPixelFormat pf = [tex pixelFormat];
	unsigned bytes = texel_bytes_of(pf);
	unsigned char row[DRAW_TEX * 16];

	for (NSUInteger z = 0; z < [tex depth]; z++) {
		for (NSUInteger y = 0; y < [tex height]; y++) {
			for (NSUInteger x = 0; x < [tex width]; x++) {
				float d = depth_texel_value((unsigned)x, (unsigned)y,
							    (unsigned)z);
				float v[4] = { d, d, d, d };

				write_texel(row + x * bytes, pf, v);
			}
			[tex replaceRegion:MTLRegionMake3D(0, y, z, [tex width], 1, 1)
			       mipmapLevel:0
				     slice:0
				 withBytes:row
			       bytesPerRow:[tex width] * bytes
			     bytesPerImage:0];
		}
	}
}

/* The depth buffer, through the driver's own aspect reader: -getBytes: has no aspect to
 * name and the compiler library refuses a depth texture through it by name, so this asks
 * for the depth aspect as floats. Returns false when the driver is older than this test. */
static bool
read_depth_buffer(id<MTLTexture> zt, float *depth, const char *what)
{
	SEL sel = sel_getUid("redwoodReadDepth:stencil:fromRegion:");

	if (![zt respondsToSelector:sel]) {
		printf("drvtest: depth: this texture does not answer "
		       "redwoodReadDepth:stencil:fromRegion:, so the driver is older than this "
		       "test\n");
		return false;
	}
	BOOL ok = ((BOOL (*)(id, SEL, float *, uint8_t *, MTLRegion))objc_msgSend)(
	    zt, sel, depth, NULL, MTLRegionMake2D(0, 0, DRAW_TEX, DRAW_TEX));

	if (!ok) {
		printf("drvtest: depth: %s: the depth could not be read back\n", what);
		return false;
	}
	bool uniform = true;

	for (unsigned i = 1; i < DRAW_TEX * DRAW_TEX; i++) {
		if (depth[i] != depth[0]) {
			uniform = false;
		}
	}
	printf("drvtest: depth: %s: depth at (0,0) = %.9g, and the 16 texels are %s\n", what,
	       depth[0], uniform ? "all that value" : "NOT all the same");
	return uniform;
}

/*
 * --argbuf: MTLArgumentEncoder on the device, and the buffer OFFSET that goes with it.
 *
 * Three measurements, and what each is worth is different, so they are printed apart:
 *
 *  1. THE FUNCTION PATH. -[MTLFunction newArgumentEncoderWithBufferIndex:] is a method of
 *     Metal's own class, and on this port Metal's own implementation cannot work: it asks
 *     the function for its reflection, which asks the device for -compiler, which is a
 *     two-instruction stub answering nil (0x181aac128), and then reports
 *     "bufferIndex %lu does not identify an argument buffer" and returns nil. The driver
 *     answers it instead, through a category, by reading the function's own bitcode. So
 *     what this measures is WHOSE answer came back: Apple's shaders declare no argument
 *     buffer at all (0 of 456 modules, docs/air.md 8.2), so the answer must still be nil
 *     -- but with the driver's own sentence, which names how many sets the function has.
 *     Metal's sentence and the driver's are told apart in the log by "this function
 *     declares".
 *  2. THE DESCRIPTOR PATH, which needs no shader: -[MTLDevice newArgumentEncoderWithArguments:]
 *     with a pointer, a texture and a sampler member. encodedLength must be 24 and
 *     alignment 8 -- 8 bytes per member, which is what Apple's own metadata says a set is
 *     (7 856 of Resident Evil 4's 7 858 sets; the two that differ are both in the one
 *     kernel that builds indirect command buffers and are refused before an encoder could
 *     be made). Then the three members are encoded and the compiler library is asked how
 *     many records it holds for that (buffer, offset), which is the whole of what an
 *     encoder does here.
 *  3. THE BUFFER OFFSET, which is not about argument buffers at all and is the reason this
 *     mode exists tonight: Apple's own clearFragment20 drawn twice, with its 48-byte
 *     constant block at offset 0 of one buffer and at OFFSET 1164 of another. 1164 is the
 *     offset the window server binds its own fragment buffer at, 55 times in one boot,
 *     and until this work every one of those bindings was refused. The two pictures must
 *     be the same four words, and the same four words the host reference gives
 *     (tools/air/metalcheck.py, and draw_path's own `expected`).
 *
 * What is NOT here, said plainly: a DRAW through an argument buffer. That needs a
 * .metallib holding a shader that declares one, and this port has no container writer --
 * Apple's libraries have no sets, and the game's are not something a job list can reach.
 * The draw-through-an-encoder differential is therefore the host's
 * (tools/air/argbufruncheck.sh, on softpipe), and what the device adds is the three above.
 */
static void
argbuf_path(id<MTLDevice> dev, const char *metallib)
{
	static const unsigned expected[4] = { 2458616957u, 2930221209u, 3401825461u,
					      3873429713u };
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: argbuf: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: argbuf: newLibraryWithData: gave nil\n");
		return;
	}

	/* 1. the function path */
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:fragment_function]];

	if (fragment == nil) {
		printf("drvtest: argbuf: no %s in that library\n", fragment_function);
		return;
	}
	SEL withIndex = sel_getUid("newArgumentEncoderWithBufferIndex:");

	printf("drvtest: argbuf: %s answers newArgumentEncoderWithBufferIndex: %s\n",
	       fragment_function,
	       [fragment respondsToSelector:withIndex] ? "yes" : "NO (nothing implements it)");
	if ([fragment respondsToSelector:withIndex]) {
		id enc = ((id (*)(id, SEL, NSUInteger))objc_msgSend)(fragment, withIndex, 0);

		printf("drvtest: argbuf: an encoder for [[buffer(0)]] of %s is %s (Apple's "
		       "shaders declare no argument buffer, so nil is the right answer and "
		       "the LINE ABOVE from the driver is the measurement)\n",
		       fragment_function,
		       enc != nil ? class_getName(object_getClass(enc)) : "nil");
		[enc release];
	}

	/* 2. the descriptor path */
	Class descClass = objc_lookUpClass("MTLArgumentDescriptor");
	SEL newArgs = sel_getUid("newArgumentEncoderWithArguments:");

	if (descClass != NULL && [dev respondsToSelector:newArgs]) {
		static const NSUInteger type[3] = { 60 /* Pointer */, 58 /* Texture */,
						    59 /* Sampler */ };
		NSMutableArray *args = [NSMutableArray array];

		for (unsigned i = 0; i < 3; i++) {
			id d = [descClass argumentDescriptor];

			[d setIndex:i];
			[d setDataType:(MTLDataType)type[i]];
			[d setArrayLength:0];
			[args addObject:d];
		}
		id enc = ((id (*)(id, SEL, id))objc_msgSend)(dev, newArgs, args);

		if (enc == nil) {
			printf("drvtest: argbuf: newArgumentEncoderWithArguments: gave nil\n");
		} else {
			NSUInteger len = [enc encodedLength], align = [enc alignment];

			printf("drvtest: argbuf: %s: encodedLength %lu (must be 24: 8 bytes a "
			       "member), alignment %lu (must be 8)\n",
			       len == 24 && align == 8 ? "ok" : "WRONG",
			       (unsigned long)len, (unsigned long)align);

			/* and the records, which is what this encoder does instead of
			 * writing: the buffer's own bytes are never read by a draw. */
			id<MTLBuffer> dest = [dev newBufferWithLength:len > 0 ? len : 64
							     options:0];
			id<MTLBuffer> member = [dev newBufferWithLength:256 options:0];
			MTLTextureDescriptor *td = [MTLTextureDescriptor
			    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							 width:4 height:4 mipmapped:NO];
			[td setUsage:MTLTextureUsageShaderRead];
			id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
			MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
			id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

			[sd release];
			[enc setArgumentBuffer:dest offset:0];
			[enc setBuffer:member offset:0 atIndex:0];
			[enc setTexture:tex atIndex:1];
			[enc setSamplerState:samp atIndex:2];
			/* An id past the set's own members, which Metal would write a handle
			 * for and this driver refuses by name: the line comes from the
			 * driver, and what is checked here is that nothing crashed. */
			[enc setTexture:tex atIndex:7];

			unsigned recorded = argbuf_records(dest, 0);

			printf("drvtest: argbuf: %s: %u of 3 members recorded against that "
			       "buffer at offset 0\n", recorded == 3 ? "ok" : "WRONG",
			       recorded);
			[dest release];
			[member release];
			[tex release];
			[samp release];
			[enc release];
		}
	} else {
		printf("drvtest: argbuf: no MTLArgumentDescriptor class or no "
		       "newArgumentEncoderWithArguments: on this device\n");
	}

	/* 3. the buffer offset, which is the window server's own case */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4 height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: argbuf: attachment %d could not be made\n", i);
			return;
		}
	}
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	[pd setVertexFunction:nil];
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];
	[pd release];
	if (pipeline == nil) {
		printf("drvtest: argbuf: no pipeline: %s\n",
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
		return;
	}

	/* The same 48 bytes in two places: at offset 0 of one buffer, and at offset 1164 of
	 * another whose other bytes are deliberately NOT that pattern -- so a driver that
	 * ignored the offset would read the wrong block and print different words, and one
	 * that refused the binding would print a refusal. */
	const NSUInteger at = 1164;
	id<MTLBuffer> flat = [dev newBufferWithLength:256 options:0];
	id<MTLBuffer> moved = [dev newBufferWithLength:4096 options:0];

	if (flat == nil || moved == nil || [flat contents] == NULL ||
	    [moved contents] == NULL) {
		printf("drvtest: argbuf: the two uniform buffers could not be made\n");
		return;
	}
	unsigned char *fb = [flat contents], *mb = [moved contents];

	for (NSUInteger i = 0; i < 256; i++) {
		fb[i] = (unsigned char)((i * 7 + 13) & 0xff);
	}
	memset(mb, 0xa5, 4096);
	for (NSUInteger i = 0; i < 256; i++) {
		mb[at + i] = fb[i];
	}

	id<MTLCommandQueue> queue = [dev newCommandQueue];
	struct { id<MTLBuffer> buffer; NSUInteger offset; const char *what; } run[2] = {
		{ flat, 0, "at offset 0" },
		{ moved, at, "at offset 1164, the window server's own" },
	};

	for (unsigned k = 0; k < 2; k++) {
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

		for (int i = 0; i < 3; i++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:i];

			[a setTexture:targets[i]];
			[a setLoadAction:MTLLoadActionClear];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
			[a setStoreAction:MTLStoreActionStore];
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc =
		    [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: argbuf: no render encoder\n");
			return;
		}
		[enc setRenderPipelineState:pipeline];
		/* every index up to 16, as draw_path binds it: this shader's block is
		 * declared at [[buffer(14)]] and a driver that bound only index 0 would
		 * leave it reading nothing */
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:run[k].buffer offset:run[k].offset atIndex:i];
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];

		if ([cmd status] != RW_CB_COMPLETED) {
			printf("drvtest: argbuf: the draw %s did not complete: status %lu%s%s\n",
			       run[k].what, (unsigned long)[cmd status],
			       [cmd error] != nil ? ", " : "",
			       [cmd error] != nil
				   ? [[[cmd error] localizedDescription] UTF8String] : "");
			continue;
		}
		unsigned pixel[4] = { 0 };

		[targets[2] getBytes:pixel
			 bytesPerRow:4 * 4 * 4
			  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
			 mipmapLevel:0];
		bool same = pixel[0] == expected[0] && pixel[1] == expected[1] &&
			    pixel[2] == expected[2] && pixel[3] == expected[3];

		printf("drvtest: argbuf: %s: the uniform block %s draws %u %u %u %u\n",
		       same ? "ok" : "WRONG", run[k].what, pixel[0], pixel[1], pixel[2],
		       pixel[3]);
	}
	printf("drvtest: argbuf: the four words above are the host's own reference "
	       "(tools/air/metalcheck.py): %u %u %u %u\n", expected[0], expected[1],
	       expected[2], expected[3]);
}

static void
depth_path(id<MTLDevice> dev, const char *metallib)
{
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: depth: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: depth: newLibraryWithData: gave nil\n");
		return;
	}
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:fragment_function]];

	if (fragment == nil) {
		printf("drvtest: depth: no %s in that library\n", fragment_function);
		return;
	}
	printf("drvtest: depth: %s %s, depth attachment pixel format %lu\n", metallib,
	       fragment_function, (unsigned long)depth_attachment_format);
	load_texture_query(dev);

	/* Three colour attachments of air_run's own format, as the colour draw has: the
	 * shader this defaults to writes render target 2, and 0 and 1 have to exist for 2
	 * to be bound at all. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:DRAW_TEX
							     height:DRAW_TEX
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: depth: colour attachment %d could not be made\n", i);
			return;
		}
	}

	/* The depth attachment: MTLTextureUsageRenderTarget and nothing else. The compiler
	 * library turns that usage into PIPE_BIND_DEPTH_STENCIL because the FORMAT is a
	 * depth one, which is the one place where the same Metal usage bit means two
	 * different Gallium bindings. */
	MTLTextureDescriptor *ztd =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_attachment_format
							      width:DRAW_TEX
							     height:DRAW_TEX
							  mipmapped:NO];

	[ztd setUsage:MTLTextureUsageRenderTarget];

	id<MTLTexture> zt = [dev newTextureWithDescriptor:ztd];

	if (zt == nil) {
		printf("drvtest: depth: the depth attachment could not be made; the driver has "
		       "printed the compiler library's own reason\n");
		return;
	}
	printf("drvtest: depth: the depth attachment is a %s of pixel format %lu\n",
	       class_getName(object_getClass(zt)), (unsigned long)[zt pixelFormat]);

	/* And the proof that it is not readable as colour, which is the contract and not an
	 * accident: a caller asking for a depth texel the colour way is told which call to
	 * use. The driver prints the library's sentence. */
	{
		unsigned char ignored[DRAW_TEX * DRAW_TEX * 8];

		memset(ignored, 0, sizeof(ignored));
		printf("drvtest: depth: -getBytes: on the depth attachment must be refused:\n");
		[zt getBytes:ignored
		 bytesPerRow:DRAW_TEX * 8
		  fromRegion:MTLRegionMake2D(0, 0, DRAW_TEX, DRAW_TEX)
		 mipmapLevel:0];
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	[pd setVertexFunction:nil];   /* the library generates the stage, as air_run does */
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	/* Both aspect properties, as an application with a combined format sets them: one
	 * texture behind the two, which is what the library requires of the pair. */
	[pd setDepthAttachmentPixelFormat:depth_attachment_format];
	if (depth_attachment_format == MTLPixelFormatDepth32Float_Stencil8) {
		[pd setStencilAttachmentPixelFormat:depth_attachment_format];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];

	[pd release];
	printf("drvtest: depth: newRenderPipelineStateWithDescriptor: gave %s%s%s\n",
	       pipeline != nil ? class_getName(object_getClass(pipeline)) : "nil",
	       err != nil ? ", error: " : "",
	       err != nil ? [[err localizedDescription] UTF8String] : "");
	if (pipeline == nil) {
		return;
	}
	SEL writes = sel_getUid("writesDepth");
	bool shader_writes_depth =
	    [pipeline respondsToSelector:writes] &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(pipeline, writes);

	printf("drvtest: depth: this pipeline's fragment shader %s to [[depth]]\n",
	       shader_writes_depth ? "STORES" : "stores nothing");

	/*
	 * The two depth/stencil states, which is the first MTLDepthStencilState this driver
	 * has ever been asked for. What they answer about themselves is printed, because
	 * those four methods are the whole of <MTLDepthStencilStateSPI> beyond the public
	 * protocol and Apple's base class answers NO to all four -- so "writesDepth = YES"
	 * here is the override being read back, not a default.
	 */
	MTLDepthStencilDescriptor *dsd = [[MTLDepthStencilDescriptor alloc] init];

	[dsd setDepthCompareFunction:MTLCompareFunctionLess];
	[dsd setDepthWriteEnabled:YES];
	id<MTLDepthStencilState> less = [dev newDepthStencilStateWithDescriptor:dsd];

	[dsd setDepthCompareFunction:MTLCompareFunctionAlways];
	id<MTLDepthStencilState> always = [dev newDepthStencilStateWithDescriptor:dsd];

	[dsd release];
	if (less == nil || always == nil) {
		printf("drvtest: depth: newDepthStencilStateWithDescriptor: gave nil, so there "
		       "is no state to test with\n");
		return;
	}
	printf("drvtest: depth: newDepthStencilStateWithDescriptor: gave %s\n",
	       class_getName(object_getClass(less)));
	{
		static const char *const spi[4] = { "readsDepth", "writesDepth", "readsStencil",
						    "writesStencil" };
		unsigned missing = 0;

		printf("drvtest: depth: the Less state answers");
		for (unsigned i = 0; i < 4; i++) {
			SEL s = sel_getUid(spi[i]);

			if (![less respondsToSelector:s]) {
				missing++;
				printf(" %s=unanswered", spi[i]);
				continue;
			}
			printf(" %s=%s", spi[i],
			       ((BOOL (*)(id, SEL))objc_msgSend)(less, s) ? "YES" : "NO");
		}
		printf(" (%u unanswered)\n", missing);

		Protocol *proto = objc_getProtocol("MTLDepthStencilState");
		unsigned n = 0, unanswered = 0;

		if (proto != NULL) {
			struct objc_method_description *req =
			    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

			for (unsigned i = 0; i < n; i++) {
				if (![less respondsToSelector:req[i].name]) {
					printf("drvtest: depth:   MTLDepthStencilState still "
					       "owes %s\n", sel_getName(req[i].name));
					unanswered++;
				}
			}
			free(req);
		}
		printf("drvtest: depth: of %u methods MTLDepthStencilState requires, %u are "
		       "unanswered\n", n, unanswered);
	}

	/* The shader's constant buffer, bound at every index up to 16 because the one this
	 * defaults to is declared at 14 (draw_path says why). Its bytes are what change
	 * between steps, so a draw that was not discarded leaves a different colour. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform == nil || [uniform contents] == NULL) {
		printf("drvtest: depth: no constant buffer\n");
		return;
	}

	/* The textures the shader declares, if any, made the shape each slot asks for --
	 * and then seeded in [0, 1] when the shader stores what it samples into [[depth]],
	 * because air_run's 100.0 would clamp to the far plane and read as the clear. */
	id<MTLTexture> dtex[RW_TEX_SLOTS] = { nil };
	id<MTLSamplerState> dsamp = nil;
	unsigned declared = 0;
	char texwhy[256] = "";

	if (!textures_for_shader(dev, fragment, dtex, RW_TEX_SLOTS, &declared, texwhy,
				 sizeof(texwhy))) {
		printf("drvtest: depth: %s: %s\n", fragment_function, texwhy);
		return;
	}
	bool declared_3d = false;

	for (unsigned i = 0; i < RW_TEX_SLOTS; i++) {
		if (dtex[i] == nil) {
			continue;
		}
		if ([dtex[i] textureType] == MTLTextureType3D) {
			declared_3d = true;
		}
		if (shader_writes_depth) {
			fill_depth_texture(dtex[i]);
		}
	}
	if (declared) {
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		dsamp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
		printf("drvtest: depth: %u texture slot%s declared, seeded with %s\n", declared,
		       declared == 1 ? "" : "s",
		       shader_writes_depth ? "(x + 4y + 16z + 0.5) / 64 in every channel"
					   : "air_run's texels");
	}

	id<MTLCommandQueue> queue = [dev newCommandQueue];

	/*
	 * One step: a render pass of its own, because a pass is where a load action lives
	 * and these steps differ in whether they clear. The first clears the colour and the
	 * depth; the rest load both, which is how the depth of the previous draw is still
	 * there to be tested against.
	 */
	struct step {
		const char *what;
		float z;
		bool pattern;
		unsigned char fill;
		bool clear;
		bool control;
		bool expect_draw;
	};
	static const struct step colour_steps[4] = {
		{ "the near draw at depth 0.5",            0.5f,  true,  0x00, true,  false, true },
		{ "the far draw at depth 0.75",            0.75f, false, 0xa5, false, false, false },
		{ "the nearer draw at depth 0.25",         0.25f, false, 0x5a, false, false, true },
		{ "the control at depth 0.75 with Always", 0.75f, false, 0x3c, false, true,  true },
	};
	static const struct step depth_steps[1] = {
		{ "the draw, with the test off and depth writes on", 0.5f, true, 0x00, true,
		  true, true },
	};
	const struct step *steps = shader_writes_depth ? depth_steps : colour_steps;
	unsigned nsteps = shader_writes_depth ? 1 : 4;
	unsigned pixel[4][4];
	float depth_at[4];
	bool uniform_depth = true;

	memset(pixel, 0, sizeof(pixel));
	memset(depth_at, 0, sizeof(depth_at));
	for (unsigned s = 0; s < nsteps; s++) {
		unsigned char *bytes = [uniform contents];

		for (int i = 0; i < 256; i++) {
			bytes[i] = steps[s].pattern ?
			    (unsigned char)((i * 7 + 13) & 0xff) : steps[s].fill;
		}

		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

		for (int i = 0; i < 3; i++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:i];

			[a setTexture:targets[i]];
			[a setLoadAction:steps[s].clear ? MTLLoadActionClear
							: MTLLoadActionLoad];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
			[a setStoreAction:MTLStoreActionStore];
		}
		MTLRenderPassDepthAttachmentDescriptor *za = [rp depthAttachment];

		[za setTexture:zt];
		[za setLoadAction:steps[s].clear ? MTLLoadActionClear : MTLLoadActionLoad];
		[za setClearDepth:1.0];
		[za setStoreAction:MTLStoreActionStore];

		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: depth: %s: no encoder\n", steps[s].what);
			return;
		}
		[enc setRenderPipelineState:pipeline];
		[enc setDepthStencilState:steps[s].control ? always : less];
		/* The quad's depth. MTLViewport's znear is the only thing in Metal that moves
		 * it: the generated vertex stage puts all four corners at clip z = 0, and
		 * Metal's NDC z starts at 0 on the near plane, so the window depth of what is
		 * drawn is znear exactly. The rectangle is the whole attachment, which is what
		 * the driver serves. */
		[enc setViewport:(MTLViewport){ 0.0, 0.0, DRAW_TEX, DRAW_TEX, steps[s].z, 1.0 }];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			if (i < RW_TEX_SLOTS && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		printf("drvtest: depth: %s: status %lu%s%s\n", steps[s].what,
		       (unsigned long)[cmd status], [cmd error] != nil ? ", error: " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
		if ([cmd status] != RW_CB_COMPLETED) {
			return;
		}

		[targets[2] getBytes:pixel[s]
			 bytesPerRow:DRAW_TEX * 4 * 4
			  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
			 mipmapLevel:0];
		printf("drvtest: depth: %s: the pixel at (0, 0) of attachment 2 is %u %u %u %u\n",
		       steps[s].what, pixel[s][0], pixel[s][1], pixel[s][2], pixel[s][3]);

		float map[DRAW_TEX * DRAW_TEX];

		if (!read_depth_buffer(zt, map, steps[s].what)) {
			uniform_depth = false;
		}
		depth_at[s] = map[0];
	}

	/* The verdicts. Each says what would have to be true for it to read otherwise, and
	 * the tolerance is the format's: Depth32Float holds 0.25, 0.5 and 0.75 exactly, so
	 * there it is ==; Depth16Unorm holds none of them and gets one step of itself. */
	float tol = depth_attachment_format == MTLPixelFormatDepth16Unorm ?
	    1.0f / 65535.0f : 0.0f;

	if (shader_writes_depth) {
		float want = depth_texel_value(1, 1, declared_3d ? 1 : 0);
		float got = depth_at[0];
		float d = got - want;
		bool same = (d < 0.0f ? -d : d) <= tol;

		printf("drvtest: depth: the shader sampled texel (1, 1, %u) and stored %.9g to "
		       "[[depth]]; the buffer holds %.9g\n", declared_3d ? 1 : 0, want, got);
		printf("drvtest: depth: the quad's own depth was 0.5 and the clear was 1, so "
		       "neither could be mistaken for it\n");
		printf("drvtest: depth: %s\n", same && uniform_depth ?
		       "THE SAME -- a Metal fragment shader's [[depth]] reached the depth "
		       "buffer on the Adreno" :
		       "DIFFERENT, and a depth buffer that is nearly right is worse than none");
	} else {
		bool far_discarded = memcmp(pixel[1], pixel[0], sizeof(pixel[0])) == 0;
		bool nearer_drawn = memcmp(pixel[2], pixel[0], sizeof(pixel[0])) != 0;
		bool control_drawn = memcmp(pixel[3], pixel[2], sizeof(pixel[0])) != 0;
		static const float want[4] = { 0.5f, 0.5f, 0.25f, 0.75f };
		bool depths_right = true;

		for (unsigned s = 0; s < 4; s++) {
			float d = depth_at[s] - want[s];

			if ((d < 0.0f ? -d : d) > tol) {
				depths_right = false;
			}
		}
		printf("drvtest: depth: the far draw was %s, and its buffer held a different "
		       "fill, so an undiscarded one would have changed the pixel\n",
		       far_discarded ? "DISCARDED (the pixel did not change)"
				     : "NOT discarded -- the pixel changed");
		printf("drvtest: depth: the nearer draw %s\n",
		       nearer_drawn ? "PASSED (the pixel changed)"
				    : "did NOT pass, and it is in front of what was there");
		printf("drvtest: depth: the Always control at the same far depth %s\n",
		       control_drawn ? "DREW (so the compare function discarded the other one, "
				       "not the draw)"
				     : "did not draw either, so the fault is not the depth test");
		printf("drvtest: depth: depth after each step: %.9g %.9g %.9g %.9g (0.5, 0.5 "
		       "kept, 0.25, 0.75)\n", depth_at[0], depth_at[1], depth_at[2],
		       depth_at[3]);
		printf("drvtest: depth: %s\n",
		       far_discarded && nearer_drawn && control_drawn && depths_right &&
		       uniform_depth ?
		       "DEPTH TESTING DISCARDS ON THE ADRENO -- the compare function, the depth "
		       "write and the buffer all agree" :
		       "SOMETHING ABOVE DISAGREES, and a depth test that is nearly right is "
		       "worse than none");
	}
	fflush(stdout);
}

/*
 * Where it died, in terms of the image it died in.
 *
 * A program that faults on this port leaves nothing behind -- the kernel prints "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is all -- so the fault has to print itself. The
 * addresses that matter here are not in the shared cache but inside
 * /redwood/lib/libredwoodair.dylib, so the slide trick the other probes use (docs/metal.md
 * section 6) is no help: dladdr gives the image and the offset within it, which
 * llvm-addr2line on the machine turns back into a line.
 */
static void
fault(int sig, siginfo_t *info, void *uap)
{
	ucontext_t *uc = uap;
	uintptr_t pc = 0, lr = 0, fp = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
	}
	printf("drvtest: SIGNAL %d at %p\n", sig, info != NULL ? info->si_addr : NULL);
	for (int i = 0; i < 10; i++) {
		uintptr_t at = i == 0 ? pc : i == 1 ? lr : 0;
		Dl_info d;

		if (i >= 2) {
			if (fp <= 0x1000) {
				break;
			}
			at = ((uintptr_t *)fp)[1];
			fp = ((uintptr_t *)fp)[0];
			if (at == 0) {
				break;
			}
		}
		if (dladdr((void *)at, &d) != 0 && d.dli_fname != NULL) {
			const char *slash = strrchr(d.dli_fname, '/');

			printf("drvtest:   %-2s %s+%#lx  (%s)\n", i == 0 ? "pc" : i == 1 ? "lr" : "",
			       slash != NULL ? slash + 1 : d.dli_fname,
			       at - (uintptr_t)d.dli_fbase,
			       d.dli_sname != NULL ? d.dli_sname : "no symbol");
		} else {
			printf("drvtest:   %-2s %#lx (in no image)\n",
			       i == 0 ? "pc" : i == 1 ? "lr" : "", at);
		}
	}
	fflush(stdout);
	_exit(sig);
}

/*
 * Every fragment shader of a library, drawn, one line each.
 *
 * One shader drawn is an example; 255 of them have a reference each (docs/air.md: air_run
 * draws every fragment shader of the corpus and the pixel was compared), so drawing them all
 * turns an example into coverage. One process does the whole library because the loader keeps
 * only the first 256 KB and the last 1 MB of a boot's log: 271 processes would lose the middle
 * of their own table, and a count read off a cut log is a lower bound pretending to be a
 * number.
 *
 * The line is deliberately terse and machine-readable -- name, status, four words -- because
 * the comparison belongs to tools/air/metalcheck.py, which produces each reference by running
 * air_run rather than trusting anything written here.
 *
 * Each shader is bound the textures IT declares, asked of the compiler library through
 * rwa_module_textures. The run before this one bound a single kind and format for the whole
 * library and 13 of Apple's 271 fragment shaders were refused by name for declaring a
 * texture3d, a texture2d<int> or a multisample texture: a true sentence about this harness
 * and nothing about those shaders. A slot no shader named keeps the single binding, so the
 * 256 shaders that declare no texture are bound what they were bound before and their pixels
 * stay comparable with that run's.
 */
static void
draw_every(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	   MTLRenderPassDescriptor *rp, id<MTLBuffer> uniform, id<MTLTexture> *dtex,
	   id<MTLSamplerState> dsamp, id<MTLTexture> *targets)
{
	unsigned drew = 0, refused = 0, nopipe = 0, notex = 0, asked = 0;

	unsigned seen = 0;

	for (NSString *name in [library functionNames]) {
		id<MTLFunction> fn = [library newFunctionWithName:name];
		NSError *err = nil;

		if (fn == nil || [fn functionType] != MTLFunctionTypeFragment) {
			continue;
		}
		if (seen++ < draw_from || drew + refused + nopipe + notex >= draw_count) {
			continue;
		}
		/*
		 * What THIS shader declares, made for it, before anything is bound. The slots
		 * it does not name keep the sweep's own dtex[], so the 256 shaders that
		 * declare no texture at all are bound exactly what they were bound before and
		 * their numbers stay comparable with the previous run's.
		 */
		id<MTLTexture> own[RW_TEX_SLOTS] = { nil };
		unsigned ndecl = 0;
		char whynot[256] = "";

		if (use_decl && !textures_for_shader(dev, fn, own, RW_TEX_SLOTS, &ndecl, whynot,
						     sizeof(whynot))) {
			notex++;
			printf("drvtest: drawall %-34s no texture: %s\n", [name UTF8String],
			       whynot);
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		if (ndecl > 0) {
			asked++;
		}
		/* The name BEFORE the work and not only in the row after it.
		 *
		 * The first sweep with per-shader textures stopped dead at its third shader
		 * (logs/070755-run.log): two multisample refusals, then one line from the
		 * driver about a generated vertex stage, then nothing for the rest of the
		 * boot. Everything about which shader that was had to be inferred, and
		 * -[_MTLLibrary functionNames] is not in container order, so it could not be
		 * inferred from the index either. A line here costs one line per shader and
		 * makes the next hang name itself.
		 */
		printf("drvtest: drawall %-34s about to draw, %u declared texture%s\n",
		       [name UTF8String], ndecl, ndecl == 1 ? "" : "s");
		fflush(stdout);
		MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

		[pd setFragmentFunction:fn];
		[pd setVertexFunction:nil];
		for (int i = 0; i < 3; i++) {
			[[[pd colorAttachments] objectAtIndexedSubscript:i]
			    setPixelFormat:MTLPixelFormatRGBA32Uint];
		}
		id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
										   error:&err];
		[pd release];
		if (ps == nil) {
			nopipe++;
			printf("drvtest: drawall %-34s no pipeline: %s\n", [name UTF8String],
			       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			[ps release];
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		[enc setRenderPipelineState:ps];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			/* this shader's own texture where it declared one, and the sweep's
			 * single binding where it did not: a slot nobody named is a slot
			 * nothing reads, and leaving the old binding there keeps the 256
			 * shaders that declare none bound exactly as they were */
			if (i < RW_TEX_SLOTS && own[i] != nil) {
				[enc setFragmentTexture:own[i] atIndex:i];
			} else if (i < 4 && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];

		if ([cmd status] != RW_CB_COMPLETED) {
			refused++;
			printf("drvtest: drawall %-34s status %lu %s\n", [name UTF8String],
			       (unsigned long)[cmd status],
			       [cmd error] != nil ?
			       [[[cmd error] localizedDescription] UTF8String] : "");
			[ps release];
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		/* All three attachments, because each shader writes the targets IT declares:
		 * some write target 0, some 1, some 2, and reading only one of them made 50 of
		 * 64 shaders uncomparable with "air_run printed no pixel for render target 2".
		 * Which target to compare is then the reference's business, not the harness's. */
		drew++;
		printf("drvtest: drawall %-34s status %d", [name UTF8String], RW_CB_COMPLETED);
		for (int t = 0; t < 3; t++) {
			unsigned pixel[4] = { 0 };

			[targets[t] getBytes:pixel
				bytesPerRow:4 * 4 * 4
				 fromRegion:MTLRegionMake2D(0, 0, 1, 1)
				mipmapLevel:0];
			printf(" t%d %u %u %u %u", t, pixel[0], pixel[1], pixel[2], pixel[3]);
		}
		/* What was bound, on the same line as the pixel: a row of this table is only
		 * a measurement if what it was measured against is in it, and metalcheck.py
		 * has to know which shape air_run should hold to produce the reference. */
		if (ndecl > 0) {
			printf(" tex");
			for (unsigned k = 0; k < RW_TEX_SLOTS; k++) {
				if (own[k] != nil) {
					printf(" %u=%lu:%lu", k,
					       (unsigned long)[own[k] textureType],
					       (unsigned long)[own[k] pixelFormat]);
				}
			}
		}
		printf("\n");
		[ps release];
		release_slots(own, RW_TEX_SLOTS);
	}
	printf("drvtest: drawall: %u drew, %u refused at the draw, %u got no pipeline, "
	       "%u could not be given the textures they declare (of the %u fragment shaders "
	       "from index %u; %u declared a texture and were bound what they declared)\n",
	       drew, refused, nopipe, notex, seen, draw_from, asked);
	fflush(stdout);
}

/*
 * --picture: the same shaders, at the size of the phone's panel, where a person can look
 * at them.
 *
 * Everything this driver has drawn so far went into a 4x4 off-screen attachment and came
 * back as four numbers. That is a measurement and it is not a picture, and they are not
 * the same claim: 4x4 says nothing about an attachment a megapixel wide, about a varying
 * that has to interpolate over a thousand pixels instead of four, or about a sampler
 * asked for a texel between two texels rather than the one a coordinate lands on.
 *
 * What is drawn, in three bands that together are exactly the panel (1080x2400 -- the
 * size the kernel prints at boot: "display 1125x2436 on a panel of 1080x2400"):
 *
 *   rows 0..1103      64 of Apple's own clearFragment shaders, one per square of an 8x8
 *                     mosaic of 135x138 tiles. The number in the name is a mask of two
 *                     bits per render target -- 0 none, 1 float4, 2 int4, 3 uint4 -- so
 *                     the 64 numbered 4k+1 are exactly those that write a float4 to
 *                     target 0, and each reads that float4 out of the constant buffer at
 *                     index 14 (its ClearFragmentInput is { float4, int4, uint4 } and
 *                     every float4 output is the first field). Each square is given a
 *                     colour of its own in that buffer, so a square is one shader's
 *                     answer and the mosaic is 64 of them side by side.
 *   rows 1104..1751   Apple's texturedVertex2D + texturedFragment2DFloat -- the pipeline
 *                     AppleMetalGLRenderer blits a texture with -- sampling an 8x8
 *                     texture over the whole 1080x648 attachment through a LINEAR
 *                     sampler. Every pixel of it is an interpolated texture coordinate
 *                     and a filtered texel, which is the one thing a 4x4 draw over a
 *                     constant coordinate cannot show at all (docs/metal.md 10.32).
 *   rows 1752..2399   the same draw with a NEAREST sampler. Blocky where the band above
 *                     is smooth, and that difference is the sampler state reaching the
 *                     hardware; it is also the band in which every pixel is exactly one
 *                     texel, so every pixel of it can be counted.
 *
 * The pixels reach the panel through the kernel's own GPU service -- kRedwoodShow,
 * iokit/Kernel/RedwoodGPU.cpp, the selector src/gl_test.c already puts its triangle on the
 * screen with -- and that is what `./xl screenshot` reads. Said plainly because it
 * matters: THE PRESENTATION IS A COPY IN THE KERNEL AND IS NOT METAL'S. What is claimed
 * is that every one of the 2 592 000 pixels handed over was computed by this driver's
 * Metal pipelines on the Adreno. Nothing here is drawn by the processor -- no border, no
 * label, no background -- so that the claim needs no exceptions.
 *
 * And the numbers beside it, because "it looks right" is not a measurement:
 *
 *   - the mosaic: every pixel of every tile against the colour that tile's buffer named,
 *     and every one of the 256 (shader, render target) pairs against what the shader's
 *     own name says it writes -- a target the name does not list must still hold the
 *     clear, which is the half a chart of colours alone would not say. Integer targets
 *     are not checked and are counted as not checked;
 *   - the nearest band: every pixel against the texel the sampler must have returned for
 *     it, floor(x / 135) and floor(y / 81);
 *   - the linear band: the 64 pixels that fall exactly on a texel centre. The band is 8
 *     texels over 1080 columns and 8 over 648 rows, so the scales are 135 and 81, both
 *     ODD -- and that is why a pixel centre can coincide with a texel centre at all
 *     (pixel p is texel centre i when p = s*i + (s-1)/2), which leaves the filter no
 *     weight to round there. Chosen, not noticed.
 *
 * All of it was drawn on this machine first, through the same library on softpipe, so
 * every count below has a reference and not an expectation.
 */
enum {
	/* The panel, not the display. A swap through IOMobileFramebuffer is scaled from the
	 * 1125x2436 user space sees down to this; kRedwoodShow is not scaled at all -- it is
	 * a row-by-row copy cut to the panel -- so this is the size to draw. */
	PANEL_W = 1080, PANEL_H = 2400,
	PIC_TILES = 8,
	PIC_TILE_W = PANEL_W / PIC_TILES,               /* 135 */
	PIC_TILE_H = 138,                               /* so eight rows of tiles are 1104, */
	PIC_MOSAIC_H = PIC_TILES * PIC_TILE_H,          /* and 1104 + 648 + 648 is 2400 */
	PIC_BAND_H = (PANEL_H - PIC_MOSAIC_H) / 2,      /* exactly: no row of the panel is */
	PIC_LINEAR_Y = PIC_MOSAIC_H,                    /* left for the processor to fill */
	PIC_NEAREST_Y = PIC_MOSAIC_H + PIC_BAND_H,
	PIC_TEX = 8,
	PIC_SX = PANEL_W / PIC_TEX,                     /* 135, odd */
	PIC_SY = PIC_BAND_H / PIC_TEX,                  /* 81, odd */
};

/* Eight levels of a channel, each an exact byte. A texel written as k/255 comes back as
 * exactly k through an 8-bit unorm attachment, so no check here needs a tolerance and a
 * difference of one is a difference. */
static const unsigned char pic_ramp[PIC_TILES] = { 0, 36, 73, 109, 146, 182, 219, 255 };

/*
 * The colour of mosaic tile (i, j) and of texel (x, y), in the panel's own order: blue
 * first. The four corners of each are four different primaries on purpose -- a channel
 * swapped anywhere between the shader and the panel, or a picture mirrored, moves which
 * corner is which, and that is seen before it is counted.
 */
static void
pic_mosaic_colour(unsigned i, unsigned j, unsigned char bgra[4])
{
	bgra[0] = pic_ramp[PIC_TILES - 1 - i];
	bgra[1] = pic_ramp[j];
	bgra[2] = pic_ramp[i];
	bgra[3] = 255;
}

static void
pic_texel_colour(unsigned x, unsigned y, unsigned char bgra[4])
{
	bgra[0] = pic_ramp[y];
	bgra[1] = pic_ramp[PIC_TILES - 1 - y];
	bgra[2] = pic_ramp[x];
	bgra[3] = 255;
}

/* The panel's pixels as this process holds them, and the request the kernel takes them in
 * (iokit/Kernel/RedwoodGPU.cpp, struct redwood_show). Declared here and not included,
 * because this program has no header of the kernel's -- exactly as src/gl_test.c does. */
static unsigned char *pic_panel;

struct pic_show_request {
	uint64_t pixels;
	uint32_t width, height, row_bytes, pad;
};

/*
 * The connection to that service, opened BEFORE anything is drawn and used after every
 * stage that finishes, rather than once at the end.
 *
 * That order is not tidiness, it is what the first boot cost (logs/101536-run.log): the
 * mosaic drew and counted perfectly on the Adreno and then the process stopped dead on
 * its way into the bands, so nothing ever reached the panel and the screenshot caught the
 * loader's own splash. A stage that finished is a picture; it belongs on the screen the
 * moment it is finished, and a stall after it should cost the bands and not the
 * screenshot. The copy persists by itself -- MiniIOMobileFramebufferShow writes the boot
 * framebuffer and, with JOBS="" , nothing else touches it -- so one push is enough to be
 * photographed even if the process never reaches the repainting loop at the end.
 */
static io_connect_t pic_connection;

static void
pic_open_panel(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));

	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &pic_connection) != 0) {
		pic_connection = 0;
		printf("drvtest: picture: no GPU service in this kernel to put the picture on "
		       "the panel with; the counts below still stand but nothing will be "
		       "seen\n");
	} else {
		printf("drvtest: picture: the panel is open; every stage goes on the screen as "
		       "it is finished\n");
	}
	fflush(stdout);
}

static void
pic_push(const char *what)
{
	struct pic_show_request show = {
		(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
	};

	if (pic_connection == 0 || pic_panel == NULL) {
		return;
	}
	kern_return_t r = IOConnectCallStructMethod(pic_connection, 0x7f, &show, sizeof(show),
						    NULL, NULL);

	printf("drvtest: picture: ON THE PANEL (%#x): %s. A screenshot taken now has it\n", r,
	       what);
	fflush(stdout);
}

static uint64_t
pic_now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

/*
 * Every step of the band stage says its own name before it starts, and how long the step
 * before it took.
 *
 * The first boot of this mode had NO line at all between the mosaic's summary and the
 * kernel's idle heartbeat, because on this path nothing prints unless something fails
 * (logs/101536-run.log) -- so the stall had the whole stage to hide in. The second boot,
 * with these lines, put it between two statements: "the probe: asking for an encoder" was
 * the last thing printed. The timestamp is here because that boot still could not say
 * whether the step was stuck or merely slow, and those are different bugs.
 */
static char pic_where[192] = "before anything";
static volatile uint64_t pic_since_ms;
static volatile int pic_alive = 1;

static void
pic_step(const char *what)
{
	uint64_t now = pic_now_ms(), took = pic_since_ms ? now - pic_since_ms : 0;

	printf("drvtest: picture: band step: %s   (the step before took %llu ms)\n", what,
	       (unsigned long long)took);
	fflush(stdout);
	snprintf(pic_where, sizeof(pic_where), "%s", what);
	pic_since_ms = now;
}

/*
 * And a thread that says the process is still alive and still in that step.
 *
 * Silence has meant three different things so far -- blocked, killed, and simply not
 * printing -- and nothing in the log told them apart. This separates two of the three: as
 * long as these lines keep coming, the process is alive and the step named in them has not
 * returned; when they stop, the process is gone. It writes with write(2) and not printf on
 * purpose: if the main thread is stuck holding stdio's lock, a printf here would block too
 * and the one case this is for would be the one case it could not report.
 *
 * It backs off by doubling -- 2, 4, 8, 16 ... seconds -- because the loader keeps the TAIL
 * of a long log, and a line a second for four minutes would push the measurements that
 * matter out of it.
 */
static void *
pic_heartbeat(void *unused)
{
	uint64_t next = 2000, watching = 0;

	(void)unused;
	for (;;) {
		char line[320];
		uint64_t in;
		int n;

		usleep(250000);
		if (!pic_alive) {
			return NULL;
		}
		if (pic_since_ms != watching) {
			watching = pic_since_ms;   /* a new step: count from it, not from the */
			next = 2000;               /* first slow one */
		}
		in = pic_now_ms() - pic_since_ms;
		if (in < next) {
			continue;
		}
		next *= 2;
		n = snprintf(line, sizeof(line), "drvtest: picture: STILL IN \"%s\" after "
			     "%llu ms -- this process is alive and that step has not "
			     "returned\n", pic_where, (unsigned long long)in);
		if (n > 0) {
			ssize_t written = write(1, line, (size_t)n);

			(void)written;
		}
	}
}

/*
 * What the QUEUE says about itself, rather than what this file infers.
 *
 * The second ladder named the stall as -[MTLCommandQueue commandBuffer] on the 65th
 * command buffer of the process, and 64 was Metal's own maxCommandBufferCount: every
 * _MTLCommandBuffer waits on the queue's semaphore in its designated initialiser and owes
 * it a signal, and this driver never paid (redwoodmetal.m). That was an inference from a
 * number that happened to be 64. These two lines are the measurement instead: the queue
 * reports how many buffers it takes at a time and how many of this process's still hold a
 * slot, and it says OUT LOUD when the next one will block -- before it does.
 */
static unsigned long
pic_queue_state(id<MTLCommandQueue> queue, const char *when)
{
	NSUInteger (*ask)(id, SEL) = (NSUInteger (*)(id, SEL))objc_msgSend;
	SEL outstanding = sel_getUid("redwoodOutstandingCommandBuffers");
	SEL made = sel_getUid("redwoodCommandBuffersMade");
	SEL most = sel_getUid("maxCommandBufferCount");

	if (![queue respondsToSelector:outstanding] || ![queue respondsToSelector:most]) {
		printf("drvtest: picture: %s: this queue does not say how many command buffers "
		       "hold a slot, so the next block would again have to be inferred\n", when);
		fflush(stdout);
		return ~0UL;
	}
	unsigned long o = (unsigned long)ask(queue, outstanding);
	unsigned long m = (unsigned long)ask(queue, most);
	unsigned long n = [queue respondsToSelector:made] ?
	    (unsigned long)ask(queue, made) : 0;

	printf("drvtest: picture: %s: the queue takes %lu command buffers at a time, %lu "
	       "made so far, %lu still holding a slot%s\n", when, m, n, o,
	       m != 0 && o >= m ? " -- THE NEXT -commandBuffer WILL BLOCK FOR EVER" : "");
	fflush(stdout);
	return o;
}

/* The backing-off clock starts again at every step, so "STILL IN" counts from the step it
 * names and not from the first slow one. */
static void
pic_watch(void)
{
	pthread_t thread;

	pic_since_ms = pic_now_ms();
	if (pthread_create(&thread, NULL, pic_heartbeat, NULL) == 0) {
		pthread_detach(thread);
		printf("drvtest: picture: a second thread will say every so often that this "
		       "process is alive and which step it is in\n");
	} else {
		printf("drvtest: picture: no second thread, so silence below will again mean "
		       "either blocked or killed\n");
	}
	fflush(stdout);
}

/*
 * Four attachments, all BGRA8Unorm and all one size: the mosaic's shaders write up to
 * four render targets and Metal requires a pass's formats to be the pipeline's, so four
 * it is for every pipeline here. BGRA8Unorm because that is the panel's own order, so
 * what -getBytes: hands back is what the panel takes, with nothing swapped in between.
 */
static bool
pic_targets(id<MTLDevice> dev, id<MTLTexture> *t, unsigned n, unsigned w, unsigned h,
	    MTLRenderPassDescriptor **rp)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	*rp = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	for (unsigned i = 0; i < n; i++) {
		t[i] = [dev newTextureWithDescriptor:td];
		if (t[i] == nil) {
			printf("drvtest: picture: attachment %u of %ux%u could not be made\n",
			       i, w, h);
			return false;
		}
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[*rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:t[i]];
		[a setLoadAction:MTLLoadActionClear];
		/* opaque black: a tile nothing drew is black on the panel and black in the
		 * count, and the two then agree about which tile it was */
		[a setClearColor:MTLClearColorMake(0, 0, 0, 1)];
		[a setStoreAction:MTLStoreActionStore];
	}
	return true;
}

static void
pic_release_targets(id<MTLTexture> *t, unsigned n, MTLRenderPassDescriptor *rp)
{
	for (unsigned i = 0; i < n; i++) {
		[t[i] release];
		t[i] = nil;
	}
	[rp release];
}

static id<MTLRenderPipelineState>
pic_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
	     unsigned n, const char *what)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	for (unsigned i = 0; i < n; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatBGRA8Unorm];
	}
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];
	[pd release];
	if (ps == nil) {
		printf("drvtest: picture: %s got no pipeline: %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/*
 * One draw into the pass, waited on. True when the GPU finished it.
 *
 * `trace` says every part of it out loud, which the band stage needs and the mosaic's 64
 * draws do not: this driver's command buffer RECORDS the draw and performs it on -commit
 * (10.13), so encoding, committing and waiting are three different places to stop and the
 * log has to tell them apart. The mosaic passes false so that 64 working draws do not bury
 * the one that does not.
 */
static bool
pic_draw(id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	 id<MTLRenderPipelineState> ps, id<MTLBuffer> *vbuf, unsigned nvbuf,
	 id<MTLBuffer> fbuf, unsigned nfbuf, id<MTLTexture> tex, id<MTLSamplerState> samp,
	 const char *what, bool trace)
{
	char line[192];

	/* The second boot stopped between these two calls and the one line covered both, so
	 * they are two lines now. Neither of them has anything in it proportional to the
	 * attachment's area -- -commandBuffer allocates an object and an NSMutableArray, and
	 * the encoder's init reads four properties per attachment (redwoodmetal.m) -- which
	 * is exactly why the two have to be told apart before anything else is believed. */
	if (trace) {
		snprintf(line, sizeof(line), "%s: asking the queue for a command buffer", what);
		pic_step(line);
	}
	id<MTLCommandBuffer> cmd = [queue commandBuffer];

	if (trace) {
		snprintf(line, sizeof(line), "%s: got the command buffer; asking IT for a "
			 "render encoder", what);
		pic_step(line);
	}
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (trace) {
		snprintf(line, sizeof(line), "%s: got the encoder", what);
		pic_step(line);
	}

	if (enc == nil) {
		printf("drvtest: picture: %s got no encoder\n", what);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < nfbuf; i++) {
		[enc setFragmentBuffer:fbuf offset:0 atIndex:i];
	}
	for (unsigned i = 0; i < nvbuf; i++) {
		[enc setVertexBuffer:vbuf[i] offset:0 atIndex:i];
	}
	if (tex != nil) {
		[enc setFragmentTexture:tex atIndex:0];
	}
	if (samp != nil) {
		[enc setFragmentSamplerState:samp atIndex:0];
	}
	if (trace) {
		snprintf(line, sizeof(line), "%s: bound; encoding the quad", what);
		pic_step(line);
	}
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
	if (trace) {
		snprintf(line, sizeof(line), "%s: encoded; committing (this driver does the "
			 "work here, not at the wait)", what);
		pic_step(line);
	}
	[cmd commit];
	if (trace) {
		snprintf(line, sizeof(line), "%s: committed; waiting", what);
		pic_step(line);
	}
	[cmd waitUntilCompleted];
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: picture: %s: status %lu%s%s\n", what,
		       (unsigned long)[cmd status], [cmd error] != nil ? ", " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String]
					  : "");
		return false;
	}
	return true;
}

/* One rectangle of an attachment into the panel's buffer at (x0, y0). */
static void
pic_blit(const unsigned char *from, unsigned w, unsigned h, unsigned x0, unsigned y0)
{
	for (unsigned y = 0; y < h; y++) {
		memcpy(pic_panel + ((size_t)(y0 + y) * PANEL_W + x0) * 4,
		       from + (size_t)y * w * 4, (size_t)w * 4);
	}
}

/* The mosaic: 64 of Apple's clearFragment shaders, each into its own square. */
static void
pic_mosaic(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	id<MTLTexture> mt[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *mrp = nil;

	if (!pic_targets(dev, mt, 4, PIC_TILE_W, PIC_TILE_H, &mrp)) {
		pic_release_targets(mt, 4, mrp);
		return;
	}
	/* The constant buffer every clearFragment reads: declared at index 14, bound at
	 * every index up to 16 for draw_path's reason -- a driver that bound only index 0
	 * would leave the shader reading nothing. Its first float4 is the square's colour
	 * and is rewritten before each draw, which is safe because each draw is waited on
	 * before the next is encoded. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];
	unsigned char *tile = malloc((size_t)PIC_TILE_W * PIC_TILE_H * 4);

	if (uniform == nil || [uniform contents] == NULL || tile == NULL) {
		printf("drvtest: picture: no constant buffer or no memory for a tile\n");
		pic_release_targets(mt, 4, mrp);
		free(tile);
		return;
	}
	unsigned drew = 0, nopipe = 0, nodraw = 0;
	unsigned long rt_colour = 0, rt_clear = 0, rt_skipped = 0, rt_wrong = 0;
	unsigned long bad_mosaic = 0;

	for (unsigned k = 0; k < PIC_TILES * PIC_TILES; k++) {
		unsigned nn = 4 * k + 1, i = k % PIC_TILES, j = k / PIC_TILES;
		char name[32];

		snprintf(name, sizeof(name), "clearFragment%02X", nn);
		id<MTLFunction> fn = [library newFunctionWithName:
		    [NSString stringWithUTF8String:name]];

		if (fn == nil) {
			printf("drvtest: picture: no %s in that library\n", name);
			nopipe++;
			continue;
		}
		id<MTLRenderPipelineState> ps = pic_pipeline(dev, nil, fn, 4, name);

		if (ps == nil) {
			nopipe++;
			[fn release];
			continue;
		}
		unsigned char want[4];

		pic_mosaic_colour(i, j, want);
		float *u = [uniform contents];

		/* zeroed first: the int4 and uint4 fields of ClearFragmentInput are read by
		 * the integer targets this check skips, and memory nobody set would make
		 * "not checked" mean two things at once */
		memset(u, 0, 256);
		u[0] = want[2] / 255.0f;        /* the shader's float4 is r, g, b, a */
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
		if (!pic_draw(queue, mrp, ps, NULL, 0, uniform, 16, nil, nil, name, false)) {
			nodraw++;
			[ps release];
			[fn release];
			continue;
		}
		drew++;
		/* Each of this shader's four targets against what its own name says it
		 * writes. A target the name does not list must still hold the clear. */
		for (unsigned t = 0; t < 4; t++) {
			unsigned kind = (nn >> (2 * t)) & 3;
			unsigned char expect[4] = { 0, 0, 0, 255 };
			unsigned long bad = 0;

			if (kind == 2 || kind == 3) {
				rt_skipped++;
				continue;
			}
			if (kind == 1) {
				memcpy(expect, want, 4);
			}
			[mt[t] getBytes:tile
			    bytesPerRow:PIC_TILE_W * 4
			     fromRegion:MTLRegionMake2D(0, 0, PIC_TILE_W, PIC_TILE_H)
			    mipmapLevel:0];
			for (size_t q = 0; q < (size_t)PIC_TILE_W * PIC_TILE_H; q++) {
				if (tile[q * 4] != expect[0] || tile[q * 4 + 1] != expect[1] ||
				    tile[q * 4 + 2] != expect[2]) {
					bad++;
				}
			}
			if (bad != 0) {
				rt_wrong++;
				if (rt_wrong <= 6) {
					printf("drvtest: picture: %s target %u (%s): %lu of "
					       "%u pixels wrong\n", name, t,
					       kind != 0 ? "its own colour"
							 : "not written, so the clear",
					       bad, PIC_TILE_W * PIC_TILE_H);
				}
			} else if (kind != 0) {
				rt_colour++;
			} else {
				rt_clear++;
			}
			if (t == 0) {
				bad_mosaic += bad;
				pic_blit(tile, PIC_TILE_W, PIC_TILE_H, i * PIC_TILE_W,
					 j * PIC_TILE_H);
			}
		}
		[ps release];
		[fn release];
	}
	free(tile);
	pic_release_targets(mt, 4, mrp);
	printf("drvtest: picture: mosaic: %u of %u shaders drew (%u got no pipeline, %u did "
	       "not finish)\n", drew, PIC_TILES * PIC_TILES, nopipe, nodraw);
	printf("drvtest: picture: mosaic: %lu pixels of %u are not the colour their square's "
	       "buffer named\n", bad_mosaic,
	       PIC_TILES * PIC_TILES * PIC_TILE_W * PIC_TILE_H);
	printf("drvtest: picture: mosaic: of %u (shader, render target) pairs, %lu held the "
	       "colour, %lu stayed at the clear, %lu are integer targets and not checked, "
	       "%lu are WRONG\n", 4 * PIC_TILES * PIC_TILES, rt_colour, rt_clear, rt_skipped,
	       rt_wrong);
	fflush(stdout);
}

/*
 * One named experiment: N attachments of W by H, a pipeline for clearFragment01 -- the
 * shader the mosaic has already drawn 64 times on this GPU -- an encoder, a draw and a
 * readback, every one of them saying its own name first.
 *
 * Four of these run in a ladder before Apple's blit, and between them they name the axis
 * the stall is on. The second boot (logs/101536-run.log's successor) stopped between
 * -commandBuffer and -renderCommandEncoderWithDescriptor: on four 1080x648 attachments,
 * and reading the driver afterwards found NOTHING in either call proportional to the
 * attachment's area: -commandBuffer allocates an object and an NSMutableArray, and the
 * encoder's init reads loadAction, storeAction, resolveTexture and clearColor per
 * attachment and nothing else. So the obvious story -- "a megapixel attachment is too
 * big" -- is not supported by the code, and the ladder is built to find out what is true
 * instead of assuming it:
 *
 *   again-small   four 135x138, the mosaic's own shape, once more. If THIS stalls, size
 *                 has nothing to do with it: what changed is that it is the 65th command
 *                 buffer of the process, with 64 encoders, 64 pipelines and 64 command
 *                 buffers behind it that nothing has freed (this file has no autorelease
 *                 pool, deliberately: the mosaic's objects are small and the leak is
 *                 bounded).
 *   one-big       ONE 1080x648. Size with no count.
 *   four-medium   four 540x324 -- 2.8 MB of attachment against four-big's 11.2 MB, and
 *                 the same count. Count with a quarter of the area.
 *   four-big      four 1080x648, the one that stalled.
 *
 * The first rung that does not finish is the answer, and the heartbeat thread says
 * whether it is stuck or merely slow while it is happening.
 */
static bool
pic_probe(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	  unsigned n, unsigned w, unsigned h, bool full, const char *name)
{
	/* the colour, three different levels of pic_ramp so that a channel in the wrong
	 * place is seen, each an exact byte so the count needs no tolerance */
	const unsigned char want[4] = { 109, 182, 73, 255 };   /* b, g, r, a */
	id<MTLTexture> t[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *rp = nil;
	id<MTLFunction> fn = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLBuffer> u = nil;
	unsigned char *back = NULL;
	bool ok = false;
	char line[192];

	snprintf(line, sizeof(line), "probe %s: %u attachment%s of %ux%u (%lu KB of "
		 "attachment)", name, n, n == 1 ? "" : "s", w, h,
		 (unsigned long)((size_t)n * w * h * 4 / 1024));
	pic_step(line);
	if (!pic_targets(dev, t, n, w, h, &rp)) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the attachments are made", name);
	pic_step(line);

	fn = [library newFunctionWithName:@"clearFragment01"];
	if (fn == nil) {
		printf("drvtest: picture: probe %s: no clearFragment01\n", name);
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the pipeline", name);
	pic_step(line);
	ps = pic_pipeline(dev, nil, fn, n, name);
	if (ps == nil) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the constant buffer", name);
	pic_step(line);
	u = [dev newBufferWithLength:256 options:0];
	if (u == nil || [u contents] == NULL) {
		printf("drvtest: picture: probe %s: no constant buffer\n", name);
		goto done;
	}
	{
		float *f = [u contents];

		memset(f, 0, 256);
		f[0] = want[2] / 255.0f;
		f[1] = want[1] / 255.0f;
		f[2] = want[0] / 255.0f;
		f[3] = 1.0f;
	}
	snprintf(line, sizeof(line), "probe %s, before it asks for a command buffer", name);
	pic_queue_state(queue, line);
	snprintf(line, sizeof(line), "probe %s", name);
	if (!pic_draw(queue, rp, ps, NULL, 0, u, 16, nil, nil, line, true)) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: drew; reading ONE pixel back", name);
	pic_step(line);
	{
		unsigned char one[4] = { 0, 0, 0, 0 };

		[t[0] getBytes:one
		    bytesPerRow:4
		     fromRegion:MTLRegionMake2D(0, 0, 1, 1)
		    mipmapLevel:0];
		printf("drvtest: picture: probe %s: pixel (0,0) is r g b %u %u %u and must be "
		       "%u %u %u\n", name, one[2], one[1], one[0], want[2], want[1], want[0]);
		fflush(stdout);
		ok = one[0] == want[0] && one[1] == want[1] && one[2] == want[2];
	}
	if (!full) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the whole %ux%u back in one getBytes:", name,
		 w, h);
	pic_step(line);
	back = malloc((size_t)w * h * 4);
	if (back == NULL) {
		printf("drvtest: picture: probe %s: no memory to read %ux%u into\n", name, w, h);
		goto done;
	}
	memset(back, 0, (size_t)w * h * 4);
	[t[0] getBytes:back
	    bytesPerRow:w * 4
	     fromRegion:MTLRegionMake2D(0, 0, w, h)
	    mipmapLevel:0];
	{
		unsigned long bad = 0;

		for (size_t q = 0; q < (size_t)w * h; q++) {
			if (back[q * 4] != want[0] || back[q * 4 + 1] != want[1] ||
			    back[q * 4 + 2] != want[2]) {
				bad++;
			}
		}
		printf("drvtest: picture: probe %s: %lu pixels of %u are not that colour, so a "
		       "%ux%u render target draws and reads back %s\n", name, bad, w * h, w, h,
		       bad == 0 ? "WHOLE" : "WRONG");
		fflush(stdout);
		ok = ok && bad == 0;
	}
done:
	free(back);
	[u release];
	[ps release];
	[fn release];
	pic_release_targets(t, n, rp);
	snprintf(line, sizeof(line), "probe %s: finished, and its resources are freed", name);
	pic_step(line);
	return ok;
}

/*
 * The ladder, in the order that makes the first failure mean the most.
 *
 * Each rung frees everything it made before the next begins, so a rung is about its own
 * shape and not about what the rungs before it are still holding.
 */
static void
pic_ladder(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	pic_step("THE LADDER: four experiments that separate count from size. The first one "
		 "that does not finish is the answer");
	/*
	 * And the verdict on the bug the picture found, named so that a later change cannot
	 * quietly lose it: the mosaic has just made 64 command buffers and this test keeps
	 * every one of them alive ON PURPOSE -- there is no autorelease pool round its loop,
	 * and there must not be one. With a pool, each buffer would be deallocated at the
	 * end of its iteration and the slot would come back from -dealloc, so this would
	 * pass whether or not -commit returns it, which is the whole failure the driver had.
	 * Sixty-four live command buffers holding no slot is the measurement.
	 */
	unsigned long held = pic_queue_state(queue, "after the mosaic's 64 command buffers");

	if (held != ~0UL) {
		printf("drvtest: picture: the mosaic made 64 command buffers and %lu of them "
		       "still hold a queue slot: %s\n", held,
		       held == 0 ? "the slot IS given back at -commit, so the next command "
				   "buffer will not block" :
				   "the slot is NOT given back, and the next command buffer "
				   "will block for ever (this is the driver bug the picture "
				   "found)");
		fflush(stdout);
	}
	pic_probe(dev, library, queue, 4, PIC_TILE_W, PIC_TILE_H, false, "again-small");
	pic_probe(dev, library, queue, 1, PANEL_W, PIC_BAND_H, true, "one-big");
	pic_probe(dev, library, queue, 4, PANEL_W / 2, PIC_BAND_H / 2, false, "four-medium");
	pic_probe(dev, library, queue, 4, PANEL_W, PIC_BAND_H, true, "four-big");
	pic_queue_state(queue, "after the ladder");
	pic_step("THE LADDER: all four rungs finished");
}

/*
 * The two bands: Apple's own textured blit over the whole attachment, linear and nearest.
 *
 * This is where the first boot stopped (logs/101536-run.log): the mosaic's summary was the
 * last line of the process, and after it the kernel's idle heartbeat and nothing else for
 * four minutes. The GPU was not wedged -- the heartbeat went on reading sensors and the run
 * ended cleanly -- and the driver refuses a draw the GPU does not finish with a sentence
 * and a status (that is how Apple's two [[depth]] shaders are known to hang it,
 * logs/094810-run.log: "status 5, the GPU did not finish the draw"). So a silent stop is
 * NOT a hung dispatch, and the suspects are all on this side of the fence.
 *
 * Everything here is therefore built to name the step rather than to survive it:
 *
 *   - every step says its own name BEFORE it starts, flushed. On the way this stage takes,
 *     nothing printed unless something failed, so the stall had the whole stage to hide in;
 *   - between the mosaic and the textured pipeline there is now a PROBE, which is the
 *     cheapest way to cut the suspect space in half in one boot. It draws
 *     clearFragment01 -- the shader the mosaic has already drawn 64 times, with the
 *     generated vertex stage, no texture, no sampler and no vertex buffer -- into the
 *     band-sized attachments, and reads first one pixel and then the whole 1080x648 back.
 *     Three different things can stall in there and each has its own line, so the boot
 *     says which: making four 1080x648 BGRA8Unorm render targets, drawing into one, and
 *     reading one back. If the probe gets through, none of those is the cause and what is
 *     left is Apple's vertex stage, the sampled texture and the sampler;
 *   - nothing here stores to [[depth]], which is the one thing known to hang this GPU.
 *
 * And the mosaic is already on the panel before any of it runs, so a stall now costs the
 * bands and not the screenshot.
 */
static void
pic_bands(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	id<MTLTexture> bt[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *brp = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLTexture> grad = nil;
	id<MTLBuffer> vpos = nil, vuv = nil;
	id<MTLFunction> vfn = nil, ffn = nil;
	MTLTextureDescriptor *gd = nil;
	unsigned char *band = NULL;
	static float texels[PIC_TEX * PIC_TEX * 4];
	static const float quad[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

	pic_step("the memory for one band on this side");
	band = malloc((size_t)PANEL_W * PIC_BAND_H * 4);
	if (band == NULL) {
		printf("drvtest: picture: no memory for a band of %u by %u\n", PANEL_W,
		       PIC_BAND_H);
		goto out;
	}

	pic_step("four 1080x648 BGRA8Unorm render targets for the two bands (the ladder has "
		 "just made and freed the same four)");
	if (!pic_targets(dev, bt, 4, PANEL_W, PIC_BAND_H, &brp)) {
		goto out;
	}
	pic_step("four attachments made");

	/* ---- Apple's own blit ------------------------------------------------------- */
	pic_step("Apple's two functions, texturedVertex2D and texturedFragment2DFloat");
	vfn = [library newFunctionWithName:@"texturedVertex2D"];
	ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];
	printf("drvtest: picture: texturedVertex2D %s, texturedFragment2DFloat %s\n",
	       vfn != nil ? "is here" : "is MISSING", ffn != nil ? "is here" : "is MISSING");
	fflush(stdout);
	if (vfn == nil || ffn == nil) {
		goto out;
	}
	pic_step("the textured pipeline, with Apple's vertex stage");
	ps = pic_pipeline(dev, vfn, ffn, 4, "texturedVertex2D + texturedFragment2DFloat");
	if (ps == nil) {
		goto out;
	}
	pic_step("the textured pipeline is made");

	/* The texture Apple's blit samples: 8x8, RGBA32Float because the shader declares
	 * texture2d<float, sample>, every channel a level of pic_ramp so that what the
	 * sampler returns is exact in eight bits. */
	pic_step("the 8x8 RGBA32Float texture");
	gd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
								width:PIC_TEX
							       height:PIC_TEX
							    mipmapped:NO];
	[gd setUsage:MTLTextureUsageShaderRead];
	grad = [dev newTextureWithDescriptor:gd];
	if (grad == nil) {
		printf("drvtest: picture: the 8x8 texture could not be made\n");
		goto out;
	}
	for (unsigned y = 0; y < PIC_TEX; y++) {
		for (unsigned x = 0; x < PIC_TEX; x++) {
			unsigned char c[4];
			float *t = texels + (y * PIC_TEX + x) * 4;

			pic_texel_colour(x, y, c);
			t[0] = c[2] / 255.0f;
			t[1] = c[1] / 255.0f;
			t[2] = c[0] / 255.0f;
			t[3] = 1.0f;
		}
	}
	/* one call for the whole picture and not one per row, for draw_path's reason: a
	 * seed written at a non-zero region origin is a second thing being measured */
	pic_step("seeding that texture in one replaceRegion:");
	[grad replaceRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
		mipmapLevel:0
		  withBytes:texels
		bytesPerRow:PIC_TEX * 4 * sizeof(float)];
	/* and read it back, for draw_path's other reason: a draw that samples a texture
	 * nobody has looked at cannot tell a wrong sample from a wrong seed */
	pic_step("reading that texture back");
	{
		static float back[PIC_TEX * PIC_TEX * 4];

		memset(back, 0, sizeof(back));
		[grad getBytes:back
		   bytesPerRow:PIC_TEX * 4 * sizeof(float)
		    fromRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
		   mipmapLevel:0];
		printf("drvtest: picture: texel (0,0) reads back r g b %g %g %g and was seeded "
		       "%g %g %g\n", (double)back[0], (double)back[1], (double)back[2],
		       (double)texels[0], (double)texels[1], (double)texels[2]);
		fflush(stdout);
	}

	/* What texturedVertex2D reads: buffer 0 one float4 of clip-space position per
	 * vertex, buffer 1 one float2 of texture coordinate. The corners and the
	 * coordinates are air_metal_test's own, so this draw and the host's are one quad.
	 *
	 * Which way up it comes out is the viewport's, not this file's, and it has just
	 * changed: the library now puts clip y = +1 in memory row 0, which is Metal's rule
	 * (docs/metal.md 10.28, landed once 10.32 had fixed the flat varying that was
	 * hiding it). The corner at clip (-1, +1) therefore lands in row 0 and carries the
	 * coordinate v = 1, so the TOP of the band is texel row PIC_TEX-1 and the band is
	 * the texture mirrored vertically.
	 *
	 * This table is deliberately NOT flipped to cancel that. The four corners and the
	 * four coordinates are air_metal_test's own, so that this draw and the host's are
	 * one quad; and compensating in the coordinates would hide the very thing the new
	 * sign made visible, which is how a flat varying passed for a vertical flip for
	 * three boots. What flips is the EXPECTATION, in pic_count_bands.
	 *
	 * The picture says it for itself: texel (0,0) is green and texel (0,7) is blue, so
	 * the band must now come out BLUE at the top and GREEN at the bottom, and two named
	 * pixels below say it in numbers rather than in a count.
	 */
	pic_queue_state(queue, "before the two band draws");
	pic_step("the two vertex buffers Apple's vertex stage reads");
	vpos = [dev newBufferWithLength:64 options:0];
	vuv = [dev newBufferWithLength:64 options:0];
	if (vpos == nil || vuv == nil || [vpos contents] == NULL || [vuv contents] == NULL) {
		printf("drvtest: picture: no vertex buffers\n");
		goto out;
	}
	memcpy([vpos contents], quad, sizeof(quad));
	memcpy([vuv contents], uv, sizeof(uv));

	for (int pass = 0; pass < 2; pass++) {
		const char *what = pass == 0 ? "the linear band" : "the nearest band";
		id<MTLBuffer> vbuf[2] = { vpos, vuv };
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		pic_step(pass == 0 ? "the linear sampler" : "the nearest sampler");
		[sd setMinFilter:pass == 0 ? MTLSamplerMinMagFilterLinear
					   : MTLSamplerMinMagFilterNearest];
		[sd setMagFilter:pass == 0 ? MTLSamplerMinMagFilterLinear
					   : MTLSamplerMinMagFilterNearest];
		[sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
		[sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
		id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

		[sd release];
		if (samp == nil) {
			printf("drvtest: picture: no %s\n", what);
			continue;
		}
		pic_step(pass == 0 ? "drawing the linear band" : "drawing the nearest band");
		if (pic_draw(queue, brp, ps, vbuf, 2, nil, 0, grad, samp, what, true)) {
			pic_step(pass == 0 ? "the linear band drew; reading it back"
					   : "the nearest band drew; reading it back");
			[bt[0] getBytes:band
			    bytesPerRow:PANEL_W * 4
			     fromRegion:MTLRegionMake2D(0, 0, PANEL_W, PIC_BAND_H)
			    mipmapLevel:0];
			pic_blit(band, PANEL_W, PIC_BAND_H, 0,
				 pass == 0 ? PIC_LINEAR_Y : PIC_NEAREST_Y);
			/* on the screen at once, before the other band is even begun */
			pic_push(what);
		}
		[samp release];
	}
out:
	free(band);
	[vpos release];
	[vuv release];
	[grad release];
	[ps release];
	pic_release_targets(bt, 4, brp);
	[vfn release];
	[ffn release];
	pic_step("done");
}

/* What the two bands must hold, and a handful of pixels named one by one. */
static void
pic_count_bands(void)
{
	unsigned long bad_nearest = 0, bad_centres = 0;

	/*
	 * THE TEXEL ROW IS COUNTED FROM THE BOTTOM, and that is Metal's orientation rather
	 * than a workaround: the viewport puts clip y = +1 in memory row 0 (10.28), the
	 * corner that lands there carries v = 1, so band row r shows texel row
	 * PIC_TEX-1 - r/PIC_SY where it used to show r/PIC_SY. The quad and its
	 * coordinates are unchanged on purpose -- see the note in pic_bands.
	 *
	 * The x axis is untouched. Nothing in the viewport's y sign is a horizontal
	 * statement, which is also why the linear/nearest contrast at column 134 below
	 * reads exactly as it did before the sign went in.
	 */
	for (unsigned q = 0; q < PIC_BAND_H; q++) {
		for (unsigned p = 0; p < PANEL_W; p++) {
			unsigned char want[4];
			const unsigned char *g = pic_panel +
			    ((size_t)(PIC_NEAREST_Y + q) * PANEL_W + p) * 4;

			pic_texel_colour(p / PIC_SX, PIC_TEX - 1 - q / PIC_SY, want);
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2]) {
				bad_nearest++;
			}
		}
	}
	for (unsigned j = 0; j < PIC_TEX; j++) {
		for (unsigned i = 0; i < PIC_TEX; i++) {
			unsigned px = PIC_SX * i + (PIC_SX - 1) / 2;
			/* texel row j is now at band row PIC_TEX-1-j */
			unsigned py = PIC_SY * (PIC_TEX - 1 - j) + (PIC_SY - 1) / 2;
			unsigned char want[4];
			const unsigned char *g = pic_panel +
			    ((size_t)(PIC_LINEAR_Y + py) * PANEL_W + px) * 4;

			pic_texel_colour(i, j, want);
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2]) {
				if (bad_centres < 6) {
					printf("drvtest: picture: linear texel centre (%u,%u) "
					       "at panel (%u,%u) is r g b %u %u %u and its "
					       "texel is %u %u %u\n", i, j, px,
					       PIC_LINEAR_Y + py, g[2], g[1], g[0], want[2],
					       want[1], want[0]);
				}
				bad_centres++;
			}
		}
	}
	printf("drvtest: picture: nearest band: %lu pixels of %u are not the texel the "
	       "sampler must have returned\n", bad_nearest, PANEL_W * PIC_BAND_H);
	printf("drvtest: picture: linear band: %lu of %u texel centres are not their texel\n",
	       bad_centres, PIC_TEX * PIC_TEX);

	/* The one place the two bands must differ, and the whole of what says the sampler
	 * state reached the hardware: halfway between texel 0 and texel 1 along a row of
	 * texel centres. Nearest can only answer one of the two; linear must answer
	 * between them, and softpipe through this same library answers 18 there.
	 *
	 * Unmoved by the viewport's y sign, and that is the point: this reads the RED
	 * channel, which pic_texel_colour makes a function of x alone, so it is a horizontal
	 * statement and a vertical mirror cannot change it. If this pair ever moves with a
	 * y sign, the cause is not the y sign. */
	unsigned x = PIC_SX - 1, y = (PIC_SY - 1) / 2;
	const unsigned char *l = pic_panel + ((size_t)(PIC_LINEAR_Y + y) * PANEL_W + x) * 4;
	const unsigned char *n = pic_panel + ((size_t)(PIC_NEAREST_Y + y) * PANEL_W + x) * 4;

	printf("drvtest: picture: at column %u, row %u of each band: linear red %u, nearest "
	       "red %u (texel 0 is %u, texel 1 is %u; softpipe gives 18 and 0)\n", x, y,
	       l[2], n[2], pic_ramp[0], pic_ramp[1]);
	printf("drvtest: picture: the sampler %s\n",
	       l[2] > pic_ramp[0] && l[2] < pic_ramp[1] && n[2] == pic_ramp[0] ?
	       "FILTERS: the linear band is between two texels where the nearest one is on "
	       "one of them" :
	       "did not do that, and the two bands are not telling two things apart");

	/* And the orientation itself, as two named pixels rather than as a count: "0 of
	 * 699840" says the band matched its expectation, whatever that expectation was, and
	 * cannot say which way up anything is. With Metal's rule the top of the band is
	 * texel row PIC_TEX-1 and the bottom is texel row 0, so it is BLUE at the top and
	 * GREEN at the bottom; under the old sign it was the other way up. */
	{
		unsigned char top[4], bottom[4];
		const unsigned char *a = pic_panel +
		    ((size_t)(PIC_LINEAR_Y + (PIC_SY - 1) / 2) * PANEL_W +
		     (PIC_SX - 1) / 2) * 4;
		const unsigned char *b = pic_panel +
		    ((size_t)(PIC_LINEAR_Y + PIC_SY * (PIC_TEX - 1) + (PIC_SY - 1) / 2) *
		     PANEL_W + (PIC_SX - 1) / 2) * 4;

		pic_texel_colour(0, PIC_TEX - 1, top);
		pic_texel_colour(0, 0, bottom);
		printf("drvtest: picture: the band's top left is r g b %u %u %u (texel (0,%u), "
		       "must be %u %u %u) and its bottom left is %u %u %u (texel (0,0), must "
		       "be %u %u %u): clip y = +1 is memory row 0\n", a[2], a[1], a[0],
		       PIC_TEX - 1, top[2], top[1], top[0], b[2], b[1], b[0], bottom[2],
		       bottom[1], bottom[0]);
	}

	/* and the four corner squares of the mosaic, which has no filtering in it at all:
	 * what a person can check against the picture by eye */
	static const unsigned corner[4][2] = { { 0, 0 }, { 7, 0 }, { 0, 7 }, { 7, 7 } };

	for (int c = 0; c < 4; c++) {
		unsigned i = corner[c][0], j = corner[c][1];
		unsigned px = i * PIC_TILE_W + PIC_TILE_W / 2;
		unsigned py = j * PIC_TILE_H + PIC_TILE_H / 2;
		unsigned char want[4];
		const unsigned char *g = pic_panel + ((size_t)py * PANEL_W + px) * 4;

		pic_mosaic_colour(i, j, want);
		printf("drvtest: picture: clearFragment%02X at panel (%u,%u) is r g b %u %u %u "
		       "and must be %u %u %u: %s\n", 4 * (j * PIC_TILES + i) + 1, px, py,
		       g[2], g[1], g[0], want[2], want[1], want[0],
		       g[0] == want[0] && g[1] == want[1] && g[2] == want[2] ? "the same"
									    : "DIFFERENT");
	}
	fflush(stdout);
}

/*
 * And then keep repainting it, for as long as the caller asked.
 *
 * The repainting is not what puts the picture there -- pic_push already did that after
 * every stage, and the copy stays in the boot framebuffer by itself. It is for the boot
 * where a daemon of Apple's IS running and draws over the panel between one screenshot
 * and the next: five times a second beats a window server that swaps occasionally. With
 * JOBS="" nothing of this is needed and the first push would have been enough.
 */
static void
pic_show(int seconds)
{
	if (pic_connection == 0) {
		return;
	}
	/* "--picture 0" is still a picture: pic_push has already painted it once. */
	if (seconds < 1) {
		seconds = 1;
	}
	printf("drvtest: picture: repainting for %d seconds; the screenshot can be taken at "
	       "any time now and more than once\n", seconds);
	fflush(stdout);
	for (int i = 0; i < seconds * 5; i++) {
		struct pic_show_request show = {
			(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
		};

		IOConnectCallStructMethod(pic_connection, 0x7f, &show, sizeof(show), NULL,
					  NULL);
		usleep(200000);
	}
	printf("drvtest: picture: done repainting after %d seconds\n", seconds);
	fflush(stdout);
}

static void
picture_path(id<MTLDevice> dev, const char *metallib, int seconds)
{
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: picture: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: picture: newLibraryWithData: gave nil\n");
		return;
	}
	pic_panel = calloc(1, (size_t)PANEL_W * PANEL_H * 4);
	if (pic_panel == NULL) {
		printf("drvtest: picture: no memory for %ux%u pixels\n", PANEL_W, PANEL_H);
		return;
	}
	printf("drvtest: picture: %s; the panel is %ux%u, the mosaic %ux%u tiles of %ux%u, "
	       "each band %ux%u\n", metallib, PANEL_W, PANEL_H, PIC_TILES, PIC_TILES,
	       PIC_TILE_W, PIC_TILE_H, PANEL_W, PIC_BAND_H);
	fflush(stdout);

	id<MTLCommandQueue> queue = [dev newCommandQueue];

	/* The panel BEFORE anything is drawn, so that a stage which finishes can be shown
	 * the moment it finishes. The first boot of this mode drew the whole mosaic
	 * correctly and then stopped on its way into the bands, and because the only push
	 * was at the very end the screenshot caught the loader's splash: a verified picture
	 * existed in this process's memory and never reached the screen. */
	pic_open_panel();

	pic_mosaic(dev, library, queue);
	pic_push("the mosaic -- 64 of Apple's fragment shaders, drawn on the Adreno");
	printf("drvtest: picture: the mosaic is done; now the ladder, then the two bands\n");
	fflush(stdout);
	/* From here on a second thread says, every so often, that this process is alive and
	 * which step it is in -- so the next silence means something definite. */
	pic_watch();
	pic_ladder(dev, library, queue);
	pic_bands(dev, library, queue);
	pic_alive = 0;
	pic_count_bands();
	pic_show(seconds);
}

/*
 * ==========================================================================================
 * --time: WHAT THIS DRIVER COSTS, ON THE PHONE, IN MICROSECONDS
 * ==========================================================================================
 *
 * Everything above this line answers "is it right". Nothing above it has ever answered "how
 * long" -- not a dispatch, not a draw, not a frame -- and the two questions fail differently:
 * a wrong draw says so in a pixel, a slow draw says nothing at all. The acceptance test for
 * this driver is a game, and a game is bought with time, so this mode measures the four
 * things a frame is actually made of and prints each with what would make it wrong.
 *
 * WHERE THE TIME GOES HERE, AND WHY IT IS NOT WHERE AN APPLICATION EXPECTS IT.
 *
 * -[RedwoodMetalCommandBuffer commit] does the work (docs/metal.md 10.13). The encoder only
 * RECORDS; commit walks the recorded list calling rwa_render_draw / rwa_dispatch, and each of
 * those flushes the Gallium context and WAITS ON ITS OWN FENCE before returning
 * (tools/mesa/air/air_metal.c, finish_or_say_why with required = true). Three consequences,
 * and the mode is built around them:
 *
 *   1. encoding is nearly free and measures nothing an application cares about;
 *   2. -commit is the whole frame -- our translation, the submission, the GPU's execution and
 *      the wait for it -- one draw after another, with NO overlap between processor and GPU.
 *      So a frame's cost is the SUM of its draws and not the maximum of two pipelines;
 *   3. -waitUntilCompleted has nothing left to wait for and must measure ~0. That is not a
 *      hole to paper over: it is the check that the split is where this comment says it is,
 *      which is why it is measured and printed instead of assumed. A -waitUntilCompleted that
 *      is NOT ~0 means this comment is out of date and every other number here is mis-split.
 *
 * ONE DRAW IS ONE RENDER PASS IS ONE SUBMISSION. rwa_render_draw creates the surfaces, sets
 * the framebuffer, clears, draws, flushes and fences, then tears the framebuffer down again
 * (air_metal.c around 4124-4346). So eight Metal draws into one encoder are eight kernel
 * submissions and eight passes over the attachment, not one -- which is exactly why this mode
 * times one draw per command buffer AND eight, and prints the marginal cost of a draw
 * separately from the cost of a command buffer.
 *
 * WHAT IS MEASURED, AND WHY THESE AND NOT OTHERS
 *
 *   the clock itself      every number below is this clock's; printed first so that nothing
 *                         within a few times its floor is read as a measurement.
 *   a pipeline state      an AIR -> NIR -> ir3 compile inside -newRenderPipelineStateWith-
 *                         Descriptor:, with NO cache anywhere (air_metal.c: "One compiled
 *                         state, not a cache"). Measured cold (the process's first, which also
 *                         pays for whatever the compiler sets up once) and warm, the same
 *                         function again and then different functions -- because if the same
 *                         function is much cheaper the second time, something IS caching and
 *                         the warm number is not a compile.
 *   a draw                at three sizes: 4x4 (every earlier number in this file), 1080x648
 *                         (the picture's band, so this is comparable with logs/140340) and
 *                         1080x2400, the panel. A 4x4 quad says nothing about a frame: that is
 *                         the whole reason the big sizes are here.
 *   with and without the clear   the same attachments drawn through two render passes that
 *                         differ in ONE field, loadAction. Every draw in a pass whose
 *                         loadAction is Clear re-clears the attachment in this driver
 *                         (redwoodmetal.m copies _clears into every recorded draw), and an
 *                         application clears once a frame, so the two have to be told apart
 *                         before a per-draw cost means anything.
 *   a texture upload      -replaceRegion: of the whole attachment-sized image, in MB/s: what
 *                         streaming a texture costs.
 *   a readback            -getBytes: of the same. No game does this; it is here because every
 *                         verifying mode in this file does it, so its cost has to be
 *                         attributable and NOT mistaken for the draw's.
 *   command buffers/s     how many the queue sustains with no work in them at all: the floor
 *                         under any frame rate, and the one number that would catch a
 *                         regression of the 64-slot semaphore bug (10.33) as a slowdown
 *                         rather than as a hang.
 *   the present           -getBytes: of the finished attachment plus the kernel's kRedwoodShow
 *                         row copy. NOT Metal's, and said so where it is printed -- but a
 *                         frame that is never shown is not a frame, and this is the only way
 *                         this port has to show one, so a frame rate that leaves it out is a
 *                         frame rate for a game nobody can see.
 *
 * HOW OUR COST IS SEPARATED FROM THE GPU's, AND WHERE IT CANNOT BE
 *
 * Three ways, in order of how much they can be trusted. The first did not exist when this mode
 * was written and was added because the other two were not enough (docs/metal.md 10.43):
 *
 *   (0) ASK THE KERNEL. Request kRedwoodGPUWorked (0x7d) on RedwoodGPU answers
 *       redwood_gpu_worked()'s three numbers -- the microseconds the 3D core worked, on how
 *       many submissions, and the histogram -- taken off CP_ALWAYS_ON_COUNTER. Reading resets
 *       them, so each case is bracketed by two reads and gets the core's own time for its own
 *       draws. An older kernel answers kIOReturnUnsupported and the mode says in words that
 *       the GPU's side is not measured, rather than printing a zero.
 *
 * and the two that were there first, which still run because they need nothing of the kernel:
 *
 *   (a) BY SUBTRACTION, in this process, no boot argument needed. The 4x4 case and the
 *       1080x2400 case emit the same commands, bind the same state and make the same number
 *       of submissions; what differs is the number of pixels. So the 4x4 per-draw median is
 *       our cost plus the round trip, and the increase at the bigger sizes is the GPU's --
 *       its fill and the bandwidth of one pass over the attachment. This attributes nothing
 *       it cannot see: it says "fixed" and "proportional to area", not "ours" and "theirs".
 *   (b) BY THE KERNEL'S OWN COUNTER, which needs the boot argument gpu_log=1 and a log to
 *       read afterwards. bsd/dev/redwood_adreno.c times every submission off the GPU's
 *       always-on counter and writes "tl: q <fence> took <us>" per submission; RedwoodGPU.cpp
 *       prints, every five seconds in which something was submitted, "the GPU worked N us on
 *       each of M" with a histogram. Those lines land in the same run log as these, so the
 *       reader subtracts. This mode cannot print them itself and does not pretend to: what it
 *       prints instead is HOW MANY submissions each case made, so the kernel's M can be
 *       matched to a case instead of guessed at.
 *
 * And one number that cannot be split at all, said so rather than attributed: the cold
 * pipeline. It contains the compiler's one-time setup and one compile and nothing separates
 * them from inside the process; what can be said is that the second compile no longer pays
 * whatever the first did, and the difference between the two is printed.
 */

enum {
	TM_MAX_SAMPLES = 256,   /* kept per measurement; more are counted and not kept */
	TM_BATCH = 8,           /* draws in the batched command buffer */
};

struct tm_run {
	unsigned n;             /* taken, which may exceed what was kept */
	uint64_t ns[TM_MAX_SAMPLES];
};

static unsigned tm_repeats = 32;
static uint64_t tm_main_entered;        /* the first statement of main() */
static uint64_t tm_mode_entered;        /* the first statement of time_path() */
static uint64_t tm_first_pixel;         /* the first draw of this mode that the GPU finished */
static uint64_t tm_cold_pipeline_ns;    /* the process's first compile, which happens once */
/*
 * The four medians the frame-rate arithmetic at the end is made of, kept here because they are
 * measured in four different groups and the arithmetic has to be done where all four are
 * known. Each is zero until its group has run, and the arithmetic says so rather than
 * substituting a guess.
 */
static uint64_t tm_small_marginal_ns;   /* a further 4x4 draw in the same command buffer */
static uint64_t tm_panel_marginal_ns;   /* a further 1080x2400 draw, trivial fragment shader */
static uint64_t tm_panel_tex_ns;        /* ... and with Apple's textured blit */
static uint64_t tm_buffer_ns;           /* the command buffer itself, once a frame */
static uint64_t tm_warm_pipeline_ns;    /* one compile, warm, a shader not seen before */
static uint64_t tm_present_ns;          /* -getBytes: the panel plus the kernel's copy */
static unsigned long tm_submissions;    /* draws this mode asked the GPU for, for the kernel's count */

/*
 * CLOCK_MONOTONIC and not pic_now_ms's timebase arithmetic, for one reason: pic_now_ms
 * divides down to milliseconds and almost everything measured here is smaller than its
 * resolution -- logs/140340-run.log says "the step before took 0 ms" for a whole 1080x648
 * textured draw, which is the exact shape of a measurement that is not one.
 */
static uint64_t
tm_now_ns(void)
{
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
		return 0;
	}
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void
tm_add(struct tm_run *r, uint64_t ns)
{
	if (r->n < TM_MAX_SAMPLES) {
		r->ns[r->n] = ns;
	}
	r->n++;
}

/*
 * One measurement, said in full.
 *
 * The MEDIAN leads, because one slow sample -- a page fault, a migration to another core, the
 * kernel's own five-second report landing in the middle of a draw -- moves a mean and cannot
 * move the median of thirty-two. The MAX is printed beside it so an outlier is seen rather
 * than smoothed away, and the FIRST sample is printed on its own because in this mode the
 * first of anything is a different measurement from the rest: the first pipeline pays for the
 * compiler, the first draw with a pipeline pays for ir3's program link (ir3_cache_lookup
 * happens at draw time, not at pipeline creation), and the first touch of a buffer pays for
 * its pages.
 */
static void
tm_say(const char *what, const struct tm_run *r, const char *wrong)
{
	uint64_t s[TM_MAX_SAMPLES];
	unsigned n = r->n < TM_MAX_SAMPLES ? r->n : TM_MAX_SAMPLES;
	uint64_t total = 0;

	if (n == 0) {
		printf("drvtest: time: %-44s NOT MEASURED (no sample was taken)\n", what);
		fflush(stdout);
		return;
	}
	memcpy(s, r->ns, (size_t)n * sizeof(s[0]));
	for (unsigned i = 1; i < n; i++) {      /* insertion sort; n is at most 256 */
		uint64_t v = s[i];
		unsigned j = i;

		while (j > 0 && s[j - 1] > v) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
	for (unsigned i = 0; i < n; i++) {
		total += s[i];
	}
	printf("drvtest: time: %-44s n %3u  first %9.1f us  median %9.1f  min %9.1f  "
	       "max %9.1f  mean %9.1f\n", what, n, r->ns[0] / 1000.0, s[n / 2] / 1000.0,
	       s[0] / 1000.0, s[n - 1] / 1000.0, (double)total / 1000.0 / n);
	if (wrong != NULL) {
		printf("drvtest: time:    ^ wrong if: %s\n", wrong);
	}
	fflush(stdout);
}

/* The median of what was kept, for the arithmetic this mode does on its own numbers. */
static uint64_t
tm_median(const struct tm_run *r)
{
	uint64_t s[TM_MAX_SAMPLES];
	unsigned n = r->n < TM_MAX_SAMPLES ? r->n : TM_MAX_SAMPLES;

	if (n == 0) {
		return 0;
	}
	memcpy(s, r->ns, (size_t)n * sizeof(s[0]));
	for (unsigned i = 1; i < n; i++) {
		uint64_t v = s[i];
		unsigned j = i;

		while (j > 0 && s[j - 1] > v) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
	return s[n / 2];
}

/* The same, plus what it is in megabytes a second -- which is the only form in which a copy
 * of ten megabytes and a copy of two hundred bytes can be compared. */
static void
tm_say_rate(const char *what, const struct tm_run *r, size_t bytes, const char *wrong)
{
	uint64_t median = tm_median(r);

	tm_say(what, r, wrong);
	if (median != 0) {
		printf("drvtest: time:    ^ %zu bytes at the median: %.1f MB/s\n", bytes,
		       (double)bytes * 1000.0 / (double)median);
		fflush(stdout);
	}
}

/*
 * The floor under every number in this mode.
 *
 * Two back-to-back reads of the clock, a thousand times: the smallest non-zero difference is
 * the clock's own granularity, and the median difference is what one reading costs. A
 * measurement of a few times either is noise wearing a number's clothes, and this is printed
 * FIRST so that no one has to take the rest on trust.
 */
static void
tm_clock_floor(void)
{
	struct tm_run gap = { 0 };
	uint64_t smallest = ~0ull;

	for (unsigned i = 0; i < 1000; i++) {
		uint64_t a = tm_now_ns(), b = tm_now_ns();

		if (b > a) {
			tm_add(&gap, b - a);
			if (b - a < smallest) {
				smallest = b - a;
			}
		}
	}
	if (gap.n == 0) {
		printf("drvtest: time: the clock never moved between two reads: either "
		       "CLOCK_MONOTONIC is not implemented here or its resolution is coarser "
		       "than this loop, and NOTHING below can be believed\n");
		fflush(stdout);
		return;
	}
	/*
	 * Said this way after the first run said it the wrong way. The median of the NON-ZERO
	 * gaps is one quantum by construction -- every non-zero gap is one tick -- so printing
	 * it as "one reading costs that much" was a wrong sentence beside a right number
	 * (logs/225409-run.log: "the smallest step is 1000 ns and one reading costs about
	 * 1000 ns"). What the fraction of pairs that moved actually gives is the cost of a
	 * reading: if 44 of 1000 pairs crossed a 1000 ns boundary, a pair takes about
	 * 0.044 x 1000 ns.
	 */
	printf("drvtest: time: the clock: it ticks in steps of %llu ns, and %u of 1000 pairs of "
	       "back-to-back reads fell either side of a tick -- so one reading costs about "
	       "%llu ns and every single number below carries +/-%llu ns of quantisation, which "
	       "a median over %u repeats averages away but a lone sample does not\n",
	       (unsigned long long)smallest, gap.n,
	       (unsigned long long)(smallest * gap.n / 1000 / 2), (unsigned long long)smallest,
	       tm_repeats);
	fflush(stdout);
}

/*
 * One render pipeline, timed.
 *
 * The descriptor is built OUTSIDE the timed region on purpose: setting four pixel formats on
 * a Metal descriptor is Metal's own bookkeeping and has nothing to do with the compile, and
 * folding it in would quietly add a constant to every compile number here.
 *
 * A refusal is a number too -- how long the library took to decide -- so it is returned with
 * its reason instead of printed, and the caller keeps refusals out of the compiled ones'
 * statistics. A refused pipeline counted as a compile would make the driver look faster the
 * more shaders it cannot translate.
 */
static id<MTLRenderPipelineState>
tm_make_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
		 unsigned targets, uint64_t *ns, char *why, size_t size)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	for (unsigned i = 0; i < targets; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatBGRA8Unorm];
	}
	uint64_t t0 = tm_now_ns();
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];
	uint64_t t1 = tm_now_ns();

	*ns = t1 - t0;
	[pd release];
	if (ps == nil && why != NULL) {
		snprintf(why, size, "%s", err != nil ?
		    [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/*
 * THE GPU'S OWN SIDE, ASKED OF THE KERNEL INSTEAD OF READ OUT OF THE LOG.
 *
 * The first run of this mode could only subtract one case from another and point at the
 * kernel's `tl: q <fence> took <us>` lines for the rest -- and that turned out to be the
 * weakest part of it: those lines number every submission on the machine, this test is not the
 * only thing submitting, and aligning 1285 of them to four cases by eye is exactly the kind of
 * arithmetic that produces a confident wrong answer (docs/metal.md 10.43 shows the three
 * populations it did produce, and the ambiguity in their boundaries).
 *
 * So the kernel now hands the numbers over: request kRedwoodGPUWorked (0x7d) on RedwoodGPU
 * answers redwood_gpu_worked()'s three outputs -- the microseconds the 3D core itself worked,
 * on how many submissions, and how many of those were under 0.1, 0.25, 0.5, 1, 2 and 4 ms --
 * measured off CP_ALWAYS_ON_COUNTER at 19.2 ticks a microsecond (bsd/dev/redwood_adreno.c).
 * Reading it RESETS it, which is what makes bracketing a case possible: read once to zero,
 * draw, read again.
 *
 * Two things this must never do, and both are why it is written this way:
 *
 *   - IT MUST BE OPTIONAL. An older kernel answers 0x7d with "request 0x7d is not known here"
 *     and kIOReturnUnsupported, and a zero from that is indistinguishable from a GPU that did
 *     no work. So the first failure disables it for the run and says so in words; it never
 *     prints a zero that could be read as a measurement.
 *   - IT MUST SAY THAT IT TAKES THE NUMBERS AWAY. The kernel's own five-second report in
 *     RedwoodGPU.cpp shares these counters, so a boot that runs this mode leaves gaps in those
 *     lines. That is expected rather than a fault, and it is printed once.
 */
struct tm_worked {
	uint64_t microseconds;
	uint32_t submissions;
	uint32_t by[7];
};

static int tm_worked_state;     /* 0 not tried, 1 answers, -1 this kernel has no such request */

static bool
tm_gpu_worked(struct tm_worked *out)
{
	struct tm_worked in = { 0, 0, { 0 } };
	size_t size = sizeof(in);

	if (tm_worked_state < 0 || pic_connection == 0) {
		return false;
	}
	kern_return_t kr = IOConnectCallStructMethod(pic_connection, 0x7d, &in, sizeof(in),
						     out, &size);

	if (kr != KERN_SUCCESS || size < sizeof(*out)) {
		/* The two answers worth telling apart are printed as the number, because they
		 * mean different things: kIOReturnUnsupported (0xe00002c7) is a kernel without
		 * this request, kIOReturnNotReady (0xe00002d8) is this kernel with the Adreno
		 * not started -- and in the second case nothing else here would be measuring
		 * either. */
		printf("drvtest: time: this kernel does not report GPU time: request 0x7d "
		       "answered %#x and gave %zu of %zu bytes (0xe00002c7 is a kernel without "
		       "the request, 0xe00002d8 is one whose GPU is not started). The GPU's own "
		       "share is NOT measured in this run -- it is only in the kernel's own "
		       "\"tl: q <fence> took <us>\" lines, if this boot has gpu_log=1\n", kr,
		       size, sizeof(*out));
		fflush(stdout);
		tm_worked_state = -1;
		return false;
	}
	if (tm_worked_state == 0) {
		printf("drvtest: time: this kernel reports GPU time (request 0x7d), so each case "
		       "below is bracketed by a read of it. Reading RESETS the kernel's counters, "
		       "which it shares with its own five-second \"the GPU worked N us\" report -- "
		       "so that report will have gaps in this boot, and that is this mode's doing "
		       "and not a fault\n");
		fflush(stdout);
		tm_worked_state = 1;
	}
	return true;
}

/*
 * What the kernel says the GPU did, beside what we measured ourselves.
 *
 * `ours_ns` is the per-draw median this case measured and `draws` how many draws it made, so
 * the two can be compared per draw rather than in bulk. The subtraction is printed only when
 * the kernel timed about as many submissions as this case made: if the counts disagree,
 * something else was submitting and the remainder is not ours -- which is said instead of
 * subtracted.
 */
static void
tm_say_gpu_side(const char *what, const struct tm_worked *w, uint64_t ours_ns,
		unsigned long draws)
{
	if (w->submissions == 0) {
		printf("drvtest: time: %s: the kernel timed NO submission of this case, so its "
		       "GPU share is unknown here (the core finishes before its times are added "
		       "up, which happens at the next submission -- so a case this short can "
		       "leave none)\n", what);
		fflush(stdout);
		return;
	}
	double per = (double)w->microseconds / w->submissions;

	printf("drvtest: time: %s: the kernel says the 3D core itself worked %.1f us on each "
	       "of %u submissions (%u under 0.1 ms, %u under 0.25, %u under 0.5, %u under 1, "
	       "%u under 2, %u under 4, %u longer)\n", what, per, w->submissions, w->by[0],
	       w->by[1], w->by[2], w->by[3], w->by[4], w->by[5], w->by[6]);
	if (draws != 0 && w->submissions >= draws - draws / 8 &&
	    w->submissions <= draws + draws / 8 && ours_ns != 0) {
		double ours = ours_ns / 1000.0;

		printf("drvtest: time: %s: THE SPLIT: %.1f us a draw measured here, %.1f us of "
		       "it the core's own, %.1f us (%.0f%%) this side of the ioctl -- our "
		       "translation, the submission and the wait. The second number is the "
		       "GPU's clock and the first is this processor's, so the remainder carries "
		       "both their errors\n", what, ours, per, ours - per,
		       ours > 0 ? 100.0 * (ours - per) / ours : 0.0);
	} else if (draws != 0) {
		printf("drvtest: time: %s: this case made %lu draws and the kernel timed %u "
		       "submissions, so they are NOT the same work and nothing is subtracted "
		       "here\n", what, draws, w->submissions);
	}
	fflush(stdout);
}

/*
 * The attachments, and the TWO passes over them that differ in one field.
 *
 * Both descriptors hold the same textures; `*clear` loads by clearing and `*load` keeps what
 * is there. That is the only difference between them, which is what makes the difference
 * between their timings the cost of the clear and nothing else. (In this driver the clear is
 * attachment 0's alone -- redwoodmetal.m reads loadAction only for i == 0 -- so what the
 * difference prices is one full-attachment clear, however many targets are bound.)
 */
static bool
tm_targets(id<MTLDevice> dev, id<MTLTexture> *t, unsigned n, unsigned w, unsigned h,
	   MTLRenderPassDescriptor **clear, MTLRenderPassDescriptor **load)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	*clear = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	*load = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	for (unsigned i = 0; i < n; i++) {
		t[i] = [dev newTextureWithDescriptor:td];
		if (t[i] == nil) {
			printf("drvtest: time: attachment %u of %u at %ux%u could not be made, "
			       "so this size is NOT MEASURED (that is %.1f MB of attachment "
			       "asked for)\n", i, n, w, h,
			       (double)n * w * h * 4 / 1048576.0);
			fflush(stdout);
			return false;
		}
		for (int which = 0; which < 2; which++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[(which == 0 ? *clear : *load) colorAttachments]
				objectAtIndexedSubscript:i];

			[a setTexture:t[i]];
			[a setLoadAction:which == 0 ? MTLLoadActionClear : MTLLoadActionLoad];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 1)];
			[a setStoreAction:MTLStoreActionStore];
		}
	}
	return true;
}

/* What one command buffer with `draws` draws in it cost, in four parts. */
struct tm_parts {
	uint64_t make;          /* -[queue commandBuffer] */
	uint64_t encode;        /* the encoder, the bindings, the draws, -endEncoding */
	uint64_t commit;        /* -commit: in THIS driver, all of the work and the fence */
	uint64_t wait;          /* -waitUntilCompleted: must be ~0 here, and that is the point */
};

/*
 * One command buffer, drawn and waited on, with each of the four parts timed separately.
 *
 * Returns false and says why when the GPU did not finish it: a draw that failed is instant,
 * and a fast number from a draw that did nothing is the one reading this mode must never
 * produce.
 */
static bool
tm_one_buffer(id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	      id<MTLRenderPipelineState> ps, id<MTLBuffer> uniform, unsigned nuniform,
	      id<MTLBuffer> *vbuf, unsigned nvbuf, id<MTLTexture> tex, id<MTLSamplerState> samp,
	      unsigned draws, struct tm_parts *parts, const char *what)
{
	uint64_t t0 = tm_now_ns();
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	uint64_t t1 = tm_now_ns();

	if (cmd == nil) {
		printf("drvtest: time: %s: no command buffer\n", what);
		fflush(stdout);
		return false;
	}
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (enc == nil) {
		printf("drvtest: time: %s: no render encoder\n", what);
		fflush(stdout);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < nuniform; i++) {
		[enc setFragmentBuffer:uniform offset:0 atIndex:i];
	}
	for (unsigned i = 0; i < nvbuf; i++) {
		[enc setVertexBuffer:vbuf[i] offset:0 atIndex:i];
	}
	if (tex != nil) {
		[enc setFragmentTexture:tex atIndex:0];
	}
	if (samp != nil) {
		[enc setFragmentSamplerState:samp atIndex:0];
	}
	for (unsigned i = 0; i < draws; i++) {
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
	[enc endEncoding];
	uint64_t t2 = tm_now_ns();

	[cmd commit];
	uint64_t t3 = tm_now_ns();

	[cmd waitUntilCompleted];
	uint64_t t4 = tm_now_ns();

	tm_submissions += draws;
	if (tm_first_pixel == 0) {
		tm_first_pixel = t4;
	}
	parts->make = t1 - t0;
	parts->encode = t2 - t1;
	parts->commit = t3 - t2;
	parts->wait = t4 - t3;
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: time: %s: status %lu%s%s -- this draw did NOT happen, so its "
		       "time is not a draw's\n", what, (unsigned long)[cmd status],
		       [cmd error] != nil ? ": " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String]
					  : "");
		fflush(stdout);
		return false;
	}
	return true;
}

/*
 * THE COLD PATH, AND IT RUNS FIRST BECAUSE IT CANNOT BE REPEATED.
 *
 * One pipeline and one draw, in that order, before anything else in this mode has compiled
 * anything. This is the only measurement here that a process gets one sample of, and it is the
 * one a launching application pays: from the first statement of main() to a pixel the GPU has
 * actually written.
 *
 * What it does NOT include is said as plainly as what it does. The device, the Mesa screen and
 * the GPU context are opened in -initWithAcceleratorPort: (redwoodmetal.m calls
 * rwa_device_open there), which main() did above, so they are inside the "before this mode"
 * part and not inside the compile. And main()'s part also contains this test's own sweep of
 * every class of the driver, which no application does -- so the breakdown is printed in three
 * pieces rather than as one number that would flatter or damn the driver depending on which
 * piece a reader assumed.
 */
static void
tm_first_frame(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	struct tm_run cold = { 0 }, first = { 0 };
	id<MTLTexture> t[1] = { nil };
	MTLRenderPassDescriptor *rp_clear = nil, *rp_load = nil;
	struct tm_parts parts = { 0, 0, 0, 0 };
	char why[512] = { 0 };
	uint64_t ns = 0;
	id<MTLFunction> fn = [library newFunctionWithName:@"clearFragment01"];

	if (fn == nil) {
		printf("drvtest: time: no clearFragment01 in this library, so the cold path is "
		       "NOT MEASURED here (every warm number below still stands)\n");
		fflush(stdout);
		return;
	}
	if (!tm_targets(dev, t, 1, 4, 4, &rp_clear, &rp_load)) {
		[fn release];
		return;
	}
	id<MTLRenderPipelineState> ps = tm_make_pipeline(dev, nil, fn, 1, &ns, why,
							 sizeof(why));

	tm_cold_pipeline_ns = ns;
	tm_add(&cold, ns);
	if (ps == nil) {
		printf("drvtest: time: the process's first pipeline was REFUSED after %.1f us: "
		       "%s\n", ns / 1000.0, why);
		fflush(stdout);
		goto out;
	}
	tm_say("a pipeline, COLD: the process's very first", &cold,
	       "something made a pipeline before this -- nothing above this mode does, so "
	       "that would mean main() changed");
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform != nil && [uniform contents] != NULL) {
		memset([uniform contents], 0, 256);
		((float *)[uniform contents])[3] = 1.0f;
	}
	/* nuniform 0 for a buffer that could not be made: binding nil sixteen times would make
	 * the library refuse the draw for a reason that is this harness's and not the GPU's. */
	if (tm_one_buffer(queue, rp_clear, ps, uniform, uniform != nil ? 16 : 0, NULL, 0, nil,
			  nil, 1, &parts, "the cold draw") && tm_first_pixel != 0) {
		tm_add(&first, parts.commit);
		tm_say("-commit, COLD: the first draw of the process (4x4)", &first,
		       "it is near the warm 4x4 number far below -- then ir3 linked this "
		       "program at pipeline creation after all, and not at the draw");
		printf("drvtest: time: from the first statement of main() to the first pixel "
		       "this GPU wrote: %.1f ms = %.1f ms before this mode (the device, the "
		       "Mesa screen, the sweep of every class -- the sweep is this test's and "
		       "no application does it) + %.1f ms this mode's library and panel + %.1f "
		       "ms the cold compile + %.1f ms the cold draw\n",
		       (tm_first_pixel - tm_main_entered) / 1000000.0,
		       (tm_mode_entered - tm_main_entered) / 1000000.0,
		       (double)(tm_first_pixel - tm_mode_entered - tm_cold_pipeline_ns -
				parts.commit) / 1000000.0,
		       tm_cold_pipeline_ns / 1000000.0, parts.commit / 1000000.0);
		fflush(stdout);
	} else {
		printf("drvtest: time: the cold draw did not finish, so the distance from "
		       "main() to a first pixel is NOT MEASURED in this run\n");
		fflush(stdout);
	}
	[uniform release];
out:
	[ps release];
	[fn release];
	[t[0] release];
	[rp_clear release];
	[rp_load release];
}

/*
 * The compiles.
 *
 * Four questions, in this order, because each one is the control for the next:
 *
 *   cold            measured above, in tm_first_frame, because a process has exactly one cold
 *                   sample and spending it inside a loop would waste it. It carries the
 *                   compiler's one-time setup and one compile, and nothing inside this process
 *                   can separate them -- said here rather than attributed.
 *   warm, same      the same function again, over and over. If this is much cheaper than
 *                   cold, the difference is the one-time setup. If it is cheaper than "warm,
 *                   different" below, something is caching compiled code and this number is
 *                   not a compile at all -- air_metal.c says there is no cache
 *                   ("One compiled state, not a cache"), and this is how that claim is
 *                   checked rather than quoted.
 *   warm, different a different function each time, so no cache can be hit. THIS is the
 *                   number that prices a game's shader load.
 *   Apple's vertex  the same again with a real vertex stage (texturedVertex2D) instead of the
 *                   one this driver generates for a fragment-only pipeline: two modules
 *                   through the front end instead of one.
 */
static void
tm_pipelines(id<MTLDevice> dev, id<MTLLibrary> library)
{
	struct tm_run same = { 0 }, different = { 0 }, refused = { 0 }, apple = { 0 };
	char why[512] = { 0 };
	uint64_t ns = 0;
	id<MTLFunction> one = [library newFunctionWithName:@"clearFragment01"];
	NSArray<NSString *> *names = [library functionNames];

	/* clearFragment01 is the shader 10.33 drew 64 times on this GPU, so a pipeline made
	 * from it is a pipeline whose correctness is already known. A library without it still
	 * gets measured, with the first fragment function it does have. */
	if (one == nil) {
		for (NSString *name in names) {
			id<MTLFunction> fn = [library newFunctionWithName:name];

			if (fn != nil && [fn functionType] == MTLFunctionTypeFragment) {
				one = fn;
				printf("drvtest: time: this library has no clearFragment01, so "
				       "the pipeline timings use %s\n", [name UTF8String]);
				break;
			}
			[fn release];
		}
	}
	if (one == nil) {
		printf("drvtest: time: this library has no fragment function at all, so no "
		       "pipeline is timed\n");
		fflush(stdout);
		return;
	}
	id<MTLRenderPipelineState> ps = tm_make_pipeline(dev, nil, one, 1, &ns, why,
							 sizeof(why));

	if (ps == nil) {
		printf("drvtest: time: a pipeline was REFUSED after %.1f us: %s. Nothing below "
		       "this line is a compile's cost\n", ns / 1000.0, why);
		fflush(stdout);
		return;
	}
	[ps release];
	tm_add(&same, ns);

	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			ps = tm_make_pipeline(dev, nil, one, 1, &ns, why, sizeof(why));
			if (ps != nil) {
				tm_add(&same, ns);
				[ps release];
			}
		}
	}
	tm_say("a pipeline, warm, the SAME function again", &same,
	       "it is far below the next line -- then something caches compiled code and this "
	       "is a cache hit, not a compile");
	if (tm_cold_pipeline_ns != 0 && tm_median(&same) != 0) {
		printf("drvtest: time: the cold compile cost %.1f us and a warm one %.1f us at "
		       "the median, so whatever the compiler sets up once is worth %.1f us -- "
		       "paid by the first pipeline of a process and by no other\n",
		       tm_cold_pipeline_ns / 1000.0, tm_median(&same) / 1000.0,
		       ((double)tm_cold_pipeline_ns - (double)tm_median(&same)) / 1000.0);
		fflush(stdout);
	}

	unsigned taken = 0;

	for (NSString *name in names) {
		if (taken >= tm_repeats) {
			break;
		}
		@autoreleasepool {
			id<MTLFunction> fn = [library newFunctionWithName:name];

			if (fn == nil || [fn functionType] != MTLFunctionTypeFragment) {
				[fn release];
				continue;
			}
			id<MTLRenderPipelineState> p = tm_make_pipeline(dev, nil, fn, 1, &ns,
								       why, sizeof(why));

			if (p != nil) {
				tm_add(&different, ns);
				[p release];
				taken++;
			} else {
				tm_add(&refused, ns);
			}
			[fn release];
		}
	}
	tm_say("a pipeline, warm, a DIFFERENT function each time", &different,
	       "the functions are all alike -- these are this library's in file order, so a "
	       "library of one shape prices only that shape");
	tm_warm_pipeline_ns = tm_median(&different);
	tm_say("a pipeline the library REFUSED (how long to decide)", &refused,
	       "nothing was refused, in which case this line is absent and not zero");

	id<MTLFunction> vfn = [library newFunctionWithName:@"texturedVertex2D"];
	id<MTLFunction> ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];

	if (vfn != nil && ffn != nil) {
		for (unsigned i = 0; i < tm_repeats; i++) {
			@autoreleasepool {
				id<MTLRenderPipelineState> p = tm_make_pipeline(dev, vfn, ffn, 1,
									       &ns, why,
									       sizeof(why));
				if (p != nil) {
					tm_add(&apple, ns);
					[p release];
				}
			}
		}
		tm_say("a pipeline with Apple's OWN vertex stage (2 modules)", &apple,
		       "it equals the generated-vertex line above -- then one of the two "
		       "modules is not being compiled");
	} else {
		printf("drvtest: time: no texturedVertex2D/texturedFragment2DFloat in this "
		       "library, so the two-module compile is not timed\n");
	}
	[one release];
	[vfn release];
	[ffn release];
	fflush(stdout);
}

/*
 * One size of draw, measured five ways.
 *
 * `targets` attachments of `w` by `h`, the pipeline from clearFragment01 (whose vertex stage
 * this driver generates, so the quad covers the whole attachment -- 10.33) or from Apple's
 * textured blit, and then:
 *
 *   the first draw          with a pipeline nothing has drawn with yet, on its own. ir3 links
 *                           its program at DRAW time (ir3_cache_lookup), not at pipeline
 *                           creation, so the first draw of a pipeline is a different number
 *                           from the rest and a median that swallowed it would hide a cost a
 *                           game pays once per material.
 *   one draw, clearing      the whole round trip as every mode in this file has done it.
 *   one draw, NOT clearing  the same attachments through a pass whose loadAction is Load.
 *   TM_BATCH draws, one buffer  the marginal cost of a draw, with the command buffer paid for
 *                           once. In this driver each of them is still its own submission and
 *                           its own pass, which is exactly what this is here to price.
 *   the four parts          -commandBuffer, the encoding, -commit and -waitUntilCompleted,
 *                           each on its own, from the same runs.
 *
 * and then ONE PIXEL is read back and compared with what the shader was told to write,
 * because a draw that silently did nothing is the fastest draw there is.
 */
static void
tm_draw_case(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	     unsigned targets, unsigned w, unsigned h, bool textured)
{
	id<MTLTexture> t[8] = { nil };
	MTLRenderPassDescriptor *rp_clear = nil, *rp_load = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLFunction> vfn = nil, ffn = nil;
	id<MTLBuffer> uniform = nil, vpos = nil, vuv = nil;
	id<MTLTexture> small = nil;
	id<MTLSamplerState> samp = nil;
	char what[160], why[512] = { 0 };
	struct tm_run clear1 = { 0 }, load1 = { 0 }, loadN = { 0 }, make = { 0 },
		      encode = { 0 }, wait = { 0 }, firstdraw = { 0 };
	struct tm_parts parts = { 0, 0, 0, 0 };
	struct tm_worked worked = { 0, 0, { 0 } };
	bool gpu_side = false;
	uint64_t ns = 0;
	unsigned long before = tm_submissions;
	/* declared here and not where they are first wanted: every failure below leaves by
	 * `goto out`, and a jump over an initialisation is the kind of thing that compiles
	 * on one compiler and not the next */
	id<MTLBuffer> vbuf[2] = { nil, nil };
	unsigned nvbuf = 0, nuniform = 0;
	unsigned char px[4] = { 0, 0, 0, 0 };
	char label[192];
	/* the colour clearFragment01 is told to write, distinct from the clear (black) and from
	 * anything a wrong texel could be */
	static const unsigned char want[4] = { 109, 182, 73, 255 };   /* b g r a */
	static const float quad[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
	static float texels[PIC_TEX * PIC_TEX * 4];

	snprintf(what, sizeof(what), "%s %ux%u x%u", textured ? "Apple's blit at" : "a draw at",
		 w, h, targets);
	printf("drvtest: time: ---- %s (%.1f MB of attachment) ----\n", what,
	       (double)targets * w * h * 4 / 1048576.0);
	fflush(stdout);
	/* The functions first and the attachments after: a library without them (AV1SW's
	 * eleven kernels, say) would otherwise allocate 41 MB of render target before finding
	 * out it has nothing to draw with. */
	if (textured) {
		vfn = [library newFunctionWithName:@"texturedVertex2D"];
		ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];
	} else {
		ffn = [library newFunctionWithName:@"clearFragment01"];
	}
	if (ffn == nil || (textured && vfn == nil)) {
		printf("drvtest: time: %s: the functions this case needs are not in this "
		       "library, so it is NOT MEASURED\n", what);
		goto out;
	}
	if (!tm_targets(dev, t, targets, w, h, &rp_clear, &rp_load)) {
		goto out;
	}
	ps = tm_make_pipeline(dev, vfn, ffn, targets, &ns, why, sizeof(why));
	if (ps == nil) {
		printf("drvtest: time: %s: no pipeline (%s), so this case is NOT MEASURED\n",
		       what, why);
		goto out;
	}
	if (textured) {
		/* Apple's blit reads a position and a texture coordinate per vertex and
		 * samples an 8x8 texture; the corners, the coordinates and the texels are the
		 * picture's own (pic_bands), so this draw and the one 10.33 photographed are
		 * the same draw at a different size. */
		MTLTextureDescriptor *gd =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
			MTLPixelFormatRGBA32Float width:PIC_TEX height:PIC_TEX mipmapped:NO];

		[gd setUsage:MTLTextureUsageShaderRead];
		small = [dev newTextureWithDescriptor:gd];
		vpos = [dev newBufferWithLength:64 options:0];
		vuv = [dev newBufferWithLength:64 options:0];
		if (small == nil || vpos == nil || vuv == nil || [vpos contents] == NULL ||
		    [vuv contents] == NULL) {
			printf("drvtest: time: %s: no texture or vertex buffers\n", what);
			goto out;
		}
		for (unsigned y = 0; y < PIC_TEX; y++) {
			for (unsigned x = 0; x < PIC_TEX; x++) {
				unsigned char c[4];
				float *v = texels + (y * PIC_TEX + x) * 4;

				pic_texel_colour(x, y, c);
				v[0] = c[2] / 255.0f;
				v[1] = c[1] / 255.0f;
				v[2] = c[0] / 255.0f;
				v[3] = 1.0f;
			}
		}
		[small replaceRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
			 mipmapLevel:0
			   withBytes:texels
			 bytesPerRow:PIC_TEX * 4 * sizeof(float)];
		memcpy([vpos contents], quad, sizeof(quad));
		memcpy([vuv contents], uv, sizeof(uv));
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		[sd setMinFilter:MTLSamplerMinMagFilterNearest];
		[sd setMagFilter:MTLSamplerMinMagFilterNearest];
		[sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
		[sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
		samp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
	} else {
		/* The constant buffer every clearFragment reads, bound at every index up to 16
		 * for the mosaic's reason: the shader declares index 14 and a driver that bound
		 * only 0 would leave it reading nothing. */
		uniform = [dev newBufferWithLength:256 options:0];
		if (uniform == nil || [uniform contents] == NULL) {
			printf("drvtest: time: %s: no constant buffer\n", what);
			goto out;
		}
		float *u = [uniform contents];

		memset(u, 0, 256);
		u[0] = want[2] / 255.0f;
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
	}
	vbuf[0] = vpos;
	vbuf[1] = vuv;
	nvbuf = textured ? 2 : 0;
	nuniform = textured ? 0 : 16;

	/* The first draw with this pipeline, alone and not in any median. */
	if (!tm_one_buffer(queue, rp_clear, ps, uniform, nuniform, vbuf, nvbuf, small, samp, 1,
			   &parts, what)) {
		goto out;
	}
	tm_add(&firstdraw, parts.commit);
	tm_say("-commit, the FIRST draw with a fresh pipeline", &firstdraw,
	       "the pipeline had drawn before -- it is made in this function and used here "
	       "first, so that would mean this file changed");

	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_clear, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, 1, &parts, what)) {
				goto out;
			}
			tm_add(&clear1, parts.commit);
			tm_add(&make, parts.make);
			tm_add(&encode, parts.encode);
			tm_add(&wait, parts.wait);
		}
	}
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_load, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, 1, &parts, what)) {
				goto out;
			}
			tm_add(&load1, parts.commit);
		}
	}
	/*
	 * The batched loop is the one the kernel's counter is bracketed around, and not the
	 * other two: every one of its draws is a submission of exactly the same work, with no
	 * clear in it, so "per submission" on the kernel's side and "per draw" on ours mean the
	 * same thing. Bracketing the whole case would mix three different draws into one
	 * average and make the subtraction meaningless.
	 */
	gpu_side = tm_gpu_worked(&worked);        /* reads to zero; what it returns here is the past */
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_load, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, TM_BATCH, &parts, what)) {
				goto out;
			}
			tm_add(&loadN, parts.commit);
		}
	}
	if (gpu_side) {
		gpu_side = tm_gpu_worked(&worked);
	}
	tm_say("-commit, 1 draw, the pass CLEARS", &clear1,
	       "the attachment is not actually cleared -- this driver reads loadAction only "
	       "for attachment 0, so with more targets this prices one clear and not four");
	tm_say("-commit, 1 draw, the pass KEEPS (no clear)", &load1,
	       "it is not below the clearing line -- then the clear costs nothing measurable "
	       "at this size, which is the expected answer at 4x4 and not at 1080x2400");
	snprintf(label, sizeof(label), "-commit, %u draws in ONE command buffer, no clear",
		 TM_BATCH);
	tm_say(label, &loadN,
	       "it is about equal to one draw -- then the draws are being merged somewhere, "
	       "which this driver does not do: each is its own submission");
	tm_say("-[queue commandBuffer] alone", &make,
	       "it grows through the run -- that is the queue's 64 slots filling, and the "
	       "last line of this mode is the test for it");
	tm_say("the encoding (encoder + bindings + draw + end)", &encode,
	       "it is anywhere near -commit -- in this driver encoding only records, so a "
	       "large number here means work moved and this comment is stale");
	tm_say("-waitUntilCompleted after that -commit", &wait,
	       "it is NOT ~0 -- then the fence is no longer paid inside -commit and every "
	       "split in this mode is wrong");

	uint64_t one = tm_median(&load1), many = tm_median(&loadN);

	if (gpu_side && many != 0) {
		tm_say_gpu_side(what, &worked, many / TM_BATCH,
				(unsigned long)tm_repeats * TM_BATCH);
	}

	if (one != 0 && many > one && TM_BATCH > 1) {
		double marginal = (double)(many - one) / (TM_BATCH - 1);

		printf("drvtest: time: %s: a draw on its own costs %.1f us and each further "
		       "draw in the same command buffer %.1f us, so the command buffer itself "
		       "is worth %.1f us\n", what, one / 1000.0, marginal / 1000.0,
		       (one - marginal) / 1000.0);
		printf("drvtest: time: %s: at %.1f us a draw, a 16.7 ms frame (60 Hz) holds "
		       "%.0f such draws and a 33.3 ms frame (30 Hz) holds %.0f\n", what,
		       marginal / 1000.0, 16700000.0 / marginal, 33300000.0 / marginal);
		if (w * h > 16) {
			/* Both, because the first run printed only the first and the two differ
			 * by the number of targets: 599 us per megapixel of PASS at 1080x648 x4
			 * is 150 us per megapixel actually written, and that is the figure
			 * comparable with the panel's single target. */
			printf("drvtest: time: %s: that is %.2f us per megapixel of the pass and "
			       "%.2f us per megapixel actually written (%u targets), fill and one "
			       "pass over it together; %.1f GB/s of attachment written\n", what,
			       marginal / 1000.0 / ((double)w * h / 1048576.0),
			       marginal / 1000.0 / ((double)w * h * targets / 1048576.0), targets,
			       (double)w * h * targets * 4.0 / marginal);
		}
		fflush(stdout);
		/* kept for the arithmetic at the end of the mode */
		if (w * h == 16 && !textured) {
			tm_small_marginal_ns = (uint64_t)marginal;
		}
		if (w == PANEL_W && h == PANEL_H) {
			if (textured) {
				tm_panel_tex_ns = (uint64_t)marginal;
			} else {
				tm_panel_marginal_ns = (uint64_t)marginal;
				tm_buffer_ns = one > (uint64_t)marginal ?
				    one - (uint64_t)marginal : 0;
			}
		}
	}

	/* And the pixel, because every number above would be better if the draw did nothing. */
	[t[0] getBytes:px
	   bytesPerRow:4
	    fromRegion:MTLRegionMake2D(0, 0, 1, 1)
	   mipmapLevel:0];
	if (textured) {
		/* The viewport puts clip y = +1 in memory row 0 (10.28), so pixel (0,0) is
		 * texel (0, PIC_TEX-1), and with a NEAREST sampler it is that texel exactly. */
		unsigned char expect[4];

		pic_texel_colour(0, PIC_TEX - 1, expect);
		printf("drvtest: time: %s: pixel (0,0) is r g b %u %u %u and texel "
		       "(0,%u) is %u %u %u -- %s\n", what, px[2], px[1], px[0], PIC_TEX - 1,
		       expect[2], expect[1], expect[0],
		       px[0] == expect[0] && px[1] == expect[1] && px[2] == expect[2] ?
		       "the same, so the timed draws drew" : "NOT THE SAME: the timings above "
		       "are of a draw that did something else");
	} else {
		printf("drvtest: time: %s: pixel (0,0) is r g b %u %u %u and its buffer named "
		       "%u %u %u -- %s\n", what, px[2], px[1], px[0], want[2], want[1], want[0],
		       px[0] == want[0] && px[1] == want[1] && px[2] == want[2] ?
		       "the same, so the timed draws drew" : "NOT THE SAME: the timings above "
		       "are of a draw that did something else");
	}
	printf("drvtest: time: %s: this case asked the GPU for %lu submissions (the kernel's "
	       "own count, with gpu_log=1, must agree)\n", what, tm_submissions - before);
	fflush(stdout);
out:
	[ps release];
	[vfn release];
	[ffn release];
	[uniform release];
	[vpos release];
	[vuv release];
	[small release];
	[samp release];
	for (unsigned i = 0; i < targets; i++) {
		[t[i] release];
	}
	[rp_clear release];
	[rp_load release];
}

/*
 * The compute round trip, which is the same question as the draw's asked of the other path.
 *
 * A dispatch goes through the same -commit and the same fence as a draw (redwoodmetal.m's
 * commit loop calls rwa_dispatch for it), so the difference between this number and the 4x4
 * draw's is the difference between the two paths' own work and nothing else -- which is the
 * only way to say whether the per-submission cost lives in the render path or under both.
 *
 * The grid is 4x4x1 in one threadgroup, air_run's own and 10.15's, so the kernel being timed
 * is doing the work it was already checked doing. Sixteen buffers of 4 KB are bound for
 * compute_path's reason: a kernel sees only what it declared, binding more is harmless and
 * binding fewer makes it read a slot nothing was bound to. A kernel that declares a texture
 * is refused by the library with its reason, which is printed and then the next kernel tried;
 * four are tried and no more, because a mode that spends a boot looking for a kernel it can
 * dispatch has stopped measuring anything.
 */
static void
tm_dispatch(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	/*
	 * Twelve tries and not four, and textures bound as well as buffers, because the first
	 * run measured nothing here and said why: all four kernels it tried were refused with
	 * "writes texture 0 and 0 textures were bound" (logs/225409-run.log). That is a true
	 * sentence about this harness and nothing about the kernels, and AV1SW.metallib has
	 * eleven of them, so both limits were this test's own.
	 */
	enum { TM_DISPATCH_BUFFERS = 16, TM_DISPATCH_TRIES = 12 };
	struct tm_run commit = { 0 }, encode = { 0 }, wait = { 0 };
	id<MTLBuffer> bufs[TM_DISPATCH_BUFFERS] = { nil };
	id<MTLTexture> texs[TM_DISPATCH_BUFFERS] = { nil };
	id<MTLSamplerState> samp = nil;
	id<MTLComputePipelineState> ps = nil;
	NSString *chosen = nil;
	unsigned tried = 0, made = 0;

	for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
		bufs[i] = [dev newBufferWithLength:4096 options:0];
		if (bufs[i] == nil) {
			printf("drvtest: time: no buffer for the dispatch, so the compute round "
			       "trip is NOT MEASURED\n");
			goto out;
		}
		if ([bufs[i] contents] != NULL) {
			memset([bufs[i] contents], 0, 4096);
		}
	}
	/*
	 * Textures and a sampler for the kernels that declare them, 64x64 and in the first
	 * format this hardware will give as an image a shader writes. Sixteen of each, for the
	 * buffers' reason: a kernel sees only what it declared, so binding more is harmless and
	 * binding fewer leaves it reading a slot nothing was bound to. A kernel whose
	 * declaration wants another element type is still refused BY NAME, and that refusal is
	 * printed -- which is the answer either way.
	 */
	{
		static const MTLPixelFormat want[] = {
			MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint,
			MTLPixelFormatRGBA16Float, MTLPixelFormatR32Float,
			MTLPixelFormatRGBA8Unorm,
		};
		MTLTextureDescriptor *td = nil;

		for (size_t f = 0; f < sizeof(want) / sizeof(want[0]) && td == nil; f++) {
			MTLTextureDescriptor *try = [MTLTextureDescriptor
			    texture2DDescriptorWithPixelFormat:want[f] width:64 height:64
						     mipmapped:NO];

			[try setUsage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
			id<MTLTexture> probe = [dev newTextureWithDescriptor:try];

			if (probe != nil) {
				[probe release];
				td = try;
			}
		}
		for (int i = 0; td != nil && i < TM_DISPATCH_BUFFERS; i++) {
			texs[i] = [dev newTextureWithDescriptor:td];
			made += texs[i] != nil;
		}
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		samp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
		printf("drvtest: time: for the dispatch: %u of %u textures (64x64, format %lu, "
		       "read and written) and a sampler %s\n", made, TM_DISPATCH_BUFFERS,
		       td != nil ? (unsigned long)[td pixelFormat] : 0UL,
		       samp != nil ? "made" : "NOT made");
		fflush(stdout);
	}
	/*
	 * A kernel that compiles AND whose first dispatch finishes. Both are needed: 10.16
	 * measured nine kernels of which eight finished, and the ninth would otherwise be
	 * timed as a very fast failure.
	 */
	for (NSString *name in [library functionNames]) {
		if (tried >= TM_DISPATCH_TRIES || ps != nil) {
			break;
		}
		id<MTLFunction> fn = [library newFunctionWithName:name];

		if (fn == nil || [fn functionType] != MTLFunctionTypeKernel) {
			[fn release];
			continue;
		}
		tried++;
		NSError *err = nil;
		id<MTLComputePipelineState> candidate =
		    [dev newComputePipelineStateWithFunction:fn error:&err];

		if (candidate == nil) {
			printf("drvtest: time: %s got no compute pipeline: %s\n",
			       [name UTF8String], err != nil ?
			       [[err localizedDescription] UTF8String] : "(no reason)");
			[fn release];
			continue;
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

		/* A nil encoder would make the command buffer record nothing, and a command
		 * buffer that recorded nothing COMPLETES -- so without this check an empty
		 * dispatch would be timed as a fast one. */
		if (enc == nil) {
			printf("drvtest: time: no compute encoder for %s, so it is not timed\n",
			       [name UTF8String]);
			[candidate release];
			[fn release];
			continue;
		}
		[enc setComputePipelineState:candidate];
		for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
			[enc setBuffer:bufs[i] offset:0 atIndex:i];
			if (texs[i] != nil) {
				[enc setTexture:texs[i] atIndex:i];
			}
			if (samp != nil) {
				[enc setSamplerState:samp atIndex:i];
			}
		}
		[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
		    threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		tm_submissions++;
		if ([cmd status] == RW_CB_COMPLETED) {
			/*
			 * And whether anything was written, because a dispatch has no pixel to
			 * compare: all sixteen buffers were zeroed above, so a kernel that
			 * leaves them zero either writes nothing or wrote zeros, and that is
			 * worth saying out loud beside its time rather than assuming.
			 */
			unsigned long changed = 0;

			for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
				const unsigned char *p = [bufs[i] contents];

				for (unsigned k = 0; p != NULL && k < 4096; k++) {
					changed += p[k] != 0;
				}
			}
			printf("drvtest: time: %s finished and %lu of %u bytes of the bound "
			       "buffers are no longer zero%s\n", [name UTF8String], changed,
			       4096 * TM_DISPATCH_BUFFERS,
			       changed == 0 ? " -- so its time below is of a dispatch that may "
					      "have written nothing" : "");
			ps = candidate;
			chosen = name;
		} else {
			printf("drvtest: time: %s compiled but its first dispatch ended with "
			       "status %lu%s%s, so it is not timed\n", [name UTF8String],
			       (unsigned long)[cmd status], [cmd error] != nil ? ": " : "",
			       [cmd error] != nil ?
			       [[[cmd error] localizedDescription] UTF8String] : "");
			[candidate release];
		}
		[fn release];
	}
	if (ps == nil) {
		printf("drvtest: time: no kernel of this library both compiled and finished in "
		       "%u tries, so the compute round trip is NOT MEASURED here -- run this "
		       "mode again over /System/Library/VideoDecoders/AV1SW.metallib, whose "
		       "eleven kernels 10.15 dispatched on this GPU\n", tried);
		fflush(stdout);
		goto out;
	}
	printf("drvtest: time: the dispatch below is %s, 4x4x1 threads in one threadgroup, 16 "
	       "buffers bound\n", [chosen UTF8String]);
	fflush(stdout);
	/*
	 * Its compile, timed here and not with the render pipelines, because a library may have
	 * kernels and no fragment function at all (AV1SW.metallib is eleven kernels) and the
	 * render group leaves early for such a library. The kernel is one that has already
	 * dispatched, so this is the compile cost of a shader known to work.
	 */
	{
		struct tm_run compile = { 0 };
		id<MTLFunction> again = [library newFunctionWithName:chosen];

		for (unsigned r = 0; again != nil && r < tm_repeats; r++) {
			@autoreleasepool {
				NSError *e = nil;
				uint64_t t0 = tm_now_ns();
				id<MTLComputePipelineState> p =
				    [dev newComputePipelineStateWithFunction:again error:&e];
				uint64_t t1 = tm_now_ns();

				if (p != nil) {
					tm_add(&compile, t1 - t0);
					[p release];
				}
			}
		}
		[again release];
		tm_say("a COMPUTE pipeline, warm, the same kernel again", &compile,
		       "a kernel's compile is its own size -- the name is printed above, and "
		       "comparing it with a fragment shader's compares two shaders and not "
		       "two paths");
	}
	struct tm_worked worked = { 0, 0, { 0 } };
	bool gpu_side = tm_gpu_worked(&worked);

	for (unsigned r = 0; r < tm_repeats; r++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLCommandBuffer> cmd = [queue commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

			if (enc == nil) {
				printf("drvtest: time: the encoder stopped being given at "
				       "repeat %u\n", r);
				break;
			}
			[enc setComputePipelineState:ps];
			for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
				[enc setBuffer:bufs[i] offset:0 atIndex:i];
				if (texs[i] != nil) {
					[enc setTexture:texs[i] atIndex:i];
				}
				if (samp != nil) {
					[enc setSamplerState:samp atIndex:i];
				}
			}
			[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
			    threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
			[enc endEncoding];
			uint64_t t1 = tm_now_ns();

			[cmd commit];
			uint64_t t2 = tm_now_ns();

			[cmd waitUntilCompleted];
			uint64_t t3 = tm_now_ns();

			tm_submissions++;
			if ([cmd status] != RW_CB_COMPLETED) {
				printf("drvtest: time: the dispatch stopped finishing at repeat "
				       "%u (status %lu), so the numbers below cover only what "
				       "came before it\n", r, (unsigned long)[cmd status]);
				break;
			}
			tm_add(&encode, t1 - t0);
			tm_add(&commit, t2 - t1);
			tm_add(&wait, t3 - t2);
		}
	}
	tm_say("-commit, one 4x4x1 compute dispatch", &commit,
	       "this kernel is a big one -- its name is printed above, and a kernel's own work "
	       "is in this number as much as the round trip is");
	if (gpu_side && tm_gpu_worked(&worked)) {
		tm_say_gpu_side("the compute dispatch", &worked, tm_median(&commit),
				(unsigned long)commit.n);
	}
	tm_say("the encoding of that dispatch", &encode,
	       "it is anywhere near -commit: encoding only records here");
	tm_say("-waitUntilCompleted after that dispatch", &wait,
	       "it is not ~0 -- then the fence moved out of -commit on the compute path too");
	[ps release];
out:
	for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
		[bufs[i] release];
		[texs[i] release];
	}
	[samp release];
}

/*
 * What memory costs: a texture in, a texture out.
 *
 * -replaceRegion: is how a game's textures arrive, and -getBytes: is how every verifying mode
 * in this file reads an attachment. The second is not a game's cost at all, and is here so
 * that it can be subtracted from the picture mode's time instead of being mistaken for a
 * draw's. Both are reported in MB/s as well, which is the only form in which 256 bytes and
 * ten megabytes are comparable.
 */
static void
tm_memory(id<MTLDevice> dev, unsigned w, unsigned h)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];
	struct tm_run up = { 0 }, down = { 0 }, newbuf = { 0 };
	size_t bytes = (size_t)w * h * 4;
	unsigned char *host = malloc(bytes);
	unsigned repeats = tm_repeats > 8 ? 8 : tm_repeats;

	[td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

	if (tex == nil || host == NULL) {
		printf("drvtest: time: no %ux%u texture or no %zu bytes on this side, so the "
		       "upload and the readback are NOT MEASURED\n", w, h, bytes);
		free(host);
		[tex release];
		fflush(stdout);
		return;
	}
	memset(host, 0x5a, bytes);
	/* Eight and not thirty-two: ten megabytes each way is the measurement, and the point
	 * of the repeats is only that one outlier cannot be it. */
	for (unsigned i = 0; i < repeats; i++) {
		uint64_t t0 = tm_now_ns();

		[tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
		       mipmapLevel:0
			 withBytes:host
		       bytesPerRow:(NSUInteger)w * 4];
		tm_add(&up, tm_now_ns() - t0);
	}
	for (unsigned i = 0; i < repeats; i++) {
		uint64_t t0 = tm_now_ns();

		[tex getBytes:host
		  bytesPerRow:(NSUInteger)w * 4
		   fromRegion:MTLRegionMake2D(0, 0, w, h)
		  mipmapLevel:0];
		tm_add(&down, tm_now_ns() - t0);
	}
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLBuffer> b = [dev newBufferWithLength:65536
							   options:MTLResourceStorageModeShared];
			uint64_t t1 = tm_now_ns();

			if (b != nil) {
				tm_add(&newbuf, t1 - t0);
				[b release];
			}
		}
	}
	char line[160];

	snprintf(line, sizeof(line), "-replaceRegion: the whole %ux%u BGRA8", w, h);
	tm_say_rate(line, &up, bytes,
		    "the first sample is the odd one -- the pages of this side's buffer are "
		    "touched by the memset above, so they are not being faulted in here");
	snprintf(line, sizeof(line), "-getBytes: the whole %ux%u BGRA8", w, h);
	tm_say_rate(line, &down, bytes,
		    "a game is thought to pay this: it does not read attachments back, and "
		    "this line exists to be SUBTRACTED from the verifying modes");
	tm_say("-newBufferWithLength: 65536", &newbuf,
	       "the buffer is never touched, so this is the allocation and not the pages: a "
	       "first write to it would fault");
	free(host);
	[tex release];
}

/*
 * How many command buffers a second the queue sustains with nothing in them.
 *
 * This is the floor under every frame rate: a driver that cannot make and retire a command
 * buffer quickly cannot draw a frame quickly whatever the GPU does. It is also the one place
 * where a return of the 64-slot bug (10.33) would show up as a number -- each buffer takes a
 * slot of maxCommandBufferCount at -init and this driver gives it back at -commit, so if the
 * return ever goes away this loop stops dead at the 65th instead of slowing down, and the
 * last line of this mode is the deliberate test for that.
 */
static void
tm_queue(id<MTLCommandQueue> queue)
{
	struct tm_run empty = { 0 };
	unsigned n = tm_repeats * 8 < TM_MAX_SAMPLES ? tm_repeats * 8 : TM_MAX_SAMPLES;

	for (unsigned i = 0; i < n; i++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLCommandBuffer> cmd = [queue commandBuffer];

			[cmd commit];
			tm_add(&empty, tm_now_ns() - t0);
		}
	}
	tm_say("an EMPTY command buffer, made and committed", &empty,
	       "the autorelease pool is doing the freeing here, so this is not the cost of a "
	       "command buffer that an application keeps a handler on");

	uint64_t median = tm_median(&empty);

	/*
	 * Only when the median is above the clock's own tick. The first run's median was 0 and
	 * 1 microsecond -- that is "below this clock's resolution", and turning it into
	 * "1 000 000 a second" would be quoting the quantum as a measurement.
	 */
	if (median > 2000) {
		printf("drvtest: time: empty command buffers: %.0f a second at the median, so "
		       "no frame can cost less than %.1f us of this driver's bookkeeping per "
		       "command buffer\n", 1000000000.0 / (double)median, median / 1000.0);
	} else {
		printf("drvtest: time: an empty command buffer is BELOW this clock's resolution "
		       "(median %.1f us against a %.1f us tick): all that can be said is that "
		       "it costs at most a microsecond or two, and the queue's bookkeeping is "
		       "not in any frame's way\n", median / 1000.0, 1.0);
	}
	fflush(stdout);
}

/*
 * What it costs to show a frame -- which is NOT Metal's, and is said so where it is printed.
 *
 * This port has no drawable: a finished attachment reaches the panel by being read back into
 * this process (-getBytes:) and handed to the kernel's kRedwoodShow, which copies it row by
 * row into the boot framebuffer (iokit/Kernel/RedwoodGPU.cpp). Both halves are measured
 * because a frame rate that leaves them out is a frame rate for a game nobody can see.
 */
static void
tm_present(id<MTLDevice> dev)
{
	struct tm_run show = { 0 }, read = { 0 };
	unsigned repeats = tm_repeats > 8 ? 8 : tm_repeats;
	size_t bytes = (size_t)PANEL_W * PANEL_H * 4;

	if (pic_panel == NULL) {
		pic_panel = calloc(1, bytes);
	}
	if (pic_panel == NULL) {
		printf("drvtest: time: no memory for the panel, so the present is NOT "
		       "MEASURED\n");
		fflush(stdout);
		return;
	}
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:PANEL_W
							      height:PANEL_H
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

	if (tex != nil) {
		for (unsigned i = 0; i < repeats; i++) {
			uint64_t t0 = tm_now_ns();

			[tex getBytes:pic_panel
			  bytesPerRow:PANEL_W * 4
			   fromRegion:MTLRegionMake2D(0, 0, PANEL_W, PANEL_H)
			  mipmapLevel:0];
			tm_add(&read, tm_now_ns() - t0);
		}
		tm_say_rate("a frame OUT of Metal: -getBytes: 1080x2400", &read, bytes,
			    "the attachment was never drawn into, so this is a read of "
			    "untouched memory -- which is the right number for the COPY and "
			    "says nothing about a draw");
		[tex release];
	}
	if (pic_connection == 0) {
		printf("drvtest: time: no GPU service open, so the panel copy is NOT MEASURED "
		       "(that is the kernel's kRedwoodShow, not Metal's)\n");
		fflush(stdout);
		return;
	}
	for (unsigned i = 0; i < repeats; i++) {
		struct pic_show_request req = {
			(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
		};
		uint64_t t0 = tm_now_ns();

		IOConnectCallStructMethod(pic_connection, 0x7f, &req, sizeof(req), NULL, NULL);
		tm_add(&show, tm_now_ns() - t0);
	}
	tm_say_rate("the panel copy in the KERNEL (kRedwoodShow), 1080x2400", &show, bytes,
		    "this is counted as Metal's: it is a row-by-row copy in the kernel and the "
		    "same call src/gl_test.c shows a triangle with");

	uint64_t frame = tm_median(&read) + tm_median(&show);

	tm_present_ns = frame;
	if (frame != 0) {
		printf("drvtest: time: showing one finished 1080x2400 frame costs %.1f us "
		       "before any drawing at all, which alone caps this port at %.1f frames a "
		       "second\n", frame / 1000.0, 1000000000.0 / (double)frame);
		fflush(stdout);
	}
}

/*
 * The 64-slot ladder, LAST and on purpose.
 *
 * -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] waits on the
 * queue's semaphore (maxCommandBufferCount, 64 by default) and this driver returns the slot
 * at -commit (10.33). Sixty-four live, committed command buffers must therefore hold zero
 * slots and the sixty-fifth must not block.
 *
 * There is deliberately NO autorelease pool in this loop: with one, each buffer would be
 * deallocated at the end of its iteration and the slot would come back from -dealloc, so the
 * ladder would pass whether or not -commit returns it -- which is exactly the bug. Sixty-four
 * live command buffers holding no slot is the measurement.
 *
 * It is last because if the return ever regresses this blocks for ever, and then everything
 * else in this mode has already been printed and the log's final line names the cause.
 */
static void
tm_slots(id<MTLCommandQueue> queue)
{
	id<MTLCommandBuffer> live[64];
	struct tm_run sixtyfifth = { 0 };

	printf("drvtest: time: the slot test: 64 command buffers, committed and kept alive, "
	       "then the 65th. If this is the last line in the log, -commit stopped returning "
	       "the queue's semaphore slot and the 65th -commandBuffer is blocked for ever\n");
	fflush(stdout);
	for (unsigned i = 0; i < 64; i++) {
		live[i] = [[queue commandBuffer] retain];
		[live[i] commit];
	}
	/* The queue's own count, asked rather than inferred -- the same two selectors
	 * pic_queue_state uses, printed here so that every line of this mode reads alike. */
	NSUInteger (*ask)(id, SEL) = (NSUInteger (*)(id, SEL))objc_msgSend;
	SEL outstanding = sel_getUid("redwoodOutstandingCommandBuffers");
	SEL most = sel_getUid("maxCommandBufferCount");

	if ([queue respondsToSelector:outstanding] && [queue respondsToSelector:most]) {
		printf("drvtest: time: the queue takes %lu command buffers at a time and %lu of "
		       "the 64 alive still hold a slot (it must be 0: the slot goes back at "
		       "-commit)\n", (unsigned long)ask(queue, most),
		       (unsigned long)ask(queue, outstanding));
	} else {
		printf("drvtest: time: this queue does not say how many command buffers hold a "
		       "slot, so a block below would again have to be inferred\n");
	}
	fflush(stdout);
	uint64_t t0 = tm_now_ns();
	id<MTLCommandBuffer> next = [queue commandBuffer];
	uint64_t t1 = tm_now_ns();

	tm_add(&sixtyfifth, t1 - t0);
	[next commit];
	tm_say("the 65th -commandBuffer with 64 alive", &sixtyfifth,
	       "it is near the two-second mark rather than near the ordinary cost above -- "
	       "then the slot is being returned late instead of at -commit");
	for (unsigned i = 0; i < 64; i++) {
		[live[i] release];
	}
}

/*
 * WHAT THE NUMBERS MEAN FOR A GAME, with the arithmetic in the log rather than in somebody's
 * head afterwards.
 *
 * Every assumption is named on the line it is used, because each of them is generous to this
 * driver and a reader who does not know that will read these frame rates as a promise:
 *
 *   - ONE COMMAND BUFFER A FRAME. An application that makes several pays the command buffer's
 *     cost several times.
 *   - NO OVERLAP, which is not an assumption but this driver's behaviour: rwa_render_draw
 *     waits on its own fence inside -commit, so the processor and the GPU take turns and a
 *     frame is the SUM of its draws. A driver that pipelined would reach the maximum of the
 *     two instead of the sum, and that is the single biggest difference between these numbers
 *     and what the same hardware can do.
 *   - EVERY DRAW COVERS THE WHOLE PANEL in the upper bracket and sixteen pixels in the lower
 *     one. A real scene is between them and nearer the lower end, so the two brackets are
 *     printed and the middle is not invented.
 *   - THE PRESENT IS PAID ONCE A FRAME, and it is not Metal's: this port shows a frame by
 *     reading the attachment back and handing it to the kernel's row copy. A game that never
 *     shows anything saves it.
 *   - NOTHING ELSE RUNS. These were measured with the panel to ourselves; with iOS's own
 *     daemons alive the GPU and the one core doing all this are shared.
 *   - THE COMPILES ARE NOT IN THE FRAME. They are counted separately, as what a game pays
 *     before its first frame and again whenever it meets a shader it has not met.
 */
static void
tm_verdict(void)
{
	static const unsigned scene[] = { 1, 10, 50, 200, 1000 };

	printf("drvtest: time: ---- what this costs a game, from the medians above ----\n");
	if (tm_warm_pipeline_ns != 0) {
		printf("drvtest: time: a shader costs %.2f ms to compile and NOTHING caches it, "
		       "so a game with 200 pipelines pays %.1f s before its first frame and one "
		       "with 2000 pays %.0f s. There is no disk cache and no pipeline library on "
		       "this port (10.40: the file Metal looks for does not exist), so it pays "
		       "that again at every launch\n", tm_warm_pipeline_ns / 1000000.0,
		       200.0 * tm_warm_pipeline_ns / 1000000000.0,
		       2000.0 * tm_warm_pipeline_ns / 1000000000.0);
	} else {
		printf("drvtest: time: the compile cost was not measured in this run, so the "
		       "startup arithmetic is missing rather than estimated\n");
	}
	if (tm_present_ns != 0) {
		printf("drvtest: time: showing a frame costs %.2f ms before any drawing, which "
		       "by itself caps this port at %.1f frames a second\n",
		       tm_present_ns / 1000000.0, 1000000000.0 / (double)tm_present_ns);
	}
	if (tm_small_marginal_ns == 0 && tm_panel_marginal_ns == 0) {
		printf("drvtest: time: no per-draw median survived this run, so no frame rate "
		       "is computed here. That is the honest answer and not a zero\n");
		fflush(stdout);
		return;
	}
	printf("drvtest: time: a frame of N draws = the present + one command buffer + N x one "
	       "draw, with no overlap between processor and GPU because this driver fences "
	       "inside -commit. Two brackets: every draw 4x4 (the per-draw floor) and every "
	       "draw the whole 1080x2400 panel (the ceiling)\n");
	for (unsigned i = 0; i < sizeof(scene) / sizeof(scene[0]); i++) {
		double fixed = (double)tm_present_ns + (double)tm_buffer_ns;
		double small = fixed + (double)scene[i] * tm_small_marginal_ns;
		double big = fixed + (double)scene[i] * tm_panel_marginal_ns;

		printf("drvtest: time: %4u draws: %8.2f ms (%6.1f fps) if every draw is 4x4; "
		       "%10.2f ms (%6.2f fps) if every draw fills the panel\n", scene[i],
		       small / 1000000.0, small > 0 ? 1000000000.0 / small : 0.0,
		       big / 1000000.0, big > 0 ? 1000000000.0 / big : 0.0);
	}
	if (tm_panel_marginal_ns != 0 && tm_small_marginal_ns != 0) {
		/*
		 * The ratio of the two is a ratio and NOT a number of draws, which is how the
		 * first run of this printed "below about 13 draws a frame" -- a wrong sentence
		 * next to a right number. What the ratio says is how much of a draw is fixed:
		 * all of it at 4x4, a fraction of it at the panel, and the area at which the
		 * two are equal is what a reader actually wants.
		 */
		printf("drvtest: time: of one full-panel draw's %.2f ms, %.2f ms is also there at "
		       "4x4 (the submission, the fence and this driver's own work per draw) and "
		       "%.2f ms is the pixels: the fixed part is %.0f%% of a full-screen draw and "
		       "all of a small one, and the two are equal at about %.2f megapixels a "
		       "draw\n", tm_panel_marginal_ns / 1000000.0,
		       tm_small_marginal_ns / 1000000.0,
		       (double)(tm_panel_marginal_ns - tm_small_marginal_ns) / 1000000.0,
		       100.0 * tm_small_marginal_ns / (double)tm_panel_marginal_ns,
		       tm_panel_marginal_ns > tm_small_marginal_ns ?
		       2.4719 * tm_small_marginal_ns /
		       (double)(tm_panel_marginal_ns - tm_small_marginal_ns) : 0.0);
	}
	if (tm_panel_tex_ns != 0 && tm_panel_marginal_ns != 0) {
		printf("drvtest: time: Apple's textured blit over the panel costs %.2f ms "
		       "against %.2f ms for the shader that only writes a colour, so one "
		       "texture fetch per pixel over 2 592 000 pixels is worth %.2f ms\n",
		       tm_panel_tex_ns / 1000000.0, tm_panel_marginal_ns / 1000000.0,
		       (double)((int64_t)tm_panel_tex_ns - (int64_t)tm_panel_marginal_ns) /
		       1000000.0);
	}
	fflush(stdout);
}

/*
 * --time [REPEATS]: the mode itself.
 *
 * The order is deliberate: the clock first (so nothing else is read without it), then the
 * compiles (which a game pays before its first frame), then the draws from the smallest shape
 * in this file to the panel's own, then memory, then the queue, then the present, then the
 * slot test. Each group says what it is and what would make it wrong, and the last lines turn
 * the medians into a frame rate with the arithmetic shown, so a reader does not have to redo
 * it to disagree with it.
 */
static void
time_path(id<MTLDevice> dev, const char *metallib, unsigned repeats)
{
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];

	tm_mode_entered = tm_now_ns();
	tm_repeats = repeats < 4 ? 4 : (repeats > TM_MAX_SAMPLES ? TM_MAX_SAMPLES : repeats);
	printf("drvtest: time: %u repeats of each measurement; every line says what it is and "
	       "what would make it wrong. A median leads, the first sample is printed apart "
	       "because the first of anything here is a different measurement, and the max is "
	       "there so an outlier is seen and not smoothed\n", tm_repeats);
	printf("drvtest: time: this driver does the GPU work inside -commit and waits for the "
	       "fence there (10.13), so -commit is what an application waits on and "
	       "-waitUntilCompleted should measure ~0. Both are printed; if the second is not "
	       "~0, the split in every line below is wrong\n");
	printf("drvtest: time: the GPU's share is separated three ways, in order of how much "
	       "they can be trusted: by ASKING THE KERNEL for the core's own time per case "
	       "(request 0x7d, if this kernel has it -- the next line says whether it does), "
	       "by subtracting the 4x4 case from the big ones, and by the kernel's own "
	       "\"tl: q\" lines in this log with gpu_log=1\n");
	fflush(stdout);
	if (data == nil) {
		printf("drvtest: time: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: time: newLibraryWithData: gave nil\n");
		return;
	}
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	if (queue == nil) {
		printf("drvtest: time: no command queue\n");
		return;
	}
	/* opened here and not at the end, because the present measurement needs it and
	 * because a mode that stops early should still have shown what it got that far */
	pic_open_panel();

	tm_clock_floor();
	tm_first_frame(dev, library, queue);
	tm_pipelines(dev, library);

	/* 4x4 is every earlier number in this file, 1080x648 is the picture's band (so these
	 * are comparable with logs/140340-run.log, where the same draw read "0 ms"), and
	 * 1080x2400 is the panel: what a frame is. Four targets at 4x4 and at the band, one at
	 * the panel, because the panel with four is 41 MB of attachment and the question this
	 * mode asks is what a frame costs, not what four frames cost. */
	tm_draw_case(dev, library, queue, 4, 4, 4, false);
	tm_draw_case(dev, library, queue, 4, PANEL_W, PIC_BAND_H, false);
	tm_draw_case(dev, library, queue, 1, PANEL_W, PANEL_H, false);
	tm_draw_case(dev, library, queue, 1, PANEL_W, PANEL_H, true);

	tm_dispatch(dev, library, queue);
	tm_memory(dev, PANEL_W, PANEL_H);
	tm_queue(queue);
	tm_present(dev);

	tm_verdict();
	printf("drvtest: time: the whole mode took %.1f s of wall clock, which is NOT a "
	       "measurement of anything -- it is here so that a boot can be given enough "
	       "seconds for it\n", (tm_now_ns() - tm_mode_entered) / 1000000000.0);
	printf("drvtest: time: this mode asked the GPU for %lu submissions in all; with "
	       "gpu_log=1 the kernel's \"tl: q <fence> took <us>\" lines in this same log are "
	       "the GPU's own side of them, and its five-second report prints the average and "
	       "a histogram -- but only in a window in which something was submitted, so a "
	       "burst shorter than five seconds leaves no such line\n", tm_submissions);
	fflush(stdout);
	tm_slots(queue);
	printf("drvtest: time: done\n");
	fflush(stdout);
}

/*
 * WHAT EVERY CLASS OF THE DRIVER OWES, COUNTED ON EVERY RUN.
 *
 * 10.17 counted this for the device object and found nothing owed; the objects below it
 * were never counted, and the first boot in which iOS's own daemons could reach them
 * (logs/130132-run.log, metal=1 with launchd alive) answered why that mattered: 88 aborts
 * in a hundred seconds, all of them -[RedwoodMetalCommandQueue setGPUPriority:] and
 * -setCompletionQueue:, each followed by launchd restarting the daemon.
 *
 * Counting them is not simply "ask the runtime", for two reasons, and both of them are the
 * reason this code is longer than the one loop it replaces:
 *
 *  1. THE PROTOCOL MAY NOT BE IN THE PROCESS. MTLDevice and MTLDeviceSPI are declared in
 *     Metal.framework, so every process that asks Metal has them. MTLCommandQueueSPI,
 *     MTLCommandBufferSPI, MTLRenderCommandEncoderSPI, MTLComputeCommandEncoderSPI,
 *     MTLRenderPipelineStateSPI, MTLComputePipelineStateSPI and even the PUBLIC
 *     MTLCommandQueue and MTLRenderCommandEncoder are declared in AGXMetalA11.bundle --
 *     the GPU bundle of a phone whose GPU is Apple's, which never loads here. So
 *     objc_getProtocol returns NULL for them and `protocol_copyMethodDescriptionList` has
 *     nothing to copy. Their required methods are compiled in below, read out of this
 *     phone's own shared cache by tools/metal/protometh.py; where the runtime does have a
 *     protocol it is asked instead, and the two counts are compared out loud.
 *
 *  2. AN ENTRY IN A METHOD TABLE IS NOT BEHAVIOUR (docs/metal.md 10.1). Apple writes an
 *     abstract method as a real entry whose body is [self doesNotRecognizeSelector:_cmd];
 *     `respondsToSelector:` answers YES for it and the abort when it is called names the
 *     RECEIVER -- our class -- so in the log it is indistinguishable from a method that was
 *     never written. _MTLCommandEncoder has 27 such entries for the mesh and object shader
 *     stages alone. A count that only asked `respondsToSelector:` would have said zero
 *     owed while 28 aborts sat waiting, so each implementation found in the chain is also
 *     READ: an abstract body branches to the image's doesNotRecognizeSelector: stub, and
 *     that is what is looked for.
 */
struct rw_protocol {
	const char *name;
	const char *const *selectors;
	unsigned count;
};
struct rw_class {
	const char *name;
	const char *const *protocols;   /* the protocol and what it adopts, NULL-terminated */
};
#define RW_PROTOCOL(name, sym) { name, sym, sizeof(sym) / sizeof(sym[0]) }

/* The required methods of each protocol that is NOT in any process on this phone,
 * read out of AGXMetalA11.bundle in the device's own shared cache. Generated;
 * tools/metal/owes.py prints the same lists and says how they were taken. */
static const char *const spi_commandbufferspi[] = {   /* MTLCommandBufferSPI: 14 required */
	"addPurgedHeap:", "addPurgedResource:", "commitAndWaitUntilSubmittedWithDeadline:",
	"commitWithDeadline:", "deadlineProfile",
	"encodeDashboardFinalizeForResourceGroup:dashboard:value:forIndex:",
	"encodeDashboardFinalizeForResourceGroup:dashboard:values:indices:count:",
	"encodeDashboardTagForResourceGroup:", "isProfilingEnabled", "presentDrawable:options:",
	"profilingResults", "setLogs:", "setProfilingEnabled:", "userDictionary",
};
static const char *const spi_commandqueue[] = {   /* MTLCommandQueue: 7 required */
	"commandBuffer", "commandBufferWithDescriptor:", "commandBufferWithUnretainedReferences",
	"device", "insertDebugCaptureBoundary", "label", "setLabel:",
};
static const char *const spi_commandqueuespi[] = {   /* MTLCommandQueueSPI: 24 required */
	"backgroundTrackingPID", "commitQueue", "commitSynchronously", "completionQueue",
	"disableCrossQueueHazardTracking", "executionEnabled", "finish", "getBackgroundGPUPriority",
	"getGPUPriority", "isOpenGLQueue", "isProfilingEnabled", "maxCommandBufferCount", "qosLevel",
	"setBackgroundGPUPriority:", "setBackgroundGPUPriority:offset:", "setBackgroundTrackingPID:",
	"setCompletionQueue:", "setExecutionEnabled:", "setGPUPriority:", "setGPUPriority:offset:",
	"setProfilingEnabled:", "setSkipRender:", "setSubmissionQueue:", "skipRender",
};
static const char *const spi_computecommandencoder[] = {   /* MTLComputeCommandEncoder: 35 required */
	"dispatchThreadgroups:threadsPerThreadgroup:",
	"dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
	"dispatchThreads:threadsPerThreadgroup:", "dispatchType",
	"executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
	"executeCommandsInBuffer:withRange:", "memoryBarrierWithResources:count:",
	"memoryBarrierWithScope:", "sampleCountersInBuffer:atSampleIndex:withBarrier:",
	"setAccelerationStructure:atBufferIndex:", "setBuffer:offset:atIndex:",
	"setBufferOffset:atIndex:", "setBuffers:offsets:withRange:", "setBytes:length:atIndex:",
	"setComputePipelineState:", "setImageblockWidth:height:",
	"setIntersectionFunctionTable:atBufferIndex:",
	"setIntersectionFunctionTables:withBufferRange:", "setSamplerState:atIndex:",
	"setSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setSamplerStates:lodMinClamps:lodMaxClamps:withRange:", "setSamplerStates:withRange:",
	"setStageInRegion:", "setStageInRegionWithIndirectBuffer:indirectBufferOffset:",
	"setTexture:atIndex:", "setTextures:withRange:", "setThreadgroupMemoryLength:atIndex:",
	"setVisibleFunctionTable:atBufferIndex:", "setVisibleFunctionTables:withBufferRange:",
	"updateFence:", "useHeap:", "useHeaps:count:", "useResource:usage:",
	"useResources:count:usage:", "waitForFence:",
};
static const char *const spi_computecommandencoderspi[] = {   /* MTLComputeCommandEncoderSPI: 4 required */
	"newKernelDebugInfo", "setThreadgroupDistributionMode:",
	"setThreadgroupDistributionModeWithClusterGroupIndex:", "setThreadgroupPackingDisabled:",
};
static const char *const spi_computepipelinestatespi[] = {   /* MTLComputePipelineStateSPI: 7 required */
	"allocatedSize", "debugInstrumentationData", "getComputeKernelTelemetryID", "gpuHandle",
	"staticThreadgroupMemoryLength", "textureWriteRoundingMode", "uniqueIdentifier",
};
static const char *const spi_pipelinestatewithprogramaddresstable[] = {   /* MTLPipelineStateWithProgramAddressTable: 1 required */
	"pipelineBinaries",
};
static const char *const spi_rendercommandencoder[] = {   /* MTLRenderCommandEncoder: 125 required */
	"dispatchThreadsPerTile:",
	"drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
	"drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:instanceCount:baseInstance:",
	"drawPrimitives:indirectBuffer:indirectBufferOffset:",
	"drawPrimitives:vertexStart:vertexCount:",
	"drawPrimitives:vertexStart:vertexCount:instanceCount:",
	"drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
	"executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
	"executeCommandsInBuffer:withRange:",
	"memoryBarrierWithResources:count:afterStages:beforeStages:",
	"memoryBarrierWithScope:afterStages:beforeStages:",
	"sampleCountersInBuffer:atSampleIndex:withBarrier:", "setBlendColorRed:green:blue:alpha:",
	"setColorStoreAction:atIndex:", "setColorStoreActionOptions:atIndex:", "setCullMode:",
	"setDepthBias:slopeScale:clamp:", "setDepthClipMode:", "setDepthStencilState:",
	"setDepthStoreAction:", "setDepthStoreActionOptions:",
	"setFragmentAccelerationStructure:atBufferIndex:", "setFragmentBuffer:offset:atIndex:",
	"setFragmentBufferOffset:atIndex:", "setFragmentBuffers:offsets:withRange:",
	"setFragmentBytes:length:atIndex:", "setFragmentIntersectionFunctionTable:atBufferIndex:",
	"setFragmentIntersectionFunctionTables:withBufferRange:", "setFragmentSamplerState:atIndex:",
	"setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setFragmentSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setFragmentSamplerStates:withRange:", "setFragmentTexture:atIndex:",
	"setFragmentTextures:withRange:", "setFragmentVisibleFunctionTable:atBufferIndex:",
	"setFragmentVisibleFunctionTables:withBufferRange:", "setFrontFacingWinding:",
	"setMeshBuffer:offset:atIndex:", "setMeshBufferOffset:atIndex:",
	"setMeshBuffers:offsets:withRange:", "setMeshBytes:length:atIndex:",
	"setMeshSamplerState:atIndex:", "setMeshSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setMeshSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setMeshSamplerStates:withRange:", "setMeshTexture:atIndex:", "setMeshTextures:withRange:",
	"setObjectBuffer:offset:atIndex:", "setObjectBufferOffset:atIndex:",
	"setObjectBuffers:offsets:withRange:", "setObjectBytes:length:atIndex:",
	"setObjectSamplerState:atIndex:", "setObjectSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setObjectSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setObjectSamplerStates:withRange:", "setObjectTexture:atIndex:",
	"setObjectTextures:withRange:", "setObjectThreadgroupMemoryLength:atIndex:",
	"setRenderPipelineState:", "setScissorRect:", "setScissorRects:count:",
	"setStencilFrontReferenceValue:backReferenceValue:", "setStencilReferenceValue:",
	"setStencilStoreAction:", "setStencilStoreActionOptions:",
	"setTessellationFactorBuffer:offset:instanceStride:", "setTessellationFactorScale:",
	"setThreadgroupMemoryLength:offset:atIndex:", "setTileAccelerationStructure:atBufferIndex:",
	"setTileBuffer:offset:atIndex:", "setTileBufferOffset:atIndex:",
	"setTileBuffers:offsets:withRange:", "setTileBytes:length:atIndex:",
	"setTileIntersectionFunctionTable:atBufferIndex:",
	"setTileIntersectionFunctionTables:withBufferRange:", "setTileSamplerState:atIndex:",
	"setTileSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setTileSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setTileSamplerStates:withRange:", "setTileTexture:atIndex:", "setTileTextures:withRange:",
	"setTileVisibleFunctionTable:atBufferIndex:",
	"setTileVisibleFunctionTables:withBufferRange:", "setTriangleFillMode:",
	"setVertexAccelerationStructure:atBufferIndex:", "setVertexAmplificationCount:viewMappings:",
	"setVertexBuffer:offset:atIndex:", "setVertexBufferOffset:atIndex:",
	"setVertexBuffers:offsets:withRange:", "setVertexBytes:length:atIndex:",
	"setVertexIntersectionFunctionTable:atBufferIndex:",
	"setVertexIntersectionFunctionTables:withBufferRange:", "setVertexSamplerState:atIndex:",
	"setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setVertexSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setVertexSamplerStates:withRange:", "setVertexTexture:atIndex:",
	"setVertexTextures:withRange:", "setVertexVisibleFunctionTable:atBufferIndex:",
	"setVertexVisibleFunctionTables:withBufferRange:", "setViewport:", "setViewports:count:",
	"setVisibilityResultMode:offset:", "textureBarrier", "tileHeight", "tileWidth",
	"updateFence:afterStages:", "useHeap:", "useHeap:stages:", "useHeaps:count:",
	"useHeaps:count:stages:", "useResource:usage:", "useResource:usage:stages:",
	"useResources:count:usage:", "useResources:count:usage:stages:",
	"waitForFence:beforeStages:",
};
static const char *const spi_rendercommandencoderspi[] = {   /* MTLRenderCommandEncoderSPI: 34 required */
	"addSplitHandler:",
	"drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"isMemorylessRender", "setDepthClipModeSPI:",
	"setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:", "setLineWidth:",
	"setMeshBuffer:offset:atIndex:", "setMeshBufferOffset:atIndex:",
	"setMeshBuffers:offsets:withRange:", "setMeshBytes:length:atIndex:",
	"setMeshSamplerState:atIndex:", "setMeshSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setMeshSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setMeshSamplerStates:withRange:", "setMeshTexture:atIndex:", "setMeshTextures:withRange:",
	"setMeshVisibleFunctionTable:atBufferIndex:",
	"setMeshVisibleFunctionTables:withBufferRange:", "setObjectBuffer:offset:atIndex:",
	"setObjectBufferOffset:atIndex:", "setObjectBuffers:offsets:withRange:",
	"setObjectBytes:length:atIndex:", "setObjectSamplerState:atIndex:",
	"setObjectSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setObjectSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setObjectSamplerStates:withRange:", "setObjectTexture:atIndex:",
	"setObjectTextures:withRange:", "setObjectThreadgroupMemoryLength:atIndex:",
	"setObjectVisibleFunctionTable:atBufferIndex:",
	"setObjectVisibleFunctionTables:withBufferRange:", "setVertexAmplificationMode:value:",
};
static const char *const spi_renderpipelinestatespi[] = {   /* MTLRenderPipelineStateSPI: 32 required */
	"allocatedSize", "fragmentDebugInstrumentationData", "fragmentFunctionHandleWithFunction:",
	"getFragmentShaderTelemetryID", "getVertexShaderTelemetryID", "gpuHandle",
	"maxTotalThreadsPerMeshThreadgroup", "maxTotalThreadsPerObjectThreadgroup",
	"meshDebugInstrumentationData", "meshFunctionHandleWithFunction:",
	"meshThreadExecutionWidth", "newFragmentIntersectionFunctionTableWithDescriptor:",
	"newFragmentShaderDebugInfo",
	"newRenderPipelineStateWithAdditionalBinaryFunctions:fragmentAdditionalBinaryFunctions:error:",
	"newTileIntersectionFunctionTableWithDescriptor:",
	"newTileRenderPipelineStateWithAdditionalBinaryFunctions:error:",
	"newVertexIntersectionFunctionTableWithDescriptor:", "newVertexShaderDebugInfo",
	"newVisibleFunctionTableFromFragmentStageWithDescriptor:",
	"newVisibleFunctionTableFromMeshStageWithDescriptor:",
	"newVisibleFunctionTableFromObjectStageWithDescriptor:",
	"newVisibleFunctionTableFromTileStageWithDescriptor:",
	"newVisibleFunctionTableFromVertexStageWithDescriptor:", "objectDebugInstrumentationData",
	"objectFunctionHandleWithFunction:", "objectThreadExecutionWidth",
	"textureWriteRoundingMode", "tileDebugInstrumentationData",
	"tileFunctionHandleWithFunction:", "uniqueIdentifier", "vertexDebugInstrumentationData",
	"vertexFunctionHandleWithFunction:",
};

static const struct rw_protocol rw_protocols[] = {
	RW_PROTOCOL("MTLCommandBufferSPI", spi_commandbufferspi),
	RW_PROTOCOL("MTLCommandQueue", spi_commandqueue),
	RW_PROTOCOL("MTLCommandQueueSPI", spi_commandqueuespi),
	RW_PROTOCOL("MTLComputeCommandEncoder", spi_computecommandencoder),
	RW_PROTOCOL("MTLComputeCommandEncoderSPI", spi_computecommandencoderspi),
	RW_PROTOCOL("MTLComputePipelineStateSPI", spi_computepipelinestatespi),
	RW_PROTOCOL("MTLPipelineStateWithProgramAddressTable", spi_pipelinestatewithprogramaddresstable),
	RW_PROTOCOL("MTLRenderCommandEncoder", spi_rendercommandencoder),
	RW_PROTOCOL("MTLRenderCommandEncoderSPI", spi_rendercommandencoderspi),
	RW_PROTOCOL("MTLRenderPipelineStateSPI", spi_renderpipelinestatespi),
};

static const char *const chain_device[] = {
	"MTLDeviceSPI", "MTLDevice", NULL,
};
static const char *const chain_commandqueue[] = {
	"MTLCommandQueueSPI", "MTLCommandQueue", NULL,
};
static const char *const chain_commandbuffer[] = {
	"MTLCommandBufferSPI", "MTLCommandBuffer", NULL,
};
static const char *const chain_renderencoder[] = {
	"MTLRenderCommandEncoderSPI", "MTLCommandEncoderSPI", "MTLCommandEncoder",
	"MTLRenderCommandEncoder", NULL,
};
static const char *const chain_computeencoder[] = {
	"MTLComputeCommandEncoderSPI", "MTLCommandEncoderSPI", "MTLCommandEncoder",
	"MTLComputeCommandEncoder", NULL,
};
static const char *const chain_renderpipeline[] = {
	"MTLRenderPipelineStateSPI", "MTLRenderPipelineState",
	"MTLPipelineStateWithProgramAddressTable", NULL,
};
static const char *const chain_computepipeline[] = {
	"MTLComputePipelineStateSPI", "MTLComputePipelineState",
	"MTLPipelineStateWithProgramAddressTable", NULL,
};
static const char *const chain_texture[] = {
	"MTLTextureSPI", "MTLTexture", "MTLResource", "MTLResourceSPI", NULL,
};
static const char *const chain_buffer[] = {
	"MTLBuffer", "MTLResource", "MTLResourceSPI", NULL,
};
static const char *const chain_sampler[] = {
	"MTLSamplerStateSPI", "MTLSamplerState", NULL,
};
static const char *const chain_depthstencilstate[] = {
	"MTLDepthStencilStateSPI", "MTLDepthStencilState", NULL,
};

static const struct rw_class rw_classes[] = {
	{ "RedwoodMetalDevice", chain_device },
	{ "RedwoodMetalCommandQueue", chain_commandqueue },
	{ "RedwoodMetalCommandBuffer", chain_commandbuffer },
	{ "RedwoodMetalRenderEncoder", chain_renderencoder },
	{ "RedwoodMetalComputeEncoder", chain_computeencoder },
	{ "RedwoodMetalRenderPipeline", chain_renderpipeline },
	{ "RedwoodMetalComputePipeline", chain_computepipeline },
	{ "RedwoodMetalTexture", chain_texture },
	{ "RedwoodMetalBuffer", chain_buffer },
	{ "RedwoodMetalSampler", chain_sampler },
	{ "RedwoodMetalDepthStencilState", chain_depthstencilstate },
};

/* The target of the first branch out of this body, which for an abstract method of Apple's
 * is the __objc_stubs entry for doesNotRecognizeSelector:. B and BL are decoded in place --
 * imm26, signed, in units of four bytes. (A11 has no pointer authentication, so an IMP is
 * an address and can simply be read.) */
static uintptr_t
first_external_branch(IMP imp)
{
	const uint32_t *words = (const uint32_t *)(uintptr_t)imp;

	if (imp == NULL) {
		return 0;
	}
	for (int i = 0; i < 10; i++) {
		uint32_t word = words[i];

		if ((word >> 26) == 0x05 || (word >> 26) == 0x25) {   /* B, BL */
			intptr_t offset = (intptr_t)((int32_t)(word << 6) >> 6) * 4;
			uintptr_t target = (uintptr_t)(words + i) + offset;

			if (target < (uintptr_t)words || target > (uintptr_t)(words + 10)) {
				return target;
			}
		}
		if (word == 0xd65f03c0) {   /* ret: the body ended and called nothing out */
			break;
		}
	}
	return 0;
}

/*
 * The stub every abstract method of Apple's calls, learned rather than assumed: two methods
 * that are known to be abstract in this Metal are read, and their first outward branch has
 * to be the same address. If it is not, the check is switched off and says so -- a wrong
 * stub address would call every method in the chain abstract, which is the kind of number
 * that looks like a finding.
 *
 * One stub is enough because every base class this driver inherits from (_MTLDevice,
 * _MTLCommandQueue, _MTLCommandBuffer, _MTLCommandEncoder, _MTLResource,
 * _MTLObjectWithLabel, _MTLSamplerState, _MTLDepthStencilState, and the two pipeline
 * states) lives in Metal.framework, and __objc_stubs is per image. A base class from
 * another image -- IOGPU, say, if this driver ever inherits one of its classes -- would
 * have its own stub and would be missed here, which is why the two methods read are both
 * Metal's and why this says so.
 */
static uintptr_t
abstract_stub(void)
{
	static uintptr_t stub;
	static int looked;

	if (!looked) {
		/* -[_MTLDevice initWithAcceleratorPort:] is the abstract method this whole
		 * subsystem started from (10.1); -[_MTLCommandEncoder setMeshTexture:atIndex:]
		 * is one of the 27 mesh-stage entries the driver now overrides. */
		const char *classes[2] = { "_MTLDevice", "_MTLCommandEncoder" };
		const char *selectors[2] = { "initWithAcceleratorPort:",
					     "setMeshTexture:atIndex:" };
		uintptr_t found[2] = { 0, 0 };

		looked = 1;
		for (int i = 0; i < 2; i++) {
			Class cls = objc_getClass(classes[i]);
			Method method = cls != Nil ?
			    class_getInstanceMethod(cls, sel_getUid(selectors[i])) : NULL;

			if (method != NULL) {
				found[i] = first_external_branch(
				    method_getImplementation(method));
			}
		}
		if (found[0] != 0 && found[0] == found[1]) {
			stub = found[0];
			printf("drvtest: sweep: Apple's doesNotRecognizeSelector: stub is at "
			       "%#lx, agreed by both of the methods known to be abstract\n",
			       (unsigned long)stub);
		} else {
			printf("drvtest: sweep: the two methods known to be abstract branch to "
			       "%#lx and %#lx, so the stub could not be identified and no "
			       "method will be CALLED abstract below (the unanswered count is "
			       "unaffected)\n", (unsigned long)found[0], (unsigned long)found[1]);
		}
	}
	return stub;
}

/* Is what answers this selector one of Apple's abstract entries -- an abort wearing a
 * method table entry? */
static int
answer_is_abstract(Class cls, SEL selector)
{
	Method method = class_getInstanceMethod(cls, selector);
	uintptr_t stub = abstract_stub();

	return stub != 0 && method != NULL &&
	       first_external_branch(method_getImplementation(method)) == stub;
}

/* The required selectors of one protocol: the runtime's copy when this process has the
 * protocol, and otherwise the list read out of the cache. Returns 0 only when neither has
 * it, which is itself worth printing. */
static unsigned
required_selectors(const char *name, SEL *into, unsigned room, const char **source)
{
	Protocol *proto = objc_getProtocol(name);
	unsigned n = 0, taken = 0;

	*source = "runtime";
	if (proto != NULL) {
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n && taken < room; i++) {
			into[taken++] = req[i].name;
		}
		free(req);
		/* and if the cache said something different about the same protocol, say so:
		 * one of the two is then out of date, and it matters which */
		for (unsigned i = 0; i < sizeof(rw_protocols) / sizeof(rw_protocols[0]); i++) {
			if (strcmp(rw_protocols[i].name, name) == 0 &&
			    rw_protocols[i].count != n) {
				printf("drvtest: sweep: %s: the runtime requires %u methods and "
				       "the cache said %u; the runtime is used\n", name, n,
				       rw_protocols[i].count);
			}
		}
		return taken;
	}
	for (unsigned i = 0; i < sizeof(rw_protocols) / sizeof(rw_protocols[0]); i++) {
		if (strcmp(rw_protocols[i].name, name) != 0) {
			continue;
		}
		*source = "the cache";
		for (unsigned j = 0; j < rw_protocols[i].count && taken < room; j++) {
			into[taken++] = sel_getUid(rw_protocols[i].selectors[j]);
		}
		return taken;
	}
	*source = "nowhere";
	return 0;
}

/* One class, every protocol of it, counted. No instance is needed and none is made: what
 * `[object respondsToSelector:]` answers is what the class chain answers, so asking the
 * class cannot be defeated by a nil object or by a GPU that is not there. */
static void
sweep_class(const struct rw_class *entry)
{
	Class cls = objc_getClass(entry->name);
	SEL selectors[768];
	unsigned total = 0, unanswered = 0, abstract = 0, shown = 0;
	char which[256] = { 0 };

	if (cls == Nil) {
		printf("drvtest: sweep: %s IS NOT IN THIS PROCESS, so nothing of it was "
		       "counted\n", entry->name);
		return;
	}
	for (unsigned p = 0; entry->protocols[p] != NULL; p++) {
		const char *source = "";
		SEL one[768];
		unsigned n = required_selectors(entry->protocols[p], one,
						sizeof(one) / sizeof(one[0]), &source);

		if (n == 0) {
			printf("drvtest: sweep: %s: no list of %s exists in this process or in "
			       "the compiled-in tables, so its methods were NOT counted\n",
			       entry->name, entry->protocols[p]);
			continue;
		}
		snprintf(which + strlen(which), sizeof(which) - strlen(which), "%s%s(%u,%s)",
			 p == 0 ? "" : "+", entry->protocols[p], n, source);
		for (unsigned i = 0; i < n; i++) {
			int seen = 0;

			/* a selector required by two protocols of the same class is one
			 * method, and counting it twice would inflate every number here;
			 * SELs are interned, so comparing the pointers is comparing the names */
			for (unsigned j = 0; j < total; j++) {
				if (selectors[j] == one[i]) {
					seen = 1;
					break;
				}
			}
			if (seen || total >= sizeof(selectors) / sizeof(selectors[0])) {
				continue;
			}
			selectors[total++] = one[i];
			if (!class_respondsToSelector(cls, one[i])) {
				unanswered++;
				if (shown < 8) {
					printf("drvtest: sweep:   %s still owes %s\n",
					       entry->name, sel_getName(one[i]));
					shown++;
				}
			} else if (answer_is_abstract(cls, one[i])) {
				abstract++;
				if (shown < 8) {
					printf("drvtest: sweep:   %s answers %s with one of "
					       "Apple's abstract stubs, which aborts when it is "
					       "called\n", entry->name, sel_getName(one[i]));
					shown++;
				}
			}
		}
	}
	printf("drvtest: sweep: of %u methods %s requires, %u are unanswered and %u are "
	       "answered by an abstract stub of Apple's [%s]\n", total, entry->name,
	       unanswered, abstract, which);
}

/*
 * Every class the driver has, on every run. The numbers belong in the log of each boot
 * rather than in a measurement someone took once: the protocols are the system's, and the
 * system is what changes under us.
 */
static void
sweep_every_class(void)
{
	unsigned classes = sizeof(rw_classes) / sizeof(rw_classes[0]);

	printf("drvtest: sweep: %u classes, counted the way 10.17 counted the device: "
	       "unanswered means absent from the chain, and an entry whose body refuses is "
	       "answered\n", classes);
	for (unsigned i = 0; i < classes; i++) {
		sweep_class(&rw_classes[i]);
	}
}

int
main(int argc, char **argv)
{
	struct sigaction sa = { 0 };

	/* First, before anything: --time reports the distance from here to the first drawn
	 * pixel, which is what a launching application waits through. */
	tm_main_entered = tm_now_ns();
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = fault;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	const char *path = "/redwood/lib/libredwoodmetal.dylib";
	const char *metallib = "/System/Library/VideoDecoders/AV1SW.metallib";
	const char *only = NULL;
	bool as_bundle = false;
	bool draw = false;
	bool picture = false;
	int picture_seconds = 60;
	bool timing = false;
	unsigned timing_repeats = 32;
	int next = 1;

	/*
	 * --sweep: ask Metal for the system's device and count what every class of the driver
	 * owes, then stop. Nothing is dlopen'd and MTLAddDevice is not called, so this is the
	 * road iOS's own daemons take -- which makes it the one form of this program that can
	 * be added to a boot with metal=1 and launchd alive (rc.jobs.metalprops) to put the
	 * numbers in that log beside whatever the daemons then do.
	 */
	if (argc > next && strcmp(argv[next], "--sweep") == 0) {
		id<MTLDevice> found = MTLCreateSystemDefaultDevice();

		printf("drvtest: sweep: MTLCreateSystemDefaultDevice() = %s\n",
		       found != nil ? class_getName(object_getClass(found)) : "nil");
		if (found == nil) {
			printf("drvtest: sweep: without a device from Metal the driver's "
			       "classes are not loaded; boot with metal=1 and run "
			       "metal_props on first\n");
			return 1;
		}
		sweep_every_class();
		return 0;
	}
	if (argc > next && strcmp(argv[next], "--bundle") == 0) {
		as_bundle = true;
		next++;
	}
	if (argc > next && strcmp(argv[next], "--draw") == 0) {
		draw = true;
		next++;
	}
	/* --draw-index implies --draw: the indexed steps are compared with the ordinary
	 * draw's own picture, so that draw has to happen first and in the same process. */
	if (argc > next && strcmp(argv[next], "--draw-index") == 0) {
		draw = true;
		draw_index = true;
		next++;
	}
	/* --draw-attrib does NOT imply --draw: it builds its own pipelines, with a vertex
	 * descriptor, and the picture it compares everything with is its own first step. */
	if (argc > next && strcmp(argv[next], "--draw-attrib") == 0) {
		draw_attrib = true;
		next++;
	}
	/* --argbuf: MTLArgumentEncoder, and the buffer offset that goes with it. Its own
	 * flag and not an option of --draw because two of its three measurements draw
	 * nothing at all (an encoder from a function, and one from descriptors), and the
	 * third draws the same shader twice -- so what it prints is a comparison and not a
	 * picture. argbuf_path says what each of the three is worth. */
	if (argc > next && strcmp(argv[next], "--argbuf") == 0) {
		argbuf = true;
		next++;
	}
	/*
	 * --vertexlib and --gated are taken out of the argument list wherever they stand,
	 * before anything else is read, because a flag's meaning must not depend on its
	 * neighbours. The job list writes
	 *     --vertexlib L --vertex V --gated G --fragment F
	 * and a loop that accepted only a contiguous run of these two stopped at --vertex
	 * and left --gated for nobody; the device reported "cannot read --gated" after a
	 * boot that had already spent its twenty minutes. Lifting them out here keeps the
	 * --vertex/--fragment pair loop below exactly as it was.
	 */
	for (int i = next; i + 1 < argc; ) {
		bool lib = strcmp(argv[i], "--vertexlib") == 0;

		if (!lib && strcmp(argv[i], "--gated") != 0) {
			i++;
			continue;
		}
		if (lib) {
			attrib_vertex_lib = argv[i + 1];
		} else {
			/* "none" leaves the gate unmeasured and says so, for a library that
			 * has no module of that shape. */
			attrib_gated_function = strcmp(argv[i + 1], "none") == 0
			    ? NULL : argv[i + 1];
		}
		for (int j = i; j + 2 < argc; j++) {
			argv[j] = argv[j + 2];
		}
		argc -= 2;
	}
	/* --picture [SECONDS]: the draw at the size of the panel, put on the panel. Its own
	 * flag and not a size for --draw, because what it produces is not a pixel to compare
	 * but a whole screen, and because it ends in a loop that repaints: a run is over when
	 * XL_SECS says so, and the screenshot is taken while this is still painting. */
	if (argc > next && strcmp(argv[next], "--picture") == 0) {
		picture = true;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			picture_seconds = (int)strtol(argv[next], NULL, 10);
			next++;
		}
	}
	/*
	 * --time [REPEATS]: what the driver costs rather than whether it is right. Its own
	 * flag and not a size for --draw, for the same reason --picture has one: what it
	 * produces is not a pixel to compare but a time, and a time has to be measured at
	 * the panel's real size over and over before one number of it means anything.
	 * REPEATS defaults to 32, which is enough that one outlier cannot be the
	 * measurement and small enough that four sizes of draw still fit in a boot.
	 */
	if (argc > next && strcmp(argv[next], "--time") == 0) {
		timing = true;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			timing_repeats = (unsigned)strtoul(argv[next], NULL, 10);
			next++;
		}
	}
	/* --draw-depth is the draw with a depth attachment under it, which no pass in this
	 * port has ever had. Its own flag because what it measures is not a pixel but
	 * whether a draw was discarded, and that takes four draws of one quad (depth_path
	 * says why each). An optional format name follows, by name and not by number for
	 * --texfmt's reason. */
	if (argc > next && strcmp(argv[next], "--draw-depth") == 0) {
		draw_depth = true;
		next++;
		if (argc > next && strncmp(argv[next], "--", 2) != 0 &&
		    strchr(argv[next], '/') == NULL) {
			MTLPixelFormat f = format_named(argv[next]);

			if (f == 0) {
				printf("drvtest: %s is not a pixel format this test names\n",
				       argv[next]);
				return 2;
			}
			depth_attachment_format = f;
			next++;
		}
	}
	if (argc > next && strcmp(argv[next], "--draw-all") == 0) {
		draw = true;
		draw_all = true;
		next++;
		if (argc > next + 1 && argv[next][0] >= '0' && argv[next][0] <= '9') {
			draw_from = (unsigned)strtoul(argv[next], NULL, 10);
			draw_count = (unsigned)strtoul(argv[next + 1], NULL, 10);
			next += 2;
		}
	}
	while (argc > next && (strcmp(argv[next], "--seed-rows") == 0 ||
			       strcmp(argv[next], "--no-texdecl") == 0)) {
		if (strcmp(argv[next], "--seed-rows") == 0) {
			seed_rows = true;
		} else {
			use_decl = false;
		}
		next++;
	}
	while (argc > next + 1 && (strcmp(argv[next], "--vertex") == 0 ||
				   strcmp(argv[next], "--fragment") == 0)) {
		if (strcmp(argv[next], "--vertex") == 0) {
			vertex_function = argv[next + 1];
		} else {
			fragment_function = argv[next + 1];
		}
		next += 2;
	}
	while (argc > next + 1 && strncmp(argv[next], "--tex", 5) == 0) {
		if (strcmp(argv[next], "--tex") == 0) {
			const char *kind = argv[next + 1];

			texture_kind = strcmp(kind, "3d") == 0 ? MTLTextureType3D :
			    strcmp(kind, "cube") == 0 ? MTLTextureTypeCube :
			    strcmp(kind, "1d") == 0 ? MTLTextureType1D :
			    strcmp(kind, "2darray") == 0 ? MTLTextureType2DArray :
			    MTLTextureType2D;
		} else if (strcmp(argv[next], "--texslots") == 0) {
			parse_slots(argv[next + 1]);
		} else if (strcmp(argv[next], "--texfmt") == 0) {
			/* By name, mapped with the SDK's own constants: a pixel format written
			 * as a number here would be a number copied out of a header by hand,
			 * and the one thing worse than a wrong format is a wrong format that
			 * looks deliberate. */
			MTLPixelFormat f = format_named(argv[next + 1]);

			if (f != 0) {
				texture_format = f;
			}
		}
		next += 2;
	}
	if (argc > next) {
		metallib = argv[next++];
	}
	if (argc > next) {
		only = argv[next++];
	}

	/*
	 * THE TWO ROADS, and taking the wrong one is an abort rather than a mistake.
	 *
	 * MTLCreateSystemDefaultDevice is a dispatch_sync of a block whose whole body is a
	 * count of Metal's device array: 0 leaves nil, 1 returns that one, and ANYTHING ELSE
	 * is MTLReportFailure("doesn't know what to do with more than one device") and
	 * abort(). MTLAddDevice appends to that same array. So in a boot where the registry
	 * switch is on, Metal's own discovery has already registered our device, and a
	 * program that then adds its own makes two and dies on the next probe -- which is
	 * exactly what happened to triangle_app in the boot of 22 October 05:50, the only
	 * process in the whole system that aborted (docs/metal.md 10.47).
	 *
	 * Apple's daemons never meet this because none of them calls MTLAddDevice; neither
	 * does a game. It is a hazard of our own test programs alone, and the fix is an
	 * order: ask first, and build a device only when Metal has none.
	 */
	id<MTLDevice> found = MTLCreateSystemDefaultDevice();

	printf("drvtest: before anything, MTLCreateSystemDefaultDevice() = %s\n",
	       found != nil ? class_getName(object_getClass(found)) : "nil");
	if (found != nil) {
		printf("drvtest: Metal's own discovery already has a device, so this process "
		       "adds NOTHING: a second one would abort the next probe\n");
	}

	Class cls = Nil;

	if (found == nil && as_bundle) {
		/* Only one of the two images may be loaded: both define the same classes, and
		 * the Objective-C runtime would keep one of them and not say which. */
		cls = class_from_bundle("/redwood/lib/RedwoodMetal.bundle");
		if (cls == Nil) {
			return 1;
		}
	} else if (found == nil) {
		void *lib = dlopen(path, RTLD_NOW);

		if (lib == NULL) {
			printf("drvtest: dlopen(%s) failed: %s\n", path, dlerror());
			printf("drvtest: if that is the class symbol, the device class has to be "
			       "built at runtime after all\n");
			return 1;
		}
		printf("drvtest: %s loaded, so _OBJC_CLASS_$__MTLDevice binds\n", path);
		cls = objc_getClass("RedwoodMetalDevice");
	}
	Class base = objc_getClass("_MTLDevice");
	if (found != nil) {
		goto have_device;           /* steps 4 and 5 are Metal's own work in this boot */
	}
	if (cls == Nil || base == Nil) {
		printf("drvtest: RedwoodMetalDevice %s, _MTLDevice %s\n",
		       cls != Nil ? "is here" : "is missing", base != Nil ? "is here" : "is missing");
		return 1;
	}
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");
	printf("drvtest: step 4 isSubclassOfClass:_MTLDevice %s; conforms to MTLDeviceSPI %s, "
	       "to MTLDevice %s\n",
	       [cls isSubclassOfClass:base] ? "PASSES" : "fails",
	       spi != NULL && [cls conformsToProtocol:spi] ? "yes" : "no",
	       [cls conformsToProtocol:@protocol(MTLDevice)] ? "yes" : "no");

	io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
	if (svc == IO_OBJECT_NULL) {
		printf("drvtest: no IOAcceleratorES service to hand it\n");
		return 1;
	}
	id dev = ((id (*)(id, SEL, io_service_t))objc_msgSend)(
	    [cls alloc], sel_getUid("initWithAcceleratorPort:"), svc);
	printf("drvtest: step 5 gave %s\n",
	       dev != nil ? class_getName(object_getClass(dev)) : "nil");
	if (dev == nil) {
		return 1;
	}

	uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
	void (*add_device)(id) = (void (*)(id))(slide + MTL_ADD_DEVICE_UNSLID);

	@try {
		add_device(dev);
		printf("drvtest: MTLAddDevice returned\n");
	} @catch (NSException *e) {
		printf("drvtest: MTLAddDevice threw %s: %s\n", [[e name] UTF8String],
		       [[e reason] UTF8String]);
		return 1;
	}

have_device:
	;
	id<MTLDevice> sys = found != nil ? found : MTLCreateSystemDefaultDevice();
	printf("drvtest: MTLCreateSystemDefaultDevice() = %s\n",
	       sys != nil ? class_getName(object_getClass(sys)) : "nil");
	if (sys == nil) {
		return 1;
	}
	printf("drvtest: it calls itself \"%s\"\n", [[sys name] UTF8String]);

	/* What the device still owes, counted by the runtime on the device rather than guessed
	 * from the cache: every method the two protocols require, asked of the real object. An
	 * unanswered one is not a gap in a table, it is an application aborting on
	 * "unrecognized selector", because the gates inside Metal are assertions. */
	for (int which = 0; which < 2; which++) {
		const char *pname = which == 0 ? "MTLDevice" : "MTLDeviceSPI";
		Protocol *proto = objc_getProtocol(pname);
		unsigned n = 0, missing = 0, shown = 0;

		if (proto == NULL) {
			continue;
		}
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n; i++) {
			if ([sys respondsToSelector:req[i].name]) {
				continue;
			}
			missing++;
			if (shown < 8) {
				printf("drvtest:   %s still owes %s\n", pname,
				       sel_getName(req[i].name));
				shown++;
			}
		}
		free(req);
		printf("drvtest: of %u methods %s requires, %u are unanswered (unanswered means "
		       "absent from the chain; an entry whose body refuses is not counted here)\n",
		       n, pname, missing);
	}

	/* (the same count for every other class of the driver, sweep_every_class(), is --sweep's
	 * alone now: on iOS 26 it ends in a pointer-authentication kill, reason namespace 28) */

	/* The first resource. A buffer here is a GEM allocation of the kernel's own gpu
	 * driver, so this checks the whole path: the kernel gave memory, the GPU has an
	 * address for it, this processor can write it and read back what it wrote. */
	id<MTLBuffer> buf = [sys newBufferWithLength:4096 options:MTLResourceStorageModeShared];

	printf("drvtest: newBufferWithLength:options: gave %s\n",
	       buf != nil ? class_getName(object_getClass(buf)) : "nil");
	if (buf != nil) {
		unsigned char *p = [buf contents];

		printf("drvtest:   length %lu, contents %p, gpu address %s\n",
		       (unsigned long)[buf length], p,
		       [buf gpuAddress] != 0 ?
		       [[NSString stringWithFormat:@"%#llx",
			 (unsigned long long)[buf gpuAddress]] UTF8String] :
		       "not exposed (this one came from the compiler library)");
		if (p != NULL) {
			int same = 1;

			for (int i = 0; i < 4096; i++) {
				p[i] = (unsigned char)(i * 7 + 13);
			}
			for (int i = 0; i < 4096; i++) {
				if (p[i] != (unsigned char)(i * 7 + 13)) {
					same = 0;
					break;
				}
			}
			printf("drvtest:   wrote 4096 bytes and read back %s\n",
			       same ? "the same bytes" : "SOMETHING ELSE");
		}
		/* and what such a buffer still owes, asked of the runtime rather than guessed */
		unsigned n = 0;
		Protocol *bp = objc_getProtocol("MTLBuffer");
		struct objc_method_description *req =
		    bp != NULL ? protocol_copyMethodDescriptionList(bp, YES, YES, &n) : NULL;
		unsigned missing = 0;

		for (unsigned i = 0; i < n; i++) {
			if (![buf respondsToSelector:req[i].name]) {
				if (missing < 12) {
					printf("drvtest:   still owed: %s\n",
					       sel_getName(req[i].name));
				}
				missing++;
			}
		}
		free(req);
		printf("drvtest:   of %u methods MTLBuffer requires, %u are unanswered\n",
		       n, missing);
	}

	/* And what Metal can do with it: the container path needs no driver code at all, which
	 * is where subsystem (1) plugs in one level further down. */
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];
	if (data == nil) {
		printf("drvtest: cannot read %s\n", metallib);
		return 0;
	}
	printf("drvtest: %s, %lu bytes\n", metallib, (unsigned long)[data length]);
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> mtllib = [sys newLibraryWithData:dd error:&err];

	printf("drvtest: newLibraryWithData: gave %s with %lu functions\n",
	       mtllib != nil ? class_getName(object_getClass(mtllib)) : "nil",
	       mtllib != nil ? (unsigned long)[[mtllib functionNames] count] : 0UL);
	if (mtllib == nil) {
		return 0;
	}
	id<MTLFunction> fn = [mtllib newFunctionWithName:[[mtllib functionNames] firstObject]];

	printf("drvtest: newFunctionWithName: gave %s\n",
	       fn != nil ? class_getName(object_getClass(fn)) : "nil");

	if (timing) {
		time_path(sys, metallib, timing_repeats);
		return 0;
	}
	if (picture) {
		picture_path(sys, metallib, picture_seconds);
		return 0;
	}
	if (draw_depth) {
		depth_path(sys, metallib);
		return 0;
	}
	if (draw_attrib) {
		attrib_path(sys, metallib);
		return 0;
	}
	if (argbuf) {
		/* the compiler library's own record count, by the same loader the texture
		 * query uses: one dlopen of the image the driver already holds */
		load_texture_query(sys);
		argbuf_path(sys, metallib);
		return 0;
	}
	if (draw) {
		draw_path(sys, metallib);
		return 0;
	}
	if (only == NULL) {
		air_of_each_function(mtllib);
	}
	compute_path(sys, mtllib, only);
	return 0;
}
