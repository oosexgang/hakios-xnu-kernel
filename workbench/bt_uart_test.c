/* Run the actual driver against GENI/H4, GCC, RPMh, TLMM and SPMI models; one scenario per process.
 * These are register-traffic checks, not evidence that a physical controller has answered. */
#include <unistd.h>
#include "fakekernel.h"
#include "board/board.h"
#include "spmi_model.h"
#include "rsc_model.h"
#include "tlmm_model.h"
#include "cmd_db_image.h"

static const struct hakios_board *b;
static struct spmi_peripheral en;
static uint32_t engine[0x1000], clocks[0x60000 / 4];
static unsigned touches, mappings, unexpected, pmic_reads, tx_commands, read_starts, rpmh_count;
static unsigned loopbacks, rx_cancels;
static unsigned rpmh_locks, spmi_locks, lock_at_return, power_steps;
static bool in_return, silent, wrong_protocol, stuck_clock, stuck_tx, bad_reset, short_version, cc_version;
static bool rcg_stuck, rx_error, coalesced, sw_low;
static uint8_t tx[8], rx[512];
static unsigned tx_size, rx_size, rx_pos;
static struct { uint32_t address, value; } votes[128];
static int failures;
extern char log_text[];
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

void hakios_rpmh_lock(void) { rpmh_locks++; lock_at_return += in_return; }
void hakios_rpmh_unlock(void) { CHECK(rpmh_locks > 0, "RPMh lock balanced"); rpmh_locks--; }
void hakios_spmi_lock(void) { spmi_locks++; lock_at_return += in_return; }
void hakios_spmi_unlock(void) { CHECK(spmi_locks > 0, "SPMI lock balanced"); spmi_locks--; }

static void
pmic_looked(struct spmi_peripheral *p __unused, uint8_t reg)
{
	static const uint8_t regs[] = { 5, 0x40, 0x41, 0x42, 0x44, 0x45, 0x46 };
	for (unsigned i = 0; i < sizeof(regs); i++) {
		if (reg == regs[i]) {
			pmic_reads |= 1u << i;
		}
	}
}

static void
pmic_written(struct spmi_peripheral *p, uint8_t reg, uint8_t value)
{
	CHECK(pmic_reads == 0x7f, "all seven GPIO registers inspected before writes");
	CHECK(in_return || spmi_locks == 1, "SPMI write serialized");
	CHECK(reg == 0x44 || reg == 0x40 || reg == 0x46, "only GPIO output/mode/enable written: %#x", reg);
	CHECK(reg != 0x44 || value == 0 || value == 0x80, "valid output source");
	CHECK(power_steps == 7, "six supplies then RF clock before enable (%u)", power_steps);
	p->reg[reg] = value;
}

