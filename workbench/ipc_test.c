/*
 * What the drivers share of the way to the always-on processor (drivers/board/ipc.h), as it
 * is: whether the mailbox that processor says is ours is one a message fits in, inside its block. Both
 * numbers are the other processor's.
 *
 *   ipc_test SCENARIO
 */
#include "fakekernel.h"
#include "board/ipc.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define BLOCK           0x400

static void
mailboxes(void)
{
	CHECK(qmp_box_usable(0x100, 0x100, BLOCK), "a box of 256 bytes at 0x100");
	CHECK(qmp_box_usable(QMP_BOXES_FROM, QMP_MESSAGE + 4, BLOCK), "the first place, the smallest size");
	CHECK(qmp_box_usable(BLOCK - 0x100, 0x100, BLOCK), "one that ends at the block's end");
	CHECK(!qmp_box_usable(0x100, QMP_MESSAGE, BLOCK), "too small for a message and its length");
	CHECK(!qmp_box_usable(0x38, 0x100, BLOCK), "over the words before the boxes");
	CHECK(!qmp_box_usable(0x102, 0x100, BLOCK), "not on a word");
	CHECK(!qmp_box_usable(BLOCK - 0xfc, 0x100, BLOCK), "over the block's end");
	CHECK(!qmp_box_usable(BLOCK + 4, 0x100, BLOCK), "past it");
}

/* the two numbers whose sum would wrap in 32 bits */
static void
sums_that_wrap(void)
{
	CHECK(!qmp_box_usable(0x100, 0xffffff00u, BLOCK), "a size that brings the sum to 0");
	CHECK(!qmp_box_usable(0xfffffff0u, 0x100, BLOCK), "a place near 2^32");
	CHECK(!qmp_box_usable(0x80, 0xffffffffu, 0xffffffffull), "even in a block as large as 32 bits");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "mailboxes", mailboxes }, { "sums_that_wrap", sums_that_wrap },
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
			fprintf(stderr, "%-16s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
