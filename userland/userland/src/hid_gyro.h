/*
 * The gyroscope's rates, given to Darwin's motion stack the way an iPhone's motion processor gives them.
 *
 * The kernel reads the phone's gyroscope with its accelerometer (drivers/board/imu.c, sensors=2) and says the rates in
 * the same sample as the accelerometer's milli-g: hw.sensors.sample is "NUMBER TIME X Y Z GX GY GZ" for a read that has
 * them, the last three in milli-rad/s on Darwin's axes, and the five of before for one that has not; hw.sensors.gyro
 * says 1 while the samples carry them. On an iPhone the rates come from the motion processor as events of a HID service
 * of its own, beside its accelerometer's: Apple's usage page 0xff00, usage 0x09 (gyroscope), built in. locationd's
 * gyroscope client opens that service, tells it how often it wants rates ("ReportInterval", microseconds, as the
 * accelerometer's) and takes from it events of type 20 (gyroscope) whose x, y and z are radians per second.
 *
 * So backboardd's library (src/inject26.c; src/inject.c on the main Darwin 26/27 image) makes that service as a third
 * one beside the orientation's and the accelerometer's (hid_orientation.h, hid_motion.h) and sends each sample's rates
 * through it:
 *
 *   the event      of type 20, made by IOHIDEventCreateGyroEvent, x y z in radians per second (the milli-rad/s over
 *                  1000) on the axes the kernel already gives them on, Darwin's motion APIs' own: +x right, +y up,
 *                  +z out of the screen, a turn counter-clockwise about an axis, seen from its + end, positive (the
 *                  right-hand rule: a phone flat on a table, turned to the left, reads +z). Its motion type left at 0,
 *                  a plain sample. Built in.
 *   its time       the time the kernel read the chip, the accelerometer's sample's own
 *   when, how often  as the accelerometer's samples (hid_motion.h: motion_feed_take()), by what this service was last
 *                  asked: each sample once, at least the interval asked between two, none while 0, every one while
 *                  nobody has asked -- and never more than the kernel reads, about 10 a second. A sample without
 *                  rates is not sent: there is nothing in it to send.
 *
 * The numbers -- the usage and the event -- are those the OnePlus Nord 2 port of iOS read out of locationd of iOS 16.7;
 * the unit and the axes are what that port gave its sensor hub's gyroscope to iOS in: radians per second, on Android's
 * axes and with Android's signs, which for a rate are Darwin's (only the accelerometer's sign differs between the
 * two). That Darwin 26 and 27 read the same is not yet seen on ours.
 * After the idea of the OnePlus Nord 2 port's piece 0760 (frxncyyy/ios-denniz); nothing of its code is here.
 *
 * The service is made only once the kernel says it follows the sensor for Darwin, gives the samples to the motion
 * stack and they carry the gyroscope's rates (hw.sensors.hid, hw.sensors.motion and hw.sensors.gyro 1: sensors=2, none
 * of orientation_hid=0, motion_hid=0, gyro_hid=0, and a gyroscope the kernel has seen answer). A gyroscope that never
 * answers -- not there, powered down, reading zeros or the same rates each time -- gets no service: hw.sensors.gyro is
 * waited for as hw.sensors.hid is (ORIENTATION_WAIT_S) and then given up. Once any of the three says 0 (the gyroscope
 * held stuck, the chip lost, the run's end) the looking stops and the service falls silent. The call that makes the
 * events is looked up by name, not linked: a Darwin without IOHIDEventCreateGyroEvent gets no gyroscope service and a
 * line that says so.
 *
 * Included right after hid_motion.h, whose feed, kernel numbers and service declarations it uses, and compiled with the
 * input part by workbench/backboard_input_test.c and backboard_input16_test.c.
 */
#ifndef HAKIOS_HID_GYRO_H
#define HAKIOS_HID_GYRO_H

#define HID_USAGE_GYRO                  0x09
#define HID_EVENT_GYRO                  20      /* kIOHIDEventTypeGyro */

typedef IOHIDEventRef (*gyro_event_fn)(CFAllocatorRef allocator, uint64_t time, double x, double y, double z, uint32_t options);

static gyro_event_fn gyro_event_create;

/* One sample's rates as the kernel says them (hw.sensors.sample). */
struct gyro_sample {
	uint32_t number;                /* the sample's number */
	uint64_t time;                  /* mach_absolute_time when it was read */
	int mrad[3];                    /* x y z, milli-rad/s on Darwin's axes */
};

/* The kernel's latest sample, if it carries the gyroscope's rates. False: it could not be read, or has no rates. */
static bool
gyro_kernel_sample(struct gyro_sample *s)
{
	char text[128];
	size_t size = sizeof(text) - 1;
	unsigned long long time;
	int mg[3];

	if (sysctlbyname("hw.sensors.sample", text, &size, NULL, 0) != 0) {
		return false;
	}
	text[size < sizeof(text) ? size : sizeof(text) - 1] = '\0';
	if (sscanf(text, "%u %llu %d %d %d %d %d %d", &s->number, &time, &mg[0], &mg[1], &mg[2], &s->mrad[0], &s->mrad[1],
	    &s->mrad[2]) != 8) {
		return false;
	}
	s->time = time;
	return true;
}

