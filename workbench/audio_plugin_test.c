/*
 * Host tests of the audio server's plug-in (components/audio/audio_plugin.c), the file itself included here: what
 * it is given of CoreFoundation, CoreAudio and the Mach clock is this file's (just enough of each), and so are its
 * open(), ioctl() and write(), which reach no device; the HAL's calls are made through the plug-in's own table.
 * Darwin's headers are not on a Linux host: workbench/run.sh builds this against empty ones of the same names
 * (CoreFoundation/CoreFoundation.h, CoreFoundation/CFPlugInCOM.h, CoreAudioTypes/CoreAudioTypes.h, mach/mach_time.h,
 * sys/ioccom.h) in a directory of its own, with -Wno-multichar for the HAL's four-character constants.
 *
 *   audio_plugin_test               the scenarios' names
 *   audio_plugin_test SCENARIO      one scenario a process
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>                  /* (_IOW, for sys/board/audio.h: Darwin's sys/ioccom.h has it) */
#include <sys/types.h>
#include <unistd.h>

/* ---- what the plug-in needs of Darwin's headers ------------------------------------------------ */

#define __unused        __attribute__((unused))

typedef uint8_t UInt8;
typedef uint32_t UInt32;
typedef int32_t SInt32;
typedef uint64_t UInt64;
typedef float Float32;
typedef double Float64;
typedef unsigned char Boolean;
typedef SInt32 OSStatus;
typedef SInt32 HRESULT;
typedef UInt32 ULONG;
typedef void *LPVOID;
typedef const void *CFTypeRef;
typedef const struct test_cfstring *CFStringRef;       /* the characters themselves, NUL-terminated */
typedef const void *CFDictionaryRef, *CFAllocatorRef, *CFPropertyListRef;
typedef struct {
	UInt8 bytes[16];
} CFUUIDBytes;
typedef CFUUIDBytes REFIID;
typedef const CFUUIDBytes *CFUUIDRef;
#define S_OK                    0
#define E_NOINTERFACE           ((HRESULT)0x80000004)
#define kCFStringEncodingUTF8   0x08000100
#define CFSTR(s)                ((CFStringRef)(s))

typedef struct {
	Float64 mMinimum, mMaximum;
} AudioValueRange;
typedef struct {
	Float64 mSampleRate;
	UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel,
	    mReserved;
} AudioStreamBasicDescription;
typedef struct {
	Float64 mSampleTime;
	UInt64 mHostTime;
	Float64 mRateScalar;
	UInt64 mWordClockTime;
	UInt8 mSMPTETime[24];
	UInt32 mFlags, mReserved;
} AudioTimeStamp;
#define kAudioFormatLinearPCM                   0x6c70636d      /* 'lpcm' */
#define kAudioFormatFlagIsSignedInteger         (1u << 2)
#define kAudioFormatFlagIsPacked                (1u << 3)

/* UUIDs: each one asked for kept here, so that CFEqual() can tell them from strings */
static CFUUIDBytes uuids[16];
static unsigned uuid_count;

static CFUUIDRef
uuid_of(const CFUUIDBytes *bytes)
{
	for (unsigned i = 0; i < uuid_count; i++) {
		if (memcmp(&uuids[i], bytes, sizeof(*bytes)) == 0) {
			return &uuids[i];
		}
	}
	if (uuid_count == sizeof(uuids) / sizeof(uuids[0])) {
		abort();
	}
	uuids[uuid_count] = *bytes;
	return &uuids[uuid_count++];
}

static CFUUIDRef
CFUUIDGetConstantUUIDWithBytes(CFAllocatorRef allocator, ...)
{
	CFUUIDBytes b;
	va_list ap;

	(void)allocator;
	va_start(ap, allocator);
	for (int i = 0; i < 16; i++) {
		b.bytes[i] = (UInt8)va_arg(ap, int);
	}
	va_end(ap);
	return uuid_of(&b);
}

static CFUUIDRef
CFUUIDCreateFromUUIDBytes(CFAllocatorRef allocator, CFUUIDBytes bytes)
{
	(void)allocator;
	return uuid_of(&bytes);
}

#define IUnknownUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46)

static Boolean
CFEqual(CFTypeRef a, CFTypeRef b)
{
	bool a_uuid = (const CFUUIDBytes *)a >= uuids && (const CFUUIDBytes *)a < uuids + 16;
	bool b_uuid = (const CFUUIDBytes *)b >= uuids && (const CFUUIDBytes *)b < uuids + 16;

	return a_uuid || b_uuid ? a == b : strcmp(a, b) == 0;
}

static int retained;            /* CFRetain()s less CFRelease()s of strings */

static CFTypeRef
CFRetain(CFTypeRef object)
{
	retained++;
	return object;
}

static void
CFRelease(CFTypeRef object)
{
	bool uuid = (const CFUUIDBytes *)object >= uuids && (const CFUUIDBytes *)object < uuids + 16;

	retained -= !uuid;
}

