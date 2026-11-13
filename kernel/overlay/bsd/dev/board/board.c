/*
 * Which board this is (board/board.h): the row of the generated table whose name the loader put in
 * /chosen/darwin-board. Also as sysctls, for user space and for a person at a shell:
 *
 *   hw.board.name          the package's name; "unknown" when the loader named none or the table has not got it
 *   hw.board.soc           "sm7350"
 *   hw.board.description   "Maker Model"
 *   hw.board.features      "cpufreq-epss ddr-rpmh ..."
 *
 * The table is made when the kernel is built (tools/boards.py gen, by kernel/build.sh), with a row for
 * every device package that was there: a kernel knows the boards it was built with.
 */
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "kernel.h"
#include "board.h"
#include "board_table.h"

static const struct hakios_board *found;
static bool looked;

/*
 * Is this the board the loader named (/chosen/darwin-board, a string with its end)? The name is the
 * loader's word and nothing else is: a board is never inferred from hw.machine, which is an iPhone's
 * for Darwin's sake, and a machine whose loader named none is no board at all -- which is not permission
 * to touch anything of a phone.
 */
static bool
loader_named(const char *name)
{
	DTEntry chosen;
	const void *value;
	unsigned int size;
	size_t length = strlen(name) + 1;

	return SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	       SecureDTGetProperty(chosen, "darwin-board", &value, &size) == kSuccess &&
	       size == length && memcmp(value, name, length) == 0;
}

const struct hakios_board *
hakios_board(void)
{
	if (!looked) {
		/* (twice at once finds the same row twice: nothing to guard) */
		for (unsigned i = 0; i < hakios_board_count; i++) {
			if (loader_named(hakios_boards[i].name)) {
				found = &hakios_boards[i];
				break;
			}
		}
		looked = true;
	}
	return found;
}

bool
hakios_board_has(uint32_t feature)
{
	const struct hakios_board *board = hakios_board();

	return board != NULL && (board->features & feature) != 0;
}

unsigned
hakios_board_cpu_groups(void)
{
	const struct hakios_board *board = hakios_board();
	unsigned groups = 0;

	while (board != NULL && groups < HAKIOS_CPU_GROUPS && board->cpufreq.domain_cores[groups] != 0) {
		groups++;
	}
	return groups;
}

int
hakios_board_cpu_group_of(unsigned core)
{
	const struct hakios_board *board = hakios_board();
	unsigned first = 0;

	for (unsigned group = 0; board != NULL && group < HAKIOS_CPU_GROUPS && board->cpufreq.domain_cores[group] != 0; group++) {
		if (core < first + board->cpufreq.domain_cores[group]) {
			return (int)group;
		}
		first += board->cpufreq.domain_cores[group];
	}
	return -1;
}

/* "cpufreq-epss ddr-rpmh", or "" */
static void
features_text(const struct hakios_board *board, char *text, size_t size)
{
	size_t at = 0;

	text[0] = 0;
	for (unsigned bit = 0; board != NULL && bit < hakios_feature_count; bit++) {
		if (board->features & (1u << bit)) {
			at += (size_t)snprintf(text + at, size - at, "%s%s", at ? " " : "", hakios_feature_names[bit]);
			if (at >= size) {
				break;
			}
		}
	}
}

void
hakios_board_init(void)
{
	const struct hakios_board *board = hakios_board();
	char features[HAKIOS_FEATURES_TEXT], line[160 + HAKIOS_FEATURES_TEXT];   /* (a board with every feature fits) */

	features_text(board, features, sizeof(features));
	if (board != NULL) {
		/* (the SoC only when it is named apart from the board: a package's soc.dtsi takes the board's name) */
		bool soc_of_its_own = strcmp(board->soc, board->name) != 0;

		snprintf(line, sizeof(line), "board: %s (%s%s%s): %s\n", board->name, board->description,
		    soc_of_its_own ? ", " : "", soc_of_its_own ? board->soc : "", features[0] ? features : "no features listed");
	} else {
		snprintf(line, sizeof(line), "board: not one of the %u this kernel knows (/chosen/darwin-board): no device "
		    "of a phone is touched\n", hakios_board_count);
	}
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

SYSCTL_NODE(_hw, OID_AUTO, board, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "the board this kernel runs on");

static int
sysctl_board SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1)
	const struct hakios_board *board = hakios_board();
	char features[HAKIOS_FEATURES_TEXT];
	const char *text;

	switch (arg2) {
	case 0: text = board ? board->name : "unknown"; break;
	case 1: text = board ? board->soc : "unknown"; break;
	case 2: text = board ? board->description : "unknown"; break;
	default:
		features_text(board, features, sizeof(features));
		text = features;
		break;
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_hw_board, OID_AUTO, name, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_board, "A", "");
SYSCTL_PROC(_hw_board, OID_AUTO, soc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 1, sysctl_board, "A", "");
SYSCTL_PROC(_hw_board, OID_AUTO, description, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 2, sysctl_board, "A", "");
SYSCTL_PROC(_hw_board, OID_AUTO, features, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 3, sysctl_board, "A", "");
