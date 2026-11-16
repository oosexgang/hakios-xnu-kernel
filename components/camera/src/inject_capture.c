/*
 * libhakios-capture.dylib: inside cameracaptured, iOS 27's camera daemon, the flashlight.
 *
 * iOS 27 has no mediaserverd: the capture stack (CMCapture) runs in cameracaptured, and so does the
 * flashlight's server (com.apple.coremedia.flashlight). Control Center's button and AVFlashlight reach
 * it through FigFlashlightRemote; the server makes a FigFlashlightLocal for each client, which asks
 * the capture stack's device vendor (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the
 * back camera's device and stream and sets the stream's "TorchLevel" (0 to 1). There is no camera of
 * Apple's here: no vendor, no device, and the flashlight is never available.
 *
 * Where the kernel has a flashlight (boot-arg torch=3, bsd/dev/board/torch.c: /dev/hakios-torch takes a
 * level of 0 to 1000), each FigFlashlightLocal is given a device and a stream of ours
 * (-_createDeviceAndStream), whose "TorchLevel" goes to the kernel; it says it is available
 * (-isAvailable), and the capture stack's thermal monitor does not take the level down
 * (-[FigCaptureThermalMonitor maxTorchLevel] 1, -[FigFlashlightLocal isOverheated] NO): the kernel's
 * driver guards the light itself -- Android's current at most, a time limit, the module's fault and
 * temperature flags, the PMIC's and the battery's temperature. Everything else of FigFlashlightLocal is
 * Apple's: the levels, the notifications its clients get, the power-off.
 *
 * Methods are found by name and checked by their type encoding; a build where they differ is left
 * alone and says so. Where there is no /dev/hakios-torch nothing is changed. Messages the capture stack
 * sends our device or stream that they do not have are answered with 0 and named once in the log.
 *
 * The camera (only where the kernel has one: cam=5, /dev/hakios-camera): the capture stack's device vendor
 * is made with the create function of our capture device (components/camera/src/camera_device.c, its formats N104's
 * with HAKIOS_IOS27) in place of the one iOS looks for among the iPhones' ISP drivers in IOKit
 * (FigCaptureGetCaptureDeviceCreateFunction, which finds none here and fails after 8 s): the factory
 * +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:] is given it when it
 * is given none. What the capture graph asks of hardware this phone has none of is refused or passed by,
 * as on 16.5 (see each below): video stabilization, the microphone of a capture session, the photograph's
 * SIS bracket and noise reduction -- and on iOS 27 its unified bracketing (NRF), its HDR and its packed 10-bit
 * buffers, which leave the photograph the pipeline 16.5 had; and the software HEVC encoder is registered for HEIC
 * photographs.
 *
 * (iOS 16.5 did all this in mediaserverd, by the addresses of the code it replaced:
 * userland/userland/src/inject_media.c.)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int torch = -1;                  /* /dev/hakios-torch */
static Class device_class, stream_class;
static Ivar device_ivar, stream_ivar;
static unsigned level_said;

static void
say(const char *format, ...) __printflike(1, 2);
static void preview_path(void);

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	if (n > 0) {
		write(2, line, (size_t)n);
	}
}

/* a method's type encoding without the offsets ("B16@0:8" -> "B@:") */
static bool
types_are(Method m, const char *want)
{
	const char *t = m ? method_getTypeEncoding(m) : NULL;
	char out[48];
	size_t n = 0;

	if (t == NULL) {
		return false;
	}
	for (; *t != '\0' && n + 2 < sizeof(out); t++) {
		if (*t == '{') {                /* a structure by its braces only: "^{opaqueCMSampleBuffer=}" -> "^{}" */
			int depth = 1;

			out[n++] = '{';
			while (depth > 0 && t[1] != '\0') {
				t++;
				depth += *t == '{' ? 1 : *t == '}' ? -1 : 0;
			}
			out[n++] = '}';
		} else if (*t < '0' || *t > '9') {
			out[n++] = *t;
		}
	}
	out[n] = '\0';
	return strcmp(out, want) == 0;
}

static void
torch_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || level_said < 40) {
		level_said++;
		say("hakios: cameracaptured: the flashlight at %u of 1000%s\n", level,
		    written == (ssize_t)sizeof(level) ? "" : " (NOT TAKEN by /dev/hakios-torch)");
	}
}

