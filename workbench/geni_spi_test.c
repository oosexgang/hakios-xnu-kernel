/*
 * SPI through a serial engine (drivers/board/geni_spi.h), the part the touch screens' drivers
 * share, against a model of the engine in its FIFO mode: what it is set up with, and transfers through
 * it -- bytes out a word at a time and never more than its FIFO holds, the device's bytes in, a last word
 * not full, an engine that says an error, one that is busy, and one that does not take a cancel.
 *
 *   geni_spi_test SCENARIO
 */
#include "fakekernel.h"

extern char log_text[];

/* ---- the engine ------------------------------------------------------------------------------------- */

#define SE_AT           0x00a8c000u
#define DEPTH           16              /* the FIFOs' words */

static uint32_t regs[0x4000 / 4];
static struct {
	unsigned protocol, depth;
	bool no_fifo;
	uint32_t errors;                /* error bits it says once a command runs */
	bool busy;                      /* a command runs that never ends */
	bool deaf;                      /* a cancel and an abort are not taken */
	bool running;
	uint32_t length, sent, made;    /* the command's bytes; of them sent out, and answered into the RX FIFO */
	uint8_t out[256];               /* what went out */
	uint32_t tx_words, most_tx_words;       /* the TX FIFO's words not yet sent, and the most there were */
	uint32_t rx[64], rx_head, rx_tail;      /* the RX FIFO */
	uint32_t status;                /* the command's interrupt status */
	unsigned cancels, aborts;
} engine = { .protocol = 1, .depth = DEPTH };

static uint32_t *
reg(uint32_t offset)
{
	return &regs[offset / 4];
}

/* the device: each byte that comes is answered with its inverse */
static uint8_t
device(uint8_t byte)
{
	return (uint8_t)~byte;
}

/* the TX FIFO's words go out on the wire, the device's answers into the RX FIFO, the last word a part one */
static void
wire(void)
{
	while (engine.tx_words > 0) {
		engine.tx_words--;
		for (int j = 0; j < 4 && engine.made < engine.length; j++, engine.made++) {
			uint8_t byte = engine.out[engine.made];

			if (engine.made % 4 == 0) {
				engine.rx[engine.rx_tail % 64] = 0;
			}
			engine.rx[engine.rx_tail % 64] |= (uint32_t)device(byte) << (8 * (engine.made % 4));
			if (engine.made % 4 == 3 || engine.made + 1 == engine.length) {
				engine.rx_tail++;
			}
		}
	}
	if (engine.running && engine.made == engine.length && engine.errors == 0) {
		engine.running = false;
		engine.status |= 1;             /* M_CMD_DONE */
	}
}

static uint32_t
rd(vm_offset_t address)
{
	uint32_t offset = (uint32_t)(address - SE_AT);

	switch (offset) {
	case 0x068: return engine.protocol << 8;
	case 0x064: return engine.no_fifo;
	case 0xe24: return engine.depth << 16;
	case 0x040: return engine.running || engine.busy;
	case 0x800: return engine.tx_words;
	case 0x610:
		wire();
		return engine.status | engine.errors;
	case 0x804: {
		uint32_t words = engine.rx_tail - engine.rx_head, status = words;

		if (words > 0 && engine.made == engine.length && engine.length % 4 != 0) {
			status |= 0x80000000u | (engine.length % 4) << 28;     /* the last word, so many bytes of it */
		}
		return status;
	}
	case 0x780: return engine.rx_head < engine.rx_tail ? engine.rx[engine.rx_head++ % 64] : 0;
	}
	return *reg(offset);
}

