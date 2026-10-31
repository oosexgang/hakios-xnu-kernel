/*
 * RedwoodTriangle -- an application of this port's own, and the first process that is not
 * one of Apple's daemons to ask this Metal driver for a frame.
 *
 * WHY IT EXISTS. Every Metal result this port has was measured inside backboardd,
 * SpringBoard, mediaserverd or a /redwood/bin diagnostic. Three things no daemon can
 * answer are answered here, and the three are independent:
 *
 *   1  does the driver work in an ORDINARY APPLICATION process -- a bundle in
 *      /Applications, launched by SpringBoard, running as mobile, with UIKit above it;
 *   2  does -[CAMetalLayer nextDrawable] come back with a texture at all. It asks a device
 *      exactly one thing for that, -newTextureWithDescriptor:iosurface:plane:
 *      (docs/metal.md 10.46), and this program is that method's first caller;
 *   3  does a frame presented through the drawable reach the panel. Presentation never
 *      goes through a device: the drawable's IOSurface is registered in the layer's
 *      CAImageQueue by surface id and the window server takes it from there, so a triangle
 *      on the screen is the whole of that chain working at once.
 *
 * WHAT IT DRAWS, and why exactly this. The pipeline is Apple's own clearFragment01 out of
 * AppleMetalGLRenderer's library with NO vertex function, which is the compiler library's
 * cue to supply its full-screen quad stage (tools/mesa/air/air_metal.h). That stage's quad
 * has four corners in a fixed order -- (-1,-1), (1,-1), (-1,1), (1,1) -- so a draw of THREE
 * vertices of it is a triangle over the first three, and clearFragment01 fills it with the
 * float4 in the constant buffer at index 14. Both halves of that are already measured on
 * this GPU: clearFragment01 drew 64 of the mosaic's 64 tiles into BGRA8Unorm attachments
 * with no wrong pixel (docs/metal.md 10.33). The only new thing in the draw is the vertex
 * COUNT, and the only new thing in the pass is that its attachment is a drawable.
 *
 * So the picture is three colours and each means something different:
 *
 *   black        the layer's own backgroundColor. Nothing was ever presented.
 *   pure blue    the render pass's clear colour, 0 0 255. A frame reached the screen and
 *                the triangle did not cover that pixel.
 *   orange       the triangle, 255 128 0.
 *
 * The three are deliberately far apart in every channel, and blue and orange are each
 * other's opposite in red and blue: a channel swap anywhere between the shader and the
 * panel turns the background RED and the triangle azure, which is seen before it is
 * counted.
 *
 * WHAT IS CHECKED IN NUMBERS, so that a photograph is not the only evidence. The frame is
 * read back twice, through two different views of the same memory:
 *
 *   - through the IOSurface, with IOSurfaceGetBaseAddress. This is the processor reading
 *     the very pages the display controller scans out, and it is the direction that tells a
 *     texture over the surface from a texture over a copy of it: nothing writes the surface
 *     back on this path, so a staging copy fails here and only here.
 *   - through -getBytes:, which goes to the GPU driver's own mapping of the same resource.
 *
 * and each is counted the same way: every pixel against the rasterisation rule. A pixel
 * centre is covered when (x + 0.5)/width < (y + 0.5)/height, which is the interior of the
 * triangle through clip (-1,-1), (1,-1), (-1,1) once the viewport puts clip y = +1 in
 * memory row 0 (docs/metal.md 10.28). For 1125 by 2436 no pixel centre falls ON that edge
 * -- (2x+1)*2436 is even and (2y+1)*1125 is odd, so they are never equal -- which is why
 * the count can be exact rather than approximate, and why a pixel that is NEITHER colour is
 * a failure and not a rounding.
 *
 * TWO MODES, one binary, because a boot is expensive and the three questions above fail
 * separately:
 *
 *   (no argument)   the application. UIApplicationMain, a window, a view whose layer is a
 *                   CAMetalLayer, and a frame every half second so that a screenshot can be
 *                   taken whenever. This is the only mode that can answer question 3.
 *   --layer         no UIKit: a bare CAMetalLayer of the panel's size, driven once, with
 *                   the same checks. It answers 1 and 2 in a process the window server
 *                   knows nothing about, so it still answers them when the application
 *                   fails to launch -- and it runs from the job list, where its output is
 *                   certain to be in the log.
 *
 * THE DEVICE: ASKED FOR FIRST, BUILT ONLY IF METAL HAS NONE -- and that order is not a
 * preference, it is the difference between running and aborting. See rwt_make_device, which
 * carries the whole reason with the addresses: MTLCreateSystemDefaultDevice aborts on a
 * device list of length two, in Apple's own words, so a process that lets discovery register
 * one and then registers one of its own dies at its next call. The first edition of this file
 * did exactly that and died in logs/055044-run.log.
 *
 * So there are two roads and the log says which was taken:
 *
 *   discovery ON   (MetalPluginName on the IOAcceleratorES service, which needs boot-arg
 *                   metal=1 and a process to set it). MTLCreateSystemDefaultDevice() answers
 *                   our plug-in device and this process adds NOTHING. This is the road a
 *                   game takes, so it is the more interesting of the two.
 *   discovery OFF   MTLCreateSystemDefaultDevice() answers nil, and this program walks
 *                   discovery's steps 3, 4 and 5 itself out of /redwood/lib/RedwoodMetal.bundle
 *                   -- the bundle, the class, -initWithAcceleratorPort:, MTLAddDevice --
 *                   exactly as src/metal_drv_test.m does. Worth keeping, because then the only
 *                   Metal device in the system is the one made here, which makes every frame
 *                   below attributable without an argument.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern id MTLCreateSystemDefaultDevice(void);

/*
 * MTLAddDevice is not exported, so it is reached by its distance from one that is -- the
 * same two numbers src/metal_drv_test.m uses, from the same cache
 * (ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64).
 */
#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define MTL_ADD_DEVICE_UNSLID    0x181a1875cUL

/* MTLCommandBufferStatus, Metal's own numbers (redwoodmetal.m says them in the log too). */
#define RW_CB_COMPLETED 4

