/*
 * The floor under the memory limits of a few of the system's daemons (kernel/overlay/bsd/kern/memory_floor.h), run as
 * a host program: kern_exec.c gives it the name of the process being spawned and the limits launchd asked for (or the
 * ones it kept through an exec), and applies what comes back.
 *
 *   memory_floor_test SCENARIO      one scenario a process
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kern/memory_floor.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* lsd with Apple's limits, 16 MB active and 12 MB inactive: both raised to the floor, and said so */
static void
lsd_raised(void)
{
	int32_t active = 16, inactive = 12;

	CHECK(memory_floor_raise("lsd", MEMORY_FLOOR_DEFAULT_MB, &active, &inactive) == 1, "raised");
	CHECK(active == MEMORY_FLOOR_DEFAULT_MB && inactive == MEMORY_FLOOR_DEFAULT_MB, "%d and %d MB", active, inactive);
}

/* each of the five, and none other: a name that only begins or ends like one is not it */
static void
only_the_five(void)
{
	static const char *const ones[] = { "lsd", "installd", "pkd", "fileproviderd", "containermanagerd" };
	static const char *const others[] = { "SpringBoard", "launchd", "xpcproxy", "lsdx", "ls", "", "installd2", "Lsd", "pk" };

	for (unsigned i = 0; i < sizeof(ones) / sizeof(ones[0]); i++) {
		int32_t active = 20, inactive = 20;

		CHECK(memory_floor_raise(ones[i], 128, &active, &inactive) == 1 && active == 128 && inactive == 128, "%s", ones[i]);
	}
	for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		int32_t active = 20, inactive = 20;

		CHECK(memory_floor_raise(others[i], 128, &active, &inactive) == 0 && active == 20 && inactive == 20, "\"%s\" left",
		    others[i]);
	}
	{
		int32_t active = 20, inactive = 20;

		CHECK(memory_floor_raise(NULL, 128, &active, &inactive) == 0 && active == 20, "no name: left");
	}
}

/* a limit of the system's (-1, or 0) is no limit of its own, and stays: the floor would only lower it */
static void
system_limit_left(void)
{
	int32_t active = -1, inactive = 0;

	CHECK(memory_floor_raise("lsd", 128, &active, &inactive) == 0 && active == -1 && inactive == 0, "%d and %d", active, inactive);
	active = -1;
	inactive = 12;
	CHECK(memory_floor_raise("lsd", 128, &active, &inactive) == 1 && active == -1 && inactive == 128, "only the inactive one: %d and %d",
	    active, inactive);
}

/* a limit at the floor or above it is the one launchd gave: never lowered */
static void
higher_limit_left(void)
{
	int32_t active = 128, inactive = 300;

	CHECK(memory_floor_raise("installd", 128, &active, &inactive) == 0 && active == 128 && inactive == 300, "%d and %d", active, inactive);
	active = 127;
	CHECK(memory_floor_raise("installd", 128, &active, &inactive) == 1 && active == 128 && inactive == 300, "%d and %d", active, inactive);
}

/* memfloor=0, or a number that does not fit (taken as negative): nothing is raised */
static void
no_floor(void)
{
	int32_t active = 16, inactive = 12;

	CHECK(memory_floor_raise("lsd", 0, &active, &inactive) == 0 && active == 16 && inactive == 12, "memfloor=0");
	CHECK(memory_floor_raise("lsd", (int32_t)4000000000u, &active, &inactive) == 0 && active == 16 && inactive == 12,
	    "a floor that does not fit");
	CHECK(memory_floor_raise("lsd", 1, &active, &inactive) == 0 && active == 16, "a floor under the limits");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "lsd_raised", lsd_raised }, { "only_the_five", only_the_five }, { "system_limit_left", system_limit_left },
		{ "higher_limit_left", higher_limit_left }, { "no_floor", no_floor },
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
