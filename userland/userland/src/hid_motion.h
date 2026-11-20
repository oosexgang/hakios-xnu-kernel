/*
 * The accelerometer's samples, given to Darwin's motion stack the way an iPhone's motion processor gives them.
 *
 * The kernel reads the phone's accelerometer (drivers/board/imu.c, sensors=2) and says its latest sample in
 * hw.sensors.sample: the sample's number, the time it was read (mach_absolute_time) and gravity's pull on Darwin's
 * axes in milli-g, the five together. On an iPhone the samples come from the motion processor as events of a HID
 * service of its own: usage page 0xff00 (Apple's), usage 0x03 (accelerometer), built in. locationd, which is what
 * CoreMotion's accelerometer updates come from, opens that service, tells it how often it wants samples (the
 * service's "ReportInterval", in microseconds) and takes from it events of type 13 (accelerometer) whose x, y and z
 * it reads as numbers of g.
 *
 * So backboardd's library (src/inject26.c; src/inject.c on the main Darwin 26/27 image) makes that service beside
 * the orientation's (hid_orientation.h) and sends each sample through it:
 *
 *   the event      of type 13, made by IOHIDEventCreateAccelerometerEvent, x y z in g (the milli-g over 1000) on the
 *                  axes the kernel already gives them on, which are Darwin's motion APIs' own: +x right, +y up,
 *                  +z out of the screen, a phone face up on a table about (0, 0, -1). Its motion type is left at 0,
 *                  a plain sample (locationd drops type 1, a shake). Built in.
 *   its time       the time the kernel read the chip, not the time it was looked at here
 *   when           each sample once, as it comes: the kernel's are looked at every MOTION_LOOK_MS, twice as often
 *                  as it reads them, so none is missed; a sample the kernel has not read again is not sent again
 *   how often      what the service was last asked ("ReportInterval"): a sample at least that long after the last
 *                  one sent (give or take half the kernel's period: never slower than asked, where the kernel can);
 *                  none while it is 0; every one while nobody has asked, or what was asked says "not configured"
 *                  (-1 or 0xffffffff). Never more than the kernel reads: about 10 a second (imu.c's head says why it
 *                  reads no more often). A program that asks CoreMotion for 50 or 100 a second gets those 10, at the
 *                  times they were read.
 *
 * The accelerometer alone here: the gyroscope's rates, which the kernel puts in the same samples (three more numbers
 * after the five, read past here), have a service of their own (hid_gyro.h); there is no compass's, so what CoreMotion
 * works out with one (heading) is not there. The orientation needs neither: it comes from the orientation service.
 *
 * The numbers are those the OnePlus Nord 2 port of iOS read out of locationd of iOS 16.7, where locationd opened its
 * kernel's own accelerometer service, set its rate and was sent samples; that Darwin 26 and 27 read the same is not
 * yet seen on ours. After the idea of the OnePlus Nord 2 port's piece 0760 (frxncyyy/ios-denniz), which gives its sensor hub's
 * samples to iOS from its kernel; nothing of its code is here.
 *
 * The service is made only while the kernel says it follows the orientation for Darwin and that the samples are for
 * the motion stack too (hw.sensors.hid and hw.sensors.motion 1: sensors=2, and neither orientation_hid=0 nor
 * motion_hid=0). Waited for as the orientation's is (ORIENTATION_WAIT_S); once either says 0 the looking stops and
 * the service falls silent. The call that makes the events is looked up by name, not linked: a Darwin without it gets
 * no accelerometer service and a line that says so.
 *
 * Included right after hid_orientation.h, whose kernel number, wait and HID declarations it uses, and compiled with
 * the input part by workbench/backboard_input_test.c and backboard_input16_test.c; its first half -- what is sent
 * when -- is plain arithmetic.
 */
#ifndef HAKIOS_HID_MOTION_H
#define HAKIOS_HID_MOTION_H

/* ---- what is sent when ------------------------------------------------------------------------------ */

