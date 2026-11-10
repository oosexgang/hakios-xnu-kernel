/*
 * libredwood-media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
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
 * and the stock wallpaper of iOS 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/redwood-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/redwood-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP. NOT FINISHED: no frames reach
 * applications yet (see camera_device.c). Its stream takes the torch level too, so FigFlashlight
 * would need nothing of its own then (not tried yet).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

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
uintptr_t redwood_cache_slide(void);
uintptr_t
redwood_cache_slide(void)
{
	return slide;
}

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/redwood-torch */
static bool camera;                     /* /dev/redwood-camera is there */
extern int redwood_camera_device_create(const void *allocator, const void *options, void **out);
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
	if (!camera && access("/dev/redwood-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)redwood_camera_device_create);
			say("redwood: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("redwood: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
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
		say("redwood: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/redwood-torch)");
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

	vendor_class = make_class("RedwoodFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("RedwoodFlashlightDevice", NULL, 0);
	stream_class = make_class("RedwoodFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in iOS 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false },
};

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (uintptr_t)original != r->expected + slide) {
			say("redwood: +[%s %s]: found %#lx, expected %#lx; left alone\n", r->class_name, r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: mediaserverd: +[%s %s] replaced\n", r->class_name, r->selector);
	}
}

/*
 * 40 s (boot-arg media_threads=SECONDS; 0: never) after the daemon starts, every thread of it in one kept
 * line each -- its first frames that are not the kernel's, dispatch's or pthread's, by symbol. On the
 * Infinix mediaserverd answers nobody: SpringBoard waits for ever for its system controller and a
 * poster for a decompression session (runs 253, 259), and this says what the daemon is doing instead.
 */
static void __attribute__((unused))
frame_name(uintptr_t address, char *out, size_t size)
{
	Dl_info info;
	const char *image;

	if (dladdr((void *)address, &info) && info.dli_fname != NULL) {
		image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
		snprintf(out, size, "%s+%#lx(%.14s)", info.dli_sname ? info.dli_sname : "?",
		    info.dli_saddr ? (unsigned long)(address - (uintptr_t)info.dli_saddr) : 0UL, image);
	} else {
		snprintf(out, size, "%#lx", (unsigned long)address);
	}
}

static int __attribute__((unused))
frame_boring(uintptr_t address)
{
	Dl_info info;

	return dladdr((void *)address, &info) && info.dli_fname != NULL && (strstr(info.dli_fname, "libsystem_kernel") ||
	    strstr(info.dli_fname, "libdispatch") || strstr(info.dli_fname, "libsystem_pthread"));
}

static void *
media_threads(void *argument)
{
	thread_act_array_t threads;
	mach_msg_type_number_t count;

	sleep((unsigned)(uintptr_t)argument);
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return NULL;
	}
	for (unsigned i = 0; i < count && i < 40; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		uintptr_t frames[24];
		unsigned depth = 0;

		if (threads[i] == mach_thread_self() || thread_suspend(threads[i]) != KERN_SUCCESS) {
			continue;
		}
		/* the addresses only while it is stopped (it may hold a lock that dladdr wants), the names after */
		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &n) == KERN_SUCCESS) {
			uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

			frames[depth++] = (uintptr_t)arm_thread_state64_get_pc(state);
			while (depth < 24 && fp != 0 && (fp & 7) == 0) {
				uintptr_t next = ((uintptr_t *)fp)[0];

				frames[depth++] = ((uintptr_t *)fp)[1] & 0x0000000fffffffffULL;
				fp = next > fp ? next : 0;
			}
		}
		thread_resume(threads[i]);
		if (depth != 0) {
			/* no names: a daemon stuck holding dyld's lock would stop dladdr here too. The addresses are
			 * unslid, for the shared cache to name them on the Mac (tools/cache-name.py) */
			char line[200];
			size_t used = (size_t)snprintf(line, sizeof(line), "!media t%u", i);

			for (unsigned k = 0; k < depth && k < 11 && used < sizeof(line); k++) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %lx", (unsigned long)(frames[k] - slide));
			}
			sysctlbyname("kern.redwood.say", NULL, NULL, line, strnlen(line, sizeof(line)));
		}
	}
	return NULL;
}

