/*
 * Bluetooth's H4 packets and what is answered or changed between bluetoothd and the controller
 * (drivers/board/bt_hci.h), as it is: a packet's length from its first bytes, the answers given in the
 * kernel with the data bluetoothd's parsers read, what goes to the controller untouched, and the
 * controller's packets as bluetoothd is to see them.
 *
 *   bt_hci_test SCENARIO
 */
#include "fakekernel.h"
#include "board/bt_hci.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint8_t event[300];

/* a command: its opcode, then its parameters */
static unsigned
command(uint8_t *c, uint16_t op, const uint8_t *parameters, uint8_t length)
{
	c[0] = BT_H4_COMMAND;
	c[1] = (uint8_t)op;
	c[2] = (uint8_t)(op >> 8);
	c[3] = length;
	if (length != 0) {
		memcpy(c + 4, parameters, length);
	}
	return 4u + length;
}

/* the event is a Command Complete for OP, status 0, with DATA bytes after it */
static bool
completes(unsigned length, uint16_t op, unsigned data)
{
	return length == 7 + data && event[0] == BT_H4_EVENT && event[1] == 0x0e && event[2] == 4 + data && event[3] == 1 &&
	       event[4] == (uint8_t)op && event[5] == (uint8_t)(op >> 8) && event[6] == 0;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* each type's length from its header, and nothing until there is enough of the header to say */
static void
lengths(void)
{
	static const struct { uint8_t b[5]; unsigned have, length; } cases[] = {
		{ { 1, 0x03, 0x0c, 0 }, 4, 4 }, { { 1, 0x01, 0xfc, 6 }, 4, 10 }, { { 1, 0x01, 0xfc }, 3, 0 },
		{ { 2, 0x01, 0x20, 0x10, 0x01 }, 5, 5 + 0x110 }, { { 2, 0x01, 0x20, 0x10 }, 4, 0 },
		{ { 3, 0x01, 0x00, 60 }, 4, 64 },
		{ { 4, 0x0e, 4 }, 3, 7 }, { { 4, 0x0e }, 2, 0 }, { { 4, 0x3e, 255 }, 3, 258 },
		{ { 5, 0x01, 0x00, 0xff, 0xff }, 5, 5 + 0x3fff }, { { 5, 0, 0, 0x10, 0x40 }, 5, 5 + 0x10 },
		{ { 6, 0, 0, 0, 0 }, 5, 0 }, { { 0, 0, 0, 0, 0 }, 5, 0 }, { { 1 }, 0, 0 },
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(bt_h4_length(cases[i].b, cases[i].have) == cases[i].length, "case %u: %u, not %u", i, bt_h4_length(cases[i].b,
		    cases[i].have), cases[i].length);
	}
	CHECK(bt_h4_length((const uint8_t[]){ 5, 0, 0, 0xff, 0xff }, 5) <= BT_H4_MOST, "nothing is longer than the longest");
}

/* what a host wrote, as a run of bytes: the whole packet at its head, nothing until it is all there, and a run that is
 * no host's thrown away */
static void
from_host(void)
{
	static const uint8_t reset[] = { 1, 0x03, 0x0c, 0, 2, 0x01, 0x20, 3, 0, 7, 8, 9 };
	uint8_t big[] = { 2, 0x01, 0x20, 0xff, 0x3f };

	CHECK(bt_h4_from_host(reset, 0, 100) == 0, "nothing: nothing yet");
	CHECK(bt_h4_from_host((const uint8_t[]){ 4 }, 0, 100) == 0, "nothing, whatever is in the queue past it");
	CHECK(bt_h4_from_host(reset, 3, 100) == 0, "a header not all there: nothing yet");
	CHECK(bt_h4_from_host(reset, 4, 100) == 4 && bt_h4_from_host(reset, 12, 100) == 4, "HCI_Reset, whatever comes after it");
	CHECK(bt_h4_from_host(reset + 4, 7, 100) == 0 && bt_h4_from_host(reset + 4, 8, 100) == 8, "an ACL packet once it is whole");
	CHECK(bt_h4_from_host((const uint8_t[]){ 4, 0x0e, 0 }, 3, 100) == -1, "an event: no host's");
	CHECK(bt_h4_from_host((const uint8_t[]){ 4 }, 1, 100) == -1, "the start of one, already");
	CHECK(bt_h4_from_host((const uint8_t[]){ 9, 0, 0, 0 }, 4, 100) == 0, "a type there is not, too few bytes to say");
	CHECK(bt_h4_from_host((const uint8_t[]){ 9, 0, 0, 0, 0 }, 5, 100) == -1, "a type there is not");
	CHECK(bt_h4_from_host(big, 5, 0x3fff + 5) == 0, "longest ACL, room for it: waited for");
	CHECK(bt_h4_from_host(big, 5, 0x3fff + 4) == -1, "no room for it: never whole");
}

/* the commands that go to the controller as they are */
static void
to_the_controller(void)
{
	uint8_t c[64];
	static const uint16_t ops[] = { 0x0c03, 0x1001, 0x1002, 0x0c13, 0x200b, 0x200c, 0x0405, 0x0c5c, 0x2049, 0x204b };

	for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		CHECK(bt_answer_here(c, command(c, ops[i], NULL, 0), BT_Q_DEFAULT, event, sizeof(event)) == 0, "0x%04x: sent", ops[i]);
	}
	CHECK(bt_answer_here((const uint8_t[]){ 2, 1, 0, 0, 0 }, 5, BT_Q_DEFAULT, event, sizeof(event)) == 0, "data: sent");
	CHECK(bt_answer_here((const uint8_t[]){ 1, 0x01, 0xfc }, 3, BT_Q_DEFAULT, event, sizeof(event)) == 0, "a command cut short: not ours");
}

