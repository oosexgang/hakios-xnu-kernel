/*
 * The speakers' guard itself (drivers/board/guard.c), run as a host program: what
 * stands between Darwin's samples and two small speakers where Android has an algorithm in the DSP.
 * It is integer arithmetic with no hardware under it, so everything it promises can be measured
 * here: the power that reaches a speaker stays under each of its three limits, no sample goes over
 * the peak limit, sound of ordinary loudness passes untouched, what is under the speakers'
 * resonance is taken out, and the level comes back when the sound is quiet again.
 *
 *   guard_test SCENARIO      one scenario a process: the guard keeps its state in statics
 */
#include <stdio.h>
#include <string.h>

#define KERNEL 1                        /* (the kernel's half of sys/board/audio.h) */
#include "board/guard.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define RATE            48000
#define PERIOD          960             /* the feeder's: 20 ms */
#define FULL            32767

/* limits as fractions of a full-scale square wave's power, and of full scale: 10^(-dB/10), 10^(-dB/20) */
#define MINUS_6_DB      0.251189
#define MINUS_9_DB      0.125893
#define MINUS_12_DB     0.063096
#define PEAK_CEILING    23198           /* 3 dB under full scale */

/* a sine without the maths library: the cosine of the step by its series, then a rotation */
static struct { double s, c, step_s, step_c; } tone;

static void
tone_of(double hz)
{
	double x = 2 * 3.14159265358979323846 * hz / RATE, term = 1, sum_c = 0, sum_s = 0;

	for (int n = 0; n < 30; n++) {
		if (n & 1) {
			sum_s += term;
		} else {
			sum_c += term;
		}
		term = term * x / (n + 1) * ((n & 1) ? -1 : 1);
	}
	tone.step_c = sum_c;
	tone.step_s = sum_s;
	tone.s = 0;
	tone.c = 1;
}

static double
next(void)
{
	double s = tone.s * tone.step_c + tone.c * tone.step_s, c = tone.c * tone.step_c - tone.s * tone.step_s;
	double norm = 1.5 - 0.5 * (s * s + c * c);      /* (kept on the circle) */

	tone.s = s * norm;
	tone.c = c * norm;
	return tone.s;
}

/*
 * What came out of the last run, in its last 0.2 s: the largest sample, and the power as a fraction of a full-scale
 * square wave's. (A tone that starts makes the filter ring for a few milliseconds: some 12% over, measured.) And the
 * largest sample that ever came out.
 */
static int peak_out, peak_ever;
static double power_out;

/* `seconds' of a sine of that amplitude on both channels through the guard, a period at a time; the frames
 * after the period are the caller's, which the guard must leave as they are */
#define AFTER           8
static void
play(double amplitude, double seconds)
{
	static int16_t frames[2 * (PERIOD + AFTER)];
	long periods = (long)(seconds * RATE / PERIOD);
	double sum = 0;
	bool overrun = false;

	peak_out = 0;
	for (long p = 0; p < periods; p++) {
		for (int f = 0; f < PERIOD; f++) {
			frames[2 * f] = frames[2 * f + 1] = (int16_t)(next() * amplitude);
		}
		for (int i = 2 * PERIOD; i < 2 * (PERIOD + AFTER); i++) {
			frames[i] = 0x5a5a;
		}
		hakios_guard_process(frames, PERIOD);
		for (int i = 2 * PERIOD; i < 2 * (PERIOD + AFTER) && !overrun; i++) {
			overrun = frames[i] != 0x5a5a;
		}
		if (p == periods - 10) {
			sum = 0;
			peak_out = 0;
		}
		for (int i = 0; i < 2 * PERIOD; i++) {
			int magnitude = frames[i] < 0 ? -frames[i] : frames[i];

			if (magnitude > peak_out) {
				peak_out = magnitude;
			}
			if (magnitude > peak_ever) {
				peak_ever = magnitude;
			}
			sum += (double)frames[i] * frames[i];
		}
	}
	power_out = sum / (10.0 * 2 * PERIOD) / (32768.0 * 32768.0);
	CHECK(!overrun, "nothing past the frames given is touched");
}

static struct hakios_guard_report report;

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* sound of ordinary loudness (20 dB under full scale) passes as it is: no limit binds, no sample is touched */
static void
ordinary_sound_passes(void)
{
	hakios_guard_attenuation(0);
	tone_of(1000);
	play(3277, 5);
	hakios_guard_take_report(&report);
	CHECK(report.least_gain == ONE && report.rms_blocks + report.fast_blocks + report.slow_blocks == 0, "no power limit bound");
	CHECK(report.limited_frames == 0, "the peak limiter touched %u frames", report.limited_frames);
	CHECK(peak_out >= 3240 && peak_out <= 3290, "the tone comes out as it went in: a peak of %d", peak_out);
	CHECK(report.peak_in == 3276 || report.peak_in == 3277, "the report has what went in: %u", report.peak_in);
}

