/*
 * font_test: whether text in other scripts than Latin gets drawn: for each sample, the font CoreText
 * falls back to from the system font (CTFontCreateForString), and how many pixels drawing the sample
 * with that fallback (a CTLine, as UIKit and WebKit draw text) paints into a bitmap, how many of them
 * in colour. A sample with no font of its own falls back to LastResort or draws nothing; the colour
 * emoji come out in colour.
 *
 * usage: font_test [TEXT...]      (default: emoji, Chinese, Japanese, Korean, Latin)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH   900
#define HEIGHT  160
#define POINTS  51.0    /* 17 points at three pixels a point */

static void
sample(CTFontRef system, const char *text)
{
	CFStringRef string = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
	CTFontRef fallback = CTFontCreateForString(system, string, CFRangeMake(0, CFStringGetLength(string)));
	CFStringRef name = CTFontCopyPostScriptName(fallback);
	char font[128] = "?";
	const void *keys[] = { kCTFontAttributeName };
	const void *values[] = { system };
	CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef attributed = CFAttributedStringCreate(NULL, string, attributes);
	CTLineRef line = CTLineCreateWithAttributedString(attributed);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	uint8_t *pixels = calloc(WIDTH * HEIGHT, 4);
	CGContextRef context = CGBitmapContextCreate(pixels, WIDTH, HEIGHT, 8, WIDTH * 4, rgb,
	    kCGImageAlphaPremultipliedLast);
	unsigned painted = 0, coloured = 0;
	CFArrayRef runs = CTLineGetGlyphRuns(line);
	char used[256] = "";

	CFStringGetCString(name, font, sizeof(font), kCFStringEncodingUTF8);
	/* which fonts the line itself drew with, after its own fallback */
	for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
		CTFontRef run_font = CFDictionaryGetValue(CTRunGetAttributes(CFArrayGetValueAtIndex(runs, i)), kCTFontAttributeName);
		CFStringRef run_name = run_font ? CTFontCopyPostScriptName(run_font) : NULL;
		char buffer[96] = "?";

		if (run_name != NULL) {
			CFStringGetCString(run_name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
			CFRelease(run_name);
		}
		if (strlen(used) + strlen(buffer) + 3 < sizeof(used)) {
			strcat(used, i ? ", " : "");
			strcat(used, buffer);
		}
	}
	CGContextSetRGBFillColor(context, 0, 0, 0, 1);
	CGContextSetTextPosition(context, 10, 50);
	CTLineDraw(line, context);
	CGContextFlush(context);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		uint8_t *p = pixels + 4 * i;
		int high = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
		int low = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);

		if (p[3] > 32) {
			painted++;
			coloured += high - low > 40;
		}
	}
	printf("font_test: \"%s\": the system font falls back to %s; drawn with %s: %u pixels painted, %u of them in colour\n",
	    text, font, used, painted, coloured);
	CGContextRelease(context);
	free(pixels);
	CGColorSpaceRelease(rgb);
	CFRelease(line);
	CFRelease(attributed);
	CFRelease(attributes);
	CFRelease(name);
	CFRelease(fallback);
	CFRelease(string);
}

int
main(int argc, char **argv)
{
	static const char *const samples[] = { "\xF0\x9F\x98\x80\xF0\x9F\x91\x8D\xE2\x9D\xA4\xEF\xB8\x8F", /* grinning face, thumbs up, heart */
		"\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97\xE4\xBD\x93",                                       /* Chinese: zhong wen zi ti */
		"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",                                                   /* Japanese: nihongo */
		"\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4",                                                   /* Korean: hangugeo */
		"Hello" };
	CTFontRef system = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, POINTS, NULL);

	setvbuf(stdout, NULL, _IONBF, 0);
	if (system == NULL) {
		printf("font_test: no system font\n");
		return 1;
	}
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			sample(system, argv[i]);
		}
	} else {
		for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
			sample(system, samples[i]);
		}
	}
	CFRelease(system);
	return 0;
}
