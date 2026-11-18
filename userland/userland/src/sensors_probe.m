/*
 * sensors_probe: what Darwin sees of the phone's motion sensors, so the sensor path can be brought up and
 * watched from the phone itself.   usage: sensors_probe [SECONDS]
 *
 *   1. MobileGestalt: the capability keys SpringBoard and CoreMotion read to decide a sensor is there
 *      ("(none)" = the key is unknown, which reads as "not fitted").
 *   2. CoreLocation: whether a heading (the compass) is available and the authorization the Compass app has.
 *   3. CoreMotion: the availability flags, then accelerometer / gyroscope / magnetometer / device-motion
 *      samples once a second for SECONDS (default 8). Device motion uses the magnetic-north reference frame,
 *      which is what the Compass app and auto-rotate's gravity read.
 *
 * It only reads; it changes nothing. A kernel that does not yet feed the sensors shows every value nil or
 * zero -- that is the thing to fix, in the sensor-hub driver, not here.
 */
#import <CoreFoundation/CoreFoundation.h>
#import <CoreLocation/CoreLocation.h>
#import <CoreMotion/CoreMotion.h>
#import <stdio.h>
#import <stdlib.h>
#import <unistd.h>

/* MobileGestalt is not in the SDK's headers; it is linked for its two entry points. */
extern CFTypeRef MGCopyAnswer(CFStringRef key);

static void
mobilegestalt(void)
{
	static const char *const keys[] = {
		"accelerometer", "gyroscope", "magnetometer", "compass", "ambient-light-sensor", "proximity-sensor",
		"DeviceSupportsHeading", "DeviceSupportsAlwaysOnCompass", "gps", "location-services",
	};
	printf("== MobileGestalt\n");
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
		CFTypeRef value = MGCopyAnswer(key);
		char shown[128] = "(none)";
		if (value) {
			if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
				snprintf(shown, sizeof(shown), "%s", CFBooleanGetValue(value) ? "true" : "false");
			} else if (CFGetTypeID(value) == CFStringGetTypeID()) {
				CFStringGetCString(value, shown, sizeof(shown), kCFStringEncodingUTF8);
			} else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
				long long n = 0;
				CFNumberGetValue(value, kCFNumberLongLongType, &n);
				snprintf(shown, sizeof(shown), "%lld", n);
			} else {
				snprintf(shown, sizeof(shown), "(other)");
			}
			CFRelease(value);
		}
		printf("  %-30s %s\n", keys[i], shown);
		CFRelease(key);
	}
}

static void
corelocation(void)
{
	printf("== CoreLocation\n");
	printf("  headingAvailable %d  locationServicesEnabled %d\n",
	    (int)[CLLocationManager headingAvailable], (int)[CLLocationManager locationServicesEnabled]);
}

static void
coremotion(int seconds)
{
	CMMotionManager *m = [[CMMotionManager alloc] init];
	printf("== CoreMotion\n");
	printf("  available: accel %d gyro %d mag %d deviceMotion %d\n",
	    (int)m.accelerometerAvailable, (int)m.gyroAvailable, (int)m.magnetometerAvailable, (int)m.deviceMotionAvailable);

	m.deviceMotionUpdateInterval = 0.1;
	[m startAccelerometerUpdates];
	[m startGyroUpdates];
	[m startMagnetometerUpdates];
	[m startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXMagneticNorthZVertical];

	for (int t = 0; t < seconds; t++) {
		sleep(1);
		CMAccelerometerData *a = m.accelerometerData;
		CMGyroData *g = m.gyroData;
		CMMagnetometerData *mag = m.magnetometerData;
		CMDeviceMotion *d = m.deviceMotion;
		CMAcceleration av = a ? a.acceleration : (CMAcceleration){ 0, 0, 0 };
		CMRotationRate gv = g ? g.rotationRate : (CMRotationRate){ 0, 0, 0 };
		CMMagneticField mv = mag ? mag.magneticField : (CMMagneticField){ 0, 0, 0 };
		printf("  t=%d  acc %s%.3f %.3f %.3f g | gyro %s%.3f %.3f %.3f rad/s | mag %s%.1f %.1f %.1f uT",
		    t + 1, a ? "" : "(nil) ", av.x, av.y, av.z, g ? "" : "(nil) ", gv.x, gv.y, gv.z,
		    mag ? "" : "(nil) ", mv.x, mv.y, mv.z);
		if (d) {
			CMAttitude *att = d.attitude;
			printf(" | roll %.2f pitch %.2f yaw %.2f heading %.1f\n", att.roll, att.pitch, att.yaw, d.heading);
		} else {
			printf(" | dm (nil)\n");
		}
		fflush(stdout);
	}

	[m stopDeviceMotionUpdates];
	[m stopMagnetometerUpdates];
	[m stopGyroUpdates];
	[m stopAccelerometerUpdates];
}

int
main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 8;
	if (seconds <= 0) {
		seconds = 8;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		mobilegestalt();
		corelocation();
		coremotion(seconds);
	}
	return 0;
}
