/*
 * libhakios.media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of Darwin userland 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/hakios-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/hakios-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP; the capture graph is built around it and
 * its frames reach Camera.app's preview (see camera_device.c). Its stream takes the torch level too,
 * so FigFlashlight would need nothing of its own then (not tried yet).
 *
 * What the capture stack asks of hardware this phone has none of is refused here: there is no ISP
 * processing session, so there can be no video stabilization (see video_stabilization_method below).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <malloc/malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <dispatch/dispatch.h>

#include "camera_portrait.h"

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
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;
static bool portrait_dump, portrait_on;      /* boot-args portrait_dump=1, portrait=1 */

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/hakios-torch */
static bool camera;                     /* /dev/hakios-camera is there */
extern int hakios_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera && access("/dev/hakios-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)hakios_camera_device_create);
			say("hakios: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("hakios: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("hakios: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/hakios-torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS node
 * (-[FigCaptureVISPipeline _newVISNodeWithUpstreamOutput:...]) is made with the ISP's processing
 * session, and this device has no such session, so the node comes back nil, the VIS pipeline with
 * it, the movie file sink's tail pipeline with that, and captureSession_buildGraphWithConfiguration
 * tears the whole graph down -- the preview too, for all that the preview has nothing to do with
 * stabilizing a recording. The connection's stabilization method is where the whole stack reads the
 * wish from (+[FigVideoCaptureConnectionConfiguration videoStabilizationMethods:includeIris:] ->
 * cspc_getVideoStabilizationType -> the connections' FigCaptureVideoStabilizationType), and 0 there
 * means none: the graph is then built without a VIS pipeline anywhere
 * (+[FigCaptureVISPipeline visPipelineIsRequiredForFigCaptureVideoStabilizationType:] says no to
 * every type but 2 and 3), and recordings are as steady as the hand that holds the phone.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

/*
 * The type the stack acts on. The connection configuration arrives from the application's
 * AVFoundation with the type in it, which is read straight out of the object and not computed from
 * the method above (-[FigCaptureSessionParsedConfiguration _computeVideoStabilizationTypes] only
 * fills in what the client left out), so the wish has to be refused here as well.
 */
static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, which the capture stack has none of here. A capture session's sound comes through
 * CoreMedia's own capture audio session -- BWAudioSourceNode's start sets its properties and reads
 * its pull format, all of it through a brokered plug-in of the audio HAL -- and that plug-in is not
 * on this phone: the node's start comes back with 'nope', the HAL's word for "no such thing".
 *
 * Camera.app's session has an audio input in it, because it is a session for photographs and for
 * video both. The graph starts its source nodes late (startDeferredSourceNodesIfNeeded) and takes
 * the first error it gets for the whole graph's, so the session reported AVCaptureSessionRuntimeError
 * to the application, which took the session down and built it again, two seconds later, for ever:
 * the camera's preview came in bursts of five frames with the sensor powering down in between.
 *
 * The node is told that it started. It then produces no sound -- which is all a phone with no
 * capture audio has to offer -- and the rest of the graph, the preview and the photograph, runs.
 * Only that one error is swallowed: a microphone that is there and fails for another reason still
 * takes the session down, as it should. (The kernel's own microphone, adsp=1 mic=4, is a different
 * path: AVAudioRecorder and Voice Memos record through it, and nothing here touches it.)
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	if (!said) {
		said = true;
		say("hakios: mediaserverd: there is no capture audio on this phone: the session's audio source gives nothing, "
		    "and the camera runs without it\n");
	}
	return true;
}

/*
 * A photograph's quality prioritization, "speed" at the most. Asked for "balanced" (Camera.app's) or
 * "quality", the capture device takes a scene for a low-light one whenever a frame was exposed at
 * least as long as the sensor's still image stabilization threshold
 * (-[BWFigVideoCaptureDevice _isLowLightSceneUsingFrameStatisticsByPortType:]) -- a threshold from
 * the sensor's tuning in CameraSetup.plist, 0 for a sensor it has no tuning for, as ours -- and
 * then asks the stream for a bracket of four frames for SIS fusion (captureType 4, BWSISNode), which
 * this port has nothing to fuse with: the photograph never came. With "speed" it asks for one frame
 * (-_resolveStillImageCaptureTypeFromStatisticsByPortType:..., captureType 1), which the stream
 * gives (camera_device.c, StillImageCaptureNow).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("hakios: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode, between the still image coordinator and the photo
 * encoder) is a FigSampleBufferProcessor of the iPhone's, which is handed the picture and never
 * hands one back here: the photograph stopped there, with no error, and the request with it (the
 * shutter's second press asked for nothing more). The picture goes past it to the node's output as
 * it came, as with no noise reduction to do.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("hakios: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

/*
 * portrait=1: the photograph's depth, ours (portrait_disparity), handed to the photo encoder as Apple's depth comes to
 * it: a sample buffer of disparity ('hdis') attached to the photograph's as its "Depth" media
 * (BWSampleBufferSetAttachedMedia), and the depth's description (a dictionary, every entry of which the encoder can do
 * without) as an attachment of the photograph's under the key its code reads (-[BWPhotoEncoderNode] at unslid
 * 0x1a1ffe3d0: the pointer at 0x1dae18508). The encoder embeds it if the photograph's settings ask for depth: see
 * capture_still_now.
 */
#define DEPTH_W         768
#define DEPTH_H         576
#define DEPTH_INFO_KEY  0x1dae18508UL

static void
attach_portrait_depth(CMSampleBufferRef photo)
{
	static void (*set_media)(CMSampleBufferRef, CFStringRef, CMSampleBufferRef);
	static CFStringRef media_key, info_key;
	static bool looked, said;
	CVPixelBufferRef pixels = NULL;
	CMVideoFormatDescriptionRef format = NULL;
	CMSampleBufferRef depth = NULL;
	CMSampleTimingInfo timing = { kCMTimeInvalid, CMSampleBufferGetPresentationTimeStamp(photo), kCMTimeInvalid };
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(photo);
	size_t length = 0;

	if (!looked) {
		uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length), at, key;
		CFStringRef *media = dlsym(RTLD_DEFAULT, "BWAttachedMediaKey_Depth");
		char name[128] = "";

		looked = true;
		set_media = dlsym(RTLD_DEFAULT, "BWSampleBufferSetAttachedMedia");
		media_key = media != NULL ? *media : NULL;
		at = DEPTH_INFO_KEY + slide;
		/* (the pointer is to where the key is kept, the key's variable: boot 29 took it for the key and died) */
		key = at >= base && at < base + length ? *(uintptr_t *)at & 0xfffffffffUL : 0;
		key = key >= base && key < base + length ? *(uintptr_t *)key & 0xfffffffffUL : 0;
		info_key = key >= base && key < base + length ? (CFStringRef)key : NULL;
		if (info_key != NULL && CFGetTypeID(info_key) != CFStringGetTypeID()) {
			info_key = NULL;
		}
		if (info_key != NULL) {
			CFStringGetCString(info_key, name, sizeof(name), kCFStringEncodingUTF8);
		}
		say("hakios: portrait: the photograph's depth: set media %s, its key %s, the description's key %s\n",
		    set_media ? "found" : "missing", media_key ? "found" : "missing", info_key ? name : "missing");
	}
	if (set_media == NULL || media_key == NULL || image == NULL) {
		return;
	}
	{
		const void *keys[] = { kCVPixelBufferIOSurfacePropertiesKey };
		CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		const void *values[] = { empty };
		CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		size_t w = CVPixelBufferGetWidth(image), h = CVPixelBufferGetHeight(image);
		uint32_t dw = w >= h ? DEPTH_W : DEPTH_H, dh = w >= h ? DEPTH_H : DEPTH_W;

		if (CVPixelBufferCreate(NULL, dw, dh, kCVPixelFormatType_DisparityFloat16, attributes, &pixels) != 0) {
			pixels = NULL;
		}
		CFRelease(attributes);
		CFRelease(empty);
	}
	if (pixels == NULL) {
		return;
	}
	CVPixelBufferLockBaseAddress(pixels, 0);
	/* the photograph's own depth (from both cameras' photographs) if it was made, else the preview's */
	bool own = portrait_photo_disparity((uint32_t)CVPixelBufferGetWidth(pixels), (uint32_t)CVPixelBufferGetHeight(pixels),
	    CVPixelBufferGetBaseAddress(pixels), CVPixelBufferGetBytesPerRow(pixels));
	bool made = own || portrait_disparity((uint32_t)CVPixelBufferGetWidth(pixels), (uint32_t)CVPixelBufferGetHeight(pixels),
	    CVPixelBufferGetBaseAddress(pixels), CVPixelBufferGetBytesPerRow(pixels));
	CVPixelBufferUnlockBaseAddress(pixels, 0);
	if (made && CMVideoFormatDescriptionCreateForImageBuffer(NULL, pixels, &format) == 0 &&
	    CMSampleBufferCreateReadyWithImageBuffer(NULL, pixels, format, &timing, &depth) == 0) {
		set_media(photo, media_key, depth);
		if (info_key != NULL && CMGetAttachment(photo, info_key, NULL) == NULL) {
			CFDictionaryRef info = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);

			CMSetAttachment(photo, info_key, info, kCMAttachmentMode_ShouldPropagate);
			CFRelease(info);
		}
		if (!said) {
			said = true;
			say("hakios: portrait: the photograph's depth attached (%zux%zu disparity, %s)\n", CVPixelBufferGetWidth(pixels),
			    CVPixelBufferGetHeight(pixels), own ? "its own" : "the preview's");
		}
	} else {
		say("hakios: portrait: the photograph's depth not made (%s)\n", made ? "its sample buffer" : "no depth yet");
	}
	if (depth != NULL) {
		CFRelease(depth);
	}
	if (format != NULL) {
		CFRelease(format);
	}
	CVPixelBufferRelease(pixels);
}

/*
 * portrait=1 bring-up: whether the photograph as saved (the newest HEIC in DCIM/100APPLE, a few seconds after the
 * encoder: Camera.app writes it) holds a disparity map. (The encoder's own sample buffer carries no data: boots 30, 31.)
 */
static void
check_portrait_depth(void *context)
{
	void *(*source_create)(CFURLRef, CFDictionaryRef);
	CFDictionaryRef (*copy_aux)(void *, size_t, CFStringRef);
	CFStringRef *disparity_type, *aux_description;
	void *image_io = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", RTLD_NOW);
	const char *folder = "/var/mobile/Media/DCIM/100APPLE";
	char newest[256] = "", path[512];
	DIR *dir = opendir(folder);
	struct dirent *e;

	(void)context;
	source_create = image_io ? dlsym(image_io, "CGImageSourceCreateWithURL") : NULL;
	copy_aux = image_io ? dlsym(image_io, "CGImageSourceCopyAuxiliaryDataInfoAtIndex") : NULL;
	disparity_type = image_io ? dlsym(image_io, "kCGImageAuxiliaryDataTypeDisparity") : NULL;
	aux_description = image_io ? dlsym(image_io, "kCGImageAuxiliaryDataInfoDataDescription") : NULL;
	if (dir == NULL || source_create == NULL || copy_aux == NULL || disparity_type == NULL) {
		say("hakios: portrait: the saved photograph not looked into (%s)\n", dir == NULL ? strerror(errno) : "ImageIO");
		if (dir != NULL) {
			closedir(dir);
		}
		return;
	}
	while ((e = readdir(dir)) != NULL) {
		if (strstr(e->d_name, ".HEIC") != NULL && strcmp(e->d_name, newest) > 0) {
			strlcpy(newest, e->d_name, sizeof(newest));
		}
	}
	closedir(dir);
	if (newest[0] == '\0') {
		say("hakios: portrait: no saved photograph in %s yet\n", folder);
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", folder, newest);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	void *source = url != NULL ? source_create(url, NULL) : NULL;
	CFDictionaryRef aux = source != NULL ? copy_aux(source, 0, *disparity_type) : NULL;

	if (aux != NULL) {
		CFDictionaryRef description = aux_description ? CFDictionaryGetValue(aux, *aux_description) : NULL;
		CFNumberRef w = description ? CFDictionaryGetValue(description, CFSTR("Width")) : NULL;
		CFNumberRef h = description ? CFDictionaryGetValue(description, CFSTR("Height")) : NULL;
		int iw = 0, ih = 0;

		if (w != NULL) {
			CFNumberGetValue(w, kCFNumberIntType, &iw);
		}
		if (h != NULL) {
			CFNumberGetValue(h, kCFNumberIntType, &ih);
		}
		say("hakios: portrait: %s holds a disparity map, %dx%d\n", newest, iw, ih);
		CFRelease(aux);
	} else {
		say("hakios: portrait: %s holds no disparity map (%s)\n", newest, source != NULL ? "read" : "not read");
	}
	if (source != NULL) {
		CFRelease(source);
	}
	if (url != NULL) {
		CFRelease(url);
	}
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			if (portrait_on) {
				dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC),
				    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL, check_portrait_depth);
			}
			say("hakios: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
			    CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (portrait_on) {
			attach_portrait_depth(sample_buffer);
		}
		began = mach_absolute_time();
	}
}

