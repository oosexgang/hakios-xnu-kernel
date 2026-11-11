/*
 * record_test: record from the microphone the way an application does, and say what came.
 *
 * usage: record_test SECONDS [vibrate AT]
 *
 * An audio session for recording (AVAudioSession: play and record, haptics allowed while recording),
 * then AVAudioRecorder into a WAV file of 16-bit samples at 48 kHz, one channel, in /tmp (in memory
 * on this machine), its meters read every tenth of a second. With "vibrate AT", AT seconds into the
 * recording the phone's motor runs for one second (CoreHaptics, a continuous event at full strength):
 * something the microphone hears that nobody needs to say, at a frequency one can look for, the
 * motor's resonance (175.5 Hz on the POCO X5 Pro). Then the file is read back: how loud each tenth
 * of a second is, and how strong 175.5 Hz is before the vibration and while it lasts.
 *
 * On this machine the samples come from the kernel's /dev/hakios-mic (boot-arg mic=4), through
 * HakiosAudio.driver's device "Codec" (audio_plugin.c), of which the virtual audio device makes
 * the built-in microphone.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH            "/tmp/record_test.wav"
#define RATE            48000
#define MOTOR_HZ        175.5

static id
send(id object, const char *selector)
{
	return object ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector)) : NULL;
}

static const char *
text(id object)
{
	id description = object ? send(object, "description") : NULL;
	const char *utf8 = description ? ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String")) : NULL;

	return utf8 ? utf8 : "(none)";
}

static unsigned long
count_of(id array)
{
	return array ? ((unsigned long (*)(id, SEL))objc_msgSend)(array, sel_registerName("count")) : 0;
}

static id
item(id array, unsigned long i)
{
	return ((id (*)(id, SEL, unsigned long))objc_msgSend)(array, sel_registerName("objectAtIndex:"), i);
}

/* a constant (NSString *) of a framework, by name */
static id
constant(const char *framework_path, const char *name)
{
	void *framework = dlopen(framework_path, RTLD_NOW);
	id *value = framework ? dlsym(framework, name) : NULL;

	return value ? *value : NULL;
}

#define AVFAUDIO        "/System/Library/Frameworks/AVFAudio.framework/AVFAudio"
#define COREHAPTICS     "/System/Library/Frameworks/CoreHaptics.framework/CoreHaptics"

static void
ports(const char *what, id list)
{
	unsigned long n = count_of(list);

	printf("record_test: %s: %lu\n", what, n);
	for (unsigned long i = 0; i < n; i++) {
		id port = item(list, i);

		printf("record_test:   %s \"%s\" (%s)\n", text(send(port, "portType")), text(send(port, "portName")),
		    text(send(port, "UID")));
	}
}

/* The audio session, set up for recording; false if it will not be. */
static bool
session_up(void)
{
	id session = send((id)objc_getClass("AVAudioSession"), "sharedInstance"), error = NULL;
	id category = constant(AVFAUDIO, "AVAudioSessionCategoryPlayAndRecord");
	BOOL ok;

	if (session == NULL || category == NULL) {
		printf("record_test: no AVAudioSession\n");
		return false;
	}
	if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(session, sel_registerName("respondsToSelector:"),
	    sel_registerName("recordPermission"))) {
		unsigned long permission = ((unsigned long (*)(id, SEL))objc_msgSend)(session, sel_registerName("recordPermission"));

		printf("record_test: the permission to record: %c%c%c%c\n", (int)(permission >> 24 & 0xff), (int)(permission >> 16 & 0xff),
		    (int)(permission >> 8 & 0xff), (int)(permission & 0xff));
	}
	ok = ((BOOL (*)(id, SEL, id, id *))objc_msgSend)(session, sel_registerName("setCategory:error:"), category, &error);
	printf("record_test: the category play-and-record: %s%s\n", ok ? "set" : "NOT SET: ", ok ? "" : text(error));
	if (!ok) {
		return false;
	}
	ok = ((BOOL (*)(id, SEL, BOOL, id *))objc_msgSend)(session, sel_registerName("setAllowHapticsAndSystemSoundsDuringRecording:error:"),
	    YES, &error);
	printf("record_test: haptics while recording: %s%s\n", ok ? "allowed" : "NOT ALLOWED: ", ok ? "" : text(error));
	ok = ((BOOL (*)(id, SEL, BOOL, id *))objc_msgSend)(session, sel_registerName("setActive:error:"), YES, &error);
	printf("record_test: the session: %s%s\n", ok ? "active" : "NOT ACTIVE: ", ok ? "" : text(error));
	if (!ok) {
		return false;
	}
	printf("record_test: input available: %s\n", ((BOOL (*)(id, SEL))objc_msgSend)(session,
	    sel_registerName("isInputAvailable")) ? "yes" : "NO");
	ports("available inputs", send(session, "availableInputs"));
	ports("the route's inputs", send(send(session, "currentRoute"), "inputs"));
	ports("the route's outputs", send(send(session, "currentRoute"), "outputs"));
	return true;
}

