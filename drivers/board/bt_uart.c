/*
 * Qualcomm Bluetooth on a QUP GENI UART: power, the UART, H4 identification and, a level further, the controller
 * handed to Darwin (board/bt_host.c: bluetoothd's service, nodes, Skywalk channel and kernel control), H4
 * packets either way through the UART. Before it is handed over it is given the firmware bluetooth { } names, the
 * phone's own patch and configuration (board/bt_qca.h); without them it runs what its ROM has.
 * Wiring and resource names come from bluetooth { } and the SoC file. Register layouts and HCI
 * framing: Linux drivers/tty/serial/qcom_geni_serial.c, drivers/soc/qcom/qcom-geni-se.c,
 * drivers/pinctrl/qcom/pinctrl-spmi-gpio.c and drivers/bluetooth/btqca.{c,h}.
 * Inspect before powering; refuse a non-UART SE, a busy engine, or an unsupported PMIC arbiter.
 */
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/board/bt.h>
#include <sys/board/input.h>
#include <sys/errno.h>

#include "kernel.h"
#include "board.h"
#include "mmio.h"
#include "time.h"
#include "log.h"
#include "cmd_db.h"
#include "spmi.h"
#include "bt_hci.h"
#include "bt_qca.h"

static vm_offset_t se, gcc, tlmm, wrapper, db, rsc_base, enable_read, enable_write;
static bool returning, enable_changed, uart_changed, pins_changed, rcg_changed;
static uint32_t added_votes, rail[8], rf_clock, saved_rcg[5], saved_pins[5];
static unsigned rails_added;
static bool rf_added;
#define W (&hakios_board()->bluetooth)
#define HAKIOS_TLMM tlmm
#include "tlmm.h"

/* RPMh's shared polling helper must spin, never sleep, when the other cores have been stopped. */
static void
bt_wait_us(uint32_t us)
{
	if (returning) {
		spin_us(us);
	} else {
		sleep_us(us);
	}
}
#define sleep_us bt_wait_us
#include "rpmh.h"
#undef sleep_us

#define SE(r) (se + (r))
#define RCG(r) (gcc + W->clock_source + (r))
#define VOTE (gcc + W->vote_register)
#define START (1u << 27)
#define M_DONE 1u
#define M_ERRORS 0x3300002eu  /* command failure, overrun, illegal command, command abort */
#define M_CANCELLED (1u << 4)
#define M_ABORTED (1u << 5)
#define S_ERRORS 0x3300002eu

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	const char *problem;

	if (!returning) {
		hakios_rpmh_lock();
	}
	problem = hakios_rpmh_send_rsc(rsc_base, hakios_board()->rpmh.tcs_offset, address, data, count);
	if (!returning) {
		hakios_rpmh_unlock();
	}
	if (problem) {
		hakios_say("bt-uart: RPMh: %s\n", problem);
	}
	return problem == NULL;
}

static bool
pmic_write(uint8_t reg, uint8_t value)
{
	const char *problem;

	if (!returning) {
		hakios_spmi_lock();
	}
	problem = hakios_spmi_write(enable_write, reg, value);
	if (!returning) {
		hakios_spmi_unlock();
	}
	if (problem) {
		hakios_say("bt-uart: enable GPIO write %#x: %s\n", reg, problem);
	}
	return problem == NULL;
}

/* Every resource is resolved and type-checked before the first regulator vote. */
static bool
resources(void)
{
	const struct cmd_db_header *header = (const struct cmd_db_header *)db;

	if (!hakios_cmd_db_valid(header)) {
		hakios_say("bt-uart: invalid command database\n");
		return false;
	}
	for (unsigned i = 0; i <= W->supply_count; i++) {
		const char *name = i == W->supply_count ? W->rf_clock : W->supplies[i];
		uint32_t address = hakios_cmd_db_address(header, hakios_board()->rpmh.cmd_db_size, name);

		hakios_say("bt-uart: resource %s at %#x\n", name, address);
		if (!address || ((address >> 16) & 15) != 4) {
			hakios_say("bt-uart: resource is not VRM; stopping\n");
			return false;
		}
		if (i == W->supply_count) {
			rf_clock = address;
		} else {
			rail[i] = address;
		}
	}
	return true;
}

/* The engine's three clocks running (the halt registers bluetooth { } names: bit 31 is set while a clock is off) */
static bool
engine_clocked(void)
{
	for (unsigned i = 0; i < 3; i++) {
		if (rd(gcc + W->clocks[i]) & (1u << 31)) {
			return false;
		}
	}
	return true;
}

static bool engine_looked_at;

/* The engine's own registers, read only while it is clocked (a read of a block whose clock is off can hang the bus):
 * a UART's, at rest */