#define MOTION_KERNEL_MS        100     /* how often the kernel reads the chip (drivers/board/imu.c, POLL_MS) */
#define MOTION_LOOK_MS          50      /* how often its latest sample is looked at: twice as often */
#define MOTION_NOT_CONFIGURED   0xffffffffLL    /* a "ReportInterval" of this or more (or below 0): nobody has said */

struct motion_feed {
	uint32_t seen;                  /* the number of the last sample looked at (0: none yet) */
	bool sent;                      /* one has been sent ... */
	uint64_t sent_us;               /* ... read at this time, in microseconds */
};

/*
 * What a "ReportInterval" (microseconds; -1: never set) asks of the feed: -1 every sample the kernel reads, 0 none,
 * else at least this many microseconds between two.
 */
static inline long long
motion_interval(long long asked)
{
	return asked < 0 || asked >= MOTION_NOT_CONFIGURED ? -1 : asked;
}

/*
 * Whether the kernel's sample NUMBER, read at TIME_US, is to be sent now that the service was last asked ASKED:
 * not a sample not yet read (0) or already looked at, none while nothing is wanted, and one too soon after the last
 * sent only when it would come sooner than half the kernel's period before what was asked.
 */
static inline bool
motion_feed_take(struct motion_feed *f, uint32_t number, uint64_t time_us, long long asked)
{
	long long interval = motion_interval(asked);

	if (number == 0 || number == f->seen) {
		return false;
	}
	f->seen = number;
	if (interval == 0) {
		return false;
	}
	if (interval > 0 && f->sent && time_us - f->sent_us + MOTION_KERNEL_MS * 1000 / 2 < (uint64_t)interval) {
		return false;
	}
	f->sent = true;
	f->sent_us = time_us;
	return true;
}

/* ---- the service ------------------------------------------------------------------------------------ */

#define HID_PAGE_APPLE_VENDOR           0xff00
#define HID_USAGE_ACCELEROMETER         0x03
#define HID_EVENT_ACCELEROMETER         13      /* kIOHIDEventTypeAccelerometer */

typedef IOHIDEventRef (*motion_event_fn)(CFAllocatorRef allocator, uint64_t time, double x, double y, double z,
    uint32_t options);

static motion_event_fn motion_event_create;

/* One sample as the kernel says it (hw.sensors.sample). */
struct motion_sample {
	uint32_t number;                /* 0: none read yet */
	uint64_t time;                  /* mach_absolute_time when it was read */
	int mg[3];                      /* x y z, milli-g on Darwin's axes */
};

/* The kernel's latest sample. False: it could not be read, or did not say five numbers (the gyroscope's three after them, if
 * any, are hid_gyro.h's). */
static bool
motion_kernel_sample(struct motion_sample *s)
{
	char text[96];
	size_t size = sizeof(text) - 1;
	unsigned long long time;

	if (sysctlbyname("hw.sensors.sample", text, &size, NULL, 0) != 0) {
		return false;
	}
	text[size < sizeof(text) ? size : sizeof(text) - 1] = '\0';
	if (sscanf(text, "%u %llu %d %d %d", &s->number, &time, &s->mg[0], &s->mg[1], &s->mg[2]) != 5) {
		return false;
	}
	s->time = time;
	return true;
}