/* A sample's rates sent through SERVICE as a gyroscope event, dated when they were read. False: no event could be made. */
static bool
gyro_send(IOHIDServiceClientRef service, const struct gyro_sample *s)
{
	IOHIDEventRef event = gyro_event_create(NULL, s->time, s->mrad[0] / 1000.0, s->mrad[1] / 1000.0, s->mrad[2] / 1000.0, 0);

	if (event == NULL) {
		return false;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(service, event);
	CFRelease(event);
	return true;
}

/* Whether the kernel still gives the gyroscope's rates to Darwin's motion stack (hw.sensors.hid, .motion and .gyro). */
static bool
gyro_for_darwin(void)
{
	return motion_for_darwin() && orientation_kernel_number("hw.sensors.gyro") == 1;
}

/*
 * The gyroscope's own thread: waits for the kernel to say it follows the sensor for Darwin and gives the motion stack
 * the gyroscope's rates, makes the service, then sends each sample's rates it is asked for, looking every
 * MOTION_LOOK_MS until the kernel says no more.
 */
static void *
gyro_thread(void *client)
{
	static mach_timebase_info_data_t base;
	struct motion_feed feed = { 0 };
	struct gyro_sample sample;
	struct service *kept = NULL;
	IOHIDServiceClientRef service;
	long long said = -2;            /* the "ReportInterval" last said in the log */
	unsigned sent = 0;
	bool failed = false;
	int waited = 0;

	pthread_setname_np("hakios gyro");
	mach_timebase_info(&base);
	while (orientation_kernel_number("hw.sensors.hid") != 1) {
		if (++waited > ORIENTATION_WAIT_S) {
			say("hakios: gyro: the kernel does not follow the sensor for Darwin (hw.sensors.hid; sensors=2): no gyroscope "
			    "service\n");
			return NULL;
		}
		sleep(1);
	}
	if (orientation_kernel_number("hw.sensors.motion") != 1) {
		say("hakios: gyro: the samples are not for Darwin's motion stack (hw.sensors.motion; motion_hid=0): no gyroscope "
		    "service\n");
		return NULL;
	}
	for (waited = 0; orientation_kernel_number("hw.sensors.gyro") != 1; sleep(1)) {
		if (++waited > ORIENTATION_WAIT_S || !motion_for_darwin()) {
			say("hakios: gyro: the kernel gives no gyroscope's rates (hw.sensors.gyro; gyro_hid=0, or a gyroscope that does "
			    "not answer): no gyroscope service\n");
			return NULL;
		}
	}
	gyro_event_create = (gyro_event_fn)dlsym(RTLD_DEFAULT, "IOHIDEventCreateGyroEvent");
	if (gyro_event_create == NULL) {
		say("hakios: gyro: this Darwin's IOKit has no IOHIDEventCreateGyroEvent: no gyroscope service\n");
		return NULL;
	}
	service = make_service_kept((IOHIDEventSystemClientRef)client, "HakiOS gyroscope", HID_PAGE_APPLE_VENDOR, HID_USAGE_GYRO,
	    false, &kept);
	say("hakios: gyro: service %p (page %#x, usage %#x), at most every %d ms\n", (void *)service, HID_PAGE_APPLE_VENDOR,
	    HID_USAGE_GYRO, MOTION_KERNEL_MS);
	if (service == NULL) {
		return NULL;
	}
	while (gyro_for_darwin()) {
		long long asked = kept->report_interval;

		if (asked != said) {
			say("hakios: gyro: asked for rates every %lld us (%s)\n", asked, motion_interval(asked) == 0 ? "none" :
			    motion_interval(asked) < 0 ? "not configured: every sample read" :
			    motion_interval(asked) <= MOTION_KERNEL_MS * 1000 ? "every sample read: the kernel reads no more often" :
			    "one at least that long after the last");
			said = asked;
		}
		if (motion_interval(asked) != 0 && gyro_kernel_sample(&sample) &&
		    motion_feed_take(&feed, sample.number, sample.time * base.numer / base.denom / 1000, asked)) {
			if (gyro_send(service, &sample)) {
				if (sent++ == 0) {
					say("hakios: gyro: the first rates sent: %d %d %d milli-rad/s\n", sample.mrad[0], sample.mrad[1],
					    sample.mrad[2]);
				}
			} else if (!failed) {
				say("hakios: gyro: no event could be made for a sample's rates\n");
				failed = true;
			}
		}
		usleep(MOTION_LOOK_MS * 1000);
	}
	say("hakios: gyro: the kernel no longer gives the gyroscope's rates (%u sent): the service falls silent\n", sent);
	return NULL;
}

/* From the input thread, beside motion_start(): the gyroscope's thread, if this kernel can say anything to it. */
static void
gyro_start(IOHIDEventSystemClientRef client)
{
	pthread_t thread;

	if (orientation_kernel_number("hw.sensors.gyro") < 0) {
		return;                 /* (a kernel without the motion sensor's driver, or from before it said this) */
	}
	if (pthread_create(&thread, NULL, gyro_thread, (void *)client) == 0) {
		pthread_detach(thread);
	}
}

#endif /* HAKIOS_HID_GYRO_H */
