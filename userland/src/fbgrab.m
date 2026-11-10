/*
 * fbgrab -log NAME [WIDTH [QUALITY]]    the picture on the panel, scaled to WIDTH pixels (default 375: a point a pixel) as a JPEG
 * fbgrab HOST PORT NAME                 ... or whole, as a PNG, sent to HOST:PORT (tools/fbrecv.py) as POST /NAME.png
 *
 * The kernel's side is sysctl kern.redwood.fbgrab (bsd/dev/redwood_fbgrab.c): 16 bytes -- width, height, stride, 'BGRA' -- and the rows.
 * fbshot's pictures go through the log too but are 60 to 130 pixels wide, a blur. The JPEG goes to the console in base64 lines
 *     fbjpg NAME I/N: ...
 * and the last 261 KB of the console come back with the run, so a picture of 60 to 100 KB is the last thing a run says; tools/fbjpg.py
 * puts it together. (A network way needs the Mac on the phone's Wi-Fi: not so, run 759.)
 */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

static CGImageRef
panel(uint8_t **keep)
{
	size_t size = 16 + 3000 * 3200 * 4;
	uint8_t *buffer = malloc(size);

	*keep = buffer;
	if (buffer == NULL || sysctlbyname("kern.redwood.fbgrab", buffer, &size, NULL, 0) != 0) {
		printf("fbgrab: the picture was not given (errno %d)\n", errno);
		return NULL;
	}
	uint32_t width = ((uint32_t *)buffer)[0], height = ((uint32_t *)buffer)[1], stride = ((uint32_t *)buffer)[2];

	printf("fbgrab: the panel %ux%u\n", width, height);
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = CGBitmapContextCreate(buffer + 16, width, height, 8, stride, space, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

	return CGBitmapContextCreateImage(context);
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		uint8_t *keep;
		CGImageRef image = panel(&keep);

		if (image == NULL || argc < 3) {
			printf("usage: fbgrab -log NAME [WIDTH [QUALITY]] | fbgrab HOST PORT NAME\n");
			return 2;
		}
		NSMutableData *data = [NSMutableData data];

		if (strcmp(argv[1], "-log") == 0) {
			size_t width = argc > 3 ? (size_t)atoi(argv[3]) : 375, from = CGImageGetWidth(image);
			double quality = argc > 4 ? atof(argv[4]) : 0.6;
			size_t height = CGImageGetHeight(image) * width / from;
			CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
			CGContextRef small = CGBitmapContextCreate(NULL, width, height, 8, 0, space, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

			CGContextSetInterpolationQuality(small, kCGInterpolationHigh);
			CGContextDrawImage(small, CGRectMake(0, 0, width, height), image);
			CGImageRef scaled = CGBitmapContextCreateImage(small);
			CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.jpeg"), 1, NULL);
			NSDictionary *options = @{ (__bridge NSString *)kCGImageDestinationLossyCompressionQuality : @(quality) };

			CGImageDestinationAddImage(destination, scaled, (__bridge CFDictionaryRef)options);
			if (!CGImageDestinationFinalize(destination)) {
				printf("fbgrab: no JPEG\n");
				return 1;
			}
			NSString *text = [data base64EncodedStringWithOptions:0];
			size_t lines = (text.length + 199) / 200;

			printf("fbgrab: %zux%zu, JPEG %lu bytes, %zu lines\n", width, height, (unsigned long)data.length, lines);
			for (size_t i = 0; i < lines; i++) {
				NSString *line = [NSString stringWithFormat:@"fbjpg %s %zu/%zu: %@\n", argv[2], i + 1, lines, [text substringWithRange:NSMakeRange(i * 200, MIN((size_t)200, text.length - i * 200))]];

				sysctlbyname("kern.redwood.say", NULL, NULL, line.UTF8String, line.length - 1);       /* the kernel's log: the console's own is a tty that drops lines (run 759: 20 of 128 came back) */
				usleep(2000);
			}
			return 0;
		}
		if (argc < 4) {
			return 2;
		}
		CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.png"), 1, NULL);

		CGImageDestinationAddImage(destination, image, NULL);
		if (!CGImageDestinationFinalize(destination)) {
			printf("fbgrab: no PNG\n");
			return 1;
		}
		struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])) };
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		inet_pton(AF_INET, argv[1], &to.sin_addr);
		if (connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0) {
			printf("fbgrab: cannot connect to %s:%s (errno %d)\n", argv[1], argv[2], errno);
			return 1;
		}
		char head[200];
		int n = snprintf(head, sizeof(head), "POST /%s.png HTTP/1.0\r\nContent-Length: %lu\r\n\r\n", argv[3], (unsigned long)data.length);

		write(fd, head, (size_t)n);
		write(fd, data.bytes, data.length);
		close(fd);
	}
	return 0;
}
