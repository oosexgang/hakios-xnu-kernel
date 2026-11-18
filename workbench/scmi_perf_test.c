/*
 * The CPU performance control over SCMI (drivers/board/scmi_perf.c), run as a host program.
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
static volatile unsigned doorbell_at = DOORBELL;        /* where the firmware listens: the board's doorbell */
static bool mailbox_unmapped;           /* the mailbox's mapping fails */

vm_offset_t
ml_io_map(vm_offset_t phys, vm_size_t size)
{
	if (phys == SHMEM_PA && size <= sizeof(shmem)) {
		return (vm_offset_t)shmem;
	}
	if (phys == MAILBOX_PA && size <= sizeof(mailbox) && !mailbox_unmapped) {
		return (vm_offset_t)mailbox;
	}
	mapped_elsewhere++;
	fprintf(stderr, "MODEL: %#lx mapped, which is not the channel\n", (unsigned long)phys);
	return 0;
}

/* what the firmware was asked, in order: the message ids of the performance protocol */
static volatile unsigned asked[64], asked_n, rings;
/* how the firmware answers */
enum { ANSWERS, SILENT, WITH_ERROR, OTHER_TOKEN, TOO_LONG, REFUSES, ONE_WORD_TOO_LONG, THE_WHOLE_MEMORY };
static volatile int behaviour = ANSWERS;
static volatile uint32_t domains = 3;
static volatile uint32_t levels[4] = { 1000, 1001, 1002, 1003 };
static unsigned sets;
static bool missing_level;
static int short_of = -1;               /* a message answered with one word fewer than it should be (0x3: none) */
static int top_at_level = -1;           /* a domain whose limits' top is its level now, or that less one */
static int top_below_level = -1;
static unsigned attribute_words = 4;    /* the words a domain's attributes are answered with */
static unsigned extra_words;            /* words more than asked for, after each answer that has any */
static bool level_fails_after_a_set;    /* a level asked for once one has been set: an error, with a word all the same */
static int refuse_first_set_of = -1;    /* a domain whose first level set is refused (and the level left) */
static int not_indexed = -1;            /* a domain whose attributes say no level indexing */

static void
answer(uint32_t status, const uint32_t *words, unsigned n)
{
	shmem[0x1c / 4] = status;
	for (unsigned i = 0; i < n; i++) {
		shmem[0x20 / 4 + i] = words[i];
	}
	shmem[0x14 / 4] = 8 + 4 * n;            /* the header, the status and the words */
	for (unsigned i = 0; n > 0 && i < extra_words; i++) {
		shmem[0x20 / 4 + n + i] = 0xdead0000u + i;
		shmem[0x14 / 4] += 4;
	}
	if (n > 0 && (int)(shmem[0x18 / 4] & 0xff) == short_of) {
		shmem[0x14 / 4] = short_of == 0x3 ? 8 : shmem[0x14 / 4] - 4;  /* (a domain's attributes: none at all) */
	}
}

