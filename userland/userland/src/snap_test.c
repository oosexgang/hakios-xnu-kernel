/*
 * snap_test: what the pictures SpringBoard keeps of applications look like -- their launch images
 * and their snapshots (Library/SplashBoard/Snapshots), as files.
 *
 * usage: snap_test DIRECTORY...
 * Every JPEG, PNG or other picture ImageIO can read under the directories is decoded and drawn
 * into 8 x 8 pixels; the line it gets says its size, its average colour and opacity, and how
 * many of those 64 pixels are near black. (A launch image of a light application that comes out
 * black was the question: the zoom that opens an application showed black at its first launch.)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <sys/stat.h>
#include <fts.h>
#include <stdio.h>
#include <string.h>

static void
look(const char *path)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CGImageSourceRef source = url != NULL ? CGImageSourceCreateWithURL(url, NULL) : NULL;
	CGImageRef image = source != NULL && CGImageSourceGetCount(source) > 0 ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
	uint8_t pixels[8 * 8 * 4];
	unsigned long sum[4] = { 0, 0, 0, 0 };
	unsigned dark = 0;

	if (image == NULL) {
		printf("snap_test: %s: NOT READ\n", path);
	} else {
		CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
		CGContextRef context = CGBitmapContextCreate(pixels, 8, 8, 8, 32, space, kCGImageAlphaPremultipliedLast);

		memset(pixels, 0, sizeof(pixels));
		CGContextDrawImage(context, CGRectMake(0, 0, 8, 8), image);
		for (unsigned i = 0; i < 64; i++) {
			for (unsigned c = 0; c < 4; c++) {
				sum[c] += pixels[4 * i + c];
			}
			dark += pixels[4 * i] < 16 && pixels[4 * i + 1] < 16 && pixels[4 * i + 2] < 16;
		}
		printf("snap_test: %s: %zu x %zu, average %lu %lu %lu opacity %lu, %u of 64 near black\n", path, CGImageGetWidth(image),
		    CGImageGetHeight(image), sum[0] / 64, sum[1] / 64, sum[2] / 64, sum[3] / 64, dark);
		CGContextRelease(context);
		CGColorSpaceRelease(space);
		CGImageRelease(image);
	}
	if (source != NULL) {
		CFRelease(source);
	}
	if (url != NULL) {
		CFRelease(url);
	}
}

int
main(int argc, char **argv)
{
	FTS *walk;
	FTSENT *entry;

	if (argc < 2) {
		fprintf(stderr, "usage: snap_test DIRECTORY...\n");
		return 2;
	}
	walk = fts_open(argv + 1, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	while (walk != NULL && (entry = fts_read(walk)) != NULL) {
		const char *dot = strrchr(entry->fts_name, '.');

		if (entry->fts_info == FTS_F && dot != NULL && (strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".jpg") == 0 ||
		    strcmp(dot, ".png") == 0 || strcmp(dot, ".ktx") == 0 || strcmp(dot, ".atx") == 0)) {
			look(entry->fts_path);
		} else if (entry->fts_info == FTS_F) {
			printf("snap_test: %s: %lld bytes, not a picture by its name\n", entry->fts_path, (long long)entry->fts_statp->st_size);
		}
	}
	if (walk != NULL) {
		fts_close(walk);
	}
	return 0;
}