/* one second of the motor at full strength, started without waiting for it */
static id
vibrate(void)
{
	id error = NULL, engine, pattern, player, event;
	const void *parameters[2], *events[1];
	CFArrayRef list, none = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);

	if (constant(COREHAPTICS, "CHHapticEventTypeHapticContinuous") == NULL) {
		printf("record_test: no CoreHaptics\n");
		return NULL;
	}
	engine = ((id (*)(id, SEL, id *))objc_msgSend)(send((id)objc_getClass("CHHapticEngine"), "alloc"),
	    sel_registerName("initAndReturnError:"), &error);
	if (engine == NULL || !((BOOL (*)(id, SEL, id *))objc_msgSend)(engine, sel_registerName("startAndReturnError:"), &error)) {
		printf("record_test: the haptic engine does not start: %s\n", text(error));
		return NULL;
	}
	parameters[0] = ((id (*)(id, SEL, id, float))objc_msgSend)(send((id)objc_getClass("CHHapticEventParameter"), "alloc"),
	    sel_registerName("initWithParameterID:value:"), constant(COREHAPTICS, "CHHapticEventParameterIDHapticIntensity"), 1.0f);
	parameters[1] = ((id (*)(id, SEL, id, float))objc_msgSend)(send((id)objc_getClass("CHHapticEventParameter"), "alloc"),
	    sel_registerName("initWithParameterID:value:"), constant(COREHAPTICS, "CHHapticEventParameterIDHapticSharpness"), 0.3f);
	list = CFArrayCreate(NULL, parameters, 2, &kCFTypeArrayCallBacks);
	event = ((id (*)(id, SEL, id, id, double, double))objc_msgSend)(send((id)objc_getClass("CHHapticEvent"), "alloc"),
	    sel_registerName("initWithEventType:parameters:relativeTime:duration:"),
	    constant(COREHAPTICS, "CHHapticEventTypeHapticContinuous"), (id)list, 0.0, 1.0);
	events[0] = event;
	list = CFArrayCreate(NULL, events, 1, &kCFTypeArrayCallBacks);
	pattern = ((id (*)(id, SEL, id, id, id *))objc_msgSend)(send((id)objc_getClass("CHHapticPattern"), "alloc"),
	    sel_registerName("initWithEvents:parameters:error:"), (id)list, (id)none, &error);
	player = pattern ? ((id (*)(id, SEL, id, id *))objc_msgSend)(engine, sel_registerName("createPlayerWithPattern:error:"),
	    pattern, &error) : NULL;
	if (player == NULL ||
	    !((BOOL (*)(id, SEL, double, id *))objc_msgSend)(player, sel_registerName("startAtTime:error:"), 0.0, &error)) {
		printf("record_test: the vibration does not play: %s\n", text(error));
		return NULL;
	}
	printf("record_test: the motor runs for 1 s\n");
	return engine;
}

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* The power of one frequency in samples [from, to): Goertzel, as dB of full scale (a full-scale sine is 0). */
static double
tone_db(const int16_t *samples, long from, long to, double hz)
{
	double w = 2 * M_PI * hz / RATE, c = 2 * cos(w), s1 = 0, s2 = 0, power;
	long n = to - from;

	if (n <= 0) {
		return -200;
	}
	for (long i = from; i < to; i++) {
		double window = 0.5 - 0.5 * cos(2 * M_PI * (i - from) / (n - 1)), s0 = samples[i] / 32768.0 * window + c * s1 - s2;

		s2 = s1;
		s1 = s0;
	}
	power = s1 * s1 + s2 * s2 - c * s1 * s2;
	/* (a Hann window halves the amplitude: a full-scale sine gives (n/4)^2) */
	return 10 * log10(power / ((n / 4.0) * (n / 4.0)) + 1e-20);
}

static double
rms_db(const int16_t *samples, long from, long to)
{
	double sum = 0;

	for (long i = from; i < to; i++) {
		sum += (double)samples[i] * samples[i];
	}
	return to > from ? 10 * log10(sum / (to - from) / (32768.0 * 32768.0) + 1e-20) : -200;
}

