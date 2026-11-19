/*
 * Host tests of what the phone's recording test (components/audio/record_test.c) makes of its WAV file once the
 * recording is over: the file itself included here, with just enough of CoreFoundation and the Objective-C runtime
 * that it compiles (nothing of them is called); its file is one of this test's, and what it prints is kept.
 * Darwin's headers are not on a Linux host: workbench/run.sh builds this against empty ones of the same names
 * (CoreFoundation/CoreFoundation.h, objc/message.h, objc/runtime.h) in a directory of its own, with -Wno-multichar.
 *
 *   record_analysis_test            the scenarios' names
 *   record_analysis_test SCENARIO   one scenario a process
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- what the program needs of Darwin's headers (none of it called here) ----------------------- */

#define UNUSED          __attribute__((unused))
typedef uint8_t UInt8;
typedef long CFIndex;
typedef struct test_object *id;
typedef const struct test_selector *SEL;
typedef bool BOOL;
#define YES             true
#define NO              false
typedef const void *CFArrayRef, *CFNumberRef, *CFDictionaryRef, *CFURLRef, *CFStringRef, *CFAllocatorRef;
#define CFSTR(s)        ((CFStringRef)(s))
static const int kCFTypeArrayCallBacks UNUSED, kCFTypeDictionaryKeyCallBacks UNUSED, kCFTypeDictionaryValueCallBacks UNUSED;
static const void *const kCFBooleanFalse UNUSED = "false";
enum { kCFNumberIntType = 9 };

static void (*objc_msgSend)(void) UNUSED;     /* (a pointer, so that the casts to each method's type are taken as they are) */

static id UNUSED
objc_getClass(const char *name)
{
	(void)name;
	return NULL;
}

static SEL UNUSED
sel_registerName(const char *name)
{
	(void)name;
	return NULL;
}

static CFArrayRef UNUSED
CFArrayCreate(CFAllocatorRef allocator, const void **values, CFIndex count, const int *callbacks)
{
	(void)allocator, (void)values, (void)count, (void)callbacks;
	return NULL;
}

static CFNumberRef UNUSED
CFNumberCreate(CFAllocatorRef allocator, int type, const void *value)
{
	(void)allocator, (void)type, (void)value;
	return NULL;
}

static CFDictionaryRef UNUSED
CFDictionaryCreate(CFAllocatorRef allocator, const void **keys, const void **values, CFIndex count, const int *key_callbacks,
    const int *value_callbacks)
{
	(void)allocator, (void)keys, (void)values, (void)count, (void)key_callbacks, (void)value_callbacks;
	return NULL;
}

static CFURLRef UNUSED
CFURLCreateFromFileSystemRepresentation(CFAllocatorRef allocator, const UInt8 *path, CFIndex length, bool directory)
{
	(void)allocator, (void)path, (void)length, (void)directory;
	return NULL;
}

/* ---- its file, and what it says ----------------------------------------------------------------- */

static char wav_path[256];
static char printed[16384];

static FILE *
test_fopen(const char *path, const char *mode)
{
	return fopen(strcmp(path, "/tmp/record_test.wav") == 0 ? wav_path : path, mode);
}

static int
test_printf(const char *format, ...)
{
	size_t have = strlen(printed);
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(printed + have, sizeof(printed) - have, format, ap);
	va_end(ap);
	return n;
}

#define fopen           test_fopen
#define printf          test_printf
#define main            record_test_main
#include "../components/audio/record_test.c"
#undef fopen
#undef printf
#undef main

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* a WAV file of one channel of 16-bit samples at 48 kHz, every one of them `level' */
static void
wav_make(long frames, int16_t level)
{
	FILE *f;
	uint32_t data = (uint32_t)frames * 2;
	unsigned char header[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0\x80\xbb\0\0\0\x77\x01\0\x02\0\x10\0data";

	snprintf(wav_path, sizeof(wav_path), "/tmp/record_analysis_test.%d.wav", (int)getpid());
	f = fopen(wav_path, "wb");
	if (f == NULL) {
		perror(wav_path);
		exit(1);
	}
	header[4] = (unsigned char)(36 + data), header[5] = (unsigned char)((36 + data) >> 8);
	header[6] = (unsigned char)((36 + data) >> 16), header[7] = (unsigned char)((36 + data) >> 24);
	header[40] = (unsigned char)data, header[41] = (unsigned char)(data >> 8);
	header[42] = (unsigned char)(data >> 16), header[43] = (unsigned char)(data >> 24);
	fwrite(header, 1, sizeof(header), f);
	for (long i = 0; i < frames; i++) {
		fwrite(&level, 2, 1, f);
	}
	fclose(f);
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/*
 * A recording shorter than the vibration's time (record_test 2 vibrate 3, or one that ended early): the stretch
 * before the vibration is measured over the samples there are, not past the end of the file's data; one at -6 dBFS
 * throughout says -6.0 dBFS, and the stretch of the vibration, which is not there, says nothing (-200).
 */
static void
scenario_short_recording(void)
{
	char *line;

	wav_make(48000, 16384);
	CHECK(analyse(2, 3) == 0, "the file read back");
	unlink(wav_path);
	line = strstr(printed, "before the vibration");
	CHECK(line != NULL, "a line on the vibration");
	CHECK(line != NULL && strncmp(line, "before the vibration -6.0 dBFS RMS", 34) == 0, "%.60s", line ? line : "");
	CHECK(line != NULL && strstr(line, "while it lasts -200.0 dBFS RMS") != NULL, "%.120s", line ? line : "");
}

/* the same file, the vibration within it: both stretches measured where they are */
static void
scenario_vibration_within(void)
{
	char *line;

	wav_make(48000 * 3, 16384);
	CHECK(analyse(3, 1) == 0, "the file read back");
	unlink(wav_path);
	line = strstr(printed, "before the vibration");
	CHECK(line != NULL && strncmp(line, "before the vibration -6.0 dBFS RMS", 34) == 0, "%.60s", line ? line : "");
	CHECK(line != NULL && strstr(line, "while it lasts -6.0 dBFS RMS") != NULL, "%.120s", line ? line : "");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "short_recording", scenario_short_recording },
		{ "vibration_within", scenario_vibration_within },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("record_analysis_test %s: %d checks, %d failed\n", argv[1], checks, failures);
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