static bool
engine_ready(void)
{
	uint32_t revision = rd(SE(0x68));

	engine_looked_at = true;
	hakios_say("bt-uart: GENI_FW_REVISION_RO %#x (protocol %u), QUP HW version %#x\n",
	    revision, (revision >> 8) & 0xff, rd(wrapper + 4));
	if (((revision >> 8) & 0xff) != 2) {
		hakios_say("bt-uart: SE protocol is not UART (2); stopping\n");
		return false;
	}
	if ((rd(SE(0x40)) & 0x1001) || (rd(SE(0x64)) & 1) || (rd(SE(0x800)) & 0xfffffff)) {
		hakios_say("bt-uart: SE busy, TX FIFO not empty or FIFO interface disabled; stopping\n");
		return false;
	}
	return true;
}

static bool
inspect(void)
{
	struct hakios_spmi arb = { 0 };         /* (all of it set by hakios_spmi_map; a compiler that cannot see so is told) */
	bool mapped = hakios_spmi_map(&arb);
	static const uint8_t regs[] = { 0x05, 0x40, 0x41, 0x42, 0x44, 0x45, 0x46 };

	hakios_say("bt-uart: GCC votes %#x, halt %#x %#x %#x; RCG cmd %#x cfg %#x\n", rd(VOTE),
	    rd(gcc + W->clocks[0]), rd(gcc + W->clocks[1]), rd(gcc + W->clocks[2]), rd(RCG(0)), rd(RCG(4)));
	for (unsigned i = 0; i < 5; i++) {
		unsigned pin = i == 4 ? W->sw_ctrl : W->uart_pins[i];

		saved_pins[i] = rd(GPIO_CFG(pin));
		hakios_say("bt-uart: gpio%u cfg %#x level %u\n", pin, saved_pins[i], gpio_get(pin));
	}
	if (!engine_clocked()) {
		hakios_say("bt-uart: the serial engine's clocks are off: its registers are read once they are on, not before\n");
	} else if (!engine_ready()) {
		return false;
	}
	if (!mapped) {
		hakios_say("bt-uart: unsupported SPMI arbiter version %#x; stopping\n", arb.version);
		return false;
	}
	for (unsigned n = 0; n < arb.channels; n++) {
		if (SPMI_ID_OF(hakios_spmi_channel_map(&arb, n)) == W->enable_ppid &&
		    hakios_spmi_channel_owner(&arb, n) == arb.system) {
			enable_read = hakios_spmi_observer(&arb, n);
			enable_write = hakios_spmi_map_write(&arb, n);
			break;
		}
	}
	if (!enable_read || !enable_write) {
		hakios_say("bt-uart: no owned enable GPIO channel for PPID %#x\n", W->enable_ppid);
		return false;
	}
	for (unsigned i = 0; i < sizeof(regs); i++) {
		uint8_t value = 0;
		const char *problem;

		hakios_spmi_lock();
		problem = hakios_spmi_read(enable_read, regs[i], &value, 1);
		hakios_spmi_unlock();
		if (problem) {
			hakios_say("bt-uart: enable GPIO read: %s\n", problem);
			return false;
		}
		hakios_say("bt-uart: enable GPIO reg %#x = %#x\n", regs[i], value);
		/* Linux pinctrl-spmi-gpio: LV/MV GPIO subtypes 0x10..0x13. */
		if (i == 0 && (value < 0x10 || value > 0x13)) {
			hakios_say("bt-uart: enable GPIO is not LV/MV; stopping\n");
			return false;
		}
	}
	return resources();
}

static bool
power_on(void)
{
	for (unsigned i = 0; i < W->supply_count; i++) {
		uint32_t address[] = { rail[i], rail[i] + 4, rail[i] + 8 };
		uint32_t value[] = { W->supply_mv[i], 1, 7 };

		hakios_say("bt-uart: supply %s %u mV on\n", W->supplies[i], W->supply_mv[i]);
		/* Remember even an unacknowledged request: it may have reached the resource. */
		rails_added = i + 1;
		if (!rpmh_write(address, value, W->supplies[i][0] == 'L' ? 3 : 2)) {
			return false;
		}
	}
	uint32_t address = rf_clock + 4, value = 1;

	rf_added = true;
	hakios_say("bt-uart: RF clock %s on\n", W->rf_clock);
	if (!rpmh_write(&address, &value, 1)) {
		return false;
	}
	/* Controller power/reset timing from the vendor Bluetooth power sequence. */
	sleep_ms(5);
	enable_changed = true;
	hakios_say("bt-uart: BT_EN low for 10 ms\n");
	if (!pmic_write(0x44, 0) || !pmic_write(0x40, 1) || !pmic_write(0x46, 0x80)) {
		return false;
	}
	sleep_ms(10);
	if (!pmic_write(0x44, 0x80)) {
		return false;
	}
	hakios_say("bt-uart: BT_EN high\n");
	pins_changed = true;
	gpio_configure(W->sw_ctrl, 0, PULL_NONE, 2, false);
	for (unsigned i = 0; i < 200 && !gpio_get(W->sw_ctrl); i++) {
		sleep_ms(1);
	}
	hakios_say("bt-uart: sw-ctrl %s after wait (200 ms maximum)\n", gpio_get(W->sw_ctrl) ? "high" : "low");
	sleep_ms(100);
	return true;
}

