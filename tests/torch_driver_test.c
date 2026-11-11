/*
 * The flashlight's driver itself (kernel/overlay/bsd/dev/redwood_torch.c), run as a host program
 * against the model of the PMIC arbiter (spmi_model.h) with a flash LED module and the PMIC's
 * temperature alarm behind it. This driver writes to a PMIC, which makes the phone's supplies, so what
 * is tested first is what it must never do: a write to a register that is not one of the module's six,
 * a current above Android's, a light that stays on when something is wrong or when the run ends.
 * Where the arbiter is, and where writes go, is the SoC's data: the model is built from the board the
 * driver reads.
 *
 *   torch_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <time.h>
#include <unistd.h>

#include "fakekernel.h"
#include "spmi_model.h"

extern char log_text[];

/* ---- the hardware's model: the flash module and the temperature alarm ---------------------------- */

#define REG_STATUS1     0x06
#define REG_STATUS2     0x07
#define REG_RT_STS      0x10
#define REG_TIMER0      0x3e
#define REG_ITARGET0    0x42
#define REG_MODULE_EN   0x46
#define REG_IRES        0x49
#define REG_STROBE0     0x4a
#define REG_EN_LED      0x4e

static struct spmi_peripheral flash_model, alarm_model;
static unsigned forbidden_writes;       /* to a register that is not one of the six */
static unsigned highest_target;         /* the highest current code ever written */
static unsigned times_lit;              /* the channel switched on with the module enabled */
static bool stuck;                      /* the module does not take what is written (a write that does not read back) */
static bool lit_too_bright;             /* lit with a resolution or a target that is not Android's */

static bool
is_lit(void)
{
	return (flash_model.reg[REG_MODULE_EN] & 0x80) && (flash_model.reg[REG_EN_LED] & 0x01);
}

static void
flash_written(struct spmi_peripheral *p, uint8_t reg, uint8_t value)
{
	bool was = is_lit();

	if (reg != REG_TIMER0 && reg != REG_ITARGET0 && reg != REG_MODULE_EN && reg != REG_IRES && reg != REG_STROBE0 &&
	    reg != REG_EN_LED) {
		forbidden_writes++;
		fprintf(stderr, "MODEL: a write to register %#04x of the flash module, which is not one of the six\n", reg);
	}
	if (reg == REG_ITARGET0 && (value & 0x7fu) > highest_target) {
		highest_target = value & 0x7f;
	}
	if (!stuck) {
		p->reg[reg] = value;
	}
	if (is_lit()) {
		if (!was) {
			times_lit++;
		}
		/* (channel 0 in 12.5 mA steps, at most Android's code 9: 125 mA) */
		lit_too_bright |= (p->reg[REG_IRES] & 0x01) || (p->reg[REG_ITARGET0] & 0x7f) > 9;
		p->reg[REG_RT_STS] |= 0x10;     /* ramped up */
	} else {
		p->reg[REG_RT_STS] &= (uint8_t)~0x10;
	}
}

static bool
lit_now(void)
{
	bool lit;

	pthread_mutex_lock(&spmi.lock);
	lit = is_lit();
	pthread_mutex_unlock(&spmi.lock);
	return lit;
}

