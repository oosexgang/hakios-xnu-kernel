/*
 * libhakios26.dylib: the touch screen and the buttons for Darwin 26's backboardd.
 *
 * backboardd is the HID event server, and without Apple's multitouch driver it has no touch
 * screen. This library (inserted by the kernel, see kern.exec_inject and build.sh's INJECT) gives
 * it one: a "virtual" HID service that turns the samples of /dev/hakios-touch -- from the phone's
 * own touch controller, or from rwtouch -- into the events Apple's
 * driver would have sent, and a consumer-control service for the buttons.
 *
 * Only public-ish IOKit HID calls are used (IOHIDVirtualServiceClient*, IOHIDEvent*), no address
 * inside the shared cache: nothing here depends on a build of Darwin. (src/inject.c is the Darwin 16.5
 * library, whose window server patches are 16.5's addresses; the input code is the same.)
 * Lines go to stderr and to the console, "hakios26: ...".
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <notify.h>
#include <os/lock.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "board/input.h"

#define PREFIX          "hakios: "
#define PREFIX26        "hakios26: "
_Static_assert(sizeof(PREFIX26) - sizeof(PREFIX) == 2, "two bytes are kept free for the longer prefix");

static void
say(const char *format, ...)
{
	char line[768];
	va_list ap;
	int n, console;

	/* (two bytes kept free for the "hakios26: " below: a line that filled the buffer was moved two bytes past
	 * its end, and the fortified memmove trapped -- PosterBoard died of it on every launch, run glass6) */
	va_start(ap, format);
	n = vsnprintf(line, sizeof(line) - 2, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 3) {
		n = sizeof(line) - 3;
	}
	/* (the two prefixes' lengths from the strings themselves: "hakios: " becomes "hakios26: ", two bytes more) */
	if (n > (int)sizeof(PREFIX) - 1 && strncmp(line, PREFIX, sizeof(PREFIX) - 1) == 0) {
		memmove(line + sizeof(PREFIX26) - 1, line + sizeof(PREFIX) - 1, (size_t)n - (sizeof(PREFIX) - 1) + 1);
		memcpy(line, PREFIX26, sizeof(PREFIX26) - 1);
		n += (int)(sizeof(PREFIX26) - sizeof(PREFIX));
		if (n > (int)sizeof(line) - 1) {
			n = sizeof(line) - 1;
		}
	}
	write(2, line, (size_t)n);
	if ((console = open("/dev/console", O_WRONLY | O_NOCTTY)) >= 0) {
		write(console, line, (size_t)n);
		close(console);
	}
}