static void
peer(void)
{
	static const uint8_t reset_cmd[] = { 1, 3, 0x0c, 0 };
	static const uint8_t ver_cmd[] = { 1, 0, 0xfc, 1, 0x19 };
	static const uint8_t reset_evt[] = { 4, 0x0e, 4, 1, 3, 0x0c, 0 };
	static const uint8_t version[] = { 4, 0xff, 14, 0, 2, 0x50, 0x77, 0, 0, 0x34, 0x12, 0x78, 0x56, 0x44, 0x33, 0x22, 0x11 };
	static const uint8_t complete[] = { 4, 0x0e, 16, 1, 0, 0xfc, 0, 0x50, 0x77, 0, 0, 0x34, 0x12, 0x78, 0x56, 0x44, 0x33, 0x22, 0x11 };
	bool loopback = (engine[0x22c / 4] & 1) != 0;
	if (loopback) {
		static const uint8_t probe[] = { 0x55, 0xa5, 0x0f, 0xf0, 0x3c };
		loopbacks++;
		CHECK(tx_size == sizeof(probe) && !memcmp(tx, probe, sizeof(probe)), "exact internal loopback probe");
		CHECK(engine[0x22c / 4] == 3, "TX/RX and RFR/CTS loopback enabled");
	}
	CHECK(loopbacks >= 1 && loopbacks <= 4, "one probe per UART configuration");
	CHECK(read_starts == 2 * loopbacks - loopback, "RX setup and loopback restart before TX (%u)", read_starts);
	CHECK(rx_cancels == 2 * loopbacks - 1 - loopback, "RX cancelled after probe and between configurations (%u)", rx_cancels);
	CHECK(engine[0x25c / 4] == (loopbacks % 2 ? 0u : 2u) && engine[0x2ac / 4] == 0,
	    "CTS/RTS enabled in first/third configuration, CTS ignored in second/fourth");
	CHECK(engine[0x48 / 4] == (loopbacks <= 2 ? 0x41u : 0x21u) && engine[0x4c / 4] == engine[0x48 / 4],
	    "16x sampling first, then 32x");
	CHECK(engine[0x268 / 4] == 8 && engine[0x28c / 4] == 8 && !engine[0x26c / 4] &&
	    !engine[0x280 / 4] && !engine[0x2a4 / 4] && !engine[0x2a8 / 4] &&
	    (loopback || !engine[0x22c / 4]) && engine[0x294 / 4] == 160, "UART 8N1, loopback disabled for HCI, stale count");
	CHECK(!(engine[0x258 / 4] & 1) && !engine[0xe18 / 4] && engine[0x810 / 4] == 13 &&
	    engine[0x814 / 4] == 14 && !engine[0x80c / 4], "FIFO mode/watermarks");
	for (unsigned i = 0; i < 4; i++) {
		CHECK(model.cfg[b->bluetooth.uart_pins[i]] == (uint32_t)b->bluetooth.uart_function << 2, "board UART pin function");
	}
	if (loopback) {
		/* Internal engine loopback works even when the external controller is silent. */
		memcpy(rx, tx, tx_size);
		rx_size = tx_size;
		rx_pos = 0;
		engine[0x610 / 4] = stuck_tx ? 0 : 1;
		return;
	}
	if (++tx_commands == loopbacks) {
		CHECK(tx_size == sizeof(reset_cmd) && !memcmp(tx, reset_cmd, sizeof(reset_cmd)), "exact H4 reset");
		memcpy(rx, reset_evt, sizeof(reset_evt));
		rx_size = sizeof(reset_evt);
		if (bad_reset) {
			rx[6] = 1;
		}
	} else {
		CHECK(tx_size == sizeof(ver_cmd) && !memcmp(tx, ver_cmd, sizeof(ver_cmd)), "exact H4 EDL_PATCH_GETVER");
		rx_size = cc_version ? sizeof(complete) : sizeof(version);
		memcpy(rx, cc_version ? complete : version, rx_size);
		if (short_version) {
			rx_size = 7;
			rx[2] = 4;
		}
		if (coalesced) {
			memmove(rx + sizeof(reset_evt), rx, rx_size);
			memcpy(rx, reset_evt, sizeof(reset_evt));
			rx_size += sizeof(reset_evt);
		}
	}
	rx_pos = 0;
	if (silent) {
		rx_size = 0;
	}
	engine[0x610 / 4] = stuck_tx ? 0 : 1;
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	touches++;
	if (address >= b->bluetooth.serial_engine && address < b->bluetooth.serial_engine + 0x4000) {
		unsigned reg = (unsigned)(address - b->bluetooth.serial_engine);
		if (reg == 0x68) {
			return wrong_protocol ? 0x100 : 0x200;
		}
		if (reg == 0x804) {
			unsigned left = rx_size - rx_pos;
			return left ? (left + 3) / 4 | (1u << 31) | ((left % 4) << 28) : 0;
		}
		if (reg == 0x780) {
			uint32_t word = 0;
			for (unsigned j = 0; j < 4 && rx_pos < rx_size; j++) {
				word |= (uint32_t)rx[rx_pos++] << (8 * j);
			}
			return word;
		}
		if (reg == 0x640 && rx_error) {
			return 1u << 24;
		}
		return engine[reg / 4];
	}
	if (address == b->bluetooth.wrapper + 4) {
		return sw_low ? 0x20040000 : 0x20050000;
	}
	if (address >= b->clocks.gcc && address < b->clocks.gcc + 0x60000) {
		unsigned reg = (unsigned)(address - b->clocks.gcc);
		if (stuck_clock && reg == b->bluetooth.clocks[2]) {
			return 1u << 31;
		}
		return clocks[reg / 4];
	}
	if (address >= model.base && address < model.base + 0x100000) {
		return tlmm_model_rd(address);
	}
	if (address == b->rpmh.rsc) {
		return 0x00020000;      /* RSC_DRV_ID: a version 2 sequencer, as the model's slot set is laid out */
	}
	if (address >= rsc.base && address < rsc.base + 0x2a0) {
		return rsc_model_rd(address);
	}
	return spmi_model_rd(address);
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	touches++;
	if (address >= b->bluetooth.serial_engine && address < b->bluetooth.serial_engine + 0x4000) {
		unsigned reg = (unsigned)(address - b->bluetooth.serial_engine);
		static const unsigned allowed[] = { 0x20, 0x24, 0x28, 0x48, 0x4c, 0x7c, 0x22c, 0x254, 0x258,
		    0x25c, 0x260, 0x264, 0x268, 0x26c, 0x270, 0x280, 0x284, 0x288, 0x28c, 0x294,
		    0x2a4, 0x2a8, 0x2ac, 0x600, 0x604, 0x614, 0x618, 0x630, 0x634, 0x644, 0x648,
		    0x700, 0x80c, 0x810, 0x814, 0xc44, 0xd44, 0xe18, 0xe1c, 0xe30 };
		bool known = false;
		for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
			known |= allowed[i] == reg;
		}
		CHECK(known, "write to defined GENI register %#x", reg);
		if (reg == 0x618) {
			engine[0x610 / 4] &= ~value;
		} else if (reg == 0x648) {
			engine[0x640 / 4] &= ~value;
		} else if (reg == 0x600) {
			CHECK(value == 1u << 27, "UART TX opcode");
			tx_size = 0;
		} else if (reg == 0x630) {
			CHECK(value == 1u << 27, "UART RX opcode");
			read_starts++;
		} else if (reg == 0x634) {
			CHECK(value == 1u << 2 || value == 2, "RX cancel or cleanup abort");
			rx_cancels += value == 1u << 2;
			engine[reg / 4] = value;
		} else if (reg == 0x700) {
			for (unsigned i = 0; i < 4 && tx_size < engine[0x270 / 4]; i++) {
				CHECK(tx_size < sizeof(tx), "TX buffer bounds");
				if (tx_size < sizeof(tx)) {
					tx[tx_size++] = (uint8_t)(value >> (8 * i));
				}
			}
			if (tx_size == engine[0x270 / 4]) {
				peer();
			}
		} else {
			engine[reg / 4] = value;
		}
		return;
	}
	if (address >= b->clocks.gcc && address < b->clocks.gcc + 0x60000) {
		unsigned reg = (unsigned)(address - b->clocks.gcc);
		if (reg == b->bluetooth.vote_register || (reg >= b->bluetooth.clock_source && reg <= b->bluetooth.clock_source + 16)) {
			clocks[reg / 4] = reg == b->bluetooth.clock_source && !rcg_stuck ? value & ~1u : value;
		} else {
			unexpected++;
		}
		return;
	}
	if (address >= model.base && address < model.base + 0x100000) {
		tlmm_model_wr(address, value);
		return;
	}
	if (address >= rsc.base && address < rsc.base + 0x2a0) {
		CHECK(in_return || rpmh_locks == 1, "RPMh serialized");
		if (address == rsc.base + 0x14 && (value & 1u << 24)) {
			for (unsigned i = 0; i < 16; i++) {
				if (!(rsc.enabled & 1u << i)) {
					continue;
				}
				CHECK(rpmh_count < 128, "vote trace fits");
				if (rpmh_count < 128) {
					votes[rpmh_count].address = rsc.address[i];
					votes[rpmh_count++].value = rsc.data[i];
				}
			}
			if (power_steps < 7) {
				unsigned i = power_steps++;
				uint32_t base = 0x40000 + i * 0x100;
				CHECK(rsc.address[0] == base + (i == 6 ? 4 : 0), "supply/RF order %u", i);
				CHECK(rsc.data[0] == (i == 6 ? 1 : b->bluetooth.supply_mv[i]), "supply/RF value %u", i);
				if (i != 6) {
					CHECK(rsc.address[1] == base + 4 && rsc.data[1] == 1, "supply enable");
					CHECK(rsc.enabled == (b->bluetooth.supplies[i][0] == 'L' ? 7u : 3u), "LDO HPM only");
					if (rsc.enabled == 7) {
						CHECK(rsc.address[2] == base + 8 && rsc.data[2] == 7, "HPM mode");
					}
				}
			}
		}
		rsc_model_wr(address, value);
		return;
	}
	spmi_model_wr(address, value);
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys __unused, vm_size_t size __unused) { mappings++; return (vm_offset_t)DB; }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
/* Avoid collisions with the model headers' rsc/model variables. */
#include "board/bt_uart.c"

