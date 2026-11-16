/*
 * The arithmetic of GLINK's two queues that the drivers share
 * (drivers/board/glink_queue.h): how much is unread, how much room there is, bytes
 * taken out and put in around a queue's end -- and that marks the other processor may have written
 * anything into never lead outside the queue.
 *
 *   glink_queue_test SCENARIO
 */
#include "fakekernel.h"

#define KERNEL 1
#include "board/glink_queue.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define SIZE    1024u
static uint8_t memory[32 + SIZE + 32];          /* (a canary before and after the queue) */
#define QUEUE   (memory + 32)

static void
fresh(void)
{
	memset(memory, 0xee, sizeof(memory));
	memset(QUEUE, 0, SIZE);
}

static bool
canaries_whole(void)
{
	for (unsigned i = 0; i < 32; i++) {
		if (memory[i] != 0xee || memory[32 + SIZE + i] != 0xee) {
			return false;
		}
	}
	return true;
}

static void
unread(void)
{
	CHECK(glink_queue_unread(0, 0, SIZE) == 0, "an empty queue");
	CHECK(glink_queue_unread(24, 8, SIZE) == 16, "written to 24, read to 8");
	CHECK(glink_queue_unread(8, 1016, SIZE) == 16, "around the end: 8 behind it and 8 before");
	CHECK(glink_queue_unread(1023, 0, SIZE) == 1023, "as full as it gets");
	CHECK(glink_queue_unread(SIZE, 0, SIZE) == 0 && glink_queue_unread(0, SIZE + 8, SIZE) == 0 &&
	    glink_queue_unread(0xffffffff, 0xffffffff, SIZE) == 0, "a mark outside the queue: nothing is believed to be there");
}

static void
room(void)
{
	CHECK(glink_queue_room(0, 0, SIZE) == SIZE - GLINK_RESERVE, "an empty queue: all but the reserve");
	CHECK(glink_queue_room(512, 0, SIZE) == 512 - GLINK_RESERVE, "half written, nothing read");
	CHECK(glink_queue_room(8, 512, SIZE) == 504 - GLINK_RESERVE, "the head behind the tail: up to the tail");
	CHECK(glink_queue_room(500, 508, SIZE) == 0, "less than the reserve left: none");
	CHECK(glink_queue_room(SIZE, 0, SIZE) == 0 && glink_queue_room(0, SIZE, SIZE) == 0, "a mark outside the queue: no room");
	/* what room says may be written never reaches the tail */
	for (uint32_t head = 0; head < SIZE; head += 8) {
		for (uint32_t tail = 0; tail < SIZE; tail += 8) {
			uint32_t may = glink_queue_room(head, tail, SIZE), free = tail <= head ? SIZE - head + tail : tail - head;

			if (may > free || (may != 0 && may + GLINK_RESERVE != free)) {
				CHECK(false, "head %u, tail %u: room %u of %u free", head, tail, may, free);
				return;
			}
		}
	}
}

static void
in_and_out(void)
{
	uint8_t out[24];
	uint32_t moved;

	fresh();
	QUEUE[15] = QUEUE[16 + 24] = 0x5a;      /* (what is there before it and after it is not the message's) */
	moved = glink_queue_put(QUEUE, SIZE, 16, "header..", 8, "and more", 9);
	CHECK(moved == 24 && memcmp(QUEUE + 16, "header..and more", 17) == 0, "two pieces, one after the other: %u bytes", moved);
	CHECK(QUEUE[16 + 17] == 0 && QUEUE[16 + 23] == 0 && QUEUE[16 + 24] == 0x5a && QUEUE[15] == 0x5a,
	    "filled up to eight with zeros, and not a byte further");
	glink_queue_peek(QUEUE, SIZE, 16, 0, out, 24);
	CHECK(memcmp(out, "header..and more", 17) == 0, "read back from the tail");
	glink_queue_peek(QUEUE, SIZE, 16, 8, out, 9);
	CHECK(memcmp(out, "and more", 9) == 0, "... and from eight bytes behind it");
	CHECK(glink_queue_after(16, 24, SIZE) == 40, "the mark after it");
	CHECK(glink_queue_put(QUEUE, SIZE, 0, "x", 1, NULL, 0) == 8, "one byte is a message of eight");
	CHECK(canaries_whole(), "nothing outside the queue was written");
}

static void
around_the_end(void)
{
	uint8_t out[16];
	uint32_t moved;

	fresh();
	moved = glink_queue_put(QUEUE, SIZE, SIZE - 8, "12345678", 8, "abcdefgh", 8);
	CHECK(moved == 16 && memcmp(QUEUE + SIZE - 8, "12345678", 8) == 0 && memcmp(QUEUE, "abcdefgh", 8) == 0,
	    "the second half is at the queue's start");
	glink_queue_peek(QUEUE, SIZE, SIZE - 8, 0, out, 16);
	CHECK(memcmp(out, "12345678abcdefgh", 16) == 0, "read back around the end");
	glink_queue_peek(QUEUE, SIZE, SIZE - 8, 12, out, 4);
	CHECK(memcmp(out, "efgh", 4) == 0, "from behind the end on");
	CHECK(glink_queue_after(SIZE - 8, 16, SIZE) == 8, "the mark goes around too");
	CHECK(canaries_whole(), "nothing outside the queue was written");
}

