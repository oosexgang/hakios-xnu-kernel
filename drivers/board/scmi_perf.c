/*
 * SM8845 CPU performance over SCMI.
 * Transport adapted from hakios-sm8845 (805567b), macan power driver.
 * The firmware owns the clocks and its own (LMh) limits; it clamps every level asked for to them.
 *
 *   scmi_perf=1   the levels and limits are logged, nothing changed (thermal=2 is given no domain)
 *   scmi_perf=2   each domain is also asked for its highest level (the UI is drawn by the CPUs);
 *                 the level it was found at stays the floor that thermal=2 holds it back to
 *
 * Each domain's attributes (PERFORMANCE_DOMAIN_ATTRIBUTES) are logged: whether this agent may set its
 * level and its limits, whether it has fast channels, its sustained frequency.
 *
 * board/thermal.c uses hakios_scmi_perf_domain() and hakios_scmi_perf_set() when the SoC has no EPSS.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#include "kernel.h"
#include "cache.h"
#include "board.h"
#include "log.h"
#include <sys/systm.h>

#define SCMI_SHMEM_SIZE 0x80UL
#define SCMI_MAILBOX_SIZE 0x1000UL
#define SCMI_WORDS        ((SCMI_SHMEM_SIZE - 0x1c) / 4)
#define SCMI_PERF               0x13
#define SCMI_VERSION            0x0
#define SCMI_ATTRIBUTES         0x1
#define SCMI_PERF_DOMAIN_ATTRIBUTES 0x3
#define SCMI_PERF_LIMITS_GET    0x6
#define SCMI_PERF_LEVEL_SET     0x7
#define SCMI_PERF_LEVEL_GET     0x8
#define SCMI_DOMAINS_MAX  4

static vm_offset_t scmi_shmem, scmi_mailbox;
static uint32_t scmi_token, scmi_doorbell;
static unsigned scmi_domains;                   /* domains with their level, limits and level indexing known */
static unsigned scmi_handed;                    /* of those, the ones thermal=2 may set: scmi_perf=2's, once raised */
static uint32_t scmi_found[SCMI_DOMAINS_MAX], scmi_top[SCMI_DOMAINS_MAX], scmi_now[SCMI_DOMAINS_MAX];
static uint32_t scmi_floor[SCMI_DOMAINS_MAX];   /* the level found, or the firmware's lowest if that is higher */
static LCK_GRP_DECLARE(scmi_lock_group, "hakios-scmi-perf");
static LCK_MTX_DECLARE(scmi_lock, &scmi_lock_group);   /* one message on the channel at a time */

static volatile uint32_t *
scmi_word(unsigned offset)
{
	return (volatile uint32_t *)(scmi_shmem + offset);
}

static void
scmi_ring(void)
{
	hakios_barrier();
	*(volatile uint32_t *)(scmi_mailbox + scmi_doorbell) = 1;
	hakios_barrier();
}

/*
 * One SCMI message of the performance protocol. Returns the SCMI status (0: success), or -100 and
 * below when the channel did not answer; `out` gets the answer's words after the status. -100: nothing
 * was sent (the channel was not free); -101 and below: sent, and the answer not there or not readable.
 */