static bool
wait_clear(vm_offset_t address, uint32_t mask, unsigned tries)
{
	for (unsigned i = 0; i < tries; i++) {
		if (!(rd(address) & mask)) {
			return true;
		}
		bt_wait_us(10);
	}
	return false;
}

static bool
clocks_on(void)
{
	uint32_t mask = 0;

	for (unsigned i = 0; i < 5; i++) {
		mask |= 1u << W->votes[i];
	}
	added_votes = mask & ~rd(VOTE);
	wr(VOTE, rd(VOTE) | mask);
	for (unsigned i = 0; i < 3; i++) {
		if (!wait_clear(gcc + W->clocks[i], 1u << 31, 1000)) {
			hakios_say("bt-uart: GCC clock %u stays off\n", i);
			return false;
		}
	}
	if (!wait_clear(RCG(0), 1, 500)) {
		hakios_say("bt-uart: RCG already busy\n");
		return false;
	}
	for (unsigned i = 0; i < 5; i++) {
		saved_rcg[i] = rd(RCG(4 * i));
	}
	rcg_changed = true;
	/* 300 MHz * 384/15625 = 7,372,800 Hz, HID=1 (undivided), dual-edge MND. */
	wr(RCG(8), 384);
	wr(RCG(12), (~(15625u - 384)) & 0xffff);
	wr(RCG(16), (~15625u) & 0xffff);
	wr(RCG(4), 1 | (6 << 8) | (2 << 12));
	wr(RCG(0), rd(RCG(0)) | 1);
	if (!wait_clear(RCG(0), 1, 500)) {
		hakios_say("bt-uart: RCG update timed out\n");
		return false;
	}
	hakios_say("bt-uart: GCC clocks on, SE source 7372800 Hz\n");
	return true;
}

static bool
uart_init(unsigned sampling, bool flow)
{
	uint32_t depth = (rd(SE(0xe24)) >> 16) & 0x3f;
	unsigned div = 7372800 / (115200 * sampling);

	if (depth < 4 || ((rd(SE(0xe24)) >> 24) & 0x3f) != 32) {
		hakios_say("bt-uart: unsupported TX FIFO geometry %#x\n", rd(SE(0xe24)));
		return false;
	}
	uint32_t rx_depth = (rd(SE(0xe28)) >> 16) & 0x3f;

	if (rx_depth < 4) {
		hakios_say("bt-uart: unsupported RX FIFO geometry\n");
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		gpio_configure(W->uart_pins[i], W->uart_function, PULL_NONE, 2, false);
	}
	uart_changed = true;
	wr(SE(0xe18), 0);                 /* GSI events off, FIFO mode */
	wr(SE(0x618), 0xffffffff);
	wr(SE(0x648), 0xffffffff);
	wr(SE(0xc44), 0xffffffff);
	wr(SE(0xd44), 0xffffffff);
	wr(SE(0x28), rd(SE(0x28)) | 0x7f);
	wr(SE(0xe30), rd(SE(0xe30)) | 0xf);
	wr(SE(0x24), 0x7f);
	wr(SE(0x20), 1);
	wr(SE(0x258), rd(SE(0x258)) & ~1u);
	wr(SE(0x810), rx_depth - 3);
	wr(SE(0x814), rx_depth - 2);
	wr(SE(0x80c), 0);                 /* TX watermark */
	wr(SE(0xe1c), 0xf);
	wr(SE(0x614), 0x7fc0007f);
	wr(SE(0x644), 0x7fc0007f);
	wr(SE(0x22c), 0);
	wr(SE(0x25c), flow ? 0 : 2);      /* UART_CTS_MASK clear: hardware flow control; set: CTS ignored */
	wr(SE(0x268), 8);
	wr(SE(0x26c), 0);
	wr(SE(0x280), 0);
	wr(SE(0x28c), 8);
	wr(SE(0x294), 160);
	wr(SE(0x2a4), 0);
	wr(SE(0x2a8), 0);
	wr(SE(0x2ac), 0);
	/* Linux packing: start bits 7,15,23,31; direction=0, length=7, stop on entry 3. */
	wr(SE(0x260), 0x0007b8ee);
	wr(SE(0x264), 0x000fbeee);
	wr(SE(0x284), 0x0007b8ee);
	wr(SE(0x288), 0x000fbeee);
	wr(SE(0x254), 0);
	wr(SE(0x7c), 0);
	wr(SE(0x48), (div << 4) | 1);
	wr(SE(0x4c), (div << 4) | 1);
	wr(SE(0x630), START);             /* UART_START_READ, once for both commands */
	hakios_say("bt-uart: UART 115200 8N1, %s, sampling %u divider %u, RX started\n", flow ? "CTS/RTS" : "no flow control",
	    sampling, div);
	return true;
}

