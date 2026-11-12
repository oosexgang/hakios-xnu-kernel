/*
 * The queue between the touch and key drivers and iOS (kernel/overlay/bsd/dev/redwood_input.c, /dev/redwood-touch),
 * run as a host program: what a driver posts reaches a reader whole and in order, a queue nobody reads
 * keeps the newest, and the way out of a run with five fingers held is a driver's fingers' alone -- a
 * program that writes samples to the device (inject.c does) cannot end the run with them.
 *
 *   input_queue_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
#include <sys/redwood_input.h>

extern char log_text[];

/* what the driver calls on the way out */
static volatile unsigned returned, gpu_stopped;
void loader_return(void) { returned++; }
void redwood_adreno_stop(void) { gpu_stopped++; }
void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void redwood_input_init(void);

/* the loader's log, where the latency lines go: kept where logged() looks */
void loader_log_putc(char c);
void
loader_log_line(const char *line)
{
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/* (the kernel's printf takes %llu for a uint64_t, which on this host is an unsigned long of the same size) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#include "redwood_input.c"
#pragma GCC diagnostic pop

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)
#define RECORD          sizeof(struct redwood_touch)
#define NODE            "redwood-touch"

/* n records read by a program, and how many came */
static unsigned
take(struct redwood_touch *out, unsigned n)
{
	size_t done = 0;

	CHECK(fake_dev_pread(NODE, out, n * RECORD, 0, &done) == 0, "a read");
	return (unsigned)(done / RECORD);
}

static int
inject(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	struct redwood_touch t = { .type = type, .finger = finger, .x = x, .y = y };

	return fake_dev_write(NODE, &t, sizeof(t));
}

/* ---- scenarios ------------------------------------------------------------------------------------ */

/* what is posted arrives whole and in order, with its time; a read takes as many records as it has room for */
static void
in_order(void)
{
	struct redwood_touch got[8];
	uint64_t before;

	redwood_input_init();
	before = mach_absolute_time();
	redwood_touch_post(REDWOOD_TOUCH_DOWN, 0, 100, 200);
	redwood_touch_post(REDWOOD_TOUCH_MOVE, 0, 110, 210);
	redwood_touch_post(REDWOOD_TOUCH_UP, 0, 120, 220);
	CHECK(take(got, 2) == 2, "two records into room for two");
	CHECK(got[0].type == REDWOOD_TOUCH_DOWN && got[0].x == 100 && got[0].y == 200 && got[1].type == REDWOOD_TOUCH_MOVE && got[1].x == 110,
	    "the first two, in order");
	CHECK(got[0].time >= before && got[1].time >= got[0].time && redwood_touch_last >= got[0].time, "with their times");
	CHECK(take(got, 8) == 1 && got[0].type == REDWOOD_TOUCH_UP && got[0].y == 220, "then the third");
	/* a key, as the keys' driver posts one */
	redwood_touch_post(REDWOOD_BUTTON_DOWN, REDWOOD_BUTTON_VOLUME_UP, 0, 0);
	CHECK(take(got, 1) == 1 && got[0].type == REDWOOD_BUTTON_DOWN && got[0].finger == REDWOOD_BUTTON_VOLUME_UP, "a key");
	/* a position past the panel is held at its edge */
	redwood_touch_post(REDWOOD_TOUCH_DOWN, 1, 70000, 1u << 31);
	CHECK(take(got, 1) == 1 && got[0].x == 65535 && got[0].y == 65535, "past the edge: %u, %u", got[0].x, got[0].y);
}

/* nobody reads: the queue keeps the newest 512, and a reader that comes later gets those */
static void
nobody_reads(void)
{
	static struct redwood_touch got[600];
	unsigned n;

	redwood_input_init();
	for (unsigned i = 0; i < 600; i++) {
		redwood_touch_post(REDWOOD_TOUCH_MOVE, 0, i, i);
	}
	n = take(got, 600);
	CHECK(n == 512, "%u records kept", n);
	CHECK(got[0].x == 88 && got[511].x == 599, "the newest: %u to %u", got[0].x, got[511].x);
}

/* a program may write samples, as the window server's injector does; something that is not one is refused */
static void
written_samples(void)
{
	struct redwood_touch got[4], two[2] = { { .type = REDWOOD_TOUCH_DOWN, .x = 5 }, { .type = REDWOOD_TOUCH_UP, .x = 6 } };

	redwood_input_init();
	CHECK(fake_dev_write(NODE, two, sizeof(two)) == 0, "two samples written");
	CHECK(take(got, 4) == 2 && got[0].x == 5 && got[1].x == 6, "and read");
	CHECK(inject(0, 0, 0, 0) == EINVAL && inject(REDWOOD_BUTTON_UP + 1, 0, 0, 0) == EINVAL, "a type there is not: refused");
	CHECK(inject(REDWOOD_BUTTON_DOWN, REDWOOD_BUTTON_LOCK, 0, 0) == 0 && take(got, 4) == 1, "a key, written");
}

/* five of a driver's fingers held for a second and a half end the run: the GPU first, then back to the loader */
static void
five_fingers_end_the_run(void)
{
	redwood_input_init();
	fake_delay_divisor = 10;                /* 1.5 s: 150 ms here */
	for (unsigned f = 0; f < 5; f++) {
		redwood_touch_post(REDWOOD_TOUCH_DOWN, f, 100 * f, 100);
	}
	EVENTUALLY(returned == 1, 3000, "the run ends: %u", returned);
	CHECK(gpu_stopped == 1, "with the GPU given back first");
}

/* ... not if one lifts before the time is up, nor with four */
static void
lifted_in_time(void)
{
	redwood_input_init();
	fake_delay_divisor = 10;
	for (unsigned f = 0; f < 5; f++) {
		redwood_touch_post(REDWOOD_TOUCH_DOWN, f, 0, 0);
	}
	usleep(50 * 1000);
	redwood_touch_post(REDWOOD_TOUCH_UP, 3, 0, 0);
	usleep(400 * 1000);
	CHECK(returned == 0, "one lifted after a third of the time: nothing ends");
	/* four stay down, a fifth lands again: the time starts over */
	redwood_touch_post(REDWOOD_TOUCH_DOWN, 3, 0, 0);
	usleep(80 * 1000);
	CHECK(returned == 0, "half the time after it landed again: not yet");
	EVENTUALLY(returned == 1, 3000, "then the run ends");
	/* (fingers beyond the 32 a driver can number are not counted) */
}

/* a program writing five fingers down does not end the run */
static void
written_fingers_do_not_count(void)
{
	redwood_input_init();
	fake_delay_divisor = 10;
	for (unsigned f = 0; f < 5; f++) {
		CHECK(inject(REDWOOD_TOUCH_DOWN, f, 0, 0) == 0, "finger %u written", f);
	}
	usleep(500 * 1000);
	CHECK(returned == 0 && gpu_stopped == 0, "three times the hold, and the run goes on");
	/* nor do they add to a driver's: four from a driver and one written */
	for (unsigned f = 0; f < 4; f++) {
		redwood_touch_post(REDWOOD_TOUCH_DOWN, f, 0, 0);
	}
	CHECK(inject(REDWOOD_TOUCH_DOWN, 4, 0, 0) == 0, "a fifth written");
	usleep(500 * 1000);
	CHECK(returned == 0, "four of a driver's and one written: the run goes on");
}

/* touch_lat=1: a line for every sample, for the latency tool */
static void
latency_lines_on(void)
{
	boot_args = "touch_lat=1";
	redwood_input_init();
	redwood_touch_post(REDWOOD_TOUCH_MOVE, 2, 300, 400);
	CHECK(logged("tl: p 2 2 300 400 at "), "the line: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "in_order", in_order }, { "nobody_reads", nobody_reads }, { "written_samples", written_samples },
		{ "five_fingers_end_the_run", five_fingers_end_the_run }, { "lifted_in_time", lifted_in_time },
		{ "written_fingers_do_not_count", written_fingers_do_not_count }, { "latency_lines_on", latency_lines_on },
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
