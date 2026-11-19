/*
 * The phone's orientation, given to Darwin the way an iPhone's always-on processor gives it.
 *
 * The kernel follows which way up the phone is held (drivers/board/imu.c, sensors=2: hw.sensors.orientation, a
 * UIDeviceOrientation, 1 portrait ... 6 face down, 0 not known), and until now only its log heard of a turn: nothing
 * of Darwin's reads a sysctl of ours. On an iPhone the orientation is worked out by the always-on processor, which
 * hands it to the HID event system as events of a service of its own: usage page 0x20 (sensors), usage 0x8a (device
 * orientation), built in. locationd's orientation notifier opens that service whatever the device, and on a device
 * whose orientation comes from such a processor takes the orientation from its events and tells the rest of the
 * system -- backboardd's auto-rotate among them.
 *
 * So backboardd's library (src/inject26.c; src/inject.c on the main Darwin 26/27 image), which already makes the
 * touch screen and the buttons as virtual services of backboardd's own event server, makes this one too, and sends
 * the kernel's orientation through it:
 *
 *   the event      of type 10 (orientation), its kind "a usage" (field 0xa0003 = 1) and the orientation as a usage
 *                  (field 0xa0004 = 0x64 + the UIDeviceOrientation: 0x65 portrait ... 0x6a face down), built in
 *   its child      first and only, a vendor-defined event of page 0xff15, usage 2, whose eight bytes are the time
 *                  the orientation was taken (here mach_absolute_time, which only ever grows): locationd drops an
 *                  orientation whose time is not newer than the last one it took
 *   when           at once when the kernel's orientation changes to one that is known; again every
 *                  ORIENTATION_AGAIN_MS, for a locationd that was started (or started again) after the last change;
 *                  never "not known" -- a sensor that goes quiet leaves what was said last, as on a phone
 *
 * The numbers are those the OnePlus Nord 2 port of iOS read out of locationd of iOS 16.7, which auto-rotated by them on
 * that phone; that Darwin 26 and 27 use the same is not yet seen on ours. After the idea of the OnePlus Nord 2 port's
 * piece 0761 (frxncyyy/ios-denniz), which gives the orientation from its kernel's own HID service; nothing of its code
 * is here.
 *
 * The service is made only while the kernel says the orientation is followed and is for Darwin (hw.sensors.hid 1:
 * sensors=2, and not orientation_hid=0); with a kernel that has no hw.sensors.hid, or that never says 1 within
 * ORIENTATION_WAIT_S of backboardd's start, nothing is made and nothing is looked at again. Once the kernel says 0
 * (the chip lost, the run's end) the looking stops and the service keeps what it said last.
 *
 * The two calls that make the events are looked up by name (IOHIDEventCreate, IOHIDEventCreateVendorDefinedEvent),
 * not linked: a Darwin without one of them gets no orientation service and a line that says so, not a backboardd that
 * does not start.
 *
 * This file is included in the middle of each library's input part, after make_service(), say() and the HID
 * declarations it uses, and is compiled with that part by workbench/backboard_input_test.c and
 * backboard_input16_test.c; its first half -- what is sent when -- is plain arithmetic.
 */
#ifndef HAKIOS_HID_ORIENTATION_H
#define HAKIOS_HID_ORIENTATION_H

/* ---- what is sent when ------------------------------------------------------------------------------ */

#define ORIENTATION_LOOK_MS     100     /* how often the kernel's orientation is looked at: as often as the kernel reads the chip */
#define ORIENTATION_AGAIN_MS    10000   /* the orientation said again, unchanged, after this long */
#define ORIENTATION_WAIT_S      60      /* how long after backboardd's start the kernel may take to say it follows the orientation */
#define ORIENTATION_USAGE_BASE  0x64    /* the usage of "not known"; UIDeviceOrientation N is this plus N */

struct orientation_feed {
	int sent;                       /* the UIDeviceOrientation said last (1..6); 0: nothing said yet */
	uint64_t sent_ms;               /* when, in milliseconds of the caller's clock */
	uint64_t stamp;                 /* the time the last event carried: the next one carries a later one */
};