/* a scenario's resources in the command database: one of them not a regulator's (VRM), or not there at all */
static int odd_resource = -1;
static uint32_t odd_address;
static const char *odd_name;

static void
setup(void)
{
	board = "phone-video";
	b = hakios_board();
	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	en.id = b->bluetooth.enable_ppid;
	en.reg[5] = 0x10;
	en.looked_at = pmic_looked;
	en.written = pmic_written;
	spmi_channel(3, &en, 3);          /* another system's alias first */
	spmi_channel(7, &en, spmi.ee);
	rsc.base = b->rpmh.rsc + b->rpmh.tcs_offset;
	tlmm_model_at(b->pins.tlmm);
	model.input[b->bluetooth.sw_ctrl] = true;
	static const unsigned dirty[] = { 0x22c, 0x254, 0x25c, 0x26c, 0x280, 0x2a4, 0x2a8, 0x2ac, 0x7c, 0xe18, 0x258 };
	for (unsigned i = 0; i < sizeof(dirty) / sizeof(dirty[0]); i++) {
		engine[dirty[i] / 4] = 1;
	}
	engine[0xe24 / 4] = (32u << 24) | (16u << 16);
	engine[0xe28 / 4] = (32u << 24) | (16u << 16);
	clocks[b->bluetooth.vote_register / 4] = (1u << b->bluetooth.votes[0]) | (1u << 30);
	empty();
	struct resource list[7];
	for (unsigned i = 0; i < 7; i++) {
		list[i] = (struct resource){ i == 6 ? b->bluetooth.rf_clock : b->bluetooth.supplies[i], 0x40000 + 0x100 * i, NULL, 0 };
	}
	if (odd_resource >= 0) {
		list[odd_resource].address = odd_address != 0 ? odd_address : list[odd_resource].address;
		list[odd_resource].name = odd_name != NULL ? odd_name : list[odd_resource].name;
	}
	slave(0, 4, list, 7);
}