static int
scmi_query(uint32_t message, const uint32_t *in, unsigned nin, uint32_t *out, unsigned nout, unsigned *nanswer)
{
	uint32_t header, length;
	unsigned i, words;

	for (i = 0; i < 1000 && !(*scmi_word(0x4) & 1); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	if (nin > SCMI_WORDS || !(*scmi_word(0x4) & 1)) {
		return -100;
	}
	header = ((++scmi_token & 0x3ff) << 18) | (SCMI_PERF << 10) | message;
	*scmi_word(0x10) = 0;
	*scmi_word(0x14) = 4 + 4 * nin;
	*scmi_word(0x18) = header;
	for (i = 0; i < nin; i++) {
		*scmi_word(0x1c + 4 * i) = in[i];
	}
	hakios_barrier();
	*scmi_word(0x4) = 0;
	scmi_ring();
	for (i = 0; i < 5000 && !(*scmi_word(0x4) & 1); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	if (!(*scmi_word(0x4) & 1)) {
		return -101;
	}
	if (*scmi_word(0x4) & 2) {
		return -102;
	}
	if (*scmi_word(0x18) != header) {
		return -103;
	}
	length = *scmi_word(0x14);
	if (length < 8 || length > SCMI_SHMEM_SIZE - 0x18 || (length & 3)) {
		return -104;
	}
	words = (length - 8) / 4;
	for (i = 0; i < nout && i < words; i++) {
		out[i] = *scmi_word(0x20 + 4 * i);
	}
	if (nanswer != NULL) {
		*nanswer = words;
	}
	return (int)*scmi_word(0x1c);
}

void hakios_scmi_perf_init(void);
int hakios_scmi_perf_domain(unsigned domain, uint32_t *found, uint32_t *top);
int hakios_scmi_perf_set(unsigned domain, uint32_t level);
static int scmi_set(unsigned domain, uint32_t level);

void
hakios_scmi_perf_init(void)
{
	const struct hakios_board *board = hakios_board();
	uint32_t version, attributes, enabled = 0;
	unsigned words;

	if (!hakios_board_has(HAKIOS_HAS_CPUFREQ_SCMI) ||
	    !PE_parse_boot_argn("scmi_perf", &enabled, sizeof(enabled)) || enabled < 1 || enabled > 2) {
		return;
	}
	scmi_doorbell = board->scmi_perf.doorbell;
	if (!board->scmi_perf.shmem || !board->scmi_perf.mailbox ||
	    scmi_doorbell > SCMI_MAILBOX_SIZE - sizeof(uint32_t) || (scmi_doorbell & 3)) {
		hakios_say("scmi-perf: invalid channel description\n");
		return;
	}
	scmi_shmem = ml_io_map(board->scmi_perf.shmem, SCMI_SHMEM_SIZE);
	scmi_mailbox = ml_io_map(board->scmi_perf.mailbox, SCMI_MAILBOX_SIZE);
	if (!scmi_shmem || !scmi_mailbox) {
		hakios_say("scmi-perf: cannot map SCMI channel\n");
		return;
	}
	if (scmi_query(SCMI_VERSION, NULL, 0, &version, 1, &words) != 0 || words < 1 ||
	    scmi_query(SCMI_ATTRIBUTES, NULL, 0, &attributes, 1, &words) != 0 || words < 1) {
		hakios_say("scmi-perf: SCMI performance protocol unavailable\n");
		return;
	}

	uint32_t domains = MIN(attributes & 0xffff, SCMI_DOMAINS_MAX);
	hakios_say("scmi-perf: SCMI performance %u.%u, %u domains\n",
	    version >> 16, version & 0xffff, domains);
	for (uint32_t domain = 0; domain < domains; domain++) {
		uint32_t level = 0, limits[2] = { 0, 0 }, attr[4];
		bool have_level, have_limits, indexed = false;

		/* what this agent may do with the domain, as the firmware says (PERFORMANCE_DOMAIN_ATTRIBUTES) */
		if (scmi_query(SCMI_PERF_DOMAIN_ATTRIBUTES, &domain, 1, attr, 4, &words) == 0 && words >= 1) {
			hakios_say("scmi-perf: domain %u attributes %#x: set limits %u, set level %u, fast channels %u, "
			    "level indexing %u, sustained %u kHz at level %u\n", domain, attr[0], attr[0] >> 31, (attr[0] >> 30) & 1,
			    (attr[0] >> 27) & 1, (attr[0] >> 25) & 1, words >= 4 ? attr[2] : 0, words >= 4 ? attr[3] : 0);
			indexed = (attr[0] >> 25) & 1;
		}

		have_level = scmi_query(SCMI_PERF_LEVEL_GET, &domain, 1, &level, 1, &words) == 0 && words >= 1;
		if (have_level) {
			hakios_say("scmi-perf: domain %u current level %u\n", domain, level);
		}
		have_limits = scmi_query(SCMI_PERF_LIMITS_GET, &domain, 1, limits, 2, &words) == 0 && words >= 2;
		if (have_limits) {
			hakios_say("scmi-perf: domain %u limits %u..%u\n", domain, limits[1], limits[0]);
		}
		/* only a prefix of domains that answered both is handed on: a gap would shift the numbering */
		if (!have_level || !have_limits || domain != scmi_domains || limits[0] < level) {
			continue;
		}
		/*
		 * Without level indexing a level is one of the values the firmware lists (PERFORMANCE_DESCRIBE_LEVELS), not
		 * a step: the top less one, the first that thermal=2 would ask for, is none of them. Such a domain is neither
		 * raised nor handed on, and with it the prefix ends.
		 */
		if (!indexed) {
			hakios_say("scmi-perf: domain %u: its levels are not known to be steps (no level indexing): left as found, "
			    "and not held back by thermal\n", domain);
			continue;
		}
		scmi_found[domain] = scmi_now[domain] = level;
		scmi_top[domain] = limits[0];
		scmi_floor[domain] = MAX(level, limits[1]);
		scmi_domains++;
	}
	if (enabled >= 2) {
		for (unsigned domain = 0; domain < scmi_domains; domain++) {
			int status = scmi_set(domain, scmi_top[domain]);
			uint32_t level = 0;

			lck_mtx_lock(&scmi_lock);
			if (scmi_query(SCMI_PERF_LEVEL_GET, &domain, 1, &level, 1, &words) != 0 || words < 1) {
				level = UINT32_MAX;
			}
			lck_mtx_unlock(&scmi_lock);
			hakios_say("scmi-perf: domain %u asked for level %u: status %d, now %d\n", domain,
			    scmi_top[domain], status, (int)level);
			/*
			 * Not raised (or not known to be): thermal=2 starts from the top, and its first step down would ask for
			 * the top less one -- above where the domain is, while the phone is hot. Its top is the level found, so
			 * it is never moved; one the firmware took without an answer is put back at the run's end.
			 */
			if (status != 0) {
				scmi_top[domain] = scmi_found[domain];
			}
		}
		scmi_handed = scmi_domains;
	}
}

/*
 * At the run's end (osfmk/arm64/loader_return.c; the other processors stopped, interrupts off): each domain asked back to
 * the level it was found at. The loader runs a next kernel without a reset: one left at the top finds it there, takes it for
 * where the firmware left it, and thermal=2 has nothing to hold back from it. No lock is taken -- nobody else runs -- and the
 * channel's waits are bounded (scmi_query()).
 */
void
hakios_scmi_perf_quiesce(void)
{
	unsigned domain, i;

	for (domain = 0; domain < scmi_domains && scmi_now[domain] == scmi_found[domain]; domain++) {
	}
	if (domain == scmi_domains) {
		return;
	}
	/*
	 * A core stopped in scmi_query() after the channel's free bit was cleared and before the doorbell was rung leaves a
	 * message the firmware was never told of, and the channel busy for good: every level below would be given up on.
	 * The lock held by nobody who will run again says a message is under way; once the firmware has had as long as it
	 * is given to answer one, the doorbell is rung for it, once, and the answer waited for as long again.
	 */
	if (!(*scmi_word(0x4) & 1)) {
		bool held = !lck_mtx_try_lock(&scmi_lock);

		if (!held) {
			lck_mtx_unlock(&scmi_lock);
		}
		for (i = 0; held && i < 5000 && !(*scmi_word(0x4) & 1); i++) {
			delay_for_interval(10, NSEC_PER_USEC);
		}
		if (held && !(*scmi_word(0x4) & 1)) {
			hakios_say("scmi-perf: at the run's end: the channel busy with a message never rung for: rung\n");
			scmi_ring();
			for (i = 0; i < 5000 && !(*scmi_word(0x4) & 1); i++) {
				delay_for_interval(10, NSEC_PER_USEC);
			}
		}
	}
	for (domain = 0; domain < scmi_domains; domain++) {
		uint32_t in[2] = { domain, scmi_found[domain] };
		unsigned words;
		int status;

		if (scmi_now[domain] == scmi_found[domain]) {
			continue;
		}
		status = scmi_query(SCMI_PERF_LEVEL_SET, in, 2, NULL, 0, &words);
		if (status == 0) {
			scmi_now[domain] = scmi_found[domain];
		}
		hakios_say("scmi-perf: at the run's end: domain %u back at level %u: status %d\n", domain, scmi_found[domain], status);
	}
}

/* A domain's levels for the thermal limiter: 0 when there is no such domain, or none it may set (scmi_perf=1). */
int
hakios_scmi_perf_domain(unsigned domain, uint32_t *found, uint32_t *top)
{
	if (domain >= scmi_handed) {
		return 0;
	}
	*found = scmi_found[domain];
	*top = scmi_top[domain];
	return 1;
}

/* Ask for a level of a domain handed on to thermal (clamped to its floor and its top); the SCMI status, 0: done. */
int
hakios_scmi_perf_set(unsigned domain, uint32_t level)
{
	if (domain >= scmi_handed) {
		return -1;
	}
	return scmi_set(domain, level);
}

static int
scmi_set(unsigned domain, uint32_t level)
{
	uint32_t in[2], was;
	unsigned words;
	int status;

	/* (the top wins over a floor above it: a domain not raised has the level found for its top) */
	level = MIN(MAX(level, scmi_floor[domain]), scmi_top[domain]);
	in[0] = domain;
	in[1] = level;
	lck_mtx_lock(&scmi_lock);
	was = scmi_now[domain];
	/*
	 * (not known from before the doorbell is rung until the answer is read: a core stopped in between -- the way back to
	 * the loader -- leaves the quiesce to put the domain back whatever it is, not take it for the level it was)
	 */
	scmi_now[domain] = UINT32_MAX;
	status = scmi_query(SCMI_PERF_LEVEL_SET, in, 2, NULL, 0, &words);
	if (status == 0) {
		scmi_now[domain] = level;
	} else if (status >= -100) {
		/* (nothing sent, or refused by the firmware: where it was) */
		scmi_now[domain] = was;
	}
	/* (-101 and below: the doorbell was rung and no answer can be read: the firmware may have taken the level, unknown) */
	lck_mtx_unlock(&scmi_lock);
	return status;
}
