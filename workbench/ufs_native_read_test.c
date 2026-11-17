/*
 * How the kernel's own UFS driver reads (drivers/board/ufshc.c, ufshc_read()), against a model of the
 * controller: its registers memory of the test's own, and a thread that, when slot 0's doorbell bit is
 * set, does what the command in the slot says -- a READ(10) of a unit of made-up bytes, into the buffers
 * its PRD table names -- answers in the slot's response, and clears the bit, at once or a while later.
 * One slot only: the registers are memory here, and a doorbell written with one slot's bit would read as
 * every other slot done. What is tested: the bytes asked for and no others, a bufferful a command, the
 * doorbell rung only while this kernel's list is the one in and running, an answer that is not good taken
 * as an error, and a slow controller waited for by the poller.
 *
 *   ufs_native_read_test SCENARIO
 */
#include <stdarg.h>

#include "fakekernel.h"

/* what the driver says, kept where logged() looks */
void loader_log_putc(char c);

static void
said(const char *format, ...)
{
	char line[768];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wformat"
#define printf said
#define HAKIOS_MMIO_TEST 1
#include "board/ufshc.c"
#undef printf

vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size __unused) { return phys_addr; }
vm_offset_t ml_io_map_unmappable(vm_offset_t p, vm_size_t s __unused, unsigned int f __unused) { return p; }
uint64_t kvtophys(vm_offset_t va) { return va; }
boolean_t pmap_valid_address(uint64_t addr __unused) { return TRUE; }
void FlushPoC_DcacheRegion(vm_offset_t va __unused, size_t length __unused) { }
boolean_t ml_set_interrupts_enabled(boolean_t enable __unused) { return FALSE; }
unsigned int cpu_broadcast_xcall(uint32_t *s __unused, boolean_t x __unused, void (*f)(void *) __unused, void *p __unused) { return 0; }

extern char log_text[];
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- the controller ------------------------------------------------------------------------------------- */

#define LIST_PA         0x8a000000u
#define BUFFER          (16 * BLOCK)            /* a slot's buffer: sixteen blocks a command */
#define MOST            8                       /* slots this test gives the driver at most */

static volatile uint32_t registers[UFSHC_SIZE / 4];
static uint32_t utrd[MOST][8], ucd[MOST][(UCD_PRDT + 16 * 8) / 4];
static uint8_t data[MOST][BUFFER];
static volatile uint32_t take_back;
static volatile unsigned commands, delay_us, doorbells;
static volatile bool backwards;                 /* the commands waiting done the last rung first */
static volatile bool held;                      /* the controller does nothing (another's command in a slot, say) */
static volatile uint8_t answer_status;          /* the SCSI status the unit answers with (0: good) */
static struct { uint8_t lun, slot; uint32_t lba, blocks; } asked[64];

#define REG(r)          registers[(r) / 4]

/* the registers as the controller has them: a write to the doorbell rings the slots whose bits are set in it, and no others */
uint32_t
hakios_test_rd(vm_offset_t address)
{
	return __atomic_load_n(&registers[(address - hc) / 4], __ATOMIC_ACQUIRE);
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	if (address - hc == UFSHC_UTRLDBR) {
		doorbells++;
		__atomic_or_fetch(&REG(UFSHC_UTRLDBR), value, __ATOMIC_RELEASE);
		return;
	}
	__atomic_store_n(&registers[(address - hc) / 4], value, __ATOMIC_RELEASE);
}

/* the made-up bytes of a unit, by their place in it */
static uint8_t
made_up(uint8_t lun, uint64_t at)
{
	return (uint8_t)(at * 7 + at / BLOCK + lun * 31);
}

