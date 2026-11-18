/*
 * Which "is this process sandboxed?" questions the sandbox stand-in answers yes (kernel/overlay/bsd/kern/sandbox_blastdoor.h),
 * run as a host program: kern_minimac.c copies in request 2's arguments, looks up the name of the process they name, and
 * writes "sandboxed" into the caller's result when this says so.
 *
 *   sandbox_blastdoor_test SCENARIO      one scenario a process
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kern/sandbox_blastdoor.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static const struct sandbox_check_args am_i = { .result = 0x16f000000ULL, .pid = 412, .operation = 0 };

/* the service asks of itself, by its long name and by the kernel's short one (16 characters): sandboxed */
static void
blastdoor_sandboxed(void)
{
	static const char *const names[] = { "MessagesBlastDoorService", "MessagesBlastDoo", "BlastDoor", "SomeBlastDoorHelper" };

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		CHECK(sandbox_blastdoor_sandboxed(&am_i, names[i], 1) == 1, "%s", names[i]);
	}
}

/* every other process is not, nor a name that only has part of it, nor one in another case */
static void
others_not(void)
{
	static const char *const names[] = { "imagent", "SpringBoard", "launchd", "BlastDo", "Blast", "blastdoor", "BLASTDOOR", "" };

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		CHECK(sandbox_blastdoor_sandboxed(&am_i, names[i], 1) == 0, "\"%s\"", names[i]);
	}
	CHECK(sandbox_blastdoor_sandboxed(&am_i, NULL, 1) == 0, "no name (the process is gone)");
}

/* a question that names an operation ("may it look up this service?") is another question: left as it was */
static void
operation_left(void)
{
	struct sandbox_check_args args = am_i;

	args.operation = 0x1c0004000ULL;
	CHECK(sandbox_blastdoor_sandboxed(&args, "MessagesBlastDoorService", 1) == 0, "an operation named");
}

/* nowhere to write the answer, and pids no process has: nothing written */
static void
bad_arguments(void)
{
	struct sandbox_check_args args = am_i;

	args.result = 0;
	CHECK(sandbox_blastdoor_sandboxed(&args, "MessagesBlastDoorService", 1) == 0, "no result");
	args = am_i;
	args.pid = 0;
	CHECK(sandbox_blastdoor_sandboxed(&args, "MessagesBlastDoorService", 1) == 0, "pid 0");
	args.pid = 0x80000000ULL;
	CHECK(sandbox_blastdoor_sandboxed(&args, "MessagesBlastDoorService", 1) == 0, "a pid past 32 bits' signed range");
	args.pid = ~0ULL;
	CHECK(sandbox_blastdoor_sandboxed(&args, "MessagesBlastDoorService", 1) == 0, "pid -1");
}

/* sandbox_blastdoor=0: BlastDoor is told it is not sandboxed either, as before */
static void
turned_off(void)
{
	CHECK(sandbox_blastdoor_sandboxed(&am_i, "MessagesBlastDoorService", 0) == 0, "sandbox_blastdoor=0");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "blastdoor_sandboxed", blastdoor_sandboxed }, { "others_not", others_not }, { "operation_left", operation_left },
		{ "bad_arguments", bad_arguments }, { "turned_off", turned_off },
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
