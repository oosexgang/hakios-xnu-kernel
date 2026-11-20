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
/* "second_bus": the enable GPIO on the second bus of a version 7 arbiter, and a GPIO at its address on the first */
static bool on_second_bus;
static struct spmi_peripheral namesake;
static unsigned namesake_reads;
static uint32_t engine[0x1000], clocks[0x60000 / 4];
static unsigned touches, mappings, unexpected, pmic_reads, tx_commands, read_starts, rpmh_count;
static unsigned loopbacks, rx_cancels;
static unsigned rpmh_locks, spmi_locks, lock_at_return, power_steps;
static bool in_return, silent, wrong_protocol, stuck_clock, stuck_tx, bad_reset, short_version, cc_version, garbled;
/* the engine's clocks off until the driver votes for them, and its registers read meanwhile (none is to be: such a read
 * can hang the bus); the engine's command cancelled or aborted, and started */
static bool clocks_off_until_voted;
static unsigned se_reads_off, m_cancels, m_aborts, tx_starts;
static bool rcg_stuck, rx_error, coalesced, sw_low, not_an_event, darwins_turn;
static bool shortest_version, short_complete, other_first, last_word_four;
static vm_offset_t unmapped;    /* a block ml_io_map() gives nothing for */

/*
 * The controller's firmware, as the RAM disks have it (hakios_image_file below) and as the model takes it: the files
 * in the order they are to come, each a TLV file of its type and download mode; what came of each; the segment the
 * model refuses (-1: none).
 */
static struct { const char *name; uint8_t type, mode; uint32_t size; uint8_t file[1200], came[1200]; uint32_t got; bool there; } fw[2] = {
	{ "msbtfw11.tlv", 1, 3, 1000, { 0 }, { 0 }, 0, true }, { "msnv11.bin", 2, 0, 500, { 0 }, { 0 }, 0, true },
};

/* each file: its TLV header word, a patch's download mode where its header has it, and bytes of its own */
static void
firmware_files(void)
{
	for (unsigned i = 0; i < 2; i++) {
		uint32_t length = fw[i].size - 4;

		for (uint32_t k = 0; k < fw[i].size; k++) {
			fw[i].file[k] = (uint8_t)(k * 7 + i * 3 + 1);
		}
		fw[i].file[0] = fw[i].type;
		fw[i].file[1] = (uint8_t)length;
		fw[i].file[2] = (uint8_t)(length >> 8);
		fw[i].file[3] = 0;
		if (fw[i].type == 1) {
			fw[i].file[4 + 10] = fw[i].mode;
		}
	}
}
static unsigned fw_now, segments_seen, answers_given;
static int refuse_segment = -1;
static bool firmware_done, reset_unanswered;

static uint8_t tx[1100], rx[2048];
static unsigned tx_size, rx_size, rx_pos, fifo_words, done_after;
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