/* the command in a slot done: its READ(10) into the buffers its PRD table names, its answer in its response */
static void
do_slot(unsigned slot)
{
	const uint8_t *u = (const uint8_t *)ucd[slot];
	uint8_t *r = (uint8_t *)ucd[slot] + UCD_RESPONSE;
	uint32_t lba = (uint32_t)u[18] << 24 | u[19] << 16 | u[20] << 8 | u[21], blocks = (uint32_t)u[23] << 8 | u[24];
	uint32_t entries = utrd[slot][7] & 0xffff;
	uint64_t at = (uint64_t)lba * BLOCK;

	if (commands < 64) {
		asked[commands].lun = u[2];
		asked[commands].slot = (uint8_t)slot;
		asked[commands].lba = lba;
		asked[commands].blocks = blocks;
	}
	commands++;
	for (uint32_t n = 0; n < entries; n++) {
		const uint32_t *prd = ucd[slot] + (UCD_PRDT + 16 * n) / 4;
		uint8_t *to = (uint8_t *)(uintptr_t)((uint64_t)prd[1] << 32 | prd[0]);

		for (uint32_t i = 0; i <= prd[3]; i++) {
			to[i] = made_up(u[2], at++);
		}
	}
	memset(r, 0, 64);
	r[0] = UPIU_RESPONSE;
	r[3] = u[3];
	r[7] = answer_status;
	utrd[slot][2] = OCS_SUCCESS;
}

static void *
controller(void *unused __unused)
{
	for (;;) {
		uint32_t db = __atomic_load_n(&REG(UFSHC_UTRLDBR), __ATOMIC_ACQUIRE);
		unsigned slot;

		if (db == 0 || held) {
			usleep(5);
			continue;
		}
		if (delay_us != 0) {
			usleep(delay_us);       /* (more may be rung meanwhile) */
			db = __atomic_load_n(&REG(UFSHC_UTRLDBR), __ATOMIC_ACQUIRE);
		}
		slot = backwards ? 31 - (unsigned)__builtin_clz(db) : (unsigned)__builtin_ctz(db);
		do_slot(slot);
		__atomic_and_fetch(&REG(UFSHC_UTRLDBR), ~(1u << slot), __ATOMIC_RELEASE);
	}
	return NULL;
}

/* the driver as switch_in leaves it, its list in and running, with SLOTS slots; the controller and the poller started */
static void
in_with(unsigned slots)
{
	pthread_t thread;
	thread_t poller;

	hc = (vm_offset_t)registers;
	REG(UFSHC_UTRLBA) = LIST_PA;
	REG(UFSHC_UTRLBAU) = 0;
	REG(UFSHC_UTRLRSR) = 1;
	hc_list_lo = LIST_PA;
	hc_list_hi = 0;
	hc_loader.take_back = &take_back;
	for (unsigned i = 0; i < slots; i++) {
		hc_slots[i] = (struct hc_slot){ .number = i, .utrd = utrd[i], .ucd = ucd[i], .ucd_pa = (uint64_t)(uintptr_t)ucd[i],
			                        .data = data[i], .data_pa = (uint64_t)(uintptr_t)data[i] };
	}
	hc_free = slots == 32 ? ~0u : (1u << slots) - 1;
	hc_slot_buffer = BUFFER;
	ufshc_set_luns(1u << 0 | 1u << 3);
	hc_usable = hc_in = true;
	pthread_create(&thread, NULL, controller, NULL);
	if (kernel_thread_start(hc_poller, NULL, &poller) == KERN_SUCCESS) {
		thread_deallocate(poller);
	}
}

static void
in(void)
{
	in_with(1);
}

static bool
is_unit(uint8_t lun, uint64_t at, const uint8_t *bytes, uint64_t length)
{
	for (uint64_t i = 0; i < length; i++) {
		if (bytes[i] != made_up(lun, at + i)) {
			return false;
		}
	}
	return true;
}

/* ---- scenarios ------------------------------------------------------------------------------------------ */

/* part of a block, then two more: one command for the three blocks that hold them, the bytes asked for and no others */
static void
a_read(void)
{
	static uint8_t out[10002];

	in();
	memset(out, 0x5a, sizeof(out));
	int error = ufshc_read(3, 5 * BLOCK + 100, out + 1, 10000);

	CHECK(error == 0, "read: %d: %s", error, log_text);
	CHECK(is_unit(3, 5 * BLOCK + 100, out + 1, 10000), "the bytes of unit 3 from there");
	CHECK(out[0] == 0x5a && out[10001] == 0x5a, "and nothing either side");
	CHECK(commands == 1 && asked[0].lun == 3 && asked[0].lba == 5 && asked[0].blocks == 3, "one command: unit %u, block %u, %u blocks",
	    asked[0].lun, asked[0].lba, asked[0].blocks);
	CHECK(hc_requests == 1 && hc_bytes == 3 * BLOCK && hc_errors == 0, "counted: %llu, %llu bytes", (unsigned long long)hc_requests,
	    (unsigned long long)hc_bytes);
	CHECK(hc_free == 1 && hc_in_flight == 0, "the slot free again");
}

