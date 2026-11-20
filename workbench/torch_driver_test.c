/*
 * The flashlight's driver itself (drivers/board/torch.c), run as a host program
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
static bool led_reads_otherwise;        /* the channels' register takes a write and then reads back with bit 1 set as well */
static bool lit_too_bright;             /* lit with a resolution or a target that is not Android's */
static bool led_read_back_fails;        /* the channel switched on, and the reads after it (its read-back's three tries) dropped, once */

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
		fprintf(stderr, "MODEL: a write to register 0x%02x of the flash module, which is not one of the six\n", reg);
	}
	if (reg == REG_ITARGET0 && (value & 0x7fu) > highest_target) {
		highest_target = value & 0x7f;
	}
	if (!stuck) {
		p->reg[reg] = value;
	}
	if (led_reads_otherwise && reg == REG_EN_LED) {
		p->reg[reg] |= 0x02;
	}
	if (led_read_back_fails && reg == REG_EN_LED && (value & 0x01)) {
		led_read_back_fails = false;
		spmi.drop_reads = 3;            /* (the model's lock is held: this is called from spmi_model_wr()) */
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

uint32_t hakios_test_rd(vm_offset_t address) { return spmi_model_rd(address); }
void hakios_test_wr(vm_offset_t address, uint32_t value) { spmi_model_wr(address, value); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }

/*
 * the battery's temperature, which the driver asks for (bsd/dev/board/battery.c in the kernel), and whether one known
 * earlier in the run is no longer reported (its source gone quiet: the temperature is then -273 there as well)
 */
static int battery_c = 30;
static bool battery_quiet;
int hakios_battery_temperature(void) { return battery_c; }
bool hakios_battery_went_quiet(void) { return battery_quiet; }

/* the one lock of the bus is the buttons' driver's (bsd/dev/board/buttons.c) */
static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;
void hakios_spmi_lock(void) { pthread_mutex_lock(&bus); }
void hakios_spmi_unlock(void) { pthread_mutex_unlock(&bus); }

/* The driver's includes are the kernel's: the fake kernel answers for them. (Its uint64_t is an unsigned
 * long long, which is what it prints it as; and two of its own names are taken on a host: get() by the
 * fake kernel, alarm() by the C library.) */
#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#define get torch_get
#define alarm torch_alarm
#include "board/board.c"
#include "board/torch.c"
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
	flash_model.id = b->torch.flash;        /* the PM8350C's, where the board says that chip is on the bus */
	alarm_model.id = b->torch.temp_alarm;
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
	hakios_torch_quiesce();
	CHECK(!lit_now(), "the light is on at the run's end");
	CHECK(spmi_get(&flash_model, REG_MODULE_EN) == 0 && (spmi_get(&flash_model, REG_EN_LED) & 0x01) == 0 &&
	    (spmi_get(&flash_model, REG_ITARGET0) & 0x7f) == 0, "the module is not as it was found: enable %#x, channels %#x, target %#x",
	    spmi_get(&flash_model, REG_MODULE_EN), spmi_get(&flash_model, REG_EN_LED), spmi_get(&flash_model, REG_ITARGET0));
}

static void
ask_for(uint32_t permille)
{
	CHECK(fake_dev_write("hakios-torch", &permille, sizeof(permille)) == 0, "/dev/hakios-torch takes a level");
}

/* given up on: a level is refused (ENXIO), so that Darwin does not show a light that nothing lights */
static void
refused(uint32_t permille)
{
	CHECK(fake_dev_write("hakios-torch", &permille, sizeof(permille)) == ENXIO, "/dev/hakios-torch refuses a level once given up");
}

static uint32_t
shown_level(void)
{
	uint32_t value = 0xffffffff;

	CHECK(fake_dev_read("hakios-torch", &value, sizeof(value)) == 0, "/dev/hakios-torch is read");
	return value;
}