uint32_t torch_test_rd(vm_offset_t address) { return spmi_model_rd(address); }
void torch_test_wr(vm_offset_t address, uint32_t value) { spmi_model_wr(address, value); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/* the battery's temperature, which the driver asks for (bsd/dev/redwood_battery.c in the kernel) */
static int battery_c = 30;
int redwood_battery_temperature(void) { return battery_c; }

/* the one lock of the bus is the buttons' driver's (bsd/dev/redwood_buttons.c) */
static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;
void redwood_spmi_lock(void) { pthread_mutex_lock(&bus); }
void redwood_spmi_unlock(void) { pthread_mutex_unlock(&bus); }

/* The driver's includes are the kernel's: the fake kernel answers for them. (Its uint64_t is an unsigned
 * long long, which is what it prints it as; and two of its own names are taken on a host: get() by the
 * fake kernel, alarm() by the C library.) */
#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define TORCH_TEST_MMIO 1
#define get torch_get
#define alarm torch_alarm
#include "hakios_board.c"
#include "redwood_torch.c"
#undef get
#undef alarm

static int failures;
/* (to stderr: this driver also prints every line of its log, which main() sends nowhere) */
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* ---- a phone with the part: the arbiter where its SoC has it, the module as the firmware leaves it ---- */

static void
phone(const char *name)
{
	const struct hakios_board *b;

	board = name;
	b = hakios_board();
	if (b == NULL) {
		return;
	}
	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	flash_model.id = 0x2ee;                 /* the PM8350C: slave 2, registers 0xee00 */
	alarm_model.id = 0x20a;
	flash_model.written = flash_written;
	/* the module has a channel of another system's first, as real tables do; the alarm is only read */
	spmi_channel(11, &flash_model, 3);
	spmi_channel(12, &alarm_model, 3);
	spmi_channel(20, &flash_model, spmi.ee);
	flash_model.reg[0x04] = 0x18;           /* a flash LED module */
	flash_model.reg[0x05] = 0x07;           /* of four channels */
	flash_model.reg[REG_STROBE0] = 0x24;    /* as a boot loader may leave it: a hardware strobe */
	flash_model.reg[REG_TIMER0] = 0x8f;     /* ... and the safety timer on */
	flash_model.reg[REG_IRES] = 0x0f;       /* ... and the fine resolution on every channel */
	alarm_model.reg[0x05] = 0x09;           /* the second generation: the stage in [6:4] */
}

static void
never(void)
{
	CHECK(forbidden_writes == 0, "%u writes to registers that are not the module's six", forbidden_writes);
	CHECK(highest_target <= 9, "a current code of %u, above Android's 9 (125 mA)", highest_target);
	CHECK(!lit_too_bright, "lit with a resolution or a target that is not Android's");
	CHECK(alarm_model.writes == 0, "%u writes to the temperature alarm", alarm_model.writes);
	CHECK(spmi.elsewhere == 0, "%u accesses where nothing of the arbiter is", spmi.elsewhere);
}

static void
dark_at_the_end(void)
{
	redwood_torch_quiesce();
	CHECK(!lit_now(), "the light is on at the run's end");
	CHECK(spmi_get(&flash_model, REG_MODULE_EN) == 0 && (spmi_get(&flash_model, REG_EN_LED) & 0x01) == 0 &&
	    (spmi_get(&flash_model, REG_ITARGET0) & 0x7f) == 0, "the module is not as it was found: enable %#x, channels %#x, target %#x",
	    spmi_get(&flash_model, REG_MODULE_EN), spmi_get(&flash_model, REG_EN_LED), spmi_get(&flash_model, REG_ITARGET0));
}

static void
ask_for(uint32_t permille)
{
	CHECK(fake_dev_write("redwood-torch", &permille, sizeof(permille)) == 0, "/dev/redwood-torch takes a level");
}

static uint32_t
shown_level(void)
{
	uint32_t value = 0xffffffff;

	CHECK(fake_dev_read("redwood-torch", &value, sizeof(value)) == 0, "/dev/redwood-torch is read");
	return value;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* torch=1: the module is found and read, and nothing is written */
static void
look_only(void)
{
	phone("phone-video");
	boot_args = "torch=1";
	redwood_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("channel 20 is the PM8350C's flash LED module (0x2ee00); written by system 0, which is us"),
	    "the module's channel that is this system's: %s", log_text);
	CHECK(logged("a flash LED module of four channels, as Android's driver expects"), "its type is read");
	CHECK(logged("the PMIC's temperature alarm (subtype 0x9) says 0: stage 0 (all is well)"), "the alarm is read");
	CHECK(spmi.writes == 0 && spmi.denied == 0, "a look writes nothing (%u writes, %u refused)", spmi.writes, spmi.denied);
	never();
}

/* torch=2: on once at Android's current, with Android's writes, then off and as it was found */
static void
light_once(void)
{
	phone("phone-video");
	boot_args = "torch=2 torch_secs=1";
	redwood_torch_init();
	WAIT_FOR(lit_now(), 5000);
	CHECK(lit_now(), "the light comes on: %s", log_text);
	CHECK(spmi_get(&flash_model, REG_ITARGET0) == 9 && (spmi_get(&flash_model, REG_IRES) & 0x01) == 0,
	    "at Android's 125 mA: target %u, resolution %#x", spmi_get(&flash_model, REG_ITARGET0), spmi_get(&flash_model, REG_IRES));
	CHECK((spmi_get(&flash_model, REG_STROBE0) & 0x74) == 0 && (spmi_get(&flash_model, REG_TIMER0) & 0x80) == 0,
	    "strobed by software, the safety timer off, as Android's probe leaves it");
	CHECK((spmi_get(&flash_model, REG_IRES) & 0x0e) == 0x0e && (spmi_get(&flash_model, REG_TIMER0) & 0x7f) == 0x0f,
	    "the other channels' bits are left as they were");
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("off again after 1 s"), "and goes off after the time asked for");
	CHECK(!lit_now() && times_lit == 1, "dark again, lit once (%u)", times_lit);
	never();
	dark_at_the_end();
}

