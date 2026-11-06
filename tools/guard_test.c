/*
 * The speakers' guard (bsd/dev/redwood_guard.c) on this machine, with signals made up for it:
 * what comes out, second by second, for a sustained full-scale tone at the amplifiers' levels that
 * are tried on the phone, for music-like noise, and for the tone once it stops (the cooling).
 *
 *   cc -O2 -DKERNEL -I<a directory with sys/redwood_audio.h and a sys/ioccom.h> tools/guard_test.c \
 *      xnu-src/xnu-8796.121.2/bsd/dev/redwood_guard.c -lm -o guard_test && ./guard_test
 * (tools/guard-test.sh does that.)
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/redwood_audio.h>

#define RATE    48000
#define BLOCK   960

static double phase, pink_scale = 1;
static uint64_t noise = 88172645463325252ull;

static double
random_uniform(void)
{
	noise ^= noise << 13;
	noise ^= noise >> 7;
	noise ^= noise << 17;
	return (double)(noise >> 11) / (double)(1ull << 53) * 2 - 1;
}

/* one block: kind 0 silence, 1 a sine at `level' (of full scale, peak), 2 noise filtered to fall 3 dB an octave, at RMS `level' */
static void
make(int16_t *frames, int kind, double level, double hz)
{
	static double b[7];

	for (int f = 0; f < BLOCK; f++) {
		double v = 0;

		if (kind == 1) {
			v = level * sin(phase);
			phase += 2 * M_PI * hz / RATE;
		} else if (kind == 2) {
			/* Paul Kellet's pink noise, scaled to about the RMS asked for */
			double w = random_uniform();

			b[0] = 0.99886 * b[0] + w * 0.0555179;
			b[1] = 0.99332 * b[1] + w * 0.0750759;
			b[2] = 0.96900 * b[2] + w * 0.1538520;
			b[3] = 0.86650 * b[3] + w * 0.3104856;
			b[4] = 0.55000 * b[4] + w * 0.5329522;
			b[5] = -0.7616 * b[5] - w * 0.0168980;
			v = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362) * pink_scale * level;
			b[6] = w * 0.115926;
		}
		v *= 32767;
		v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
		frames[2 * f] = frames[2 * f + 1] = (int16_t)lrint(v);
	}
}

static double
decibels(double ratio)
{
	return ratio > 0 ? 10 * log10(ratio) : -99;
}

/* `seconds' of one kind of signal; a line every `every' seconds: the output's RMS and peak at the speaker, and the guard */
static void
run(const char *what, uint32_t attenuation, int kind, double level, double hz, int seconds, int every)
{
	int16_t frames[2 * BLOCK];
	bool silent = attenuation == REDWOOD_GUARD_SILENT;
	double at_speaker = silent ? 0 : pow(10, -(double)attenuation / 80.0);   /* power */

	if (silent) {
		printf("-- %s, the amplifiers off\n", what);
	} else {
		printf("-- %s, the amplifiers %.1f dB under Android's level\n", what, attenuation / 8.0);
	}
	redwood_guard_attenuation(attenuation);
	for (int s = 0; s < seconds; s++) {
		double sum = 0, peak = 0, in_sum = 0;
		struct redwood_guard_report r;

		for (int b = 0; b < RATE / BLOCK; b++) {
			make(frames, kind, level, hz);
			for (int i = 0; i < 2 * BLOCK; i++) {
				in_sum += (double)frames[i] * frames[i];
			}
			redwood_guard_process(frames, BLOCK);
			for (int i = 0; i < 2 * BLOCK; i++) {
				double v = frames[i] / 32768.0;

				sum += v * v;
				peak = fabs(v) > peak ? fabs(v) : peak;
			}
		}
		redwood_guard_take_report(&r);
		if ((s + 1) % every == 0 || s == 0) {
			/* at the speaker: relative to a full-scale square wave at Android's level */
			printf("%4d s: in %6.1f dB, out %6.1f dB (peak %6.1f dB) at the speaker; guard %5.1f dB (%u rms, %u fast, %u slow "
			    "blocks), peaks limited in %u frames; averages %.1f %.1f %.1f dB\n", s + 1,
			    decibels(in_sum / (2.0 * RATE) / (32768.0 * 32768.0) * at_speaker), decibels(sum / (2.0 * RATE) * at_speaker),
			    silent ? -99 : 20 * log10(peak > 0 ? peak : 1e-9) - attenuation / 8.0, redwood_guard_decibels(r.gain * r.gain >> 30) / 10.0,
			    r.rms_blocks, r.fast_blocks, r.slow_blocks, r.limited_frames, redwood_guard_decibels(r.rms) / 10.0,
			    redwood_guard_decibels(r.fast) / 10.0, redwood_guard_decibels(r.slow) / 10.0);
		}
	}
}

int
main(void)
{
	int16_t frames[2 * BLOCK];
	double sum = 0;

	/* the noise's own RMS, so that it can be made as loud as asked (at full scale, then scaled down in make()) */
	pink_scale = 1.0 / 32767;
	for (int b = 0; b < 500; b++) {
		make(frames, 2, 1.0, 0);
		for (int i = 0; i < 2 * BLOCK; i += 2) {
			sum += (double)frames[i] * frames[i];
		}
	}
	pink_scale = 1.0 / sqrt(sum / (500.0 * BLOCK));
	redwood_guard_configure(true, 0);
	run("a full-scale 1 kHz sine", 160, 1, 1.0, 1000, 10, 5);
	run("a full-scale 1 kHz sine", 80, 1, 1.0, 1000, 10, 5);
	run("a full-scale 1 kHz sine", 0, 1, 1.0, 1000, 120, 10);
	run("silence, the speakers cooling", REDWOOD_GUARD_SILENT, 0, 0, 0, 120, 20);
	run("the same sine again", 0, 1, 1.0, 1000, 5, 1);
	run("silence", REDWOOD_GUARD_SILENT, 0, 0, 0, 300, 300);
	run("a 100 Hz sine at full scale (the high-pass filter)", 0, 1, 1.0, 100, 4, 2);
	run("silence", REDWOOD_GUARD_SILENT, 0, 0, 0, 300, 300);
	run("pink noise at -15 dB RMS (loud music)", 0, 2, pow(10, -15 / 20.0), 0, 60, 10);
	run("pink noise at -9 dB RMS (music mastered as loud as it goes)", 0, 2, pow(10, -9 / 20.0), 0, 120, 20);
	run("silence", REDWOOD_GUARD_SILENT, 0, 0, 0, 300, 300);
	/* iOS at its top: a tone 12.6 dB under full scale, then 12 dB of gain before the guard */
	redwood_guard_configure(true, 96);
	run("iOS's top (a 440 Hz sine at -12.6 dB) with 12 dB of gain", 0, 1, pow(10, -12.6 / 20), 440, 90, 10);
	run("silence", REDWOOD_GUARD_SILENT, 0, 0, 0, 600, 600);
	run("pink noise at -24 dB RMS (loud music as iOS leaves it) with 12 dB of gain", 0, 2, pow(10, -24 / 20.0), 0, 60, 20);
	run("silence", REDWOOD_GUARD_SILENT, 0, 0, 0, 600, 600);
	run("the same with the amplifiers 90 dB under (sound_below=720)", 720, 2, pow(10, -24 / 20.0), 0, 10, 5);
	return 0;
}
