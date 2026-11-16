/*
 * A request to RPMh as the drivers share it (drivers/board/rpmh.h), by itself, against
 * the model of a sequencer's first slot set (rsc_model.h): what is sent, and that the slot set is
 * left as it was found whatever RPMh does.
 *
 *   rpmh_test SCENARIO
 */
#include "fakekernel.h"
#include "rsc_model.h"

static uint32_t rd(vm_offset_t address) { return rsc_model_rd(address); }
static void wr(vm_offset_t address, uint32_t value) { rsc_model_wr(address, value); }

#define KERNEL 1
#include "board/rpmh.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define SAME(a, b) ((a) != NULL && strcmp((a), (b)) == 0)
#define TCS             0x18220d00u

static void
sequencer(void)
{
	rsc.base = TCS;
	rsc.irq_enable = 0x6;                   /* (other slot sets' interrupts: not ours to change) */
	fake_delay_divisor = 20;
}

static void
as_found(void)
{
	CHECK(rsc.irq_enable == 0x6 && rsc.wait == 0 && rsc.enabled == 0 && (rsc.control & (1u << 24 | 1u << 16)) == 0 &&
	    rsc.irq_status == 0, "the slot set is not as it was found: interrupts %#x, wait %#x, commands %#x, control %#x, status %#x",
	    rsc.irq_enable, rsc.wait, rsc.enabled, rsc.control, rsc.irq_status);
	CHECK(rsc.elsewhere == 0, "%u accesses where nothing of the slot set is", rsc.elsewhere);
	CHECK(rsc.changed_while_sent == 0, "%u writes to the commands while the slot set was triggered", rsc.changed_while_sent);
}

/* three writes: sent together, each as a write of four bytes whose answer is waited for */
static void
three_writes(void)
{
	static const uint32_t address[3] = { 0x41100, 0x41104, 0x41108 }, data[3] = { 1800, 1, 7 };

	sequencer();
	CHECK(hakios_rpmh_send(TCS, address, data, 3) == NULL, "done");
	CHECK(rsc.triggers == 1 && rsc.sends == 3, "one request of three: %u, %u", rsc.triggers, rsc.sends);
	CHECK(rsc.wait_at_trigger == 0x7, "the answers of all three waited for, and of no other: %#x", rsc.wait_at_trigger);
	for (unsigned i = 0; i < 3; i++) {
		CHECK(rsc.sent[i].address == address[i] && rsc.sent[i].data == data[i] && rsc.sent[i].message == 0x10108,
		    "command %u: %#x <- %u, message %#x", i, rsc.sent[i].address, rsc.sent[i].data, rsc.sent[i].message);
	}
	as_found();
}

/* one, and then another: the second does not carry the first's commands */
static void
one_after_another(void)
{
	static const uint32_t first[2] = { 0x50000, 0x50004 }, values[2] = { 5, 6 }, second = 0x30000, level = 3;

	sequencer();
	CHECK(hakios_rpmh_send(TCS, first, values, 2) == NULL && hakios_rpmh_send(TCS, &second, &level, 1) == NULL, "both done");
	CHECK(rsc.triggers == 2 && rsc.sends == 3 && rsc.sent[2].address == 0x30000 && rsc.sent[2].data == 3,
	    "two and then one: %u commands in %u requests", rsc.sends, rsc.triggers);
	as_found();
}

/* what is not a request is not sent as one */
static void
not_a_request(void)
{
	uint32_t address[17] = { 0 }, data[17] = { 0 };

	sequencer();
	CHECK(SAME(hakios_rpmh_send(TCS, address, data, 0), RPMH_SAYS_NOT_ONE), "nothing");
	CHECK(SAME(hakios_rpmh_send(TCS, address, data, 17), RPMH_SAYS_NOT_ONE), "more than a slot set holds");
	CHECK(rsc.writes == 0 && rsc.triggers == 0, "nothing written (%u)", rsc.writes);
	for (unsigned i = 0; i < 16; i++) {
		address[i] = 0x40000 + 4 * i;
	}
	CHECK(hakios_rpmh_send(TCS, address, data, 16) == NULL && rsc.triggers == 1 && rsc.wait == 0 && rsc.enabled == 0, "sixteen is one");
}

/* RPMh never says done: said after a fifth of a second, and the slot set cleaned up */
static void
no_answer(void)
{
	static const uint32_t address = 0x41100, data = 1;
	const char *problem;
	uint64_t start = mach_absolute_time(), taken;

	sequencer();
	fake_delay_divisor = 1;
	rsc.deaf = true;
	problem = hakios_rpmh_send(TCS, &address, &data, 1);
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &taken);
	CHECK(SAME(problem, RPMH_SAYS_NO_ANSWER), "said: %s", problem ? problem : "done");
	CHECK(taken >= 150000000ull && taken < 5000000000ull, "after about a fifth of a second: %llu ms", (unsigned long long)(taken / 1000000));
	CHECK(rsc.triggers == 1, "it was sent");
	as_found();
}

/* a slot set in use is somebody's: not one write into it */
static void
in_use(void)
{
	static const uint32_t address = 0x41100, data = 1;

	sequencer();
	rsc.busy = true;
	CHECK(SAME(hakios_rpmh_send(TCS, &address, &data, 1), RPMH_SAYS_BUSY), "said");
	CHECK(rsc.writes == 0 && rsc.triggers == 0, "nothing written (%u)", rsc.writes);
}

/* a trigger left set by whoever was here before (a boot loader does) is taken down before the commands change */
static void
trigger_left_set(void)
{
	static const uint32_t address = 0x41100, data = 1;

	sequencer();
	rsc.control = 1u << 24 | 1u << 16;
	CHECK(hakios_rpmh_send(TCS, &address, &data, 1) == NULL && rsc.triggers == 1 && rsc.sends == 1, "one request, of ours: %u, %u",
	    rsc.triggers, rsc.sends);
	as_found();
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "three_writes", three_writes }, { "one_after_another", one_after_another }, { "not_a_request", not_a_request },
		{ "no_answer", no_answer }, { "in_use", in_use }, { "trigger_left_set", trigger_left_set },
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