/* ---- our device and stream ------------------------------------------------------------------------ */

/* the stream's -supportedProperties: none of the optional ones FigFlashlightLocal sets up */
static id
stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID() && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		torch_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/* anything else asked of our device or stream: 0, and its name once */
static long
answer_zero(id self, SEL selector)
{
	static char said[16][96];
	static unsigned count;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	const char *name = sel_getName(selector);
	bool known = false;

	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < count && !known; i++) {
		known = strncmp(said[i], name, sizeof(said[i]) - 1) == 0;
	}
	if (!known && count < 16) {
		strlcpy(said[count++], name, sizeof(said[0]));
		say("hakios: cameracaptured: the flashlight's %s was asked -%s: answered 0\n", class_getName(object_getClass(self)), name);
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* +resolveInstanceMethod: of our classes: every selector they lack answers 0 (answer_zero) */
static BOOL
resolve(Class cls, __unused SEL cmd, SEL selector)
{
	return class_addMethod(cls, selector, (IMP)answer_zero, "q@:");
}

static Class
make_class(const char *name, bool stream)
{
	Class made = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	if (made == NULL) {
		return objc_getClass(name);
	}
	if (stream) {
		class_addMethod(made, sel_registerName("supportedProperties"), (IMP)stream_properties, "@@:");
		class_addMethod(made, sel_registerName("setProperty:value:"), (IMP)stream_set, "i@:@@");
	}
	class_addMethod(object_getClass((id)made), sel_registerName("resolveInstanceMethod:"), (IMP)resolve, "B@::");
	objc_registerClassPair(made);
	return made;
}

/* ---- FigFlashlightLocal --------------------------------------------------------------------------- */

typedef int (*int_fn)(id, SEL);
static int_fn original_create;

/* -[FigFlashlightLocal _createDeviceAndStream]: ours, set where Apple's would be */
static int
create_device_and_stream(id self, SEL selector)
{
	id device, stream;

	if (torch < 0) {
		return original_create(self, selector);
	}
	device = class_createInstance(device_class, 0);
	stream = class_createInstance(stream_class, 0);
	/*
	 * Retained there as Apple's own would be: object_setIvar() stores without retaining where the runtime
	 * does not know the ivar to be strong -- it did not here -- and the release below then freed our stream
	 * under FigFlashlightLocal: cameracaptured died in objc_msgSend at its first message to it (far 0x10, the
	 * freed object's class zeroed).
	 */
	object_setIvarWithStrongDefault(self, device_ivar, device);
	object_setIvarWithStrongDefault(self, stream_ivar, stream);
	((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("release"));
	((void (*)(id, SEL))objc_msgSend)(stream, sel_registerName("release"));
	say("hakios: cameracaptured: a flashlight client gets the kernel's flashlight\n");
	return 0;
}

static BOOL
is_available(__unused id self, __unused SEL selector)
{
	return YES;
}

static BOOL
is_overheated(__unused id self, __unused SEL selector)
{
	return NO;
}

static float
max_torch_level(__unused id self, __unused SEL selector)
{
	return 1.0f;
}

static bool
replace(const char *class_name, const char *selector, const char *types, IMP with, IMP *original)
{
	Class cls = objc_getClass(class_name);
	Method m = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (!types_are(m, types)) {
		say("hakios: cameracaptured: no -[%s %s] of type %s: left alone\n", class_name, selector, types);
		return false;
	}
	if (original != NULL) {
		*original = method_getImplementation(m);
	}
	method_setImplementation(m, with);
	return true;
}

static void
flashlight_init(void)
{
	Class local = objc_getClass("FigFlashlightLocal");
	IMP was;

	device_ivar = local ? class_getInstanceVariable(local, "_device") : NULL;
	stream_ivar = local ? class_getInstanceVariable(local, "_stream") : NULL;
	if (device_ivar == NULL || stream_ivar == NULL) {
		say("hakios: cameracaptured: FigFlashlightLocal has no _device and _stream: the flashlight is left alone\n");
		return;
	}
	device_class = make_class("HakiosFlashlightDevice", false);
	stream_class = make_class("HakiosFlashlightStream", true);
	if (!replace("FigFlashlightLocal", "_createDeviceAndStream", "i@:", (IMP)create_device_and_stream, &was)) {
		return;
	}
	original_create = (int_fn)was;
	replace("FigFlashlightLocal", "isAvailable", "B@:", (IMP)is_available, NULL);
	replace("FigFlashlightLocal", "isOverheated", "B@:", (IMP)is_overheated, NULL);
	replace("FigCaptureThermalMonitor", "maxTorchLevel", "f@:", (IMP)max_torch_level, NULL);
	say("hakios: cameracaptured: the flashlight is the kernel's (/dev/hakios-torch)\n");
}

/* ---- the camera ----------------------------------------------------------------------------------- */

static bool camera;                     /* /dev/hakios-camera is there */
extern OSStatus hakios_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, void **out);

typedef id (*factory_fn)(Class, SEL, void *);
static factory_fn original_factory;

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:]: ours where none is given */
static id
vendor_factory(Class self, SEL selector, void *create)
{
	static bool said;

	if (create == NULL && camera) {
		create = (void *)hakios_camera_device_create;
		if (!said) {
			said = true;
			say("hakios: cameracaptured: the camera's device vendor is made with our capture device (camera_device.c)\n");
		}
	}
	return original_factory(self, selector, create);
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS pipeline needs the ISP's
 * processing session, and without it the graph is torn down -- the preview too. 0 is "none", in the
 * method the stack computes it by and in the type the application's configuration brings.
 */
static int
no_stabilization(__unused id self, __unused SEL selector)
{
	return 0;
}

/*
 * The microphone, which the capture stack has none of here: a session's audio source node's start comes
 * back with 'nope' (the HAL's "no such thing"), and the graph took that for the whole session's error.
 * That one error is swallowed: the node gives no sound and the rest -- the preview, the photograph -- runs.
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

typedef BOOL (*start_fn)(id, SEL, id *);
static start_fn original_audio_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = original_audio_start(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;
	if (!said) {
		said = true;
		say("hakios: cameracaptured: no capture audio on this phone: the session's audio source gives nothing\n");
	}
	return YES;
}

/* A photograph's quality prioritization, "speed" at the most: one frame, no SIS bracket nothing here fuses */
typedef int (*int_getter)(id, SEL);
static int_getter original_quality;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = original_quality(self, selector);

	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction and fusion scheme, 2 at the most: N104's plist says 4 (unified bracketing), and
 * the graph had FigCaptureStillImageUnifiedBracketingSinkPipeline make its BWUBNode, whose processor coordinator
 * is NRF's: NRF's DenoiseFusePipeline needs kernels with imageblock inputs (adjustEvmPyramidColor), which a GPU
 * without tile shaders has no way to run, so the coordinator failed when the graph went live, and every
 * photograph with it (-16808, "Processor coordinator setup previously failed"; logs/235342-run.log in
 * hakios-sm7325). Scheme 2 is what iOS 16.5 had for this phone (D22's plist): FigCaptureStillImageSinkPipeline,
 * whose noise reduction is BWGNRNode -- passed by below, as it was there.
 */
static int_getter original_scheme;

static int
fusion_scheme(id self, SEL selector)
{
	int asked = original_scheme(self, selector);
	static bool said;

	if (asked > 2 && !said) {
		said = true;
		say("hakios: cameracaptured: a photograph's noise reduction and fusion scheme %d taken as 2 (GNR, as on 16.5)\n",
		    asked);
	}
	return asked > 2 ? 2 : asked;
}

/*
 * N104's formats give their photographs as packed 10-bit buffers ('pf20', "UsesPacked10BitFirmwareStillImage
 * OutputPixelFormat"), which the camera's ISP here does not write (camera_device.c: 420f, 420v, BGRA): the
 * photograph's buffer stayed empty ("StillImage: 4224x2376 pf20 buffers are not for this ISP"). 8-bit stills.
 */
static BOOL
no_packed_stills(__unused id self, __unused SEL selector)
{
	static bool said;

	if (!said) {
		said = true;
		say("hakios: cameracaptured: a photograph's buffers are 8-bit, not packed 10-bit\n");
	}
	return NO;
}

/*
 * No HDR photographs: with scheme 2 the still image pipeline put a BWHDRNode before the noise reduction wherever
 * the format says HDR (N104's do), and the node's sample buffer processor is the iPhone's, which is not here:
 * -[BWHDRNode _setupSampleBufferProcessor] read through NULL when the graph went live, and cameracaptured died of
 * it at every start of the camera (SIGSEGV, far 8; logs/092322-run.log in hakios-sm7325). A bracket of exposures
 * has nothing to fuse it here anyway. (The pipeline's configuration takes it with a direct setter, -setHdrSupported:,
 * which the runtime does not see: the format's answer is where it comes from.)
 */
static BOOL
no_hdr(__unused id self, __unused SEL selector)
{
	static bool said;

	if (!said) {
		said = true;
		say("hakios: cameracaptured: no HDR photographs (no HDR processor here)\n");
	}
	return NO;
}

/* A photograph's noise reduction (BWGNRNode) is the iPhone's and never hands the picture back: passed by */
static void
gnr_render(id self, __unused SEL selector, void *sample_buffer, __unused id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));

	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

/* No sample buffer at all (the silent audio source's) is not passed on: nodes take it into arrays and abort */
typedef void (*emit_fn)(id, SEL, void *);
static emit_fn original_emit;

static void
emit_sample_buffer(id self, SEL selector, void *sample_buffer)
{
	if (sample_buffer != NULL) {
		original_emit(self, selector, sample_buffer);
	}
}

/* The portrait filters' prewarming: nobody takes a portrait here */
static void
no_prewarm(__unused id self, __unused SEL selector, __unused unsigned version, __unused BOOL styles)
{
}

/*
 * The photograph's encoder: HEIC is HEVC, and VideoToolbox here has only the iPhone's hardware HEVC encoders.
 * Apple's software codec (VCPHEVC.videocodec) has an encoder its own registration leaves out; it is
 * registered here, as the codec registers its decoder.
 */
static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("hakios: cameracaptured: no software HEVC encoder to register\n");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	int status = register_encoder('hvc1', info, create);

	CFRelease(info);
	say("hakios: cameracaptured: the software HEVC encoder registered for 'hvc1' (%d)\n", status);
}