static CFStringRef
CFStringCreateWithCString(CFAllocatorRef allocator, const char *text, UInt32 encoding)
{
	(void)allocator;
	(void)encoding;
	return (CFStringRef)strdup(text);
}

static const char *
CFStringGetCStringPtr(CFStringRef string, UInt32 encoding)
{
	(void)encoding;
	return (const char *)string;
}

/* the Mach clock: Apple's arm64 timebase (24 MHz: 125/3 ns a tick), at whatever the scenario says */
typedef struct {
	uint32_t numer, denom;
} mach_timebase_info_data_t;
static UInt64 now_ticks = 1000000;

static int
mach_timebase_info(mach_timebase_info_data_t *info)
{
	info->numer = 125;
	info->denom = 3;
	return 0;
}

static UInt64
mach_absolute_time(void)
{
	return now_ticks;
}

/* ---- the devices it opens: none; what it gives the kernel's audio device by ioctl, kept ------------ */

#define FAKE_SINK       1000    /* the file descriptor /dev/hakios-audio is given as */
static int ioctls;
static uint32_t ioctl_below, ioctl_muted;
static char said_text[16384];

static int
test_open(const char *path, int flags, ...)
{
	(void)flags;
	if (strcmp(path, "/dev/hakios-audio") == 0) {
		return FAKE_SINK;
	}
	errno = ENOENT;
	return -1;
}

static int
test_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	const uint32_t *v;

	va_start(ap, request);
	v = va_arg(ap, const uint32_t *);
	va_end(ap);
	if (fd != FAKE_SINK) {
		errno = EBADF;
		return -1;
	}
	ioctls++;
	ioctl_below = v[0];
	ioctl_muted = v[1];
	/* (the kernel's own check: drivers/board/audio.c) */
	return v[0] > 120 * 8 ? (errno = EINVAL, -1) : 0;
}

static ssize_t
test_write(int fd, const void *data, size_t size)
{
	if (fd == 2) {
		size_t have = strlen(said_text);

		if (have + size < sizeof(said_text)) {
			memcpy(said_text + have, data, size);
			said_text[have + size] = 0;
		}
		return write(2, data, size);
	}
	return fd == FAKE_SINK ? (ssize_t)size : (errno = EBADF, -1);
}

#define open            test_open
#define ioctl           test_ioctl
#define write           test_write
#include "../components/audio/audio_plugin.c"
#undef open
#undef ioctl
#undef write

/* ---- the HAL's side ----------------------------------------------------------------------------- */

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

static int changes;             /* the PropertiesChanged() the plug-in told */

static OSStatus
properties_changed(AudioServerPlugInHostRef the_host, AudioObjectID object, UInt32 count,
    const AudioObjectPropertyAddress *addresses)
{
	(void)the_host;
	(void)object;
	(void)addresses;
	changes += (int)count;
	return 0;
}

static const AudioServerPlugInHostInterface host_interface = { .PropertiesChanged = properties_changed };

/* the plug-in as the HAL has it: the factory's answer, and Initialize() called */
static AudioServerPlugInDriverRef
plugin(void)
{
	AudioServerPlugInDriverRef d = HakiosAudio_Create(NULL, kAudioServerPlugInTypeUUID);

	if (d == NULL || (*d)->Initialize(d, &host_interface) != 0) {
		printf("FAIL: the plug-in does not start\n");
		exit(1);
	}
	return d;
}

static OSStatus
set_float(AudioServerPlugInDriverRef d, UInt32 selector, Float32 value)
{
	AudioObjectPropertyAddress a = { selector, kAudioObjectPropertyScopeGlobal, 0 };

	return (*d)->SetPropertyData(d, OBJECT_SPEAKER_VOLUME, 0, &a, 0, NULL, sizeof(value), &value);
}