/*
 * A node's output given no sample buffer at all, which no node takes: the session's audio source
 * (-[BWAudioSourceNode _generateSamples]), whose audio unit never started here (see audio_source_start
 * above), fills the silence by a timer with buffers it fails to make and emits them all the same --
 * NULL. Live Photo's staging node added that to an array (-[BWIrisStagingNode renderSampleBuffer:forInput:])
 * and mediaserverd died of the NSInvalidArgumentException, every half a minute with Live Photo on, the
 * preview black for a second each time. A NULL is not passed on.
 */
static IMP original_emit_sample_buffer;

/*
 * Where a recording's samples go (bring-up aid for video): the first sample of each media type that
 * each kind of node emits is told, and then every 300th, so a log shows how far the audio and the
 * video get -- the audio source, the file coordinator, the movie file sink.
 */
/*
 * portrait=1 bring-up: -[BWCrossoverNode renderSampleBuffer:forInput:] passes a buffer on only when one flag in one of its
 * attachments (a dictionary) is true. The two keys, read where its code reads them (the shared cache's addresses, unslid).
 */
extern const void *_dyld_get_shared_cache_range(size_t *length);
static CFStringRef crossover_key[2];

static void
crossover_keys(void)
{
	static const uintptr_t where[2] = { 0x1dae16250, 0x1dae16e90 };
	size_t length = 0;
	uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;

	for (int i = 0; i < 2; i++) {
		const CFStringRef *global = *(const CFStringRef *const *)(where[i] + slide);
		char text[128] = "?";

		crossover_key[i] = global != NULL ? *global : NULL;
		if (crossover_key[i] != NULL && CFGetTypeID(crossover_key[i]) == CFStringGetTypeID()) {
			CFStringGetCString(crossover_key[i], text, sizeof(text), kCFStringEncodingUTF8);
		}
		say("hakios: mediaserverd: flow: the crossover's key %d: %s\n", i, text);
	}
}

static void
crossover_say(const char *name, CMSampleBufferRef sample_buffer)
{
	CFTypeRef d = crossover_key[0] != NULL ? CMGetAttachment(sample_buffer, crossover_key[0], NULL) : NULL;
	CFTypeRef v = d != NULL && CFGetTypeID(d) == CFDictionaryGetTypeID() && crossover_key[1] != NULL ?
	    CFDictionaryGetValue(d, crossover_key[1]) : NULL;
	CFStringRef text = v != NULL ? CFCopyDescription(v) : NULL;
	char line[200] = "none";

	if (text != NULL) {
		CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8);
		CFRelease(text);
	}
	say("hakios: mediaserverd: flow: %s: the crossover's flag: %s (the attachment %s)\n", name, line, d != NULL ? "there" : "absent");
}

static void
trace_flow(id output, CMSampleBufferRef sample_buffer)
{
	static struct { const char *node; FourCharCode type; unsigned count; id output; } seen[64];
	static unsigned used;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	id node = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node"));
	const char *name = node != nil ? object_getClassName(node) : "?";
	CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample_buffer);
	FourCharCode type = format != NULL ? CMFormatDescriptionGetMediaType(format) : 0;
	unsigned i, count = 0;

	pthread_mutex_lock(&lock);
	for (i = 0; i < used && (seen[i].node != name || seen[i].type != type || (portrait_on && seen[i].output != output)); i++) {
	}
	if (i == used && used < sizeof(seen) / sizeof(seen[0])) {
		seen[used].node = name, seen[used].type = type, seen[used].count = 0, seen[used].output = output;
		used++;
	}
	if (i < used) {
		count = ++seen[i].count;
	}
	pthread_mutex_unlock(&lock);
	if (count == 1 && portrait_dump) {
		CFDictionaryRef attachments = CMCopyDictionaryOfAttachments(kCFAllocatorDefault, sample_buffer,
		    kCMAttachmentMode_ShouldPropagate);
		CFIndex n = attachments != NULL ? CFDictionaryGetCount(attachments) : 0;
		const void **keys = n > 0 ? calloc((size_t)n, sizeof(*keys)) : NULL;

		if (keys != NULL) {
			CFDictionaryGetKeysAndValues(attachments, keys, NULL);
			for (CFIndex k = 0; k < n; k++) {
				char key[128] = "?";

				if (CFGetTypeID(keys[k]) == CFStringGetTypeID()) {
					CFStringGetCString(keys[k], key, sizeof(key), kCFStringEncodingUTF8);
				}
				say("hakios: mediaserverd: flow: %s attachment %s\n", name, key);
			}
			free(keys);
		}
		if (attachments != NULL) {
			CFRelease(attachments);
		}
	}
	if (portrait_on && (count == 1 || count == 100)) {
		static bool keys;

		if (!keys) {
			keys = true;
			crossover_keys();
		}
		crossover_say(name, sample_buffer);
	}
	if (count == 1 && portrait_on) {
		/* (who takes it: the output's connections' inputs' nodes) */
		SEL connections = sel_registerName("connections"), connection = sel_registerName("connection");
		SEL input_sel = sel_registerName("input"), node_sel = sel_registerName("node");
		id list = nil, one = nil;

		if (class_respondsToSelector(object_getClass(output), connections)) {
			list = ((id (*)(id, SEL))objc_msgSend)(output, connections);
		}
		if (list == nil && class_respondsToSelector(object_getClass(output), connection)) {
			one = ((id (*)(id, SEL))objc_msgSend)(output, connection);
			list = one != nil ? (id)CFAutorelease(CFArrayCreate(NULL, (const void **)&one, 1, &kCFTypeArrayCallBacks)) : nil;
		}
		if (list != nil && CFGetTypeID((CFTypeRef)list) == CFArrayGetTypeID()) {
			for (CFIndex k = 0; k < CFArrayGetCount((CFArrayRef)list); k++) {
				id c = (id)CFArrayGetValueAtIndex((CFArrayRef)list, k), in = nil, to = nil;

				if (class_respondsToSelector(object_getClass(c), input_sel)) {
					in = ((id (*)(id, SEL))objc_msgSend)(c, input_sel);
				}
				if (in != nil && class_respondsToSelector(object_getClass(in), node_sel)) {
					to = ((id (*)(id, SEL))objc_msgSend)(in, node_sel);
				}
				say("hakios: mediaserverd: flow: %s (%p) gives to %s (%p)\n", name, output, to != nil ? object_getClassName(to) : "?", in);
			}
		} else {
			say("hakios: mediaserverd: flow: %s gives to (no connections found: %s)\n", name, object_getClassName(output));
		}
	}
	if (count == 1 || count % 300 == 0) {
		CMTime t = CMSampleBufferGetPresentationTimeStamp(sample_buffer);

		say("hakios: mediaserverd: flow: %s (%p) emits %c%c%c%c #%u (pts %.3f, %ld samples)\n", name, output,
		    (char)(type >> 24), (char)(type >> 16), (char)(type >> 8), (char)type, count,
		    CMTIME_IS_NUMERIC(t) ? CMTimeGetSeconds(t) : -1.0, (long)CMSampleBufferGetNumSamples(sample_buffer));
	}
}