/*
 * A transmit that went wrong part way (the FIFO stayed full, an error, no done): the engine's command cancelled, or
 * aborted when it will not cancel, and its interrupts taken, so that the next command starts on an engine at rest --
 * not on one still busy with this one, whose words would go out as the rest of it. false.
 */
static bool
transmit_failed(void)
{
	wr(SE(0x604), 1u << 2);                 /* M_GENI_CMD_CTRL: cancel */
	for (unsigned i = 0; i < 100 && !(rd(SE(0x610)) & M_CANCELLED); i++) {
		sleep_us(10);
	}
	if (!(rd(SE(0x610)) & M_CANCELLED)) {
		wr(SE(0x604), 1u << 1);         /* abort */
		for (unsigned i = 0; i < 100 && !(rd(SE(0x610)) & M_ABORTED); i++) {
			sleep_us(10);
		}
	}
	wr(SE(0x618), 0xffffffff);
	return false;
}

static bool
transmit(const uint8_t *bytes, unsigned length)
{
	uint32_t depth = (rd(SE(0xe24)) >> 16) & 0x3f;  /* (at least four words: uart_init() refused fewer) */

	wr(SE(0x618), 0xffffffff);
	wr(SE(0x270), length);
	wr(SE(0x600), START);
	/* A word at a time, each once the FIFO has room for it: a packet of Darwin's is longer than the FIFO. */
	for (unsigned i = 0; i < length; i += 4) {
		uint32_t word = 0;

		for (unsigned j = 0; j < 4 && i + j < length; j++) {
			word |= (uint32_t)bytes[i + j] << (8 * j);
		}
		for (unsigned tries = 0; (rd(SE(0x800)) & 0xfffffff) >= depth; tries++) {
			if (tries >= 1000) {
				hakios_say("bt-uart: TX FIFO stays full\n");
				return transmit_failed();
			}
			sleep_us(100);
		}
		wr(SE(0x700), word);
	}
	/* (at 115200 baud a byte takes 87 us: the longest packet's last word leaves in about 90 ms) */
	for (unsigned i = 0; i < 1000 + length; i++) {
		uint32_t status = rd(SE(0x610));

		if (status & M_ERRORS) {
			hakios_say("bt-uart: TX error %#x\n", status);
			return transmit_failed();
		}
		if (status & M_DONE) {
			wr(SE(0x618), status);
			return true;
		}
		sleep_us(100);
	}
	hakios_say("bt-uart: TX timed out\n");
	return transmit_failed();
}

/* Keep unused bytes of a FIFO word, including when two H4 events share that word. */
static uint32_t rx_word;
static unsigned rx_left;

/*
 * A byte from the controller: one the FIFO's last word still holds whatever the time, else one that comes before
 * DEADLINE. (The deadline is the wait's measure: a count of tries was, and gave up after half a second whatever the
 * deadline said. The tries left are a backstop for a clock that does not move.)
 */
static bool
receive_byte(uint8_t *byte, uint64_t deadline)
{
	static unsigned said;

	for (unsigned tries = 0; tries < 1000000; tries++) {
		uint32_t irq;

		if (rx_left) {
			*byte = (uint8_t)rx_word;
			rx_word >>= 8;
			rx_left--;
			return true;
		}
		irq = rd(SE(0x640));

		if (irq) {
			wr(SE(0x648), irq);
			if (irq & S_ERRORS) {
				hakios_say("bt-uart: RX error %#x\n", irq);
				return false;
			}
		}
		if (!rx_left) {
			uint32_t status = rd(SE(0x804)), words = status & 0x1ffffff;

			if (words) {
				rx_left = 4;
				if (words == 1 && (status & (1u << 31))) {
					unsigned valid = (status >> 28) & 7;

					if (valid > 4) {
						/* (taken out all the same: left in the FIFO, it would stop every read after it) */
						(void)rd(SE(0x780));
						rx_left = 0;
						if (said++ < 4) {
							hakios_say("bt-uart: the RX FIFO's last word said to hold %u bytes of 4: thrown away\n",
							    valid);
						}
						continue;
					}
					rx_left = valid ? valid : 4;
				}
				rx_word = rd(SE(0x780));
				continue;
			}
		}
		if (now_us() >= deadline) {
			return false;
		}
		sleep_us(100);
	}
	return false;
}