/* The file read back: the samples of its data chunk. */
static int
analyse(double seconds, double vibrate_at)
{
	FILE *file = fopen(PATH, "rb");
	unsigned char header[12], chunk[8];
	int16_t *samples = NULL;
	long count = 0, peak = 0;

	if (file == NULL || fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
		printf("record_test: %s is not a WAV file\n", PATH);
		return 1;
	}
	while (fread(chunk, 1, 8, file) == 8) {
		uint32_t size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (uint32_t)chunk[7] << 24;

		if (memcmp(chunk, "data", 4) == 0) {
			samples = malloc(size + 2);
			count = (long)fread(samples, 1, size, file) / 2;
			break;
		}
		fseek(file, (long)(size + (size & 1)), SEEK_CUR);
	}
	fclose(file);
	if (samples == NULL || count == 0) {
		printf("record_test: %s has no samples\n", PATH);
		return 1;
	}
	for (long i = 0; i < count; i++) {
		long v = samples[i] < 0 ? -(long)samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	printf("record_test: the file: %ld samples (%.2f s of %.2f), peak %ld of 32768, %.1f dBFS RMS over all\n", count,
	    (double)count / RATE, seconds, peak, rms_db(samples, 0, count));
	printf("record_test: dBFS RMS of each tenth of a second:");
	for (long at = 0; at + RATE / 10 <= count; at += RATE / 10) {
		printf(" %.0f", rms_db(samples, at, at + RATE / 10));
	}
	printf("\n");
	if (vibrate_at > 0) {
		long quiet_from = (long)(0.5 * RATE), quiet_to = (long)(vibrate_at * RATE) - RATE / 10;
		long loud_from = (long)((vibrate_at + 0.15) * RATE), loud_to = (long)((vibrate_at + 0.95) * RATE);

		loud_to = loud_to < count ? loud_to : count;
		printf("record_test: before the vibration %.1f dBFS RMS, %.1f dBFS at %.1f Hz; while it lasts %.1f dBFS RMS, %.1f dBFS at %.1f "
		    "Hz\n", rms_db(samples, quiet_from, quiet_to), tone_db(samples, quiet_from, quiet_to, MOTOR_HZ), MOTOR_HZ,
		    rms_db(samples, loud_from, loud_to), tone_db(samples, loud_from, loud_to, MOTOR_HZ), MOTOR_HZ);
	}
	free(samples);
	return 0;
}

int
main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 5, vibrate_at = 0;
	const void *keys[6], *values[6];
	CFDictionaryRef settings;
	CFURLRef url;
	id recorder, error = NULL;
	BOOL ok, vibrated = NO;
	int tenths;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 3 && strcmp(argv[2], "vibrate") == 0) {
		vibrate_at = atof(argv[3]);
	}
	seconds = seconds < 1 ? 1 : seconds > 60 ? 60 : seconds;
	if (!session_up()) {
		return 1;
	}
	keys[0] = CFSTR("AVFormatIDKey");
	values[0] = number('lpcm');
	keys[1] = CFSTR("AVSampleRateKey");
	values[1] = number(RATE);
	keys[2] = CFSTR("AVNumberOfChannelsKey");
	values[2] = number(1);
	keys[3] = CFSTR("AVLinearPCMBitDepthKey");
	values[3] = number(16);
	keys[4] = CFSTR("AVLinearPCMIsFloatKey");
	values[4] = kCFBooleanFalse;
	keys[5] = CFSTR("AVLinearPCMIsBigEndianKey");
	values[5] = kCFBooleanFalse;
	settings = CFDictionaryCreate(NULL, keys, values, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	unlink(PATH);
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)PATH, (CFIndex)strlen(PATH), false);
	recorder = ((id (*)(id, SEL, id, id, id *))objc_msgSend)(send((id)objc_getClass("AVAudioRecorder"), "alloc"),
	    sel_registerName("initWithURL:settings:error:"), (id)url, (id)settings, &error);
	if (recorder == NULL) {
		printf("record_test: no AVAudioRecorder: %s\n", text(error));
		return 1;
	}
	((void (*)(id, SEL, BOOL))objc_msgSend)(recorder, sel_registerName("setMeteringEnabled:"), YES);
	ok = ((BOOL (*)(id, SEL))objc_msgSend)(recorder, sel_registerName("prepareToRecord")) &&
	    ((BOOL (*)(id, SEL, double))objc_msgSend)(recorder, sel_registerName("recordForDuration:"), seconds);
	printf("record_test: recording %.1f s into %s%s: %s\n", seconds, PATH, vibrate_at > 0 ? ", the motor at" : "",
	    ok ? "started" : "DOES NOT START");
	if (!ok) {
		return 1;
	}
	printf("record_test: meters (average/peak dB) every half second:");
	for (tenths = 0; tenths < (int)(seconds * 10) + 5 && ((BOOL (*)(id, SEL))objc_msgSend)(recorder, sel_registerName("isRecording"));
	    tenths++) {
		if (vibrate_at > 0 && !vibrated && tenths >= (int)(vibrate_at * 10)) {
			printf("\n");
			(void)vibrate();
			vibrated = YES;
		}
		usleep(100000);
		send(recorder, "updateMeters");
		if (tenths % 5 == 4) {
			printf(" %.0f/%.0f", ((float (*)(id, SEL, unsigned long))objc_msgSend)(recorder, sel_registerName("averagePowerForChannel:"), 0),
			    ((float (*)(id, SEL, unsigned long))objc_msgSend)(recorder, sel_registerName("peakPowerForChannel:"), 0));
		}
	}
	printf("\n");
	send(recorder, "stop");
	usleep(300000);
	return analyse(seconds, vibrate_at);
}
