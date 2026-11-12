/*
 * SM8845 CPU performance diagnostics over SCMI.
 * Transport adapted from hakios-sm8845 (805567b), macan power driver.
 * The firmware owns the clock and thermal policy.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#include "hakios_board.h"
#include "hakios_log.h"
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define SCMI_SHMEM_SIZE 0x80UL
#define SCMI_MAILBOX_SIZE 0x1000UL
#define SCMI_WORDS        ((SCMI_SHMEM_SIZE - 0x1c) / 4)
#define SCMI_PERF               0x13
#define SCMI_VERSION            0x0
#define SCMI_ATTRIBUTES         0x1
#define SCMI_PERF_LIMITS_GET    0x6
#define SCMI_PERF_LEVEL_GET     0x8
#define SCMI_DOMAINS_MAX  4

static vm_offset_t scmi_shmem, scmi_mailbox;
static uint32_t scmi_token, scmi_doorbell;

/* what was written has reached the shared memory and the mailbox before what follows (a compiler's barrier on a host) */
static inline void
scmi_barrier(void)
{
#if defined(__arm64__) || defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");
#else
	__asm__ volatile ("" ::: "memory");
#endif
}

static volatile uint32_t *
scmi_word(unsigned offset)
{
	return (volatile uint32_t *)(scmi_shmem + offset);
}

/*
 * One SCMI message of the performance protocol. Returns the SCMI status (0: success), or -100 and
 * below when the channel did not answer; `out` gets the answer's words after the status.
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
	scmi_barrier();
	*scmi_word(0x4) = 0;
	scmi_barrier();
	*(volatile uint32_t *)(scmi_mailbox + scmi_doorbell) = 1;
	scmi_barrier();
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

void
hakios_scmi_perf_init(void);

void
hakios_scmi_perf_init(void)
{
	const struct hakios_board *board = hakios_board();
	uint32_t version, attributes, enabled = 0;
	unsigned words;

	/* scmi_perf=1 queries firmware levels and limits without changing them. */
	if (!hakios_board_has(HAKIOS_HAS_CPUFREQ_SCMI) ||
	    !PE_parse_boot_argn("scmi_perf", &enabled, sizeof(enabled)) || enabled != 1) {
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
		uint32_t level, limits[2];

		if (scmi_query(SCMI_PERF_LEVEL_GET, &domain, 1, &level, 1, &words) == 0 && words >= 1) {
			hakios_say("scmi-perf: domain %u current level %u\n", domain, level);
		}
		if (scmi_query(SCMI_PERF_LIMITS_GET, &domain, 1, limits, 2, &words) == 0 && words >= 2) {
			hakios_say("scmi-perf: domain %u limits %u..%u\n", domain, limits[1], limits[0]);
		}
	}
}