static void *
firmware(void *unused __unused)
{
	for (;;) {
		uint32_t header, message, protocol, in;

		while (mailbox[doorbell_at / 4] == 0) {
			usleep(20);
		}
		mailbox[doorbell_at / 4] = 0;
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

			answer(level_fails_after_a_set && sets > 0 ? (uint32_t)-3 : 0, &level, missing_level && in == 1 ? 0 : 1);
		} else if (message == 0x7 && (int)in == refuse_first_set_of) {
			refuse_first_set_of = -1;
			answer((uint32_t)-6, NULL, 0);          /* SCMI_BUSY */
		} else if (message == 0x7) {
			levels[in] = shmem[0x20 / 4];
			sets++;
			answer(0, NULL, 0);
		} else if (message == 0x3) {
			/* set limits, set level, fast channels, level indexing; rate limit; sustained kHz and level */
			uint32_t attributes[4] = { (int)in == not_indexed ? 0xe8000000u : 0xea000000u, 0, 2000000 + in, 20 + in };

			answer(0, attributes, attribute_words);
		} else if (message == 0x6) {
			uint32_t limits[2] = { 3000 + in, 300 + in };   /* max, min */

			if ((int)in == top_at_level || (int)in == top_below_level) {
				limits[0] = levels[in] - ((int)in == top_below_level);
			}

			answer(0, limits, 2);
		} else {
			answer((uint32_t)-1, NULL, 0);
		}
		if (behaviour == TOO_LONG) {
			shmem[0x14 / 4] = 0x200;
		}
		if (behaviour == ONE_WORD_TOO_LONG || behaviour == THE_WHOLE_MEMORY) {
			/* (the shared memory is 0x80 bytes; the length counts from 0x18: 0x68 fills it) */
			shmem[0x14 / 4] = behaviour == THE_WHOLE_MEMORY ? 0x68 : 0x6c;
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
#include "board/thermal_core.h"

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
	static const int ways[] = { SILENT, WITH_ERROR, OTHER_TOKEN, REFUSES, TOO_LONG, ONE_WORD_TOO_LONG };
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

/* an answer as long as the shared memory holds: taken */
static void
answers_fill_the_memory(void)
{
	channel();
	behaviour = THE_WHOLE_MEMORY;
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("scmi-perf: SCMI performance 4.0, 3 domains") && logged("domain 2 limits 302..3002"), "taken: %s", log_text);
}

/* a channel the board describes wrong (no shared memory, no mailbox, a doorbell past the mailbox's page or not on a
 * word): said, nothing mapped, nothing asked */
static void
channel_described_wrong(void)
{
	static struct hakios_board copy;
	const char *which = getenv("SCMI_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;

	channel();
	copy = *hakios_board();
	switch (i) {
	case 0: copy.scmi_perf.shmem = 0; break;
	case 1: copy.scmi_perf.mailbox = 0; break;
	case 2: copy.scmi_perf.doorbell = 0x1000; break;
	default: copy.scmi_perf.doorbell = DOORBELL + 2; break;
	}
	found = &copy;
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("scmi-perf: invalid channel description") && asked_n == 0 && rings == 0 && mapped_elsewhere == 0,
	    "case %u: said, nothing asked: %s", i, log_text);
}

/* the doorbell in the mailbox page's last word: a doorbell like another */
static void
doorbell_in_the_last_word(void)
{
	static struct hakios_board copy;

	channel();
	copy = *hakios_board();
	copy.scmi_perf.doorbell = 0x1000 - 4;
	doorbell_at = 0x1000 - 4;
	found = &copy;
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("scmi-perf: SCMI performance 4.0, 3 domains") && rings == asked_n && asked_n > 0, "rung there: %s", log_text);
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
	boot_args = "scmi_perf=0";
	hakios_scmi_perf_init();
	CHECK(asked_n == 0 && log_text[0] == 0, "not without scmi_perf=1 or 2 (none, 3, 0): %s", log_text);
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
	CHECK(logged("scmi-perf: domain 0 asked for level 3000: status 0, now 3000") &&
	    logged("scmi-perf: domain 2 asked for level 3002: status 0, now 3002"), "each set said, with the level read back: %s", log_text);
	CHECK(!logged("domain 3 asked") && !logged("status -1"), "and no more than the domains there are: %s", log_text);
	/* the run's end: each back where it was found, for the loader and the next kernel (which takes the level it finds) */
	hakios_scmi_perf_quiesce();
	CHECK(sets == 6 && levels[0] == 1000 && levels[1] == 1001 && levels[2] == 1002, "at the end: %u sets, levels %u %u %u", sets,
	    levels[0], levels[1], levels[2]);
	CHECK(logged("scmi-perf: at the run's end: domain 2 back at level 1002: status 0"), "said: %s", log_text);
	hakios_scmi_perf_quiesce();
	CHECK(sets == 6, "and once only: %u sets", sets);
}

/* scmi_perf=1 only looks: nothing raised, so nothing is asked at the end either */
static void
nothing_back_when_only_looked(void)
{
	channel();
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	hakios_scmi_perf_quiesce();
	CHECK(sets == 0, "no set: %u", sets);
}

/* a level read back with an error after the set: said as none (-1), not the word that came with the error */
static void
read_back_fails(void)
{
	channel();
	level_fails_after_a_set = true;
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(sets == 3 && logged("scmi-perf: domain 0 asked for level 3000: status 0, now -1"), "said: %s", log_text);
}

/* a channel the firmware never frees: nothing written into it, the doorbell not rung */
static void
channel_never_free(void)
{
	channel();
	shmem[0x04 / 4] = 0;
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("scmi-perf: SCMI performance protocol unavailable") && rings == 0 && asked_n == 0, "%u rings: %s", rings, log_text);
}

/* the mailbox cannot be mapped: said, and nothing asked */
static void
mailbox_not_mapped(void)
{
	channel();
	mailbox_unmapped = true;
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("scmi-perf: cannot map SCMI channel") && asked_n == 0, "said: %s", log_text);
}

/* answers longer than what was asked for: what was asked for is taken, nothing past it written */
static void
longer_answers(void)
{
	uint32_t found = 0, top = 0;

	channel();
	extra_words = 3;
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(logged("SCMI performance 4.0, 3 domains") && hakios_scmi_perf_domain(2, &found, &top) && found == 1002 && top == 3002,
	    "taken as they are: %s", log_text);
	CHECK(logged("domain 1 asked for level 3001: status 0, now 3001"), "%s", log_text);
}

