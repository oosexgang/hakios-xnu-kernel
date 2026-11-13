/*
 * The CPU performance control over SCMI (kernel/overlay/bsd/dev/board/scmi_perf.c), run as a host program.
 * The driver asks the firmware, through a channel in shared memory and a doorbell, for its version, its
 * domains and each one's level and limits, and sets levels only with scmi_perf=2. The model is that firmware:
 * a thread that answers what the shared memory asks when the doorbell rings, notes every message, and
 * answers wrongly, late or not at all when a scenario says so.
 *
 *   scmi_perf_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the channel: its shared memory and the mailbox's doorbell -------------------------------------- */

#define SHMEM_PA        0x80a40000ul            /* (workbench/devices/phone-keys/soc.dtsi) */
#define MAILBOX_PA      0x1c900000ul
#define DOORBELL        0x208

static _Alignas(64) volatile uint32_t shmem[0x80 / 4];
static _Alignas(64) volatile uint32_t mailbox[0x1000 / 4];
static unsigned mapped_elsewhere;

vm_offset_t
ml_io_map(vm_offset_t phys, vm_size_t size)
{
	if (phys == SHMEM_PA && size <= sizeof(shmem)) {
		return (vm_offset_t)shmem;
	}
	if (phys == MAILBOX_PA && size <= sizeof(mailbox)) {
		return (vm_offset_t)mailbox;
	}
	mapped_elsewhere++;
	fprintf(stderr, "MODEL: %#lx mapped, which is not the channel\n", (unsigned long)phys);
	return 0;
}

/* what the firmware was asked, in order: the message ids of the performance protocol */
static volatile unsigned asked[64], asked_n, rings;
/* how the firmware answers */
enum { ANSWERS, SILENT, WITH_ERROR, OTHER_TOKEN, TOO_LONG, REFUSES };
static volatile int behaviour = ANSWERS;
static volatile uint32_t domains = 3;
static volatile uint32_t levels[4] = { 1000, 1001, 1002, 1003 };
static unsigned sets;
static bool missing_level;

static void
answer(uint32_t status, const uint32_t *words, unsigned n)
{
	shmem[0x1c / 4] = status;
	for (unsigned i = 0; i < n; i++) {
		shmem[0x20 / 4 + i] = words[i];
	}
	shmem[0x14 / 4] = 8 + 4 * n;            /* the header, the status and the words */
}

static void *
firmware(void *unused __unused)
{
	for (;;) {
		uint32_t header, message, protocol, in;

		while (mailbox[DOORBELL / 4] == 0) {
			usleep(20);
		}
		mailbox[DOORBELL / 4] = 0;
		rings++;
		header = shmem[0x18 / 4];
		message = header & 0xff;
		protocol = header >> 10 & 0xff;
		in = shmem[0x1c / 4];
		if (asked_n < 64) {
			asked[asked_n] = protocol << 8 | message;
		}
		asked_n++;
		if (behaviour == SILENT) {
			continue;
		}
		if (behaviour == OTHER_TOKEN) {
			shmem[0x18 / 4] = header + (1u << 18);
		}
		if (behaviour == REFUSES) {
			answer((uint32_t)-1, NULL, 0);          /* SCMI_NOT_SUPPORTED */
		} else if (message == 0x0) {
			uint32_t version = 0x00040000;  /* 4.0 */

			answer(0, &version, 1);
		} else if (message == 0x1) {
			uint32_t attributes = domains;

			answer(0, &attributes, 1);
		} else if (message == 0x8) {
			uint32_t level = levels[in];

			answer(0, &level, missing_level && in == 1 ? 0 : 1);
		} else if (message == 0x7) {
			levels[in] = shmem[0x20 / 4];
			sets++;
			answer(0, NULL, 0);
		} else if (message == 0x3) {
			/* set limits, set level, fast channels, level indexing; rate limit; sustained kHz and level */
			uint32_t attributes[4] = { 0xea000000u, 0, 2000000 + in, 20 + in };

			answer(0, attributes, 4);
		} else if (message == 0x6) {
			uint32_t limits[2] = { 3000 + in, 300 + in };   /* max, min */

			answer(0, limits, 2);
		} else {
			answer((uint32_t)-1, NULL, 0);
		}
		if (behaviour == TOO_LONG) {
			shmem[0x14 / 4] = 0x200;
		}
		shmem[0x04 / 4] = behaviour == WITH_ERROR ? 3 : 1;     /* the channel free again, with an error if so */
	}
	return NULL;
}

static void
channel(void)
{
	pthread_t thread;

	board = "phone-keys";
	fake_delay_divisor = 10;
	shmem[0x04 / 4] = 1;                    /* free */
	pthread_create(&thread, NULL, firmware, NULL);
}

#pragma GCC diagnostic ignored "-Wformat"
#include "board/board.c"
#include "board/scmi_perf.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* every message the firmware was asked is one that reads: version, attributes, a level, limits -- never a set */
static void
only_reads(void)
{
	unsigned n = asked_n < 64 ? asked_n : 64;

	for (unsigned i = 0; i < n; i++) {
		unsigned protocol = asked[i] >> 8, message = asked[i] & 0xff;

		CHECK(protocol == 0x13, "message %u of the performance protocol, not of %#x", i, protocol);
		CHECK(message == 0x0 || message == 0x1 || message == 0x3 || message == 0x6 || message == 0x8,
		    "message %u is %#x: not one that only reads", i,
		    message);
	}
}

/* ---- scenarios -------------------------------------------------------------------------------------- */