/*
 * The two fields of _CAMetalDrawablePrivate this file reads, and nothing else of it. The
 * struct is calloc(1, 0x70) in get_unused_drawable and its layout is in two independent
 * places: the instructions that write it (docs/metal.md 10.46) and the Objective-C type
 * encoding of CAMetalDrawable's one ivar, which the cache spells out --
 *   ^{_CAMetalDrawablePrivate={Atomic={?=i}}IIIQQQQd^{_CAMetalLayerPrivate}
 *     ^{__IOSurface}@@^{CGColorSpace}@Cb1b1b1b1b1b1} _priv
 * whose fields land at 0x38 the layer's private, 0x40 the surface, 0x48 the texture and
 * 0x50 the device.
 *
 * The offsets are USED as a test before they are used as an answer: this file reads 0x48
 * and 0x50 and requires them to be the texture and the device it already has in hand, and
 * only then believes 0x40. A layout that moved therefore makes this measurement absent
 * rather than wrong.
 */
#define RWT_PRIV_SURFACE 0x40
#define RWT_PRIV_TEXTURE 0x48
#define RWT_PRIV_DEVICE  0x50

/* The library and the shader, by the paths and names that have already drawn on this GPU. */
static const char *rwt_metallib =
    "/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib";
static const char *rwt_fragment = "clearFragment01";
static const char *rwt_bundle = "/redwood/lib/RedwoodMetal.bundle";

/* The two colours, as the bytes a BGRA8Unorm attachment must end up holding. */
static const unsigned char rwt_clear_bgra[4]    = { 255, 0, 0, 255 };     /* pure blue */
static const unsigned char rwt_triangle_bgra[4] = { 0, 128, 255, 255 };   /* orange */

/* How big the bare layer of --layer mode is: the panel, so that both modes measure one
 * geometry and the stride arithmetic below is the same number in both logs. */
enum { RWT_LAYER_W = 1125, RWT_LAYER_H = 2436 };

enum { RWT_FRAMES = 600, RWT_COUNTED_FRAMES = 3 };

/* ------------------------------------------------------------------ saying things out loud */

/*
 * The console, opened by name rather than trusted on fd 1.
 *
 * An application is spawned for SpringBoard and nothing promises it the descriptors init
 * gave launchd; src/inject_app.c already opens /dev/console for exactly this reason, and
 * init makes the device world-writable (src/init.c) so that a process of mobile's can.
 * Both are done here: our own lines go to the console directly, AND the console is put on
 * fd 1 and 2, because the driver underneath says everything it refuses with printf and
 * those lines are half the evidence.
 */
static int rwt_console = -1;

static void
rwt_open_console(void)
{
	rwt_console = open("/dev/console", O_WRONLY);
	if (rwt_console >= 0) {
		(void)dup2(rwt_console, 1);
		(void)dup2(rwt_console, 2);
	}
	/* unbuffered, so a line printed before a stall is a line in the log */
	setvbuf(stdout, NULL, _IONBF, 0);
}

static void
rwt_say(const char *format, ...)
{
	char line[1024];
	va_list arguments;
	int n;

	va_start(arguments, format);
	n = vsnprintf(line, sizeof(line) - 2, format, arguments);
	va_end(arguments);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	(void)write(rwt_console >= 0 ? rwt_console : 2, line, (size_t)n);
}

/*
 * The name of what is about to happen, before it happens. Everything in this program
 * between the device and the present can stall rather than fail -- a pipeline compile, a
 * dispatch the GPU never finishes, a present waiting on a transaction -- and a stall is
 * only readable if the log says which call was entered. The picture mode of
 * src/metal_drv_test.m cost one boot to learn that.
 */
static void
rwt_step(const char *what)
{
	rwt_say("triangle: step: %s", what);
}

/* --------------------------------------------------------------------------- the device */

/*
 * Whether Metal's own discovery is switched on in this boot, read out of the registry
 * BEFORE Metal is asked anything.
 *
 * Nothing branches on this -- the branch below is on the device Metal actually hands back,
 * which is the stronger test -- but it belongs in the log, because it is the one line that
 * says WHICH of the two roads this run was going to take and why. It is the same pair of
 * properties src/metal_props.c sets and the same way of reading them, so a disagreement
 * between the two programs is visible.
 */
static void
rwt_say_registry_switch(void)
{
	io_service_t service = IOServiceGetMatchingService(0,
	    IOServiceMatching("IOAcceleratorES"));
	CFTypeRef name, class_name;
	char name_buffer[128] = { 0 }, class_buffer[128] = { 0 };

	if (service == IO_OBJECT_NULL) {
		rwt_say("triangle: device: no IOAcceleratorES service in this kernel");
		return;
	}
	name = IORegistryEntryCreateCFProperty(service, CFSTR("MetalPluginName"), NULL, 0);
	class_name = IORegistryEntryCreateCFProperty(service, CFSTR("MetalPluginClassName"),
	    NULL, 0);
	if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID()) {
		CFStringGetCString(name, name_buffer, sizeof(name_buffer),
		    kCFStringEncodingUTF8);
	}
	if (class_name != NULL && CFGetTypeID(class_name) == CFStringGetTypeID()) {
		CFStringGetCString(class_name, class_buffer, sizeof(class_buffer),
		    kCFStringEncodingUTF8);
	}
	rwt_say("triangle: device: the registry switch is MetalPluginName %s, "
	    "MetalPluginClassName %s, so Metal's own discovery %s in this process",
	    name_buffer[0] != '\0' ? name_buffer : "(none)",
	    class_buffer[0] != '\0' ? class_buffer : "(none)",
	    name_buffer[0] != '\0' ? "WILL run and register a device"
				   : "will find nothing to register");
	if (name != NULL) {
		CFRelease(name);
	}
	if (class_name != NULL) {
		CFRelease(class_name);
	}
	IOObjectRelease(service);
}