/* What the engine and its lines say, for a log read afterwards. */
static void
uart_state(const char *when)
{
	hakios_say("bt-uart: %s: M_IRQ %#x S_IRQ %#x TX FIFO %#x RX FIFO %#x status %#x; RCG cfg %#x m %#x n %#x d %#x; "
	    "lines CTS %u RFR %u TX %u RX %u\n", when, rd(SE(0x610)), rd(SE(0x640)), rd(SE(0x800)), rd(SE(0x804)),
	    rd(SE(0x40)), rd(RCG(4)), rd(RCG(8)), rd(RCG(12)), rd(RCG(16)), gpio_get(W->uart_pins[0]),
	    gpio_get(W->uart_pins[1]), gpio_get(W->uart_pins[2]), gpio_get(W->uart_pins[3]));
}

/* Stop the read command and empty the receive FIFO, so the next configuration starts clean. */
static void
uart_stop_rx(void)
{
	wr(SE(0x634), 1u << 2);         /* S_CMD_CTRL: cancel */
	bt_wait_us(2000);
	for (unsigned i = 0; i < 64 && (rd(SE(0x804)) & 0x1ffffff); i++) {
		(void)rd(SE(0x780));
	}
	wr(SE(0x648), 0xffffffff);
	rx_word = 0;
	rx_left = 0;
}

static bool receive_byte(uint8_t *byte, uint64_t deadline);

/* The engine's own loopback (TX to RX, RFR to CTS): whether sending and receiving work at all. */
static void
uart_loopback(void)
{
	static const uint8_t probe[] = { 0x55, 0xa5, 0x0f, 0xf0, 0x3c };
	uint8_t back[sizeof(probe)];
	uint64_t deadline;
	unsigned got = 0;

	wr(SE(0x22c), 3);
	bt_wait_us(1000);
	if (transmit(probe, sizeof(probe))) {
		deadline = now_us() + 100000;
		while (got < sizeof(probe) && receive_byte(&back[got], deadline)) {
			got++;
		}
	}
	wr(SE(0x22c), 0);
	/* (said, and nothing more: the controller may answer whatever the engine's own loop gave back) */
	hakios_say("bt-uart: loopback: %u of %u bytes back%s\n", got, (unsigned)sizeof(probe),
	    got == sizeof(probe) && !memcmp(back, probe, sizeof(probe)) ? ", the same" : "");
	uart_stop_rx();
	wr(SE(0x630), START);
}

static uint32_t
little32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool
version_event(const uint8_t *event, unsigned length)
{
	unsigned offset;

	/* btqca EDL response: command response 0, patch/app version response 0x19/2. */
	if (length >= 5 && event[1] == 0xff && event[3] == 0 && (event[4] == 2 || event[4] == 0x19)) {
		offset = 5;
	} else if (length >= 7 && event[1] == 0x0e && event[4] == 0 && event[5] == 0xfc && event[6] == 0) {
		/* Modern command-complete response: status followed by qca_btsoc_version. */
		offset = 7;
	} else {
		return false;
	}
	if (length < offset + 12) {
		hakios_say("bt-uart: version response too short (%u bytes)\n", length);
		return false;
	}
	const uint8_t *p = event + offset;

	hakios_say("bt-uart: product id %#x, patch version %#x, ROM version %#x, SoC id %#x\n",
	    little32(p), p[4] | p[5] << 8, p[6] | p[7] << 8, little32(p + 8));
	return true;
}

/* One H4 event from the controller by DEADLINE, into EVENT (258 bytes), logged when SAID: its length; 0 when none
 * came in time; -1 when what came is not an event. */
static int
receive_event(uint8_t *event, uint64_t deadline, bool said)
{
	unsigned length = 3;

	for (unsigned i = 0; i < length; i++) {
		if (!receive_byte(&event[i], deadline)) {
			return 0;
		}
		if (i == 0 && event[0] != 4) {
			hakios_say("bt-uart: unexpected H4 packet type %#x\n", event[0]);
			return -1;
		}
		if (i == 2) {
			length += event[2];
		}
	}
	/* Log bounded chunks so neither log's line buffer can truncate an event. */
	for (unsigned i = 0; said && i < length; i += 16) {
		char line[16 * 3 + 1];
		unsigned at = 0;

		for (unsigned j = i; j < length && j < i + 16; j++) {
			at += (unsigned)snprintf(line + at, sizeof(line) - at, "%02x ", event[j]);
		}
		hakios_say("bt-uart: H4 event +%u: %s\n", i, line);
	}
	return (int)length;
}

