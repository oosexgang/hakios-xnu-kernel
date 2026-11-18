/*
 * The queue between the touch and key drivers and Darwin (drivers/board/input.c, /dev/hakios-touch),
 * run as a host program: what a driver posts reaches a reader whole and in order, a queue nobody reads
 * keeps the newest, and the way out of a run with five fingers held is a driver's fingers' alone -- a
 * program that writes samples to the device (rwtouch does) cannot end the run with them; only root
 * writes to it, and one program reads it.
 *
 *   input_queue_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include <stdarg.h>

#include "fakekernel.h"
#include <sys/board/input.h>

extern char log_text[];

/* what the driver calls on the way out */
static volatile unsigned returned, gpu_stopped;
void loader_return(void) { returned++; }
void hakios_adreno_stop(void) { gpu_stopped++; }
void hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void hakios_input_init(void);

/* (the loader's log, where the latency lines go, is the fake kernel's loader_log_line: kept where
 * logged() looks) */

/* what the driver prints, kept where logged() looks too */
static void
said(const char *format, ...)
{
	char line[768];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/* (the kernel's printf takes %llu for a uint64_t, which on this host is an unsigned long of the same size) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
LCK_MTX_DECLARE(hakios_rsc_lock, 0);       /* (board/soc.c's, which this test does not take in: the RPMh slots' lock) */
#define printf said
#include "board/input.c"
#undef printf
#pragma GCC diagnostic pop

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)
#define RECORD          sizeof(struct hakios_touch)
#define NODE            "hakios-touch"

/* n records read by a program, and how many came */
static unsigned
take(struct hakios_touch *out, unsigned n)
{
	size_t done = 0;

	CHECK(fake_dev_pread(NODE, out, n * RECORD, 0, &done) == 0, "a read");
	return (unsigned)(done / RECORD);
}

static int
inject(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	struct hakios_touch t = { .type = type, .finger = finger, .x = x, .y = y };

	return fake_dev_write(NODE, &t, sizeof(t));
}

/* ---- scenarios ------------------------------------------------------------------------------------ */

/* what is posted arrives whole and in order, with its time; a read takes as many records as it has room for */
static void
in_order(void)
{
	struct hakios_touch got[8];
	uint64_t before;

	hakios_input_init();
	CHECK(!logged("cannot create the device"), "the device is made, and nothing said of it: %s", log_text);
	before = mach_absolute_time();
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, 100, 200);
	hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, 110, 210);
	hakios_touch_post(HAKIOS_TOUCH_UP, 0, 120, 220);
	CHECK(take(got, 2) == 2, "two records into room for two");
	CHECK(got[0].type == HAKIOS_TOUCH_DOWN && got[0].x == 100 && got[0].y == 200 && got[1].type == HAKIOS_TOUCH_MOVE && got[1].x == 110,
	    "the first two, in order");
	CHECK(got[0].time >= before && got[1].time >= got[0].time && hakios_touch_last >= got[0].time, "with their times");
	CHECK(take(got, 8) == 1 && got[0].type == HAKIOS_TOUCH_UP && got[0].y == 220, "then the third");
	/* a key, as the keys' driver posts one */
	hakios_touch_post(HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_VOLUME_UP, 0, 0);
	CHECK(take(got, 1) == 1 && got[0].type == HAKIOS_BUTTON_DOWN && got[0].finger == HAKIOS_BUTTON_VOLUME_UP, "a key");
	/* a position past the panel is held at its edge */
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 1, 70000, 1u << 31);
	CHECK(take(got, 1) == 1 && got[0].x == 65535 && got[0].y == 65535, "past the edge: %u, %u", got[0].x, got[0].y);
}

/* nobody reads: the queue keeps the newest 512, and a reader that comes later gets those */
static void
nobody_reads(void)
{
	static struct hakios_touch got[600];
	unsigned n;

	hakios_input_init();
	for (unsigned i = 0; i < 600; i++) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, i, i);
	}
	n = take(got, 600);
	CHECK(n == 512, "%u records kept", n);
	CHECK(got[0].x == 88 && got[511].x == 599, "the newest: %u to %u", got[0].x, got[511].x);
}

/* a program may write samples, as the window server's injector does; something that is not one is refused */
static void
written_samples(void)
{
	struct hakios_touch got[4], two[2] = { { .type = HAKIOS_TOUCH_DOWN, .x = 5 }, { .type = HAKIOS_TOUCH_UP, .x = 6 } };

	hakios_input_init();
	CHECK(fake_dev_write(NODE, two, sizeof(two)) == 0, "two samples written");
	CHECK(take(got, 4) == 2 && got[0].x == 5 && got[1].x == 6, "and read");
	CHECK(inject(0, 0, 0, 0) == EINVAL && inject(HAKIOS_BUTTON_UP + 1, 0, 0, 0) == EINVAL, "a type there is not: refused");
	CHECK(inject(HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_LOCK, 0, 0) == 0 && take(got, 4) == 1, "a key, written");
	CHECK(inject(HAKIOS_BUTTON_UP, HAKIOS_BUTTON_LOCK, 0, 0) == 0 && take(got, 4) == 1 && got[0].type == HAKIOS_BUTTON_UP,
	    "and let go: the last of the types, written");
}