/*
 * The capture session's sound where the kernel has no microphone (no /dev/hakios-mic: zeus). The session's audio source
 * gave one buffer from the audio HAL (940 frames at 48 kHz, on the host clock) and from then on its own fill-in for a
 * unit that delivers nothing (-[BWAudioSourceNode _generateSamples]): buffers that said 20671759 samples each at times
 * that went backwards, -263.8 s, -240.5 s, -234.1 s (logs/181958-run.log). The movie file sink starts a file on the
 * first sample of each of its tracks, in order: Camera.app's recording stayed at 00:00:00. Each of the audio source's
 * buffers is given on as silence instead: its format (the first plausible one it gave), as many frames as the time
 * since the last one covers, from where the last one ended, on the host clock (the video's clock too).
 */
static bool no_microphone;

static CMSampleBufferRef
silence_for(CMSampleBufferRef given)
{
	static CMAudioFormatDescriptionRef format;
	static AudioStreamBasicDescription asbd;
	static CMTime end;
	static bool said;
	CMFormatDescriptionRef f = CMSampleBufferGetFormatDescription(given);
	CMTime now = CMClockGetTime(CMClockGetHostTimeClock()), start;
	CMBlockBufferRef block = NULL;
	CMSampleBufferRef made = NULL;
	CMItemCount frames;
	size_t bytes;

	if (format == NULL && f != NULL && CMFormatDescriptionGetMediaType(f) == kCMMediaType_Audio) {
		const AudioStreamBasicDescription *d = CMAudioFormatDescriptionGetStreamBasicDescription(f);

		if (d != NULL && d->mFormatID == kAudioFormatLinearPCM && d->mSampleRate >= 8000 && d->mSampleRate <= 192000 &&
		    d->mBytesPerFrame >= 1 && d->mBytesPerFrame <= 64 && d->mFramesPerPacket == 1) {
			asbd = *d;
			format = (CMAudioFormatDescriptionRef)CFRetain(f);
		}
	}
	if (format == NULL) {
		return NULL;
	}
	/* from where the last ended; after a pause of more than half a second (or the first time), 20 ms back from now */
	if (CMTIME_IS_VALID(end) && CMTimeGetSeconds(CMTimeSubtract(now, end)) >= 0 && CMTimeGetSeconds(CMTimeSubtract(now, end)) < 0.5) {
		start = end;
	} else {
		start = CMTimeSubtract(now, CMTimeMakeWithSeconds(0.02, 1000000));
	}
	frames = (CMItemCount)(CMTimeGetSeconds(CMTimeSubtract(now, start)) * asbd.mSampleRate);
	if (frames < 1) {
		return NULL;            /* (nothing new to cover: this one is not passed on) */
	}
	start = CMTimeConvertScale(start, (int32_t)asbd.mSampleRate, kCMTimeRoundingMethod_Default);
	bytes = (size_t)frames * asbd.mBytesPerFrame;
	if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, bytes, NULL, NULL, 0, bytes, kCMBlockBufferAssureMemoryNowFlag, &block) != 0 ||
	    CMBlockBufferFillDataBytes(0, block, 0, bytes) != 0 ||
	    CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, block, format, frames, start, NULL, &made) != 0) {
		made = NULL;
	}
	if (block != NULL) {
		CFRelease(block);
	}
	if (made != NULL) {
		end = CMTimeAdd(start, CMTimeMake(frames, (int32_t)asbd.mSampleRate));
		if (!said) {
			said = true;
			say("hakios: mediaserverd: no microphone (/dev/hakios-mic): the capture session's sound is silence, %.0f Hz, "
			    "%u channel(s), on the host clock\n", asbd.mSampleRate, asbd.mChannelsPerFrame);
		}
	}
	return made;
}

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (no_microphone && sample_buffer != NULL) {
		id node = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("node"));

		if (node != nil && strcmp(object_getClassName(node), "BWAudioSourceNode") == 0) {
			CMSampleBufferRef quiet = silence_for(sample_buffer);

			if (quiet != NULL) {
				trace_flow(self, quiet);
				((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, quiet);
				CFRelease(quiet);
			}
			return;
		}
	}
	if (sample_buffer != NULL) {
		trace_flow(self, sample_buffer);
	}
	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("hakios: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	time_photo_encoder(self, sample_buffer);
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/*
 * The hardware video encoder (only where the kernel has one: vid=2, /dev/hakios-video; zeus's Iris2). VideoToolbox's
 * encoders are objects with a table of four functions (start a session, encode a frame, complete frames, the
 * properties it knows), as VCPHEVC's own table shows. The software HEVC encoder registered below is made as before
 * and stays the one VideoToolbox talks to, properties and all; its object is given a copy of its table in which
 * start, encode and complete are the ones here. A session of a size the kernel's encoder takes (hakios.video.h) with
 * NV12 frames then goes to the device: the frame is written, its bitstream read (Annex B, parameter sets before
 * every key frame), made a sample of length-prefixed NAL units with a format description of those parameter sets,
 * and emitted as the encoder's. Anything else (a photograph's 12 megapixels, a second session while the device is
 * taken, a device that fails) is left to the software encoder as before.
 */
#include "hakios.video.h"

struct hw_vtable {                              /* CMBaseVTable and the encoder's class after it */
	const void *reserved;
	const void **base_class;                /* version, storage size, equal, invalidate, finalize, description, copy
	                                         * property, set property, barrier, protocols: 10 words */
	const void **encoder_class;             /* version, start, encode, complete, supported properties, ... */
};
#define HW_BASE_WORDS           22      /* (all that lies before the encoder's class in VCPHEVC: the base class may be
                                         * longer than the ten words known, and what is copied must not end before it) */
#define HW_ENCODER_WORDS        22      /* (VCPHEVC's class has functions as far as word 20: the tile ones a photograph
                                         * is encoded through; boot 80, with 16 copied, no photograph came out) */
#define HW_BASE_INVALIDATE      3
#define HW_BASE_FINALIZE        4
#define HW_BASE_SET_PROPERTY    7
#define HW_START                1
#define HW_ENCODE               2
#define HW_COMPLETE             3

typedef OSStatus (*hw_start_fn)(void *encoder, void *session, CMVideoDimensions dimensions, void *more);
typedef OSStatus (*hw_encode_fn)(void *encoder, void *frame, CVPixelBufferRef pixels, CMTime time, CMTime duration,
    CFDictionaryRef properties, void *flags_out, void *more);
typedef OSStatus (*hw_complete_fn)(void *encoder, CMTime until);
typedef OSStatus (*hw_set_property_fn)(void *object, CFStringRef key, CFTypeRef value);
typedef OSStatus (*hw_object_fn)(void *object);
typedef void (*hw_finalize_fn)(void *object);

static struct hw_encoder {
	void *encoder, *session;
	const struct hw_vtable *original;
	struct hw_vtable table;
	const void *base_class[HW_BASE_WORDS], *encoder_class[HW_ENCODER_WORDS];
	int32_t width, height;
	uint32_t rate, bits, key_interval;
	int fd;                                 /* /dev/hakios-video, once the session is the device's */
	bool software;                          /* this session is the software encoder's after all */
	uint8_t *frame, *stream;
	CMFormatDescriptionRef format;
	unsigned frames, dropped;
} hw_encoders[4];
static pthread_mutex_t hw_lock = PTHREAD_MUTEX_INITIALIZER;
static void *(*hw_software_create)(void);
static OSStatus (*hw_emit)(void *session, void *frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample);
extern const void *CMBaseObjectGetVTable(void *object);

#define HW_STREAM_BYTES         (8u << 20)

static struct hw_encoder *
hw_find(void *encoder)
{
	struct hw_encoder *found = NULL;

	pthread_mutex_lock(&hw_lock);
	for (unsigned i = 0; i < sizeof(hw_encoders) / sizeof(hw_encoders[0]); i++) {
		if (hw_encoders[i].encoder == encoder) {
			found = &hw_encoders[i];
		}
	}
	pthread_mutex_unlock(&hw_lock);
	return found;
}

static void
hw_close(struct hw_encoder *e)
{
	if (e->fd >= 0) {
		struct hakios_video_command stop = { .command = HAKIOS_VIDEO_STOP };

		(void)write(e->fd, &stop, sizeof(stop));
		close(e->fd);
		e->fd = -1;
		say("hakios: mediaserverd: the hardware encoder's %dx%d session ends: %u frames, %u dropped\n", e->width, e->height,
		    e->frames, e->dropped);
	}
	free(e->frame);
	free(e->stream);
	e->frame = e->stream = NULL;
	if (e->format != NULL) {
		CFRelease(e->format);
		e->format = NULL;
	}
}

static OSStatus
hw_start(void *encoder, void *session, CMVideoDimensions dimensions, void *more)
{
	struct hw_encoder *e = hw_find(encoder);

	if (e == NULL) {
		return -12903;
	}
	e->session = session;
	e->width = dimensions.width;
	e->height = dimensions.height;
	return ((hw_start_fn)e->original->encoder_class[HW_START])(encoder, session, dimensions, more);
}

static OSStatus
hw_set_property(void *object, CFStringRef key, CFTypeRef value)
{
	struct hw_encoder *e = hw_find(object);
	int32_t number = 0;

	if (e == NULL) {
		return -12903;
	}
	if (key != NULL && value != NULL && CFGetTypeID(value) == CFNumberGetTypeID() &&
	    CFNumberGetValue(value, kCFNumberSInt32Type, &number) && number > 0) {
		if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) {
			e->bits = (uint32_t)number;
		} else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) {
			e->rate = (uint32_t)number;
		} else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) {
			e->key_interval = (uint32_t)number;
		}
	}
	return ((hw_set_property_fn)e->original->base_class[HW_BASE_SET_PROPERTY])(object, key, value);
}