/* the same part on another chip: the arbiter elsewhere, this system its third. Reads and writes go where
 * the SoC file says, through the channel that is this system's. */
static void
another_chip(void)
{
	phone("phone-keys");
	boot_args = "torch=2 torch_secs=1";
	redwood_torch_init();
	WAIT_FOR(lit_now(), 5000);
	CHECK(logged("channel 20 is the PM8350C's flash LED module (0x2ee00); written by system 2, which is us"),
	    "the channel of this system, which is the arbiter's third: %s", log_text);
	CHECK(lit_now() && spmi_get(&flash_model, REG_ITARGET0) == 9, "lit at Android's current");
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(!lit_now() && times_lit == 1 && spmi.denied == 0, "and dark again; no write was refused (%u)", spmi.denied);
	never();
	dark_at_the_end();
}

/* torch=3: the level iOS writes is the light, never above Android's current, and a read says what is lit */
static void
for_ios(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	redwood_torch_init();
	WAIT_FOR(logged("THE LIGHT IS IOS'S"), 5000);
	CHECK(logged("THE LIGHT IS IOS'S"), "the device is made: %s", log_text);
	CHECK(!lit_now() && spmi.writes == 0, "no light, and nothing written, until iOS asks");
	ask_for(500);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now() && spmi_get(&flash_model, REG_ITARGET0) == 4, "half is 62.5 mA: code %u", spmi_get(&flash_model, REG_ITARGET0));
	WAIT_FOR(shown_level() == 500, 1000);
	CHECK(shown_level() == 500, "a read gives what is lit: %u", shown_level());
	ask_for(5000);                            /* more than there is: taken for all of it */
	WAIT_FOR(spmi_get(&flash_model, REG_ITARGET0) == 9, 3000);
	CHECK(spmi_get(&flash_model, REG_ITARGET0) == 9 && lit_now(), "at most Android's current: code %u",
	    spmi_get(&flash_model, REG_ITARGET0));
	ask_for(1);                               /* the least there is: one step */
	WAIT_FOR(spmi_get(&flash_model, REG_ITARGET0) == 0, 3000);
	CHECK(spmi_get(&flash_model, REG_ITARGET0) == 0 && lit_now(), "the least is one step, lit");
	ask_for(0);
	WAIT_FOR(!lit_now(), 3000);
	CHECK(!lit_now() && shown_level() == 0, "0 is off");
	CHECK(times_lit == 1, "one light through the three levels (%u)", times_lit);
	never();
	dark_at_the_end();
}