/* five of a driver's fingers held for a second and a half end the run: the GPU first, then back to the loader */
static void
five_fingers_end_the_run(void)
{
	hakios_input_init();
	fake_delay_divisor = 2;                 /* 1.5 s: 750 ms here (wide: a busy machine's sleeps overrun) */
	for (unsigned f = 0; f < 5; f++) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, f, 100 * f, 100);
	}
	EVENTUALLY(returned == 1, 3000, "the run ends: %u", returned);
	CHECK(gpu_stopped == 1, "with the GPU given back first");
}

/* ... not if one lifts before the time is up, nor with four */
static void
lifted_in_time(void)
{
	hakios_input_init();
	fake_delay_divisor = 2;                 /* the hold: 750 ms here */
	for (unsigned f = 0; f < 5; f++) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, f, 0, 0);
	}
	usleep(100 * 1000);
	hakios_touch_post(HAKIOS_TOUCH_UP, 3, 0, 0);
	usleep(1200 * 1000);
	CHECK(returned == 0, "one lifted well before the time: nothing ends, long after");
	/* four stay down, a fifth lands again: the time starts over */
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 3, 0, 0);
	usleep(100 * 1000);
	CHECK(returned == 0, "a little after it landed again: not yet");
	EVENTUALLY(returned == 1, 3000, "then the run ends");
}

/* a sixth finger landing while five are held does not put the end off: the time runs from the fifth */
static void
a_sixth_finger_changes_nothing(void)
{
	hakios_input_init();                    /* (the hold: 1.5 s) */
	for (unsigned f = 0; f < 5; f++) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, f, 0, 0);
	}
	usleep(1000 * 1000);
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 5, 0, 0);
	usleep(900 * 1000);                     /* 1.9 s from the fifth; 0.9 s from the sixth */
	CHECK(returned == 1, "the run ended 1.5 s after the fifth finger, not the sixth: %u", returned);
}

/* six held and one lifted: five are still held, and the run ends all the same */
static void
six_and_one_lifted(void)
{
	hakios_input_init();
	fake_delay_divisor = 2;                 /* the hold: 750 ms here */
	for (unsigned f = 0; f < 6; f++) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, f, 0, 0);
	}
	hakios_touch_post(HAKIOS_TOUCH_UP, 5, 0, 0);
	EVENTUALLY(returned == 1, 3000, "five still held: the run ends: %u", returned);
}

/* a finger numbered past the 32 a driver can number is no finger: four and that one do not end the run */
static void
finger_32_is_none(void)
{
	hakios_input_init();
	fake_delay_divisor = 2;
	for (unsigned f = 1; f < 5; f++) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, f, 0, 0);
	}
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 32, 0, 0);
	hakios_touch_post(HAKIOS_TOUCH_DOWN, 33, 0, 0);
	usleep(1500 * 1000);
	CHECK(returned == 0, "four fingers and two that are none: nothing ends");
}

/* fingers a driver says before the input path is set up are counted for nothing, and harm nothing */
static void
fingers_before_the_start(void)
{
	for (unsigned f = 0; f < 5; f++) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, f, 0, 0);
	}
	hakios_input_init();
	fake_delay_divisor = 2;
	usleep(1500 * 1000);
	CHECK(returned == 0, "nothing ends of what came before the start");
}

/* a program writing five fingers down does not end the run */
static void
written_fingers_do_not_count(void)
{
	hakios_input_init();
	fake_delay_divisor = 10;                /* the hold: 150 ms here; waited for many times over */
	for (unsigned f = 0; f < 5; f++) {
		CHECK(inject(HAKIOS_TOUCH_DOWN, f, 0, 0) == 0, "finger %u written", f);
	}
	usleep(500 * 1000);
	CHECK(returned == 0 && gpu_stopped == 0, "three times the hold, and the run goes on");
	/* nor do they add to a driver's: four from a driver and one written */
	for (unsigned f = 0; f < 4; f++) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, f, 0, 0);
	}
	CHECK(inject(HAKIOS_TOUCH_DOWN, 4, 0, 0) == 0, "a fifth written");
	usleep(500 * 1000);
	CHECK(returned == 0, "four of a driver's and one written: the run goes on");
}