/* how many words the log's first line that begins with PREFIX has after it; -1: no such line */
static int
words_after(const char *prefix)
{
	const char *at = strstr(log_text, prefix);
	int words = 0;

	if (at == NULL) {
		return -1;
	}
	for (at += strlen(prefix); *at != '\0' && *at != '\n'; at++) {
		words += at[0] == ' ' && at[1] != ' ' && at[1] != '\n' && at[1] != '\0';
	}
	return words;
}

static double
seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* torch=1: the module is found and read, and nothing is written */
static void
look_only(void)
{
	phone("phone-video");
	boot_args = "torch=1";
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("channel 20 is the PM8350C's flash LED module (0x2ee00); written by system 0, which is us"),
	    "the module's channel that is this system's: %s", log_text);
	CHECK(logged("a flash LED module of four channels, as Android's driver expects"), "its type is read");
	CHECK(logged("the PMIC's temperature alarm (subtype 0x9) says 0: stage 0 (all is well)"), "the alarm is read");
	CHECK(logged("torch: 0x00: 00 00 00 00 18 07 00 00") && words_after("torch: 0x00:") == 32 && words_after("torch: 0xe0:") == 32,
	    "the module's 256 registers, 32 a line: %s", log_text);
	CHECK(!logged("torch: 0x100:") && !logged("unreadable"), "and no more: %s", log_text);
	CHECK(spmi.writes == 0 && spmi.denied == 0, "a look writes nothing (%u writes, %u refused)", spmi.writes, spmi.denied);
	never();
}

/* torch=2: on once at Android's current, with Android's writes, then off and as it was found */
static void
light_once(void)
{
	phone("phone-video");
	double on;

	boot_args = "torch=2 torch_secs=1";
	hakios_torch_init();
	WAIT_FOR(lit_now(), 5000);
	on = seconds();
	CHECK(lit_now(), "the light comes on: %s", log_text);
	CHECK(spmi_get(&flash_model, REG_ITARGET0) == 9 && (spmi_get(&flash_model, REG_IRES) & 0x01) == 0,
	    "at Android's 125 mA: target %u, resolution %#x", spmi_get(&flash_model, REG_ITARGET0), spmi_get(&flash_model, REG_IRES));
	CHECK((spmi_get(&flash_model, REG_STROBE0) & 0x74) == 0 && (spmi_get(&flash_model, REG_TIMER0) & 0x80) == 0,
	    "strobed by software, the safety timer off, as Android's probe leaves it");
	CHECK((spmi_get(&flash_model, REG_IRES) & 0x0e) == 0x0e && (spmi_get(&flash_model, REG_TIMER0) & 0x7f) == 0x0f,
	    "the other channels' bits are left as they were");
	WAIT_FOR(!lit_now(), 5000);
	CHECK(seconds() - on > 0.8, "on for the second asked for, not less: %.2f s", seconds() - on);
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
	hakios_torch_init();
	WAIT_FOR(lit_now(), 5000);
	CHECK(logged("channel 20 is the PM8350C's flash LED module (0x3ee00); written by system 2, which is us"),
	    "the channel of this system, which is the arbiter's third: %s", log_text);
	CHECK(lit_now() && spmi_get(&flash_model, REG_ITARGET0) == 9, "lit at Android's current");
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(!lit_now() && times_lit == 1 && spmi.denied == 0, "and dark again; no write was refused (%u)", spmi.denied);
	never();
	dark_at_the_end();
}

/* the same part behind an arbiter of version 7, on the second of its two buses, this system the arbiter's
 * second. The first bus has a flash module at the same address, through channels of the same numbers: it
 * is not read and, above all, not written. */