/* a fault while lit: off within a moment, and off for the rest of the run whatever iOS asks */
static void
fault_while_lit(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	redwood_torch_init();
	WAIT_FOR(logged("THE LIGHT IS IOS'S"), 5000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit");
	spmi_set(&flash_model, REG_STATUS2, 0x20);      /* over-temperature, the first threshold */
	WAIT_FOR(!lit_now(), 2000);
	CHECK(!lit_now(), "off when the module says it is too hot");
	CHECK(logged("no more writing to the flash module for the rest of the run"), "said in the log");
	spmi_set(&flash_model, REG_STATUS2, 0);
	ask_for(0);
	ask_for(1000);
	usleep(300 * 1000);
	CHECK(!lit_now() && times_lit == 1, "and stays off for the rest of the run (%u times lit)", times_lit);
	never();
	dark_at_the_end();
}

/* the PMIC's own temperature alarm at a stage: the same */
static void
pmic_hot(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	redwood_torch_init();
	WAIT_FOR(logged("THE LIGHT IS IOS'S"), 5000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	spmi_set(&alarm_model, 0x08, 0x10);             /* stage 1 */
	WAIT_FOR(!lit_now(), 2000);
	CHECK(!lit_now() && logged("the PMIC's temperature stage 1"), "off when the PMIC's alarm is at a stage");
	never();
}

/* trouble before the light: it is never lit */
static void
battery_hot(void)
{
	phone("phone-video");
	battery_c = 46;
	boot_args = "torch=2 torch_secs=1";
	redwood_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("not lit: the battery is at 46 C"), "said in the log: %s", log_text);
	CHECK(times_lit == 0 && spmi.writes == 0, "not lit, nothing written (%u writes)", spmi.writes);
	never();
}

/* the limit: on for torch_max_s at most, then dark until iOS has asked for 0 */
static void
time_limit(void)
{
	phone("phone-video");
	boot_args = "torch=3 torch_max_s=1";
	redwood_torch_init();
	WAIT_FOR(logged("THE LIGHT IS IOS'S"), 5000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit");
	WAIT_FOR(!lit_now(), 3000);
	CHECK(!lit_now() && logged("on for 1 s, the limit"), "cut at the limit");
	usleep(300 * 1000);
	CHECK(!lit_now() && times_lit == 1, "dark while iOS still asks for it");
	ask_for(0);
	usleep(100 * 1000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now() && times_lit == 2, "lit again once iOS has asked for 0 and then for more");
	never();
	dark_at_the_end();
}

/* a module that does not take what is written: the first write that does not read back ends all writing */
static void
write_does_not_stick(void)
{
	phone("phone-video");
	stuck = true;
	boot_args = "torch=2 torch_secs=1";
	redwood_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("a register that did not read back as written"), "said in the log: %s", log_text);
	CHECK(spmi.writes == 1 && times_lit == 0, "one write, no more (%u), and no light", spmi.writes);
	never();
}

/* the module's channel is another system's to write: looked at, never written */
static void
not_ours_to_write(void)
{
	phone("phone-video");
	spmi.owner[20] = 3;
	boot_args = "torch=2";
	redwood_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("the channel is not ours to write through: nothing written"), "said in the log: %s", log_text);
	CHECK(spmi.writes == 0 && spmi.denied == 0, "not even tried (%u refused)", spmi.denied);
	never();
}

/* something else at the module's address: read, said, left alone */
static void
another_part(void)
{
	phone("phone-video");
	flash_model.reg[0x05] = 0x03;
	boot_args = "torch=3";
	redwood_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("NOT WHAT WAS EXPECTED"), "said in the log");
	CHECK(spmi.writes == 0 && fake_dev_write("redwood-torch", "\0\0\0\0", 4) == -1, "nothing written, no device made");
	never();
}

static void
another_arbiter(void)
{
	phone("phone-video");
	spmi.version = 0x70000000;
	boot_args = "torch=2";
	redwood_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("PMIC arbiter version 0x70000000, not 5: nothing done"), "said in the log: %s", log_text);
	CHECK(spmi.transfers == 0, "not a transfer on the bus (%u)", spmi.transfers);
	never();
}

/* a board that does not list the part: the driver says which line is missing and touches nothing */
static void
board_without_the_part(void)
{
	phone("phone-cmd");
	boot_args = "torch=3";
	redwood_torch_init();
	usleep(200 * 1000);
	CHECK(logged("lists no \"torch-pm8350c\""), "said in the log: %s", log_text);
	CHECK(spmi.transfers == 0 && spmi.elsewhere == 0, "nothing read or written");
}

static void
off_by_default(void)
{
	phone("phone-video");
	boot_args = "";
	redwood_torch_init();
	usleep(200 * 1000);
	CHECK(spmi.transfers == 0 && log_text[0] == 0, "without torch=N: not a read, not a word");
	redwood_torch_quiesce();
	CHECK(spmi.transfers == 0, "... at the run's end either");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "light_once", light_once }, { "another_chip", another_chip },
		{ "for_ios", for_ios },
		{ "fault_while_lit", fault_while_lit }, { "pmic_hot", pmic_hot }, { "battery_hot", battery_hot },
		{ "time_limit", time_limit }, { "write_does_not_stick", write_does_not_stick },
		{ "not_ours_to_write", not_ours_to_write }, { "another_part", another_part }, { "another_arbiter", another_arbiter },
		{ "board_without_the_part", board_without_the_part },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			/* the driver's own printing of its log is not the test's result (the log is kept: logged()) */
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