/*
 * The device, by whichever of the two roads this boot leaves open -- and the ORDER IS THE
 * WHOLE POINT, because getting it wrong aborts the process.
 *
 * ASK METAL FIRST. If MTLCreateSystemDefaultDevice() answers a device, Metal's own
 * discovery has already registered one and this process must add NOTHING: it takes that
 * device and stops. Only when Metal answers nil does it walk discovery's steps 3, 4 and 5
 * itself and call MTLAddDevice.
 *
 * WHY, in Apple's own words. MTLCreateSystemDefaultDevice (0x181a34b3c) is a dispatch_sync
 * of one block (0x181a3442c), and that block is three lines:
 *
 *     [devices count] == 0  ->  leave the result nil and return
 *     [devices count] != 1  ->  MTLReportFailure(0, "MTLCreateSystemDefaultDevice_block_invoke",
 *                                  648, @"MTLCreateSystemDefaultDevice doesn't know what to
 *                                  do with more than one device")   -- which is abort()
 *     otherwise             ->  [[devices objectAtIndex:0] retain]
 *
 * `devices` is the global at 0x1e1efe9a0, and MTLAddDevice (0x181a1875c) appends to that
 * same array under that same queue (0x1e1efe9b0). So a process that lets discovery register
 * one device AND THEN registers one of its own has two, and the next
 * MTLCreateSystemDefaultDevice() in it dies -- not on anything to do with this port, but on
 * an assertion Apple wrote about a list it will not choose from.
 *
 * That is exactly what killed this program in logs/055044-run.log, the first boot that had
 * both a window server and the registry switch: the abort's own frame is
 * MTLCreateSystemDefaultDevice + 140, which is the return address of that dispatch_sync, and
 * the frame below it is the instruction after the SECOND of this file's two calls -- the one
 * that follows MTLAddDevice. Apple's daemons never aborted in that boot because none of them
 * calls MTLAddDevice: they let discovery register one and ask for it. This process was the
 * only one in the system that registered a device of its own.
 *
 * So the in-process road is kept -- it is what makes the result attributable on a boot where
 * discovery is off, since then the only Metal device in the system is the one made here --
 * but it is now the road taken ONLY when Metal has nothing.
 */
static id<MTLDevice>
rwt_make_device(void)
{
	rwt_say_registry_switch();

	/*
	 * Safe to ask: before this process has added anything, the array holds the devices
	 * discovery found, and there is one IOAcceleratorES service publishing one plug-in
	 * name, so that is nought or one -- never the two that aborts.
	 */
	id<MTLDevice> found = MTLCreateSystemDefaultDevice();

	if (found != nil) {
		const char *name = class_getName(object_getClass(found));

		rwt_say("triangle: device: Metal's own discovery gave %s, so this process adds "
		    "NOTHING and asks for no second device -- which is also the road a game "
		    "takes, and the reason this is the interesting boot", name);
		rwt_say("triangle: device: it calls itself \"%s\"", [[found name] UTF8String]);
		if (strcmp(name, "RedwoodMetalDevice") != 0) {
			rwt_say("triangle: device: and that is NOT RedwoodMetalDevice, so the "
			    "frames below belong to something else in this image");
		}
		return [found retain];
	}
	rwt_say("triangle: device: MTLCreateSystemDefaultDevice() = nil, so Metal has no "
	    "device of its own here and this process builds one -- steps 3, 4 and 5 by hand");

	rwt_step("opening the Metal plug-in bundle");
	NSBundle *bundle = [NSBundle bundleWithPath:
	    [NSString stringWithUTF8String:rwt_bundle]];

	if (bundle == nil) {
		rwt_say("triangle: NOT MEASURED: NSBundle would not open %s, so this image has "
		    "no Metal driver to ask", rwt_bundle);
		return nil;
	}
	Class cls = [bundle classNamed:@"RedwoodMetalDevice"];

	if (cls == Nil) {
		cls = [bundle principalClass];
	}
	rwt_say("triangle: device: bundle %s, class %s", rwt_bundle,
	    cls != Nil ? class_getName(cls) : "none -- the bundle gave up no class");
	if (cls == Nil) {
		return nil;
	}
	Class base = objc_getClass("_MTLDevice");

	rwt_say("triangle: device: step 4 isSubclassOfClass:_MTLDevice %s",
	    base != Nil && [cls isSubclassOfClass:base] ? "PASSES" : "FAILS");

	rwt_step("IOServiceGetMatchingService(IOAcceleratorES)");
	io_service_t service = IOServiceGetMatchingService(0,
	    IOServiceMatching("IOAcceleratorES"));

	if (service == IO_OBJECT_NULL) {
		rwt_say("triangle: NOT MEASURED: no IOAcceleratorES service in this kernel to "
		    "hand the device class");
		return nil;
	}
	rwt_step("-initWithAcceleratorPort: (this opens RedwoodGPU and the compiler library)");
	id device = ((id (*)(id, SEL, io_service_t))objc_msgSend)([cls alloc],
	    sel_getUid("initWithAcceleratorPort:"), service);

	rwt_say("triangle: device: step 5 gave %s",
	    device != nil ? class_getName(object_getClass(device)) : "nil");
	if (device == nil) {
		return nil;
	}

	/*
	 * MTLAddDevice: everything inherited from _MTLDevice answers out of ivars that only
	 * Metal's registration fills, and a CAMetalLayer sends the device at least one
	 * inherited selector (-supportsExtendedXR10Formats, for XR10 formats alone). It is
	 * guarded because its failure mode is an exception and not a return value.
	 */
	uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
	void (*add_device)(id) = (void (*)(id))(slide + MTL_ADD_DEVICE_UNSLID);

	rwt_step("MTLAddDevice");
	@try {
		add_device(device);
		rwt_say("triangle: device: MTLAddDevice returned");
	} @catch (NSException *exception) {
		rwt_say("triangle: FAILED: MTLAddDevice threw %s: %s",
		    [[exception name] UTF8String], [[exception reason] UTF8String]);
		return nil;
	}

	/* Safe for the reason the comment on this function gives: the array was EMPTY a moment
	 * ago -- that is what brought us down this road -- and MTLAddDevice has put exactly one
	 * device in it, so the count is 1 and not the 2 that aborts. */
	id<MTLDevice> system = MTLCreateSystemDefaultDevice();

	rwt_say("triangle: device: MTLCreateSystemDefaultDevice() = %s, built in this process",
	    system != nil ? class_getName(object_getClass(system)) : "nil");
	if (system == nil) {
		rwt_say("triangle: FAILED: MTLAddDevice took the device and Metal still has "
		    "none, so nothing below can be asked");
		return nil;
	}
	rwt_say("triangle: device: it calls itself \"%s\"", [[system name] UTF8String]);
	return [system retain];
}

/* ---------------------------------------------------------- the pipeline and its colour */

/*
 * The pipeline, built once and kept: one BGRA8Unorm colour attachment, Apple's
 * clearFragment01 as the fragment stage and NO vertex stage, which is what makes the
 * compiler library supply its own quad (and what makes a draw of three vertices a
 * triangle).
 */