/*
 * Inside Camera.app (inserted by the diagnostic image only: the line "libredwood-media.dylib mediaserverd Camera" of etc/inject.conf): what the app does with a
 * shutter tap and what it is told. mediaserverd's capture graph compresses the photo (run 581) and nothing reaches DCIM (runs 582, 583); the part between is the
 * app's: -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] and the delegate's callbacks, with their errors, said to kern.redwood.host_keep.
 */
static void
host_line(const char *format, ...)
{
	char line[400];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strnlen(line, sizeof(line)));
}

/* a long text as lines of its own (the host buffer takes 400 bytes a line) */
static void
host_long(const char *prefix, const char *text)
{
	size_t length = strlen(text), at = 0;

	for (unsigned part = 0; at < length && part < 8; part++, at += 300) {
		host_line("%s [%u] %.300s", prefix, part, text + at);
	}
}

static const char *
text_of_object(id object)
{
	id text = object ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description")) : NULL;

	return text ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "-";
}

/* the delegate's callbacks that carry (output, object, error) or (output, object): each gets a line, then the original */
static const char *const cb3[] = {
	"captureOutput:didFinishProcessingPhoto:error:", "captureOutput:didFinishCaptureForResolvedSettings:error:",
	"captureOutput:didFinishRecordingLivePhotoMovieForEventualFileAtURL:resolvedSettings:", "captureOutput:didFinishProcessingLivePhotoToMovieFileAtURL:duration:photoDisplayTime:resolvedSettings:error:",
};
static const char *const cb2[] = {
	"captureOutput:willBeginCaptureForResolvedSettings:", "captureOutput:willCapturePhotoForResolvedSettings:", "captureOutput:didCapturePhotoForResolvedSettings:",
};

static void
wrap_delegate(Class cls)
{
	static Class done[16];
	static unsigned ndone;

	for (unsigned i = 0; i < ndone; i++) {
		if (done[i] == cls) {
			return;
		}
	}
	if (ndone < 16) {
		done[ndone++] = cls;
	}
	for (unsigned k = 0; k < sizeof(cb3) / sizeof(cb3[0]); k++) {
		SEL sel = sel_registerName(cb3[k]);
		Method m = class_getInstanceMethod(cls, sel);

		if (m == NULL) {
			continue;
		}
		const char *name = cb3[k];
		IMP orig = method_getImplementation(m);
		unsigned nargs = method_getNumberOfArguments(m);

		if (nargs == 5) {
			method_setImplementation(m, imp_implementationWithBlock((id)^(id me, id output, id object, id error) {
				host_long(error ? "camera app: error" : "camera app: no error", error ? text_of_object(error) : name);
				((void (*)(id, SEL, id, id, id))orig)(me, sel, output, object, error);
			}));
		} else if (nargs == 4) {
			method_setImplementation(m, imp_implementationWithBlock((id)^(id me, id output, id object) {
				host_line("camera app: %s %s", class_getName(cls), name);
				((void (*)(id, SEL, id, id))orig)(me, sel, output, object);
			}));
		}
	}
	for (unsigned k = 0; k < sizeof(cb2) / sizeof(cb2[0]); k++) {
		SEL sel = sel_registerName(cb2[k]);
		Method m = class_getInstanceMethod(cls, sel);

		if (m == NULL || method_getNumberOfArguments(m) != 4) {
			continue;
		}
		const char *name = cb2[k];
		IMP orig = method_getImplementation(m);

		method_setImplementation(m, imp_implementationWithBlock((id)^(id me, id output, id object) {
			host_line("camera app: %s %s", class_getName(cls), name);
			((void (*)(id, SEL, id, id))orig)(me, sel, output, object);
		}));
	}
	host_line("camera app: delegate class %s wrapped", class_getName(cls));
}

/*
 * Run 585: addMainImage:metadata:options: answers -12908 (kVTCouldNotFindVideoEncoderErr) for the HEIC photos Camera asks for (hvc1): this phone has no hardware HEVC
 * encoder and no VideoToolbox plug-in for one. Run 586: emptying the output's codec list for HEIC only stopped the app's capture. The app's own switches are the ones to
 * turn: CameraUI's "HEVC encoding supported" and "prefer HEVC" answers become NO, so that it asks for JPEG.
 */