/* a full-scale tone: at once no sample over the peak limit and no more power than 6 dB under a square wave's */
static void
loud_tone_is_held(void)
{
	hakios_guard_attenuation(0);
	tone_of(1000);
	play(FULL, 0.5);
	CHECK(peak_ever <= PEAK_CEILING, "a sample of %d is over the peak limit", peak_ever);
	CHECK(power_out <= MINUS_6_DB * 1.03, "the power is %.4f of a square wave's, over the first limit", power_out);
	CHECK(power_out >= MINUS_6_DB * 0.80, "... and it is not turned down further than the limit asks: %.4f", power_out);
	hakios_guard_take_report(&report);
	CHECK(report.rms_blocks > 0 && report.least_gain < ONE, "the report says which limit bound");
}

/* ... held for seconds: 9 dB under; for minutes: 12 dB under */
static void
sustained_tone_is_turned_down(void)
{
	hakios_guard_attenuation(0);
	tone_of(1000);
	play(FULL, 15);
	CHECK(power_out <= MINUS_9_DB * 1.03 && power_out >= MINUS_9_DB * 0.80, "after 15 s: %.4f of a square wave's power", power_out);
	hakios_guard_take_report(&report);
	CHECK(report.fast_blocks > 0, "the 3 s average bound");
	play(FULL, 285);
	CHECK(power_out <= MINUS_12_DB * 1.03 && power_out >= MINUS_12_DB * 0.80, "after 5 min: %.4f of a square wave's power", power_out);
	hakios_guard_take_report(&report);
	CHECK(report.slow_blocks > 0, "the 60 s average bound");
	CHECK(peak_ever <= PEAK_CEILING, "a sample of %d is over the peak limit", peak_ever);
}

/* the level comes back: after a loud minute, quiet sound is whole again once the averages have fallen */
static void
level_comes_back(void)
{
	hakios_guard_attenuation(0);
	tone_of(1000);
	play(FULL, 60);
	play(3277, 1);
	CHECK(peak_out < 3200, "right after it the quiet tone is still turned down: %d", peak_out);
	play(0, 240);
	play(3277, 1);
	hakios_guard_take_report(&report);
	CHECK(report.gain == ONE && peak_out >= 3240, "after four quiet minutes it is whole: a peak of %d", peak_out);
}

/* the averages go on falling while the amplifiers are off, and nothing counts as reaching a speaker then */
static void
amplifiers_off(void)
{
	tone_of(1000);
	play(FULL, 5);                          /* (the guard starts silent: the amplifiers are off) */
	hakios_guard_take_report(&report);
	CHECK(report.rms == 0 && report.fast == 0 && report.slow == 0 && report.least_gain == ONE, "nothing reached a speaker");
	hakios_guard_attenuation(0);
	play(FULL, 15);
	hakios_guard_attenuation(HAKIOS_GUARD_SILENT);
	play(FULL, 120);
	hakios_guard_take_report(&report);
	CHECK(report.fast < (uint64_t)(MINUS_9_DB * ONE / 100), "the 3 s average fell while they were off");
	CHECK(report.gain == ONE, "... and the level is back");
}

/* what is under the speakers' resonance is taken out: 50 Hz to a sixteenth, a constant to nothing; 2 kHz is left alone */
static void
low_tones_are_taken_out(void)
{
	static int16_t frames[2 * PERIOD];

	hakios_guard_attenuation(0);
	tone_of(50);
	play(3277, 2);
	CHECK(peak_out > 150 && peak_out < 260, "50 Hz comes out at %d of 3277", peak_out);
	tone_of(2000);
	play(3277, 2);
	CHECK(peak_out >= 3240 && peak_out <= 3290, "2 kHz comes out at %d of 3277", peak_out);
	for (int p = 0; p < 100; p++) {
		for (int i = 0; i < 2 * PERIOD; i++) {
			frames[i] = 10000;
		}
		hakios_guard_process(frames, PERIOD);
	}
	CHECK(frames[0] >= -1 && frames[0] <= 1 && frames[2 * PERIOD - 1] >= -1 && frames[2 * PERIOD - 1] <= 1, "a constant comes out as %d", frames[0]);
}

