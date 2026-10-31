/*
 * camera_avtest: what cameras iOS lists for applications, as AVFoundation gives them: the video
 * devices, and every format of the default one (on this machine the device is libredwood-media.dylib's
 * in mediaserverd: camera_device.c, the kernel's /dev/redwood-camera, cam=5).
 *
 * usage: camera_avtest
 *
 * Frames are for applications in the foreground, as iOS has it: a program like this one is not
 * given any (its session is interrupted at once). Camera.app is the test for those (rc.jobs.camera).
 */
#import <AVFoundation/AVFoundation.h>
#include <stdio.h>

int
main(void)
{
	@autoreleasepool {
		setvbuf(stdout, NULL, _IOLBF, 0);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		NSArray *devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
		printf("camera_avtest: %lu video device(s)\n", (unsigned long)devices.count);
		for (AVCaptureDevice *d in devices) {
			printf("camera_avtest:   %s (%s), position %ld, %lu formats\n", d.localizedName.UTF8String, d.uniqueID.UTF8String,
			    (long)d.position, (unsigned long)d.formats.count);
		}
		AVCaptureDevice *device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
		if (device == nil) {
			printf("camera_avtest: no default video device\n");
			return 1;
		}
		for (AVCaptureDeviceFormat *f in device.formats) {
			printf("camera_avtest:   format %s\n", f.description.UTF8String);
		}
		printf("camera_avtest: the default device's active format: %s\n", device.activeFormat.description.UTF8String);
		return 0;
	}
}