static dispatch_semaphore_t input_services_made;

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on Darwin (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 402)
#define EDGE_TOP        (16.0 / 874)
#define EDGE_BOTTOM     (34.0 / 874)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
	volatile long long report_interval;     /* the "ReportInterval" a client last set (microseconds; -1: none yet) */
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("hakios: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("hakios: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	/* how often a client (locationd) wants a sensor's samples: hid_motion.h */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("ReportInterval")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		long long interval = -1;

		CFNumberGetValue(value, kCFNumberSInt64Type, &interval);
		((struct service *)refcon)->report_interval = interval;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 *
 * KEPT, if not NULL, is given the service's own state, for a caller that reads what clients set on it.
 */
static IOHIDServiceClientRef
make_service_kept(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display,
    struct service **kept)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("HakiOS"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* (the dictionary holds what it was given: these references are let go) */
	CFRelease(name);
	CFRelease(page_number);
	CFRelease(usage_number);
	CFRelease(zero);
	CFRelease(pairs);
	CFRelease(pair);
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	made->report_interval = -1;
	if (kept != NULL) {
		*kept = made;
	}
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("hakios: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	return make_service_kept(client, product, page, usage, display, NULL);
}

static pthread_mutex_t touch_lock = PTHREAD_MUTEX_INITIALIZER;   /* the screen's fingers: the panel's and the script's */

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch_locked(IOHIDServiceClientRef screen, const struct hakios_touch *t);

static void
send_touch(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	pthread_mutex_lock(&touch_lock);
	send_touch_locked(screen, t);
	pthread_mutex_unlock(&touch_lock);
}

static void
send_touch_locked(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	uint32_t change = t->type == HAKIOS_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != HAKIOS_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != HAKIOS_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == HAKIOS_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static uint32_t buttons_held;           /* one bit for each button (HAKIOS_BUTTON_*) whose press was sent and not its release */

static void
send_button(IOHIDServiceClientRef buttons, const struct hakios_touch *t)
{
	uint32_t usage = t->finger == HAKIOS_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == HAKIOS_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == HAKIOS_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == HAKIOS_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == HAKIOS_BUTTON_LOCK) {
		publish_lock_button(t->type == HAKIOS_BUTTON_DOWN);
	}
	if (t->type == HAKIOS_BUTTON_DOWN) {
		__atomic_or_fetch(&buttons_held, 1u << t->finger, __ATOMIC_RELAXED);
	} else {
		__atomic_and_fetch(&buttons_held, ~(1u << t->finger), __ATOMIC_RELAXED);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == HAKIOS_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Where the samples that would have lifted them may be lost (the device given up on and opened again):
 * every finger that is down lifted where it was last, and every button that is held let go -- or the
 * window server keeps a finger on the screen, or the side button held, for good.
 */
static void
release_all(IOHIDServiceClientRef screen, IOHIDServiceClientRef buttons)
{
	struct hakios_touch t = { .time = mach_absolute_time() };
	uint32_t held = __atomic_load_n(&buttons_held, __ATOMIC_RELAXED);

	pthread_mutex_lock(&touch_lock);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (fingers[i].touching) {
			t.type = HAKIOS_TOUCH_UP;
			t.finger = i;
			t.x = (uint32_t)(fingers[i].x * 65535 + 0.5);
			t.y = (uint32_t)(fingers[i].y * 65535 + 0.5);
			send_touch_locked(screen, &t);
		}
	}
	pthread_mutex_unlock(&touch_lock);
	t.type = HAKIOS_BUTTON_UP;
	t.x = t.y = 0;
	for (uint32_t b = 0; buttons != NULL && b < 32; b++) {
		if (held & (1u << b)) {
			t.finger = b;
			send_button(buttons, &t);
		}
	}
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static void script_start(IOHIDServiceClientRef screen, IOHIDServiceClientRef buttons);

/* the phone's orientation, as a service of its own beside the touch screen and the buttons */
#include "hid_orientation.h"
/* ... and the accelerometer's samples, for Darwin's motion stack, as another */
#include "hid_motion.h"
/* ... and the gyroscope's rates, for the same, as a third */
#include "hid_gyro.h"

/*
 * /dev/hakios-touch, opened for reading. One program may read it (board/input.c): while another does -- a
 * backboardd of before that is still going away -- the open is EBUSY, and is tried again each second
 * rather than leaving this one without touches for good. -1: it cannot be read at all (said why).
 */
static int
open_touch_device(void)
{
	int device;
	bool said = false;

	while ((device = open(HAKIOS_TOUCH_DEVICE, O_RDONLY)) < 0 && (errno == EBUSY || errno == EINTR)) {
		if (errno == EBUSY && !said) {
			say("hakios: input: %s: %s: waiting for the other reader to let it go\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
			said = true;
		}
		sleep(1);
	}
	if (device < 0) {
		say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
	} else if (said) {
		say("hakios: input: %s: ours now\n", HAKIOS_TOUCH_DEVICE);
	}
	return device;
}

static void *
input_thread(__unused void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct hakios_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("hakios input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open_touch_device();
	if (device < 0) {
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			say("hakios: input: no HID client of type %d\n", types[i]);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("hakios.input", NULL));
		screen = make_service(client, "HakiOS touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("hakios: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "HakiOS buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
		orientation_start(client);      /* (the phone's orientation, when the kernel follows it: hid_orientation.h) */
		motion_start(client);           /* (the accelerometer's samples, when the kernel gives them: hid_motion.h) */
		gyro_start(client);             /* (the gyroscope's rates, when the kernel gives them: hid_gyro.h) */
	}
	say("hakios: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}
	script_start(screen, buttons);

	for (;;) {
		ssize_t got = read(device, &t, sizeof(t));
		uint64_t read_at = mach_absolute_time();

		if (got != (ssize_t)sizeof(t)) {
			/*
			 * A signal (EINTR) took nothing: read again. Anything else may have taken samples with it: what is
			 * held is let go, and the device opened again -- the thread that ended here left the phone with no
			 * touch screen and no buttons until backboardd was started again, and with a finger held, if one was.
			 */
			if (got < 0 && errno == EINTR) {
				continue;
			}
			say("hakios: input: %s: %s: opening it again\n", HAKIOS_TOUCH_DEVICE, got < 0 ? strerror(errno) :
			    "not a whole sample");
			release_all(screen, buttons);
			waking = 0;
			close(device);
			sleep(1);
			if ((device = open_touch_device()) < 0) {
				return NULL;
			}
			continue;
		}

		if (t.type == HAKIOS_TOUCH_DOWN || t.type == HAKIOS_TOUCH_UP || count++ < 4) {
			say("hakios: input: %s finger %u at %u %u (%.0f%% %.0f%%)\n",
			    t.type == HAKIOS_TOUCH_DOWN ? "DOWN" : t.type == HAKIOS_TOUCH_UP ? "UP  " : "move",
			    t.finger, t.x, t.y, t.x * 100.0 / 65535, t.y * 100.0 / 65535);
		}
		if (t.type > HAKIOS_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == HAKIOS_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct hakios_touch press = { .time = t.time, .type = HAKIOS_BUTTON_DOWN, .finger = HAKIOS_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = HAKIOS_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == HAKIOS_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
}

/*
 * Boot-arg taps=1: a script of touches, /hakios/taps.txt (userland/taps.txt), played on the touch screen as if a
 * finger did them -- to walk through the system's screens in a run nobody is holding the phone for, and record them.
 * One command a line, positions in percent of the screen:
 *   wait SECONDS          tap X Y          swipe X1 Y1 X2 Y2 MS          home          say TEXT
 * The script's finger is finger 9, out of the panel's way.
 */
#define SCRIPT_FINGER   9

static IOHIDServiceClientRef script_screen, script_buttons;

static void
script_touch(uint32_t type, double x, double y)
{
	struct hakios_touch t = { .time = mach_absolute_time(), .type = type, .finger = SCRIPT_FINGER,
		                   .x = (uint32_t)(x * 65535 / 100), .y = (uint32_t)(y * 65535 / 100) };

	send_touch(script_screen, &t);
}

static void *
script_thread(__unused void *arg)
{
	FILE *f = fopen("/hakios/taps.txt", "r");
	char line[256];
	unsigned lineno = 0;

	pthread_setname_np("hakios taps");
	if (f == NULL) {
		say("hakios: taps: no /hakios/taps.txt\n");
		return NULL;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		double a, b, c, d, ms;

		lineno++;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || line[0] == 0) {
			continue;
		}
		if (sscanf(line, "wait %lf", &a) == 1) {
			usleep((useconds_t)(a * 1000000));
		} else if (sscanf(line, "tap %lf %lf", &a, &b) == 2) {
			say("hakios: taps: %u: tap %.0f%% %.0f%%\n", lineno, a, b);
			script_touch(HAKIOS_TOUCH_DOWN, a, b);
			usleep(80000);
			script_touch(HAKIOS_TOUCH_UP, a, b);
		} else if (sscanf(line, "swipe %lf %lf %lf %lf %lf", &a, &b, &c, &d, &ms) == 5) {
			unsigned steps = (unsigned)(ms / 8) + 1;

			say("hakios: taps: %u: swipe %.0f,%.0f -> %.0f,%.0f in %.0f ms\n", lineno, a, b, c, d, ms);
			script_touch(HAKIOS_TOUCH_DOWN, a, b);
			for (unsigned i = 1; i <= steps; i++) {
				usleep(8000);
				script_touch(HAKIOS_TOUCH_MOVE, a + (c - a) * i / steps, b + (d - b) * i / steps);
			}
			script_touch(HAKIOS_TOUCH_UP, c, d);
		} else if (strcmp(line, "home") == 0 && script_buttons != NULL) {
			struct hakios_touch press = { .time = mach_absolute_time(), .type = HAKIOS_BUTTON_DOWN,
				                       .finger = HAKIOS_BUTTON_HOME };

			say("hakios: taps: %u: home\n", lineno);
			send_button(script_buttons, &press);
			usleep(60000);
			press.type = HAKIOS_BUTTON_UP;
			press.time = mach_absolute_time();
			send_button(script_buttons, &press);
		} else if (strncmp(line, "say ", 4) == 0) {
			say("hakios: taps: %u: %s\n", lineno, line + 4);
		} else {
			say("hakios: taps: %u: not understood: %s\n", lineno, line);
		}
	}
	fclose(f);
	say("hakios: taps: the script is done\n");
	return NULL;
}

static void
script_start(IOHIDServiceClientRef screen, IOHIDServiceClientRef buttons)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 || strstr(arguments, "taps=1") == NULL) {
		return;
	}
	script_screen = screen;
	script_buttons = buttons;
	if (pthread_create(&thread, NULL, script_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

static void
input_start(void)
{
	pthread_t thread;

	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}


/* ---- big thread stacks, and CoreAnimation switches from the boot-args ------------------------------- */

/*
 * No GPU: CoreAnimation draws with its software renderer, which walks the layer tree recursively
 * (CA::OGL render_layers -> LayerNode::apply -> ImagingNode::render -> ...). A deep but finite tree --
 * the app switcher's blurred snapshots of every app, a Settings push transition -- overflows the small
 * stack of the render thread and the process aborts (what looks like a "log out": SpringBoard and
 * backboardd respawn). Every thread this process makes with a stack under BIG_STACK_MIN gets BIG_STACK
 * instead, by interposing pthread_create, so the recursion has room.
 */
#define BIG_STACK_MIN   (4u << 20)
#define BIG_STACK       (24u << 20)

static int
big_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    void *(*start)(void *), void *arg)
{
	pthread_attr_t own;
	size_t stack = 0;
	void *given = NULL;

	if (attr != NULL) {
		pthread_attr_getstacksize(attr, &stack);
		pthread_attr_getstackaddr(attr, &given);
	}
	/*
	 * (as said above: a stack under BIG_STACK_MIN made bigger -- not one the caller asked to be bigger still, and not one
	 * whose memory the caller gave, which cannot be told it has more than it was given)
	 */
	if ((stack == 0 || stack < BIG_STACK_MIN) && given == NULL) {
		if (pthread_attr_init(&own) == 0) {
			if (attr != NULL) {
				own = *attr;
			}
			pthread_attr_setstacksize(&own, BIG_STACK);
			int r = pthread_create(thread, &own, start, arg);
			pthread_attr_destroy(&own);
			return r;
		}
	}
	return pthread_create(thread, attr, start, arg);
}

/*
 * What the window server hands the display, swap by swap (boot-arg swaplog=1). With the GPU's
 * renderer the panel stayed on the loader's picture although the kernel's display driver counted
 * 41 swaps in five seconds, none of them with a surface in it: which layer, which surface, and
 * which events the swap was told to wait for, from the window server's side of the call.
 */
typedef void *IOMobileFramebufferRef;
typedef struct { double x, y, w, h; } rw_rect;
extern int IOMobileFramebufferSwapBegin(IOMobileFramebufferRef fb, int *token);
extern int IOMobileFramebufferSwapEnd(IOMobileFramebufferRef fb);
extern int IOMobileFramebufferSwapSetLayer(IOMobileFramebufferRef fb, int layer, void *surface, rw_rect bounds,
    rw_rect frame, int flags);
/* (the two event calls' arguments are not known for certain: all six registers are passed on as they came) */
extern int IOMobileFramebufferSwapSetEventWait(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5);
extern int IOMobileFramebufferSwapSetEventSignal(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5);
extern uint32_t IOSurfaceGetID(void *surface);

static unsigned swaplog_count;

static bool
swaplog_on(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "swaplog=1") != NULL;
	}
	/* the first 40 swaps, then one in 200 */
	return on && (swaplog_count < 40 || swaplog_count % 200 == 0);
}

static void
swaplog(const char *fmt, ...)
{
	char line[300];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, line, strlen(line));
		close(fd);
	}
}

static int
rw_swap_begin(IOMobileFramebufferRef fb, int *token)
{
	int r = IOMobileFramebufferSwapBegin(fb, token);

	if (swaplog_on()) {
		swaplog("hakios26: swaplog %u: begin -> %#x token %d\n", swaplog_count, r, token ? *token : -1);
	}
	return r;
}

static int
rw_swap_end(IOMobileFramebufferRef fb)
{
	int r = IOMobileFramebufferSwapEnd(fb);

	if (swaplog_on()) {
		swaplog("hakios26: swaplog %u: end -> %#x\n", swaplog_count, r);
	}
	swaplog_count++;
	return r;
}

static int
rw_swap_set_layer(IOMobileFramebufferRef fb, int layer, void *surface, rw_rect bounds, rw_rect frame, int flags)
{
	int r = IOMobileFramebufferSwapSetLayer(fb, layer, surface, bounds, frame, flags);

	if (swaplog_on()) {
		swaplog("hakios26: swaplog %u: layer %d surface %u (%p) bounds %.0f,%.0f %.0fx%.0f frame %.0f,%.0f %.0fx%.0f "
		    "flags %#x -> %#x\n", swaplog_count, layer, surface ? IOSurfaceGetID(surface) : 0, surface, bounds.x,
		    bounds.y, bounds.w, bounds.h, frame.x, frame.y, frame.w, frame.h, flags, r);
	}
	return r;
}

static int
rw_swap_event_wait(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	int r = IOMobileFramebufferSwapSetEventWait(fb, a1, a2, a3, a4, a5);

	if (swaplog_on()) {
		swaplog("hakios26: swaplog %u: event wait %#llx %#llx %#llx %#llx -> %#x\n", swaplog_count,
		    (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4, r);
	}
	return r;
}

static int
rw_swap_event_signal(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	int r = IOMobileFramebufferSwapSetEventSignal(fb, a1, a2, a3, a4, a5);

	if (swaplog_on()) {
		swaplog("hakios26: swaplog %u: event signal %#llx %#llx %#llx %#llx -> %#x\n", swaplog_count,
		    (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4, r);
	}
	return r;
}

__attribute__((used)) static struct {
	const void *replacement;
	const void *original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)(uintptr_t)&big_pthread_create, (const void *)(uintptr_t)&pthread_create },
	{ (const void *)(uintptr_t)&rw_swap_begin, (const void *)(uintptr_t)&IOMobileFramebufferSwapBegin },
	{ (const void *)(uintptr_t)&rw_swap_end, (const void *)(uintptr_t)&IOMobileFramebufferSwapEnd },
	{ (const void *)(uintptr_t)&rw_swap_set_layer, (const void *)(uintptr_t)&IOMobileFramebufferSwapSetLayer },
	{ (const void *)(uintptr_t)&rw_swap_event_wait, (const void *)(uintptr_t)&IOMobileFramebufferSwapSetEventWait },
	{ (const void *)(uintptr_t)&rw_swap_event_signal, (const void *)(uintptr_t)&IOMobileFramebufferSwapSetEventSignal },
};

/* Boot-arg caenv=NAME=VALUE (as many as wanted): into this process's environment, for CoreAnimation's
 * own switches (CA_DISABLE_BACKDROP_CACHE=1, CA_DISABLE_FILTER_MERGING=1, ...). Set before CA starts. */
static void
caenv_from_bootargs(void)
{
	char args[2048] = "";
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return;
	}
	for (const char *a = args; (a = strstr(a, "caenv=")) != NULL; a += 6) {
		char setting[160];
		size_t len = strcspn(a + 6, " ");
		char *eq;

		if ((a != args && a[-1] != ' ') || len == 0 || len >= sizeof(setting)) {
			continue;
		}
		memcpy(setting, a + 6, len);
		setting[len] = 0;
		if ((eq = strchr(setting, '=')) != NULL) {
			*eq = 0;
			setenv(setting, eq + 1, 1);
			say("hakios: caenv: %s=%s\n", setting, eq + 1);
		}
	}
}

/* ---- SpringBoard: the device is "activated", and the setup assistant is not needed ------------------ */

/*
 * SpringBoard decides whether to show the lock/home screen or the setup assistant from two things,
 * both of which depend on daemons this port does not run:
 *   - whether the device is "activated" (-[SBLockdownManager isActivated], normally the answer of
 *     mobileactivationd): forced to YES;
 *   - whether the setup assistant is required (-[SBSetupManager _setSetupRequiredReason:], normally
 *     driven by the purplebuddy state): forced to 0, "no reason".
 * Without these, unlocking launches Setup.app, whose first screen the software renderer cannot draw --
 * backboardd runs out of stack in CoreAnimation's render tree and SpringBoard respawns with it (the
 * "log out"). Swizzled by selector name only, so nothing here depends on a build of Darwin. (src/inject_sb.c
 * does the same for Darwin 16.5, by hard-coded address.)
 */
static BOOL
sb_is_activated(__unused id self, __unused SEL sel)
{
	return YES;
}

static void (*orig_set_setup_required_reason)(id, SEL, long);

static void
sb_set_setup_required_reason(id self, SEL sel, long reason)
{
	if (reason != 0) {
		say("hakios: SpringBoard wants the setup assistant (reason %ld); refused\n", reason);
	}
	if (orig_set_setup_required_reason != NULL) {
		orig_set_setup_required_reason(self, sel, 0);
	}
}

static int
swizzle(const char *class_name, const char *selector, IMP replacement, IMP *original)
{
	Class cls = objc_getClass(class_name);
	Method m = cls != NULL ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (m == NULL) {
		say("hakios: -[%s %s]: not found\n", class_name, selector);
		return 0;
	}
	/* the original first: another thread can call the replacement the moment it is set */
	if (original != NULL) {
		*original = method_getImplementation(m);
	}
	method_setImplementation(m, replacement);
	return 1;
}

/*
 * The wallpaper. Darwin 26's wallpaper is a "poster" of PosterBoard's, and the first ones are made when the setup
 * assistant finishes -- which never runs here (refused above). PosterBoard itself will not make a default one on an
 * iPhone ("Hello NOT iPad, trying to restoreDefaultWallpaperForAllVariantsAndNotify? not today."), so the lock and
 * home screens stayed black. What Settings' "restore the default wallpaper" asks for is asked here, once SpringBoard
 * is up: +[SBSUIWallpaperService restoreDefaultWallpaper]. Boot-arg sb_wallpaper=0: not.
 */
static void
sb_restore_wallpaper(void *context)
{
	Class service = objc_getClass("SBSUIWallpaperService");
	SEL restore = sel_registerName("restoreDefaultWallpaper");

	(void)context;
	say("hakios: SpringBoard: the default wallpaper: asking (%s)\n", service != Nil ? "SBSUIWallpaperService" : "no service class");
	if (service != Nil && class_getClassMethod(service, restore) != NULL) {
		((void (*)(Class, SEL))objc_msgSend)(service, restore);
		say("hakios: SpringBoard: the default wallpaper asked for (SBSUIWallpaperService)\n");
	} else {
		say("hakios: SpringBoard: no +[SBSUIWallpaperService restoreDefaultWallpaper]\n");
	}
}

/*
 * PosterBoard is started by its clients' requests (its Mach service does not start it), and SpringBoard asks only
 * once it has a poster: so nothing starts it, and no poster is ever made (runs flip2, wp1). Asked here as a client
 * asks, through PosterBoardServices' PRSService: prewarm, which starts it and sets up its data store, the role
 * coordinators and their first posters. The reply's arguments are not looked at: what they are is not known, and
 * describing one took SpringBoard down (run wp4: a trap in snprintf inside the reply block).
 */
static void
sb_prewarm_done(void *context)
{
	(void)context;
	say("hakios: SpringBoard: PosterBoard: prewarm answered\n");
}

static void
sb_wake_posterboard(void *context)
{
	Class service_class = objc_getClass("PRSService");
	SEL prewarm = sel_registerName("prewarmWithCompletion:");
	id service;

	(void)context;
	if (service_class == Nil) {
		say("hakios: SpringBoard: no PRSService\n");
		return;
	}
	/* (a private method: on a build without it the message raises on a global queue, which ends SpringBoard) */
	if (!class_respondsToSelector(service_class, prewarm)) {
		say("hakios: SpringBoard: no -[PRSService prewarmWithCompletion:]; PosterBoard is not woken\n");
		return;
	}
	service = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(service_class, sel_registerName("alloc")),
	    sel_registerName("init"));
	say("hakios: SpringBoard: PosterBoard: prewarm asked\n");
	((void (*)(id, SEL, void (^)(void)))objc_msgSend)(service, prewarm, ^{
		dispatch_async_f(dispatch_get_main_queue(), NULL, sb_prewarm_done);
	});
}

static void
springboard_fixes(void)
{
	{
		char args[1024] = { 0 };
		size_t size = sizeof(args) - 1;

		if (!(sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "sb_wallpaper=0") != NULL)) {
			/* not on the main queue: the service answers from SpringBoard's main thread, so asking there waits on itself */
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL,
			    sb_wake_posterboard);
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL,
			    sb_restore_wallpaper);
		}
	}
	int activation = swizzle("SBLockdownManager", "isActivated", (IMP)sb_is_activated, NULL);
	int setup = swizzle("SBSetupManager", "_setSetupRequiredReason:", (IMP)sb_set_setup_required_reason,
	    (IMP *)&orig_set_setup_required_reason);

	say("hakios: SpringBoard: activation %s, setup assistant %s\n",
	    activation ? "forced on" : "NOT replaced", setup ? "refused" : "NOT replaced");
}