/* The device's session, at the first frame (by then the properties are set). False: the software encoder's. */
static bool
hw_open(struct hw_encoder *e, CVPixelBufferRef pixels)
{
	OSType format = CVPixelBufferGetPixelFormatType(pixels);
	struct hakios_video_command start = { .command = HAKIOS_VIDEO_START, .a = HAKIOS_VIDEO_HEVC, .b = (uint32_t)e->width,
	    .c = (uint32_t)e->height, .d = e->rate != 0 && e->rate <= 240 ? e->rate : 30, .e = e->bits, .f = e->key_interval,
	    .g = format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };

	if (format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange && format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
		say("hakios: mediaserverd: a %dx%d encoder session of '%c%c%c%c' frames: the software encoder's\n", e->width, e->height,
		    (char)(format >> 24), (char)(format >> 16), (char)(format >> 8), (char)format);
		return false;
	}
	e->fd = open(HAKIOS_VIDEO_DEVICE, O_RDWR);
	if (e->fd < 0) {
		say("hakios: mediaserverd: a %dx%d encoder session: no %s (or it is taken): the software encoder's\n", e->width,
		    e->height, HAKIOS_VIDEO_DEVICE);
		return false;
	}
	if (write(e->fd, &start, sizeof(start)) != (ssize_t)sizeof(start)) {
		say("hakios: mediaserverd: a %dx%d encoder session: not a size of the hardware encoder's: the software encoder's\n",
		    e->width, e->height);
		close(e->fd);
		e->fd = -1;
		return false;
	}
	e->frame = malloc(sizeof(start) + (size_t)e->width * (size_t)e->height * 3 / 2);
	e->stream = malloc(HW_STREAM_BYTES);
	if (e->frame == NULL || e->stream == NULL) {
		hw_close(e);
		return false;
	}
	say("hakios: mediaserverd: a %dx%d HEVC encoder session IN HARDWARE (%s): %u frames a second, %u bit/s, a key frame "
	    "every %u\n", e->width, e->height, HAKIOS_VIDEO_DEVICE, start.d, start.e, start.f);
	return true;
}

/* The bitstream as a sample: NAL units with their lengths before them, the parameter sets in the format description. */
static CMSampleBufferRef
hw_sample(struct hw_encoder *e, const uint8_t *data, uint32_t bytes, CMTime time, CMTime duration, bool key)
{
	const uint8_t *sets[8];
	size_t set_sizes[8], sample_size = 0;
	unsigned set_count = 0;
	uint8_t *out = malloc(bytes + 64);
	uint32_t at = 0, start;
	CMBlockBufferRef block = NULL;
	CMSampleBufferRef sample = NULL;
	CMSampleTimingInfo timing = { .duration = duration, .presentationTimeStamp = time, .decodeTimeStamp = kCMTimeInvalid };

	if (out == NULL) {
		return NULL;
	}
	while (at + 3 <= bytes && !(data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1)) {
		at++;
	}
	while (at + 3 <= bytes) {                       /* (at a start code) */
		uint32_t next, end, size, type;

		start = at + 3;
		for (next = start; next + 3 <= bytes && !(data[next] == 0 && data[next + 1] == 0 && data[next + 2] == 1); next++) {
		}
		if (next + 3 > bytes) {
			next = end = bytes;
		} else {
			for (end = next; end > start && data[end - 1] == 0; end--) {    /* (the zeros before the next start code) */
			}
		}
		at = next;
		if (end < start + 2) {
			continue;
		}
		size = end - start;
		type = data[start] >> 1 & 0x3f;
		if (type >= 32 && type <= 34) {                 /* VPS, SPS, PPS */
			if (set_count < 8) {
				sets[set_count] = data + start;
				set_sizes[set_count++] = size;
			}
		} else if (type != 35 && sample_size + 4 + size <= bytes + 64) {       /* (no delimiters) */
			out[sample_size] = (uint8_t)(size >> 24);
			out[sample_size + 1] = (uint8_t)(size >> 16);
			out[sample_size + 2] = (uint8_t)(size >> 8);
			out[sample_size + 3] = (uint8_t)size;
			memcpy(out + sample_size + 4, data + start, size);
			sample_size += 4 + size;
		}
	}
	if (set_count >= 3) {
		CMFormatDescriptionRef format = NULL;

		if (CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, set_count, sets, set_sizes, 4, NULL, &format) == 0) {
			if (e->format != NULL) {
				CFRelease(e->format);
			}
			e->format = format;
		}
	}
	if (e->format == NULL || sample_size == 0 ||
	    CMBlockBufferCreateWithMemoryBlock(NULL, out, sample_size, kCFAllocatorMalloc, NULL, 0, sample_size, 0, &block) != 0) {
		free(out);
		return NULL;
	}
	if (CMSampleBufferCreateReady(NULL, block, e->format, 1, 1, &timing, 1, &sample_size, &sample) == 0 && !key) {
		CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);

		if (attachments != NULL && CFArrayGetCount(attachments) > 0) {
			CFMutableDictionaryRef first = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);

			CFDictionarySetValue(first, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
			CFDictionarySetValue(first, kCMSampleAttachmentKey_DependsOnOthers, kCFBooleanTrue);
		}
	}
	CFRelease(block);
	return sample;
}

static OSStatus
hw_encode(void *encoder, void *frame, CVPixelBufferRef pixels, CMTime time, CMTime duration, CFDictionaryRef properties,
    void *flags_out, void *more)
{
	struct hw_encoder *e = hw_find(encoder);
	struct hakios_video_command *c;
	struct hakios_video_packet packet;
	CMSampleBufferRef sample = NULL;
	size_t width, height, bytes;
	bool key = false;
	ssize_t got;

	if (e == NULL) {
		return -12903;
	}
	if (!e->software && e->fd < 0 && (pixels == NULL || !hw_open(e, pixels))) {
		e->software = true;
	}
	if (e->software) {
		return ((hw_encode_fn)e->original->encoder_class[HW_ENCODE])(encoder, frame, pixels, time, duration, properties,
		           flags_out, more);
	}
	width = (size_t)e->width;
	height = (size_t)e->height;
	bytes = width * height * 3 / 2;
	if (properties != NULL) {
		CFTypeRef force = CFDictionaryGetValue(properties, kVTEncodeFrameOptionKey_ForceKeyFrame);

		key = force != NULL && CFEqual(force, kCFBooleanTrue);
	}
	c = (struct hakios_video_command *)e->frame;
	memset(c, 0, sizeof(*c));
	c->command = HAKIOS_VIDEO_FRAME;
	c->bytes = (uint32_t)bytes;
	c->a = (uint32_t)width;
	c->b = key || e->frames == 0;
	c->time = (uint64_t)e->frames;
	if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != 0 || CVPixelBufferGetPlaneCount(pixels) < 2 ||
	    CVPixelBufferGetWidthOfPlane(pixels, 0) < width || CVPixelBufferGetHeightOfPlane(pixels, 0) < height) {
		say("hakios: mediaserverd: the hardware encoder: a frame it cannot read\n");
		return -12902;
	}
	for (unsigned plane = 0; plane < 2; plane++) {
		const uint8_t *from = CVPixelBufferGetBaseAddressOfPlane(pixels, plane);
		size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixels, plane), rows = plane == 0 ? height : height / 2;
		uint8_t *to = e->frame + sizeof(*c) + (plane == 0 ? 0 : width * height);

		for (size_t row = 0; row < rows; row++) {
			memcpy(to + row * width, from + row * stride, width);
		}
	}
	CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
	if (write(e->fd, c, sizeof(*c) + bytes) < 0) {
		if (e->dropped++ < 5) {
			say("hakios: mediaserverd: the hardware encoder does not take frame %u\n", e->frames);
		}
		return hw_emit(e->session, frame, 0, kVTEncodeInfo_FrameDropped, NULL);
	}
	e->frames++;
	got = read(e->fd, e->stream, HW_STREAM_BYTES);
	if (got >= (ssize_t)sizeof(packet)) {
		memcpy(&packet, e->stream, sizeof(packet));
		if (memcmp(packet.magic, HAKIOS_VIDEO_MAGIC, 8) == 0 && packet.bytes <= (size_t)got - sizeof(packet) &&
		    !(packet.flags & HAKIOS_VIDEO_ERROR)) {
			sample = hw_sample(e, e->stream + packet.header_bytes, packet.bytes, time, duration,
			    (packet.flags & HAKIOS_VIDEO_KEY) != 0);
		}
	}
	if (sample == NULL) {
		if (e->dropped++ < 5) {
			say("hakios: mediaserverd: the hardware encoder gives nothing for frame %u (read %ld)\n", e->frames - 1, (long)got);
		}
		return hw_emit(e->session, frame, 0, kVTEncodeInfo_FrameDropped, NULL);
	}
	if (e->frames <= 3 || e->frames % 150 == 0) {
		say("hakios: mediaserverd: the hardware encoder: frame %u out, %u bytes%s (%u dropped)\n", e->frames, packet.bytes,
		    packet.flags & HAKIOS_VIDEO_KEY ? ", key" : "", e->dropped);
	}
	(void)hw_emit(e->session, frame, 0, 0, sample);
	CFRelease(sample);
	return 0;
}

static OSStatus
hw_complete(void *encoder, CMTime until)
{
	struct hw_encoder *e = hw_find(encoder);

	if (e == NULL) {
		return -12903;
	}
	if (!e->software && e->fd >= 0) {
		return 0;                       /* (every frame is emitted before its encode call returns) */
	}
	return ((hw_complete_fn)e->original->encoder_class[HW_COMPLETE])(encoder, until);
}

static OSStatus
hw_invalidate(void *object)
{
	struct hw_encoder *e = hw_find(object);

	if (e == NULL) {
		return 0;
	}
	hw_close(e);
	return e->original->base_class[HW_BASE_INVALIDATE] != NULL ?
	       ((hw_object_fn)e->original->base_class[HW_BASE_INVALIDATE])(object) : 0;
}