static void
second_bus(void)
{
	static struct spmi_peripheral namesake;

	phone("phone-buses");
	spmi_version_7(hakios_board()->spmi.observer, 1, 24, 40);
	namesake = flash_model;
	namesake.written = NULL;
	namesake.bus = 0;
	spmi_channel(11, &namesake, 3);
	spmi_channel(20, &namesake, spmi.ee);
	boot_args = "torch=2 torch_secs=1";
	hakios_torch_init();
	WAIT_FOR(lit_now(), 5000);
	CHECK(logged("channel 20 is the PM8350C's flash LED module (0x4ee00); written by system 1, which is us"),
	    "the channel of this system on the second bus: %s", log_text);
	CHECK(lit_now() && spmi_get(&flash_model, REG_ITARGET0) == 9, "lit at Android's current");
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(!lit_now() && times_lit == 1 && spmi.denied == 0, "and dark again; no write was refused (%u)", spmi.denied);
	CHECK(namesake.writes == 0 && flash_model.writes > 0, "every write to this bus's module, none to the other's (%u, %u)",
	    flash_model.writes, namesake.writes);
	CHECK(spmi.past_the_table == 0, "%u reads of the first bus's part of the tables", spmi.past_the_table);
	never();
	dark_at_the_end();
}

/* torch=3: the level Darwin writes is the light, never above Android's current, and a read says what is lit */
static void
for_darwin(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	CHECK(logged("THE LIGHT IS DARWIN'S"), "the device is made: %s", log_text);
	CHECK(!lit_now() && spmi.writes == 0, "no light, and nothing written, until Darwin asks");
	ask_for(500);
	WAIT_FOR(lit_now() && spmi_get(&flash_model, REG_ITARGET0) == 4, 3000);
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
	WAIT_FOR(!lit_now() && shown_level() == 0, 3000);       /* (the light goes out, then the level that is shown follows) */
	CHECK(!lit_now() && shown_level() == 0, "0 is off");
	CHECK(times_lit == 1, "one light through the three levels (%u)", times_lit);
	never();
	dark_at_the_end();
}

/* a transfer the arbiter drops has not been made: asked for again, the light is lit as asked all the same */
static void
dropped_and_asked_again(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	pthread_mutex_lock(&spmi.lock);
	spmi.drop_writes = 1;
	spmi.drop_reads = 1;
	pthread_mutex_unlock(&spmi.lock);
	ask_for(500);
	WAIT_FOR(lit_now() && spmi_get(&flash_model, REG_ITARGET0) == 4, 3000);
	CHECK(lit_now() && spmi_get(&flash_model, REG_ITARGET0) == 4, "lit at half all the same: code %u", spmi_get(&flash_model, REG_ITARGET0));
	CHECK(spmi.drop_writes == 0 && spmi.drop_reads == 0 && !logged("no more writing"), "the dropped ones asked for again: %s", log_text);
	ask_for(0);
	WAIT_FOR(!lit_now(), 3000);
	CHECK(!lit_now(), "and off");
}

/* a register that is not one of the six is not written, whatever asks: refused, said, and no writing after */
static void
only_the_six(void)
{
	unsigned before;

	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	before = spmi.writes;
	CHECK(!put(REG_STATUS1, 0x01), "a write to the module's status register is refused");
	CHECK(spmi.writes == before && forbidden_writes == 0, "and not made: %u writes", spmi.writes - before);
	CHECK(logged("a register that is not to be written"), "and said: %s", log_text);
	CHECK(!put(REG_ITARGET0, 1) && spmi.writes == before, "nor anything after it, even one of the six");
}

/* a fault while lit: off within a moment, and off for the rest of the run whatever Darwin asks */
static void
fault_while_lit(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit");
	spmi_set(&flash_model, REG_STATUS2, 0x20);      /* over-temperature, the first threshold */
	WAIT_FOR(!lit_now(), 2000);
	CHECK(!lit_now(), "off when the module says it is too hot");
	/* (the light goes off, then the line is written: waited for on its own) */
	EVENTUALLY(logged("no more writing to the flash module for the rest of the run"), 2000, "said in the log");
	spmi_set(&flash_model, REG_STATUS2, 0);
	refused(0);
	refused(1000);
	usleep(300 * 1000);
	CHECK(!lit_now() && times_lit == 1, "and stays off for the rest of the run (%u times lit)", times_lit);
	never();
	dark_at_the_end();
}