static id<MTLRenderPipelineState>
rwt_make_pipeline(id<MTLDevice> device)
{
	NSError *error = nil;

	rwt_step("reading Apple's shader library off the disk");
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:rwt_metallib]];

	if (data == nil) {
		rwt_say("triangle: NOT MEASURED: cannot read %s", rwt_metallib);
		return nil;
	}
	rwt_say("triangle: pipeline: %s is %lu bytes", rwt_metallib,
	    (unsigned long)[data length]);

	dispatch_data_t wrapped = dispatch_data_create([data bytes], [data length], NULL,
	    DISPATCH_DATA_DESTRUCTOR_DEFAULT);

	rwt_step("-newLibraryWithData: (Apple's own metallib reader, above our device)");
	id<MTLLibrary> library = [device newLibraryWithData:wrapped error:&error];

	if (library == nil) {
		rwt_say("triangle: FAILED: newLibraryWithData: gave nil%s%s",
		    error != nil ? ": " : "",
		    error != nil ? [[error localizedDescription] UTF8String] : "");
		return nil;
	}
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:rwt_fragment]];

	if (fragment == nil) {
		rwt_say("triangle: FAILED: no %s in that library", rwt_fragment);
		return nil;
	}
	MTLRenderPipelineDescriptor *descriptor =
	    [[MTLRenderPipelineDescriptor alloc] init];

	[descriptor setVertexFunction:nil];     /* the library generates the quad stage */
	[descriptor setFragmentFunction:fragment];
	[[[descriptor colorAttachments] objectAtIndexedSubscript:0]
	    setPixelFormat:MTLPixelFormatBGRA8Unorm];

	rwt_step("-newRenderPipelineStateWithDescriptor: (this compiles AIR on this machine)");
	error = nil;
	id<MTLRenderPipelineState> pipeline =
	    [device newRenderPipelineStateWithDescriptor:descriptor error:&error];

	[descriptor release];
	rwt_say("triangle: pipeline: %s + the generated vertex stage gave %s%s%s",
	    rwt_fragment,
	    pipeline != nil ? class_getName(object_getClass(pipeline)) : "nil",
	    error != nil ? ", error: " : "",
	    error != nil ? [[error localizedDescription] UTF8String] : "");
	return pipeline;
}

/*
 * The constant buffer clearFragment01 reads. Its ClearFragmentInput is
 * { float4, int4, uint4 } and the float4 output is the first field, declared at buffer
 * index 14; the rest is zeroed because the integer fields belong to the targets this
 * shader does not write and memory nobody set would be a second unknown.
 *
 * The float4 is r, g, b, a in that order -- the attachment's format, not the shader,
 * decides that this ends up as b, g, r, a in memory -- and each channel is a whole eight
 * bits divided by 255, so the round trip through a unorm attachment is exact and a
 * difference of one is a difference.
 */
static id<MTLBuffer>
rwt_make_uniform(id<MTLDevice> device)
{
	id<MTLBuffer> buffer = [device newBufferWithLength:256 options:0];

	if (buffer == nil || [buffer contents] == NULL) {
		rwt_say("triangle: FAILED: no constant buffer for the shader to read");
		return nil;
	}
	float *floats = [buffer contents];

	memset(floats, 0, 256);
	floats[0] = rwt_triangle_bgra[2] / 255.0f;
	floats[1] = rwt_triangle_bgra[1] / 255.0f;
	floats[2] = rwt_triangle_bgra[0] / 255.0f;
	floats[3] = 1.0f;
	return buffer;
}

/* ------------------------------------------------------------------ reading the frame back */

/* Whether the pixel centre of (x, y) is inside the triangle through clip (-1,-1), (1,-1)
 * and (-1,1). The comparison is in integers, so it is the same answer the rasteriser's own
 * arithmetic gives and not a float that could land either way. */
static int
rwt_covered(unsigned x, unsigned y, unsigned width, unsigned height)
{
	return (unsigned long long)(2 * x + 1) * height <
	       (unsigned long long)(2 * y + 1) * width;
}

/* A pixel centre exactly ON the edge, which would make the pixel's colour the fill rule's
 * business rather than this file's. Counted so that a geometry where it happens says so
 * instead of showing up as pixels of the wrong colour. */
static int
rwt_on_edge(unsigned x, unsigned y, unsigned width, unsigned height)
{
	return (unsigned long long)(2 * x + 1) * height ==
	       (unsigned long long)(2 * y + 1) * width;
}

struct rwt_tally {
	unsigned long triangle, clear, neither, on_edge;
	unsigned long want_triangle, want_clear;
	unsigned long wrong;
	unsigned char first_wrong[4];
	unsigned first_wrong_x, first_wrong_y;
	int have_first_wrong;
};

/*
 * Every pixel of one view of the frame against the rule. `rows` is where row 0 starts and
 * `stride` is how far apart the rows are in THAT view -- which is the whole reason this is
 * a function and not a loop written twice: the IOSurface's stride is the kernel's
 * (aligned to 64) and -getBytes: is given this program's own, and a mismatch between them
 * is one of the ways an import can be wrong.
 */
static void
rwt_tally(const unsigned char *rows, size_t stride, unsigned width, unsigned height,
	  struct rwt_tally *tally)
{
	memset(tally, 0, sizeof(*tally));
	for (unsigned y = 0; y < height; y++) {
		const unsigned char *row = rows + (size_t)y * stride;

		for (unsigned x = 0; x < width; x++) {
			const unsigned char *pixel = row + (size_t)x * 4;
			int want = rwt_covered(x, y, width, height);
			const unsigned char *expect = want ? rwt_triangle_bgra
							   : rwt_clear_bgra;
			int is_triangle = pixel[0] == rwt_triangle_bgra[0] &&
					  pixel[1] == rwt_triangle_bgra[1] &&
					  pixel[2] == rwt_triangle_bgra[2];
			int is_clear = pixel[0] == rwt_clear_bgra[0] &&
				       pixel[1] == rwt_clear_bgra[1] &&
				       pixel[2] == rwt_clear_bgra[2];

			if (rwt_on_edge(x, y, width, height)) {
				tally->on_edge++;
			}
			if (is_triangle) {
				tally->triangle++;
			} else if (is_clear) {
				tally->clear++;
			} else {
				tally->neither++;
			}
			if (want) {
				tally->want_triangle++;
			} else {
				tally->want_clear++;
			}
			if (pixel[0] != expect[0] || pixel[1] != expect[1] ||
			    pixel[2] != expect[2]) {
				tally->wrong++;
				if (!tally->have_first_wrong) {
					tally->have_first_wrong = 1;
					tally->first_wrong_x = x;
					tally->first_wrong_y = y;
					memcpy(tally->first_wrong, pixel, 4);
				}
			}
		}
	}
}