/* without the filter (a measurement's choice) a low tone passes; the limits hold all the same */
static void
filter_off(void)
{
	hakios_guard_configure(false, 0);
	hakios_guard_attenuation(0);
	tone_of(50);
	play(3277, 2);
	CHECK(peak_out >= 3240 && peak_out <= 3290, "50 Hz comes out at %d of 3277", peak_out);
	play(FULL, 2);
	CHECK(peak_ever <= PEAK_CEILING && power_out <= MINUS_6_DB * 1.03, "a peak of %d, %.4f of a square wave's power", peak_ever, power_out);
}

/* the gain before everything: quiet sound is lifted by it, and what it lifts over the limit is held, never clipped */
static void
gain_is_limited_not_clipped(void)
{
	static int16_t frames[2 * PERIOD];
	int flat = 0;

	hakios_guard_configure(true, 12 * 8);
	hakios_guard_attenuation(0);
	tone_of(1000);
	play(820, 2);                           /* 32 dB under full scale */
	CHECK(peak_out >= 3210 && peak_out <= 3300, "12 dB more: %d comes out at %d", 820, peak_out);
	play(FULL, 1);
	CHECK(peak_ever <= PEAK_CEILING, "a sample of %d is over the peak limit", peak_ever);
	/* a clipped sine sits at one value for many samples running; a limited one does not */
	for (int f = 0; f < PERIOD; f++) {
		frames[2 * f] = frames[2 * f + 1] = (int16_t)(next() * FULL);
	}
	hakios_guard_process(frames, PERIOD);
	for (int f = 1; f < PERIOD; f++) {
		flat += frames[2 * f] == frames[2 * f - 2] && frames[2 * f] != 0;
	}
	CHECK(flat < PERIOD / 20, "%d of %d samples repeat the one before: clipped", flat, PERIOD);
	hakios_guard_configure(true, 1000);
	play(0, 1);
	play(820, 2);
	CHECK(peak_out <= 3300, "more than 12 dB is not given: %d", peak_out);
}

/* a single full-scale sample in quiet sound: turned down at that very sample */
static void
one_spike(void)
{
	static int16_t frames[2 * PERIOD];

	hakios_guard_configure(false, 0);
	hakios_guard_attenuation(0);
	for (int f = 0; f < PERIOD; f++) {
		frames[2 * f] = frames[2 * f + 1] = (int16_t)((f & 1) ? 1000 : -1000);
	}
	frames[2 * 500] = 32767;
	frames[2 * 700 + 1] = -32768;
	hakios_guard_process(frames, PERIOD);
	CHECK(frames[2 * 500] <= PEAK_CEILING && frames[2 * 500] > PEAK_CEILING - 300, "the spike comes out at %d", frames[2 * 500]);
	CHECK(frames[2 * 700 + 1] >= -PEAK_CEILING && frames[2 * 700 + 1] < -PEAK_CEILING + 300, "the negative one at %d", frames[2 * 700 + 1]);
	CHECK(frames[2 * 499] == 1000 || frames[2 * 499] == 999, "the sample before it is untouched: %d", frames[2 * 499]);
	hakios_guard_take_report(&report);
	CHECK(report.peak_in == 32768 && report.peak_out <= PEAK_CEILING && report.limited_frames > 0, "the report has it");
}

/* the amplifiers turned down: the same samples are that much quieter at the speakers, and are held back only that much later */
static void
amplifiers_turned_down(void)
{
	hakios_guard_attenuation(12 * 8);
	tone_of(1000);
	play(28000, 20);                        /* (so that the filter's ringing at the start stays under full scale) */
	hakios_guard_take_report(&report);
	CHECK(report.least_gain == ONE && report.limited_frames == 0, "a tone near full scale 12 dB down is under every limit");
	CHECK(peak_out >= 27900 && peak_out <= 28010, "... and comes out whole: %d", peak_out);
	hakios_guard_attenuation(8);
	play(FULL, 1);
	/* 1 dB down: the peak limit is 2 dB under full scale for the samples */
	CHECK(peak_out <= 26030 && peak_out >= 25000, "1 dB down, the peaks are held 2 dB under full scale: %d", peak_out);
}

/* what the amplifiers' monitor asks for is taken off as well */
static void
monitor_is_obeyed(void)
{
	hakios_guard_attenuation(0);
	hakios_guard_monitor(3 * 8);
	tone_of(1000);
	play(3277, 2);
	CHECK(peak_out >= 2290 && peak_out <= 2350, "3 dB less: %d of 3277", peak_out);
	hakios_guard_monitor(0);
	play(3277, 2);
	CHECK(peak_out >= 3240, "and whole again without it: %d", peak_out);
}

/* the gain moves across a block from the last block's to this one's and has arrived by the block's last frame:
 * a block of one frame is all at the new gain */