/*
 * Each of the other things the module can say is wrong, on its own: lit, then that, then dark within a
 * moment and for the rest of the run. (One fault a process: the driver gives up for good.)
 */
static void
lit_then(void (*goes_wrong)(void), const char *said, const char *what)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit");
	goes_wrong();
	/* (the light goes off, then writing is given up on: the refusals below wait for the second) */
	WAIT_FOR(!lit_now() && logged(said) && logged("no more writing to the flash module for the rest of the run"), 2000);
	CHECK(!lit_now(), "off when %s", what);
	CHECK(logged(said), "and why is said (\"%s\"): %s", said, log_text);
	refused(0);
	refused(1000);
	usleep(300 * 1000);
	CHECK(!lit_now() && times_lit == 1, "and stays off for the rest of the run (%u times lit)", times_lit);
	never();
	dark_at_the_end();
}

static void the_module_faults(void) { spmi_set(&flash_model, REG_RT_STS, flash_model.reg[REG_RT_STS] | 0x01); }
static void a_channel_faults(void) { spmi_set(&flash_model, REG_STATUS1, 0x01); }
static void second_threshold(void) { spmi_set(&flash_model, REG_STATUS2, 0x10); }
static void the_supply_droops(void) { spmi_set(&flash_model, REG_STATUS2, 0x01); }
static void the_alarm_goes_silent(void) { pthread_mutex_lock(&spmi.lock); alarm_model.silent = true; pthread_mutex_unlock(&spmi.lock); }

static void module_fault(void) { lit_then(the_module_faults, "the module says fault 0x11", "the module says its LED is open, shorted or without supply"); }
static void channel_fault(void) { lit_then(a_channel_faults, "channels 0x1", "a channel is open or shorted"); }
static void hotter_still(void) { lit_then(second_threshold, "temperature and supply 0x10", "the module is past its second temperature threshold"); }
static void supply_droops(void) { lit_then(the_supply_droops, "temperature and supply 0x1", "the supply droops"); }
/* what cannot be read is not taken for good news */
static void flags_not_readable(void) { lit_then(the_alarm_goes_silent, "a read of the flags that failed", "the PMIC's alarm cannot be read any more"); }

/*
 * The arbiter's table changes under the driver (another system re-programmed it): the channel that was the
 * module's is somebody else's now. Not one byte goes through it.
 */
static void
channel_changes_hands(void)
{
	static struct spmi_peripheral somebody_else = { .id = 0x4c0 };

	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	spmi_channel(20, &somebody_else, spmi.ee);
	ask_for(1000);
	WAIT_FOR(logged("the arbiter's channel is no longer the flash module's"), 3000);
	CHECK(logged("the arbiter's channel is no longer the flash module's"), "said: %s", log_text);
	usleep(200 * 1000);
	CHECK(somebody_else.writes == 0 && !lit_now() && times_lit == 0, "nothing is written through it: %u writes", somebody_else.writes);
}

/* the PMIC's own temperature alarm at a stage: the same */
static void
pmic_hot(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	spmi_set(&alarm_model, 0x08, 0x10);             /* stage 1 */
	WAIT_FOR(!lit_now() && logged("the PMIC's temperature stage 1"), 2000);
	CHECK(!lit_now() && logged("the PMIC's temperature stage 1"), "off when the PMIC's alarm is at a stage");
	never();
}

/* the first generation of the PMIC's alarm keeps its stage in bits 1:0, not 6:4: 0x10 is stage 0, lit */
static void
first_generation_alarm(void)
{
	phone("phone-video");
	alarm_model.reg[0x05] = 0x08;
	alarm_model.reg[0x08] = 0x10;
	boot_args = "torch=2 torch_secs=1";
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("(subtype 0x8) says 0x10: stage 0 (all is well)") && times_lit == 1, "stage 0, lit: %s", log_text);
	never();
	dark_at_the_end();
}