/*
 * PosterBoard: why no poster is made. The first lock and home screen posters are made by PosterBoard's role
 * coordinators from +[PBFInitialPosterHelper initialPosterConfigurationsWithModelCoordinatorProvider:role:], out of
 * the poster extensions it knows. Each step is said here, and 30 s on what the data store has: its extensions and
 * each role's posters.
 */
static const char *
pb_desc(id object, char *buffer, size_t size)
{
	id text = object != nil ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description")) : nil;
	const char *c = text != nil ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "nil";

	strlcpy(buffer, c != NULL ? c : "?", size);
	for (char *p = buffer; *p; p++) {
		if (*p == '\n') {
			*p = ' ';
		}
	}
	return buffer;
}

static unsigned long
pb_count(id object)
{
	SEL count = sel_registerName("count");

	return object != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(object, sel_registerName("respondsToSelector:"), count)
	    ? ((unsigned long (*)(id, SEL))objc_msgSend)(object, count) : 0;
}

/*
 * Whether `object` (or a class, for its class methods) answers `name`. What is asked of PosterBoard below are its
 * private methods, from a global queue: one this build has not got raises there, and that ends PosterBoard. Each
 * missing one is said once.
 */
static bool
pb_answers(id object, const char *name)
{
	static SEL told[8];
	static unsigned told_count;
	SEL selector = sel_registerName(name);

	if (object != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(object, sel_registerName("respondsToSelector:"), selector)) {
		return true;
	}
	for (unsigned i = 0; i < told_count; i++) {
		if (told[i] == selector) {
			return false;
		}
	}
	if (told_count < sizeof(told) / sizeof(told[0])) {
		told[told_count++] = selector;
	}
	say("hakios: PosterBoard: %s does not answer %s; not asked\n", object != nil ? object_getClassName(object) : "nil", name);
	return false;
}