/* a domain's attributes in one word: said, the rest as none */
static void
one_attribute_word(void)
{
	channel();
	attribute_words = 1;
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	CHECK(logged("domain 0 attributes 0xea000000: set limits 1, set level 1, fast channels 1, level indexing 1, sustained 0 kHz at "
	    "level 0"), "said: %s", log_text);
}

/* thermal requests are bounded by the firmware top; a refused request never changes the cached level */
static void
sets_are_clamped_and_errors_preserve_state(void)
{
	channel();
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(sets == 3, "each raised: %u", sets);
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

/* a level the firmware took but whose answer could not be read: put back at the run's end all the same */
static void
taken_without_an_answer(void)
{
	channel();
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	behaviour = OTHER_TOKEN;                /* (the firmware sets the level; its answer carries another token) */
	CHECK(hakios_scmi_perf_set(0, 2500) == -103 && levels[0] == 2500, "the level taken, the answer not ours");
	behaviour = ANSWERS;
	hakios_scmi_perf_quiesce();
	CHECK(levels[0] == 1000, "back at the level found at the run's end: %u", levels[0]);
	CHECK(levels[1] == 1001 && levels[2] == 1002, "and the others, raised, back too: %u %u", levels[1], levels[2]);
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

/* answers one word short: of the version or the attributes, no protocol; of a level or the limits, not that domain;
 * a domain's attributes with none: not said, and with no level indexing known the domain not handed on either */
static void
short_answers(void)
{
	const char *which = getenv("SCMI_CASE");
	unsigned i = which ? (unsigned)atoi(which) : 0;
	static const int messages[] = { 0x0, 0x1, 0x8, 0x6, 0x3 };
	uint32_t found, top;

	channel();
	short_of = messages[i];
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	if (i < 2) {
		CHECK(logged("SCMI performance protocol unavailable") && !hakios_scmi_perf_domain(0, &found, &top), "case %u: none: %s", i,
		    log_text);
	} else if (i < 4) {
		CHECK(!hakios_scmi_perf_domain(0, &found, &top), "case %u: the first domain not handed on: %s", i, log_text);
	} else {
		CHECK(!hakios_scmi_perf_domain(0, &found, &top) && !logged("domain 0 attributes") && sets == 0 &&
		    logged("domain 0: its levels are not known to be steps"), "case %u: no attributes said, the domain left: %s", i, log_text);
	}
}

/* a domain whose limits' top is its level now is one; one whose top is under it is not */
static void
top_at_the_level(void)
{
	uint32_t found = 0, top = 0;

	channel();
	top_at_level = 0;
	top_below_level = 1;
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(hakios_scmi_perf_domain(0, &found, &top) && found == 1000 && top == 1000, "the top at the level: %u, %u", found, top);
	CHECK(!hakios_scmi_perf_domain(1, &found, &top), "a top under the level: not handed on");
}

/* scmi_perf=1 only looks: thermal=2 is given no domain, and a level it asked for all the same is not set */
static void
only_looked_none_handed_on(void)
{
	uint32_t found = 0, top = 0;

	channel();
	boot_args = "scmi_perf=1";
	hakios_scmi_perf_init();
	for (unsigned d = 0; d < 3; d++) {
		CHECK(!hakios_scmi_perf_domain(d, &found, &top), "domain %u not handed on: %u..%u", d, found, top);
	}
	unsigned before = asked_n;

	CHECK(hakios_scmi_perf_set(0, 2999) == -1 && asked_n == before && sets == 0 && levels[0] == 1000,
	    "a set refused, nothing asked: %u sets, level %u", sets, levels[0]);
	only_reads();
}

/* scmi_perf=2, a domain whose raise the firmware refused: thermal=2 takes it as at its top, and its first step must not
 * raise it -- its top is where it was found */
static void
failed_raise_not_raised_by_thermal(void)
{
	uint32_t found = 0, top = 0;

	channel();
	refuse_first_set_of = 1;
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(levels[0] == 3000 && levels[1] == 1001 && levels[2] == 3002, "the others raised: %u %u %u", levels[0], levels[1], levels[2]);
	CHECK(hakios_scmi_perf_domain(0, &found, &top) && top == 3000, "a raised one handed on with its top: %u", top);
	if (hakios_scmi_perf_domain(1, &found, &top)) {
		CHECK(top == found && found == 1001, "the one not raised: its top the level found: %u..%u", found, top);
		/* (thermal's first step, as board/thermal.c asks it) */
		hakios_scmi_perf_set(1, th_row(top, found, 1));
	}
	CHECK(levels[1] == 1001, "thermal's first step leaves it where it was found: %u", levels[1]);
	CHECK(hakios_scmi_perf_domain(2, &found, &top) && top == 3002, "and the one after it handed on all the same: %u", top);
}

/* thermal asks for levels down to its floor: below the level found is clamped to it */
static void
set_below_the_floor_clamped(void)
{
	channel();
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(hakios_scmi_perf_set(0, 0) == 0 && levels[0] == 1000, "clamped to the level found: %u", levels[0]);
	CHECK(hakios_scmi_perf_set(2, 999) == 0 && levels[2] == 1002, "and just under it: %u", levels[2]);
	CHECK(hakios_scmi_perf_set(1, 1500) == 0 && levels[1] == 1500, "between the two, as asked: %u", levels[1]);
}

/* a domain without level indexing: its levels are not steps thermal can count down; not raised, not handed on, and
 * the ones after it neither (no gap in the numbering) */
static void
not_indexed_not_handed_on(void)
{
	uint32_t found = 0, top = 0;

	channel();
	not_indexed = 1;
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(hakios_scmi_perf_domain(0, &found, &top) && levels[0] == 3000, "the indexed one before it as before");
	CHECK(!hakios_scmi_perf_domain(1, &found, &top) && !hakios_scmi_perf_domain(2, &found, &top), "it and the next not handed on");
	CHECK(sets == 1 && levels[1] == 1001 && levels[2] == 1002, "nor raised: %u sets", sets);
	CHECK(logged("scmi-perf: domain 1: its levels are not known to be steps (no level indexing)"), "said why: %s", log_text);
}

/* the run's end came while a core was between taking the channel and ringing for its message (the free bit clear, the
 * lock held): the doorbell rung for it once, and each domain put back all the same */
static void
quiesce_with_the_channel_left_busy(void)
{
	channel();
	boot_args = "scmi_perf=2";
	hakios_scmi_perf_init();
	CHECK(levels[0] == 3000 && levels[2] == 3002, "raised");
	unsigned rung = rings;

	lck_mtx_lock(&scmi_lock);
	/* (what scmi_query() had written: a level of domain 0, then the channel taken; the doorbell not rung) */
	shmem[0x10 / 4] = 0;
	shmem[0x14 / 4] = 12;
	shmem[0x18 / 4] = ((scmi_token + 1) & 0x3ff) << 18 | 0x13 << 10 | 0x7;
	shmem[0x1c / 4] = 0;
	shmem[0x20 / 4] = 2000;
	shmem[0x04 / 4] = 0;
	hakios_scmi_perf_quiesce();
	lck_mtx_unlock(&scmi_lock);
	CHECK(levels[0] == 1000 && levels[1] == 1001 && levels[2] == 1002, "each back where it was found: %u %u %u", levels[0], levels[1],
	    levels[2]);
	CHECK(rings == rung + 4 && logged("never rung for: rung"), "rung once for it, then once a domain: %u rings: %s", rings - rung,
	    log_text);
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
		{ "does_not_answer", does_not_answer, 6 }, { "only_when_asked", only_when_asked, 1 },
		{ "answers_fill_the_memory", answers_fill_the_memory, 1 }, { "channel_described_wrong", channel_described_wrong, 4 },
		{ "doorbell_in_the_last_word", doorbell_in_the_last_word, 1 }, { "read_back_fails", read_back_fails, 1 },
		{ "board_without_it", board_without_it, 1 },
		{ "raises_when_asked", raises_when_asked, 1 }, { "nothing_back_when_only_looked", nothing_back_when_only_looked, 1 }, { "channel_never_free", channel_never_free, 1 },
		{ "mailbox_not_mapped", mailbox_not_mapped, 1 }, { "longer_answers", longer_answers, 1 },
		{ "one_attribute_word", one_attribute_word, 1 },
		{ "sets_are_clamped_and_errors_preserve_state", sets_are_clamped_and_errors_preserve_state, 1 },
		{ "taken_without_an_answer", taken_without_an_answer, 1 },
		{ "incomplete_domains_are_not_exposed", incomplete_domains_are_not_exposed, 1 },
		{ "short_answers", short_answers, 5 }, { "top_at_the_level", top_at_the_level, 1 },
		{ "only_looked_none_handed_on", only_looked_none_handed_on, 1 },
		{ "failed_raise_not_raised_by_thermal", failed_raise_not_raised_by_thermal, 1 },
		{ "set_below_the_floor_clamped", set_below_the_floor_clamped, 1 },
		{ "not_indexed_not_handed_on", not_indexed_not_handed_on, 1 },
		{ "quiesce_with_the_channel_left_busy", quiesce_with_the_channel_left_busy, 1 },
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