static void
wr(vm_offset_t address, uint32_t value)
{
	uint32_t offset = (uint32_t)(address - SE_AT);

	*reg(offset) = value;
	switch (offset) {
	case 0x600:                             /* a command: full duplex, so many bytes */
		engine.running = true;
		engine.length = *reg(0x26c);
		engine.sent = engine.made = 0;
		engine.rx_head = engine.rx_tail = 0;
		break;
	case 0x618:                             /* status cleared */
		engine.status &= ~value;
		break;
	case 0x604:
		if (!engine.deaf) {
			engine.status |= value & 4 ? 1u << 4 : value & 2 ? 1u << 5 : 0;
			engine.running = engine.busy = false;
		}
		engine.cancels += (value & 4) != 0;
		engine.aborts += (value & 2) != 0;
		break;
	case 0x700:                             /* a word out */
		for (int j = 0; j < 4 && engine.sent < engine.length && engine.sent < sizeof(engine.out); j++, engine.sent++) {
			engine.out[engine.sent] = (uint8_t)(value >> (8 * j));
		}
		engine.tx_words++;
		if (engine.tx_words > engine.most_tx_words) {
			engine.most_tx_words = engine.tx_words;
		}
		break;
	}
}

static void sleep_us(uint32_t us) { (void)us; }

static vm_offset_t se = SE_AT;
static uint32_t se_source_hz = 19200000, se_clock_select = 2;