static bool
wait_event(bool version)
{
	uint8_t event[258];
	uint64_t deadline = now_us() + 500000;
	static const uint8_t reset[] = { 4, 0x0e, 4, 1, 3, 0x0c, 0 };

	/* A peer flooding unrelated events cannot extend the deadline or the event budget. */
	for (unsigned n = 0; n < 32; n++) {
		int length = receive_event(event, deadline, true);

		if (length < 0) {
			return false;
		}
		if (length == 0) {
			break;
		}
		if (version ? version_event(event, (unsigned)length) : ((unsigned)length == sizeof(reset) && !memcmp(event, reset, sizeof(reset)))) {
			return true;
		}
	}
	hakios_say("bt-uart: %s response timed out (500 ms/event budget)\n", version ? "getver" : "reset");
	return false;
}

/* ---- the firmware: the patch to the controller's ROM and its configuration (board/bt_qca.h) ------------------- */

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);     /* board/image.c */

/* The controller's answer to segment NUMBER: true when it took it. Other events before it are passed over (at most
 * 16, within 2 s: a segment of the patch is written into the controller's RAM before it answers). */
static bool
wait_segment(uint32_t number)
{
	uint8_t event[258];
	uint64_t deadline = now_us() + 2000000;

	for (unsigned n = 0; n < 16; n++) {
		int length = receive_event(event, deadline, false), answer;

		if (length <= 0) {
			break;
		}
		if ((answer = bt_qca_segment_answer(event, (unsigned)length)) != 0) {
			if (answer < 0) {
				hakios_say("bt-uart: segment %u refused (status %#x)\n", number, event[1] == 0xff ? event[5] : event[6]);
			}
			return answer > 0;
		}
	}
	hakios_say("bt-uart: segment %u: no answer within 2 s\n", number);
	return false;
}

/* The file NAME of the board's firmware directory sent to the controller, in segments: true when all were taken */
static bool
download(const char *name)
{
	const char *directory = W->firmware != NULL ? W->firmware : "fw";
	uint64_t size = 0;
	const uint8_t *data = hakios_image_file(directory, name, &size, NULL);
	static uint8_t command[BT_QCA_COMMAND_MOST];
	struct bt_qca_file f;
	uint32_t count;

	if (data == NULL) {
		hakios_say("bt-uart: no /%s/%s in the RAM disks: the controller's firmware is taken from the phone's own "
		    "vendor/firmware (bluetooth { } names it)\n", directory, name);
		return false;
	}
	f = bt_qca_identify(data, size > 0xffffffffu ? 0 : (uint32_t)size);
	if (f.kind == BT_QCA_NOT_ONE) {
		hakios_say("bt-uart: /%s/%s (%llu bytes) is neither a TLV file nor an ELF one\n", directory, name, (unsigned long long)size);
		return false;
	}
	count = bt_qca_segments((uint32_t)size);
	hakios_say("bt-uart: sending /%s/%s: %s, %llu bytes in %u segments (download mode %u, product %#x, ROM build %#x, "
	    "patch %#x)\n", directory, name, f.kind == BT_QCA_PATCH ? "a TLV patch" : f.kind == BT_QCA_NVM ? "a TLV configuration" :
	    "an ELF patch", (unsigned long long)size, count, f.mode, f.product, f.rom_build, f.patch_version);
	for (uint32_t i = 0; i < count; i++) {
		unsigned n = bt_qca_segment(command, sizeof(command), data, (uint32_t)size, i);

		if (n == 0 || !transmit(command, n)) {
			hakios_say("bt-uart: segment %u of /%s/%s not sent\n", i, directory, name);
			return false;
		}
		if (bt_qca_answered(f.mode, (uint32_t)size, i) && !wait_segment(i)) {
			return false;
		}
	}
	hakios_say("bt-uart: /%s/%s taken\n", directory, name);
	return true;
}

/* The patch, then the configuration, then HCI_Reset, after which the controller runs what it was given */
static bool
firmware(void)
{
	static const uint8_t reset[] = { 1, 3, 0x0c, 0 };

	if (!download(W->rampatch) || (W->nvm != NULL && !download(W->nvm))) {
		return false;
	}
	hakios_say("bt-uart: sending HCI_Reset after the firmware\n");
	return transmit(reset, sizeof(reset)) && wait_event(false);
}

