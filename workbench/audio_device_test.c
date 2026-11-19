/*
 * /dev/hakios-audio (drivers/board/audio.c), run as a host program: what Darwin plays goes to
 * the driver that plays it, a recording keeps it with its time and never runs past its memory, a line says
 * what was played once the writer stops, and the volume Darwin sets is kept for the speakers' driver.
 *
 *   audio_device_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#define KERNEL 1                        /* (the header's declarations for the kernel's drivers) */
#include <sys/board/audio.h>

extern char log_text[];
void loader_log_putc(char c);

static void
said(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

uint64_t loader_sound[3];
uint64_t kvtophys(vm_offset_t va) { return (uint64_t)va ^ 0x8000000000000000ull; }    /* (an address the loader would be given) */
bool hakios_audio_recording_fed(void);
bool hakios_audio_recording_mic(void);
void hakios_audio_record_fed(const int16_t *frames, uint32_t count);

#pragma GCC diagnostic ignored "-Wformat"
#define printf said
#include "board/audio.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

#define NODE            "hakios-audio"

/* the driver that plays: takes what it is given, or only so much */
static volatile uint64_t sunk, sink_calls;
static volatile uint32_t sink_takes = 1u << 30;
static int16_t last_sample;

static uint32_t
sink(const int16_t *frames, uint32_t count)
{
	uint32_t taken = count < sink_takes ? count : sink_takes;

	sink_calls++;
	sunk += taken;
	if (count > 0) {
		last_sample = frames[2 * count - 1];
	}
	return taken;
}

/* `frames' frames of a tone at that level, written by a program */
static int
play(uint32_t frames, int16_t level)
{
	static int16_t buffer[2 * 48000];
	size_t done = 0;

	for (uint32_t i = 0; i < 2 * frames; i++) {
		buffer[i] = (int16_t)(i % 2 ? -level : level);
	}
	return fake_dev_pwrite(NODE, buffer, 4 * (size_t)frames, 0, &done) == 0 && done == 4 * (size_t)frames ? 0 : -1;
}

/* ---- scenarios -------------------------------------------------------------------------------------- */

/* what is written goes to the sink, a pass of 1024 frames at a time; a line says what was played once it stops */
static void
played_and_said(void)
{
	hakios_audio_init();
	hakios_audio_set_sink("a test's sink", sink);
	CHECK(logged("hakios-audio: sound goes to a test's sink"), "said: %s", log_text);
	CHECK(play(4800, 16384) == 0, "a tenth of a second written");
	CHECK(sunk == 4800 && sink_calls == 5 && last_sample == -16384, "to the sink, in passes of 1024: %llu frames in %llu calls",
	    (unsigned long long)sunk, (unsigned long long)sink_calls);
	/* (the line comes from the driver's own thread once it sees the writer gone: on a loaded machine, late) */
	EVENTUALLY(logged("hakios-audio: 0.10 s written, 0.10 s of it sound, peak 50% of full scale"), 15000, "once the writer stopped: %s",
	    log_text);
}

/* what the sink did not take is counted and said */
static void
sink_full(void)
{
	hakios_audio_init();
	hakios_audio_set_sink("a test's sink", sink);
	sink_takes = 512;
	CHECK(play(2048, 1000) == 0, "written");
	EVENTUALLY(logged("ms lost (the sink was full)"), 15000, "what did not go in is said: %s", log_text);
}

/* a write of part of a frame takes the whole frames only; silence is not sound */
static void
whole_frames(void)
{
	int16_t six[3] = { 3, 3, 3 };
	size_t done = 0;

	hakios_audio_init();
	hakios_audio_set_sink("a test's sink", sink);
	CHECK(fake_dev_pwrite(NODE, six, sizeof(six), 0, &done) == 0 && done == 4 && sunk == 1, "one frame of six bytes: %zu bytes, %llu frames",
	    done, (unsigned long long)sunk);
	EVENTUALLY(logged("0.00 s of it sound"), 15000, "a level of 3 is silence: %s", log_text);
}

/* rec_sound=1: the first second kept, each write as a block with its time, and nothing past the memory */
static void
recording(void)
{
	const uint8_t *base;
	uint32_t blocks, rate, frames = 0;
	uint64_t used;

	boot_args = "rec_sound=1";
	hakios_audio_init();
	CHECK(logged("hakios-audio: recording the first 1 s of sound") && loader_sound[2] != 0 && loader_sound[0] == kvtophys(loader_sound[2]),
	    "a recording is made, and the loader told where: %s", log_text);
	base = (const uint8_t *)(uintptr_t)loader_sound[2];
	CHECK(memcmp(base, "RWSND001", 8) == 0, "its header");
	for (int i = 0; i < 30; i++) {
		CHECK(play(2000, (int16_t)(100 * i)) == 0, "written");
	}
	memcpy(&rate, base + 8, 4);
	memcpy(&blocks, base + 16, 4);
	used = loader_sound[1];
	CHECK(rate == 48000, "at 48 kHz: %u", rate);
	for (uint64_t at = 24; at < used;) {
		uint32_t n;

		memcpy(&n, base + at + 8, 4);
		frames += n;
		at += 16 + 4ull * n;
	}
	CHECK(frames >= 48000 && frames < 48000 + 2000, "one second kept, and the write that reached it: %u frames in %u blocks", frames, blocks);
	CHECK(logged("hakios-audio: recording finished: 1 s"), "and said: %s", log_text);
	CHECK(used <= round_page(24 + 48000ull * 4 + 100 * 16), "never past its memory: %llu bytes", (unsigned long long)used);
}

/* a writer of one frame at a time: each write is a block with a header of its own, more than the memory was made
 * for; the recording stops at the memory's end, not past it */
static void
many_small_writes(void)
{
	uint64_t size;

	boot_args = "rec_sound=1";
	hakios_audio_init();
	size = round_page(24 + 48000ull * 4 + 100 * 16);
	for (int i = 0; i < 20000; i++) {
		CHECK(play(1, 500) == 0, "written");
		if (failures) {
			return;
		}
	}
	CHECK(loader_sound[1] <= size, "the recording ends inside its memory: %llu of %llu bytes", (unsigned long long)loader_sound[1],
	    (unsigned long long)size);
	CHECK(loader_sound[1] > size - 20, "and fills it: %llu of %llu", (unsigned long long)loader_sound[1], (unsigned long long)size);
}

/* rec_sound_fed=1: what the guard lets through is recorded, not what a program writes; the recording takes
 * it until its length, and from then on says it wants no more */
static void
fed_recording(void)
{
	static int16_t frames[2 * 24000];

	boot_args = "rec_sound=1 rec_sound_fed=1";
	hakios_audio_init();
	CHECK(logged("as the speakers' guard lets it through"), "said: %s", log_text);
	CHECK(hakios_audio_recording_fed() && !hakios_audio_recording_mic(), "the guard's sound wanted, the microphone's not");
	CHECK(play(4800, 1000) == 0 && loader_sound[1] == 24, "what a program writes is not recorded: %llu bytes",
	    (unsigned long long)loader_sound[1]);
	for (unsigned i = 0; i < 2 * 24000; i++) {
		frames[i] = 2000;
	}
	hakios_audio_record_fed(frames, 24000);
	CHECK(loader_sound[1] == 24 + 16 + 24000 * 4 && hakios_audio_recording_fed(), "half a second recorded: %llu bytes",
	    (unsigned long long)loader_sound[1]);
	hakios_audio_record_fed(frames, 24000);
	CHECK(!hakios_audio_recording_fed(), "the second it was made for is full: no more wanted");
}

/* rec_sound_mic=1: the microphone's sound is the recording's */
static void
mic_recording(void)
{
	boot_args = "rec_sound=1 rec_sound_mic=1";
	hakios_audio_init();
	CHECK(logged("the microphone hears (in both channels)"), "said: %s", log_text);
	CHECK(hakios_audio_recording_mic() && !hakios_audio_recording_fed(), "the microphone's sound wanted, the guard's not");
	CHECK(play(4800, 1000) == 0 && loader_sound[1] == 24, "what a program writes is not recorded");
}

/* rec_sound_fed=1 and rec_sound_mic=1 together: the guard's sound only, the two never interleaved in one recording,
 * and the log says the microphone's was left out */
static void
fed_and_mic_recording(void)
{
	boot_args = "rec_sound=1 rec_sound_fed=1 rec_sound_mic=1";
	hakios_audio_init();
	CHECK(hakios_audio_recording_fed() && !hakios_audio_recording_mic(), "the guard's sound wanted, the microphone's not");
	CHECK(logged("as the speakers' guard lets it through (not the microphone's: rec_sound_fed=1 comes first)"), "said: %s", log_text);
}

/* without a recording, neither is wanted */
static void
no_recording(void)
{
	hakios_audio_init();
	CHECK(!hakios_audio_recording_fed() && !hakios_audio_recording_mic(), "no recording: nothing wanted");
}

/* a writer that keeps writing for over a second: the rate it wrote at is said too */
static void
rate_said(void)
{
	hakios_audio_init();
	hakios_audio_set_sink("a test's sink", sink);
	for (int i = 0; i < 5; i++) {
		CHECK(play(9600, 4000) == 0, "a fifth of a second written");
		usleep(300 * 1000);             /* (under the half second that ends a stretch) */
	}
	EVENTUALLY(logged("hakios-audio: 1.00 s written at "), 15000, "the rate of a second and more of writing: %s", log_text);
}

/* a level of 8 is still silence, 9 is sound */
static void
silence_ends_at_8(void)
{
	hakios_audio_init();
	hakios_audio_set_sink("a test's sink", sink);
	CHECK(play(4800, 8) == 0, "written at 8");
	EVENTUALLY(logged("0.00 s of it sound"), 15000, "silence: %s", log_text);
	CHECK(play(4800, 9) == 0, "written at 9");
	EVENTUALLY(logged("0.10 s of it sound"), 15000, "sound: %s", log_text);
}

/* rec_sound=0 is no recording */
static void
rec_sound_zero(void)
{
	boot_args = "rec_sound=0";
	hakios_audio_init();
	CHECK(!logged("recording the first") && loader_sound[2] == 0, "none made: %s", log_text);
}

/* a recording that is full takes nothing more, and the microphone's says it wants no more */
static void
full_takes_no_more(void)
{
	static int16_t frames[2 * 24000];
	uint64_t used;

	boot_args = "rec_sound=1 rec_sound_mic=1";
	hakios_audio_init();
	hakios_audio_record_fed(frames, 24000);
	CHECK(hakios_audio_recording_mic(), "half full: more wanted");
	hakios_audio_record_fed(frames, 24000);
	used = loader_sound[1];
	CHECK(!hakios_audio_recording_mic(), "the second it was made for is full: no more wanted");
	hakios_audio_record_fed(frames, 100);
	CHECK(loader_sound[1] == used, "and nothing more is taken: %llu, %llu", (unsigned long long)loader_sound[1], (unsigned long long)used);
}

/* the volume Darwin sets is kept for the speakers' driver, with how many times it was set */
static void
volume(void)
{
	struct hakios_audio_volume v = { .below = 100, .muted = 0 };
	uint32_t below = 7, generation = 0;
	bool muted = true;

	hakios_audio_init();
	CHECK(!hakios_audio_volume(&below, &muted, &generation), "none set yet");
	CHECK(fake_dev_ioctl(NODE, HAKIOS_AUDIO_SET_VOLUME, &v) == 0, "set");
	CHECK(hakios_audio_volume(&below, &muted, &generation) && below == 100 && !muted && generation == 1, "kept: %u, %d, %u", below, muted,
	    generation);
	CHECK(logged("hakios-audio: the volume: 12.500 dB under the loudest"), "said: %s", log_text);
	v.muted = 1;
	CHECK(fake_dev_ioctl(NODE, HAKIOS_AUDIO_SET_VOLUME, &v) == 0 && hakios_audio_volume(&below, &muted, &generation) && muted &&
	    generation == 2, "muted, a second setting");
	CHECK(fake_dev_ioctl(NODE, 0x1234, &v) == ENOTTY, "nothing else is answered");
	/* a level that would wrap the sound driver's sum (its ceiling under Android's level plus this) is refused */
	v.muted = 0;
	v.below = 0xffffff10;
	CHECK(fake_dev_ioctl(NODE, HAKIOS_AUDIO_SET_VOLUME, &v) == EINVAL, "refused");
	CHECK(hakios_audio_volume(&below, &muted, &generation) && muted && generation == 2, "and the volume kept as it was");
	v.below = HAKIOS_AUDIO_BELOW_MOST;
	CHECK(fake_dev_ioctl(NODE, HAKIOS_AUDIO_SET_VOLUME, &v) == 0 && hakios_audio_volume(&below, &muted, &generation) &&
	    below == HAKIOS_AUDIO_BELOW_MOST && generation == 3, "the most is taken");
}

/* the same level set again and again is said once: any process may open the node, and a loop of settings filled the log */
static void
same_volume_said_once(void)
{
	struct hakios_audio_volume v = { .below = 100, .muted = 0 };
	uint32_t below = 0, generation = 0, said = 0;
	bool muted = true;

	hakios_audio_init();
	for (unsigned i = 0; i < 1000; i++) {
		CHECK(fake_dev_ioctl(NODE, HAKIOS_AUDIO_SET_VOLUME, &v) == 0, "set");
	}
	for (const char *at = log_text; (at = strstr(at, "hakios-audio: the volume:")) != NULL; at++) {
		said++;
	}
	CHECK(said == 1, "said %u times", said);
	CHECK(hakios_audio_volume(&below, &muted, &generation) && below == 100 && generation == 1000, "each setting counted: %u",
	    generation);
	v.muted = 1;
	CHECK(fake_dev_ioctl(NODE, HAKIOS_AUDIO_SET_VOLUME, &v) == 0 && logged("muted; 12.500 dB"), "a change is said: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "played_and_said", played_and_said }, { "sink_full", sink_full }, { "whole_frames", whole_frames },
		{ "recording", recording }, { "many_small_writes", many_small_writes }, { "volume", volume }, { "same_volume_said_once", same_volume_said_once }, { "rate_said", rate_said }, { "silence_ends_at_8", silence_ends_at_8 },
		{ "rec_sound_zero", rec_sound_zero }, { "full_takes_no_more", full_takes_no_more },
		{ "fed_recording", fed_recording }, { "mic_recording", mic_recording }, { "no_recording", no_recording },
		{ "fed_and_mic_recording", fed_and_mic_recording },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-18s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
