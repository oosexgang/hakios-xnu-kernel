/*
 * QRTR's framing (drivers/board/qrtr_frame.h), as it is: what another processor sends is
 * taken apart into a header and the place of a datagram, in either version of the header, and nothing
 * that does not fit what arrived is taken.
 *
 *   qrtr_frame_test SCENARIO
 */
#include "fakekernel.h"
#include "board/qrtr_frame.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint8_t message[256];

static void
put32(unsigned at, uint32_t value)
{
	memcpy(message + at, &value, 4);
}

/* version 1, as Linux sends it: eight words, then the datagram */
static uint32_t
version1(uint32_t type, uint32_t size, uint32_t datagram)
{
	memset(message, 0, sizeof(message));
	put32(0, 1);
	put32(4, type);
	put32(8, 5);                    /* from node 5 */
	put32(12, 0x4001);              /* port */
	put32(16, 1);                   /* confirm */
	put32(20, size);
	put32(24, QRTR_US);
	put32(28, 0x4000);
	memset(message + 32, 0xab, datagram);
	return 32 + datagram;
}

/* version 2: four bytes, the size, four numbers of 16 bits, options, then the datagram */
static uint32_t
version2(uint8_t options, uint32_t size, uint32_t datagram)
{
	memset(message, 0, sizeof(message));
	message[0] = 3;
	message[1] = QRTR_DATA;
	message[2] = 1;
	message[3] = options;
	put32(4, size);
	message[8] = 5;
	message[10] = 0xfe, message[11] = 0xff;        /* from the control port */
	message[12] = QRTR_US;
	message[14] = 0x00, message[15] = 0x40;
	memset(message + 16 + options, 0xcd, datagram);
	return 16 + options + datagram;
}

static void
first_version(void)
{
	struct qrtr_header header = { 0 };
	uint32_t at = 0, length = version1(QRTR_DATA, 20, 20);

	CHECK(qrtr_frame(message, length, &header, &at) == QRTR_FRAME_OK, "a datagram of 20");
	CHECK(at == 32 && header.size == 20 && header.type == QRTR_DATA && header.from_node == 5 && header.from_port == 0x4001 &&
	    header.to_port == 0x4000 && header.confirm == 1, "its header as it was sent");
	CHECK(qrtr_frame(message, length + 7, &header, &at) == QRTR_FRAME_OK, "with bytes after it (GLINK fills up to eight)");
	CHECK(qrtr_frame(message, length - 1, &header, &at) == QRTR_FRAME_SIZE_WRONG, "one byte short: not taken");
	length = version1(QRTR_DATA, 0, 0);
	CHECK(qrtr_frame(message, length, &header, &at) == QRTR_FRAME_SIZE_WRONG, "an empty datagram: not taken");
	CHECK(qrtr_frame(message, 31, &header, &at) == QRTR_FRAME_NO_DATAGRAM, "less than a header: no datagram");
}

static void
second_version(void)
{
	struct qrtr_header header = { 0 };
	uint32_t at = 0, length = version2(0, 24, 24);

	CHECK(qrtr_frame(message, length, &header, &at) == QRTR_FRAME_OK, "a datagram of 24");
	CHECK(at == 16 && header.version == 2 && header.size == 24 && header.from_node == 5 && header.to_node == QRTR_US &&
	    header.to_port == 0x4000 && header.confirm == 1, "its header in version 1's shape");
	CHECK(header.from_port == QRTR_CONTROL, "the control port's 16-bit number is the control port");
	length = version2(8, 24, 24);
	CHECK(qrtr_frame(message, length, &header, &at) == QRTR_FRAME_OK && at == 24, "options before the datagram are stepped over");
	length = version2(200, 24, 0);
	CHECK(qrtr_frame(message, 100, &header, &at) == QRTR_FRAME_SIZE_WRONG, "options past what arrived: not taken");
	CHECK(qrtr_frame(message, 15, &header, &at) == QRTR_FRAME_NO_DATAGRAM, "less than its header: no datagram");
	version2(0, 24, 24);
	CHECK(qrtr_frame(message, 16, &header, &at) == QRTR_FRAME_SIZE_WRONG, "its header alone: a header, of a datagram not there");
}

/* sizes that would wrap a sum of offset and size */
static void
sizes_that_wrap(void)
{
	struct qrtr_header header = { 0 };
	uint32_t at = 0, length = version1(QRTR_DATA, 0xfffffff0u, 8);

	CHECK(qrtr_frame(message, length, &header, &at) == QRTR_FRAME_SIZE_WRONG, "version 1, a size of 2^32 - 16: not taken");
	length = version1(QRTR_DATA, 0xffffffe0u, 8);
	CHECK(qrtr_frame(message, length, &header, &at) == QRTR_FRAME_SIZE_WRONG, "one that would make the sum exactly 0: not taken");
	length = version2(255, 0xffffff10u, 0);
	CHECK(qrtr_frame(message, 255, &header, &at) == QRTR_FRAME_SIZE_WRONG, "version 2 with options and such a size: not taken");
}

static void
not_qrtr(void)
{
	struct qrtr_header header = { 0 };
	uint32_t at = 0;

	memset(message, 0, sizeof(message));
	message[0] = 2;
	CHECK(qrtr_frame(message, 64, &header, &at) == QRTR_FRAME_NO_DATAGRAM, "a version that is neither");
	CHECK(qrtr_frame(message, 0, &header, &at) == QRTR_FRAME_NO_DATAGRAM, "nothing at all");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "first_version", first_version }, { "second_version", second_version }, { "sizes_that_wrap", sizes_that_wrap },
		{ "not_qrtr", not_qrtr },
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
