/*
 * torch_test: the flashlight as an application reaches it on iOS 26/27 -- AVFlashlight, which talks to
 * cameracaptured's flashlight server (com.apple.coremedia.flashlight) -- for a job list:
 *
 *   torch_test [SECONDS]   says whether there is a flashlight and whether it is available, lights it at
 *                          full level for SECONDS (default 2), at a quarter for as long, then off
 *
 * Each step is said on standard error with what came back. The light itself is the kernel's
 * (boot-arg torch=3, /dev/hakios-torch; components/camera/src/inject_capture.c in cameracaptured).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void
error_text(id error, char *out, size_t size)
{
	CFStringRef text;

	if (error == NULL) {
		snprintf(out, size, "no error");
		return;
	}
	text = (CFStringRef)((id (*)(id, SEL))objc_msgSend)(error, sel_registerName("description"));
	if (text == NULL || !CFStringGetCString(text, out, (CFIndex)size, kCFStringEncodingUTF8)) {
		snprintf(out, size, "an error");
	}
}

static bool
level(id light, float value)
{
	id error = NULL;
	char text[300];
	BOOL done = ((BOOL (*)(id, SEL, float, id *))objc_msgSend)(light, sel_registerName("setFlashlightLevel:withError:"), value, &error);

	error_text(error, text, sizeof(text));
	fprintf(stderr, "torch_test: level %.2f: %s (%s); the flashlight says its level is %.2f\n", value, done ? "set" : "NOT SET",
	    text, ((float (*)(id, SEL))objc_msgSend)(light, sel_registerName("flashlightLevel")));
	return done;
}

int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 2;
	Class cls;
	id light, error = NULL;
	char text[300];
	BOOL on;

	if (dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW) == NULL) {
		fprintf(stderr, "torch_test: no AVFoundation: %s\n", dlerror());
		return 1;
	}
	cls = objc_getClass("AVFlashlight");
	if (cls == NULL) {              /* (iOS 17 on: AVFoundation's capture classes are AVFCapture's) */
		dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
		cls = objc_getClass("AVFlashlight");
	}
	if (cls == NULL) {
		fprintf(stderr, "torch_test: no AVFlashlight class\n");
		return 1;
	}
	fprintf(stderr, "torch_test: +[AVFlashlight hasFlashlight] %d\n",
	    ((BOOL (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("hasFlashlight")));
	light = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc")), sel_registerName("init"));
	if (light == NULL) {
		fprintf(stderr, "torch_test: no AVFlashlight made\n");
		return 1;
	}
	sleep(1);       /* (the server's first answers come in asynchronously) */
	fprintf(stderr, "torch_test: available %d, overheated %d\n",
	    ((BOOL (*)(id, SEL))objc_msgSend)(light, sel_registerName("isAvailable")),
	    ((BOOL (*)(id, SEL))objc_msgSend)(light, sel_registerName("isOverheated")));
	on = ((BOOL (*)(id, SEL, id *))objc_msgSend)(light, sel_registerName("turnPowerOnWithError:"), &error);
	error_text(error, text, sizeof(text));
	fprintf(stderr, "torch_test: power on: %s (%s)\n", on ? "yes" : "NO", text);
	if (level(light, 1.0f)) {
		sleep(seconds);
	}
	if (level(light, 0.25f)) {
		sleep(seconds);
	}
	level(light, 0.0f);
	((void (*)(id, SEL))objc_msgSend)(light, sel_registerName("turnPowerOff"));
	fprintf(stderr, "torch_test: power off\n");
	return 0;
}
