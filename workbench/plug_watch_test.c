/*
 * When a charger plugged in or pulled out between two of the battery's rounds calls for a round at once
 * (drivers/board/plug_watch_core.h, used by the audio DSP's battery manager in drivers/board/adsp.c), run as a host
 * program against the kernel's own header: the rounds and the looks between them are played as the driver makes them.
 *
 *   plug_watch_test SCENARIO      one scenario a process
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/plug_watch_core.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* Plugged in after a round that read nothing: a round at once, then one a second after and one three seconds after. */
static void
plugged_in(void)
{
	struct plug_watch w;

	plug_watch_reset(&w);
	plug_watch_round(&w, 0);
	CHECK(!plug_watch_look(&w, 0, 500), "nothing changed, and a round was called for");
	CHECK(!plug_watch_due(&w, 500), "a round after a change without one");
	CHECK(plug_watch_look(&w, 1, 1000), "a charger plugged in and no round at once");
	CHECK(w.changes == 1, "%u changes", w.changes);
	plug_watch_round(&w, 1);
	CHECK(!plug_watch_look(&w, 1, 1500), "the round read it: no round again for the same");
	CHECK(!plug_watch_due(&w, 1999), "the second round too soon");
	CHECK(plug_watch_due(&w, 2000) && !plug_watch_due(&w, 2000), "the round a second after: not once");
	CHECK(!plug_watch_due(&w, 3999), "the third round too soon");
	CHECK(plug_watch_due(&w, 4000) && !plug_watch_due(&w, 60000), "the round three seconds after: not once");
}

/* Pulled out: the same the other way. */
static void
pulled_out(void)
{
	struct plug_watch w;

	plug_watch_reset(&w);
	plug_watch_round(&w, 1);
	CHECK(plug_watch_look(&w, 0, 7000), "pulled out and no round at once");
	plug_watch_round(&w, 0);
	CHECK(!plug_watch_look(&w, 0, 7500), "a round again for the same");
}

/* Before any round has read the port, and when a look or a round gets no answer, nothing is decided by a look. */
static void
not_known(void)
{
	struct plug_watch w;

	plug_watch_reset(&w);
	CHECK(!plug_watch_look(&w, 1, 100) && !plug_watch_look(&w, 0, 200), "a look before any round called for one");
	plug_watch_round(&w, 0);
	CHECK(!plug_watch_look(&w, -1, 300), "a look without an answer called for a round");
	plug_watch_round(&w, -1);
	CHECK(w.online == -1, "a round without an answer kept the port as it was: %d", w.online);
	CHECK(!plug_watch_look(&w, 1, 400), "after a round without an answer, a look called for a round");
	CHECK(w.changes == 0 && !plug_watch_due(&w, 100000), "%u changes, or a round after one", w.changes);
}

/* A round that comes late (the DSP slow to answer) does for both rounds after a change that are past by then. */
static void
late_round(void)
{
	struct plug_watch w;

	plug_watch_reset(&w);
	plug_watch_round(&w, 0);
	CHECK(plug_watch_look(&w, 1, 1000), "plugged in");
	plug_watch_round(&w, 1);
	CHECK(plug_watch_due(&w, 9000), "no round once both are past");
	CHECK(!plug_watch_due(&w, 9050), "a second round for the one already made");
}

/* Plugged in and out again within the three seconds: the rounds after count from the last change. */
static void
in_and_out(void)
{
	struct plug_watch w;

	plug_watch_reset(&w);
	plug_watch_round(&w, 0);
	CHECK(plug_watch_look(&w, 1, 1000), "plugged in");
	plug_watch_round(&w, 1);
	CHECK(plug_watch_look(&w, 0, 1500), "pulled out");
	plug_watch_round(&w, 0);
	CHECK(!plug_watch_due(&w, 2000), "the rounds after count from the first change");
	CHECK(plug_watch_due(&w, 2500) && plug_watch_due(&w, 4500), "the rounds after the second change");
	CHECK(w.changes == 2, "%u changes", w.changes);
}

/* battmgr_plug=: 0 never looks; anything else within 100 to 5000 ms. */
static void
how_often(void)
{
	CHECK(plug_watch_look_ms(0) == 0, "battmgr_plug=0 looks");
	CHECK(plug_watch_look_ms(1) == PLUG_LOOK_MS_LEAST && plug_watch_look_ms(99) == PLUG_LOOK_MS_LEAST, "looks more often than %d ms",
	    PLUG_LOOK_MS_LEAST);
	CHECK(plug_watch_look_ms(PLUG_LOOK_MS_DEFAULT) == PLUG_LOOK_MS_DEFAULT && plug_watch_look_ms(250) == 250, "not as asked");
	CHECK(plug_watch_look_ms(100000) == PLUG_LOOK_MS_MOST && plug_watch_look_ms(0xffffffffu) == PLUG_LOOK_MS_MOST,
	    "looks less often than %d ms", PLUG_LOOK_MS_MOST);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "plugged_in", plugged_in }, { "pulled_out", pulled_out }, { "not_known", not_known }, { "late_round", late_round },
		{ "in_and_out", in_and_out }, { "how_often", how_often },
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