static void
run(void)
{
	hakios_bt_uart_init();
	for (unsigned i = 0; i < 3000 && !logged("bt-uart: done"); i++) {
		usleep(1000);
	}
	CHECK(logged("bt-uart: done"), "worker finished: %s", log_text);
}

static void
quiesce(void)
{
	in_return = true;
	hakios_bt_uart_quiesce();
	in_return = false;
	CHECK(en.reg[0x44] == 0, "enable GPIO low");
	CHECK(clocks[b->bluetooth.vote_register / 4] == ((1u << b->bluetooth.votes[0]) | (1u << 30)), "preserved preexisting GCC votes");
	CHECK(!lock_at_return, "quiesce takes no locks");
	for (unsigned i = 0; i < 4; i++) {
		CHECK(!model.cfg[b->bluetooth.uart_pins[i]], "UART pin restored");
	}
	CHECK(!model.cfg[b->bluetooth.sw_ctrl], "sw-ctrl pin restored");
	if (!rcg_stuck) {
		for (unsigned i = 0; i < 5; i++) {
			CHECK(!clocks[b->bluetooth.clock_source / 4 + i], "RCG restored");
		}
	}
	unsigned count = touches;
	hakios_bt_uart_quiesce();
	CHECK(count == touches, "quiesce is idempotent");
}

