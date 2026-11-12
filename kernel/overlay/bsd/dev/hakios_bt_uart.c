/*
 * Qualcomm Bluetooth on a QUP GENI UART, H4 identification only. No firmware loader or iOS device.
 * Wiring and resource names come from bluetooth { } and the SoC file. Register layouts and HCI
 * framing: Linux drivers/tty/serial/qcom_geni_serial.c, drivers/soc/qcom/qcom-geni-se.c,
 * drivers/pinctrl/qcom/pinctrl-spmi-gpio.c and drivers/bluetooth/btqca.{c,h}.
 * Inspect before powering; refuse a non-UART SE, a busy engine, or an unsupported PMIC arbiter.
 */
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/hakios_input.h>

#include "hakios_kernel.h"
#include "hakios_board.h"
#include "hakios_mmio.h"
#include "hakios_time.h"
#include "hakios_log.h"
#include "hakios_cmd_db.h"
#include "hakios_spmi.h"

static vm_offset_t se, gcc, tlmm, wrapper, db, rsc_base, enable_read, enable_write;
static bool returning, enable_changed, uart_changed, pins_changed, rcg_changed;
static uint32_t added_votes, rail[8], rf_clock, saved_rcg[5], saved_pins[5];
static unsigned rails_added;
static bool rf_added;
#define W (&hakios_board()->bluetooth)
#define HAKIOS_TLMM tlmm
#include "hakios_tlmm.h"

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
#include "hakios_rpmh.h"
#undef sleep_us

#define SE(r) (se + (r))
#define RCG(r) (gcc + W->clock_source + (r))
#define VOTE (gcc + W->vote_register)
#define START (1u << 27)
#define M_DONE 1u
#define M_ERRORS 0x3300002eu  /* command failure, overrun, illegal command, command abort */
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

static bool
inspect(void)
{
	struct hakios_spmi arb;
	uint32_t revision = rd(SE(0x68));
	bool mapped = hakios_spmi_map(&arb);
	static const uint8_t regs[] = { 0x05, 0x40, 0x41, 0x42, 0x44, 0x45, 0x46 };

	hakios_say("bt-uart: GENI_FW_REVISION_RO %#x (protocol %u), QUP HW version %#x\n",
	    revision, (revision >> 8) & 0xff, rd(wrapper + 4));
	hakios_say("bt-uart: GCC votes %#x, halt %#x %#x %#x; RCG cmd %#x cfg %#x\n", rd(VOTE),
	    rd(gcc + W->clocks[0]), rd(gcc + W->clocks[1]), rd(gcc + W->clocks[2]), rd(RCG(0)), rd(RCG(4)));
	for (unsigned i = 0; i < 5; i++) {
		unsigned pin = i == 4 ? W->sw_ctrl : W->uart_pins[i];

		saved_pins[i] = rd(GPIO_CFG(pin));
		hakios_say("bt-uart: gpio%u cfg %#x level %u\n", pin, saved_pins[i], gpio_get(pin));
	}
	if (((revision >> 8) & 0xff) != 2) {
		hakios_say("bt-uart: SE protocol is not UART (2); stopping\n");
		return false;
	}
	if ((rd(SE(0x40)) & 0x1001) || (rd(SE(0x64)) & 1) || (rd(SE(0x800)) & 0xfffffff)) {
		hakios_say("bt-uart: SE busy, TX FIFO not empty or FIFO interface disabled; stopping\n");
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
			enable_write = hakios_spmi_map_write(n);
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

static bool
transmit(const uint8_t *bytes, unsigned length)
{
	wr(SE(0x618), 0xffffffff);
	wr(SE(0x270), length);
	wr(SE(0x600), START);
	/* The only commands are at most five bytes; the verified FIFO has at least four words. */
	for (unsigned i = 0; i < length; i += 4) {
		uint32_t word = 0;

		for (unsigned j = 0; j < 4 && i + j < length; j++) {
			word |= (uint32_t)bytes[i + j] << (8 * j);
		}
		wr(SE(0x700), word);
	}
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t status = rd(SE(0x610));

		if (status & M_ERRORS) {
			hakios_say("bt-uart: TX error %#x\n", status);
			return false;
		}
		if (status & M_DONE) {
			wr(SE(0x618), status);
			return true;
		}
		sleep_us(100);
	}
	hakios_say("bt-uart: TX timed out\n");
	return false;
}

/* Keep unused bytes of a FIFO word, including when two H4 events share that word. */
static uint32_t rx_word;
static unsigned rx_left;

static bool
receive_byte(uint8_t *byte, uint64_t deadline)
{
	for (unsigned tries = 0; tries < 5000 && now_us() < deadline; tries++) {
		uint32_t irq = rd(SE(0x640));

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
						return false;
					}
					rx_left = valid ? valid : 4;
				}
				rx_word = rd(SE(0x780));
			}
		}
		if (rx_left) {
			*byte = (uint8_t)rx_word;
			rx_word >>= 8;
			rx_left--;
			return true;
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
static bool
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
	hakios_say("bt-uart: loopback: %u of %u bytes back%s\n", got, (unsigned)sizeof(probe),
	    got == sizeof(probe) && !memcmp(back, probe, sizeof(probe)) ? ", the same" : "");
	uart_stop_rx();
	wr(SE(0x630), START);
	return got == sizeof(probe) && !memcmp(back, probe, sizeof(probe));
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

static bool
wait_event(bool version)
{
	uint8_t event[258];
	uint64_t deadline = now_us() + 500000;
	static const uint8_t reset[] = { 4, 0x0e, 4, 1, 3, 0x0c, 0 };

	/* A peer flooding unrelated events cannot extend the deadline or the event budget. */
	for (unsigned n = 0; n < 32; n++) {
		unsigned length = 3;

		for (unsigned i = 0; i < length; i++) {
			if (!receive_byte(&event[i], deadline)) {
				goto timeout;
			}
			if (i == 0 && event[0] != 4) {
				hakios_say("bt-uart: unexpected H4 packet type %#x\n", event[0]);
				return false;
			}
			if (i == 2) {
				length += event[2];
			}
		}
		/* Log bounded chunks so neither log's line buffer can truncate an event. */
		for (unsigned i = 0; i < length; i += 16) {
			char line[16 * 3 + 1];
			unsigned at = 0;

			for (unsigned j = i; j < length && j < i + 16; j++) {
				at += (unsigned)snprintf(line + at, sizeof(line) - at, "%02x ", event[j]);
			}
			hakios_say("bt-uart: H4 event +%u: %s\n", i, line);
		}
		if (version ? version_event(event, length) : (length == sizeof(reset) && !memcmp(event, reset, sizeof(reset)))) {
			return true;
		}
	}
 timeout:
	hakios_say("bt-uart: %s response timed out (500 ms/event budget)\n", version ? "getver" : "reset");
	return false;
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
	int mode = 0;
	static const uint8_t reset[] = { 1, 3, 0x0c, 0 };
	static const uint8_t getver[] = { 1, 0, 0xfc, 1, 0x19 };

	PE_parse_boot_argn("bt", &mode, sizeof(mode));
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
	if (!power_on() || !clocks_on()) {
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
			(void)uart_loopback();
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
	hakios_say("bt-uart: identification complete; no firmware downloaded\n");
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

	/* boot-arg bt=-1: read and log only; bt=1: power, UART and HCI identification; bt=0/absent: off. */
	if (!PE_parse_boot_argn("bt", &mode, sizeof(mode)) || (mode != -1 && mode != 1)) {
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