static Float32
get_float(AudioServerPlugInDriverRef d, UInt32 selector, Float32 in)
{
	AudioObjectPropertyAddress a = { selector, kAudioObjectPropertyScopeGlobal, 0 };
	Float32 value = in;
	UInt32 size = 0;

	if ((*d)->GetPropertyData(d, OBJECT_SPEAKER_VOLUME, 0, &a, 0, NULL, sizeof(value), &size, &value) != 0 ||
	    size != sizeof(value)) {
		return -1000;
	}
	return value;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/*
 * The speaker's volume: what is set reaches the kernel in eighths of a dB under the loudest, 48 dB from top to
 * bottom, 0 as silence. A value that is not a number -- as a scalar, in decibels, or to be converted -- is
 * silence, not the loudest the kernel allows, and what is read back is then a number.
 */
static void
scenario_volume(void)
{
	AudioServerPlugInDriverRef d = plugin();
	Float32 v;

	CHECK(set_float(d, kAudioLevelControlPropertyScalarValue, 0.5f) == 0, "scalar 0.5");
	CHECK(ioctls == 1 && ioctl_below == 192 && !ioctl_muted, "0.5: %d ioctls, below %u, muted %u", ioctls, ioctl_below,
	    ioctl_muted);
	CHECK(set_float(d, kAudioLevelControlPropertyDecibelValue, -100) == 0, "-100 dB");
	CHECK(ioctl_below == 384 && ioctl_muted, "-100 dB: below %u, muted %u", ioctl_below, ioctl_muted);
	CHECK(set_float(d, kAudioLevelControlPropertyDecibelValue, INFINITY) == 0, "+inf dB");
	CHECK(ioctl_below == 0 && !ioctl_muted, "+inf dB: below %u, muted %u", ioctl_below, ioctl_muted);

	CHECK(set_float(d, kAudioLevelControlPropertyScalarValue, NAN) == 0, "scalar NaN");
	CHECK(ioctl_muted && ioctl_below == 384, "scalar NaN: below %u, muted %u (the loudest is below 0, not muted)",
	    ioctl_below, ioctl_muted);
	v = get_float(d, kAudioLevelControlPropertyScalarValue, 0);
	CHECK(v == 0, "scalar NaN: read back as %f", v);
	v = get_float(d, kAudioLevelControlPropertyDecibelValue, 0);
	CHECK(v == -48, "scalar NaN: read back as %f dB", v);

	CHECK(set_float(d, kAudioLevelControlPropertyScalarValue, 1) == 0, "scalar 1");
	CHECK(ioctl_below == 0 && !ioctl_muted, "1: below %u, muted %u", ioctl_below, ioctl_muted);
	CHECK(set_float(d, kAudioLevelControlPropertyDecibelValue, NAN) == 0, "NaN dB");
	CHECK(ioctl_muted && ioctl_below == 384, "NaN dB: below %u, muted %u", ioctl_below, ioctl_muted);
	v = get_float(d, kAudioLevelControlPropertyScalarValue, 0);
	CHECK(v == 0, "NaN dB: read back as %f", v);

	v = get_float(d, kAudioLevelControlPropertyConvertScalarToDecibels, NAN);
	CHECK(v == -48, "NaN converted to %f dB", v);
	v = get_float(d, kAudioLevelControlPropertyConvertDecibelsToScalar, NAN);
	CHECK(v == 0, "NaN dB converted to %f", v);
	v = get_float(d, kAudioLevelControlPropertyConvertDecibelsToScalar, -24);
	CHECK(v == 0.5f, "-24 dB converted to %f", v);
	CHECK(changes == 2 * 6, "%d properties said changed", changes);
}

static unsigned
times_said(const char *what)
{
	unsigned n = 0;

	for (const char *p = said_text; (p = strstr(p, what)) != NULL; p += strlen(what)) {
		n++;
	}
	return n;
}

/*
 * The speaker plays; the actuator (the haptics, a device of its own with an output of its own) starts and stops in
 * the middle of it. What the speaker has played is still counted from its own start, and the line at its stop says
 * so; only the speaker is said to play.
 */
static void
scenario_speaker_and_actuator(void)
{
	AudioServerPlugInDriverRef d = plugin();
	static int16_t samples[2 * 4800];

	CHECK((*d)->StartIO(d, OBJECT_SPEAKER, 1) == 0, "the speaker starts");
	for (int i = 0; i < 10; i++) {
		CHECK((*d)->DoIOOperation(d, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, 1, kAudioServerPlugInIOOperationWriteMix, 4800, NULL,
		    samples, NULL) == 0, "the speaker's mix");
	}
	CHECK((*d)->StartIO(d, OBJECT_ACTUATOR, 2) == 0, "the actuator starts");
	CHECK((*d)->DoIOOperation(d, OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT, 2, kAudioServerPlugInIOOperationWriteMix, 4800, NULL,
	    samples, NULL) == 0, "the actuator's mix");
	CHECK((*d)->StopIO(d, OBJECT_ACTUATOR, 2) == 0, "the actuator stops");
	for (int i = 0; i < 10; i++) {
		CHECK((*d)->DoIOOperation(d, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, 1, kAudioServerPlugInIOOperationWriteMix, 4800, NULL,
		    samples, NULL) == 0, "the speaker's mix");
	}
	CHECK(written == 20 * 4800 && refused == 0, "the speaker: %llu frames written, %llu refused", (unsigned long long)written,
	    (unsigned long long)refused);
	CHECK((*d)->StopIO(d, OBJECT_SPEAKER, 1) == 0, "the speaker stops");
	CHECK(strstr(said_text, "stopped after 2.00 s (0 frames") != NULL, "the speaker's line at its stop");
	CHECK(times_said("hakios: audio: playing\n") == 1, "said to play %u times", times_said("hakios: audio: playing\n"));
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "volume", scenario_volume },
		{ "speaker_and_actuator", scenario_speaker_and_actuator },
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
			printf("audio_plugin_test %s: %d checks, %d failed\n", argv[1], checks, failures);
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