static void
packing_and_clock(void)
{
	uint32_t cfg[2] = { 0, 0 };
	/* Recompute Linux geni_se_config_packing(8, 4, false, true, true):
	 * start index [9:5], direction [4], length-1 [3:1], final vector stop [0]. */
	for (unsigned i = 0; i < 4; i++) {
		uint32_t vector = ((7 + i * 8) << 5) | (7 << 1) | (i == 3);
		cfg[i / 2] |= vector << ((i % 2) * 10);
	}
	CHECK(engine[0x260 / 4] == cfg[0] && engine[0x264 / 4] == cfg[1] &&
	    engine[0x284 / 4] == cfg[0] && engine[0x288 / 4] == cfg[1], "LSB-first 8-bit packing");
	unsigned cmd = b->bluetooth.clock_source / 4;
	CHECK(clocks[cmd + 1] == (1 | 6 << 8 | 2 << 12), "RCG parent/HID/MND");
	CHECK(clocks[cmd + 2] == 384 && clocks[cmd + 3] == ((~(15625u - 384)) & 0xffff) &&
	    clocks[cmd + 4] == ((~15625u) & 0xffff), "16-bit M/N/D");
	CHECK(engine[0x48 / 4] == 0x41u && engine[0x4c / 4] == engine[0x48 / 4], "first configuration uses 16x sampling regardless of wrapper version");
}

