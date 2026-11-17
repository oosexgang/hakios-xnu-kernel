/*
 * The two queues between bluetoothd and the controller (drivers/board/bt_queue.h), as they are: the line of what
 * bluetoothd wrote, whole packets taken from its front, and the ring of packets for bluetoothd, each in and out whole
 * across the ring's end -- held, at the end, to a plain list of packets over thousands of steps.
 *
 *   bt_queue_test SCENARIO
 */
#include "fakekernel.h"
#include "board/bt_queue.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* a packet of TYPE whose parameters or data are LENGTH bytes, each its own: its whole length */
static unsigned
packet(uint8_t *p, uint8_t type, unsigned length, uint8_t seed)
{
	unsigned header;

	p[0] = type;
	switch (type) {
	case BT_H4_COMMAND:
		p[1] = seed;
		p[2] = 0x0c;
		p[3] = (uint8_t)length;
		header = 4;
		break;
	case BT_H4_EVENT:
		p[1] = 0x0e;
		p[2] = (uint8_t)length;
		header = 3;
		break;
	default:                                        /* ACL */
		p[1] = seed;
		p[2] = 0x20;
		p[3] = (uint8_t)length;
		p[4] = (uint8_t)(length >> 8);
		header = 5;
		break;
	}
	for (unsigned i = 0; i < length; i++) {
		p[header + i] = (uint8_t)(seed + i * 3);
	}
	return header + length;
}

/* ---- the line ----------------------------------------------------------------------------------- */

/* packets come in pieces, and leave whole, in order */
static void
line_in_pieces(void)
{
	static uint8_t storage[64], in[200], out[200];
	struct bt_line l = { storage, sizeof(storage), 0 };
	uint32_t dropped = 0;
	unsigned a = packet(in, BT_H4_COMMAND, 3, 1), b = packet(in + a, BT_H4_ACL, 10, 2);

	CHECK(bt_line_take(&l, out, sizeof(out), &dropped) == 0, "nothing: nothing");
	CHECK(bt_line_put(&l, in, 2), "two bytes in");
	CHECK(bt_line_take(&l, out, sizeof(out), &dropped) == 0 && l.count == 2, "a header not whole: waited for");
	CHECK(bt_line_put(&l, in + 2, a - 2 + 4), "the rest of the command and the start of the data");
	CHECK(bt_line_take(&l, out, sizeof(out), &dropped) == (int)a && memcmp(out, in, a) == 0, "the command, whole");
	CHECK(bt_line_take(&l, out, sizeof(out), &dropped) == 0 && l.count == 4, "the data not whole: waited for");
	CHECK(bt_line_put(&l, in + a + 4, b - 4), "the rest of it");
	CHECK(bt_line_take(&l, out, sizeof(out), &dropped) == (int)b && memcmp(out, in + a, b) == 0 && l.count == 0, "the data, whole");
	CHECK(dropped == 0, "nothing dropped");
}

/* what is no host's is thrown away whole; what does not fit is refused whole */
static void
line_refuses(void)
{
	static uint8_t storage[64], in[200], out[200];
	struct bt_line l = { storage, sizeof(storage), 0 };
	uint32_t dropped = 0;
	unsigned n = packet(in, BT_H4_EVENT, 2, 0);

	CHECK(bt_line_put(&l, in, n) && bt_line_take(&l, out, sizeof(out), &dropped) == -1 && l.count == 0 && dropped == n,
	    "an event, which only a controller sends: thrown away (%u)", dropped);
	CHECK(bt_line_put(&l, in, 60) && !bt_line_put(&l, in, 5) && l.count == 60, "no room for five more: none of them put");
	CHECK(bt_line_put(&l, in, 4) && l.count == 64, "room for four to the byte: put");
	l.count = 0;
	n = packet(in, BT_H4_ACL, 100, 1);
	CHECK(bt_line_put(&l, in, 10) && bt_line_take(&l, out, sizeof(out), &dropped) == -1 && l.count == 0,
	    "a packet longer than the line: it would never be whole, thrown away");
	l.count = 0;
	n = packet(in, BT_H4_ACL, 40, 1);
	CHECK(bt_line_put(&l, in, n) && bt_line_take(&l, out, 44, &dropped) == -1 && l.count == 0, "longer than the room it is taken to");
	n = packet(in, BT_H4_ACL, 40, 1);
	CHECK(bt_line_put(&l, in, n) && bt_line_take(&l, out, 45, &dropped) == 45, "room for it to the byte: taken");
}

/* ---- the ring ----------------------------------------------------------------------------------- */