static id (*orig_initial_posters)(id, SEL, id, id);
static void (*orig_setup_role)(id, SEL, id);
static void (*orig_setup_coordinators)(id, SEL);
static id pb_store;

static id
pb_initial_posters(id self, SEL sel, id provider, id role)
{
	char a[160], b[400];
	id result = orig_initial_posters(self, sel, provider, role);

	say("hakios: PosterBoard: initial posters for %s: %lu (%s)\n", pb_desc(role, a, sizeof(a)), pb_count(result),
	    pb_desc(result, b, sizeof(b)));
	return result;
}

static void
pb_setup_role(id self, SEL sel, id storage)
{
	char a[160];
	id role = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("role"));

	say("hakios: PosterBoard: setting up role %s\n", pb_desc(role, a, sizeof(a)));
	orig_setup_role(self, sel, storage);
}

extern id objc_retain(id object);

static void
pb_setup_coordinators(id self, SEL sel)
{
	/* kept, because pb_report uses it 30 s on from another thread (an older one, if there was one, is not
	 * released: that thread may be using it) */
	if (pb_store != self) {
		pb_store = objc_retain(self);
	}
	say("hakios: PosterBoard: the data store sets up its role coordinators\n");
	orig_setup_coordinators(self, sel);
}

static void pb_report(void *context);

/*
 * ... and the first lock screen poster, asked for again once the poster extensions are known. The role coordinators
 * are set up as the data store is built, which here is before PosterBoard has found a single poster extension (/var
 * is made at every boot and the extension registry is still being filled), so +[PBFInitialPosterHelper
 * initialPosterConfigurationsWithModelCoordinatorProvider:role:] finds neither the hero poster's extension nor
 * com.apple.PaperBoard.LegacyPoster (-pbf_posterExtensionStoreCoordinatorForProviderIdentifier:error: answers nil)
 * and the role starts with none (runs glass8, glass10: "initial posters for PRPosterRoleLockScreen: 0 (nil)").
 * PosterBoard asks again only when a change to the role is finalised with no poster left
 * (-[PBFPosterRoleCoordinator finalizeChangesWithChangeHandler:outEvents:error:]). Asked here the same way, and what
 * it answers -- PRMutablePosterConfigurations -- handed in as incoming posters, which adds them to the role
 * (-processIncomingPosterConfiguration:completion: -> addPosterToRole:incomingPoster:). Once per boot.
 */
static int pb_initial_tried;

/*
 * What the helper finds, step by step, while it is asked again (pb_tracing) -- and every failure to find an extension's
 * store at any time. PosterBoard's own log of these steps (PBFLogPosterContents) reaches the console only in the live
 * log of a run, not in what is kept of it (run glass11).
 */
static volatile int pb_tracing;
static id (*orig_store_for_provider)(id, SEL, id, id *);
static id (*orig_static_descriptors)(id, SEL, id);
static id (*orig_create_dynamic)(id, SEL, id, id, id *);
static BOOL (*orig_hero)(id, SEL);
static id (*orig_incoming)(id, SEL, id, id, id);