/* ... and 0x01 there is stage 1: never lit */
static void
first_generation_alarm_at_a_stage(void)
{
	phone("phone-video");
	alarm_model.reg[0x05] = 0x08;
	alarm_model.reg[0x08] = 0x01;
	boot_args = "torch=2 torch_secs=1";
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("(subtype 0x8) says 0x1: stage 1") && logged("the PMIC's temperature stage 1") && times_lit == 0, "not lit: %s",
	    log_text);
	never();
}

/* trouble while the light is off is no trouble: Darwin's light is still served once it has passed */
static void
hot_while_dark(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	battery_c = 50;
	usleep(400 * 1000);
	CHECK(!logged("given up"), "not given up while dark: %s", log_text);
	battery_c = 30;
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit when asked, once it is cool: %s", log_text);
	never();
	dark_at_the_end();
}

/* trouble before the light: it is never lit (at 45 C, the hottest the battery may be: already too hot) */
static void
battery_hot(void)
{
	phone("phone-video");
	battery_c = 45;
	boot_args = "torch=2 torch_secs=1";
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("not lit: the battery is at 45 C"), "said in the log: %s", log_text);
	CHECK(times_lit == 0 && spmi.writes == 0, "not lit, nothing written (%u writes)", spmi.writes);
	never();
}

/* lit with the battery's temperature known, and then its source goes quiet: not known to be cool any more, off */
static void
battery_goes_quiet_while_lit(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	ask_for(1000);
	/* (the light is on before its line is said: both waited for) */
	WAIT_FOR(lit_now() && logged("the battery 30 C"), 3000);
	CHECK(lit_now() && logged("the battery 30 C"), "lit with the battery at 30 C: %s", log_text);
	battery_c = -273;                       /* (as battery.c has it once its source is quiet) */
	battery_quiet = true;
	WAIT_FOR(!lit_now() && logged("the light is given up on"), 3000);
	CHECK(!lit_now(), "off once the battery's temperature is no longer reported");
	CHECK(logged("OFF: the battery's temperature, known earlier, is no longer reported (its source went quiet)"), "why is said: %s",
	    log_text);
	refused(1000);
	never();
	dark_at_the_end();
}

/* ... and quiet before the light: never lit */
static void
battery_quiet_before_the_light(void)
{
	phone("phone-video");
	battery_c = -273;
	battery_quiet = true;
	boot_args = "torch=2 torch_secs=1";
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("not lit: the battery's temperature, known earlier, is no longer reported"), "said in the log: %s", log_text);
	CHECK(times_lit == 0 && spmi.writes == 0, "not lit, nothing written (%u writes)", spmi.writes);
	never();
}

/* a phone with no battery's temperature at all (no adsp=1, no battery=1): the light as before, the other two guarding */
static void
no_battery_at_all(void)
{
	phone("phone-video");
	battery_c = -273;
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit: %s", log_text);
	usleep(500 * 1000);                     /* (five looks at what would put it out) */
	CHECK(lit_now() && !logged("given up") && !logged("no longer reported"), "and stays lit: %s", log_text);
	ask_for(0);
	WAIT_FOR(!lit_now(), 3000);
	CHECK(!lit_now() && times_lit == 1, "off when asked (%u times lit)", times_lit);
	never();
	dark_at_the_end();
}

/* the limit: on for torch_max_s at most, then dark until Darwin has asked for 0 */
static void
time_limit(void)
{
	phone("phone-video");
	boot_args = "torch=3 torch_max_s=1";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit");
	WAIT_FOR(!lit_now() && logged("on for 1 s, the limit"), 3000);
	CHECK(!lit_now() && logged("on for 1 s, the limit"), "cut at the limit");
	usleep(300 * 1000);
	CHECK(!lit_now() && times_lit == 1, "dark while Darwin still asks for it");
	ask_for(0);
	usleep(100 * 1000);
	ask_for(1000);
	WAIT_FOR(lit_now() && times_lit == 2, 3000);
	CHECK(lit_now() && times_lit == 2, "lit again once Darwin has asked for 0 and then for more");
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
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("a register that did not read back as written"), "said in the log: %s", log_text);
	CHECK(spmi.writes == 1 && times_lit == 0, "one write, no more (%u), and no light", spmi.writes);
	never();
}