/* Shared rollback: locks and sleeping are bypassed only at loader return. */
static void
bt_cleanup(void)
{
	if (enable_changed) {
		(void)pmic_write(0x44, 0);
		enable_changed = false;
		hakios_say("bt-uart: BT_EN low at quiesce\n");
	}
	if (uart_changed) {
		wr(SE(0x604), 2);            /* abort M and S commands; bounded acknowledgement wait */
		wr(SE(0x634), 2);
		if (!wait_clear(SE(0x40), 0x1001, 1000)) {
			hakios_say("bt-uart: UART abort timed out at cleanup\n");
		}
		wr(SE(0x614), 0);
		wr(SE(0x644), 0);
		wr(SE(0xe1c), 0);
		wr(SE(0x48), 0);
		wr(SE(0x4c), 0);
		uart_changed = false;
	}
	if (rcg_changed) {
		for (unsigned i = 1; i < 5; i++) {
			wr(RCG(4 * i), saved_rcg[i]);
		}
		wr(RCG(0), saved_rcg[0] | 1);
		if (!wait_clear(RCG(0), 1, 500)) {
			hakios_say("bt-uart: RCG restore timed out at cleanup\n");
		}
		rcg_changed = false;
	}
	if (pins_changed) {
		for (unsigned i = 0; i < 5; i++) {
			wr(GPIO_CFG(i == 4 ? W->sw_ctrl : W->uart_pins[i]), saved_pins[i]);
		}
		pins_changed = false;
	}
	if (added_votes) {
		hakios_say("bt-uart: dropping GCC votes %#x\n", added_votes);
		wr(VOTE, rd(VOTE) & ~added_votes);
		added_votes = 0;
	}
	if (rf_added) {
		uint32_t address = rf_clock + 4, zero = 0;

		hakios_say("bt-uart: RF clock vote off\n");
		(void)rpmh_write(&address, &zero, 1);
		rf_added = false;
	}
	while (rails_added) {
		uint32_t address = rail[--rails_added] + 4, zero = 0;

		hakios_say("bt-uart: supply %s enable vote off\n", W->supplies[rails_added]);
		(void)rpmh_write(&address, &zero, 1);
	}
}

/* ---- bt=2: the controller, once it has answered, Darwin's (board/bt_host.c carries the packets) ------------- */

/*
 * Once a packet has gone out in part, the controller takes what comes next for the rest of it: its framing is lost, and
 * nothing more is sent to it (Bluetooth stops until a restart) rather than commands it would read wrong.
 */
static bool tx_lost;

static int
controller_send(const uint8_t *h4, unsigned length)
{
	if (tx_lost) {
		return EIO;
	}
	if (!transmit(h4, length)) {
		tx_lost = true;
		hakios_say("bt-uart: a packet of %u bytes went out in part, or not at all: the controller is out of step with "
		    "what would follow, and is sent nothing more\n", length);
		return EIO;
	}
	return 0;
}

/*
 * Out of step with the controller: what is left of the FIFO's word and what comes until the line has been quiet for
 * 5 ms is thrown away, so that the next byte read is, as like as not, the start of a packet -- for 50 ms at most: a
 * controller that does not pause (a scan's reports back to back) would otherwise keep bluetoothd's packets waiting for
 * seconds, and the next look tries again. -1.
 */
static int
out_of_step(void)
{
	unsigned dropped = rx_left;
	uint64_t until = now_us() + 50000;
	uint8_t byte;

	rx_left = 0;
	while (dropped < 65536 && now_us() < until && receive_byte(&byte, now_us() + 5000 < until ? now_us() + 5000 : until)) {
		dropped++;
	}
	hakios_say("bt-uart: %u more bytes thrown away to be in step again\n", dropped);
	return -1;
}

/* A whole H4 packet from the controller: its type within WAIT_MS, the rest within 200 ms of it; -1: out of step */
static int
controller_receive(uint8_t *h4, unsigned room, unsigned wait_ms)
{
	uint64_t deadline = now_us() + wait_ms * 1000ull;
	unsigned have = 0, length = 0, header;

	if (room < 5 || !receive_byte(&h4[0], deadline)) {
		return 0;
	}
	have = 1;
	/* (what a controller sends: ACL 2, SCO 3, an event 4, ISO 5; its header has the length) */
	header = h4[0] == BT_H4_EVENT ? 3 : h4[0] == BT_H4_SCO ? 4 : h4[0] == BT_H4_ACL || h4[0] == BT_H4_ISO ? 5 : 0;
	if (header == 0) {
		hakios_say("bt-uart: a byte %#x from the controller where a packet was to begin: out of step\n", h4[0]);
		return out_of_step();
	}
	deadline = now_us() + 200000;
	while (have < header || have < (length = bt_h4_length(h4, have))) {
		if (length > room) {
			hakios_say("bt-uart: a packet of %u bytes from the controller, room for %u: out of step\n", length, room);
			return out_of_step();
		}
		if (!receive_byte(&h4[have], deadline)) {
			hakios_say("bt-uart: a packet from the controller cut short after %u bytes\n", have);
			return out_of_step();
		}
		have++;
	}
	return (int)have;
}

static const struct board_bt_controller qualcomm_uart = {
	.name = "the Qualcomm controller on the UART", .send = controller_send, .receive = controller_receive,
};

void
hakios_bt_uart_quiesce(void)
{
	returning = true;
	bt_cleanup();
	returning = false;
}