static bool
replace_class_method(const char *class_name, const char *selector, const char *types, IMP with, IMP *original)
{
	Class cls = objc_getClass(class_name);
	Method m = cls ? class_getClassMethod(cls, sel_registerName(selector)) : NULL;

	if (!types_are(m, types)) {
		say("hakios: cameracaptured: no +[%s %s] of type %s: left alone\n", class_name, selector, types);
		return false;
	}
	if (original != NULL) {
		*original = method_getImplementation(m);
	}
	method_setImplementation(m, with);
	return true;
}

static void
camera_init(void)
{
	IMP was;

	if (!replace_class_method("BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:",
	    "@@:^?", (IMP)vendor_factory, &was)) {
		say("hakios: cameracaptured: the camera is left alone\n");
		return;
	}
	original_factory = (factory_fn)was;
	replace("FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", "i@:", (IMP)no_stabilization, NULL);
	replace("FigVideoCaptureConnectionConfiguration", "videoStabilizationType", "i@:", (IMP)no_stabilization, NULL);
	if (replace("BWAudioSourceNode", "start:", "B@:^@", (IMP)audio_source_start, &was)) {
		original_audio_start = (start_fn)was;
	}
	if (replace("FigCaptureStillImageSettings", "qualityPrioritization", "i@:", (IMP)quality_prioritization, &was)) {
		original_quality = (int_getter)was;
	}
	if (replace("FigCaptureSourceAttributes", "stillImageNoiseReductionAndFusionScheme", "i@:", (IMP)fusion_scheme, &was)) {
		original_scheme = (int_getter)was;
	}
	replace("FigCaptureSourceVideoFormat", "usesPacked10BitFirmwareStillImageOutputPixelFormat", "B@:",
	    (IMP)no_packed_stills, NULL);
	replace("FigCaptureSourceVideoFormat", "isHDRSupported", "B@:", (IMP)no_hdr, NULL);
	replace("BWGNRNode", "renderSampleBuffer:forInput:", "v@:^{}@", (IMP)gnr_render, NULL);
	/*
	 * The preview stitcher joins the wide camera's preview with the ultra wide's (the frame's outside); there is
	 * one camera here, and its Core Image kernels (BWPreviewStitcherNodeCoreImageArchive's, compiled anew by
	 * our Metal compiler) gave pictures of zeros: the preview's frames were displayed (BWImageQueueSinkNode, about
	 * 8 a second) and every one of them was black, green where the zoom button's glass took its colour. The wide
	 * camera's buffer is passed on as it is; the Preview Scaler after it makes it the preview's size.
	 */
	replace("BWPreviewStitcherNode", "renderSampleBuffer:forInput:", "v@:^{}@", (IMP)gnr_render, NULL);
	if (replace("BWNodeOutput", "emitSampleBuffer:", "v@:^{}", (IMP)emit_sample_buffer, &was)) {
		original_emit = (emit_fn)was;
	}
	replace_class_method("BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", "v@:IB",
	    (IMP)no_prewarm, NULL);
	preview_path();
	register_hevc_encoder();
	say("hakios: cameracaptured: the camera is the kernel's (/dev/hakios-camera)\n");
}