static void
rwt_say_tally(const char *view, const struct rwt_tally *tally, unsigned width,
	      unsigned height)
{
	rwt_say("triangle: %s: of %lu pixels, %lu are orange, %lu are blue and %lu are "
	    "NEITHER; the rule says %lu orange and %lu blue, and %lu pixel centres sit on "
	    "the edge", view, (unsigned long)width * height, tally->triangle, tally->clear,
	    tally->neither, tally->want_triangle, tally->want_clear, tally->on_edge);
	if (tally->wrong == 0) {
		rwt_say("triangle: %s: 0 of %lu pixels are not the colour the rule says",
		    view, (unsigned long)width * height);
	} else {
		rwt_say("triangle: %s: WRONG: %lu of %lu pixels are not the colour the rule "
		    "says; the first is (%u,%u), which must be %s and reads b g r %u %u %u",
		    view, tally->wrong, (unsigned long)width * height, tally->first_wrong_x,
		    tally->first_wrong_y,
		    rwt_covered(tally->first_wrong_x, tally->first_wrong_y, width, height)
			? "orange" : "blue",
		    tally->first_wrong[0], tally->first_wrong[1], tally->first_wrong[2]);
	}
}

/*
 * Six pixels by name, printed as numbers next to what they must be. The count above says
 * whether the picture is right; these say WHAT IT IS when it is not, which a count cannot:
 * two of them straddle the hypotenuse eight hundredths of the panel apart, so a triangle
 * that is the wrong half, or mirrored, or shifted, moves a NAMED pixel and the log shows
 * which way.
 */
static unsigned
rwt_say_named_pixels(const char *view, const unsigned char *rows, size_t stride,
		     unsigned width, unsigned height, int loud)
{
	static const double points[6][2] = {
		{ 0.20, 0.80 }, { 0.80, 0.20 }, { 0.45, 0.55 },
		{ 0.55, 0.45 }, { 0.10, 0.95 }, { 0.95, 0.10 },
	};
	unsigned wrong = 0;

	for (unsigned i = 0; i < 6; i++) {
		unsigned x = (unsigned)(points[i][0] * width);
		unsigned y = (unsigned)(points[i][1] * height);
		const unsigned char *pixel;
		int want;
		const unsigned char *expect;

		if (x >= width) {
			x = width - 1;
		}
		if (y >= height) {
			y = height - 1;
		}
		pixel = rows + (size_t)y * stride + (size_t)x * 4;
		want = rwt_covered(x, y, width, height);
		expect = want ? rwt_triangle_bgra : rwt_clear_bgra;
		int agrees = pixel[0] == expect[0] && pixel[1] == expect[1] &&
			     pixel[2] == expect[2];

		if (!agrees) {
			wrong++;
		}
		/* Said in full on the frames that are counted in full, and after that only
		 * when it disagrees: six lines a frame for six hundred frames would bury the
		 * one frame that mattered under four thousand that did not. */
		if (loud || !agrees) {
			rwt_say("triangle: %s: pixel (%u,%u) must be %s (b g r %u %u %u) and is "
			    "%u %u %u: %s", view, x, y, want ? "orange" : "blue", expect[0],
			    expect[1], expect[2], pixel[0], pixel[1], pixel[2],
			    agrees ? "AGREES" : "WRONG");
		}
	}
	return wrong;
}

/* ----------------------------------------------------- the IOSurface behind the drawable */

/*
 * The drawable's own IOSurface, found through CAMetalDrawable's one ivar and validated
 * before it is believed.
 *
 * The driver's texture answers nil to -iosurface (redwoodmetal.m), so there is no way to
 * ask Metal for it; CoreAnimation keeps it in the private structure of the drawable. The
 * search below is over the ivars of the drawable's class, narrowed to the one whose type
 * encoding names _CAMetalDrawablePrivate, so nothing of another shape is ever
 * dereferenced -- and the structure is only trusted after the texture and the device it
 * holds turn out to be the two this program already has.
 */
static IOSurfaceRef
rwt_surface_of_drawable(id drawable, id texture, id device)
{
	unsigned count = 0;
	Ivar *ivars = class_copyIvarList(object_getClass(drawable), &count);
	IOSurfaceRef surface = NULL;

	for (unsigned i = 0; i < count && surface == NULL; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);
		char *private;

		if (type == NULL || strstr(type, "_CAMetalDrawablePrivate") == NULL) {
			continue;
		}
		private = *(char **)((char *)drawable + ivar_getOffset(ivars[i]));
		if (private == NULL) {
			continue;
		}
		if (*(id *)(private + RWT_PRIV_TEXTURE) != texture) {
			rwt_say("triangle: surface: %s holds a structure whose texture at "
			    "+%#x is not this drawable's, so its layout has moved and the "
			    "surface is not read from it", ivar_getName(ivars[i]),
			    RWT_PRIV_TEXTURE);
			continue;
		}
		if (*(id *)(private + RWT_PRIV_DEVICE) != device) {
			rwt_say("triangle: surface: %s holds the right texture at +%#x and "
			    "the wrong device at +%#x, so the layout is half right and "
			    "nothing is read from it", ivar_getName(ivars[i]),
			    RWT_PRIV_TEXTURE, RWT_PRIV_DEVICE);
			continue;
		}
		surface = *(IOSurfaceRef *)(private + RWT_PRIV_SURFACE);
		/* once: the structure is the same structure every frame, and the only reason
		 * to say it at all is that the offsets were validated rather than assumed */
		static int said;

		if (!said) {
			said = 1;
			rwt_say("triangle: surface: through ivar %s, whose structure holds this "
			    "drawable's own texture and device where 10.46 says they are",
			    ivar_getName(ivars[i]));
		}
	}
	free(ivars);
	if (surface == NULL) {
		static int said_not;

		if (!said_not) {
			said_not = 1;
			rwt_say("triangle: NOT MEASURED: the drawable's IOSurface could not be "
			    "reached, so the frame is read back through -getBytes: alone");
		}
	}
	return surface;
}

