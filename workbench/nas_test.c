/*
 * The QMI NAS parser (drivers/board/nas.c), compiled as it is, given the serving system's answer as the modem sends it.
 * What is tested: the network's name in TLV 0x10 is what the network sent, cut to what is kept, and nothing in it that
 * is not printable ASCII reaches the state the log and kern.hakios.cellular_operator are made from: an escape, a
 * line's end or a byte of UTF-8 is a '?', and a zero ends the name.
 *
 *   nas_test SCENARIO
 */
#include "fakekernel.h"
#include "board/nas.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint8_t frame[128];

/* An answer to Get Serving System (0x24): the result (success), registered on LTE, and the PLMN 262-02 named `name` */
static size_t
serving_system(const void *name, uint8_t length)
{
	size_t at = 7;

	memset(frame, 0, sizeof(frame));
	frame[0] = 2;                           /* an answer */
	frame[1] = 1;                           /* transaction 1 */
	frame[3] = 0x24;
	frame[at++] = 2, frame[at++] = 4, frame[at++] = 0;              /* the result: 0, 0 */
	at += 4;
	frame[at++] = 1, frame[at++] = 6, frame[at++] = 0;              /* the serving system: registered, CS, PS, 3GPP, 1 radio: LTE */
	frame[at++] = 1, frame[at++] = 1, frame[at++] = 1, frame[at++] = 3, frame[at++] = 1, frame[at++] = 8;
	frame[at++] = 0x10, frame[at++] = (uint8_t)(5 + length), frame[at++] = 0;
	frame[at++] = 262 & 0xff, frame[at++] = 262 >> 8, frame[at++] = 2, frame[at++] = 0, frame[at++] = length;
	memcpy(frame + at, name, length);
	at += length;
	frame[5] = (uint8_t)(at - 7);
	frame[6] = (uint8_t)((at - 7) >> 8);
	return at;
}

static bool
all_printable(const char *name)
{
	for (const char *c = name; *c != 0; c++) {
		if ((uint8_t)*c < 0x20 || (uint8_t)*c > 0x7e) {
			return false;
		}
	}
	return true;
}

static void
plain_name(void)
{
	struct hakios_nas_state state = { 0 };

	CHECK(hakios_nas_apply(&state, frame, serving_system("Telekom.de", 10)) == 1, "a change");
	CHECK(state.registered && state.mcc == 262 && state.mnc == 2, "registered with 262-02 (%u-%u)", state.mcc, state.mnc);
	CHECK(strcmp(state.operator_name, "Telekom.de") == 0, "the name as sent: \"%s\"", state.operator_name);
	CHECK(hakios_nas_apply(&state, frame, serving_system("Telekom.de", 10)) == 0, "the same again: no change");
}

static void
escape_in_name(void)
{
	static const char name[] = "Op\x1b[2J\x1b]0;x\x07\r\nnet";
	struct hakios_nas_state state = { 0 };

	CHECK(hakios_nas_apply(&state, frame, serving_system(name, sizeof(name) - 1)) == 1, "a change");
	CHECK(all_printable(state.operator_name), "nothing but printable ASCII kept");
	CHECK(strcmp(state.operator_name, "Op?[2J?]0;x???net") == 0, "each such byte a '?': \"%s\"", state.operator_name);
}

static void
utf8_and_zero(void)
{
	static const uint8_t name[] = { 'C', 'a', 'f', 0xc3, 0xa9, 0x7f, ' ', 'M', 0, 'X', 'Y' };
	struct hakios_nas_state state = { 0 };

	CHECK(hakios_nas_apply(&state, frame, serving_system(name, sizeof(name))) == 1, "a change");
	CHECK(strcmp(state.operator_name, "Caf??? M") == 0, "bytes of UTF-8 and DEL a '?', a zero the end: \"%s\"", state.operator_name);
}

static void
long_name(void)
{
	char name[60];
	struct hakios_nas_state state = { 0 };

	memset(name, 'n', sizeof(name));
	name[10] = '\n';
	CHECK(hakios_nas_apply(&state, frame, serving_system(name, sizeof(name))) == 1, "a change");
	CHECK(strlen(state.operator_name) == sizeof(state.operator_name) - 1, "cut to what is kept (%zu)", strlen(state.operator_name));
	CHECK(all_printable(state.operator_name), "nothing but printable ASCII kept");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "plain_name", plain_name }, { "escape_in_name", escape_in_name }, { "utf8_and_zero", utf8_and_zero },
		{ "long_name", long_name },
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