/* Default Erroneous Data Reporting: Write done, Read done with the mode 0 -- and sent on, without the quirk */
static void
erroneous_data_reporting(void)
{
	uint8_t c[16];

	CHECK(completes(bt_answer_here(c, command(c, 0x0c5b, (const uint8_t[]){ 1 }, 1), BT_Q_DEFAULT, event, sizeof(event)), 0x0c5b, 0),
	    "write");
	CHECK(completes(bt_answer_here(c, command(c, 0x0c5a, NULL, 0), BT_Q_DEFAULT, event, sizeof(event)), 0x0c5a, 1) && event[7] == 0,
	    "read: off");
	CHECK(bt_answer_here(c, command(c, 0x0c5a, NULL, 0), BT_Q_DEFAULT & ~BT_Q_ERRDATA, event, sizeof(event)) == 0, "without it: sent");
}

/* LE Read Periodic Advertiser List Size: sixteen, said here */
static void
legacy_le(void)
{
	uint8_t c[16];

	CHECK(completes(bt_answer_here(c, command(c, 0x204a, NULL, 0), BT_Q_DEFAULT, event, sizeof(event)), 0x204a, 1) && event[7] == 0x10,
	    "sixteen");
	CHECK(bt_answer_here(c, command(c, 0x204a, NULL, 0), BT_Q_DEFAULT & ~BT_Q_LEGACY_LE, event, sizeof(event)) == 0, "without it: sent");
}

/* vendor commands: done, with what each parser reads after the status and no more */
static void
vendor_commands(void)
{
	static const struct { uint16_t op; uint8_t sub; unsigned data; int first; } cases[] = {
		{ 0xfd1b, 0, 64, 0x51 }, { 0xfce2, 0x15, 16, 0 }, { 0xfce2, 0x36, 16, 0 }, { 0xfce2, 0x0d, 8, 0 }, { 0xfce2, 0x2d, 4, 0 },
		{ 0xfce2, 0x05, 1, 0 }, { 0xfce2, 0x0a, 1, 0 }, { 0xfce2, 0x01, 0, 0 }, { 0xfd48, 0, 1, 0 }, { 0xfce9, 0, 0, 0 },
		{ 0xfc01, 0, 0, 0 }, { 0xfc6f, 0, 0, 0 },
	};
	uint8_t c[16];

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		unsigned n = bt_answer_here(c, command(c, cases[i].op, &cases[i].sub, 1), BT_Q_DEFAULT, event, sizeof(event));
		bool zeros = true;

		for (unsigned k = 0; k < cases[i].data; k++) {
			zeros &= event[7 + k] == (k == 0 ? (uint8_t)cases[i].first : 0);
		}
		CHECK(completes(n, cases[i].op, cases[i].data) && zeros, "0x%04x sub 0x%02x: %u bytes", cases[i].op, cases[i].sub, n);
	}
	CHECK(bt_answer_here(c, command(c, 0xfce2, NULL, 0), BT_Q_DEFAULT, event, sizeof(event)) == 7, "no sub-command: none");
	CHECK(bt_answer_here(c, command(c, 0xfd1b, NULL, 0), BT_Q_DEFAULT & ~BT_Q_VENDOR, event, sizeof(event)) == 0, "without it: sent");
	CHECK(bt_answer_here(c, command(c, 0xfd1b, NULL, 0), BT_Q_DEFAULT, event, 70) == 0, "no room for the answer: none");
	CHECK(bt_answer_here(c, command(c, 0xfd1b, NULL, 0), BT_Q_DEFAULT, event, 71) == 71, "room for it to the byte: made");
}

