/*
 * stream_fetch() -- the part of reading a file's contents that runs without the volume's
 * lock -- against made-up runs, since the iOS 16.5 root volume has no sparse file and so
 * never gives it a hole: holes at the front, between runs and at the end, runs that start
 * and end inside a block, a run inside one block, no runs at all.  apfs_lib.c is included
 * whole, so that its static functions can be called.  The device is 64 blocks of memory,
 * block n holding bytes that depend on n and on the place in the block, so a byte from
 * the wrong block or the wrong place shows.  Built and run by run-tests.sh.
 */
#include "apfs_lib.c"

#define BS   4096
#define NBLK 64

static uint8_t disk[NBLK * BS];

static int
dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	(void)ctx;
	if (off % BS != 0 || len % BS != 0 || off + len > sizeof(disk)) {
		return EINVAL;          /* whole blocks only, as apfs_dev.read promises */
	}
	memcpy(buf, disk + off, len);
	return 0;
}

struct fcase {
	const char      *what;
	uint32_t         nruns;
	struct apfs_run  runs[APFS_MAX_RUNS];
	uint64_t         covered;
};

static const struct fcase cases[] = {
	{ "whole blocks", 1, { { 0, 3, 0, 3 * BS } }, 3 * BS },
	{ "starts and ends inside blocks", 1, { { 0, 5, 100, 2 * BS + 50 } }, 2 * BS + 50 },
	{ "inside one block", 1, { { 0, 7, 10, 20 } }, 20 },
	{ "ends where a block does", 1, { { 0, 10, 4000, 96 + BS } }, 96 + BS },
	{ "a hole at the front", 1, { { 1000, 2, 0, 5000 } }, 6000 },
	{ "a hole between two runs", 2, { { 0, 1, 0, BS }, { 2 * BS, 9, 0, BS } }, 3 * BS },
	{ "a hole at the end", 1, { { 0, 4, 0, 100 } }, 2 * BS },
	{ "no runs: all hole", 0, { { 0 } }, 5000 },
	{ "eight runs, every one unaligned", 8, {
		  { 0, 20, 1, 10 }, { 10, 21, 4095, 2 }, { 100, 22, 0, BS + 1 },
		  { 2 * BS, 30, 2048, 2048 }, { 3 * BS, 31, 0, 1 }, { 3 * BS + 5, 40, 7, 3 * BS },
		  { 7 * BS, 50, 4000, 200 }, { 8 * BS, 60, 0, 4 * BS } }, 12 * BS },
};

int
main(void)
{
	struct apfs_nx nx;
	static uint8_t dst[16 * BS], want[16 * BS], bounce[BS];
	unsigned c, failed = 0;
	uint64_t i;

	for (i = 0; i < sizeof(disk); i++) {
		disk[i] = (uint8_t)((i / BS) * 37 + (i % BS) * 7 + 1);
	}
	memset(&nx, 0, sizeof(nx));
	nx.block_size = BS;
	nx.block_count = NBLK;
	nx.dev.read = dev_read;

	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		const struct fcase *fc = &cases[c];
		uint64_t calls = 0, bytes = 0;
		uint32_t r;
		int error;

		memset(want, 0, sizeof(want));
		for (r = 0; r < fc->nruns; r++) {
			const struct apfs_run *run = &fc->runs[r];

			memcpy(want + run->at, disk + run->addr * BS + run->skew, run->len);
		}
		memset(dst, 0xAA, sizeof(dst));         /* what is not written shows */
		error = stream_fetch(&nx, fc->runs, fc->nruns, fc->covered, dst, bounce, &calls,
		    &bytes);
		if (error != 0 || memcmp(dst, want, fc->covered) != 0 || dst[fc->covered] != 0xAA) {
			printf("  FAIL  %s (error %d)\n", fc->what, error);
			failed++;
		} else {
			printf("  ok    %s (%llu requests, %llu bytes)\n", fc->what,
			    (unsigned long long)calls, (unsigned long long)bytes);
		}
	}
	printf("stream_fetch: %u cases, %u failed\n", (unsigned)(sizeof(cases) / sizeof(cases[0])),
	    failed);
	return failed ? 1 : 0;
}