static void __printflike(1, 2)
said(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

#define printf said
#define XO_HZ 19200000
#define GENI_SPI_NAME "touch"
#include "board/geni_spi.h"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- scenarios ---------------------------------------------------------------------------------------- */

/* set up for SPI: mode 0, eight bits, the FIFO's watermarks by its depth, the clock divided down to the rate asked */
static void
set_up(void)
{
	CHECK(geni_spi_init(4800000) == NULL, "set up");
	CHECK(fifo_depth == DEPTH, "its FIFO's depth: %u", fifo_depth);
	CHECK(*reg(0x810) == DEPTH - 3 && *reg(0x814) == DEPTH - 2 && *reg(0x80c) == 0, "the watermarks: %u, %u", *reg(0x810), *reg(0x814));
	CHECK(*reg(0x048) == (4u << 4 | 1) && *reg(0x07c) == 2, "19.2 MHz divided by 4, preset 2: %#x", *reg(0x048));
	CHECK(*reg(0x268) == 4 && *reg(0x224) == 0 && *reg(0x230) == 0 && *reg(0x22c) == 0, "eight bits, mode 0, no loopback");
	CHECK(*reg(0x260) == 0x0007f8fe && *reg(0x284) == 0x0007f8fe, "four bytes a word, the first lowest");
	CHECK(geni_spi_init(5000000) == NULL && *reg(0x048) == (4u << 4 | 1), "5 MHz asked: not over it, 4.8");
	CHECK(geni_spi_init(40000000) == NULL && *reg(0x048) == (1u << 4 | 1), "more than the clock: undivided");
}

/* the engine's clock's rate, as its RCG's registers make it: the crystal, GPLL0 or its half, divided by halves, M/N */
static void
clock_rates(void)
{
	CHECK(rcg_rate(0, 0, 0) == 19200000, "the crystal: %u", rcg_rate(0, 0, 0));
	CHECK(rcg_rate(1u << 8, 0, 0) == 600000000, "GPLL0");
	CHECK(rcg_rate(6u << 8, 0, 0) == 300000000, "GPLL0 halved");
	CHECK(rcg_rate(1u << 8 | 3, 0, 0) == 300000000, "GPLL0 divided by 2 (a divider of 3: 2 x 2 - 1)");
	CHECK(rcg_rate(1u << 8 | 4, 0, 0) == 240000000, "... by 2.5 (4)");
	CHECK(rcg_rate(6u << 8 | 2u << 12 | 1, 1, (uint32_t)~(25u - 1)) == 12000000, "GPLL0 halved, M/N 1/25: 12 MHz: %u",
	    rcg_rate(6u << 8 | 2u << 12 | 1, 1, (uint32_t)~(25u - 1)));
	CHECK(rcg_rate(2u << 12, 1, (uint32_t)~(0u)) == 19200000, "an M/N of N 0 (NOT(N - M) all ones, N = 0): undivided");
	CHECK(rcg_rate(3u << 8, 0, 0) == 0, "a source this does not know: none");
}

/* an engine that cannot be used for this: said, and nothing set up */
static void
not_for_spi(void)
{
	engine.protocol = 2;
	CHECK(geni_spi_init(4800000) != NULL && strstr(geni_spi_init(4800000), "no SPI firmware"), "I2C's firmware");
	engine.protocol = 1;
	engine.no_fifo = true;
	CHECK(geni_spi_init(4800000) != NULL && strstr(geni_spi_init(4800000), "disabled"), "no FIFO interface");
	engine.no_fifo = false;
	engine.depth = 3;
	CHECK(geni_spi_init(4800000) != NULL && strstr(geni_spi_init(4800000), "no FIFO"), "a FIFO of three words");
	engine.depth = 4;
	CHECK(geni_spi_init(4800000) == NULL, "of four, the least: taken");
}

/* bytes out and in, whatever their number: whole words and a part one */
static void
transfers(void)
{
	static const uint32_t lengths[] = { 1, 3, 4, 5, 8, 10, 63, 64, 200 };
	uint8_t tx[256], rx[256];

	CHECK(geni_spi_init(4800000) == NULL, "set up");
	for (unsigned n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
		uint32_t length = lengths[n];

		for (uint32_t i = 0; i < length; i++) {
			tx[i] = (uint8_t)(i * 13 + n);
		}
		memset(rx, 0x55, sizeof(rx));
		engine.most_tx_words = 0;
		CHECK(geni_transfer(tx, rx, length), "%u bytes", length);
		CHECK(engine.length == length && memcmp(engine.out, tx, length) == 0, "%u bytes out, as given", length);
		for (uint32_t i = 0; i < length; i++) {
			CHECK(rx[i] == (uint8_t)~tx[i], "%u bytes: byte %u in: %#x", length, i, rx[i]);
		}
		CHECK(rx[length] == 0x55, "%u bytes: not a byte more in", length);
		CHECK(engine.most_tx_words < DEPTH, "%u bytes: the TX FIFO never over its depth: %u words", length, engine.most_tx_words);
	}
}

/* nothing to send: zeros go; nothing to keep: the answers are taken and let go */
static void
one_way(void)
{
	uint8_t rx[8], tx[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	CHECK(geni_spi_init(4800000) == NULL, "set up");
	CHECK(geni_transfer(NULL, rx, 6) && memcmp(engine.out, "\0\0\0\0\0\0", 6) == 0 && rx[0] == 0xff && rx[5] == 0xff, "zeros out");
	CHECK(geni_transfer(tx, NULL, 8) && memcmp(engine.out, tx, 8) == 0 && engine.rx_head == engine.rx_tail, "nothing kept, all taken");
}

/* an engine that says an error: the transfer fails, is said, and the engine is cancelled for the next */
static void
an_error(void)
{
	uint8_t rx[8];

	CHECK(geni_spi_init(4800000) == NULL, "set up");
	engine.errors = 1u << 2;
	CHECK(!geni_transfer(NULL, rx, 8), "failed");
	CHECK(strstr(log_text, "touch: SPI transfer of 8 bytes failed: status 0x4") != NULL, "said: %s", log_text);
	CHECK(engine.cancels == 1 && engine.aborts == 0, "cancelled: %u, %u", engine.cancels, engine.aborts);
	engine.errors = 0;
	CHECK(geni_transfer(NULL, rx, 8), "and the next goes");
	CHECK(engine.status == 0, "its status cleared after it: %#x", engine.status);
}

/* an engine busy with a command that does not end, and one that takes no cancel: cancelled, then aborted */
static void
busy_and_deaf(void)
{
	uint8_t rx[4];

	CHECK(geni_spi_init(4800000) == NULL, "set up");
	engine.busy = true;
	CHECK(geni_transfer(NULL, rx, 4), "busy: cancelled, then the transfer goes");
	CHECK(engine.cancels == 1, "one cancel: %u", engine.cancels);
	engine.deaf = true;
	engine.errors = 1u << 3;
	CHECK(!geni_transfer(NULL, rx, 4), "an error");
	CHECK(engine.cancels == 2 && engine.aborts == 1, "a cancel not taken: aborted: %u, %u", engine.cancels, engine.aborts);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "set_up", set_up }, { "clock_rates", clock_rates }, { "not_for_spi", not_for_spi }, { "transfers", transfers }, { "one_way", one_way },
		{ "an_error", an_error }, { "busy_and_deaf", busy_and_deaf },
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