/* the channel switched on, and its register not reading back as written: given up on, and the light switched off all the
 * same, not left on with its safety timer off; Darwin told no more is taken */
static void
lit_and_then_given_up(void)
{
	uint32_t level = 500;

	phone("phone-video");
	led_reads_otherwise = true;
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 5000);
	ask_for(500);
	WAIT_FOR(logged("the light is given up on"), 5000);
	CHECK(logged("a register that did not read back as written"), "said: %s", log_text);
	CHECK(!is_lit(), "the light is off: enable %#x, channels %#x", flash_model.reg[REG_MODULE_EN], flash_model.reg[REG_EN_LED]);
	CHECK(fake_dev_write("hakios-torch", &level, sizeof(level)) == ENXIO, "a level after it: refused");
	never();
}

/*
 * torch=2, the channel switched on and its read-back failing: the light was lit by that write all the same, and is
 * put out, not left on with no time limit for the rest of the run
 */
static void
light_once_read_back_fails(void)
{
	phone("phone-video");
	led_read_back_fails = true;
	boot_args = "torch=2 torch_secs=1";
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("torch: done") && logged("a write that failed"), "the read-back failed, and that is said: %s", log_text);
	CHECK(!led_read_back_fails && times_lit == 1, "lit by the write whose read-back failed (%u)", times_lit);
	CHECK(!lit_now() && spmi_get(&flash_model, REG_MODULE_EN) == 0 && spmi_get(&flash_model, REG_EN_LED) == 0,
	    "and put out: enable %#x, channels %#x", spmi_get(&flash_model, REG_MODULE_EN), spmi_get(&flash_model, REG_EN_LED));
	usleep(300 * 1000);
	CHECK(!lit_now() && times_lit == 1, "and dark after the thread has ended (%u times lit)", times_lit);
	never();
	dark_at_the_end();
}

/*
 * torch=3, lit, and then no read of the module goes through (the flags cannot be read, nor anything else): put out
 * all the same, by writes that wait on no read, and dark for the rest of the run
 */
static void
flags_read_fails_while_lit(void)
{
	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 20000);
	ask_for(1000);
	WAIT_FOR(lit_now(), 3000);
	CHECK(lit_now(), "lit");
	pthread_mutex_lock(&spmi.lock);
	spmi.drop_reads = 1u << 30;             /* every read from here on dropped, each time it is asked for again */
	pthread_mutex_unlock(&spmi.lock);
	WAIT_FOR(!lit_now() && logged("the light is given up on"), 3000);
	CHECK(logged("a read of the flags that failed"), "why is said: %s", log_text);
	CHECK(!lit_now() && spmi_get(&flash_model, REG_MODULE_EN) == 0 && spmi_get(&flash_model, REG_EN_LED) == 0,
	    "put out, both enables written 0: enable %#x, channels %#x", spmi_get(&flash_model, REG_MODULE_EN),
	    spmi_get(&flash_model, REG_EN_LED));
	refused(1000);
	usleep(300 * 1000);
	CHECK(!lit_now() && times_lit == 1, "and stays dark for the rest of the run (%u times lit)", times_lit);
	pthread_mutex_lock(&spmi.lock);
	spmi.drop_reads = 0;                    /* (the reads back, for the run's end to find the module as it was) */
	pthread_mutex_unlock(&spmi.lock);
	never();
	dark_at_the_end();
}