/* the controller's own vendor events are not handed on; other events are, untouched */
static void
vendor_events(void)
{
	uint8_t p[] = { 4, 0xff, 2, 0x12, 0x34 }, q[] = { 4, 0x0e, 4, 1, 0x03, 0x0c, 0 }, r[] = { 4, 0x0e, 4, 1, 0x03, 0x0c, 0 };

	CHECK(bt_rewrite(p, sizeof(p), BT_Q_DEFAULT) == 0, "a vendor event: dropped");
	CHECK(bt_rewrite((uint8_t[]){ 4, 0xff, 0 }, 3, BT_Q_DEFAULT) == 0, "an empty one too");
	CHECK(bt_rewrite(p, sizeof(p), BT_Q_DEFAULT & ~BT_Q_VENDOR_EVENTS) == sizeof(p), "without it: handed on");
	CHECK(bt_rewrite(q, sizeof(q), BT_Q_DEFAULT) == sizeof(q) && memcmp(q, r, sizeof(q)) == 0, "HCI_Reset's answer: as it is");
}

/* Read Local Supported Commands: the LE commands and the reporting said, the rest as the controller has it */
static void
supported_commands(void)
{
	uint8_t p[7 + 64], before[7 + 64];

	memset(p, 0, sizeof(p));
	p[0] = 4;
	p[1] = 0x0e;
	p[2] = 4 + 64;
	p[3] = 1;
	p[4] = 0x02;
	p[5] = 0x10;
	p[7 + 5] = 0x80;                        /* (a bit of the controller's own) */
	memcpy(before, p, sizeof(p));
	CHECK(bt_rewrite(p, sizeof(p), BT_Q_DEFAULT) == sizeof(p), "handed on");
	CHECK(p[7 + 25] == 0xf7 && p[7 + 26] == 0xff && p[7 + 27] == 0xff && p[7 + 28] == 0x7f && p[7 + 24] == 0x60 && p[7 + 18] == 0x0c,
	    "the LE commands and the reporting: %02x %02x %02x %02x %02x %02x", p[32], p[33], p[34], p[35], p[31], p[25]);
	CHECK(p[7 + 5] == 0x80 && p[7 + 63] == 0, "the controller's own bits kept");
	memcpy(p, before, sizeof(p));
	p[6] = 1;                               /* (an error: no bits to speak of) */
	CHECK(bt_rewrite(p, sizeof(p), BT_Q_DEFAULT) == sizeof(p) && p[7 + 25] == 0, "a failed read: as it is");
	memcpy(p, before, sizeof(p));
	CHECK(bt_rewrite(p, sizeof(p) - 1, BT_Q_DEFAULT) == sizeof(p) - 1 && p[7 + 25] == 0, "a short one: as it is");
	CHECK(bt_rewrite(p, sizeof(p), BT_Q_DEFAULT & ~BT_Q_ERRDATA) == sizeof(p) && p[7 + 18] == 0 && p[7 + 25] == 0xf7,
	    "the reporting said only when it is answered");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "lengths", lengths }, { "from_host", from_host }, { "to_the_controller", to_the_controller }, { "erroneous_data_reporting", erroneous_data_reporting },
		{ "legacy_le", legacy_le }, { "vendor_commands", vendor_commands }, { "vendor_events", vendor_events },
		{ "supported_commands", supported_commands },
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