static void
the_gain_arrives_by_the_last_frame(void)
{
	int16_t frame[2] = { 10000, 10000 };

	hakios_guard_configure(false, 0);
	hakios_guard_attenuation(0);
	hakios_guard_monitor(6 * 8);            /* 6 dB down: a half */
	hakios_guard_process(frame, 1);
	CHECK(frame[0] >= 4950 && frame[0] <= 5050 && frame[1] == frame[0], "at the half at once: %d %d of 10000", frame[0], frame[1]);
}

/* more frames at once than the guard works on in one go */
static void
long_call(void)
{
	static int16_t frames[2 * 5000];

	hakios_guard_attenuation(0);
	for (int f = 0; f < 5000; f++) {
		frames[2 * f] = (int16_t)((f & 1) ? 30000 : -30000);
		frames[2 * f + 1] = 0;
	}
	hakios_guard_process(frames, 5000);
	for (int f = 0; f < 5000; f++) {
		if (frames[2 * f] > PEAK_CEILING || frames[2 * f] < -PEAK_CEILING || frames[2 * f + 1] != 0) {
			CHECK(false, "frame %d came out as %d, %d", f, frames[2 * f], frames[2 * f + 1]);
			break;
		}
	}
	CHECK(frames[2 * 4999] > 10000, "the last frame was worked on too: %d", frames[2 * 4999]);
	hakios_guard_take_report(&report);
	CHECK(report.blocks == 5, "5000 frames are five goes of at most 1024: %u", report.blocks);
}

/* 10 log10 of a ratio, in tenths of a dB: what the driver's log lines are made of */
static void
decibels(void)
{
	CHECK(hakios_guard_decibels(ONE) == 0, "1 is 0 dB: %d", hakios_guard_decibels(ONE));
	CHECK(hakios_guard_decibels(ONE / 2) == -30, "a half is -3.0 dB: %d", hakios_guard_decibels(ONE / 2));
	CHECK(hakios_guard_decibels(ONE * 2) == 30, "twice is 3.0 dB: %d", hakios_guard_decibels(ONE * 2));
	CHECK(hakios_guard_decibels(ONE / 10) == -100 || hakios_guard_decibels(ONE / 10) == -99, "a tenth is -10.0 dB: %d",
	    hakios_guard_decibels(ONE / 10));
	CHECK(hakios_guard_decibels(ONE / 1000) <= -299 && hakios_guard_decibels(ONE / 1000) >= -301, "a thousandth is -30.0 dB: %d",
	    hakios_guard_decibels(ONE / 1000));
	CHECK(hakios_guard_decibels(1) <= -902 && hakios_guard_decibels(1) >= -904, "the smallest is -90.3 dB: %d", hakios_guard_decibels(1));
	CHECK(hakios_guard_decibels(0) == -999, "nothing is said as -99.9 dB");
	/* the table of attenuations the limits are computed from: 6 dB is a half, to a part in a thousand */
	CHECK(amplitude(48) > ONE / 2 && amplitude(48) < ONE / 2 + ONE / 400, "6 dB down is %llu", (unsigned long long)amplitude(48));
	CHECK(amplitude(0) == ONE && amplitude(160) > ONE / 10 - ONE / 2000 && amplitude(160) < ONE / 10 + ONE / 2000, "20 dB down is a tenth: %llu",
	    (unsigned long long)amplitude(160));
	for (uint32_t n = 1; n < 400; n++) {
		if (amplitude(n) >= amplitude(n - 1)) {
			CHECK(false, "%u eighths of a dB down is not less than %u", n, n - 1);
			break;
		}
	}
	CHECK(isqrt(0) == 0 && isqrt(1) == 1 && isqrt(99) == 9 && isqrt(100) == 10 && isqrt(1ull << 60) == 1ull << 30, "the square root");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "ordinary_sound_passes", ordinary_sound_passes }, { "loud_tone_is_held", loud_tone_is_held },
		{ "sustained_tone_is_turned_down", sustained_tone_is_turned_down }, { "level_comes_back", level_comes_back },
		{ "amplifiers_off", amplifiers_off }, { "low_tones_are_taken_out", low_tones_are_taken_out }, { "filter_off", filter_off },
		{ "gain_is_limited_not_clipped", gain_is_limited_not_clipped }, { "one_spike", one_spike },
		{ "amplifiers_turned_down", amplifiers_turned_down }, { "monitor_is_obeyed", monitor_is_obeyed },
		{ "the_gain_arrives_by_the_last_frame", the_gain_arrives_by_the_last_frame }, { "long_call", long_call }, { "decibels", decibels },
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
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