static id
pb_store_for_provider(id self, SEL sel, id identifier, id *error)
{
	static int failures;
	char a[200], b[600];
	id local = nil;
	id result = orig_store_for_provider(self, sel, identifier, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (pb_tracing || (result == nil && failures++ < 20)) {
		say("hakios: PosterBoard: the store of extension %s: %s%s%s\n", pb_desc(identifier, a, sizeof(a)),
		    result != nil ? "found" : "NONE", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	}
	return result;
}

static id
pb_static_descriptors(id self, SEL sel, id identifier)
{
	char a[200];
	id result = orig_static_descriptors(self, sel, identifier);

	if (pb_tracing) {
		BOOL known = result != nil && ((BOOL (*)(id, SEL))objc_msgSend)(result, sel_registerName("hasKnownVersions"));
		say("hakios: PosterBoard: static descriptor %s: %s\n", pb_desc(identifier, a, sizeof(a)),
		    result == nil ? "none" : known ? "with versions" : "no versions");
	}
	return result;
}

static id
pb_create_dynamic(id self, SEL sel, id identifier, id role, id *error)
{
	char a[200], b[600];
	id local = nil;
	id result = orig_create_dynamic(self, sel, identifier, role, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	say("hakios: PosterBoard: dynamic descriptor %s made: %s%s%s\n", pb_desc(identifier, a, sizeof(a)),
	    result != nil ? "yes" : "NO", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	return result;
}

static BOOL
pb_hero(id self, SEL sel)
{
	BOOL hero = orig_hero(self, sel);

	say("hakios: PosterBoard: the hero poster as the default lock screen: %s\n", hero ? "yes" : "no");
	return hero;
}

static id
pb_incoming(id self, SEL sel, id role, id path, id info)
{
	char a[300], b[300];
	id result = orig_incoming(self, sel, role, path, info);

	say("hakios: PosterBoard: incoming configuration from %s: %s\n", pb_desc(path, a, sizeof(a)),
	    pb_desc(result, b, sizeof(b)));
	return result;
}

/* The data store's state lock, which the role coordinators hold when they ask the helper: the os_unfair_lock ivar at
 * 0xc4 in this build (-processIncomingPosterConfiguration:completion:'s block takes it there; 0xc0 beside it is the
 * extension store coordinators' own). Looked up by offset and type, and NULL if this PosterBoard is laid out otherwise. */
static os_unfair_lock *
pb_state_lock(id store)
{
	unsigned count = 0;
	Ivar *ivars = class_copyIvarList(object_getClass(store), &count);
	os_unfair_lock *lock = NULL;

	for (unsigned i = 0; i < count; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);

		if (ivar_getOffset(ivars[i]) == 0xc4 && type != NULL && strncmp(type, "{os_unfair_lock", 15) == 0) {
			say("hakios: PosterBoard: the data store's state lock is %s\n", ivar_getName(ivars[i]));
			lock = (os_unfair_lock *)((char *)store + 0xc4);
		}
	}
	free(ivars);
	return lock;
}

static void
pb_make_initial_posters(id role)
{
	char a[160], b[600];
	Class helper = objc_getClass("PBFInitialPosterHelper");
	SEL make = sel_registerName("initialPosterConfigurationsWithModelCoordinatorProvider:role:");
	SEL ingest = sel_registerName("processIncomingPosterConfiguration:completion:");
	id configurations;

	if (pb_initial_tried++ || helper == Nil || class_getClassMethod(helper, make) == NULL ||
	    !((BOOL (*)(id, SEL, SEL))objc_msgSend)(pb_store, sel_registerName("respondsToSelector:"), ingest)) {
		return;
	}
	os_unfair_lock *lock = pb_state_lock(pb_store);
	if (lock != NULL) {
		os_unfair_lock_lock(lock);
	}
	if (pb_answers(pb_store, "knownExtensions")) {
		id known = ((id (*)(id, SEL))objc_msgSend)(pb_store, sel_registerName("knownExtensions"));
		for (unsigned long i = 0; i < pb_count(known); i++) {
			id extension = ((id (*)(id, SEL, unsigned long))objc_msgSend)(known, sel_registerName("objectAtIndex:"), i);
			id identifier = ((id (*)(id, SEL))objc_msgSend)(extension, sel_registerName("posterExtensionBundleIdentifier"));
			say("hakios: PosterBoard: known extension %lu: %s\n", i, pb_desc(identifier, a, sizeof(a)));
		}
	}
	pb_tracing = 1;
	configurations = ((id (*)(Class, SEL, id, id))objc_msgSend)(helper, make, pb_store, role);
	pb_tracing = 0;
	if (lock != NULL) {
		os_unfair_lock_unlock(lock);
	}
	say("hakios: PosterBoard: initial posters for %s, asked again: %lu\n", pb_desc(role, a, sizeof(a)),
	    pb_count(configurations));
	for (unsigned long i = 0; i < pb_count(configurations); i++) {
		id configuration = ((id (*)(id, SEL, unsigned long))objc_msgSend)(configurations, sel_registerName("objectAtIndex:"), i);

		say("hakios: PosterBoard: handing in %s\n", pb_desc(configuration, b, sizeof(b)));
		/* answered (nil, NSError) on failure: only the second argument is looked at */
		((void (*)(id, SEL, id, void (^)(id, id)))objc_msgSend)(pb_store, ingest, configuration, ^(id result, id error) {
			char e[600];

			(void)result;
			say("hakios: PosterBoard: the incoming poster: %s%s\n", error != nil ? "error " : "added",
			    error != nil ? pb_desc(error, e, sizeof(e)) : "");
		});
	}
	/* and what the roles hold afterwards */
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, pb_report);
}

static void
pb_report(void *context)
{
	char a[160], b[600];
	id roles, extensions;

	(void)context;
	if (pb_store == nil) {
		/* nobody has asked PosterBoard anything yet (SpringBoard asks only once there is a poster): its data
		 * store, and with it the role coordinators and their first posters, made the way a client's request makes it.
		 * -dataStore only answers the one already made (nil: runs wp3, wp4); -_dataStoreWithError: makes it
		 * (-_lock_dataStoreWithError:: base URL, migrations, buildDataStoreForURL:...) and says why it could not. */
		Class glue = objc_getClass("PBFPosterExtensionDataStoreXPCServiceGlue");
		id shared = glue != Nil ? ((id (*)(Class, SEL))objc_msgSend)(glue, sel_registerName("sharedInstance")) : nil;
		id error = nil;
		id store = shared != nil && pb_answers(shared, "_dataStoreWithError:") ?
		    ((id (*)(id, SEL, id *))objc_msgSend)(shared, sel_registerName("_dataStoreWithError:"), &error) : nil;

		say("hakios: PosterBoard: no data store after 30 s: made one: %s%s%s\n", pb_desc(store, a, sizeof(a)),
		    error != nil ? ", error " : "", error != nil ? pb_desc(error, b, sizeof(b)) : "");
		if (pb_store == nil && store != nil) {
			pb_store = objc_retain(store);
		}
		if (pb_store == nil) {
			return;
		}
	}
	extensions = pb_answers(pb_store, "knownExtensions") ?
	    ((id (*)(id, SEL))objc_msgSend)(pb_store, sel_registerName("knownExtensions")) : nil;
	say("hakios: PosterBoard: %lu poster extensions known: %s\n", pb_count(extensions), pb_desc(extensions, b, sizeof(b)));
	if (!pb_answers(pb_store, "posterConfigurationsForRole:error:") ||
	    !pb_answers((id)objc_getClass("PBFPosterExtensionDataStore"), "supportedRoles")) {
		return;
	}
	roles = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("PBFPosterExtensionDataStore"), sel_registerName("supportedRoles"));
	roles = pb_answers(roles, "allObjects") ? ((id (*)(id, SEL))objc_msgSend)(roles, sel_registerName("allObjects")) : nil;
	for (unsigned long i = 0; i < pb_count(roles); i++) {
		id role = ((id (*)(id, SEL, unsigned long))objc_msgSend)(roles, sel_registerName("objectAtIndex:"), i);
		id error = nil;
		id posters = ((id (*)(id, SEL, id, id *))objc_msgSend)(pb_store, sel_registerName("posterConfigurationsForRole:error:"),
		    role, &error);

		say("hakios: PosterBoard: role %s: %lu posters%s%s\n", pb_desc(role, a, sizeof(a)), pb_count(posters),
		    error != nil ? ", error " : "", error != nil ? pb_desc(error, b, sizeof(b)) : "");
		if (error == nil && pb_count(posters) == 0 && pb_count(extensions) != 0 && strcmp(a, "PRPosterRoleLockScreen") == 0) {
			pb_make_initial_posters(role);
		}
	}
}

/*
 * Why the data store is not made: PosterBoard's own log (run glass1, 28 October) says "Beginning migration to
 * Version 59", "Failed to migrate to Version 61 from 59", then "could not initialize data store: Data store
 * could not be created for an un[known reason]" -- on a /var made fresh at every boot. The migration and the
 * build, with their arguments and the NSError they give back.
 */
static BOOL (*orig_migrate)(Class, SEL, id, unsigned long, unsigned long, BOOL, id *);

static BOOL
pb_migrate(Class self, SEL sel, id url, unsigned long from, unsigned long to, BOOL cleanup, id *error)
{
	char a[300], b[1200];
	id local = nil;
	BOOL ok = orig_migrate(self, sel, url, from, to, cleanup, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	say("hakios: PosterBoard: migrate %s from %lu to %lu: %s%s%s\n", pb_desc(url, a, sizeof(a)), from, to,
	    ok ? "done" : "FAILED", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	return ok;
}

static id (*orig_build)(id, SEL, id, id, id, id, BOOL, id *);

static id
pb_build(id self, SEL sel, id url, id assertions, id monitor, id observer, BOOL migrated, id *error)
{
	char a[300], b[1200];
	id local = nil;
	id store = orig_build(self, sel, url, assertions, monitor, observer, migrated, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	say("hakios: PosterBoard: build data store at %s (migrated %d): %s%s%s\n", pb_desc(url, a, sizeof(a)),
	    (int)migrated, store != nil ? "made" : "NONE", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	return store;
}

/* ... and the file operations the migrator makes, each failure said with its NSError: the migration
 * fails with none (run glass3: "migrate ... from 59 to 61: FAILED", no error), after copying a version's
 * store forward and making its directories with NSFileProtectionNone (PFFileProtectionNoneAttributes). */
static BOOL (*orig_fm_create)(id, SEL, id, BOOL, id, id *);
static BOOL (*orig_fm_copy)(id, SEL, id, id, id *);
static BOOL (*orig_url_update)(id, SEL, id, id *);

/* `dict` without `key`, or nil when it has no such key (PosterBoard's attributes and resource values) */
static id
pb_without(id dict, const char *key)
{
	id name = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"), key);

	if (dict == nil || ((id (*)(id, SEL, id))objc_msgSend)(dict, sel_registerName("objectForKey:"), name) == nil) {
		return nil;
	}
	id copy = ((id (*)(id, SEL))objc_msgSend)(dict, sel_registerName("mutableCopy"));
	((void (*)(id, SEL, id))objc_msgSend)(copy, sel_registerName("removeObjectForKey:"), name);
	/* (autoreleased: mutableCopy gives it +1, and none of the three callers lets it go -- one leaked a retried call) */
	return ((id (*)(id, SEL))objc_msgSend)(copy, sel_registerName("autorelease"));
}

/*
 * The check that fails it. The migrator makes a version's directories (store, extension container, gallery
 * cache) and then asks __PBFDetermineDataStoreViabilityForVersionWithinBaseURL whether that version is viable:
 * each must exist AND -[NSURL pbf_URLIsReachableAndConformToResourceValues:error:] against
 * PFPosterPathURLResourceValues(), whose file protection class this port's /var (no keybag, no data protection)
 * never reports -- so a store made a moment ago was "not viable" and the migration gave up with no error (run
 * glass4: 59 -> 61 FAILED, every directory made). Checked again without the class when the first answer is no.
 */
static BOOL (*orig_url_conform)(id, SEL, id, id *);

static BOOL
pb_url_conform(id self, SEL sel, id values, id *error)
{
	char a[300], b[600];
	id local = nil;
	BOOL ok = orig_url_conform(self, sel, values, error != NULL ? error : &local);

	if (!ok) {
		id plain = pb_without(values, "NSURLFileProtectionKey");
		if (plain != nil) {
			local = nil;
			ok = orig_url_conform(self, sel, plain, &local);
		}
		static unsigned said;
		if (said++ < 12) {
			say("hakios: PosterBoard: %s does not conform to %s; without the protection class: %s\n",
			    pb_desc(self, a, sizeof(a)), pb_desc(values, b, sizeof(b)), ok ? "conforms" : "STILL NOT");
		}
		if (ok && error != NULL) {
			*error = nil;
		}
	}
	return ok;
}

static BOOL
pb_fm_create(id self, SEL sel, id url, BOOL intermediate, id attributes, id *error)
{
	char a[300], b[300], c[900];
	id local = nil;
	BOOL ok = orig_fm_create(self, sel, url, intermediate, attributes, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("hakios: PosterBoard: mkdir %s (attributes %s) FAILED: %s\n", pb_desc(url, a, sizeof(a)),
		    pb_desc(attributes, b, sizeof(b)), pb_desc(e, c, sizeof(c)));
		/* this port's /var has no data protection classes (no keybag): once more without the class */
		id plain = pb_without(attributes, "NSFileProtectionKey");
		if (plain != nil) {
			local = nil;
			ok = orig_fm_create(self, sel, url, intermediate, plain, error != NULL ? error : &local);
			say("hakios: PosterBoard: mkdir %s without NSFileProtectionKey: %s\n", pb_desc(url, a, sizeof(a)),
			    ok ? "made" : "FAILED again");
		}
	}
	return ok;
}

/*
 * ... and the files it writes with -[NSData writeToURL:options:error:]: a descriptor's identity
 * (PRBPosterExtensionDataStore/61/Extensions/<extension>/descriptors/<UUID>/com.apple.posterkit.provider.descriptor.identifier)
 * is written NSDataWritingAtomic | NSDataWritingFileProtectionNone by +[PBFPosterModelStoreCoordinator
 * createNewStoreCoordinatorWithPath:error:], and that write failed for every default poster (run glass12: "dynamic
 * descriptor space.dynamic made: NO ... errno=-1"), so no lock screen poster was ever made. Said with its error, then
 * once more without the protection class, then without the atomic rename as well.
 */
static BOOL (*orig_data_write)(id, SEL, id, unsigned long, id *);

static BOOL
pb_data_write(id self, SEL sel, id url, unsigned long options, id *error)
{
	char a[300], c[900];
	id local = nil;
	BOOL ok = orig_data_write(self, sel, url, options, error != NULL ? error : &local);
	int saved = errno;
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("hakios: PosterBoard: write %s (options %#lx) FAILED, errno %d: %s\n", pb_desc(url, a, sizeof(a)), options,
		    saved, pb_desc(e, c, sizeof(c)));
		if (options & 0xf0000000ul) {
			local = nil;
			ok = orig_data_write(self, sel, url, options & ~0xf0000000ul, error != NULL ? error : &local);
			say("hakios: PosterBoard: ... without the protection class: %s\n", ok ? "written" : "FAILED again");
		}
		if (!ok && (options & 1ul)) {
			local = nil;
			ok = orig_data_write(self, sel, url, options & ~0xf0000001ul, error != NULL ? error : &local);
			say("hakios: PosterBoard: ... and not atomically: %s\n", ok ? "written" : "FAILED again");
		}
	}
	return ok;
}

/*
 * Who makes PosterBoard's "errno=-1" errors: the dynamic descriptor of every default poster failed with one whose
 * URL is the descriptor's identity file, after that file was written (run glass13: no write failed). Each such error
 * is said with the calls that made it, as image + offset from the image's header (dladdr names only exported symbols).
 */
static id (*orig_general_error)(id, SEL, long, id);

static id
pb_general_error(id self, SEL sel, long code, id info)
{
	static int said;
	char b[600];

	if (said++ < 6) {
		uintptr_t fp = (uintptr_t)__builtin_frame_address(0);

		say("hakios: PosterBoard: general error %ld %s, made by:\n", code, pb_desc(info, b, sizeof(b)));
		for (int i = 0; i < 10 && fp != 0 && (fp & 7) == 0; i++) {
			uintptr_t *frame = (uintptr_t *)fp;
			uintptr_t ret = frame[1] & 0x0000007fffffffffUL;
			Dl_info dl;

			if (ret == 0) {
				break;
			}
			if (dladdr((void *)ret, &dl) && dl.dli_fname != NULL) {
				say("hakios: PosterBoard:   %s+%#lx (%s)\n", strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 :
				    dl.dli_fname, (unsigned long)(ret - (uintptr_t)dl.dli_fbase), dl.dli_sname != NULL ? dl.dli_sname : "?");
			}
			fp = frame[0];
		}
	}
	return orig_general_error(self, sel, code, info);
}

/* ... and the same for one made straight from the domain (PosterFoundation does not have pbf_generalErrorWithCode:) */
static id (*orig_error_with_domain)(id, SEL, id, long, id);

static id
pb_error_with_domain(id self, SEL sel, id domain, long code, id info)
{
	static int said;
	char d[120];

	if (said < 6 && domain != nil && strcmp(pb_desc(domain, d, sizeof(d)), "com.apple.PosterBoard.generalErrorDomain") == 0) {
		uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
		char b[600];

		said++;
		say("hakios: PosterBoard: error %s %ld %s, made by:\n", d, code, pb_desc(info, b, sizeof(b)));
		for (int i = 0; i < 10 && fp != 0 && (fp & 7) == 0; i++) {
			uintptr_t *frame = (uintptr_t *)fp;
			uintptr_t ret = frame[1] & 0x0000007fffffffffUL;
			Dl_info dl;

			if (ret == 0) {
				break;
			}
			if (dladdr((void *)ret, &dl) && dl.dli_fname != NULL) {
				say("hakios: PosterBoard:   %s+%#lx (%s)\n", strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 :
				    dl.dli_fname, (unsigned long)(ret - (uintptr_t)dl.dli_fbase), dl.dli_sname != NULL ? dl.dli_sname : "?");
			}
			fp = frame[0];
		}
	}
	return orig_error_with_domain(self, sel, domain, code, info);
}

/*
 * THE WALLPAPER'S LAST STEP. Every store PosterBoard makes for a poster or a descriptor has its internal directories
 * marked purgeable first (-[PBFPosterModelStoreCoordinator _accessQueue_correctPermissionsForInternalDirectories:],
 * -[PBFPosterExtensionStoreCoordinator _correctPermissionsForInternalDirectories]) by -[NSURL pbf_setPurgable:error:]:
 * open() and ffsctl(fd, APFSIOC_MARK_PURGEABLE). That is APFS's, and /var here is the kernel's ramfs, whose ioctl
 * answers ENOTTY to everything; the failure comes back as an error with "errno" -1 (the return value), the store is
 * not made, and so neither the hero poster (com.apple.MercuryPoster) nor the legacy one could be set up: no lock screen
 * poster, a black wallpaper (runs glass12-14: "failed to correct file purgable", "errno=-1"). Purgeable is a hint
 * for reclaiming space, which a memory filesystem has no use for: a failure is taken as done.
 */
static BOOL (*orig_set_purgable)(id, SEL, BOOL, id *);

static BOOL
pb_set_purgable(id self, SEL sel, BOOL purgable, id *error)
{
	static int said;
	char a[300];
	id local = nil;
	BOOL ok = orig_set_purgable(self, sel, purgable, error != NULL ? error : &local);

	if (!ok) {
		if (said++ < 3) {
			say("hakios: PosterBoard: %s cannot be marked purgeable here (no APFS): taken as done\n", pb_desc(self, a, sizeof(a)));
		}
		if (error != NULL) {
			*error = nil;
		}
		ok = YES;
	}
	return ok;
}

static BOOL
pb_fm_copy(id self, SEL sel, id from, id to, id *error)
{
	char a[300], b[300], c[900];
	id local = nil;
	BOOL ok = orig_fm_copy(self, sel, from, to, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("hakios: PosterBoard: copy %s -> %s FAILED: %s\n", pb_desc(from, a, sizeof(a)), pb_desc(to, b, sizeof(b)),
		    pb_desc(e, c, sizeof(c)));
	}
	return ok;
}

static BOOL
pb_url_update(id self, SEL sel, id values, id *error)
{
	char a[300], b[400], c[900];
	id local = nil;
	BOOL ok = orig_url_update(self, sel, values, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("hakios: PosterBoard: resource values %s on %s FAILED: %s\n", pb_desc(values, b, sizeof(b)),
		    pb_desc(self, a, sizeof(a)), pb_desc(e, c, sizeof(c)));
		id plain = pb_without(values, "NSURLFileProtectionKey");
		if (plain != nil) {
			local = nil;
			ok = orig_url_update(self, sel, plain, error != NULL ? error : &local);
			say("hakios: PosterBoard: resource values on %s without NSURLFileProtectionKey: %s\n",
			    pb_desc(self, a, sizeof(a)), ok ? "set" : "FAILED again");
		}
	}
	return ok;
}

/*
 * Where PosterBoard traps. With its data store made at last (pb_url_conform) it dies of SIGTRAP ~100 ms after
 * every launch, right after "Data store migration not needed" (run glass5), and no crash report reaches the
 * console. The trap's pc and the frame chain, each named by dladdr, then the default action.
 */
static void
pb_say_frame(const char *what, uintptr_t pc)
{
	Dl_info info;

	if (pc != 0 && dladdr((void *)pc, &info) && info.dli_fname != NULL) {
		const char *image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
		say("hakios: PosterBoard: trap %s %#lx %s`%s+%#lx\n", what, (unsigned long)pc, image,
		    info.dli_sname != NULL ? info.dli_sname : "?",
		    (unsigned long)(pc - (uintptr_t)(info.dli_saddr != NULL ? info.dli_saddr : info.dli_fbase)));
	} else {
		say("hakios: PosterBoard: trap %s %#lx\n", what, (unsigned long)pc);
	}
}

static void
pb_trap(int sig, siginfo_t *si, void *context)
{
	ucontext_t *uc = context;
	uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
	uintptr_t lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
	uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);

	(void)si;
	say("hakios: PosterBoard: signal %d\n", sig);
	pb_say_frame("pc", pc & 0x0000007fffffffffUL);
	pb_say_frame("lr", lr & 0x0000007fffffffffUL);
	for (int i = 0; i < 24 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t *frame = (uintptr_t *)fp;
		uintptr_t ret = frame[1] & 0x0000007fffffffffUL;
		if (ret == 0) {
			break;
		}
		pb_say_frame("fr", ret);
		fp = frame[0];
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
posterboard_fixes(void)
{
	Class helper = objc_getClass("PBFInitialPosterHelper");
	Method m = helper != Nil ? class_getClassMethod(helper,
	    sel_registerName("initialPosterConfigurationsWithModelCoordinatorProvider:role:")) : NULL;

	/* (in each, the original is kept before the replacement is set: another thread can call it at once) */
	if (m != NULL) {
		orig_initial_posters = (id (*)(id, SEL, id, id))method_getImplementation(m);
		method_setImplementation(m, (IMP)pb_initial_posters);
	}
	swizzle("PBFPosterRoleCoordinator", "setupRoleIfNecessaryWithStorage:", (IMP)pb_setup_role, (IMP *)&orig_setup_role);
	{
		Class migrator = objc_getClass("PBFPosterExtensionDataStoreMigrator");
		Method mm = migrator != Nil ? class_getClassMethod(migrator,
		    sel_registerName("migrateDataStoreAtBaseURL:fromVersion:toVersion:cleanupAfterMigrationSucceeds:error:")) : NULL;
		if (mm != NULL) {
			orig_migrate = (BOOL (*)(Class, SEL, id, unsigned long, unsigned long, BOOL, id *))method_getImplementation(mm);
			method_setImplementation(mm, (IMP)pb_migrate);
		}
	}
	swizzle("PBFPosterExtensionDataStoreXPCServiceGlue",
	    "buildDataStoreForURL:runtimeAssertionProvider:applicationStateMonitor:observer:wasMigrationJustPerformed:error:",
	    (IMP)pb_build, (IMP *)&orig_build);
	swizzle("NSFileManager", "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", (IMP)pb_fm_create,
	    (IMP *)&orig_fm_create);
	swizzle("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)pb_fm_copy, (IMP *)&orig_fm_copy);
	swizzle("NSData", "writeToURL:options:error:", (IMP)pb_data_write, (IMP *)&orig_data_write);
	swizzle("NSURL", "pbf_setPurgable:error:", (IMP)pb_set_purgable, (IMP *)&orig_set_purgable);
	{
		Method me = class_getClassMethod(objc_getClass("NSError"), sel_registerName("pbf_generalErrorWithCode:userInfo:"));
		if (me != NULL) {
			orig_general_error = (id (*)(id, SEL, long, id))method_getImplementation(me);
			method_setImplementation(me, (IMP)pb_general_error);
		} else {
			say("hakios: +[NSError pbf_generalErrorWithCode:userInfo:]: not found\n");
		}
		me = class_getClassMethod(objc_getClass("NSError"), sel_registerName("errorWithDomain:code:userInfo:"));
		if (me != NULL) {
			orig_error_with_domain = (id (*)(id, SEL, id, long, id))method_getImplementation(me);
			method_setImplementation(me, (IMP)pb_error_with_domain);
		}
	}
	swizzle("NSURL", "pbf_recursivelyUpdateResourceValues:error:", (IMP)pb_url_update, (IMP *)&orig_url_update);
	swizzle("NSURL", "pbf_URLIsReachableAndConformToResourceValues:error:", (IMP)pb_url_conform,
	    (IMP *)&orig_url_conform);
	swizzle("PBFPosterExtensionDataStore", "_stateLock_setupRoleCoordinatorsIfNeeded", (IMP)pb_setup_coordinators,
	    (IMP *)&orig_setup_coordinators);
	swizzle("PBFPosterExtensionDataStore", "pbf_posterExtensionStoreCoordinatorForProviderIdentifier:error:",
	    (IMP)pb_store_for_provider, (IMP *)&orig_store_for_provider);
	swizzle("PBFPosterExtensionStoreCoordinator", "staticDescriptorStoreCoordinatorForIdentifier:",
	    (IMP)pb_static_descriptors, (IMP *)&orig_static_descriptors);
	swizzle("PBFPosterExtensionStoreCoordinator", "createDynamicDescriptorStoreCoordinatorForIdentifier:role:error:",
	    (IMP)pb_create_dynamic, (IMP *)&orig_create_dynamic);
	swizzle("PBFWallpaperKitBridge", "shouldInstallHeroPosterAsDefaultLockScreenWallpaper", (IMP)pb_hero,
	    (IMP *)&orig_hero);
	if (helper != Nil) {
		Method mi = class_getClassMethod(helper, sel_registerName("incomingPosterConfigurationForRole:templatePath:userInfo:"));
		if (mi != NULL) {
			orig_incoming = (id (*)(id, SEL, id, id, id))method_getImplementation(mi);
			method_setImplementation(mi, (IMP)pb_incoming);
		}
	}
	say("hakios: PosterBoard: watching its poster setup (%s)\n", m != NULL ? "all three steps" : "no initial poster helper");
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = pb_trap;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGTRAP, &sa, NULL);
		sigaction(SIGILL, &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, pb_report);
}

/*
 * A SAMPLE of this process's threads, for the screen that stops changing (runs glass15-18: once the
 * lock screen poster exists the GPU goes idle and no frame is presented again, with nothing in the
 * log to say who waits for what). At 90 and 150 s after the process starts every thread is
 * suspended in turn and its pc, lr and frame chain said as image+offset (dladdr's names are
 * mostly "<redacted>" in the shared cache; the offsets are symbolized off the phone). Boot-arg
 * sample=0: not. The frame walk stays inside the thread's own stack.
 */
#include <mach/mach.h>

static void
rw_say_pc(const char *tag, unsigned t, const char *what, uintptr_t pc)
{
	Dl_info dl;

	pc &= 0x0000007fffffffffUL;
	if (pc != 0 && dladdr((void *)pc, &dl) && dl.dli_fname != NULL) {
		const char *image = strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname;
		say("hakios: sample %s t%u %s %s+%#lx %s\n", tag, t, what, image, (unsigned long)(pc - (uintptr_t)dl.dli_fbase),
		    dl.dli_sname != NULL ? dl.dli_sname : "?");
	} else {
		say("hakios: sample %s t%u %s %#lx\n", tag, t, what, (unsigned long)pc);
	}
}

/* sample_every=N: the period of a repeated sample, in seconds (0: the two at 90 and 150 s) */
static unsigned rw_sample_period;

/* does one of these addresses lie in the image whose file name has `part` in it? */
static bool
rw_in_image(const uintptr_t *pcs, unsigned n, const char *part)
{
	for (unsigned i = 0; i < n; i++) {
		Dl_info dl;
		uintptr_t pc = pcs[i] & 0x0000007fffffffffUL;

		if (pc != 0 && dladdr((void *)pc, &dl) && dl.dli_fname != NULL && strstr(dl.dli_fname, part) != NULL) {
			return true;
		}
	}
	return false;
}

static void
rw_sample(void *context)
{
	const char *tag = context;
	thread_act_array_t threads;
	mach_msg_type_number_t count = 0;
	thread_t self = mach_thread_self();
	/* copied while a thread is suspended, said after it runs again: one that is stopped while it
	 * holds malloc's or dyld's lock would otherwise hang this one in say() or dladdr() */
	static uintptr_t pcs[18];

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	say("hakios: sample %s: %u threads\n", tag, count);
	for (unsigned t = 0; t < count; t++) {
		arm_thread_state64_t st;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		char tname[64] = "";
		pthread_t pt = pthread_from_mach_thread_np(threads[t]);
		uintptr_t lo = 0, hi = 0;
		unsigned got = 0;

		if (threads[t] == self) {
			continue;
		}
		if (pt != NULL) {
			pthread_getname_np(pt, tname, sizeof(tname));
			hi = (uintptr_t)pthread_get_stackaddr_np(pt);
			lo = hi - pthread_get_stacksize_np(pt);
		}
		if (thread_suspend(threads[t]) != KERN_SUCCESS) {
			continue;
		}
		if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
			uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(st);

			pcs[got++] = (uintptr_t)__darwin_arm_thread_state64_get_pc(st);
			pcs[got++] = (uintptr_t)__darwin_arm_thread_state64_get_lr(st);
			while (got < 18 && fp >= lo && fp + 16 <= hi && (fp & 7) == 0) {
				uintptr_t *frame = (uintptr_t *)fp;
				pcs[got++] = frame[1];
				if (frame[0] <= fp) {
					break;
				}
				fp = frame[0];
			}
		}
		thread_resume(threads[t]);
		/* in a repeated sample (sample_every=), only the threads inside the Metal library */
		if (rw_sample_period != 0 && !rw_in_image(pcs, got, "libhakios-air")) {
			continue;
		}
		say("hakios: sample %s t%u '%s'\n", tag, t, tname);
		for (unsigned i = 0; i < got; i++) {
			rw_say_pc(tag, t, i == 0 ? "pc" : i == 1 ? "lr" : "fr", pcs[i]);
		}
	}
	for (unsigned t = 0; t < count; t++) {
		mach_port_deallocate(mach_task_self(), threads[t]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
	mach_port_deallocate(mach_task_self(), self);
}

static void
rw_schedule_samples(const char *tag)
{
	char args[1024] = { 0 };
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "sample=0") != NULL) {
		return;
	}
	/* sample_every=N: every N seconds from the 30th, 200 times, the threads in the Metal library only --
	 * a profile of where its time goes, which two samples are not (under QEMU a pipeline of
	 * CoreAnimation's took tens of seconds that the host makes in milliseconds) */
	const char *every = strstr(args, "sample_every=");
	if (every != NULL && (every == args || every[-1] == ' ')) {
		rw_sample_period = (unsigned)strtoul(every + strlen("sample_every="), NULL, 10);
	}
	if (rw_sample_period != 0) {
		for (unsigned k = 0; k < 200; k++) {
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(30 + k * rw_sample_period) * NSEC_PER_SEC),
			    dispatch_get_global_queue(0, 0), (void *)tag, rw_sample);
		}
		return;
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), (void *)tag,
	    rw_sample);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), (void *)tag,
	    rw_sample);
}

__attribute__((constructor))
static void
hakios26_init(void)
{
	const char *name = getprogname();

	say("hakios: loaded into %s (pid %d)\n", name ? name : "?", getpid());
	caenv_from_bootargs();
	if (name != NULL && strcmp(name, "backboardd") == 0) {
		input_start();
		rw_schedule_samples("backboardd");
	} else if (name != NULL && strcmp(name, "SpringBoard") == 0) {
		springboard_fixes();
		rw_schedule_samples("SpringBoard");
	} else if (name != NULL && strcmp(name, "PosterBoard") == 0) {
		posterboard_fixes();
	}
}