/* --------------------------------------------------------------------------- one frame */

struct rwt_frame_state {
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLRenderPipelineState> pipeline;
	id<MTLBuffer> uniform;
	CAMetalLayer *layer;
	unsigned frame;
	unsigned long failures;
	int present;                    /* 0 in --layer mode until the very last frame */
	unsigned char *scratch;         /* for -getBytes:, one whole frame */
	size_t scratch_bytes;
};

/*
 * Memory for the -getBytes: view of a frame, taken from the size the DRAWABLE turned out to
 * be and not from the size this file guessed.
 *
 * Lazily, and that is the point: in the application the layer has no drawableSize until
 * nextDrawable has worked one out ([layer size] x [layer contentsScale], docs/metal.md
 * 10.46), so a buffer allocated before the first frame would be allocated against a number
 * nobody had measured -- and a buffer too small is a view silently not read.
 *
 * Absent is not a failure: the surface view stands on its own, and the log says which views
 * were read.
 */
static void
rwt_want_scratch(struct rwt_frame_state *state, unsigned width, unsigned height)
{
	size_t want = (size_t)width * height * 4;

	if (state->scratch != NULL && state->scratch_bytes >= want) {
		return;
	}
	free(state->scratch);
	state->scratch = malloc(want);
	state->scratch_bytes = state->scratch != NULL ? want : 0;
	if (state->scratch == NULL) {
		rwt_say("triangle: NOT MEASURED: no memory for a %ux%u copy of the frame, so "
		    "-getBytes: is not asked", width, height);
	}
}

/*
 * One frame, end to end, and every part of it said out loud on the first few.
 *
 * The order is Metal's and the reading is deliberately BEFORE the present: the surface
 * belongs to this process until it is handed to the image queue, so reading it first
 * measures what the GPU wrote and not what a compositor may have done with it afterwards.
 */
static void
rwt_frame(struct rwt_frame_state *state)
{
	int loud = state->frame < RWT_COUNTED_FRAMES;

	state->frame++;
	@autoreleasepool {
		if (loud) {
			rwt_step("-[CAMetalLayer nextDrawable] (the first caller this driver's "
				 "-newTextureWithDescriptor:iosurface:plane: has ever had)");
		}
		id<CAMetalDrawable> drawable = [state->layer nextDrawable];

		if (drawable == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: nextDrawable gave NIL. Either the driver "
			    "refused the texture -- look for \"redwoodmetal: no drawable:\" "
			    "just above -- or all %lu drawables of the pool are still in "
			    "flight, which means the presents are not completing",
			    state->frame,
			    (unsigned long)[state->layer maximumDrawableCount]);
			return;
		}
		id<MTLTexture> texture = [drawable texture];

		if (texture == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: the drawable is %s and its "
			    "-texture is nil, which is what CoreAnimation does with a nil "
			    "from the device: it does not test it (docs/metal.md 10.46)",
			    state->frame, class_getName(object_getClass(drawable)));
			return;
		}
		unsigned width = (unsigned)[texture width];
		unsigned height = (unsigned)[texture height];

		if (loud) {
			rwt_say("triangle: frame %u: drawable %s, texture %s, %ux%u, pixel "
			    "format %lu, usage %lu, isDrawable %s", state->frame,
			    class_getName(object_getClass(drawable)),
			    class_getName(object_getClass(texture)), width, height,
			    (unsigned long)[texture pixelFormat],
			    (unsigned long)[texture usage],
			    [texture respondsToSelector:sel_getUid("isDrawable")] &&
			    ((BOOL (*)(id, SEL))objc_msgSend)(texture,
				sel_getUid("isDrawable")) ? "YES" : "NO");
		}
		if (width == 0 || height == 0) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: the drawable's texture is %ux%u",
			    state->frame, width, height);
			return;
		}
		rwt_want_scratch(state, width, height);

		/* The surface, found before anything is drawn, so that a frame which draws
		 * and cannot be read back is told from a frame that never drew. */
		IOSurfaceRef surface = rwt_surface_of_drawable(drawable, texture,
		    state->device);
		size_t surface_stride = 0;
		const unsigned char *surface_rows = NULL;

		if (surface != NULL) {
			surface_stride = IOSurfaceGetBytesPerRow(surface);
			surface_rows = IOSurfaceGetBaseAddress(surface);
			if (loud) {
				rwt_say("triangle: frame %u: its IOSurface is id %u, %zux%zu, "
				    "%zu bytes a row, %zu bytes allocated, base %p",
				    state->frame, (unsigned)IOSurfaceGetID(surface),
				    IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
				    surface_stride, IOSurfaceGetAllocSize(surface),
				    (void *)surface_rows);
			}
			/* The one thing that would make reading it a wild read rather than a
			 * measurement, checked rather than assumed. */
			if (surface_rows == NULL ||
			    IOSurfaceGetWidth(surface) != width ||
			    IOSurfaceGetHeight(surface) != height ||
			    surface_stride < (size_t)width * 4 ||
			    IOSurfaceGetAllocSize(surface) < surface_stride * height) {
				rwt_say("triangle: NOT MEASURED: that surface does not describe "
				    "the texture (%ux%u, %zu bytes a row, %zu allocated), so it "
				    "is not read", width, height, surface_stride,
				    surface != NULL ? IOSurfaceGetAllocSize(surface) : 0);
				surface_rows = NULL;
			}
		}

		/* ---- the pass ---- */
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor
		    renderPassDescriptor];
		MTLRenderPassColorAttachmentDescriptor *attachment =
		    [[pass colorAttachments] objectAtIndexedSubscript:0];

		[attachment setTexture:texture];
		[attachment setLoadAction:MTLLoadActionClear];
		[attachment setClearColor:MTLClearColorMake(rwt_clear_bgra[2] / 255.0,
		    rwt_clear_bgra[1] / 255.0, rwt_clear_bgra[0] / 255.0, 1.0)];
		[attachment setStoreAction:MTLStoreActionStore];

		if (loud) {
			rwt_step("asking the queue for a command buffer");
		}
		id<MTLCommandBuffer> command = [state->queue commandBuffer];

		if (command == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: the queue gave no command buffer",
			    state->frame);
			return;
		}
		if (loud) {
			rwt_step("asking it for a render encoder over the drawable");
		}
		id<MTLRenderCommandEncoder> encoder =
		    [command renderCommandEncoderWithDescriptor:pass];

		if (encoder == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: no render encoder over the "
			    "drawable's texture", state->frame);
			return;
		}
		[encoder setRenderPipelineState:state->pipeline];
		/* at every index up to 16: the shader's buffer is declared at 14, and a
		 * driver that bound only index 0 would leave it reading nothing */
		for (unsigned i = 0; i < 16; i++) {
			[encoder setFragmentBuffer:state->uniform offset:0 atIndex:i];
		}
		if (loud) {
			rwt_step("encoding THREE vertices of the generated quad, which is one "
				 "triangle");
		}
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[encoder endEncoding];

		/*
		 * presentDrawable: BEFORE commit, because this driver does its work in
		 * -commit and Apple's -[_MTLCommandBuffer presentDrawable:] puts the present
		 * on a scheduled handler which -commit then delivers (docs/metal.md 10.46).
		 */
		if (state->present) {
			if (loud) {
				rwt_step("-[MTLCommandBuffer presentDrawable:] (Apple's own, "
					 "which hangs the present on a scheduled handler)");
			}
			[command presentDrawable:drawable];
		}
		if (loud) {
			rwt_step("commit (this driver draws HERE, not at the wait)");
		}
		[command commit];
		if (loud) {
			rwt_step("waitUntilCompleted");
		}
		[command waitUntilCompleted];

		NSUInteger status = [command status];

		if (status != RW_CB_COMPLETED) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: command buffer status %lu%s%s",
			    state->frame, (unsigned long)status,
			    [command error] != nil ? ": " : "",
			    [command error] != nil
				? [[[command error] localizedDescription] UTF8String] : "");
			return;
		}
		if (loud) {
			rwt_say("triangle: frame %u: the GPU finished the pass (status %lu)",
			    state->frame, (unsigned long)status);
		}

		/* ---- what is in it, through two different views of the same memory ---- */
		unsigned named_wrong = 0;

		if (surface_rows != NULL) {
			uint32_t seed = 0;
			IOReturn locked = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, &seed);

			if (loud) {
				rwt_say("triangle: frame %u: IOSurfaceLock(read only) = %#x",
				    state->frame, locked);
			}
			named_wrong += rwt_say_named_pixels("surface check", surface_rows,
			    surface_stride, width, height, loud);
			if (loud) {
				struct rwt_tally tally;

				rwt_tally(surface_rows, surface_stride, width, height, &tally);
				rwt_say_tally("surface count", &tally, width, height);
				state->failures += tally.wrong != 0;
			}
			if (locked == kIOReturnSuccess) {
				IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, &seed);
			}
		}
		if (state->scratch != NULL &&
		    state->scratch_bytes >= (size_t)width * height * 4) {
			if (loud) {
				rwt_step("-getBytes: the whole frame, the GPU driver's own view "
					 "of the same resource");
			}
			memset(state->scratch, 0xa5, (size_t)width * height * 4);
			[texture getBytes:state->scratch
			      bytesPerRow:(size_t)width * 4
			       fromRegion:MTLRegionMake2D(0, 0, width, height)
			      mipmapLevel:0];
			named_wrong += rwt_say_named_pixels("getBytes check", state->scratch,
			    (size_t)width * 4, width, height, loud);
			if (loud) {
				struct rwt_tally tally;

				rwt_tally(state->scratch, (size_t)width * 4, width, height,
				    &tally);
				rwt_say_tally("getBytes count", &tally, width, height);
				state->failures += tally.wrong != 0;
			}
		}
		state->failures += named_wrong;
		/* Every twentieth frame, and every frame that went wrong: a frame is the same
		 * picture as the one before it, so what a per-frame line is actually for is the
		 * frame NUMBER -- proof that nextDrawable is still answering, which is what
		 * stops if the presents never complete. */
		if (loud || named_wrong != 0 || state->frame % 20 == 0) {
			rwt_say("triangle: frame %u: %u of the named pixels disagree; %lu "
			    "failures so far", state->frame, named_wrong, state->failures);
		}
	}
}