/* a write of less than a level is refused, not taken as nothing; a read gives the level once, then the end */
static void
short_writes_and_reads(void)
{
	uint32_t got[2] = { 7, 7 };
	size_t done = 0;

	phone("phone-video");
	boot_args = "torch=3";
	hakios_torch_init();
	WAIT_FOR(logged("THE LIGHT IS DARWIN'S"), 5000);
	CHECK(fake_dev_write("hakios-torch", got, 2) == EINVAL, "two bytes: refused");
	CHECK(fake_dev_pread("hakios-torch", got, sizeof(got), 0, &done) == 0 && done == 4 && got[0] == 0 && got[1] == 7,
	    "a read: the level, 4 bytes (%zu)", done);
	CHECK(fake_dev_pread("hakios-torch", got, sizeof(got), 4, &done) == 0 && done == 0, "then the end (%zu)", done);
	dark_at_the_end();
	never();
}

/* the module's channel is another system's to write: looked at, never written */
static void
not_ours_to_write(void)
{
	phone("phone-video");
	spmi.owner[20] = 3;
	boot_args = "torch=2";
	hakios_torch_init();
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
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("NOT WHAT WAS EXPECTED"), "said in the log");
	CHECK(spmi.writes == 0 && fake_dev_write("hakios-torch", "\0\0\0\0", 4) == -1, "nothing written, no device made");
	never();
}

static void
another_arbiter(void)
{
	phone("phone-video");
	spmi.version = 0x30000000;
	boot_args = "torch=2";
	hakios_torch_init();
	WAIT_FOR(logged("torch: done"), 5000);
	CHECK(logged("PMIC arbiter version 0x30000000, not 5, 7 or 8: nothing done"), "said in the log: %s", log_text);
	CHECK(spmi.transfers == 0, "not a transfer on the bus (%u)", spmi.transfers);
	never();
}

/* a board that does not list the part: the driver says which line is missing and touches nothing */
static void
board_without_the_part(void)
{
	phone("phone-cmd");
	boot_args = "torch=3";
	hakios_torch_init();
	usleep(200 * 1000);
	CHECK(logged("lists no \"torch-pm8350c\""), "said in the log: %s", log_text);
	CHECK(spmi.transfers == 0 && spmi.elsewhere == 0, "nothing read or written");
}

static void
off_by_default(void)
{
	phone("phone-video");
	boot_args = "";
	hakios_torch_init();
	usleep(200 * 1000);
	CHECK(spmi.transfers == 0 && log_text[0] == 0, "without torch=N: not a read, not a word");
	hakios_torch_quiesce();
	CHECK(spmi.transfers == 0, "... at the run's end either");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "off_by_default", off_by_default }, { "look_only", look_only }, { "light_once", light_once }, { "another_chip", another_chip },
		{ "for_darwin", for_darwin },
		{ "fault_while_lit", fault_while_lit }, { "module_fault", module_fault }, { "channel_fault", channel_fault },
		{ "hotter_still", hotter_still }, { "supply_droops", supply_droops }, { "flags_not_readable", flags_not_readable },
		{ "channel_changes_hands", channel_changes_hands }, { "pmic_hot", pmic_hot }, { "first_generation_alarm", first_generation_alarm },
		{ "first_generation_alarm_at_a_stage", first_generation_alarm_at_a_stage }, { "hot_while_dark", hot_while_dark }, { "battery_hot", battery_hot },
		{ "time_limit", time_limit }, { "write_does_not_stick", write_does_not_stick },
		{ "lit_and_then_given_up", lit_and_then_given_up }, { "short_writes_and_reads", short_writes_and_reads },
		{ "not_ours_to_write", not_ours_to_write }, { "another_part", another_part }, { "another_arbiter", another_arbiter },
		{ "board_without_the_part", board_without_the_part }, { "only_the_six", only_the_six },
		{ "dropped_and_asked_again", dropped_and_asked_again }, { "second_bus", second_bus },
		{ "light_once_read_back_fails", light_once_read_back_fails }, { "flags_read_fails_while_lit", flags_read_fails_while_lit },
		{ "battery_goes_quiet_while_lit", battery_goes_quiet_while_lit },
		{ "battery_quiet_before_the_light", battery_quiet_before_the_light }, { "no_battery_at_all", no_battery_at_all },
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