/* more than a bufferful: a command a bufferful, one after another, all of it there */
static void
a_long_read(void)
{
	static uint8_t out[40 * BLOCK];

	in();
	CHECK(ufshc_read(0, 7 * BLOCK, out, sizeof(out)) == 0, "read: %s", log_text);
	CHECK(is_unit(0, 7 * BLOCK, out, sizeof(out)), "all of it");
	CHECK(commands == 3 && asked[0].lba == 7 && asked[0].blocks == 16 && asked[1].lba == 23 && asked[1].blocks == 16 &&
	    asked[2].lba == 39 && asked[2].blocks == 8, "16, 16 and 8 blocks: %u commands", commands);
	CHECK(hc_most_in_flight == 1, "one in flight at a time with one slot: %llu", (unsigned long long)hc_most_in_flight);
}

/* the doorbell only while this kernel's list is the one in and running and the loader does not take it back */
static void
rung_only_while_in(void)
{
	static uint8_t out[BLOCK];
	static const char *const ways[] = { "another list's address", "another list's high half", "the list not running",
		                            "the loader taking it back", "a doorbell bit set already" };

	in();
	held = true;
	for (unsigned i = 0; i < sizeof(ways) / sizeof(ways[0]); i++) {
		REG(UFSHC_UTRLBA) = i == 0 ? LIST_PA + 0x1000 : LIST_PA;
		REG(UFSHC_UTRLBAU) = i == 1 ? 1 : 0;
		REG(UFSHC_UTRLRSR) = i == 2 ? 0 : 1;
		take_back = i == 3;
		hc_slots[0].number = i == 4 ? 1 : 0;    /* (its bit, 2, set below: as if another kernel's command were there) */
		REG(UFSHC_UTRLDBR) = i == 4 ? 2 : 0;
		CHECK(ufshc_read(0, 0, out, sizeof(out)) == ENXIO && commands == 0, "%s: not rung (%u commands)", ways[i], commands);
		CHECK(hc_free == 1 && hc_in_flight == 0, "%s: the slot free again", ways[i]);
	}
	hc_slots[0].number = 0;
	REG(UFSHC_UTRLDBR) = 0;
	held = false;
	REG(UFSHC_UTRLBA) = LIST_PA;
	REG(UFSHC_UTRLBAU) = 0;
	REG(UFSHC_UTRLRSR) = 1;
	take_back = 0;
	CHECK(ufshc_read(0, 0, out, sizeof(out)) == 0 && commands == 1, "and rung when all is as it should be");
	hc_in = false;
	CHECK(ufshc_read(0, 0, out, sizeof(out)) == ENXIO && commands == 1, "nor once the list is out");
}

/* several slots: as many reads in flight as there are slots, done in whatever order, the bytes in theirs */
static void
reads_in_flight(void)
{
	static uint8_t out[10 * BUFFER + 100];
	bool used[MOST] = { false };
	unsigned slots_used = 0;

	in_with(4);
	backwards = true;                       /* (the last rung done first) */
	delay_us = 2000;
	CHECK(ufshc_read(0, 3 * BLOCK + 100, out, sizeof(out)) == 0, "read: %s", log_text);
	CHECK(is_unit(0, 3 * BLOCK + 100, out, sizeof(out)), "all of it, in its order");
	CHECK(commands == 11, "a bufferful a command: %u", commands);
	for (unsigned i = 0; i < commands && i < 64; i++) {
		slots_used += !used[asked[i].slot];
		used[asked[i].slot] = true;
	}
	CHECK(hc_most_in_flight == 4 && slots_used == 4, "four in flight at once, through the four slots: %llu, %u",
	    (unsigned long long)hc_most_in_flight, slots_used);
	CHECK(asked[0].lba > asked[3].lba || asked[1].lba > asked[2].lba, "and done out of order: %u %u %u %u", asked[0].lba,
	    asked[1].lba, asked[2].lba, asked[3].lba);
	CHECK(doorbells == commands, "each slot rung once, by its own bit: %u doorbells", doorbells);
	CHECK(hc_free == 0xf && hc_in_flight == 0 && hc_waiting == 0, "all free again: %#x", hc_free);
}

