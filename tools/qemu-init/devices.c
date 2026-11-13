/* tools/qemu-init: the kernel's sound and input devices: tone, input */
#include "init.h"

#define AUDIO_RATE              48000           /* sys/board/audio.h: 48 kHz, two channels of 16 bits */

/*
 * HZ [SECONDS]: a sine of HZ (y[n] = 2 cos(w) y[n-1] - y[n-2], no libm), a tenth of a second at a time
 * and in time: the device takes what it is given at once and plays it at its own pace, holding no more
 * than a third of a second, so the writes keep two tenths ahead of the clock.
 */
void
command_tone(int argc, char **argv)
{
	unsigned hz = argc > 1 ? parse_number(argv[1]) : 0, seconds = argc > 2 ? parse_number(argv[2]) : 1;
	static int16_t frames[AUDIO_RATE / 10 * 2];

	if (hz < 20 || hz > 20000 || seconds < 1 || seconds > 30) {
		say("tone HZ [SECONDS]: 20 to 20000 Hz, 1 to 30 s\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)"/dev/hakios-audio", O_WRONLY, 0, 0, 0, 0);
	if (fd < 0) {
		say("tone: no sound device (/dev/hakios-audio)\n");
		return;
	}
	double w = 2 * 3.14159265358979 * hz / AUDIO_RATE, w2 = w * w;
	double c = 1 - w2 / 2 * (1 - w2 / 12 * (1 - w2 / 30 * (1 - w2 / 56)));        /* cos(w) */
	double s1 = 0, s2 = -8000 * (w - w * w2 / 6 * (1 - w2 / 20));                   /* sin(-w) times the level */

	int64_t start = now_us();

	for (unsigned tenth = 0; tenth < seconds * 10; tenth++) {
		int64_t early = start + ((int64_t)tenth - 2) * 100000 - now_us();

		if (early > 0) {
			sleep_us(early);
		}
		for (unsigned i = 0; i < AUDIO_RATE / 10; i++) {
			double y = 2 * c * s1 - s2;

			s2 = s1;
			s1 = y;
			frames[2 * i] = frames[2 * i + 1] = (int16_t)y;
		}
		if (syscall6(SYS_write, fd, (long)frames, sizeof(frames), 0, 0, 0) != (long)sizeof(frames)) {
			say("tone: the sound device took less than it was given\n");
			break;
		}
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	say("played ");
	say_number(hz);
	say(" Hz for ");
	say_number(seconds);
	say(" s\n");
}

struct touch_record {
	uint64_t time;
	uint32_t type, finger, x, y;
};

/* [COUNT]: the next COUNT records of /dev/hakios-touch, as they come */
void
command_input(int argc, char **argv)
{
	static const char *const types[] = { "?", "touch down", "touch move", "touch up", "button down", "button up" };
	static const char *const buttons[] = { "?", "lock", "volume up", "volume down", "home" };
	unsigned count = argc > 1 ? parse_number(argv[1]) : 4;
	struct touch_record r;

	if (count < 1 || count > 1000) {
		say("input [COUNT]: 1 to 1000 records\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)"/dev/hakios-touch", 0, 0, 0, 0, 0);
	if (fd < 0) {
		say("input: no input device (/dev/hakios-touch)\n");
		return;
	}
	say("input: waiting for ");
	say_number(count);
	say(" records\n");
	for (unsigned i = 0; i < count && syscall6(SYS_read, fd, (long)&r, sizeof(r), 0, 0, 0) == (long)sizeof(r); i++) {
		say(r.type < 6 ? types[r.type] : "?");
		if (r.type >= 4) {
			say(" ");
			say(r.finger < 5 ? buttons[r.finger] : "?");
		} else {
			say(" finger ");
			say_number(r.finger);
			say(" at ");
			say_number(r.x);
			say(",");
			say_number(r.y);
		}
		say("\n");
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}