static void
hw_finalize(void *object)
{
	struct hw_encoder *e = hw_find(object);

	if (e == NULL) {
		return;
	}
	hw_close(e);
	if (e->original->base_class[HW_BASE_FINALIZE] != NULL) {
		((hw_finalize_fn)e->original->base_class[HW_BASE_FINALIZE])(object);
	}
	pthread_mutex_lock(&hw_lock);
	e->encoder = NULL;
	pthread_mutex_unlock(&hw_lock);
}

/* What VideoToolbox calls for an 'hvc1' encoder: the software one, with the table above in its object. */
static OSStatus
hw_create(CMVideoCodecType codec, CFAllocatorRef allocator, void **out)
{
	OSStatus status = ((OSStatus (*)(CMVideoCodecType, CFAllocatorRef, void **))hw_software_create)(codec, allocator, out);
	struct hw_encoder *e = NULL;
	const struct hw_vtable *table;
	const void **slot = NULL;

	if (status != 0 || out == NULL || *out == NULL) {
		return status;
	}
	table = CMBaseObjectGetVTable(*out);
	/* (the object: the CoreFoundation header, then, some words on, its table's address: looked for, and anything not
	 * as known is left alone) */
	for (unsigned word = 2; table != NULL && word < 8 && slot == NULL; word++) {
		if (((const void **)*out)[word] == (const void *)table) {
			slot = &((const void **)*out)[word];
		}
	}
	if (slot == NULL || table->base_class == NULL || table->encoder_class == NULL ||
	    (uint32_t)(uintptr_t)table->encoder_class[0] < 1 || (uint32_t)(uintptr_t)table->encoder_class[0] > 8) {
		const void **words = (const void **)*out;

		say("hakios: mediaserverd: the software HEVC encoder's object is not as known (table %p, base %p, class %p, version "
		    "%#lx; object %p %p %p %p %p %p): no hardware encoder for it\n", (void *)table,
		    table != NULL ? (void *)table->base_class : NULL, table != NULL ? (void *)table->encoder_class : NULL,
		    table != NULL && table->encoder_class != NULL ? (unsigned long)(uintptr_t)table->encoder_class[0] : 0ul, words[0],
		    words[1], words[2], words[3], words[4], words[5]);
		return status;
	}
	pthread_mutex_lock(&hw_lock);
	for (unsigned i = 0; i < sizeof(hw_encoders) / sizeof(hw_encoders[0]) && e == NULL; i++) {
		if (hw_encoders[i].encoder == NULL) {
			e = &hw_encoders[i];
			memset(e, 0, sizeof(*e));
			e->encoder = *out;
		}
	}
	pthread_mutex_unlock(&hw_lock);
	if (e == NULL) {
		return status;                  /* (more sessions at once than slots: the software encoder's) */
	}
	e->fd = -1;
	e->original = table;
	memcpy(e->base_class, table->base_class, sizeof(e->base_class));
	memcpy(e->encoder_class, table->encoder_class, sizeof(e->encoder_class));
	e->base_class[HW_BASE_INVALIDATE] = (const void *)hw_invalidate;
	e->base_class[HW_BASE_FINALIZE] = (const void *)hw_finalize;
	e->base_class[HW_BASE_SET_PROPERTY] = (const void *)hw_set_property;
	e->encoder_class[HW_START] = (const void *)hw_start;
	e->encoder_class[HW_ENCODE] = (const void *)hw_encode;
	e->encoder_class[HW_COMPLETE] = (const void *)hw_complete;
	e->table.reserved = table->reserved;
	e->table.base_class = e->base_class;
	e->table.encoder_class = e->encoder_class;
	*slot = &e->table;
	return status;
}

/*
 * The photograph's encoder. Camera.app asks for HEIC -- HEVC inside a HEIF file, which CMPhoto makes
 * through VideoToolbox -- and VideoToolbox here has no HEVC encoder but the iPhone's hardware ones
 * (com.apple.videotoolbox.videoencoder.hevc and .hevc.gva, behind its AVE): the photo encoder node
 * failed with kVTCouldNotFindVideoEncoderErr (-12908). Apple's software HEVC codec is in the system
 * all the same, VCPHEVC.videocodec, whose decoder VideoToolbox takes here for every HEIC picture (see
 * the top), and there is an encoder in it, HEVCVideoEncoder_CreateInstance, that Darwin never registers:
 * the codec's VCPHEVCRegisterEncoder is an empty function. It is registered here, as the codec
 * registers its decoder (VTRegisterVideoDecoderWithInfo, not hardware accelerated).
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
	int status;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("hakios: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	hw_software_create = create;
	hw_emit = dlsym(vt, "VTEncoderSessionEmitEncodedFrame");
	/* (the device is asked for at each session's first frame: the kernel makes it when its codec is up, which is after
	 * mediaserverd has started) */
	if (hw_emit != NULL) {
		status = register_encoder('hvc1', info, (void *)hw_create);
		say("hakios: mediaserverd: 'hvc1' sessions of the hardware encoder's sizes go to %s where the kernel has it, the "
		    "rest to VCPHEVC.videocodec in software (%d)\n", HAKIOS_VIDEO_DEVICE, status);
	} else {
		status = register_encoder('hvc1', info, create);
		say("hakios: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
	}
	CFRelease(info);
}

/*
 * What the file coordinator and the movie file sink are handed (bring-up aid for video, as
 * trace_flow above): the first sample of each media type, then every 300th, and when a file is
 * started and finished.
 */
static IMP original_coordinator_render, original_movie_sink_render;

static void
trace_received(const char *who, CMSampleBufferRef sample_buffer, unsigned *counts)
{
	CMFormatDescriptionRef format = sample_buffer != NULL ? CMSampleBufferGetFormatDescription(sample_buffer) : NULL;
	FourCharCode type = format != NULL ? CMFormatDescriptionGetMediaType(format) : 0;
	unsigned *count = &counts[type == kCMMediaType_Audio ? 0 : type == kCMMediaType_Video ? 1 : 2];

	if (++*count == 1 || *count % 300 == 0) {
		say("hakios: mediaserverd: flow: %s takes %c%c%c%c #%u\n", who, (char)(type >> 24), (char)(type >> 16),
		    (char)(type >> 8), (char)type, *count);
	}
}

static void
coordinator_render(id self, SEL selector, CMSampleBufferRef sample_buffer, id input)
{
	static unsigned counts[3];

	trace_received("BWFileCoordinatorNode", sample_buffer, counts);
	((void (*)(id, SEL, CMSampleBufferRef, id))original_coordinator_render)(self, selector, sample_buffer, input);
}

static void
movie_sink_render(id self, SEL selector, CMSampleBufferRef sample_buffer, id input)
{
	static unsigned counts[3];

	trace_received("BWQuickTimeMovieFileSinkNode", sample_buffer, counts);
	((void (*)(id, SEL, CMSampleBufferRef, id))original_movie_sink_render)(self, selector, sample_buffer, input);
}

/*
 * A recording's video encoder. The movie path's compressor (BWVideoCompressorNode) gave one frame in a 10 s recording
 * and nothing after, while the file coordinator took 300 (logs/193544-run.log): its encoder is the software HEVC one
 * registered above, about 5 megapixels a second here (a 12-megapixel photograph took 2.3 s), asked for with frame
 * reordering and look-ahead as the iPhone's hardware encoder is. Its frames sat in the look-ahead; the stop of the
 * recording then waited for them, and mediaserverd died of the wait (SIGTRAP, 10 s on). Every compression session
 * made in here is told to run in real time, without reordering or a frame delay, speed before quality (what the
 * encoder cannot keep up with it drops), and its frames out are counted. This library is inserted when mediaserverd
 * starts, so dyld's interposing reaches VideoToolbox's callers in CMCapture.
 */
struct encoder_watch {
	VTCompressionOutputCallback callback;
	void *refcon;
	unsigned frames, errors, dropped;
	int32_t width, height;
	CMVideoCodecType codec;
};

static void
encoder_output(void *refcon, void *frame_refcon, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef buffer)
{
	struct encoder_watch *w = refcon;

	if (status != 0) {
		if (w->errors++ < 5) {
			say("hakios: mediaserverd: the %dx%d encoder: error %d\n", w->width, w->height, (int)status);
		}
	} else if (buffer == NULL || (flags & kVTEncodeInfo_FrameDropped)) {
		w->dropped++;
	} else if (++w->frames <= 3 || w->frames % 150 == 0) {
		say("hakios: mediaserverd: the %dx%d encoder: frame %u out (%u dropped)\n", w->width, w->height, w->frames, w->dropped);
	}
	if (w->callback != NULL) {
		w->callback(w->refcon, frame_refcon, status, flags, buffer);
	}
}