/*
 * The Preview Scaler (a BWPixelTransferNode of the preview sink, crop mode 1) crops each buffer to the rect in its
 * metadata ("FinalCropRect"), which the preview stitcher wrote NaN (its camera information has no base zoom factor
 * here): the transfer from a NaN rect is -21774 and the preview stays dark. In crop mode 0 the node takes the whole
 * buffer: the preview is the camera's picture as it is, its overscan included, but a picture.
 */
typedef void (*crop_mode_fn)(id, SEL, int);
static crop_mode_fn original_set_crop_mode;

/*
 * The stitcher, passed by (below), also turned the camera's landscape buffer (4032x3024) into the preview's
 * portrait (828x1792), by the rotation the graph gives it (setRotationDegrees:). Passed by, the Preview Scaler
 * scaled the landscape buffer into the portrait one and wrote only bands of it: the rest of each frame stayed the
 * YCbCr black of an unwritten buffer (logs/054046-run.log, fb_yuv's profile). The rotation is handed to the
 * Preview Scaler instead.
 */
typedef void (*rotation_fn)(id, SEL, int);
static rotation_fn original_stitcher_rotation;
static int stitcher_rotation = -1;

static void
stitcher_set_rotation(id self, SEL sel, int degrees)
{
	stitcher_rotation = degrees;
	original_stitcher_rotation(self, sel, degrees);
}