/* packets in and out across the ring's end, each whole */
static void
ring_wraps(void)
{
	static uint8_t storage[32], in[64], out[64];
	struct bt_ring r = { storage, sizeof(storage), 0, 0 };
	unsigned n = packet(in, BT_H4_EVENT, 7, 9);        /* (10 bytes) */

	CHECK(bt_ring_next(&r) == 0 && bt_ring_take(&r, out) == 0, "empty: nothing");
	for (unsigned round = 0; round < 20; round++) {
		CHECK(bt_ring_put(&r, in, n) && bt_ring_put(&r, in, n) && bt_ring_put(&r, in, n), "round %u: three in", round);
		CHECK(!bt_ring_put(&r, in, n) && r.count == 30, "round %u: a fourth does not fit, none of it put", round);
		for (unsigned k = 0; k < 3; k++) {
			memset(out, 0, sizeof(out));
			CHECK(bt_ring_next(&r) == n && bt_ring_take(&r, out) == n && memcmp(out, in, n) == 0, "round %u: one out, whole (head %u)",
			    round, r.head);
		}
		CHECK(r.count == 0, "round %u: empty again", round);
	}
	CHECK(!bt_ring_put(&r, in, 0), "nothing is no packet");
}

/* a packet dropped unread, and a header read across the end */
static void
ring_drops_and_headers(void)
{
	static uint8_t storage[16], in[64], out[64];
	struct bt_ring r = { storage, sizeof(storage), 14, 0 };   /* (the next byte goes in two before the end) */
	unsigned n = packet(in, BT_H4_ACL, 4, 5);          /* (9 bytes: its header across the end) */

	CHECK(bt_ring_put(&r, in, n) && bt_ring_next(&r) == n, "a header across the end, read whole");
	CHECK(bt_ring_take(&r, NULL) == n && r.count == 0 && r.head == (14 + n) % 16, "taken unread: gone");
	CHECK(bt_ring_put(&r, in, n) && bt_ring_take(&r, out) == n && memcmp(out, in, n) == 0, "and the next one as it was");
	r.head = 5;
	r.count = 3;                                    /* (bytes that are no whole packet: cannot be, and are not used) */
	storage[r.head] = BT_H4_ACL;
	CHECK(bt_ring_take(&r, out) == 0 && r.count == 0 && r.head == 0, "a header cut short: emptied");
	r.head = 5;
	r.count = 5;                                    /* (a whole header that says more than there is) */
	memcpy(storage + 5, (const uint8_t[]){ BT_H4_ACL, 1, 0x20, 100, 0 }, 5);
	CHECK(bt_ring_take(&r, out) == 0 && r.count == 0 && r.head == 0, "a packet longer than what is there: emptied");
}

/* both, step by step at random, against a plain list of what went in */
static void
against_a_list(void)
{
	static uint8_t storage[300], in[400], out[400], line_storage[512];
	static struct { uint8_t bytes[300]; unsigned length; } list[400];
	struct bt_ring r = { storage, sizeof(storage), 0, 0 };
	struct bt_line l = { line_storage, sizeof(line_storage), 0 };
	unsigned first = 0, last = 0, held = 0, sent = 0, received = 0, offered = 0, written = 0;
	uint32_t seed = 12345, dropped = 0;

	for (unsigned step = 0; step < 20000; step++) {
		unsigned choice;

		seed = seed * 1103515245u + 12345u;
		choice = (seed >> 16) % 4;
		if (choice < 2) {
			unsigned length = (seed >> 8) % 60, n = packet(in, (seed >> 20) & 1 ? BT_H4_EVENT : BT_H4_ACL, length, (uint8_t)step);
			bool fits = n <= r.size - r.count;

			CHECK(bt_ring_put(&r, in, n) == fits, "step %u: put as there is room (%u in %u)", step, n, r.size - r.count);
			if (fits) {
				memcpy(list[last % 400].bytes, in, n);
				list[last % 400].length = n;
				last++;
				held += n;
			}
		} else if (choice == 2) {
			unsigned n = bt_ring_take(&r, out);

			if (first == last) {
				CHECK(n == 0, "step %u: empty, nothing taken", step);
			} else {
				CHECK(n == list[first % 400].length && memcmp(out, list[first % 400].bytes, n) == 0, "step %u: the oldest, whole", step);
				held -= n;
				first++;
				received++;
			}
		} else {
			/* the line: a command written in two pieces, taken back whole */
			unsigned n = packet(in, BT_H4_COMMAND, (seed >> 8) % 40, (uint8_t)step), cut = (seed >> 4) % n;

			if (bt_line_put(&l, in, cut) && bt_line_put(&l, in + cut, n - cut)) {
				written++;
				offered++;
				CHECK(bt_line_take(&l, out, sizeof(out), &dropped) == (int)n && memcmp(out, in, n) == 0, "step %u: the command back", step);
				sent++;
			}
		}
		CHECK(r.count == held, "step %u: the ring holds what the list does (%u, %u)", step, r.count, held);
	}
	CHECK(received > 1000 && sent > 1000 && sent == written && dropped == 0 && offered == written, "enough of each (%u, %u)", received, sent);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "line_in_pieces", line_in_pieces }, { "line_refuses", line_refuses }, { "ring_wraps", ring_wraps },
		{ "ring_drops_and_headers", ring_drops_and_headers }, { "against_a_list", against_a_list },
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
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