/* two readers at once, with fewer slots than both would take: each gets its own bytes */
static void *
reader(void *lun)
{
	static uint8_t out[2][6 * BUFFER];
	uintptr_t n = (uintptr_t)lun;

	return ufshc_read((uint8_t)(n ? 3 : 0), n * BUFFER + 512, out[n ? 1 : 0], sizeof(out[0])) == 0 &&
	       is_unit((uint8_t)(n ? 3 : 0), n * BUFFER + 512, out[n ? 1 : 0], sizeof(out[0])) ? (void *)1 : NULL;
}

static void
two_readers(void)
{
	pthread_t a, b;
	void *ok_a, *ok_b;

	in_with(3);
	delay_us = 500;
	pthread_create(&a, NULL, reader, (void *)0);
	pthread_create(&b, NULL, reader, (void *)1);
	pthread_join(a, &ok_a);
	pthread_join(b, &ok_b);
	CHECK(ok_a != NULL && ok_b != NULL, "both read what they asked for: %s", log_text);
	CHECK(hc_most_in_flight <= 3 && hc_free == 0x7 && hc_in_flight == 0, "never more than the slots: %llu",
	    (unsigned long long)hc_most_in_flight);
}

/* an answer that is not good: an error, counted, and nothing copied */
static void
a_bad_answer(void)
{
	static uint8_t out[BLOCK];

	in();
	answer_status = 0x02;                   /* CHECK CONDITION */
	memset(out, 0x5a, sizeof(out));
	CHECK(ufshc_read(0, 0, out, sizeof(out)) == EIO, "an error");
	CHECK(hc_errors == 1 && hc_requests == 0 && out[0] == 0x5a && out[BLOCK - 1] == 0x5a, "counted, nothing copied");
	CHECK(logged("ufs: native: slot 0 (LUN 0, blocks 0..0)"), "said: %s", log_text);
	CHECK(hc_free == 1, "the slot free again");
}

/* a controller slower than the spin: the waiter sleeps, the poller sees the slot done and wakes it */
static void
a_slow_controller(void)
{
	static uint8_t out[3 * BLOCK];

	in();
	delay_us = 3000;
	CHECK(ufshc_read(0, BLOCK, out, sizeof(out)) == 0 && is_unit(0, BLOCK, out, sizeof(out)), "read: %s", log_text);
	CHECK(hc_sleeps >= 1 && hc_poller_rounds >= 1, "slept, and the poller looked: %llu, %llu", (unsigned long long)hc_sleeps,
	    (unsigned long long)hc_poller_rounds);
	CHECK(hc_waiting == 0 && hc_done == 0, "nobody left waiting: %#x, %#x", hc_waiting, hc_done);
}

/* nothing at all before the driver is usable, or once it is off */
static void
not_usable(void)
{
	static uint8_t out[BLOCK];

	in();
	hc_usable = false;
	CHECK(ufshc_read(0, 0, out, sizeof(out)) == ENXIO, "not usable");
	hc_usable = true;
	hc_dead = true;
	CHECK(ufshc_read(0, 0, out, sizeof(out)) == ENXIO, "off");
	hc_dead = false;
	hc_closed = true;
	CHECK(ufshc_read(0, 0, out, sizeof(out)) == ENXIO && commands == 0, "closed: nothing rung");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "a_read", a_read }, { "a_long_read", a_long_read }, { "rung_only_while_in", rung_only_while_in },
		{ "a_bad_answer", a_bad_answer }, { "reads_in_flight", reads_in_flight }, { "two_readers", two_readers }, { "a_slow_controller", a_slow_controller }, { "not_usable", not_usable },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