/* the version, the domains, and each one's level and limits, said; nothing set */
static void
reads_and_says(void)
{
	channel();
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("scmi-perf: SCMI performance 4.0, 3 domains"), "said: %s", log_text);
	CHECK(logged("scmi-perf: domain 0 current level 1000") && logged("scmi-perf: domain 2 current level 1002"), "each level: %s", log_text);
	CHECK(logged("scmi-perf: domain 1 limits 301..3001"), "and limits, lowest first: %s", log_text);
	CHECK(logged("scmi-perf: domain 1 attributes 0xea000000: set limits 1, set level 1, fast channels 1, level indexing 1, "
	    "sustained 2000001 kHz at level 21"), "each one's attributes: %s", log_text);
	CHECK(asked_n == 2 + 3 * 3 && rings == asked_n, "two questions, then three a domain: %u", asked_n);
	only_reads();
	CHECK(mapped_elsewhere == 0, "nothing but the channel mapped");
}

/* no more than four domains are asked about, however many the firmware says it has */
static void
four_domains_at_most(void)
{
	channel();
	domains = 0xffff;
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("4 domains") && asked_n == 2 + 4 * 3, "four: %u questions: %s", asked_n, log_text);
	only_reads();
}

/* a firmware that does not answer, answers with an error, with another's token, refuses, or answers longer than
 * the shared memory: nothing is believed */
static void
does_not_answer(void)
{
	static const int ways[] = { SILENT, WITH_ERROR, OTHER_TOKEN, REFUSES, TOO_LONG };
	const char *which = getenv("SCMI_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;

	channel();
	behaviour = ways[i];
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("scmi-perf: SCMI performance protocol unavailable"), "case %u: said: %s", i, log_text);
	CHECK(!logged("current level") && !logged("domains"), "case %u: and nothing it answered taken: %s", i, log_text);
	CHECK(asked_n == 1, "case %u: and nothing more asked: %u", i, asked_n);
	only_reads();
}

/* only when asked for, on a board that has the channel and lists the feature */
static void
only_when_asked(void)
{
	channel();
	boot_args = "";
	hakios_scmi_perf_init();
	boot_args = "scmi_perf=3";
	hakios_scmi_perf_init();
	CHECK(asked_n == 0 && log_text[0] == 0, "not without scmi_perf=1 or 2: %s", log_text);
}

/* mode 2 raises each complete domain to its allowed top, keeping the found level as its floor */
static void
raises_when_asked(void)
{
	channel();
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(sets == 3, "one set per domain: %u", sets);
	for (unsigned d = 0; d < 3; d++) {
		uint32_t found = 0, top = 0;

		CHECK(hakios_scmi_perf_domain(d, &found, &top) && found == 1000 + d && top == 3000 + d,
		    "domain %u keeps its found level and top", d);
		CHECK(levels[d] == top, "domain %u raised: %u", d, levels[d]);
	}
}

/* thermal requests are bounded by the firmware top; a refused request never changes the cached level */
static void
sets_are_clamped_and_errors_preserve_state(void)
{
	channel();
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(sets == 0, "mode 1 only reads");
	CHECK(hakios_scmi_perf_set(0, UINT32_MAX) == 0 && levels[0] == 3000, "clamped to the top");
	CHECK(hakios_scmi_perf_set(0, 1200) == 0 && levels[0] == 1200, "a lower thermal level accepted");
	behaviour = REFUSES;
	CHECK(hakios_scmi_perf_set(0, 1300) == -1 && scmi_now[0] == 1200 && levels[0] == 1200,
	    "a refusal leaves the level as it was");
	unsigned before = asked_n;
	uint32_t found, top;

	CHECK(hakios_scmi_perf_set(3, 1000) == -1 && asked_n == before, "no message for an unknown domain");
	CHECK(!hakios_scmi_perf_domain(3, &found, &top), "an unknown domain has no levels");
}

/* an incomplete reply leaves a gap: later domains cannot be renumbered into its place */
static void
incomplete_domains_are_not_exposed(void)
{
	channel();
	missing_level = true;
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	uint32_t found, top;

	CHECK(hakios_scmi_perf_domain(0, &found, &top), "the complete prefix is usable");
	CHECK(!hakios_scmi_perf_domain(1, &found, &top) && !hakios_scmi_perf_domain(2, &found, &top),
	    "no gap, and no domain shifted over it");
	CHECK(sets == 1 && levels[1] == 1001 && levels[2] == 1002, "only the complete prefix raised");
}

/* (the board is looked up once a process: another board is another scenario) */
static void
board_without_it(void)
{
	channel();
	board = "phone-video";
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(asked_n == 0 && log_text[0] == 0 && mapped_elsewhere == 0, "nothing on a board that does not list it: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); unsigned cases; } scenarios[] = {
		{ "reads_and_says", reads_and_says, 1 }, { "four_domains_at_most", four_domains_at_most, 1 },
		{ "does_not_answer", does_not_answer, 5 }, { "only_when_asked", only_when_asked, 1 },
		{ "board_without_it", board_without_it, 1 },
		{ "raises_when_asked", raises_when_asked, 1 },
		{ "sets_are_clamped_and_errors_preserve_state", sets_are_clamped_and_errors_preserve_state, 1 },
		{ "incomplete_domains_are_not_exposed", incomplete_domains_are_not_exposed, 1 },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			for (unsigned c = 0; c < scenarios[i].cases; c++) {
				if (scenarios[i].cases == 1) {
					printf("%s\n", scenarios[i].name);
				} else {
					printf("%s:%u\n", scenarios[i].name, c);
				}
			}
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		size_t length = strlen(scenarios[i].name);

		if (strncmp(argv[1], scenarios[i].name, length) == 0 && (argv[1][length] == 0 || argv[1][length] == ':')) {
			if (argv[1][length] == ':') {
				setenv("SCMI_CASE", argv[1] + length + 1, 1);
			}
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