static OSStatus
compression_session_create(CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec,
    CFDictionaryRef specification, CFDictionaryRef source_attributes, CFAllocatorRef data_allocator,
    VTCompressionOutputCallback callback, void *refcon, VTCompressionSessionRef *out)
{
	struct encoder_watch *w = calloc(1, sizeof(*w));     /* (for the session's life: a few bytes a recording) */
	OSStatus status;

	w->callback = callback;
	w->refcon = refcon;
	w->width = width;
	w->height = height;
	w->codec = codec;
	status = VTCompressionSessionCreate(allocator, width, height, codec, specification, source_attributes, data_allocator,
	    callback != NULL ? encoder_output : NULL, callback != NULL ? w : refcon, out);
	if (status == 0 && out != NULL && *out != NULL) {
		OSStatus a = VTSessionSetProperty(*out, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		OSStatus b = VTSessionSetProperty(*out, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
		int zero = 0;
		CFNumberRef none = CFNumberCreate(NULL, kCFNumberIntType, &zero);
		OSStatus c = VTSessionSetProperty(*out, kVTCompressionPropertyKey_MaxFrameDelayCount, none);
		OSStatus d = VTSessionSetProperty(*out, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);

		CFRelease(none);
		say("hakios: mediaserverd: a %c%c%c%c encoder session, %dx%d: real time (%d), no reordering (%d), no frame delay (%d), "
		    "speed first (%d)\n", (char)(codec >> 24), (char)(codec >> 16), (char)(codec >> 8), (char)codec, width, height,
		    (int)a, (int)b, (int)c, (int)d);
	} else {
		say("hakios: mediaserverd: a %c%c%c%c encoder session, %dx%d: NOT MADE (%d)\n", (char)(codec >> 24), (char)(codec >> 16),
		    (char)(codec >> 8), (char)codec, width, height, (int)status);
		free(w);
	}
	return status;
}

/* (an application's settings after the session's own: the encoder's asks for reordering again are refused) */
static OSStatus
session_set_property(VTSessionRef session, CFStringRef key, CFTypeRef value)
{
	if (key != NULL && value != NULL && CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering) && CFEqual(value, kCFBooleanTrue)) {
		value = kCFBooleanFalse;
	} else if (key != NULL && value != NULL && CFEqual(key, kVTCompressionPropertyKey_RealTime) && CFEqual(value, kCFBooleanFalse)) {
		value = kCFBooleanTrue;
	}
	return VTSessionSetProperty(session, key, value);
}

__attribute__((used)) static const struct { const void *replacement, *original; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)compression_session_create, (const void *)VTCompressionSessionCreate },
	{ (const void *)session_set_property, (const void *)VTSessionSetProperty },
};

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("HakiosFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("HakiosFlashlightDevice", NULL, 0);
	stream_class = make_class("HakiosFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The portrait mode's pipeline of AppleCVA (+[CVAVideoPipelineLibrary portraitVideoPipelineWithProperties:...] and the
 * disparity and matting ones) is Metal from end to end -- and asserts at once without a device. Ours stands in
 * (portrait=1): an object that answers whatever the pipeline's class would, for now by saying what it was asked and
 * returning zero; our own disparity and blur go behind it (camera-app.md, "own depth and blur").
 */
static Class portrait_pipeline_class;

static id
portrait_signature(__unused id self, __unused SEL selector, SEL wanted)
{
	Class real = objc_getClass("CVAPortraitVideoPipeline_Impl");
	Method method = real != NULL ? class_getInstanceMethod(real, wanted) : NULL;
	const char *types = method != NULL ? method_getTypeEncoding(method) : "@@:";

	return ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSMethodSignature"),
	    sel_registerName("signatureWithObjCTypes:"), types);
}

static BOOL
portrait_responds(__unused id self, __unused SEL selector, SEL wanted)
{
	Class real = objc_getClass("CVAPortraitVideoPipeline_Impl");

	return class_getInstanceMethod(object_getClass(self), wanted) != NULL ||
	    (real != NULL && class_getInstanceMethod(real, wanted) != NULL);
}