/* a tail or a head the other processor wrote nonsense into: taken into the queue, never outside it */
static void
marks_of_nonsense(void)
{
	uint8_t out[8] = { 0 };

	fresh();
	memcpy(QUEUE + 8, "inside!", 8);
	glink_queue_peek(QUEUE, SIZE, SIZE + 8, 0, out, 8);
	CHECK(memcmp(out, "inside!", 8) == 0, "a tail past the end is taken around");
	glink_queue_peek(QUEUE, SIZE, 0xfffffff8, 0xfffffff8, out, 8);
	CHECK(canaries_whole(), "marks near the largest number there is");
	glink_queue_put(QUEUE, SIZE, 0xfffffffc, "12345678", 8, NULL, 0);
	glink_queue_put(QUEUE, SIZE, SIZE * 5 + 1020, "12345678", 8, NULL, 0);
	CHECK(canaries_whole(), "a head past the end writes inside the queue");
	CHECK(glink_queue_after(0xfffffff0, 0xfffffff0, SIZE) < SIZE, "the mark after it is inside");
	/*
	 * ... and at the place the mark means, also where mark and offset together are more than a number holds:
	 * in a queue whose size is not a power of two that is another byte than the sum's remainder.
	 */
	{
		static uint8_t odd[1032];
		const uint32_t tail = 0xfffffff8u, offset = 0x20;
		uint32_t place = (tail % 1032 + offset % 1032) % 1032;

		for (unsigned i = 0; i < sizeof(odd); i++) {
			odd[i] = (uint8_t)(i * 7 + (i >> 8));
		}
		glink_queue_peek(odd, sizeof(odd), tail, offset, out, 8);
		CHECK(memcmp(out, odd + place, 8) == 0 && place != (uint32_t)(tail + offset) % 1032, "a mark and an offset whose sum wraps: byte %u", place);
		CHECK(glink_queue_after(tail, offset, 1032) == place, "the mark after it is that place");
	}
}

static void
shapes(void)
{
	CHECK(glink_queues_usable(1024, 0x2000), "two queues of whole messages");
	CHECK(!glink_queues_usable(512, 0x2000) && !glink_queues_usable(0x2000, 0), "one too small, or not there");
	CHECK(!glink_queues_usable(1028, 0x2000) && !glink_queues_usable(0x2000, 1025), "one that is not whole eights");
}

/* how long a message the other side sends is: what follows it counted, filled up to eight, and no count wraps */
static void
message_lengths(void)
{
	struct glink_message m = { .command = GLINK_VERSION, .param1 = 1, .param2 = 0x7f };

	CHECK(glink_message_length(&m, 0) == 8, "a message alone: 8");
	m.command = GLINK_OPEN;
	m.param2 = 5;                           /* a name of five bytes */
	CHECK(glink_message_length(&m, 0) == 16, "an OPEN with a name of 5: 16, not 13: %llu", (unsigned long long)glink_message_length(&m, 0));
	m.command = GLINK_INTENT;
	m.param2 = 3;                           /* three intents of eight bytes */
	CHECK(glink_message_length(&m, 0) == 32, "three intents: 32");
	m.command = GLINK_DATA;
	CHECK(glink_message_length(&m, 100) == 120, "a piece of 100 bytes: 16 + 100, filled up: 120");
	m.command = GLINK_DATA_MORE;
	CHECK(glink_message_length(&m, 0) == 16, "an empty piece: 16");
	/* counts that 32 bits would wrap */
	m.command = GLINK_INTENT;
	m.param2 = 1u << 29;
	CHECK(glink_message_length(&m, 0) == 8 + (8ull << 29), "2^29 intents are 4 GiB, not 0: %llu", (unsigned long long)glink_message_length(&m, 0));
	m.command = GLINK_OPEN;
	m.param2 = 0xfffffff8u;
	CHECK(glink_message_length(&m, 0) > 0xffffffffull, "a name of nearly 4 GiB is not 0 bytes");
	m.command = GLINK_DATA;
	CHECK(glink_message_length(&m, 0xfffffff0u) > 0xffffffffull, "nor is such a piece");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "unread", unread }, { "room", room }, { "in_and_out", in_and_out }, { "around_the_end", around_the_end },
		{ "marks_of_nonsense", marks_of_nonsense }, { "shapes", shapes }, { "message_lengths", message_lengths },
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
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