static void
set_crop_mode(id self, SEL sel, int mode)
{
	static int told;

	if (mode == 1 && !told) {
		told = 1;
		say("hakios: cameracaptured: a pixel transfer's crop mode 1 (the buffer's FinalCropRect) taken as 0\n");
	}
	original_set_crop_mode(self, sel, mode == 1 ? 0 : mode);
	if (mode == 1) {
		SEL rotate = sel_registerName("setRotationDegrees:");
		int degrees = stitcher_rotation >= 0 ? stitcher_rotation : 90;

		if (class_respondsToSelector(object_getClass(self), rotate)) {
			((void (*)(id, SEL, int))objc_msgSend)(self, rotate, degrees);
			say("hakios: cameracaptured: the Preview Scaler turns the camera's buffer by %d degrees (the stitcher's, which is "
			    "passed by%s)\n", degrees, stitcher_rotation >= 0 ? "" : "; none given yet: 90");
		} else {
			say("hakios: cameracaptured: the Preview Scaler has no setRotationDegrees: the preview stays unturned\n");
		}
	}
}

static void
preview_path(void)
{
	IMP crop, rotation;

	if (replace("BWPreviewStitcherNode", "setRotationDegrees:", "v@:i", (IMP)stitcher_set_rotation, &rotation)) {
		original_stitcher_rotation = (rotation_fn)rotation;
	}

	if (replace("BWPixelTransferNode", "setCropMode:", "v@:i", (IMP)set_crop_mode, &crop)) {
		original_set_crop_mode = (crop_mode_fn)crop;
	}
}

/* CMCapture may come after this library: the methods are replaced once its classes are there */
static void
image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	static bool done;

	if (done || objc_getClass("FigFlashlightLocal") == NULL || objc_getClass("BWFigCaptureDeviceVendor") == NULL) {
		return;
	}
	done = true;
	if (torch >= 0) {
		flashlight_init();
	}
	if (camera) {
		camera_init();
	}
}

__attribute__((constructor)) static void
capture_init(void)
{
	torch = open("/dev/hakios-torch", O_WRONLY);
	camera = access("/dev/hakios-camera", F_OK) == 0;
	if (torch < 0 && !camera) {
		return;                 /* neither a flashlight (torch=3) nor a camera (cam=5) in the kernel: Apple's code */
	}
	_dyld_register_func_for_add_image(image_added);
}