static void
bt_thread(__unused void *arg, __unused wait_result_t result)
{
	const struct hakios_board *b = hakios_board();
	int mode = 0, fw = 1;
	static const uint8_t reset[] = { 1, 3, 0x0c, 0 };
	static const uint8_t getver[] = { 1, 0, 0xfc, 1, 0x19 };

	PE_parse_boot_argn("bt", &mode, sizeof(mode));
	/* bt_fw=0: at bt=2, the controller handed over as its ROM runs, without the firmware bluetooth { } names */
	PE_parse_boot_argn("bt_fw", &fw, sizeof(fw));
	se = ml_io_map(W->serial_engine, 0x4000);
	wrapper = ml_io_map(W->wrapper, 0x1000);
	gcc = ml_io_map(b->clocks.gcc, 0x60000);
	tlmm = ml_io_map(b->pins.tlmm, TLMM_SIZE);
	db = ml_io_map_wcomb(b->rpmh.cmd_db, b->rpmh.cmd_db_size);
	rsc_base = ml_io_map(b->rpmh.rsc, 0x10000);
	if (!se || !wrapper || !gcc || !tlmm || !db || !rsc_base) {
		hakios_say("bt-uart: mapping failed\n");
		goto done;
	}
	if (!inspect() || mode == -1) {
		goto done;
	}
	if (!power_on() || !clocks_on() || (!engine_looked_at && !engine_ready())) {
		goto failed;
	}
	/* The engine's oversampling (16 from QUP 2.5, 32 before) and the controller's CTS are what the first
	 * answer depends on; each way is tried in turn, the engine's own loopback first. */
	{
		static const struct { unsigned sampling; bool flow; } ways[] = { { 16, true }, { 16, false }, { 32, true }, { 32, false } };
		bool answered = false;

		hakios_say("bt-uart: QUP HW version %#x\n", rd(wrapper + 4));
		for (unsigned w = 0; w < sizeof(ways) / sizeof(ways[0]) && !answered; w++) {
			if (w > 0) {
				uart_stop_rx();
			}
			if (!uart_init(ways[w].sampling, ways[w].flow)) {
				goto failed;
			}
			uart_state("after setup");
			uart_loopback();
			hakios_say("bt-uart: sending HCI_Reset\n");
			answered = transmit(reset, sizeof(reset)) && wait_event(false);
			uart_state(answered ? "answered" : "no answer");
		}
		if (!answered) {
			goto failed;
		}
	}
	hakios_say("bt-uart: sending Qualcomm EDL_PATCH_GETVER\n");
	if (!transmit(getver, sizeof(getver)) || !wait_event(true)) {
		goto failed;
	}
	if (mode == 2 && W->rampatch != NULL && fw) {
		if (!firmware()) {
			hakios_say("bt-uart: the firmware was not taken\n");
			goto failed;
		}
		hakios_say("bt-uart: identification complete; the firmware taken\n");
	} else {
		hakios_say("bt-uart: identification complete; no firmware downloaded%s\n", mode != 2 ? "" : W->rampatch == NULL ?
		    " (bluetooth { } names none: the controller runs its ROM)" : " (bt_fw=0)");
	}
	if (mode == 2) {
		int error = board_bt_start(&qualcomm_uart);

		if (error != 0) {
			hakios_say("bt-uart: not handed to Darwin (%d)\n", error);
			goto failed;
		}
		hakios_say("bt-uart: the controller is Darwin's from here on\n");
	}
	goto done;
 failed:
	hakios_say("bt-uart: bring-up stopped; powering down\n");
	bt_cleanup();
 done:
	hakios_say("bt-uart: done\n");
	thread_terminate(current_thread());
}

void
hakios_bt_uart_init(void)
{
	int mode = 0;
	thread_t thread;

	/* boot-arg bt=-1: read and log only; bt=1: power, UART and HCI identification; bt=2: then the controller
	 * Darwin's (bluetoothd's); bt=0/absent: off. */
	if (!PE_parse_boot_argn("bt", &mode, sizeof(mode)) || (mode != -1 && mode != 1 && mode != 2)) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_BLUETOOTH_WCN_UART)) {
		hakios_say("bt-uart: board.dts lists no \"bluetooth-wcn-uart\" feature\n");
		return;
	}
	const struct hakios_board *b = hakios_board();

	if (!W->serial_engine || !W->wrapper || !W->supply_count || W->supply_count > 8 || !W->rf_clock ||
	    !b->clocks.gcc || !b->pins.tlmm || !b->rpmh.rsc || !b->rpmh.cmd_db || !b->spmi.channels) {
		hakios_say("bt-uart: incomplete bluetooth { }, clocks { }, pins { }, rpmh { } or spmi { }\n");
		return;
	}
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