/* ---------------------------------------------------------------- the common setting-up */

/* The device, the queue, the pipeline and the constant buffer, which are the same in both
 * modes. 0, or a sentence already printed. */
static int
rwt_prepare(struct rwt_frame_state *state)
{
	state->device = rwt_make_device();
	if (state->device == nil) {
		return -1;
	}
	rwt_step("-newCommandQueue");
	state->queue = [[state->device newCommandQueue] retain];
	if (state->queue == nil) {
		rwt_say("triangle: FAILED: the device gave no command queue");
		return -1;
	}
	state->pipeline = [rwt_make_pipeline(state->device) retain];
	if (state->pipeline == nil) {
		return -1;
	}
	state->uniform = [rwt_make_uniform(state->device) retain];
	if (state->uniform == nil) {
		return -1;
	}
	return 0;
}

static void
rwt_say_layer(CAMetalLayer *layer)
{
	CGSize size = [layer drawableSize];

	rwt_say("triangle: layer: drawableSize %g x %g, contentsScale %g, bounds %g x %g, "
	    "pixelFormat %lu, framebufferOnly %s, maximumDrawableCount %lu, device %s",
	    size.width, size.height, [layer contentsScale],
	    [layer bounds].size.width, [layer bounds].size.height,
	    (unsigned long)[layer pixelFormat],
	    [layer framebufferOnly] ? "YES (so the drawable's usage is RenderTarget alone)"
				    : "NO",
	    (unsigned long)[layer maximumDrawableCount],
	    [layer device] != nil ? class_getName(object_getClass([layer device])) : "nil");
}

/* ------------------------------------------------------------------------- --layer mode */

/*
 * The same frame without UIKit: a CAMetalLayer nobody can see, of the panel's size.
 *
 * It cannot answer whether a frame reaches the screen -- there is no window and no scene --
 * and it does not pretend to: the present is sent ONCE, at the very end, after every number
 * above it has been printed, so that a present which kills a process with no window server
 * behind it costs nothing that was already measured.
 */