static void
portrait_forward(__unused id self, __unused SEL selector, id invocation)
{
	static struct { SEL selector; unsigned count; } seen[32];
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	SEL asked = ((SEL (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("selector"));
	id signature = ((id (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("methodSignature"));
	unsigned long length = ((unsigned long (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnLength"));
	unsigned i, count = 0;

	pthread_mutex_lock(&lock);
	for (i = 0; i < sizeof(seen) / sizeof(seen[0]) && seen[i].selector != NULL && seen[i].selector != asked; i++) {
	}
	if (i < sizeof(seen) / sizeof(seen[0])) {
		seen[i].selector = asked;
		count = ++seen[i].count;
	}
	pthread_mutex_unlock(&lock);
	if (count == 1 || count % 100 == 0) {
		say("hakios: portrait: the pipeline is asked -%s (#%u): answered with zero\n", sel_getName(asked), count);
	}
	if (length != 0 && length <= 64) {
		char zero[64] = { 0 };

		((void (*)(id, SEL, void *))objc_msgSend)(invocation, sel_registerName("setReturnValue:"), zero);
	}
}

static id
portrait_pipeline(__unused id cls, SEL selector, __unused id properties, id *error)
{
	id pipeline;

	if (portrait_pipeline_class == NULL) {
		static const struct method_spec methods[] = {
			{ "methodSignatureForSelector:", "@@::", (IMP)portrait_signature },
			{ "respondsToSelector:", "B@::", (IMP)portrait_responds },
			{ "forwardInvocation:", "v@:@", (IMP)portrait_forward },
		};

		portrait_pipeline_class = make_class("HakiosPortraitPipeline", methods, sizeof(methods) / sizeof(methods[0]));
	}
	if (strstr(sel_getName(selector), "error:") != NULL && error != NULL) {
		*error = nil;
	}
	pipeline = ((id (*)(id, SEL))objc_msgSend)(class_createInstance(portrait_pipeline_class, 0), sel_registerName("init"));
	say("hakios: portrait: +[CVAVideoPipelineLibrary %s]: ours (%p)\n", sel_getName(selector), (void *)pipeline);
	return ((id (*)(id, SEL))objc_msgSend)(pipeline, sel_registerName("autorelease"));
}

/*
 * The portrait photograph. With both cameras the shutter's settings carry a stream's settings for each, and the
 * device then refuses (-12780) unless it was told one of stereo fusion, constituent photos or depth -- which nothing
 * here tells it. Constituent photos (a plain flag: each camera's picture of its own) is what this is, so it is set.
 */
static IMP original_capture_still_now;

static int
capture_still_now(id self, SEL selector, id settings, id capture_settings)
{
	static bool said;
	id streams = ((id (*)(id, SEL))objc_msgSend)(capture_settings, sel_registerName("captureStreamSettings"));
	unsigned long n = streams ? ((unsigned long (*)(id, SEL))objc_msgSend)(streams, sel_registerName("count")) : 0;
	int r;

	if (n >= 2 && !((BOOL (*)(id, SEL))objc_msgSend)(self, sel_registerName("bravoConstituentPhotoDeliveryEnabled"))) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(self, sel_registerName("setBravoConstituentPhotoDeliveryEnabled:"), YES);
		if (!said) {
			said = true;
			say("hakios: portrait: shutter with %lu cameras' settings: constituent photos on\n", n);
		}
	}
	/* the photograph's depth (ours: attach_portrait_depth) is embedded only if what was asked for asks for it */
	{
		SEL requested = sel_registerName("requestedSettings"), on = sel_registerName("setDepthDataDeliveryEnabled:");
		SEL embed = sel_registerName("setEmbedsDepthDataInImage:");
		/* (boot 29: what comes is what was asked for itself, a FigCaptureIrisStillImageSettings) */
		id asked = settings != nil && class_respondsToSelector(object_getClass(settings), requested) ?
		    ((id (*)(id, SEL))objc_msgSend)(settings, requested) : settings;

		if (asked != nil && class_respondsToSelector(object_getClass(asked), on) &&
		    class_respondsToSelector(object_getClass(asked), embed)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(asked, on, YES);
			((void (*)(id, SEL, BOOL))objc_msgSend)(asked, embed, YES);
		} else {
			say("hakios: portrait: the photograph's settings (%s) not told to keep depth\n",
			    asked != nil ? object_getClassName(asked) : settings != nil ? object_getClassName(settings) : "none");
		}
	}
	r = ((int (*)(id, SEL, id, id))original_capture_still_now)(self, selector, settings, capture_settings);
	if (r != 0) {
		say("hakios: portrait: shutter refused (%d), %lu cameras' settings\n", r, n);
	}
	return r;
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in Darwin userland 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
	bool portrait;                  /* only with boot-arg portrait=1 */
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0x1a1eefa18UL, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0x1a1e99ef8UL,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0x1a20d8f48UL,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0x1a1f533dcUL,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0x1a1ec83b8UL, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0x1a1ea37e0UL, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
	/* the portrait mode's pipelines (portrait=1: see portrait_pipeline) */
	{ "CVAVideoPipelineLibrary", "portraitVideoPipelineWithProperties:error:", 0x1af40f058UL, (IMP)portrait_pipeline, NULL,
	  false, false, true },
	{ "CVAVideoPipelineLibrary", "portraitVideoPipelineWithProperties:", 0x1af4108ccUL, (IMP)portrait_pipeline, NULL,
	  false, false, true },
	{ "CVAVideoPipelineLibrary", "disparityPostprocessingVideoPipelineWithProperties:error:", 0x1af40f118UL,
	  (IMP)portrait_pipeline, NULL, false, false, true },
	{ "CVAVideoPipelineLibrary", "disparityPostprocessingVideoPipelineWithProperties:", 0x1af410a6cUL, (IMP)portrait_pipeline,
	  NULL, false, false, true },
	{ "CVAVideoPipelineLibrary", "mattingVideoPipelineWithProperties:error:", 0x1af40f0b8UL, (IMP)portrait_pipeline, NULL,
	  false, false, true },
	{ "CVAVideoPipelineLibrary", "mattingVideoPipelineWithProperties:", 0x1af41099cUL, (IMP)portrait_pipeline, NULL,
	  false, false, true },
	{ "BWFigVideoCaptureDevice", "captureStillImageNowWithSettings:captureSettings:", 0x1a2149c00UL,
	  (IMP)capture_still_now, &original_capture_still_now, true, false, true },
	/* bring-up aids for video (expected 0: whatever is there, as both only look and pass on) */
	{ "BWFileCoordinatorNode", "renderSampleBuffer:forInput:", 0, (IMP)coordinator_render, &original_coordinator_render,
	  true, false },
	{ "BWQuickTimeMovieFileSinkNode", "renderSampleBuffer:forInput:", 0, (IMP)movie_sink_render,
	  &original_movie_sink_render, true, false },
};

/*
 * Bring-up aid for the portrait mode (boot-arg portrait_dump=1): what the capture stack's depth and portrait
 * pieces are made of, written once each as their images arrive -- methods with their types and their addresses
 * before the slide (to be read in the extracted cache), instance variables, protocols' methods -- and which of
 * CMCapture's classes keep one of AppleCVA's objects. Our own disparity and blur go in their place (camera-app.md).
 */
static void
dump_line(const char *format, ...)
{
	char line[600];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line) - 1, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}

static void
dump_methods(const char *name, Class cls, char kind)
{
	unsigned n = 0;
	Method *methods = class_copyMethodList(cls, &n);

	for (unsigned i = 0; i < n; i++) {
		const char *types = method_getTypeEncoding(methods[i]);

		dump_line("pdump: %s %c%s %#lx %s", name, kind, sel_getName(method_getName(methods[i])),
		    (unsigned long)((uintptr_t)method_getImplementation(methods[i]) - slide), types ? types : "?");
	}
	free(methods);
}

static void
dump_class(const char *name)
{
	Class cls = objc_getClass(name);
	unsigned n = 0;
	Ivar *ivars;

	if (cls == NULL) {
		return;
	}
	dump_line("pdump: class %s : %s (%s)", name, class_getSuperclass(cls) ? class_getName(class_getSuperclass(cls)) : "-",
	    class_getImageName(cls) ? class_getImageName(cls) : "?");
	dump_methods(name, object_getClass((id)cls), '+');
	dump_methods(name, cls, '-');
	ivars = class_copyIvarList(cls, &n);
	for (unsigned i = 0; i < n; i++) {
		dump_line("pdump: %s ivar %s @%ld %s", name, ivar_getName(ivars[i]), (long)ivar_getOffset(ivars[i]),
		    ivar_getTypeEncoding(ivars[i]) ? ivar_getTypeEncoding(ivars[i]) : "?");
	}
	free(ivars);
}

static void
dump_protocol(const char *name)
{
	Protocol *protocol = objc_getProtocol(name);

	if (protocol == NULL) {
		return;
	}
	for (int required = 1; required >= 0; required--) {
		for (int instance = 1; instance >= 0; instance--) {
			unsigned n = 0;
			struct objc_method_description *m = protocol_copyMethodDescriptionList(protocol, required, instance, &n);

			for (unsigned i = 0; i < n; i++) {
				dump_line("pdump: <%s> %s%c%s %s", name, required ? "" : "optional ", instance ? '-' : '+',
				    sel_getName(m[i].name), m[i].types ? m[i].types : "?");
			}
			free(m);
		}
	}
}

/*
 * A return address (before the slide) named by the nearest Objective-C method at or below it in the capture stack's
 * frameworks (CMCapture, MediaToolbox, AppleCVA*): good enough to find one's way in the extracted cache, where the
 * symbols are gone. Used by camera_device.c's bring-up stack traces (portrait=1).
 */
struct imp_name { uintptr_t imp; const char *cls, *sel; char kind; };
static struct imp_name *imp_names;
static size_t imp_count;

static int
imp_order(const void *a, const void *b)
{
	uintptr_t x = ((const struct imp_name *)a)->imp, y = ((const struct imp_name *)b)->imp;

	return x < y ? -1 : x > y;
}

static void
imp_collect(Class cls, char kind, size_t *capacity)
{
	unsigned n = 0;
	Method *methods = class_copyMethodList(cls, &n);

	for (unsigned i = 0; i < n; i++) {
		if (imp_count == *capacity) {
			*capacity = *capacity ? *capacity * 2 : 8192;
			imp_names = realloc(imp_names, *capacity * sizeof(*imp_names));
		}
		imp_names[imp_count++] = (struct imp_name){ (uintptr_t)method_getImplementation(methods[i]) - slide,
		    class_getName(cls), sel_getName(method_getName(methods[i])), kind };
	}
	free(methods);
}

void hakios_media_symbol(uintptr_t unslid, char *out, size_t size);

void
hakios_media_symbol(uintptr_t unslid, char *out, size_t size)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	size_t lo = 0, hi;

	pthread_mutex_lock(&lock);
	if (imp_names == NULL) {
		unsigned count = 0;
		Class *classes = objc_copyClassList(&count);
		size_t capacity = 0;

		for (unsigned i = 0; i < count; i++) {
			const char *image = class_getImageName(classes[i]);

			if (image != NULL && (strstr(image, "CMCapture") || strstr(image, "MediaToolbox") || strstr(image, "AppleCVA"))) {
				imp_collect(classes[i], '-', &capacity);
				imp_collect(object_getClass((id)classes[i]), '+', &capacity);
			}
		}
		free(classes);
		qsort(imp_names, imp_count, sizeof(*imp_names), imp_order);
	}
	hi = imp_count;
	while (lo < hi) {               /* the first above it */
		size_t mid = (lo + hi) / 2;

		if (imp_names[mid].imp <= unslid) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0 || unslid - imp_names[lo - 1].imp > 0x4000) {
		snprintf(out, size, "%#lx", (unsigned long)unslid);
	} else {
		struct imp_name *m = &imp_names[lo - 1];

		snprintf(out, size, "%c[%s %s]+%#lx", m->kind, m->cls, m->sel, (unsigned long)(unslid - m->imp));
	}
	pthread_mutex_unlock(&lock);
}

/*
 * Bring-up aid (boot-arg mtrace=ClassA,ClassB,...): every method of those classes (their own, not inherited) goes
 * through one trampoline that says which was called and jumps on to it. No return values: the order is what is
 * wanted (what the capture stack did last before giving the streams back).
 */
struct traced { Class cls; SEL sel; IMP original; unsigned count; };
static struct traced *traced;
static size_t traced_count;

void hk_trace_tramp(void);
__asm__(
    ".text\n.align 4\n.globl _hk_trace_tramp\n_hk_trace_tramp:\n"
    "stp x29, x30, [sp, #-16]!\nmov x29, sp\nsub sp, sp, #208\n"
    "stp x0, x1, [sp]\nstp x2, x3, [sp, #16]\nstp x4, x5, [sp, #32]\nstp x6, x7, [sp, #48]\nstr x8, [sp, #64]\n"
    "stp q0, q1, [sp, #80]\nstp q2, q3, [sp, #112]\nstp q4, q5, [sp, #144]\nstp q6, q7, [sp, #176]\n"
    "bl _hk_trace_enter\nmov x16, x0\n"
    "ldp x0, x1, [sp]\nldp x2, x3, [sp, #16]\nldp x4, x5, [sp, #32]\nldp x6, x7, [sp, #48]\nldr x8, [sp, #64]\n"
    "ldp q0, q1, [sp, #80]\nldp q2, q3, [sp, #112]\nldp q4, q5, [sp, #144]\nldp q6, q7, [sp, #176]\n"
    "blr x16\n"
    "stp x0, x1, [sp]\nstp q0, q1, [sp, #80]\nstp q2, q3, [sp, #112]\n"
    "bl _hk_trace_exit\n"
    "ldp x0, x1, [sp]\nldp q0, q1, [sp, #80]\nldp q2, q3, [sp, #112]\n"
    "add sp, sp, #208\nldp x29, x30, [sp], #16\nret\n");

IMP hk_trace_enter(id self, SEL sel, uintptr_t a2, uintptr_t a3);
void hk_trace_exit(uintptr_t r0);

/* (what each thread has entered and not yet left: the exit says what came back) */
struct trace_frame { struct traced *t; unsigned n; uintptr_t a3; bool hot; };
static __thread struct trace_frame trace_stack[64];
static __thread unsigned trace_depth;

static void
trace_object(const char *what, uintptr_t value, char *out, size_t size)
{
	CFStringRef d;
	char text[300] = "";

	if (value == 0) {
		snprintf(out, size, "%s nil", what);
		return;
	}
	if (value >= 0x100000000UL && value < (1UL << 40) && (value & 7) == 0 && (d = CFCopyDescription((CFTypeRef)value)) != NULL) {
		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
		for (char *c = text; *c != '\0'; c++) {
			if (*c == '\n') {
				*c = ' ';
			}
		}
	}
	snprintf(out, size, "%s %#lx %s", what, (unsigned long)value, text);
}

/* (after the first setActiveStreams:, every traced call for a while: the pair's session start) */
static int mtrace_hot, mtrace_hot_done;

IMP
hk_trace_enter(id self, SEL sel, uintptr_t a2, uintptr_t a3)
{
	Class cls = object_getClass(self);
	bool meta = class_isMetaClass(cls);

	for (Class c = cls; c != NULL; c = class_getSuperclass(c)) {
		for (size_t i = 0; i < traced_count; i++) {
			struct traced *t = &traced[i];

			if (t->cls == c && t->sel == sel) {
				unsigned n = __atomic_add_fetch(&t->count, 1, __ATOMIC_RELAXED);
				const char *name = sel_getName(sel);

				bool hot = false;

				if (strcmp(name, "setActiveStreams:") == 0 && mtrace_hot_done == 0) {
					mtrace_hot = 400;
					mtrace_hot_done = 1;
				}
				if (mtrace_hot > 0) {
					hot = true;
					mtrace_hot--;
				}
				if (n <= 8 || n % 500 == 0 || hot) {
					char arg[400] = "";

					if (strncmp(name, "copy", 4) == 0 || strncmp(name, "request", 7) == 0 ||
					    strncmp(name, "relinquish", 10) == 0 || strncmp(name, "set", 3) == 0 || strncmp(name, "get", 3) == 0) {
						trace_object(" arg", a2, arg, sizeof(arg));
					}
					say("hakios: mtrace: %c[%s %s] #%u%s%s%s\n", meta ? '+' : '-', class_getName(c), name, n,
					    c != cls ? " on a " : "", c != cls ? class_getName(cls) : "", arg);
				}
				if (trace_depth < 64) {
					trace_stack[trace_depth] = (struct trace_frame){ t, n, a3, hot };
				}
				trace_depth++;
				return t->original;
			}
		}
	}
	say("hakios: mtrace: no original for %s\n", sel_getName(sel));
	abort();
}

void
hk_trace_exit(uintptr_t r0)
{
	struct trace_frame *f;
	const char *name;

	if (trace_depth == 0) {
		return;
	}
	if (--trace_depth >= 64) {
		return;
	}
	f = &trace_stack[trace_depth];
	name = sel_getName(f->t->sel);
	if (f->hot || ((f->n <= 8) && (strncmp(name, "copy", 4) == 0 || strncmp(name, "get", 3) == 0 || strncmp(name, "request", 7) == 0 || strstr(name, "Group") != NULL ||
	    strcmp(name, "streams") == 0 || strncmp(name, "init", 4) == 0 || strstr(name, "till") != NULL))) {
		char ret[400];
		int err = 0;

		/* (the still image's: mostly states and flags, numbers) */
		if (strncmp(name, "init", 4) == 0 || strstr(name, "till") != NULL || (f->n > 8 && r0 != 0 && malloc_zone_from_ptr((const void *)r0) == NULL)) {
			snprintf(ret, sizeof(ret), "-> %#lx", (unsigned long)r0);
		} else {
			trace_object("->", r0, ret, sizeof(ret));
		}
		/* (not after a CMTime: that goes by address in its place, and the error is a plain number after it) */
		if (strstr(name, "error:") != NULL && strstr(name, "PTS:") == NULL && f->a3 >= 0x100000000UL && f->a3 < (1UL << 40) && (f->a3 & 3) == 0) {
			err = *(int *)f->a3;
		}
		say("hakios: mtrace:   %s #%u %s%s\n", name, f->n, ret, err != 0 ? " (error set)" : "");
		if (err != 0) {
			say("hakios: mtrace:     error %d\n", err);
		}
	}
}

static char mtrace_sels[256];   /* boot-arg mtrace_sel=a,b: only the methods whose names have one of these in them */

static void
mtrace_class(Class cls)
{
	unsigned n = 0;
	Method *methods = class_copyMethodList(cls, &n);

	for (unsigned i = 0; i < n; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (strcmp(name, "dealloc") == 0 || strcmp(name, ".cxx_destruct") == 0 || strcmp(name, ".cxx_construct") == 0 ||
		    strcmp(name, "retain") == 0 || strcmp(name, "release") == 0 || strcmp(name, "autorelease") == 0 ||
		    strcmp(name, "retainCount") == 0 || strcmp(name, "_tryRetain") == 0 || strcmp(name, "_isDeallocating") == 0 ||
		    strcmp(name, "allowsWeakReference") == 0 || strcmp(name, "retainWeakReference") == 0 ||
		    strcmp(name, "initialize") == 0 || strcmp(name, "load") == 0) {
			continue;
		}
		/* more than 6 arguments: some on the stack, which the trampoline's frame moves (mediaserverd died of SIGBUS) */
		unsigned colons = 0;

		for (const char *p = name; *p != '\0'; p++) {
			colons += *p == ':';
		}
		if (colons > 6) {
			continue;
		}
		if (mtrace_sels[0] != '\0') {
			char sels[sizeof(mtrace_sels)], *rest = sels, *part;
			bool wanted = false;

			strlcpy(sels, mtrace_sels, sizeof(sels));
			while (!wanted && (part = strsep(&rest, ",")) != NULL) {
				wanted = *part != '\0' && strstr(name, part) != NULL;
			}
			if (!wanted) {
				continue;
			}
		}
		if (mtrace_sels[0] != '\0') {
			/* (a filtered few: where they are, for the disassembly) */
			say("hakios: mtrace: %c[%s %s] at %#lx (unslid)\n", class_isMetaClass(cls) ? '+' : '-', class_getName(cls), name,
			    (unsigned long)((uintptr_t)method_getImplementation(methods[i]) - slide));
		}
		traced = realloc(traced, (traced_count + 1) * sizeof(*traced));
		traced[traced_count++] = (struct traced){ cls, method_getName(methods[i]), method_getImplementation(methods[i]), 0 };
	}
	free(methods);
}

static char mtrace_names[512];
static bool mtrace_done[16];

static void
mtrace_install(void)
{
	char names[512], *next = names, *name;
	unsigned k = 0;

	strlcpy(names, mtrace_names, sizeof(names));
	while ((name = strsep(&next, ",")) != NULL && k < 16) {
		Class cls;
		size_t first;

		if (mtrace_done[k] || *name == '\0' || (cls = objc_getClass(name)) == NULL) {
			k++;
			continue;
		}
		mtrace_done[k++] = true;
		first = traced_count;
		mtrace_class(cls);
		mtrace_class(object_getClass((id)cls));
		for (size_t i = first; i < traced_count; i++) {
			Method m = class_isMetaClass(traced[i].cls) ? class_getClassMethod(cls, traced[i].sel) :
			    class_getInstanceMethod(cls, traced[i].sel);

			method_setImplementation(m, (IMP)hk_trace_tramp);
		}
		say("hakios: mtrace: %s: %zu methods traced\n", name, traced_count - first);
	}
}

static const char *const dumped_classes[] = {
	"CVAPortraitVideoPipeline", "CVAPortraitVideoPipeline_Impl", "CVAVideoPipelineLibrary", "CVAVideoPipelineDevice",
	"CVADisparityPostprocessingRequest", "BWVideoDepthNode", "BWStillImageBravoDisparityNode", "BWStillImageMetalSDOFRenderer",
	"BWStereoDisparityProcessorController", "BWStereoDisparityProcessorControllerConfiguration", "BWStereoDisparityRequest",
	"BWStereoDisparityProcessorInput", "BWFastStereoDisparityConfiguration", "BWVideoSDOFSplitNode",
	"BWStillImageDisparitySplitterNode", "BWStillImagePortraitMetadataNode", "BWDepthConverterNode",
	"BWDepthSynchronizerNode", "BWDisparityPostProcessingInferenceProvider", "BWStereoFusionNode", "BWStreamingCVAFilterRenderer",
	"BWCrossoverNode", "BWMultiStreamCameraSourceNode",
};
static const char *const dumped_protocols[] = {
	"CVAPortraitVideoPipeline", "CVAPortraitGenericRequest", "CVAPortraitResult", "CVADisparityPostprocessingResult",
	"BWStereoDisparityProcessorInputDelegate",
};
static bool dumped_class[sizeof(dumped_classes) / sizeof(dumped_classes[0])];
static bool dumped_protocol[sizeof(dumped_protocols) / sizeof(dumped_protocols[0])];
static bool dumped_holders;

/* CMCapture's classes with an instance variable of AppleCVA's (by its type: "@\"<CVA..." or "@\"CVA...") */
static void
dump_holders(void)
{
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);

	for (unsigned i = 0; i < count; i++) {
		const char *image = class_getImageName(classes[i]);
		unsigned n = 0;
		Ivar *ivars;

		if (image == NULL || strstr(image, "CMCapture") == NULL) {
			continue;
		}
		ivars = class_copyIvarList(classes[i], &n);
		for (unsigned j = 0; j < n; j++) {
			const char *type = ivar_getTypeEncoding(ivars[j]);

			if (type != NULL && (strstr(type, "\"CVA") != NULL || strstr(type, "\"<CVA") != NULL)) {
				dump_line("pdump: holder %s ivar %s %s", class_getName(classes[i]), ivar_getName(ivars[j]), type);
			}
		}
		free(ivars);
	}
	free(classes);
}

static void
dump_portrait(void)
{
	for (size_t i = 0; i < sizeof(dumped_classes) / sizeof(dumped_classes[0]); i++) {
		if (!dumped_class[i] && objc_getClass(dumped_classes[i]) != NULL) {
			dumped_class[i] = true;
			dump_class(dumped_classes[i]);
		}
	}
	for (size_t i = 0; i < sizeof(dumped_protocols) / sizeof(dumped_protocols[0]); i++) {
		if (!dumped_protocol[i] && objc_getProtocol(dumped_protocols[i]) != NULL) {
			dumped_protocol[i] = true;
			dump_protocol(dumped_protocols[i]);
		}
	}
	if (!dumped_holders && objc_getClass("BWVideoDepthNode") != NULL) {
		dumped_holders = true;
		dump_holders();
	}
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	if (portrait_dump) {
		dump_portrait();
	}
	if (mtrace_names[0] != '\0') {
		mtrace_install();
	}
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (r->portrait && !portrait_on) || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (r->expected == 0 && method != NULL && r->instance) {
			unsigned n = 0;
			Method *own = class_copyMethodList(cls, &n);
			bool found = false;

			for (unsigned j = 0; j < n; j++) {
				found |= own[j] == method;
			}
			free(own);
			if (!found) {               /* inherited: replacing it would change every node's */
				say("hakios: -[%s %s] is inherited; left alone\n", r->class_name, r->selector);
				continue;
			}
		}
		if (original == NULL || (r->expected != 0 && (uintptr_t)original != r->expected + slide)) {
			say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
			    r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("hakios: mediaserverd: %c[%s %s] replaced\n", r->instance ? '-' : '+', r->class_name, r->selector);
	}
}

__attribute__((constructor))
static void
hakios_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
			portrait_dump = strstr(args, "portrait_dump=1") != NULL;
			portrait_on = strstr(args, "portrait=1") != NULL;
			{
				const char *m = strstr(args, "mtrace=");

				if (m != NULL) {
					size_t len = strcspn(m + 7, " ");

					if (len >= sizeof(mtrace_names)) {
						len = sizeof(mtrace_names) - 1;
					}
					memcpy(mtrace_names, m + 7, len);
					mtrace_names[len] = '\0';
				}
				m = strstr(args, "mtrace_sel=");
				if (m != NULL) {
					size_t len = strcspn(m + 11, " ");

					if (len >= sizeof(mtrace_sels)) {
						len = sizeof(mtrace_sels) - 1;
					}
					memcpy(mtrace_sels, m + 11, len);
					mtrace_sels[len] = '\0';
				}
			}
		}
	}
	torch = open("/dev/hakios-torch", O_WRONLY);
	camera = access("/dev/hakios-camera", F_OK) == 0;
	no_microphone = access("/dev/hakios-mic", F_OK) != 0;
	if (torch >= 0) {
		flashlight_classes();
		say("hakios: mediaserverd: the kernel has a flashlight (/dev/hakios-torch)\n");
	}
	if (camera) {
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
		register_hevc_encoder();
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