/* A sample sent through SERVICE as an accelerometer event, dated when it was read. False: no event could be made. */
static bool
motion_send(IOHIDServiceClientRef service, const struct motion_sample *s)
{
	IOHIDEventRef event = motion_event_create(NULL, s->time, s->mg[0] / 1000.0, s->mg[1] / 1000.0, s->mg[2] / 1000.0, 0);

	if (event == NULL) {
		return false;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(service, event);
	CFRelease(event);
	return true;
}

/* Whether the kernel still has the samples for Darwin's motion stack (hw.sensors.hid and hw.sensors.motion). */
static bool
motion_for_darwin(void)
{
	return orientation_kernel_number("hw.sensors.hid") == 1 && orientation_kernel_number("hw.sensors.motion") == 1;
}

/*
 * The accelerometer's own thread: waits for the kernel to say it follows the orientation for Darwin, makes the
 * service if the samples are for the motion stack too, then sends each sample it is asked for, looking every
 * MOTION_LOOK_MS until the kernel says no more.
 */
static void *
motion_thread(void *client)
{
	static mach_timebase_info_data_t base;
	struct motion_feed feed = { 0 };
	struct motion_sample sample;
	struct service *kept = NULL;
	IOHIDServiceClientRef service;
	long long said = -2;            /* the "ReportInterval" last said in the log */
	unsigned sent = 0;
	bool failed = false;
	int waited = 0;

	pthread_setname_np("hakios motion");
	mach_timebase_info(&base);
	while (orientation_kernel_number("hw.sensors.hid") != 1) {
		if (++waited > ORIENTATION_WAIT_S) {
			say("hakios: motion: the kernel does not follow the accelerometer for Darwin (hw.sensors.hid; sensors=2): "
			    "no accelerometer service\n");
			return NULL;
		}
		sleep(1);
	}
	if (orientation_kernel_number("hw.sensors.motion") != 1) {
		say("hakios: motion: the samples are not for Darwin's motion stack (hw.sensors.motion; motion_hid=0): "
		    "no accelerometer service\n");
		return NULL;
	}
	motion_event_create = (motion_event_fn)dlsym(RTLD_DEFAULT, "IOHIDEventCreateAccelerometerEvent");
	if (motion_event_create == NULL) {
		say("hakios: motion: this Darwin's IOKit has no IOHIDEventCreateAccelerometerEvent: no accelerometer service\n");
		return NULL;
	}
	service = make_service_kept((IOHIDEventSystemClientRef)client, "HakiOS accelerometer", HID_PAGE_APPLE_VENDOR,
	    HID_USAGE_ACCELEROMETER, false, &kept);
	say("hakios: motion: service %p (page %#x, usage %#x), at most every %d ms\n", (void *)service, HID_PAGE_APPLE_VENDOR,
	    HID_USAGE_ACCELEROMETER, MOTION_KERNEL_MS);
	if (service == NULL) {
		return NULL;
	}
	while (motion_for_darwin()) {
		long long asked = kept->report_interval;

		if (asked != said) {
			say("hakios: motion: asked for a sample every %lld us (%s)\n", asked, motion_interval(asked) == 0 ? "none" :
			    motion_interval(asked) < 0 ? "not configured: every one read" :
			    motion_interval(asked) <= MOTION_KERNEL_MS * 1000 ? "every one read: the kernel reads no more often" :
			    "one at least that long after the last");
			said = asked;
		}
		if (motion_interval(asked) != 0 && motion_kernel_sample(&sample) &&
		    motion_feed_take(&feed, sample.number, sample.time * base.numer / base.denom / 1000, asked)) {
			if (motion_send(service, &sample)) {
				if (sent++ == 0) {
					say("hakios: motion: the first sample sent: %d %d %d milli-g\n", sample.mg[0], sample.mg[1],
					    sample.mg[2]);
				}
			} else if (!failed) {
				say("hakios: motion: no event could be made for a sample\n");
				failed = true;
			}
		}
		usleep(MOTION_LOOK_MS * 1000);
	}
	say("hakios: motion: the kernel no longer gives the samples (%u sent): the service falls silent\n", sent);
	return NULL;
}

/* From the input thread, beside orientation_start(): the accelerometer's thread, if this kernel can say anything to it. */
static void
motion_start(IOHIDEventSystemClientRef client)
{
	pthread_t thread;

	if (orientation_kernel_number("hw.sensors.motion") < 0) {
		return;                 /* (a kernel without the motion sensor's driver, or from before it said this) */
	}
	if (pthread_create(&thread, NULL, motion_thread, (void *)client) == 0) {
		pthread_detach(thread);
	}
}

#endif /* HAKIOS_HID_MOTION_H */