/* the first bus's GPIO at the enable GPIO's address: nothing of it is the driver's to ask or to change */
static void
namesake_looked(struct spmi_peripheral *p __unused, uint8_t reg __unused)
{
	namesake_reads++;
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
	if (tx_size >= 6 && tx[0] == 1 && tx[1] == 0x00 && tx[2] == 0xfc && tx[4] == 0x1e) {
		/* a segment of the firmware: kept as the file it is part of, answered when the file's mode or its end says */
		unsigned length = tx[5];

		CHECK(fw_now < 2 && tx[3] == 2 + length && tx_size == 6u + length && length <= 243, "a segment as the controller takes it");
		if (fw_now < 2) {
			bool last;

			CHECK(fw[fw_now].got + length <= fw[fw_now].size, "no more than the file");
			memcpy(fw[fw_now].came + fw[fw_now].got, tx + 6, length);
			fw[fw_now].got += length;
			last = fw[fw_now].got >= fw[fw_now].size;
			rx_size = 0;
			if ((int)segments_seen == refuse_segment) {
				static const uint8_t no[] = { 4, 0xff, 3, 0x00, 0x1e, 0x05 };

				memcpy(rx, no, sizeof(no));
				rx_size = sizeof(no);
			} else if (last || fw[fw_now].mode == 0 || fw[fw_now].mode == 2) {
				static const uint8_t yes[] = { 4, 0xff, 3, 0x00, 0x1e, 0x00 };

				memcpy(rx, yes, sizeof(yes));
				rx_size = sizeof(yes);
				answers_given++;
			}
			rx_pos = 0;
			segments_seen++;
			fw_now += last;
			firmware_done = fw_now == 2 || (fw_now == 1 && !fw[1].there);
		}
		engine[0x610 / 4] = 1;
		return;
	}
	if (firmware_done && tx_size == sizeof(reset_cmd) && memcmp(tx, reset_cmd, sizeof(reset_cmd)) == 0) {
		memcpy(rx, reset_evt, sizeof(reset_evt));      /* (the reset after the firmware) */
		rx_size = reset_unanswered ? 0 : sizeof(reset_evt);
		rx_pos = 0;
		firmware_done = false;
		engine[0x610 / 4] = 1;
		return;
	}
	if (darwins_turn) {
		/* (Darwin's packets: kept in tx for the scenario to look at; done once each byte has had a look's time to leave --
		 * or never, stuck_tx) */
		engine[0x610 / 4] = stuck_tx ? 0 : 1;
		done_after = tx_size;
		return;
	}
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
	CHECK(!(engine[0x258 / 4] & 1) && !engine[0xe18 / 4] && engine[0x810 / 4] == ((engine[0xe28 / 4] >> 16) & 0x3f) - 3 &&
	    engine[0x814 / 4] == ((engine[0xe28 / 4] >> 16) & 0x3f) - 2 && !engine[0x80c / 4], "FIFO mode/watermarks");
	for (unsigned i = 0; i < 4; i++) {
		CHECK(model.cfg[b->bluetooth.uart_pins[i]] == (uint32_t)b->bluetooth.uart_function << 2, "board UART pin function");
	}
	if (loopback) {
		/* Internal engine loopback works even when the external controller is silent. */
		memcpy(rx, tx, tx_size);
		rx[2] ^= garbled ? 0xff : 0;    /* (loopback_garbled: a byte comes back changed) */
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
		if (not_an_event) {
			rx[0] = 2;              /* (an ACL data packet's type where the reset's event was to come) */
		}
	} else {
		CHECK(tx_size == sizeof(ver_cmd) && !memcmp(tx, ver_cmd, sizeof(ver_cmd)), "exact H4 EDL_PATCH_GETVER");
		rx_size = cc_version ? sizeof(complete) : sizeof(version);
		memcpy(rx, cc_version ? complete : version, rx_size);
		if (short_version) {
			rx_size = 7;
			rx[2] = 4;
		}
		if (shortest_version) {
			rx_size = 5;            /* (a vendor event with its response type and nothing after) */
			rx[2] = 2;
		}
		if (short_complete) {
			memcpy(rx, complete, 7);        /* (a Command Complete of 0xfc00 with its status and nothing after) */
			rx[2] = 4;
			rx_size = 7;
		}
		if (other_first) {
			/* (another command's Command Complete before it, as long as one of 0xfc00 cut short) */
			static const uint8_t other[] = { 4, 0x0e, 4, 1, 0x05, 0x10, 0 };

			memmove(rx + sizeof(other), rx, rx_size);
			memcpy(rx, other, sizeof(other));
			rx_size += sizeof(other);
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

/* the driver's votes for the engine's clocks are all in the GCC's vote register */
static bool
voted(void)
{
	uint32_t mask = 0;

	for (unsigned i = 0; i < 5; i++) {
		mask |= 1u << b->bluetooth.votes[i];
	}
	return (clocks[b->bluetooth.vote_register / 4] & mask) == mask;
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	touches++;
	if (clocks_off_until_voted && !voted() && ((address >= b->bluetooth.serial_engine &&
	    address < b->bluetooth.serial_engine + 0x4000) || address == b->bluetooth.wrapper + 4)) {
		se_reads_off++;
	}
	if (address >= b->bluetooth.serial_engine && address < b->bluetooth.serial_engine + 0x4000) {
		unsigned reg = (unsigned)(address - b->bluetooth.serial_engine);
		if (reg == 0x68) {
			return wrong_protocol ? 0x100 : 0x200;
		}
		if (reg == 0x804) {
			unsigned left = rx_size - rx_pos;
			/* (the last word's valid bytes: 1 to 3, and for a whole word 0 -- or 4, as an engine may say it) */
			unsigned valid = left % 4 != 0 ? left % 4 : last_word_four ? 4 : 0;

			return left ? (left + 3) / 4 | (1u << 31) | (valid << 28) : 0;
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
		if (reg == 0x800) {
			/* the TX FIFO: the words written and not yet gone, a byte going for each look (a word for four) */
			static unsigned looks;
			unsigned words = fifo_words;

			fifo_words -= fifo_words > 0 && ++looks % 4 == 0;
			return words;
		}
		if (reg == 0x610 && done_after > 0) {
			done_after--;           /* (at 115200 baud a byte takes 87 us: about one of the driver's looks) */
			return 0;
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
		if (clocks_off_until_voted && !voted() &&
		    (reg == b->bluetooth.clocks[0] || reg == b->bluetooth.clocks[1] || reg == b->bluetooth.clocks[2])) {
			return 1u << 31;        /* (the halt register: off) */
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
			tx_starts++;
		} else if (reg == 0x604) {
			CHECK(value == 1u << 2 || value == 1u << 1, "TX cancel or abort");
			m_cancels += value == 1u << 2;
			m_aborts += value == 1u << 1;
			engine[0x610 / 4] |= value == 1u << 2 ? 1u << 4 : 1u << 5;     /* (cancelled, or aborted) */
			fifo_words = 0;
		} else if (reg == 0x630) {
			CHECK(value == 1u << 27, "UART RX opcode");
			read_starts++;
		} else if (reg == 0x634) {
			CHECK(value == 1u << 2 || value == 2, "RX cancel or cleanup abort");
			rx_cancels += value == 1u << 2;
			engine[reg / 4] = value;
		} else if (reg == 0x700) {
			CHECK(fifo_words < 16, "a word written to a full TX FIFO");
			fifo_words++;
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

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { mappings++; return phys == unmapped ? 0 : phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys __unused, vm_size_t size __unused) { mappings++; return (vm_offset_t)DB; }

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
/* Avoid collisions with the model headers' rsc/model variables. */
#include "board/bt_uart.c"

/* the RAM disks' files (board/image.c in the kernel): the model's firmware, in the directory the board names */
const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	(void)disk_out;
	for (unsigned i = 0; i < 2; i++) {
		if (fw[i].there && strcmp(dir, b->bluetooth.firmware) == 0 && strcmp(name, fw[i].name) == 0) {
			*size = fw[i].size;
			return fw[i].file;
		}
	}
	return NULL;
}

/* the side Darwin talks to (board/bt_host.c): what it was handed, and an error to give back when asked to */
static const struct board_bt_controller *handed;
static int start_error;

int
board_bt_start(const struct board_bt_controller *controller)
{
	handed = controller;
	return start_error;
}

/* a scenario's resources in the command database: one of them not a regulator's (VRM), or not there at all */
static int odd_resource = -1;
static uint32_t odd_address;
static const char *odd_name;

static void
setup(void)
{
	board = "phone-video";
	b = hakios_board();
	if (on_second_bus) {
		/* the same phone on a chip with the arbiter of workbench/devices/phone-buses: version 7, the second
		 * bus, this system the arbiter's second */
		static struct hakios_board copy;

		copy = *b;
		for (unsigned i = 0; i < hakios_board_count; i++) {
			if (!strcmp(hakios_boards[i].name, "phone-buses")) {
				copy.spmi = hakios_boards[i].spmi;
			}
		}
		CHECK(copy.spmi.bus == 1 && copy.spmi.channels, "the arbiter of the phone with two buses");
		found = b = &copy;
	}
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
	if (on_second_bus) {
		/* 24 channels of the first bus before the second's 40: the arbiter's channels 3 and 7 are the first
		 * bus's, of a GPIO at the same address, the second of them this system's to write as well */
		spmi_version_7(b->spmi.observer, 1, 24, 40);
		namesake = en;
		namesake.bus = 0;
		namesake.looked_at = namesake_looked;
		namesake.written = NULL;
		spmi_channel(3, &namesake, 3);
		spmi_channel(7, &namesake, spmi.ee);
	}
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

/* the firmware: each file whole and in order, answered as its mode says, then the reset; or none at bt_fw=0 */
static void
firmware_checks(const char *scenario)
{
	if (!strcmp(scenario, "firmware_rom")) {
		CHECK(segments_seen == 0 && logged("no firmware downloaded (bt_fw=0)") && handed != NULL, "the ROM, as asked: %s", log_text);
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		CHECK(fw[i].got == fw[i].size && memcmp(fw[i].came, fw[i].file, fw[i].size) == 0, "%s whole, in order (%u of %u bytes)",
		    fw[i].name, fw[i].got, fw[i].size);
	}
	CHECK(segments_seen == 5 + 3, "five segments of the patch, three of the configuration (%u)", segments_seen);
	/* the TLV patch says its controller answers only its last segment (mode 3); an ELF one is answered for each */
	CHECK(answers_given == (fw[0].mode == 3 ? 1u : 5u) + 3, "%u answers", answers_given);
	CHECK(logged(fw[0].mode == 3 ? "a TLV patch, 1000 bytes in 5 segments (download mode 3" : "an ELF patch, 1000 bytes in 5 segments"),
	    "what was sent, said: %s", log_text);
	CHECK(logged("/fw-bt/msnv11.bin taken") && logged("identification complete; the firmware taken"), "then the reset: %s", log_text);
	{
		const char *reset = strstr(log_text, "sending HCI_Reset after the firmware");

		CHECK(reset != NULL && strstr(reset, "H4 event +0: 04 0e 04 01 03 0c 00") != NULL, "its answer read after it: %s", log_text);
	}
	CHECK(handed != NULL && tx_commands == 2, "then handed to Darwin");
}

/* what the controller is to say next: these bytes in the RX FIFO */
static void
controller_says(const uint8_t *bytes, unsigned length)
{
	memcpy(rx, bytes, length);
	rx_size = length;
	rx_pos = 0;
}

/* bt=2: handed to Darwin's side once it has answered, and its packets through the UART either way */
static void
darwins(void)
{
	static const uint8_t complete[] = { 4, 0x0e, 4, 1, 0x03, 0x0c, 0 };
	uint8_t packet[1100], got[1100];
	int n;

	CHECK(handed != NULL && handed->send != NULL && handed->receive != NULL && logged("the controller is Darwin's from here on"),
	    "handed to Darwin: %s", log_text);
	darwins_turn = true;
	/* a packet longer than the FIFO (16 words): all of it, in order, once there is room */
	packet[0] = 2;
	packet[1] = 0x01;
	packet[2] = 0x20;
	packet[3] = (uint8_t)1016;
	packet[4] = 1016 >> 8;
	for (unsigned i = 5; i < 1021; i++) {
		packet[i] = (uint8_t)(i * 7);
	}
	CHECK(handed->send(packet, 1021) == 0 && tx_size == 1021 && memcmp(tx, packet, 1021) == 0, "a long ACL packet sent whole: %u bytes",
	    tx_size);
	CHECK(handed->send(complete + 0, 4) == 0 && tx_size == 4, "a short one after it");
	/* the controller's packets, one whole one a call, and the FIFO's words split between them */
	memcpy(packet, complete, sizeof(complete));
	packet[7] = 2;
	packet[8] = 0x01;
	packet[9] = 0x20;
	packet[10] = 26;
	packet[11] = 0;
	for (unsigned i = 12; i < 38; i++) {
		packet[i] = (uint8_t)i;
	}
	controller_says(packet, 38);
	CHECK((n = handed->receive(got, sizeof(got), 5)) == 7 && memcmp(got, complete, 7) == 0, "the event: %d bytes", n);
	CHECK((n = handed->receive(got, sizeof(got), 5)) == 31 && memcmp(got, packet + 7, 31) == 0, "then the ACL packet: %d bytes", n);
	CHECK((n = handed->receive(got, sizeof(got), 5)) == 0, "then nothing: %d", n);
	/* out of step: a byte no packet begins with; a packet cut short; one longer than the room for it */
	/* an event without parameters, an ISO packet: each its own length, from the header each has */
	controller_says((const uint8_t[]){ 4, 0x10, 0, 5, 0x01, 0x00, 4, 0, 9, 8, 7, 6, 4, 0x0e, 4, 1, 0x03, 0x0c, 0 }, 19);
	CHECK((n = handed->receive(got, 4, 5)) == 0, "no room for a header: nothing read (%d)", n);
	CHECK((n = handed->receive(got, 5, 5)) == 3 && got[1] == 0x10, "room for a header to the byte: an event of three bytes: %d", n);
	CHECK((n = handed->receive(got, 9, 5)) == 9 && got[0] == 5 && got[8] == 6, "an ISO packet, room for it to the byte: %d", n);
	CHECK((n = handed->receive(got, sizeof(got), 5)) == 7 && memcmp(got, complete, 7) == 0, "and the event after them: %d", n);
	controller_says((const uint8_t[]){ 0x07, 1, 2, 3, 4, 5, 6, 7, 8 }, 9);
	CHECK(handed->receive(got, sizeof(got), 5) == -1 && logged("where a packet was to begin: out of step") &&
	    logged("8 more bytes thrown away to be in step again"), "a stray byte, and what came with it: %s", log_text);
	controller_says(complete, sizeof(complete));
	CHECK((n = handed->receive(got, sizeof(got), 5)) == 7 && memcmp(got, complete, 7) == 0, "in step again after it: %d bytes", n);
	controller_says((const uint8_t[]){ 4, 0x0e, 10, 1 }, 4);
	CHECK(handed->receive(got, sizeof(got), 5) == -1 && logged("cut short after 4 bytes"), "a packet cut short: %s", log_text);
	controller_says((const uint8_t[]){ 4, 0x3e, 200, 1 }, 4);
	CHECK(handed->receive(got, 100, 5) == -1 && logged("a packet of 203 bytes from the controller, room for 100"), "too long: %s",
	    log_text);
	/* a packet that does not go out whole (the engine never says done): the engine's command cancelled, and the
	 * controller, out of step with whatever would follow, sent nothing more -- however the engine is after it */
	{
		unsigned starts;

		stuck_tx = true;
		starts = tx_starts;
		CHECK(handed->send(complete + 0, 4) == EIO && m_cancels >= 1 && tx_starts == starts + 1, "not sent: cancelled (%u)",
		    m_cancels);
		CHECK(logged("went out in part, or not at all: the controller is out of step"), "said: %s", log_text);
		stuck_tx = false;
		CHECK(handed->send(complete + 0, 4) == EIO && tx_starts == starts + 1, "nothing more sent (%u starts)", tx_starts - starts);
	}
	rx_size = rx_pos = 0;
	darwins_turn = false;
}

int
main(int argc, char **argv)
{
	static const char *const scenarios[] = { "off", "zero", "no_feature", "invalid_mode", "look", "wrong_protocol", "unsupported_arbiter", "full",
	    "silent", "quiesce", "clock_timeout", "rcg_timeout", "tx_timeout", "bad_reset", "short_version", "cc_version", "rx_error", "coalesced", "old_wrapper",
	    "loopback_garbled", "not_vrm", "missing_resource", "enable_not_ours", "enable_not_gpio", "mv_gpio", "tx_fifo_busy",
	    "not_an_event", "darwins", "not_darwins", "firmware", "firmware_elf", "firmware_refused", "firmware_missing", "firmware_rom",
	    "firmware_junk", "firmware_no_reset",
	    "fifo_narrow", "fifo_shallow", "fifo_four", "rx_shallow", "rx_four", "shortest_version", "short_complete", "other_first",
	    "last_word_four", "no_mapping", "clocks_off", "look_clocks_off", "second_bus" };
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
		{ "tx_fifo_busy", "SE busy, TX FIFO not empty" },
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
	on_second_bus = !strcmp(scenario, "second_bus");
	setup();
	boot_args = "bt=1";
	if (!strcmp(scenario, "enable_not_ours")) {
		spmi.map[7] = 0;                /* (only another system's alias of the enable GPIO is left) */
	} else if (!strcmp(scenario, "enable_not_gpio")) {
		en.reg[5] = 0x09;               /* (a subtype that is no LV/MV GPIO's) */
	} else if (!strcmp(scenario, "tx_fifo_busy")) {
		engine[0x64 / 4] = 1;           /* (something still in the TX FIFO, the engine otherwise idle) */
	} else if (!strcmp(scenario, "mv_gpio")) {
		en.reg[5] = 0x13;               /* (the last of the LV/MV subtypes) */
	}
	if (!strcmp(scenario, "off") || !strcmp(scenario, "zero")) {
		boot_args = !strcmp(scenario, "off") ? "" : "bt=0";
		hakios_bt_uart_init();
		hakios_bt_uart_quiesce();
		CHECK(!touches && !mappings && !log_text[0], "off touches/maps/logs nothing");
	} else if (!strcmp(scenario, "invalid_mode")) {
		boot_args = "bt=3";
		hakios_bt_uart_init();
		for (unsigned i = 0; i < 300 && !logged("bt-uart: done"); i++) {
			usleep(1000);           /* (time for a thread started after all to have done something) */
		}
		CHECK(!touches && !mappings && !log_text[0], "unsupported mode does nothing: %s", log_text);
	} else {
		wrong_protocol = !strcmp(scenario, "wrong_protocol");
		silent = !strcmp(scenario, "silent");
		stuck_clock = !strcmp(scenario, "clock_timeout");
		rcg_stuck = !strcmp(scenario, "rcg_timeout");
		stuck_tx = !strcmp(scenario, "tx_timeout");
		bad_reset = !strcmp(scenario, "bad_reset");
		garbled = !strcmp(scenario, "loopback_garbled");
		short_version = !strcmp(scenario, "short_version");
		cc_version = !strcmp(scenario, "cc_version");
		rx_error = !strcmp(scenario, "rx_error");
		coalesced = !strcmp(scenario, "coalesced");
		sw_low = !strcmp(scenario, "old_wrapper");
		not_an_event = !strcmp(scenario, "not_an_event");
		shortest_version = !strcmp(scenario, "shortest_version");
		short_complete = !strcmp(scenario, "short_complete");
		last_word_four = !strcmp(scenario, "last_word_four");
		/* (with another event first the version's answer ends on a whole word: 7 + 17 bytes, whose last valid count is 4) */
		other_first = !strcmp(scenario, "other_first") || last_word_four;
		/* the engine's FIFOs: words 32 bits wide, the TX one at least four deep, the RX one too */
		if (!strcmp(scenario, "fifo_narrow")) { engine[0xe24 / 4] = (16u << 24) | (16u << 16); }
		if (!strcmp(scenario, "fifo_shallow")) { engine[0xe24 / 4] = (32u << 24) | (3u << 16); }
		if (!strcmp(scenario, "fifo_four")) { engine[0xe24 / 4] = (32u << 24) | (4u << 16); }
		if (!strcmp(scenario, "rx_shallow")) { engine[0xe28 / 4] = (32u << 24) | (3u << 16); }
		if (!strcmp(scenario, "rx_four")) { engine[0xe28 / 4] = (32u << 24) | (4u << 16); }
		if (!strcmp(scenario, "no_mapping")) { unmapped = b->bluetooth.serial_engine; }
		model.input[b->bluetooth.sw_ctrl] = !sw_low;
		if (!strcmp(scenario, "unsupported_arbiter")) { spmi.version = 0x80000000; }
		if (!strcmp(scenario, "look") || !strcmp(scenario, "look_clocks_off")) { boot_args = "bt=-1"; }
		clocks_off_until_voted = !strcmp(scenario, "clocks_off") || !strcmp(scenario, "look_clocks_off");
		if (!strcmp(scenario, "darwins") || !strcmp(scenario, "not_darwins")) { boot_args = "bt=2 bt_fw=0"; }
		firmware_files();
		if (!strncmp(scenario, "firmware", 8)) {
			boot_args = !strcmp(scenario, "firmware_rom") ? "bt=2 bt_fw=0" : "bt=2";
			refuse_segment = !strcmp(scenario, "firmware_refused") ? 4 : -1;   /* (the patch's last: always answered) */
			fw[1].there = strcmp(scenario, "firmware_missing") != 0;
			reset_unanswered = !strcmp(scenario, "firmware_no_reset");
			if (!strcmp(scenario, "firmware_junk")) {
				fw[0].file[0] = 9;              /* (a type of TLV file there is not) */
			}
			if (!strcmp(scenario, "firmware_elf")) {
				/* an ELF patch (.mbn): sent as it is, every segment answered */
				memcpy(fw[0].file, "\x7f" "ELF\x02\x01\x01", 7);
				fw[0].mode = 0;
			}
		}
		if (!strcmp(scenario, "not_darwins")) { start_error = ENXIO; }
		run();
		if (wrong_protocol || spmi.version == 0x80000000 || !strcmp(scenario, "look") || !strcmp(scenario, "look_clocks_off") ||
		    refused != NULL) {
			CHECK(!en.writes && !rsc.writes && !model.writes && !tx_commands, "inspection/refusal performs no hardware changes");
			if (clocks_off_until_voted) {
				CHECK(logged("the serial engine's clocks are off: its registers are read once they are on") &&
				    !logged("GENI_FW_REVISION_RO"), "clocks off: said, the engine not read: %s", log_text);
			}
			if (wrong_protocol) { CHECK(logged("SE protocol is not UART"), "protocol refusal logged"); }
			if (refused != NULL) { CHECK(logged(refused), "refusal logged: %s", log_text); }
		} else if (!strcmp(scenario, "firmware_refused") || !strcmp(scenario, "firmware_missing") ||
		    !strcmp(scenario, "firmware_junk") || !strcmp(scenario, "firmware_no_reset")) {
			CHECK(logged("the firmware was not taken") && logged("bring-up stopped; powering down") && !en.reg[0x44] && handed == NULL,
			    "firmware not taken: powered down, not handed to Darwin: %s", log_text);
			if (refuse_segment >= 0) {
				CHECK(logged("segment 4 refused (status 0x5)") && segments_seen == 5, "the segment refused, and nothing after it");
			} else if (!strcmp(scenario, "firmware_junk")) {
				CHECK(logged("/fw-bt/msbtfw11.tlv (1000 bytes) is neither a TLV file nor an ELF one") && segments_seen == 0,
				    "a file that is none: nothing of it sent");
			} else if (reset_unanswered) {
				CHECK(segments_seen == 8 && logged("reset response timed out"), "all of it sent, the reset after it unanswered");
			} else {
				CHECK(logged("no /fw-bt/msnv11.bin in the RAM disks") && fw[0].got == fw[0].size, "the patch sent, the configuration missing");
			}
		} else if (start_error != 0) {
			CHECK(logged("not handed to Darwin (6)") && logged("bring-up stopped; powering down") && !en.reg[0x44],
			    "Darwin's side refusing it: the controller powered down: %s", log_text);
		} else if (!strcmp(scenario, "no_mapping")) {
			CHECK(logged("bt-uart: mapping failed") && !en.writes && !rsc.writes && !model.writes && !tx_commands,
			    "a block not mapped: nothing done: %s", log_text);
		} else if (!strcmp(scenario, "fifo_narrow") || !strcmp(scenario, "fifo_shallow") || !strcmp(scenario, "rx_shallow")) {
			CHECK(logged(!strcmp(scenario, "rx_shallow") ? "unsupported RX FIFO geometry" : "unsupported TX FIFO geometry") &&
			    logged("bring-up stopped; powering down") && !en.reg[0x44] && !tx_commands, "a FIFO it cannot use: refused: %s", log_text);
		} else if (shortest_version || short_complete) {
			CHECK(logged(shortest_version ? "version response too short (5 bytes)" : "version response too short (7 bytes)") &&
			    logged("bring-up stopped; powering down") && !en.reg[0x44], "a version answer with no version in it: %s", log_text);
		} else if (silent || stuck_clock || rcg_stuck || stuck_tx || bad_reset || short_version || rx_error || not_an_event) {
			CHECK(logged("bring-up stopped; powering down") && !logged("identification complete"), "failure is explicit: %s", log_text);
			CHECK(!en.reg[0x44], "failure powers down");
			if (silent) {
				CHECK(loopbacks == 4 && tx_commands == 4 && logged("reset response timed out"), "silent controller tries all four configurations without getver");
				CHECK(read_starts == 8 && rx_cancels == 7, "four RX setups, four loopback restarts, seven cancels");
				CHECK(logged("loopback: 5 of 5 bytes back, the same"), "internal loopback succeeds with silent controller");
			}
			if (not_an_event) {
				CHECK(logged("unexpected H4 packet type 0x2"), "a packet that is not an event is said: %s", log_text);
			}
			if (short_version) {
				CHECK(logged("version response too short (7 bytes)"), "how short, said: %s", log_text);
			}
		} else {
			CHECK(logged("product id 0x7750, patch version 0x1234, ROM version 0x5678, SoC id 0x11223344"), "decoded version: %s", log_text);
			CHECK(logged("identification complete") && tx_commands == 2, "both commands completed");
			CHECK(!strncmp(scenario, "firmware", 8) || segments_seen == 0, "no firmware sent but at bt=2 (%u segments)", segments_seen);
			CHECK(!other_first || !logged("too short"), "another command's answer passed over, not taken for the version's: %s", log_text);
			CHECK(loopbacks == 1 && read_starts == 2 && rx_cancels == 1, "first configuration succeeds after loopback RX restart");
			if (garbled) {
				/* (said, and nothing more: the controller may still answer, and it does) */
				CHECK(logged("loopback: 5 of 5 bytes back\n") && !logged(", the same"), "a changed byte is not the same: %s", log_text);
			} else {
				CHECK(logged("loopback: 5 of 5 bytes back, the same"), "all probe bytes echoed intact");
			}
			if (!strncmp(scenario, "firmware", 8)) {
				firmware_checks(scenario);
			}
			if (handed != NULL) {
				darwins();
			}
			CHECK(en.reg[0x44] == 0x80, "enable high until quiesce");
			CHECK(en.reg[0x40] == 1 && en.reg[0x46] == 0x80, "the enable GPIO a digital output, and enabled: mode %#x, enable %#x",
			    en.reg[0x40], en.reg[0x46]);
			CHECK(en.writes == 4 && spmi.writes == 4, "the four writes of the power-up, all to the enable GPIO: %u of %u", en.writes,
			    spmi.writes);
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
	CHECK(se_reads_off == 0, "no register of the serial engine read while its clocks were off: %u", se_reads_off);
	if (!strcmp(scenario, "clocks_off")) {
		CHECK(logged("the serial engine's clocks are off") && logged("GENI_FW_REVISION_RO 0x200"), "looked at once voted: %s",
		    log_text);
	}
	if (on_second_bus) {
		/* the controller answered (above), so the GPIO the driver inspected and drove is the second bus's; the first
		 * bus's, through the arbiter's channels of the same numbers, was never asked and is as it was */
		CHECK(logged("identification complete") && pmic_reads == 0x7f && en.writes == 5 && spmi.writes == 5,
		    "inspected and driven on the second bus: reads %#x, %u of %u writes", pmic_reads, en.writes, spmi.writes);
		CHECK(!namesake.writes && !namesake_reads && !namesake.reg[0x40] && !namesake.reg[0x44] && !namesake.reg[0x46],
		    "the first bus's GPIO neither read nor written (%u, %u)", namesake_reads, namesake.writes);
		CHECK(!spmi.past_the_table, "%u reads of the first bus's part of the tables", spmi.past_the_table);
	}
	CHECK(!unexpected && !spmi.elsewhere && !model.elsewhere && !rsc.elsewhere, "all traffic inside declared register models");
	CHECK(!rsc.changed_while_sent && !spmi.denied && !rpmh_locks && !spmi_locks && canary_whole(), "shared transports and database intact");
	fprintf(stderr, "%-20s %s\n", scenario, failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