/* a read whose buffer stops being writable halfway gets what was moved before it, and no error */
static void
a_fault_halfway(void)
{
	struct hakios_touch got[3];

	hakios_input_init();
	for (unsigned i = 0; i < 3; i++) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, i, i);
	}
	fake_uiomove_fault_at = fake_uiomove_calls + 2;
	CHECK(take(got, 3) == 1 && got[0].x == 0, "the first record, and the read does not fail");
}

/* without touch_lat, or with touch_lat=0: no such lines */
static void
latency_lines_off(const char *arguments)
{
	boot_args = arguments;
	hakios_input_init();
	hakios_touch_post(HAKIOS_TOUCH_MOVE, 2, 300, 400);
	CHECK(!logged("tl: "), "\"%s\": no line: %s", boot_args, log_text);
}

static void latency_lines_not_asked(void) { latency_lines_off(""); }
static void latency_lines_zero(void) { latency_lines_off("touch_lat=0"); }

/* touch_lat=1: a line for every sample, for the latency tool */
static void
latency_lines_on(void)
{
	boot_args = "touch_lat=1";
	hakios_input_init();
	hakios_touch_post(HAKIOS_TOUCH_MOVE, 2, 300, 400);
	CHECK(logged("tl: p 2 2 300 400 at "), "the line: %s", log_text);
}

/* samples written by a program not root's are refused, at the open for writing and at the write: they would be taps on the screen */
static void
only_root_writes(void)
{
	struct hakios_touch got;

	hakios_input_init();
	fake_uid = 501;
	CHECK(inject(HAKIOS_TOUCH_DOWN, 0, 100, 200) == EPERM, "a write by user 501");
	CHECK(fake_dev_open(NODE, FWRITE) == EPERM, "an open for writing by user 501");
	CHECK(fake_dev_open(NODE, FREAD) == 0, "an open for reading by user 501 (the window server's)");
	fake_uid = 0;
	CHECK(fake_dev_open(NODE, FREAD | FWRITE) == 0, "an open for writing by root");
	CHECK(inject(HAKIOS_TOUCH_DOWN, 0, 100, 200) == 0, "a write by root");
	CHECK(take(&got, 1) == 1 && got.x == 100 && got.y == 200, "root's sample, and only it, in the queue");
}

/* the first to open the node for reading reads it: another gets EBUSY while it lives, root aside; once it is gone, or the node closed, the next */
static void
one_reader(void)
{
	hakios_input_init();
	fake_uid = 501;
	fake_pid = 200;
	CHECK(fake_dev_open(NODE, FREAD) == 0, "the window server opens it");
	fake_other_pid = 200;           /* (and lives on) */
	fake_pid = 300;
	CHECK(fake_dev_open(NODE, FREAD) == EBUSY, "an application, after it");
	CHECK(logged("pid 300 may not read the touches: pid 200 does"), "said: %s", log_text);
	fake_pid = 200;
	CHECK(fake_dev_open(NODE, FREAD) == 0, "the window server again (a second descriptor)");
	fake_uid = 0;
	fake_pid = 300;
	CHECK(fake_dev_open(NODE, FREAD) == 0, "root, to look");
	fake_uid = 501;
	CHECK(fake_dev_open(NODE, FREAD) == EBUSY, "root's look did not make pid 300 the reader");
	fake_other_pid = 0;             /* the window server ended, the node still open somewhere else */
	CHECK(fake_dev_open(NODE, FREAD) == 0, "the next window server, once the first is gone");
	fake_other_pid = 300;
	fake_pid = 400;
	CHECK(fake_dev_open(NODE, FREAD) == EBUSY, "and it is now the reader");
	CHECK(fake_dev_close(NODE) == 0, "the node's last close");
	CHECK(fake_dev_open(NODE, FREAD) == 0, "after it, whoever opens it first");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "in_order", in_order }, { "nobody_reads", nobody_reads }, { "written_samples", written_samples },
		{ "five_fingers_end_the_run", five_fingers_end_the_run }, { "lifted_in_time", lifted_in_time },
		{ "written_fingers_do_not_count", written_fingers_do_not_count }, { "latency_lines_on", latency_lines_on },
		{ "a_sixth_finger_changes_nothing", a_sixth_finger_changes_nothing }, { "a_fault_halfway", a_fault_halfway },
		{ "six_and_one_lifted", six_and_one_lifted }, { "finger_32_is_none", finger_32_is_none },
		{ "fingers_before_the_start", fingers_before_the_start }, { "latency_lines_not_asked", latency_lines_not_asked },
		{ "latency_lines_zero", latency_lines_zero }, { "only_root_writes", only_root_writes }, { "one_reader", one_reader },
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
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