/*
 * What to say now that the kernel's orientation is NOW (-1: it could not be read) at NOW_MS: the orientation to send,
 * or 0 for nothing. A known orientation is said when it is not the one said last, or when that was said
 * ORIENTATION_AGAIN_MS ago or more; "not known" and an orientation that could not be read are never said.
 */
static inline int
orientation_feed_next(struct orientation_feed *f, int now, uint64_t now_ms)
{
	if (now < 1 || now > 6) {
		return 0;
	}
	if (now == f->sent && now_ms - f->sent_ms < ORIENTATION_AGAIN_MS) {
		return 0;
	}
	f->sent = now;
	f->sent_ms = now_ms;
	return now;
}

/* The time an event is to carry for a sample taken at TIME: TIME, or one past the last when the clock has not moved on. */
static inline uint64_t
orientation_feed_stamp(struct orientation_feed *f, uint64_t time)
{
	f->stamp = time > f->stamp ? time : f->stamp + 1;
	return f->stamp;
}

/* The usage an orientation (0..6) is said by. */
static inline uint32_t
orientation_usage(int orientation)
{
	return ORIENTATION_USAGE_BASE + (uint32_t)orientation;
}

/* ---- the service ------------------------------------------------------------------------------------ */

#define HID_PAGE_SENSOR                 0x20
#define HID_USAGE_DEVICE_ORIENTATION    0x8a
#define HID_EVENT_ORIENTATION           10      /* kIOHIDEventTypeOrientation */
#define FIELD_ORIENTATION_KIND          0xa0003 /* how the orientation is given: ... */
#define ORIENTATION_KIND_USAGE          1       /* ... as a usage, in the next field */
#define FIELD_ORIENTATION_USAGE         0xa0004
#define HID_PAGE_SENSOR_TIME            0xff15  /* the child that carries when the orientation was taken */
#define HID_USAGE_SENSOR_TIME           2

typedef IOHIDEventRef (*orientation_event_fn)(CFAllocatorRef allocator, uint32_t type, uint64_t time, uint32_t options);
typedef IOHIDEventRef (*orientation_vendor_fn)(CFAllocatorRef allocator, uint64_t time, uint32_t page, uint32_t usage,
    uint32_t version, const uint8_t *data, CFIndex length, uint32_t options);

static orientation_event_fn orientation_event_create;
static orientation_vendor_fn orientation_vendor_create;

/* A number the kernel says (an int sysctl), or -1 when it says none. */
static int
orientation_kernel_number(const char *name)
{
	int value = 0;
	size_t size = sizeof(value);

	if (sysctlbyname(name, &value, &size, NULL, 0) != 0 || size != sizeof(value)) {
		return -1;
	}
	return value;
}

/* Milliseconds of mach_absolute_time. */
static uint64_t
orientation_now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

/* One orientation (1..6) sent through SERVICE, carrying the time STAMP. False: an event could not be made. */
static bool
orientation_send(IOHIDServiceClientRef service, int orientation, uint64_t stamp)
{
	uint8_t when[8];
	IOHIDEventRef event, child;

	for (unsigned i = 0; i < sizeof(when); i++) {
		when[i] = (uint8_t)(stamp >> (8 * i));          /* (as the processor's own: little-endian) */
	}
	if ((event = orientation_event_create(NULL, HID_EVENT_ORIENTATION, stamp, 0)) == NULL) {
		return false;
	}
	if ((child = orientation_vendor_create(NULL, stamp, HID_PAGE_SENSOR_TIME, HID_USAGE_SENSOR_TIME, 0, when, sizeof(when), 0))
	    == NULL) {
		CFRelease(event);
		return false;
	}
	IOHIDEventSetIntegerValue(event, FIELD_ORIENTATION_KIND, ORIENTATION_KIND_USAGE);
	IOHIDEventSetIntegerValue(event, FIELD_ORIENTATION_USAGE, (CFIndex)orientation_usage(orientation));
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDEventAppendEvent(event, child, 0);
	CFRelease(child);
	IOHIDVirtualServiceClientDispatchEvent(service, event);
	CFRelease(event);
	return true;
}