static int
rwt_layer_mode(void)
{
	struct rwt_frame_state state;

	memset(&state, 0, sizeof(state));
	if (rwt_prepare(&state) != 0) {
		return 1;
	}
	rwt_step("a bare CAMetalLayer");
	state.layer = [[CAMetalLayer alloc] init];
	if (state.layer == nil) {
		rwt_say("triangle: FAILED: no CAMetalLayer");
		return 1;
	}
	[state.layer setDevice:state.device];
	[state.layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
	[state.layer setFramebufferOnly:YES];
	[state.layer setBounds:CGRectMake(0, 0, RWT_LAYER_W, RWT_LAYER_H)];
	[state.layer setDrawableSize:CGSizeMake(RWT_LAYER_W, RWT_LAYER_H)];
	rwt_say_layer(state.layer);

	rwt_frame(&state);

	/* and now, last of all, the one present this mode can try */
	rwt_say("triangle: layer: the numbers above are complete; the present below is the "
	    "only thing after them, and it is sent with no window and no scene behind it");
	state.present = 1;
	rwt_frame(&state);

	rwt_say("triangle: done: --layer mode drew %u frames with %lu failures",
	    state.frame, state.failures);
	return state.failures != 0 ? 1 : 0;
}

/* ---------------------------------------------------------------------- the application */

@interface RWTriangleView : UIView
@end

@implementation RWTriangleView

/* The whole reason this class exists: the view's layer IS the CAMetalLayer, so UIKit gives
 * it the window's geometry and the screen's contentsScale, and the window server composites
 * it as it composites any view. */
+ (Class)layerClass
{
	return [CAMetalLayer class];
}

@end

@interface RWTriangleController : UIViewController
@end

static struct rwt_frame_state rwt_app_state;
static NSTimer *rwt_timer;

@implementation RWTriangleController

- (void)loadView
{
	RWTriangleView *view = [[RWTriangleView alloc] initWithFrame:
	    [[UIScreen mainScreen] bounds]];

	/* black, and black is a third colour with a meaning of its own: it is what the panel
	 * shows when nothing was ever presented, and it is neither of the two the frame is
	 * made of */
	[view setBackgroundColor:[UIColor blackColor]];
	[self setView:view];
	[view release];
}

- (BOOL)prefersStatusBarHidden
{
	return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden
{
	return YES;
}

/*
 * The frames start here and not in -didFinishLaunching: a drawable's size is the layer's,
 * and the layer has no size until UIKit has laid the window out. Everything runs on the
 * main thread on purpose -- -[CAMetalDrawable present] ends in CA::Transaction::add_command,
 * so the present is carried out by the run loop's own transaction flush, and a frame drawn
 * off the main thread would be a frame nothing commits.
 */
- (void)viewDidAppear:(BOOL)animated
{
	[super viewDidAppear:animated];
	if (rwt_timer != nil) {
		return;
	}
	/*
	 * No device or no pipeline: the window stays, and what the panel then shows is the
	 * layer's black. That is a measurement of its own -- the application launched, so
	 * questions 1 and 3 are separable from question 2 -- and it is said here rather than
	 * discovered from a nil pipeline bound to an encoder.
	 */
	if (rwt_app_state.device == nil || rwt_app_state.pipeline == nil) {
		rwt_say("triangle: NOT MEASURED: no %s in this process, so no frame is drawn "
		    "and the panel will be the layer's black. The application itself launched, "
		    "which is the one thing this still says",
		    rwt_app_state.device == nil ? "Metal device" : "render pipeline");
		return;
	}
	CAMetalLayer *layer = (CAMetalLayer *)[[self view] layer];

	[layer setDevice:rwt_app_state.device];
	[layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
	[layer setFramebufferOnly:YES];
	/* drawableSize is left to CoreAnimation: nextDrawable works it out as
	 * [layer size] x [layer contentsScale] (docs/metal.md 10.46), which is what an
	 * application gets and therefore what is worth measuring */
	rwt_say_layer(layer);
	rwt_app_state.layer = layer;
	rwt_app_state.present = 1;

	rwt_frame(&rwt_app_state);
	rwt_timer = [[NSTimer scheduledTimerWithTimeInterval:0.5
						      target:self
						    selector:@selector(tick:)
						    userInfo:nil
						     repeats:YES] retain];
}

/*
 * A frame every half second, and not because anything animates: the picture is identical
 * every time, so a screenshot taken at any moment is the same photograph. What the repeat
 * buys is the one failure a single frame cannot show -- if the presents never complete, the
 * pool of two or three drawables empties and nextDrawable starts answering nil, which this
 * log then says by frame number.
 */
- (void)tick:(NSTimer *)timer
{
	if (rwt_app_state.frame >= RWT_FRAMES) {
		[timer invalidate];
		rwt_say("triangle: done: %u frames, %lu failures. The picture stays on the "
		    "panel: the last surface presented is the one the window server holds",
		    rwt_app_state.frame, rwt_app_state.failures);
		return;
	}
	rwt_frame(&rwt_app_state);
}

@end

@interface RWTriangleDelegate : NSObject <UIApplicationDelegate>
@property (nonatomic, retain) UIWindow *window;
@end

@implementation RWTriangleDelegate

@synthesize window = _window;

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)options
{
	(void)application;
	(void)options;
	rwt_say("triangle: app: didFinishLaunchingWithOptions, so UIKit is up in this "
	    "process");
	if (rwt_prepare(&rwt_app_state) != 0) {
		rwt_say("triangle: app: no device, so the window is made all the same and the "
		    "panel will show the layer's black: that black is the answer");
	}
	UIWindow *w = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];

	[w setRootViewController:[[[RWTriangleController alloc] init] autorelease]];
	[self setWindow:w];
	[w makeKeyAndVisible];
	[w release];
	rwt_say("triangle: app: the window is key and visible, %g x %g points",
	    [[self window] bounds].size.width, [[self window] bounds].size.height);
	return YES;
}

@end

/* ------------------------------------------------------------------------------- main */

int
main(int argc, char *argv[])
{
	int layer_mode = argc > 1 && strcmp(argv[1], "--layer") == 0;

	rwt_open_console();
	rwt_say("triangle: start: RedwoodTriangle, pid %d, uid %d, mode %s", (int)getpid(),
	    (int)getuid(), layer_mode ? "--layer (no UIKit, no window)" : "application");
	if (layer_mode) {
		int bad;

		@autoreleasepool {
			bad = rwt_layer_mode();
		}
		return bad;
	}
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil,
		    NSStringFromClass([RWTriangleDelegate class]));
	}
}