static BOOL
answer_no(__unused id me, __unused SEL sel)
{
	return NO;
}

static void
no_hevc_in_camera_ui(void)
{
	static const char *const names[] = { "isHEVCEncodingSupported", "HEVCEncodingSupported", "preferHEVC", "_preferHEVC", "_preferHEVCWhenAvailable", "preferHEVCWhenAvailable" };
	unsigned count = 0, changed = 0;
	Class *list = objc_copyClassList(&count);

	for (unsigned i = 0; i < count; i++) {
		const char *image = class_getImageName(list[i]);

		if (image == NULL || strstr(image, "CameraUI") == NULL) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
			Method m = class_getInstanceMethod(list[i], sel_registerName(names[k]));
			const char *type = m ? method_getTypeEncoding(m) : NULL;

			if (m != NULL && type != NULL && type[0] == 'B' && class_getInstanceMethod(class_getSuperclass(list[i]), sel_registerName(names[k])) != m) {
				method_setImplementation(m, (IMP)answer_no);
				host_line("camera app: %s %s -> NO", class_getName(list[i]), names[k]);
				changed++;
			}
		}
	}
	free(list);
	host_line("camera app: %u HEVC switches of CameraUI turned off", changed);
}

static IMP orig_capture_photo;

static void
capture_photo(id me, SEL sel, id settings, id delegate)
{
	if (settings != NULL) {
		SEL speed = sel_registerName("setPhotoQualityPrioritization:"), fusion = sel_registerName("setAutoVirtualDeviceFusionEnabled:"), highres = sel_registerName("setHighResolutionPhotoEnabled:");

		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(settings, sel_registerName("respondsToSelector:"), speed)) {
			((void (*)(id, SEL, long))objc_msgSend)(settings, speed, 1);              /* AVCapturePhotoQualityPrioritizationSpeed */
		}
		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(settings, sel_registerName("respondsToSelector:"), fusion)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(settings, fusion, NO);
		}
		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(settings, sel_registerName("respondsToSelector:"), highres)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(settings, highres, NO);
		}
	}
	host_line("camera app: capturePhotoWithSettings: %s delegate %s", text_of_object(settings), delegate ? class_getName(object_getClass(delegate)) : "nil");
	if (delegate) {
		wrap_delegate(object_getClass(delegate));
	}
	((void (*)(id, SEL, id, id))orig_capture_photo)(me, sel, settings, delegate);
}

static void
camera_app_probe(void)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		Class output = objc_getClass("AVCapturePhotoOutput");
		Method m = output ? class_getInstanceMethod(output, sel_registerName("capturePhotoWithSettings:delegate:")) : NULL;

		if (m == NULL) {
			host_line("camera app: no -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:]");
			return;
		}
		orig_capture_photo = method_setImplementation(m, (IMP)capture_photo);
		host_line("camera app: capturePhotoWithSettings:delegate: watched");
		no_hevc_in_camera_ui();
	});
}

__attribute__((constructor))
static void
redwood_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name != NULL && strcmp(name, "Camera") == 0) {
		CFPreferencesSetAppValue(CFSTR("CAMUserPreferenceEnableResponsiveShutter"), kCFBooleanFalse, CFSTR("com.apple.camera"));
		camera_app_probe();
		return;
	}
	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	torch = open("/dev/redwood-torch", O_WRONLY);
	camera = access("/dev/redwood-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("redwood: mediaserverd: the kernel has a flashlight (/dev/redwood-torch)\n");
	}
	if (camera) {
		say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;
		pthread_t thread;

		int seconds = 40;       /* (its sandbox may not let it read the boot-args: then the default) */

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "media_threads=")) != NULL) {
			seconds = atoi(at + 14);
		}
		if (seconds > 0) {
			pthread_create(&thread, NULL, media_threads, (void *)(uintptr_t)seconds);
			pthread_detach(thread);
		}
	}
}