/* One look at the kernel's orientation at NOW_MS, and what it calls for sent through SERVICE. */
static void
orientation_look(IOHIDServiceClientRef service, struct orientation_feed *f, uint64_t now_ms)
{
	static const char *const names[] = { "not known", "portrait", "portrait upside down", "landscape left", "landscape right",
		                             "face up", "face down" };
	int before = f->sent, now = orientation_feed_next(f, orientation_kernel_number("hw.sensors.orientation"), now_ms);

	if (now == 0) {
		return;
	}
	if (!orientation_send(service, now, orientation_feed_stamp(f, mach_absolute_time()))) {
		say("hakios: orientation: no event could be made for %s\n", names[now]);
	} else if (now != before) {
		say("hakios: orientation: %s (usage %#x)\n", names[now], orientation_usage(now));
	}
}

/*
 * The service, once the two calls that make its events are found. NULL: they are not there (said), or the event
 * server would not take the service.
 */
static IOHIDServiceClientRef
orientation_service_make(IOHIDEventSystemClientRef client)
{
	IOHIDServiceClientRef service;

	orientation_event_create = (orientation_event_fn)dlsym(RTLD_DEFAULT, "IOHIDEventCreate");
	orientation_vendor_create = (orientation_vendor_fn)dlsym(RTLD_DEFAULT, "IOHIDEventCreateVendorDefinedEvent");
	if (orientation_event_create == NULL || orientation_vendor_create == NULL) {
		say("hakios: orientation: this Darwin's IOKit has no %s: no orientation service\n",
		    orientation_event_create == NULL ? "IOHIDEventCreate" : "IOHIDEventCreateVendorDefinedEvent");
		return NULL;
	}
	service = make_service(client, "HakiOS orientation", HID_PAGE_SENSOR, HID_USAGE_DEVICE_ORIENTATION, false);
	say("hakios: orientation: service %p (page %#x, usage %#x)\n", (void *)service, HID_PAGE_SENSOR, HID_USAGE_DEVICE_ORIENTATION);
	return service;
}

/*
 * The orientation's own thread: waits for the kernel to say it follows the orientation for Darwin, makes the service,
 * then looks every ORIENTATION_LOOK_MS until the kernel says it no longer does.
 */
static void *
orientation_thread(void *client)
{
	struct orientation_feed feed = { 0 };
	IOHIDServiceClientRef service;
	int waited = 0;

	pthread_setname_np("hakios orientation");
	while (orientation_kernel_number("hw.sensors.hid") != 1) {
		if (++waited > ORIENTATION_WAIT_S) {
			say("hakios: orientation: the kernel does not follow the orientation for Darwin (hw.sensors.hid; sensors=2): "
			    "no orientation service\n");
			return NULL;
		}
		sleep(1);
	}
	if ((service = orientation_service_make((IOHIDEventSystemClientRef)client)) == NULL) {
		return NULL;
	}
	while (orientation_kernel_number("hw.sensors.hid") == 1) {
		orientation_look(service, &feed, orientation_now_ms());
		usleep(ORIENTATION_LOOK_MS * 1000);
	}
	say("hakios: orientation: the kernel no longer follows the orientation: what was said last stands\n");
	return NULL;
}

/* From the input thread, once the touch screen is made: the orientation's thread, if this kernel can say anything to it. */
static void
orientation_start(IOHIDEventSystemClientRef client)
{
	pthread_t thread;

	if (orientation_kernel_number("hw.sensors.hid") < 0) {
		return;                 /* (a kernel without the motion sensor's driver, or from before it said this) */
	}
	if (pthread_create(&thread, NULL, orientation_thread, (void *)client) == 0) {
		pthread_detach(thread);
	}
}

#endif /* HAKIOS_HID_ORIENTATION_H */