int
main(int argc, char **argv)
{
	static const char *const scenarios[] = { "off", "zero", "no_feature", "invalid_mode", "look", "wrong_protocol", "unsupported_arbiter", "full",
	    "silent", "quiesce", "clock_timeout", "rcg_timeout", "tx_timeout", "bad_reset", "short_version", "cc_version", "rx_error", "coalesced", "old_wrapper",
	    "not_vrm", "missing_resource", "enable_not_ours", "enable_not_gpio", "mv_gpio" };
	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			puts(scenarios[i]);
		}
		return 0;
	}
	const char *scenario = argv[1];
	if (!freopen("/dev/null", "w", stdout)) { return 2; }
	if (!strcmp(scenario, "no_feature")) {
		board = "phone-cmd";
		boot_args = "bt=1";
		hakios_bt_uart_init();
		hakios_bt_uart_quiesce();
		CHECK(!touches && !mappings && logged("lists no \"bluetooth-wcn-uart\""), "feature gate precedes all hardware");
		fprintf(stderr, "%-20s %s\n", scenario, failures ? "FAILED" : "ok");
		return failures ? 1 : 0;
	}
	/* what is refused before anything is written, and what it is said as */
	static const struct { const char *scenario, *said; } refusals[] = {
		{ "not_vrm", "resource is not VRM" }, { "missing_resource", "resource is not VRM" },
		{ "enable_not_ours", "no owned enable GPIO channel" }, { "enable_not_gpio", "enable GPIO is not LV/MV" },
	};
	const char *refused = NULL;

	for (unsigned i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
		refused = !strcmp(scenario, refusals[i].scenario) ? refusals[i].said : refused;
	}
	if (!strcmp(scenario, "not_vrm")) {
		odd_resource = 2;
		odd_address = 0x30200;          /* (an ARC resource's, slave kind 3) */
	} else if (!strcmp(scenario, "missing_resource")) {
		odd_resource = 6;
		odd_name = "nothing.lvl";       /* (the RF clock's name nowhere in the database) */
	}
	setup();
	boot_args = "bt=1";
	if (!strcmp(scenario, "enable_not_ours")) {
		spmi.map[7] = 0;                /* (only another system's alias of the enable GPIO is left) */
	} else if (!strcmp(scenario, "enable_not_gpio")) {
		en.reg[5] = 0x09;               /* (a subtype that is no LV/MV GPIO's) */
	} else if (!strcmp(scenario, "mv_gpio")) {
		en.reg[5] = 0x13;               /* (the last of the LV/MV subtypes) */
	}
	if (!strcmp(scenario, "off") || !strcmp(scenario, "zero")) {
		boot_args = !strcmp(scenario, "off") ? "" : "bt=0";
		hakios_bt_uart_init();
		hakios_bt_uart_quiesce();
		CHECK(!touches && !mappings && !log_text[0], "off touches/maps/logs nothing");
	} else if (!strcmp(scenario, "invalid_mode")) {
		boot_args = "bt=2";
		hakios_bt_uart_init();
		CHECK(!touches && !mappings, "unsupported mode does nothing");
	} else {
		wrong_protocol = !strcmp(scenario, "wrong_protocol");
		silent = !strcmp(scenario, "silent");
		stuck_clock = !strcmp(scenario, "clock_timeout");
		rcg_stuck = !strcmp(scenario, "rcg_timeout");
		stuck_tx = !strcmp(scenario, "tx_timeout");
		bad_reset = !strcmp(scenario, "bad_reset");
		short_version = !strcmp(scenario, "short_version");
		cc_version = !strcmp(scenario, "cc_version");
		rx_error = !strcmp(scenario, "rx_error");
		coalesced = !strcmp(scenario, "coalesced");
		sw_low = !strcmp(scenario, "old_wrapper");
		model.input[b->bluetooth.sw_ctrl] = !sw_low;
		if (!strcmp(scenario, "unsupported_arbiter")) { spmi.version = 0x80000000; }
		if (!strcmp(scenario, "look")) { boot_args = "bt=-1"; }
		run();
		if (wrong_protocol || spmi.version == 0x80000000 || !strcmp(scenario, "look") || refused != NULL) {
			CHECK(!en.writes && !rsc.writes && !model.writes && !tx_commands, "inspection/refusal performs no hardware changes");
			if (wrong_protocol) { CHECK(logged("SE protocol is not UART"), "protocol refusal logged"); }
			if (refused != NULL) { CHECK(logged(refused), "refusal logged: %s", log_text); }
		} else if (silent || stuck_clock || rcg_stuck || stuck_tx || bad_reset || short_version || rx_error) {
			CHECK(logged("bring-up stopped; powering down") && !logged("identification complete"), "failure is explicit: %s", log_text);
			CHECK(!en.reg[0x44], "failure powers down");
			if (silent) {
				CHECK(loopbacks == 4 && tx_commands == 4 && logged("reset response timed out"), "silent controller tries all four configurations without getver");
				CHECK(read_starts == 8 && rx_cancels == 7, "four RX setups, four loopback restarts, seven cancels");
				CHECK(logged("loopback: 5 of 5 bytes back, the same"), "internal loopback succeeds with silent controller");
			}
		} else {
			CHECK(logged("product id 0x7750, patch version 0x1234, ROM version 0x5678, SoC id 0x11223344"), "decoded version: %s", log_text);
			CHECK(logged("identification complete") && tx_commands == 2, "both commands completed");
			CHECK(loopbacks == 1 && read_starts == 2 && rx_cancels == 1, "first configuration succeeds after loopback RX restart");
			CHECK(logged("loopback: 5 of 5 bytes back, the same"), "all probe bytes echoed intact");
			CHECK(en.reg[0x44] == 0x80, "enable high until quiesce");
			packing_and_clock();
			unsigned count = rpmh_count;
			quiesce();
			CHECK(rpmh_count == count + 7, "RF and six rail enable votes dropped");
			for (unsigned i = 0; i < 7 && count + i < rpmh_count; i++) {
				CHECK(votes[count + i].address == 0x40004 + (6 - i) * 0x100 && votes[count + i].value == 0, "reverse-order vote release");
			}
		}
		quiesce();
	}
	CHECK(!unexpected && !spmi.elsewhere && !model.elsewhere && !rsc.elsewhere, "all traffic inside declared register models");
	CHECK(!rsc.changed_while_sent && !spmi.denied && !rpmh_locks && !spmi_locks && canary_whole(), "shared transports and database intact");
	fprintf(stderr, "%-20s %s\n", scenario, failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
