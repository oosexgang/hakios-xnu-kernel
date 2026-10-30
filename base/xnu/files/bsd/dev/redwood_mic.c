/*
 * The phone's microphones, through the audio DSP. Every address and number below belongs to the
 * POCO X5 Pro (redwood), from Android's own live device tree, its mixer paths (fw-audio/android/)
 * and Xiaomi's kernel (redwood-s-oss, techpack/audio: asoc/codecs/bolero/, asoc/codecs/wcd938x/,
 * soc/swr-mstr-ctrl.c, soc/pinctrl-lpi.c, dsp/q6afe.c, asoc/codecs/audio-ext-clk-up.c). Nothing
 * here happens unless boot-arg mic=N asks for it, with adsp=1 (redwood_adsp.c), which calls
 * redwood_mic_dsp_up() once the DSP's audio services are up.
 *
 * The main microphone under Android (mixer paths "handset-mic" = "amic1", "audio-record"): the
 * analog microphone at ADC1 of the WCD938x codec, whose samples go over SoundWire (the TX master
 * of the LPASS's digital codec, "bolero") into decimator 1 of the TX macro, out of it through the
 * codec DMA interface TX_CDC_DMA_TX_3, the AFE's port, into a stream that is read (ASM). Before any
 * of it: the LPASS's own clocks, which the DSP gives out (Android's audio-ref-clk nodes):
 *   lpass_core_hw_vote   AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, block 3 ("LPASS_HW_MACRO")
 *   lpass_audio_hw_vote  the same, block 4 ("LPASS_HW_DCODEC")
 *   tx_core_clk          AFE clock 0x30c at 19.2 MHz (qcom,codec-lpass-clk-id, -ext-clk-freq)
 *   tx_npl_clk           AFE clock 0x30d at 19.2 MHz
 * Without them the macros' registers are not to be touched at all: an access to a block without
 * its clock may hang the bus.
 *
 *   mic=1    a look: the votes and the two clocks asked for, then -- only if all four were given --
 *            the registers of the TX and VA macros, of the TX SoundWire master, of the LPASS pins
 *            the master uses and of the codec's reset pin read (nothing written to any of them);
 *            then the clocks and the votes given back
 *   mic=2    ... and the codec brought up far enough to answer on SoundWire, nothing of it set:
 *            its supplies as the boot chain leaves them (L18B and BoB are on: the codec answers; nothing
 *            of the PMICs is touched), its reset released (GPIO83 low 20 us, then high, 16 mA:
 *            msm-cdc-pinctrl's two states), the master's pins to their active state (LPASS GPIOs
 *            0, 1, 2, 14: function 1, 2 mA, slew rate 1; the clock without a pull, the data with a
 *            keeper), the TX macro's clocks as tx_macro_tx_va_mclk_enable() starts them (bolero's
 *            "fs-gen-sequence" in the VA macro, FREQ_MCLK, MCLK_CONTROL, FS_CNT_CONTROL, the
 *            SoundWire clock with a reset), the master started as swrm_runtime_resume() and
 *            swrm_master_init() start it (the frame 50 rows x 2 columns, auto-enumeration, its
 *            interrupts' masks), the codec's TX device waited for (ID 0x000d01170223, the tree's
 *            wcd938x-tx-slave) and its chip ID read over SoundWire. Then all of it undone, in reverse:
 *            the codec back in reset, the pins and registers as they were found
 *   mic=3    ... and the main microphone listened to for mic_secs seconds (default 10, at most 600)
 *            the way Android records from it: the codec set up as wcd938x_bind() and
 *            wcd938x_init_reg() set it up (its interrupts as edges, the mic biases' 2750 mV, its
 *            bias and sleep registers); the DSP's side (a stream read through our memory, ASM
 *            session 2; a device path for recording, ADM; the codec DMA port TX_CDC_DMA_TX_3, AFE
 *            port 0xB037, 48 kHz, 16 bits, one channel: decimator 1); the TX macro's decimator 1 fed
 *            from SWR_MIC0 (the mixer controls "TX DEC1 MUX" SWR_MIC, "TX SMIC MUX1" SWR_MIC0, and
 *            tx_macro_enable_dec()); "MIC BIAS1" and ADC1 in "low power" mode, "ADC1 Volume" 6 (Xiaomi's
 *            mixer paths); the SoundWire port with the bus as Android has it for its handset
 *            microphones (see datapath_on()); then the decimator unmuted after 200 ms and its
 *            filter settled after 300 ms, as tx-macro's delayed work does. Each second's level goes
 *            to the log; with rec_sound=N rec_sound_mic=1 the samples go into the recording that
 *            ./xl sound fetches. Every register write is read back (a few registers that read 0
 *            whatever is written, as on Android, are said so); any mismatch or a DSP command
 *            refused stops it all. Tests: mic_input=0 (decimator 1 fed from ZERO: exact silence),
 *            mic_adc=0 (ADC1 left off), mic_test=1 (no mic bias), mic_test=2 (ADC1's input held),
 *            mic_main=1 (the bus and ports of Android's one-microphone tables: a tone, not the room),
 *            mic_tap=1 with haptic=5 (the vibration motor from 2 s to 3 s of listening: the motor's
 *            175.5 Hz in the samples)
 *
 * What is asked for is given back at the end, and before the DSP is shut down (redwood_mic_dsp_down()).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <kern/kalloc.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static LCK_GRP_DECLARE(mic_lock_group, "redwood-mic");
static LCK_MTX_DECLARE(mic_lock, &mic_lock_group);      /* what is asked for, and given back */

static uint32_t level;
static bool running;

static int say(const char *format, ...) __printflike(1, 2);

/*
 * Every line is kept as well, and all of them said again at the run's end (redwood_mic_quiesce()):
 * the log of a run keeps its beginning and its end, and this happens in between.
 */
#define KEPT_LINES              400
static char kept[KEPT_LINES][168];
static unsigned kept_count;

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	strlcpy(kept[kept_count++ % KEPT_LINES], line, sizeof(kept[0]));
	return length;
}

/* A line that says all went as it should: not said again in iOS's later recordings (mic=4), where it is the same each time. */
static bool quiet;
#define note(...)       do { if (!quiet) { say(__VA_ARGS__); } } while (0)

void
redwood_mic_quiesce(void)
{
	if (kept_count == 0) {
		return;
	}
	for (const char *c = "mic: what happened in this run, again:\n"; *c != 0; c++) {
		loader_log_putc(*c);
	}
	for (unsigned i = kept_count > KEPT_LINES ? kept_count - KEPT_LINES : 0; i < kept_count; i++) {
		loader_log_putc('>');
		loader_log_putc(' ');
		for (const char *c = kept[i % KEPT_LINES]; *c != 0; c++) {
			loader_log_putc(*c);
		}
		if (kept[i % KEPT_LINES][strlen(kept[i % KEPT_LINES]) - 1] != '\n') {
			loader_log_putc('\n');
		}
	}
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static bool broken;                     /* a write that did not read back: nothing more is written, all is undone */
static char last_write[120] = "nothing written";

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	value = rd(b->base + offset);
	note("mic: %s %s (%#x) = %#x\n", b->name, name, b->phys + offset, value);
	return value;
}

/*
 * A register written (and read back, unless `check' is 0: some bits clear themselves). A write that
 * does not read back as written (in the bits of `check') stops the bring-up.
 */
static bool
put(struct block *b, const char *name, uint32_t offset, uint32_t value, uint32_t check)
{
	uint32_t back;

	if (broken) {
		return false;
	}
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	snprintf(last_write, sizeof(last_write), "%#x to %s %s (%#x)", value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
	back = rd(b->base + offset);
	if ((back & check) != (value & check)) {
		say("mic: %s %s (%#x) <- %#x, but it reads %#x: STOPPING HERE\n", b->name, name, b->phys + offset, value, back);
		broken = true;
		return false;
	}
	note("mic: %s %s (%#x) <- %#x%s\n", b->name, name, b->phys + offset, value, check ? ", reads back" : "");
	return true;
}

/* only the bits of `mask' changed */
static bool
put_bits(struct block *b, const char *name, uint32_t offset, uint32_t mask, uint32_t value)
{
	uint32_t now;

	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	now = rd(b->base + offset);
	return put(b, name, offset, (now & ~mask) | (value & mask), mask);
}

/* ---- the DSP: votes and clocks ------------------------------------------------------------------- */

#define APR_SVC_AFE                             4
#define APR_BASIC_RSP_RESULT                    0x000110e8u
#define APR_WAIT_MS                             1000
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u
#define AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST       0x000100f4u
#define AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST   0x000100f5u
#define AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST     0x000100f6u
#define LPASS_HW_MACRO_BLOCK                    3
#define LPASS_HW_DCODEC_BLOCK                   4
#define CLOCK_TX_CORE                           0x30c
#define CLOCK_TX_NPL                            0x30d
#define CLOCK_HZ                                19200000
#define CLOCK_COUPLE_NO                         1

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "an error this does not know";
}

static bool
command(const char *what, uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct redwood_apr_answer *answer)
{
	struct redwood_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	if (!redwood_adsp_apr(service, 0, 0, opcode, payload, length, APR_WAIT_MS, answer)) {
		say("mic: %s (opcode %#x): NO ANSWER\n", what, opcode);
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->words[0] == opcode) {
		if (answer->words[1] == 0) {
			note("mic: %s: done\n", what);
		} else {
			say("mic: %s: REFUSED: %s\n", what, dsp_error(answer->words[1]));
		}
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		return true;
	}
	say("mic: %s: an answer not expected: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
	    answer->words[0], answer->words[1]);
	return false;
}

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

static bool
afe_service_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	uint32_t *head = (uint32_t *)packet;
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	head[0] = (uint32_t)sizeof(*p) + size;          /* payload size; address 0 and handle 0: in band */
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

static bool
afe_clock(const char *what, uint32_t id, bool on)
{
	struct afe_clock_set clock = { 1, id, CLOCK_HZ, CLOCK_COUPLE_NO, 0, on };

	return afe_service_param(what, AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static uint32_t vote_handle[5];
static bool voted[5], core_clock, npl_clock;

static bool
vote(uint32_t block, const char *client)
{
	struct {
		uint32_t block;
		char client[8];
	} __attribute__((packed)) request = { block, { 0 } };
	struct redwood_apr_answer answer;
	char what[80];

	strlcpy(request.client, client, sizeof(request.client));
	snprintf(what, sizeof(what), "the LPASS's vote for block %u (\"%s\")", block, client);
	if (!command(what, APR_SVC_AFE, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, &request, sizeof(request),
	    AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, &answer)) {
		return false;
	}
	vote_handle[block] = answer.words[0];
	voted[block] = true;
	say("mic: %s: given, handle %u\n", what, vote_handle[block]);
	return true;
}

static void
devote(uint32_t block)
{
	uint32_t request[2] = { block, vote_handle[block] };
	char what[64];

	if (!voted[block]) {
		return;
	}
	snprintf(what, sizeof(what), "the LPASS's vote for block %u, given back", block);
	(void)command(what, APR_SVC_AFE, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST, request, sizeof(request), 0, NULL);
	voted[block] = false;
}

static bool
clocks_on(void)
{
	if (!vote(LPASS_HW_MACRO_BLOCK, "LPASS_HW_MACRO") || !vote(LPASS_HW_DCODEC_BLOCK, "LPASS_HW_DCODEC")) {
		return false;
	}
	core_clock = afe_clock("the TX macro's core clock (0x30c) at 19.2 MHz, on", CLOCK_TX_CORE, true);
	if (!core_clock) {
		return false;
	}
	npl_clock = afe_clock("the TX macro's NPL clock (0x30d) at 19.2 MHz, on", CLOCK_TX_NPL, true);
	return npl_clock;
}

static void
clocks_off(void)
{
	if (npl_clock) {
		(void)afe_clock("the TX macro's NPL clock, off", CLOCK_TX_NPL, false);
		npl_clock = false;
	}
	if (core_clock) {
		(void)afe_clock("the TX macro's core clock, off", CLOCK_TX_CORE, false);
		core_clock = false;
	}
	devote(LPASS_HW_DCODEC_BLOCK);
	devote(LPASS_HW_MACRO_BLOCK);
}

/* ---- the blocks, read ------------------------------------------------------------------------------ */

static struct block tx_macro = { "the TX macro", 0x03220000, 0x1000 };
static struct block va_macro = { "the VA macro", 0x03370000, 0x1000 };
static struct block tx_swr = { "the TX SoundWire master", 0x03230000, 0x2000 };
static struct block tx_hctl = { "the TX SoundWire master's HCTL", 0x032a9000, 0x1000 };
static struct block lpi = { "the LPASS pins", 0x033c0000, 0x10000 };
static struct block lpi_high = { "the LPASS pins", 0x033ce000, 0x1000 };
static struct block tlmm = { "the pin controller", 0x0f100000, 0x100000 };

static void
look(void)
{
	static const struct { const char *name; uint32_t offset; } tx[] = {
		{ "MCLK_CONTROL", 0x000 }, { "FS_CNT_CONTROL", 0x004 }, { "SWR_CONTROL", 0x008 }, { "TOP_CFG0", 0x080 },
		{ "SWR_CTRL", 0x088 }, { "FREQ_MCLK", 0x090 }, { "SWR_AMIC0_CTL", 0x0d0 }, { "SWR_AMIC1_CTL", 0x0d4 },
	}, va[] = {
		{ "MCLK_CONTROL", 0x000 }, { "FS_CNT_CONTROL", 0x004 }, { "TOP_CFG0", 0x080 },
	}, swr[] = {
		{ "COMP_HW_VERSION", 0x000 }, { "COMP_CFG", 0x004 }, { "COMP_STATUS", 0x014 }, { "COMP_PARAMS", 0x100 },
		{ "COMP_MASTER_ID", 0x104 }, { "INTERRUPT_STATUS", 0x200 }, { "ENUMERATOR_CFG", 0x500 },
		{ "ENUMERATOR_STATUS", 0x504 }, { "MCP_FRAME_CTRL_BANK0", 0x101c }, { "MCP_BUS_CTRL", 0x1044 },
		{ "MCP_CFG", 0x1048 }, { "MCP_STATUS", 0x104c }, { "MCP_SLV_STATUS", 0x1090 },
	};
	static const unsigned pins[] = { 0, 1, 2 };

	for (size_t i = 0; i < sizeof(tx) / sizeof(tx[0]); i++) {
		(void)get(&tx_macro, tx[i].name, tx[i].offset);
	}
	for (size_t i = 0; i < sizeof(va) / sizeof(va[0]); i++) {
		(void)get(&va_macro, va[i].name, va[i].offset);
	}
	for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		char name[48];

		snprintf(name, sizeof(name), "gpio%u config (SoundWire TX)", pins[i]);
		(void)get(&lpi, name, 0x1000 * pins[i]);
		snprintf(name, sizeof(name), "gpio%u in/out", pins[i]);
		(void)get(&lpi, name, 0x1000 * pins[i] + 4);
	}
	(void)get(&lpi_high, "gpio14 config (SoundWire TX data 2)", 0);
	(void)get(&lpi_high, "gpio14 in/out", 4);
	(void)get(&tlmm, "gpio83 config (the codec's reset)", 0x1000 * 83);
	(void)get(&tlmm, "gpio83 in/out", 0x1000 * 83 + 4);
	/* (last: the master is a block of its own, and not yet known; with its version above 1.5.1 the votes are all it needs) */
	for (size_t i = 0; i < sizeof(swr) / sizeof(swr[0]); i++) {
		(void)get(&tx_swr, swr[i].name, swr[i].offset);
	}
	(void)get(&tx_hctl, "HCTL (0x32a90a8)", 0x0a8);
}

/* ---- the codec's reset, the master's pins ------------------------------------------------------------ */

#define TLMM_CFG_83             (0x1000 * 83)
#define TLMM_IO_83              (0x1000 * 83 + 4)
static uint32_t found_reset_cfg, found_reset_io;
static bool reset_taken;

static bool
reset_release(void)
{
	if (tlmm.base == 0) {
		tlmm.base = ml_io_map(tlmm.phys, tlmm.size);
	}
	found_reset_cfg = rd(tlmm.base + TLMM_CFG_83);
	found_reset_io = rd(tlmm.base + TLMM_IO_83);
	reset_taken = true;
	/* the sleep state: gpio, 16 mA, no pull, output low; 20 us; the active state: output high */
	if (!put(&tlmm, "gpio83 out (the codec's reset)", TLMM_IO_83, 0, 2) ||
	    !put(&tlmm, "gpio83 config: gpio, 16 mA, no pull, output", TLMM_CFG_83, 0x3c0, 0x3ff)) {
		return false;
	}
	spin_us(30);
	if (!put(&tlmm, "gpio83 out: high (the codec out of reset)", TLMM_IO_83, 2, 2)) {
		return false;
	}
	spin_us(30);
	return true;
}

static void
reset_restore(void)
{
	if (!reset_taken) {
		return;
	}
	broken = false;         /* (putting back is done whatever went before) */
	(void)put(&tlmm, "gpio83 out: low (the codec in reset)", TLMM_IO_83, 0, 2);
	(void)put(&tlmm, "gpio83 config, as it was found", TLMM_CFG_83, found_reset_cfg, 0x3ff);
	(void)put(&tlmm, "gpio83 out, as it was found", TLMM_IO_83, found_reset_io & 2, 2);
	reset_taken = false;
}

static struct block lpi_slew = { "the LPASS pins' slew rates", 0x0355a000, 0x1000 };
static const struct { unsigned pin, slew_bit; uint32_t pull; const char *name; } swr_pins[] = {
	{ 0, 0, 0, "gpio0 (SoundWire TX clock)" }, { 1, 2, 2, "gpio1 (data 0)" }, { 2, 4, 2, "gpio2 (data 1)" },
	{ 14, 6, 2, "gpio14 (data 2)" },
};
#define SWR_PINS                (sizeof(swr_pins) / sizeof(swr_pins[0]))
static uint32_t found_pin_cfg[SWR_PINS], found_pin_dir[SWR_PINS], found_slew;
static bool pins_taken;

static bool
pins_on(void)
{
	uint32_t slew;

	if (lpi.base == 0) {
		lpi.base = ml_io_map(lpi.phys, lpi.size);
	}
	if (lpi_slew.base == 0) {
		lpi_slew.base = ml_io_map(lpi_slew.phys, lpi_slew.size);
	}
	for (unsigned i = 0; i < SWR_PINS; i++) {
		found_pin_cfg[i] = rd(lpi.base + 0x1000 * swr_pins[i].pin);
		found_pin_dir[i] = rd(lpi.base + 0x1000 * swr_pins[i].pin + 4);
	}
	found_slew = rd(lpi_slew.base);
	pins_taken = true;
	slew = found_slew;
	for (unsigned i = 0; i < SWR_PINS; i++) {
		slew = (slew & ~(3u << swr_pins[i].slew_bit)) | (1u << swr_pins[i].slew_bit);
	}
	if (!put(&lpi_slew, "slew rates: 1 for the four", 0, slew, 0xff)) {
		return false;
	}
	for (unsigned i = 0; i < SWR_PINS; i++) {
		/* [1:0] pull, [5:2] function, [8:6] drive (mA / 2 - 1), [9] output: function 1, 2 mA, not an output */
		uint32_t cfg = (found_pin_cfg[i] & ~0x3ffu) | swr_pins[i].pull | 1u << 2;

		if (!put(&lpi, swr_pins[i].name, 0x1000 * swr_pins[i].pin, cfg, 0x3ff) ||
		    !put(&lpi, "its direction: in", 0x1000 * swr_pins[i].pin + 4, 0, 2)) {
			return false;
		}
	}
	return true;
}

static void
pins_restore(void)
{
	if (!pins_taken) {
		return;
	}
	broken = false;
	for (unsigned i = 0; i < SWR_PINS; i++) {
		(void)put(&lpi, "a pin, as it was found", 0x1000 * swr_pins[i].pin, found_pin_cfg[i], 0x3ff);
		(void)put(&lpi, "its direction, as it was found", 0x1000 * swr_pins[i].pin + 4, found_pin_dir[i], 2);
	}
	(void)put(&lpi_slew, "slew rates, as they were found", 0, found_slew, 0xff);
	pins_taken = false;
}

/* ---- the TX macro's clocks, the SoundWire master ----------------------------------------------------- */

#define TX_MCLK_CONTROL         0x000
#define TX_FS_CNT_CONTROL       0x004
#define TX_SWR_CONTROL          0x008
#define TX_FREQ_MCLK            0x090
#define VA_MCLK_CONTROL         0x000
#define VA_FS_CNT_CONTROL       0x004
#define VA_TOP_CFG0             0x080

#define SWRM_COMP_CFG                   0x0004
#define SWRM_COMP_SW_RESET              0x0008
#define SWRM_COMP_STATUS                0x0014
#define SWRM_INTERRUPT_EN               0x0204
#define SWRM_INTERRUPT_CLEAR            0x0208
#define SWRM_CPU1_INTERRUPT_EN          0x0210
#define SWRM_CMD_FIFO_WR_CMD            0x0300
#define SWRM_CMD_FIFO_RD_CMD            0x0304
#define SWRM_CMD_FIFO_CMD               0x0308
#define SWRM_CMD_FIFO_STATUS            0x030c
#define SWRM_CMD_FIFO_CFG               0x0314
#define SWRM_CMD_FIFO_RD_FIFO           0x0318
#define SWRM_ENUMERATOR_CFG             0x0500
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_1(m) (0x0530 + 0x8 * (m))
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_2(m) (0x0534 + 0x8 * (m))
#define SWRM_MCP_FRAME_CTRL_BANK0       0x101c
#define SWRM_MCP_BUS_CTRL               0x1044
#define SWRM_MCP_CFG                    0x1048
#define SWRM_MCP_SLV_STATUS             0x1090
#define SWRM_INTERRUPT_STATUS_MASK      0x1fdfd
#define SWR_BUS_HZ                      9600000         /* MCLK_FREQ: FREQ_MCLK 1 is 9.6 MHz */
#define SWR_ROWS                        50
#define SWR_COLUMNS                     2
#define SWR_FRAME_SYNC_HZ               4000
#define CODEC_TX_ID                     0x000d01170223ull       /* the tree's wcd938x-tx-slave */
#define SWRS_SCP_INT_STATUS_MASK_1      0x41

static bool tx_clocks_taken, master_taken;
static uint32_t found_hctl;
static uint8_t read_id = 0;

static bool
tx_clocks_on(void)
{
	tx_clocks_taken = true;
	/* bolero_clk_rsc_fs_gen_request(): the tree's qcom,fs-gen-sequence, register, mask, value */
	return put_bits(&va_macro, "MCLK_CONTROL (fs-gen)", VA_MCLK_CONTROL, 0x01, 0x01) &&
	       put_bits(&va_macro, "FS_CNT_CONTROL (fs-gen)", VA_FS_CNT_CONTROL, 0x03, 0x03) &&
	       put_bits(&va_macro, "FS_CNT_CONTROL (fs-gen)", VA_FS_CNT_CONTROL, 0x03, 0x01) &&
	       put_bits(&va_macro, "TOP_CFG0 (fs-gen)", VA_TOP_CFG0, 0x02, 0x02) &&
	       /*
	        * tx_macro_mclk_enable(): "9.6 MHz MCLK" -- written as Android writes it; on the phone it reads
	        * 0 back whether the macro's clock runs or not (Android never reads it)
	        */
	       put(&tx_macro, "FREQ_MCLK: 9.6 MHz (it reads 0 back whatever is written)", TX_FREQ_MCLK, 1, 0) &&
	       put_bits(&tx_macro, "MCLK_CONTROL: on", TX_MCLK_CONTROL, 0x01, 0x01) &&
	       put_bits(&tx_macro, "FS_CNT_CONTROL: on", TX_FS_CNT_CONTROL, 0x01, 0x01) &&
	       /* tx_macro_tx_va_mclk_enable(): the SoundWire clock, with the first reset */
	       put_bits(&tx_macro, "SWR_CONTROL: reset", TX_SWR_CONTROL, 0x02, 0x02) &&
	       put_bits(&tx_macro, "SWR_CONTROL: clock on", TX_SWR_CONTROL, 0x01, 0x01) &&
	       put_bits(&tx_macro, "SWR_CONTROL: reset over", TX_SWR_CONTROL, 0x02, 0x00);
}

static void
tx_clocks_off(void)
{
	if (!tx_clocks_taken) {
		return;
	}
	broken = false;
	(void)put_bits(&tx_macro, "SWR_CONTROL: clock off", TX_SWR_CONTROL, 0x01, 0x00);
	(void)put_bits(&tx_macro, "FS_CNT_CONTROL: off", TX_FS_CNT_CONTROL, 0x01, 0x00);
	(void)put_bits(&tx_macro, "MCLK_CONTROL: off", TX_MCLK_CONTROL, 0x01, 0x00);
	(void)put(&tx_macro, "FREQ_MCLK: as at reset", TX_FREQ_MCLK, 0, 0);
	(void)put_bits(&va_macro, "TOP_CFG0 (fs-gen off)", VA_TOP_CFG0, 0x02, 0x00);
	(void)put_bits(&va_macro, "FS_CNT_CONTROL (fs-gen off)", VA_FS_CNT_CONTROL, 0x03, 0x00);
	(void)put_bits(&va_macro, "MCLK_CONTROL (fs-gen off)", VA_MCLK_CONTROL, 0x01, 0x00);
	tx_clocks_taken = false;
}

static uint32_t
swr_rd(uint32_t offset)
{
	if (tx_swr.base == 0) {
		tx_swr.base = ml_io_map(tx_swr.phys, tx_swr.size);
	}
	return rd(tx_swr.base + offset);
}

static bool
master_on(void)
{
	uint32_t ssp = SWR_BUS_HZ * 2 / (SWR_ROWS * SWR_COLUMNS * SWR_FRAME_SYNC_HZ), cfg;
	bool linked = false;

	if (tx_hctl.base == 0) {
		tx_hctl.base = ml_io_map(tx_hctl.phys, tx_hctl.size);
	}
	found_hctl = rd(tx_hctl.base + 0x0a8);
	master_taken = true;
	/* "SW workaround to gate hw_ctl for SWR version >= 1.6" */
	if (!put(&tx_hctl, "HCTL: bit 1 cleared", 0x0a8, found_hctl & ~2u, 0x3)) {
		return false;
	}
	spin_us(505);
	/* swrm_runtime_resume() */
	if (!put(&tx_swr, "COMP_SW_RESET", SWRM_COMP_SW_RESET, 1, 0) || !put(&tx_swr, "COMP_SW_RESET", SWRM_COMP_SW_RESET, 1, 0) ||
	    !put(&tx_swr, "MCP_BUS_CTRL", SWRM_MCP_BUS_CTRL, 1, 0)) {
		return false;
	}
	/* swrm_master_init() */
	cfg = (swr_rd(SWRM_MCP_CFG) & ~0x3e0000u) | 0x1fu << 17;
	if (!put(&tx_swr, "MCP_FRAME_CTRL_BANK0: 50 rows, 2 columns", SWRM_MCP_FRAME_CTRL_BANK0, 1u << 3 | 0u | (ssp - 1) << 16,
	    0xffffffff) ||
	    !put(&tx_swr, "ENUMERATOR_CFG: auto", SWRM_ENUMERATOR_CFG, 1, 1) ||
	    !put(&tx_swr, "MCP_CFG: 31 pings", SWRM_MCP_CFG, cfg, 0x3e0000) ||
	    !put(&tx_swr, "CMD_FIFO_CFG: 3 retries", SWRM_CMD_FIFO_CFG, 3, 0xff) ||
	    !put(&tx_swr, "MCP_BUS_CTRL", SWRM_MCP_BUS_CTRL, 2, 0) ||
	    !put(&tx_swr, "COMP_CFG: interrupts as pulses", SWRM_COMP_CFG, 2, 0) ||
	    !put(&tx_swr, "INTERRUPT_CLEAR", SWRM_INTERRUPT_CLEAR, 0xffffffff, 0) ||
	    !put(&tx_swr, "INTERRUPT_EN", SWRM_INTERRUPT_EN, SWRM_INTERRUPT_STATUS_MASK, 0) ||
	    !put(&tx_swr, "CPU1_INTERRUPT_EN", SWRM_CPU1_INTERRUPT_EN, SWRM_INTERRUPT_STATUS_MASK, 0) ||
	    !put(&tx_swr, "COMP_CFG: on", SWRM_COMP_CFG, 3, 0)) {
		return false;
	}
	for (int i = 0; i < 100 && !linked; i++) {
		linked = swr_rd(SWRM_COMP_STATUS) & 1;
		if (!linked) {
			spin_us(510);
		}
	}
	say("mic: the SoundWire link %s (COMP_STATUS %#x)\n", linked ? "IS UP" : "DID NOT COME UP", swr_rd(SWRM_COMP_STATUS));
	if (!linked) {
		return false;
	}
	return put(&tx_swr, "CMD_FIFO_CFG: | 0x80000000", SWRM_CMD_FIFO_CFG, swr_rd(SWRM_CMD_FIFO_CFG) | 0x80000000u, 0);
}

static void
master_off(void)
{
	if (!master_taken) {
		return;
	}
	broken = false;
	(void)put(&tx_swr, "COMP_CFG: off", SWRM_COMP_CFG, 0, 0);
	(void)put(&tx_swr, "INTERRUPT_EN: none", SWRM_INTERRUPT_EN, 0, 0);
	(void)put(&tx_swr, "CPU1_INTERRUPT_EN: none", SWRM_CPU1_INTERRUPT_EN, 0, 0);
	(void)put(&tx_swr, "COMP_SW_RESET", SWRM_COMP_SW_RESET, 1, 0);
	(void)put(&tx_hctl, "HCTL, as it was found", 0x0a8, found_hctl, 0x3);
	master_taken = false;
}

/* A command to a device on the bus (SWR_REG_VAL_PACK): a register written. */
static bool
swr_write(uint8_t device, uint16_t reg, uint8_t value, uint8_t id)
{
	uint32_t command = reg | (uint32_t)id << 16 | (uint32_t)device << 20 | (uint32_t)value << 24;

	for (int i = 0; i < 20 && ((swr_rd(SWRM_CMD_FIFO_STATUS) >> 8) & 0x1f) >= 16; i++) {
		spin_us(500);
	}
	snprintf(last_write, sizeof(last_write), "SoundWire: device %u register %#x <- %#x", device, reg, value);
	wr(tx_swr.base + SWRM_CMD_FIFO_WR_CMD, command);
	spin_us(155);
	return true;
}

/* ... and one read: false if the answer does not come (or is not this command's) */
static bool
swr_read(uint8_t device, uint16_t reg, uint8_t *value)
{
	uint32_t answer = 0;

	read_id = read_id < 14 ? read_id + 1 : 0;
	wr(tx_swr.base + SWRM_CMD_FIFO_RD_CMD, reg | (uint32_t)read_id << 16 | (uint32_t)device << 20 | 1u << 24);
	spin_us(255);
	for (int tries = 0; tries < 4; tries++) {
		for (int i = 0; i < 20 && ((swr_rd(SWRM_CMD_FIFO_STATUS) >> 16) & 0x1f) == 0; i++) {
			spin_us(500);
		}
		answer = swr_rd(SWRM_CMD_FIFO_RD_FIFO);
		if (((answer >> 8) & 0xf) == read_id) {
			*value = (uint8_t)answer;
			return true;
		}
		spin_us(505);
	}
	say("mic: SoundWire: device %u register %#x: no answer (fifo %#x, last %#x)\n", device, reg, swr_rd(SWRM_CMD_FIFO_STATUS),
	    answer);
	return false;
}

static uint8_t codec_device;

/* the codec's TX device, enumerated: its device number, 0 if it did not show up */
static uint8_t
codec_wait(void)
{
	for (int waited = 0; waited < 200; waited++) {
		for (uint8_t d = 1; d <= 11; d++) {
			uint64_t id = (uint64_t)swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32 | swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));
			uint32_t status = (swr_rd(SWRM_MCP_SLV_STATUS) >> (2 * d)) & 3;

			if ((id & 0xffffffffffffull) == CODEC_TX_ID && (status == 1 || status == 2)) {
				say("mic: the codec's TX device is device %u on the bus (ID %#llx, status %u) after %d ms\n", d,
				    id & 0xffffffffffffull, status, waited);
				return d;
			}
		}
		sleep_ms(1);
	}
	say("mic: the codec's TX device did not show up; the master has seen:\n");
	for (uint8_t d = 1; d <= 11; d++) {
		uint64_t id = (uint64_t)swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32 | swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));

		if (id != 0) {
			say("mic:   device %u: ID %#llx\n", d, id);
		}
	}
	say("mic:   slave status %#x\n", swr_rd(SWRM_MCP_SLV_STATUS));
	return 0;
}

static void
codec_look(void)
{
	static const struct { const char *name; uint16_t reg; } regs[] = {
		{ "DIGITAL_CHIP_ID0", 0x3401 }, { "DIGITAL_CHIP_ID1", 0x3402 }, { "DIGITAL_CHIP_ID2", 0x3403 },
		{ "DIGITAL_CHIP_ID3", 0x3404 }, { "SCP DEVICE_ID_0", 0x0050 }, { "SCP DEVICE_ID_1", 0x0051 },
	};

	for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t value;

		if (swr_read(codec_device, regs[i].reg, &value)) {
			say("mic: the codec's %s (%#x) = %#x\n", regs[i].name, regs[i].reg, value);
		}
	}
}

/* ---- mic=3: the codec set up as Android sets it up for the main microphone ----------------------------- */

#define WCD_ANA_BIAS                    0x3001
#define WCD_ANA_TX_CH1                  0x300e
#define WCD_ANA_TX_CH2                  0x300f
#define WCD_ANA_MICB1                   0x3022
#define WCD_ANA_MICB2                   0x3023
#define WCD_ANA_MICB3                   0x3025
#define WCD_ANA_MICB4                   0x3026
#define WCD_BIAS_VBG_FINE_ADJ           0x3029
#define WCD_MICB1_TEST_CTL_1            0x306b
#define WCD_MICB1_TEST_CTL_2            0x306c
#define WCD_MICB2_TEST_CTL_1            0x306e
#define WCD_MICB2_TEST_CTL_2            0x306f
#define WCD_MICB3_TEST_CTL_1            0x3071
#define WCD_MICB3_TEST_CTL_2            0x3072
#define WCD_MICB4_TEST_CTL_1            0x3074
#define WCD_MICB4_TEST_CTL_2            0x3075
#define WCD_TX_3_4_TEST_BLK_EN2         0x308f
#define WCD_HPH_REFBUFF_UHQA_CTL        0x30dd
#define WCD_HPH_SURGE_HPHLR_SURGE_EN    0x30e2
#define WCD_SLEEP_CTL                   0x3103
#define WCD_LDORXTX_CONFIG              0x312a
#define WCD_HPH_NEW_INT_RDAC_GAIN_CTL   0x3132
#define WCD_HPH_NEW_INT_RDAC_HD2_CTL_L  0x3140
#define WCD_HPH_NEW_INT_RDAC_HD2_CTL_R  0x3141
#define WCD_TXFE_ICTRL_STG2MAIN_ULP     0x31df
#define WCD_TXFE_ICTRL_STG2CASC_ULP     0x31e1
#define WCD_SWR_TX_CLK_RATE             0x3405
#define WCD_CDC_ANA_CLK_CTL             0x3408
#define WCD_CDC_DIG_CLK_CTL             0x3409
#define WCD_CDC_TX_ANA_MODE_0_1         0x3410
#define WCD_CDC_ANA_TX_CLK_CTL          0x3417
#define WCD_CDC_REQ_CTL                 0x3457
#define WCD_CDC_DMIC_CTL                0x345b
#define WCD_INTR_LEVEL_0                0x3474
#define WCD_EFUSE_REG_30                0x34ce
#define WCD_TX_REQ_FB_CTL_0             0x34d0
#define SWRS_SCP_CONTROL                0x0044
#define SWRS_SCP_FRAME_CTRL_BANK(m)     (0x0060 + 0x10 * (m))
#define SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(m) (0x00e0 + 0x10 * (m))
#define SWRS_DP_CHANNEL_ENABLE_BANK(n, m) (0x120 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_SAMPLE_CONTROL_1_BANK(n, m) (0x122 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_SAMPLE_CONTROL_2_BANK(n, m) (0x123 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_OFFSET_CONTROL_1_BANK(n, m) (0x124 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_OFFSET_CONTROL_2_BANK(n, m) (0x125 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_LANE_CONTROL_BANK(n, m) (0x128 + 0x100 * (n) + 0x10 * (m))
#define SWRM_MCP_STATUS                 0x104c
#define SWRM_MCP_FRAME_CTRL_BANK(m)     (0x101c + 0x40 * (m))
#define SWRM_DP_PORT_CTRL_BANK(n, m)    (0x1024 + 0x100 * (n) + 0x40 * (m))
#define SWRM_DP_PORT_CTRL_2_BANK(n, m)  (0x1028 + 0x100 * (n) + 0x40 * (m))
#define SWRM_DP_PORT_HCTRL_BANK(n, m)   (0x1034 + 0x100 * (n) + 0x40 * (m))
#define SWRM_DP_SAMPLECTRL2_BANK(n, m)  (0x103c + 0x100 * (n) + 0x40 * (m))

static uint8_t write_id;

/* a register of the codec's (device codec_device on the bus) written and, unless `check' is 0, read back */
static bool
codec_put(uint16_t reg, uint8_t value, uint8_t check)
{
	uint8_t back = 0;

	if (broken) {
		return false;
	}
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(codec_device, reg, value, write_id);
	if (check == 0) {
		note("mic: the codec's %#06x <- %#04x\n", reg, value);
		return true;
	}
	if (!swr_read(codec_device, reg, &back) || (back & check) != (value & check)) {
		say("mic: the codec's %#06x <- %#04x, but it reads %#04x: STOPPING HERE\n", reg, value, back);
		broken = true;
		return false;
	}
	note("mic: the codec's %#06x <- %#04x, reads back\n", reg, value);
	return true;
}

static bool
codec_bits(uint16_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0;

	if (broken || !swr_read(codec_device, reg, &now)) {
		broken = true;
		return false;
	}
	return (now & mask) == (value & mask) || codec_put(reg, (uint8_t)((now & ~mask) | (value & mask)), mask);
}

/* a command to all devices on the bus (device 15, command id 15: Android's broadcasts) */
static void
broadcast(uint16_t reg, uint8_t value)
{
	swr_write(0xf, reg, value, 0xf);
	note("mic: SoundWire, to all: %#06x <- %#04x\n", reg, value);
	sleep_ms(10);           /* ("10 ms for the broadcast to complete", swrm_cmd_fifo_wr_cmd()) */
}

/* to all devices, with an ordinary command id (swrm_cmd_fifo_wr_cmd(..., 0xF, 0x00, ...)) */
static void
to_all(uint16_t reg, uint8_t value)
{
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(0xf, reg, value, write_id);
	note("mic: SoundWire, to all: %#06x <- %#04x\n", reg, value);
}

/* wcd938x_bind(), wcd938x_set_micbias_data(), wcd938x_init_reg() */
static bool
codec_init(void)
{
	uint8_t efuse = 0;
	/* the tree's qcom,cdc-micbias1..4-mv: 2750 mV, (2750 - 1000) / 50 */
	const uint8_t vout = (2750 - 1000) / 50;

	if (!codec_put(WCD_INTR_LEVEL_0, 0, 0xff) || !codec_put(WCD_INTR_LEVEL_0 + 1, 0, 0xff) ||
	    !codec_put(WCD_INTR_LEVEL_0 + 2, 0, 0xff) ||
	    !codec_bits(WCD_ANA_MICB1, 0x3f, vout) || !codec_bits(WCD_ANA_MICB2, 0x3f, vout) ||
	    !codec_bits(WCD_ANA_MICB3, 0x3f, vout) || !codec_bits(WCD_ANA_MICB4, 0x3f, vout) ||
	    !codec_bits(WCD_SLEEP_CTL, 0x0e, 0x0e) || !codec_bits(WCD_SLEEP_CTL, 0x80, 0x80)) {
		return false;
	}
	sleep_ms(1);
	if (!codec_bits(WCD_SLEEP_CTL, 0x40, 0x40)) {
		return false;
	}
	sleep_ms(1);
	if (!codec_bits(WCD_LDORXTX_CONFIG, 0x10, 0x00) || !codec_bits(WCD_BIAS_VBG_FINE_ADJ, 0xf0, 0x80) ||
	    !codec_bits(WCD_ANA_BIAS, 0x80, 0x80) || !codec_bits(WCD_ANA_BIAS, 0x40, 0x40)) {
		return false;
	}
	sleep_ms(10);
	if (!codec_bits(WCD_ANA_BIAS, 0x40, 0x00) || !codec_bits(WCD_HPH_NEW_INT_RDAC_GAIN_CTL, 0xf0, 0x00) ||
	    !codec_bits(WCD_HPH_NEW_INT_RDAC_HD2_CTL_L, 0x1f, 0x15) || !codec_bits(WCD_HPH_NEW_INT_RDAC_HD2_CTL_R, 0x1f, 0x15) ||
	    !codec_bits(WCD_HPH_REFBUFF_UHQA_CTL, 0xc0, 0x80) || !codec_bits(WCD_CDC_DMIC_CTL, 0x02, 0x02) ||
	    !codec_bits(WCD_TXFE_ICTRL_STG2CASC_ULP, 0xff, 0x14) || !codec_bits(WCD_TXFE_ICTRL_STG2MAIN_ULP, 0x1f, 0x08) ||
	    !codec_bits(WCD_TX_REQ_FB_CTL_0, 0xff, 0x55) || !codec_bits(WCD_TX_REQ_FB_CTL_0 + 1, 0xff, 0x44) ||
	    !codec_bits(WCD_TX_REQ_FB_CTL_0 + 2, 0xff, 0x11) || !codec_bits(WCD_TX_REQ_FB_CTL_0 + 3, 0xff, 0x00) ||
	    !codec_bits(WCD_TX_REQ_FB_CTL_0 + 4, 0xff, 0x00) ||
	    !codec_bits(WCD_MICB1_TEST_CTL_1, 0xe0, 0xe0) || !codec_bits(WCD_MICB2_TEST_CTL_1, 0xe0, 0xe0) ||
	    !codec_bits(WCD_MICB3_TEST_CTL_1, 0xe0, 0xe0) || !codec_bits(WCD_MICB4_TEST_CTL_1, 0xe0, 0xe0) ||
	    !codec_bits(WCD_TX_3_4_TEST_BLK_EN2, 0x01, 0x00) || !swr_read(codec_device, WCD_EFUSE_REG_30, &efuse) ||
	    !codec_bits(WCD_SLEEP_CTL, 0x0e, (uint8_t)((efuse & 0x07) << 1)) ||
	    !codec_bits(WCD_HPH_SURGE_HPHLR_SURGE_EN, 0xc0, 0x00)) {
		return false;
	}
	say("mic: the codec is set up as wcd938x_init_reg() sets it up (efuse 30: %#x)\n", efuse);
	return true;
}

/*
 * The bus and ADC1's data port as Android has them while the phone records from its handset
 * microphones (the HAL's "handset-mic" = "handset-dmic-endfire": ADC1, the main microphone, and
 * ADC4, the top one, both in "low power" mode). With two ADCs on, wcd938x_codec_enable_adc() asks
 * for twice an ADC's 4.8 MHz, 9.6 MHz, and swr-mstr-ctrl adds its ports' rates up to the same: the
 * bus at 9.6 MHz (MCLK / 1), the frame 50 rows x 16 columns, the codec's TX clock rate "9.6 MHz".
 * The ports then come from the 9.6 MHz tables: the codec's (the tree's qcom,swr-phy-dev-addr makes
 * swrm_get_device_frame_shape() use swrdev_frame_params_9p6MHz: tx_wcd_9p6MHz TX1 { si 3, offsets
 * 0 0, lane 1 }) and the master's (sm_port_map_shima, usecase 0: tx_frame_params_shima TX1 { si 7,
 * offsets 0 0, lane 1 }). ADC1 alone is all that is on here; its port is the codec's port 0 (its
 * DP1), channel 1, to the master's port 1, channel 1. Written into the bank not in use, then the
 * banks switched, then the other bank.
 *
 * Android's tables for ADC1 alone (the "main-mic" path: a 4.8 MHz bus, si 7 on both sides) carry
 * half of what ADC1 makes in "low power" mode: what comes out of the decimator is then a loud,
 * clipped tone at about the bus clock / 560, whatever the microphone hears (mic_main=1 shows it).
 */
static uint8_t bank_now;
static uint32_t adc_on = 1;             /* boot-arg mic_adc=0 (a test): ADC1 and its mic bias left off, the port alone */
static uint32_t main_timing;            /* boot-arg mic_main=1 (a test): the bus and ports of Android's one-microphone tables */
static uint32_t tap;                    /* boot-arg mic_tap=1 (a test): the vibration motor (haptic=5) runs from 2 s to 3 s of listening */
static uint32_t test_bits;              /* boot-arg mic_test=N (tests): 1 no mic bias, 2 the ADC's input held (HPF init) */

static bool
port_config(uint8_t bank, bool first)
{
	uint8_t codec_interval = main_timing ? 7 : 3, master_interval = 7;

	/* the codec's side (swrm_apply_port_config() first: the devices' clock halved in that bank) */
	if (first) {
		to_all(SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(bank), 0x01);
	}
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(codec_device, SWRS_DP_CHANNEL_ENABLE_BANK(0, bank), 0x01, write_id);
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(codec_device, SWRS_DP_SAMPLE_CONTROL_1_BANK(0, bank), codec_interval, write_id);
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(codec_device, SWRS_DP_SAMPLE_CONTROL_2_BANK(0, bank), 0, write_id);
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(codec_device, SWRS_DP_OFFSET_CONTROL_1_BANK(0, bank), 0, write_id);
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(codec_device, SWRS_DP_OFFSET_CONTROL_2_BANK(0, bank), 0, write_id);
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(codec_device, SWRS_DP_LANE_CONTROL_BANK(0, bank), 1, write_id);
	note("mic: the codec's data port 1, bank %u: channel 1 on, sample interval %u, offsets 0 0, lane 1\n", bank, codec_interval);
	/* the master's port 1 */
	return put(&tx_swr, "DP_PORT_CTRL (port 1): channel 1, offsets 0 0, interval 7", SWRM_DP_PORT_CTRL_BANK(1, bank),
	           0x01u << 24 | 0u << 16 | 0u << 8 | master_interval, 0xffffffff) &&
	       put(&tx_swr, "DP_SAMPLECTRL2 (port 1)", SWRM_DP_SAMPLECTRL2_BANK(1, bank), 0, 0xff) &&
	       put(&tx_swr, "DP_PORT_CTRL_2 (port 1): lane 1", SWRM_DP_PORT_CTRL_2_BANK(1, bank), 1, 0xff) &&
	       /* (it reads 0 back on the phone whatever is written; Android writes it and never reads it) */
	       put(&tx_swr, "DP_PORT_HCTRL (port 1): hstop 15, hstart 0", SWRM_DP_PORT_HCTRL_BANK(1, bank), 0xf0, 0);
}

static bool
datapath_on(void)
{
	/* the codec's TX clock rate: "9.6 MHz" (0) in both banks; "4.8 MHz" (1) for mic_main=1 */
	uint8_t current = 0, bank, rate = main_timing ? 1 : 0;
	uint32_t frame;

	/* wcd938x_codec_enable_adc(): the codec's TX clock rate in the bank about to be used */
	if (!swr_read(codec_device, SWRS_SCP_CONTROL, &current)) {
		return false;
	}
	bank = (current & 0x40) ? 0 : 1;
	if (!codec_bits(WCD_SWR_TX_CLK_RATE, bank ? 0xf0 : 0x0f, (uint8_t)(bank ? rate << 4 : rate))) {
		return false;
	}
	/* swrm_slvdev_datapath_control() */
	bank = (swr_rd(SWRM_MCP_STATUS) & 1) ? 0 : 1;
	if (!put(&tx_swr, "CPU1_INTERRUPT_EN", SWRM_CPU1_INTERRUPT_EN, SWRM_INTERRUPT_STATUS_MASK, 0) || !port_config(bank, true)) {
		return false;
	}
	/*
	 * 50 rows, 16 columns; the bus at 9.6 MHz: MCLK / 1, "ssp period" 9.6 MHz x 2 / (50 x 16 x 4000) = 6
	 * (mic_main=1: 4.8 MHz, MCLK / 2, 3)
	 */
	frame = (swr_rd(SWRM_MCP_FRAME_CTRL_BANK(bank)) & ~0xff07ffu) | 1u << 3 | 7u | (main_timing ? 1u : 0u) << 8 |
	    (main_timing ? 3u - 1 : 6u - 1) << 16;
	if (!put(&tx_swr, main_timing ? "MCP_FRAME_CTRL (the bank to come): 50 x 16, MCLK / 2" :
	    "MCP_FRAME_CTRL (the bank to come): 50 x 16, MCLK / 1", SWRM_MCP_FRAME_CTRL_BANK(bank), frame, 0xff07ff)) {
		return false;
	}
	broadcast(SWRS_SCP_FRAME_CTRL_BANK(bank), 1u << 3 | 7u);       /* (enable_bank_switch(): the switch itself) */
	bank_now = (swr_rd(SWRM_MCP_STATUS) & 1);
	say("mic: the banks switched: the master is in bank %u now (asked for %u)\n", bank_now, bank);
	if (bank_now != bank || !port_config(bank ? 0 : 1, false)) {
		return false;
	}
	/* and the codec's rate in the other bank */
	if (!codec_bits(WCD_SWR_TX_CLK_RATE, bank ? 0x0f : 0xf0, (uint8_t)(bank ? rate : rate << 4))) {
		return false;
	}
	/* (what the codec's port says it is set to, both banks; its interrupts) */
	for (uint8_t b = 0; b < 2; b++) {
		uint8_t v[6] = { 0 };

		(void)swr_read(codec_device, SWRS_DP_CHANNEL_ENABLE_BANK(0, b), &v[0]);
		(void)swr_read(codec_device, SWRS_DP_SAMPLE_CONTROL_1_BANK(0, b), &v[1]);
		(void)swr_read(codec_device, SWRS_DP_OFFSET_CONTROL_1_BANK(0, b), &v[2]);
		(void)swr_read(codec_device, SWRS_DP_LANE_CONTROL_BANK(0, b), &v[3]);
		(void)swr_read(codec_device, SWRS_SCP_FRAME_CTRL_BANK(b), &v[4]);
		(void)swr_read(codec_device, SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(b), &v[5]);
		note("mic: the codec's DP1, bank %u: channels %#x, interval %u, offset %u, lane %u; frame %#x, div2 %#x\n", b, v[0], v[1],
		    v[2], v[3], v[4], v[5]);
	}
	{
		uint8_t status = 0, control = 0, dp_int = 0, dp_ctl = 0;

		(void)swr_read(codec_device, 0x0040, &status);
		(void)swr_read(codec_device, SWRS_SCP_CONTROL, &control);
		(void)swr_read(codec_device, 0x0100, &dp_int);
		(void)swr_read(codec_device, 0x0102, &dp_ctl);
		note("mic: the codec's SCP int status %#x, control/status %#x; DP1 int status %#x, port control %#x; the master's DP1 "
		    "interrupt status %#x\n", status, control, dp_int, dp_ctl, swr_rd(0x1100));
	}
	return true;
}

/* "MIC BIAS1" (wcd938x_micbias_control(MICB_ENABLE)), then ADC1's request (wcd938x_enable_req()) in "low power" mode */
static bool
adc1_on(void)
{
	return codec_bits(WCD_CDC_DIG_CLK_CTL, 0xf0, 0xf0) && codec_bits(WCD_CDC_ANA_CLK_CTL, 0x10, 0x10) &&
	       codec_bits(WCD_CDC_ANA_TX_CLK_CTL, 0x01, 0x01) && codec_bits(WCD_MICB1_TEST_CTL_2, 0x01, 0x01) &&
	       codec_bits(WCD_MICB2_TEST_CTL_2, 0x01, 0x01) && codec_bits(WCD_MICB3_TEST_CTL_2, 0x01, 0x01) &&
	       codec_bits(WCD_MICB4_TEST_CTL_2, 0x01, 0x01) && codec_bits(WCD_ANA_MICB1, 0xc0, (test_bits & 1) ? 0x00 : 0x40) &&
	       /* "ADC1 Volume" 6 (Xiaomi's mixer paths) */
	       codec_bits(WCD_ANA_TX_CH1, 0x1f, 6) &&
	       codec_bits(WCD_CDC_ANA_CLK_CTL, 0x08, 0x08) && codec_bits(WCD_CDC_ANA_CLK_CTL, 0x10, 0x10) &&
	       codec_bits(WCD_CDC_REQ_CTL, 0x02, 0x02) && codec_bits(WCD_CDC_REQ_CTL, 0x01, 0x00) &&
	       codec_bits(WCD_ANA_TX_CH2, 0x40, 0x40) && codec_bits(WCD_CDC_TX_ANA_MODE_0_1, 0x0f, 0x05) &&
	       codec_bits(WCD_CDC_DIG_CLK_CTL, 0x10, 0x10) && codec_bits(WCD_ANA_TX_CH2, 0x40, (test_bits & 2) ? 0x40 : 0x00);
}

/* ---- the TX macro's decimator 1 ------------------------------------------------------------------------- */

#define TX_TOP_CSR_SWR_CTRL             0x088
#define TX_SWR_MIC0_CTL                 0x0d0
#define TX_SWR_MIC1_CTL                 0x0d4
#define TX_SWR_MIC2_CTL                 0x0c0
#define TX_ADC_MUX1_CFG0                0x108
#define TX_ADC_MUX1_CFG1                0x10c
#define TX1_PATH_CTL                    0x480
#define TX1_PATH_CFG0                   0x484
#define TX1_VOL_CTL                     0x48c
#define TX1_PATH_SEC2                   0x498

#define VA_CORE_ID_0                    0x0c0
#define VA_CORE_ID_1                    0x0c4
#define VA_SWR_MIC_CTL0                 0x0d0
#define VA_SWR_MIC_CTL1                 0x0d4
#define VA_SWR_MIC_CTL2                 0x0d8

static uint32_t found_dec[10], found_va_mic[3];
static uint32_t smic_input = 1;         /* boot-arg mic_input=N: TX SMIC MUX1's input (1 SWR_MIC0, Android's; 0 ZERO, a test) */
static bool dec_taken, va_mic_taken;
static unsigned bolero_version;         /* 20 or 21: the digital codec's own core ID (bolero_soc_codec_probe()) */
static uint8_t hpf_cut_off;             /* the decimator's high-pass filter as it was found */

static const uint32_t dec_regs[] = {
	TX_TOP_CSR_SWR_CTRL, TX_SWR_MIC0_CTL, TX_SWR_MIC1_CTL, TX_SWR_MIC2_CTL, TX_SWR_MIC2_CTL + 4, TX_SWR_MIC2_CTL + 8,
	TX_SWR_MIC2_CTL + 12, TX_ADC_MUX1_CFG0, TX_ADC_MUX1_CFG1, TX1_PATH_CTL,
};

/*
 * Which bolero this is: its core ID, as bolero_soc_codec_probe() reads it (01 0F: 2.0, 02 0E: 2.1),
 * whatever the tree says ("qcom,bolero-version" 4). 2.1 has the SoundWire mics' clock dividers in
 * the VA macro, which va_macro_init() sets (SWR_MIC_CTL0..2: 0xEE, 0xCC); 2.0 in the TX macro,
 * which tx_macro_enable_dec() sets.
 */
static bool
bolero_look(void)
{
	uint32_t id0 = get(&va_macro, "CORE_ID_0", VA_CORE_ID_0) & 0xff, id1 = get(&va_macro, "CORE_ID_1", VA_CORE_ID_1) & 0xff;

	bolero_version = id0 == 0x01 && id1 == 0x0f ? 20 : id0 == 0x02 && id1 == 0x0e ? 21 : 0;
	say("mic: the digital codec is bolero %s\n", bolero_version == 20 ? "2.0" : bolero_version == 21 ? "2.1" : "OF AN UNKNOWN VERSION");
	if (bolero_version != 21) {
		return bolero_version == 20;
	}
	for (unsigned i = 0; i < 3; i++) {
		found_va_mic[i] = get(&va_macro, "SWR_MIC_CTL (as found)", VA_SWR_MIC_CTL0 + 4 * i);
	}
	va_mic_taken = true;
	return put_bits(&va_macro, "SWR_MIC_CTL0 (va_macro_init())", VA_SWR_MIC_CTL0, 0xee, 0xcc) &&
	       put_bits(&va_macro, "SWR_MIC_CTL1", VA_SWR_MIC_CTL1, 0xee, 0xcc) &&
	       put_bits(&va_macro, "SWR_MIC_CTL2", VA_SWR_MIC_CTL2, 0xee, 0xcc);
}

/* the mixer controls' writes ("TX SMIC MUX1" SWR_MIC0, "TX DEC1 MUX" SWR_MIC), hw_params' rate, and the decimator's PMU */
static bool
dec1_on(void)
{
	uint32_t cfg0;

	for (size_t i = 0; i < sizeof(dec_regs) / sizeof(dec_regs[0]); i++) {
		found_dec[i] = get(&tx_macro, "(as found)", dec_regs[i]);
	}
	dec_taken = true;
	if (!put_bits(&tx_macro, "TX1_TX_PATH_CFG0: an analog mic", TX1_PATH_CFG0, 0x80, 0x00) ||
	    !put_bits(&tx_macro, smic_input == 1 ? "ADC_MUX1_CFG0: SWR_MIC0" : "ADC_MUX1_CFG0: ANOTHER INPUT (mic_input)", TX_ADC_MUX1_CFG0,
	    0x0f, smic_input & 0x0f) ||
	    !put_bits(&tx_macro, "ADC_MUX1_CFG1: SWR_MIC", TX_ADC_MUX1_CFG1, 0x03, 0x01) ||
	    !put_bits(&tx_macro, "TX1_TX_PATH_CTL: 48 kHz", TX1_PATH_CTL, 0x0f, 0x04) ||
	    /* PRE_PMU: the decimator's mode (0), its PGA muted */
	    !put_bits(&tx_macro, "TX1_TX_PATH_CFG0: mode 0", TX1_PATH_CFG0, 0x06, 0x00) ||
	    !put_bits(&tx_macro, "TX1_TX_PATH_CTL: muted", TX1_PATH_CTL, 0x10, 0x10) ||
	    /* POST_PMU: on; the high-pass filter at 150 Hz while it settles */
	    !put_bits(&tx_macro, "TX1_TX_PATH_CTL: on", TX1_PATH_CTL, 0x20, 0x20)) {
		return false;
	}
	cfg0 = get(&tx_macro, "TX1_TX_PATH_CFG0", TX1_PATH_CFG0);
	hpf_cut_off = (cfg0 & 0x60) >> 5;
	if (hpf_cut_off != 2 && !put_bits(&tx_macro, "TX1_TX_PATH_CFG0: high-pass 150 Hz", TX1_PATH_CFG0, 0x60, 2u << 5)) {
		return false;
	}
	if (hpf_cut_off != 2) {
		if (!put_bits(&tx_macro, "TX1_TX_PATH_SEC2: filter gate", TX1_PATH_SEC2, 0x03, 0x02)) {
			return false;
		}
		sleep_ms(6);
		if (!put_bits(&tx_macro, "TX1_TX_PATH_SEC2", TX1_PATH_SEC2, 0x02, 0x00)) {
			return false;
		}
	}
	/* the gain, written again after the decimator is on ("TX_DEC1 Volume" 84: 0 dB) */
	if (!put(&tx_macro, "TX1_TX_VOL_CTL: 0 dB", TX1_VOL_CTL, get(&tx_macro, "TX1_TX_VOL_CTL", TX1_VOL_CTL), 0xff)) {
		return false;
	}
	if (bolero_version != 20) {
		return true;
	}
	/* bolero 2.0 only: the SoundWire mics' clocks */
	return put_bits(&tx_macro, "TOP_CSR_SWR_CTRL", TX_TOP_CSR_SWR_CTRL, 0x01, 0x01) &&
	       put_bits(&tx_macro, "SWR_MIC0_CTL", TX_SWR_MIC0_CTL, 0x0e, 0x0c) &&
	       put_bits(&tx_macro, "SWR_MIC1_CTL", TX_SWR_MIC1_CTL, 0x0e, 0x0c) &&
	       put_bits(&tx_macro, "SWR_MIC2_CTL", TX_SWR_MIC2_CTL, 0x0e, 0x00) &&
	       put_bits(&tx_macro, "SWR_MIC3_CTL", TX_SWR_MIC2_CTL + 4, 0x0e, 0x00) &&
	       put_bits(&tx_macro, "SWR_MIC4_CTL", TX_SWR_MIC2_CTL + 8, 0x0e, 0x00) &&
	       put_bits(&tx_macro, "SWR_MIC5_CTL", TX_SWR_MIC2_CTL + 12, 0x0e, 0x00);
}

/* after TX_MACRO_AMIC_UNMUTE_DELAY_MS: the PGA unmuted */
static bool
dec1_unmute(void)
{
	return put_bits(&tx_macro, "TX1_TX_PATH_CTL: unmuted", TX1_PATH_CTL, 0x10, 0x00);
}

/* after TX_MACRO_AMIC_HPF_DELAY_MS: the codec's hold on ADC1 let go, the filter back to where it was */
static bool
dec1_settled(void)
{
	return codec_bits(WCD_ANA_TX_CH2, 0x40, (test_bits & 2) ? 0x40 : 0x00) &&
	       put_bits(&tx_macro, "TX1_TX_PATH_CFG0: high-pass as it was", TX1_PATH_CFG0, 0x60, (uint32_t)hpf_cut_off << 5) &&
	       put_bits(&tx_macro, "TX1_TX_PATH_SEC2: filter gate", TX1_PATH_SEC2, 0x03, 0x02) &&
	       (spin_us(21), put_bits(&tx_macro, "TX1_TX_PATH_SEC2", TX1_PATH_SEC2, 0x03, 0x01));
}

static void
dec1_off(void)
{
	if (!dec_taken) {
		return;
	}
	broken = false;
	(void)put_bits(&tx_macro, "TX1_TX_PATH_CTL: muted", TX1_PATH_CTL, 0x10, 0x10);
	(void)put_bits(&tx_macro, "TX1_TX_PATH_CTL: off", TX1_PATH_CTL, 0x20, 0x00);
	for (size_t i = 0; i < sizeof(dec_regs) / sizeof(dec_regs[0]); i++) {
		(void)put(&tx_macro, "(put back as found)", dec_regs[i], found_dec[i], 0);
	}
	dec_taken = false;
}

static void
va_mic_off(void)
{
	if (!va_mic_taken) {
		return;
	}
	broken = false;
	for (unsigned i = 0; i < 3; i++) {
		(void)put(&va_macro, "SWR_MIC_CTL (put back as found)", VA_SWR_MIC_CTL0 + 4 * i, found_va_mic[i], 0);
	}
	va_mic_taken = false;
}

/* ---- the DSP: the codec DMA port, a device path for recording, a stream that is read ------------------ */

#define APR_SVC_ASM                             7
#define APR_SVC_ADM                             8
#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_CODEC_DMA_CONFIG           0x000102b8u
#define MIC_PORT                                0xb037          /* TX_CDC_DMA_TX_3 */
#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_PATH_LIVE_RECORDING                 2
#define ADM_MATRIX_AUDIO_TX                     1
#define ADM_TOPOLOGY_NONE                       0x00010312u
#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_READ_V3             0x00010db4u
#define ASM_STREAM_CMD_SET_ENCDEC_PARAM         0x00010c10u
#define ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2      0x00010da3u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_READ_V2                    0x00010dacu
#define ASM_DATA_EVENT_READ_DONE_V2             0x00010d9au
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define ASM_SHMEM_4K_POOL                       3
#define MIC_SESSION                             2               /* (the speakers' is 1) */
#define MIC_STREAM_PORT                         (MIC_SESSION << 8 | 1)
#define READ_TOKEN                              0x40000000u     /* | the period */
#define PCM_CHANNEL_FC                          3
#define MIC_RATE                                48000
#define MIC_PERIODS                             8
#define MIC_PERIOD_FRAMES                       960             /* 20 ms, one channel of 16 bits */
#define MIC_PERIOD_BYTES                        (MIC_PERIOD_FRAMES * 2)

static uintptr_t mic_memory;
static uint64_t mic_address;
static bool memory_taken, mic_mapped, mic_copp_open, mic_stream_open, mic_port_on;
static uint32_t mic_map_handle, mic_copp;
static volatile uint32_t read_ready, read_errors, reads_done;
static volatile uint32_t read_sizes[MIC_PERIODS];

static bool
mic_events(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if (service != APR_SVC_ASM || !(token & READ_TOKEN) || (token & 0x3fffffff) >= MIC_PERIODS) {
		return false;
	}
	if (opcode == ASM_DATA_EVENT_READ_DONE_V2) {
		if (length < 24 || payload[0] != 0) {
			OSIncrementAtomic((volatile SInt32 *)&read_errors);
		}
		read_sizes[token & 0x3fffffff] = length >= 24 ? payload[4] : 0;
		OSIncrementAtomic((volatile SInt32 *)&reads_done);
		OSBitOrAtomic(1u << (token & 31), &read_ready);
		return true;
	}
	if (opcode == APR_BASIC_RSP_RESULT) {
		OSIncrementAtomic((volatile SInt32 *)&read_errors);
		OSBitOrAtomic(1u << (token & 31), &read_ready);
		read_sizes[token & 0x3fffffff] = 0;
		return true;
	}
	return false;
}

static bool
mic_read(unsigned period)
{
	uint32_t read[5] = { (uint32_t)(mic_address + 4096 * period), (uint32_t)((mic_address + 4096 * period) >> 32),
		             mic_map_handle, MIC_PERIOD_BYTES, period };

	return redwood_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, READ_TOKEN | period, ASM_DATA_CMD_READ_V2, read, sizeof(read), 0, NULL);
}

static bool
afe_port_param(uint32_t module, uint32_t param, const void *data, uint16_t size, const char *what)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = MIC_PORT;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

/* the stream and the path, opened, mapped and routed (msm-pcm-q6 and q6adm/q6asm at a recording's open and prepare) */
static bool
dsp_path_on(void)
{
	struct {
		uint16_t pool, regions;
		uint32_t property, low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, 0, 0, 0 };
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_LIVE_RECORDING, MIC_PORT, 0xffff, ADM_TOPOLOGY_NONE, 1, 16, MIC_RATE,
		                           { PCM_CHANNEL_FC } };
	struct {
		uint32_t mode, endpoint, topology, format;
		uint16_t bits, reserved;
	} __attribute__((packed)) reading = { 0, 0, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2, 16, 0 };
	struct {
		uint32_t param, size, frames_per_buffer, block_size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) encoder = { ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2, 28, 0, 20, 1, 16, MIC_RATE, 1, 0, { PCM_CHANNEL_FC } };
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_TX, 1, MIC_SESSION, 1, { 0, 0 } };
	struct redwood_apr_answer answer;
	uint32_t bytes = 0;

	if (!redwood_audio_dsp_memory(&mic_memory, &mic_address, &bytes) || bytes < MIC_PERIODS * 4096) {
		say("mic: no memory the DSP can see\n");
		return false;
	}
	memory_taken = true;
	map.low = (uint32_t)mic_address;
	map.high = (uint32_t)(mic_address >> 32);
	map.size = MIC_PERIODS * 4096;
	redwood_adsp_apr_events_second(mic_events);
	if (!command("our periods for the microphone, mapped for the DSP", APR_SVC_ASM, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map,
	    sizeof(map), ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, &answer)) {
		return false;
	}
	mic_mapped = true;
	mic_map_handle = answer.words[0];
	say("mic: our %u KB at %#llx are the DSP's handle %#x\n", MIC_PERIODS * 4, mic_address, mic_map_handle);
	if (!redwood_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_STREAM_CMD_OPEN_READ_V3, &reading, sizeof(reading), APR_WAIT_MS,
	    &answer) || answer.opcode != APR_BASIC_RSP_RESULT || answer.words[1] != 0) {
		say("mic: the stream, opened for reading: %s (%#x %#x)\n", "REFUSED OR NO ANSWER", answer.opcode, answer.words[1]);
		return false;
	}
	mic_stream_open = true;
	say("mic: the stream (session 2), opened for reading 16-bit linear PCM, no processing: done\n");
	if (!redwood_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_STREAM_CMD_SET_ENCDEC_PARAM, &encoder, sizeof(encoder), APR_WAIT_MS,
	    &answer) || answer.opcode != APR_BASIC_RSP_RESULT || answer.words[1] != 0) {
		say("mic: the stream's format: REFUSED OR NO ANSWER (%#x %#x)\n", answer.opcode, answer.words[1]);
		return false;
	}
	say("mic: the stream's format: 1 channel of 16 bits at 48 kHz: done\n");
	if (!redwood_adsp_apr(APR_SVC_ADM, MIC_PORT, 0, ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), APR_WAIT_MS, &answer) ||
	    answer.opcode != ADM_CMDRSP_DEVICE_OPEN_V5 || answer.words[0] != 0) {
		say("mic: a device path from the port: REFUSED OR NO ANSWER (%#x %#x)\n", answer.opcode, answer.words[0]);
		return false;
	}
	mic_copp_open = true;
	mic_copp = answer.words[1] & 0xffff;
	route.copp[0] = (uint16_t)mic_copp;
	say("mic: the device path from port %#x is open: COPP %u (live recording, no processing, 1 channel, 16 bits, 48 kHz)\n",
	    MIC_PORT, mic_copp);
	return command("the device path routed to session 2", APR_SVC_ADM, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route), 0,
	           NULL);
}

/* the codec DMA port: TX_CDC_DMA_TX_3, one channel of 16 bits at 48 kHz from decimator 1 */
static bool
dsp_port_on(void)
{
	struct {
		uint32_t version, rate;
		uint16_t bits, format, channels, mask;
	} __attribute__((packed)) config = { 1, MIC_RATE, 16, 0, 1, 1u << 1 };
	uint16_t start[2] = { MIC_PORT, 0 };

	if (!afe_port_param(AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_CODEC_DMA_CONFIG, &config, sizeof(config),
	    "the port TX_CDC_DMA_TX_3 (0xb037): 1 channel of 16 bits at 48 kHz, decimator 1")) {
		return false;
	}
	mic_port_on = command("the port 0xb037, started", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL);
	return mic_port_on;
}

static void
dsp_off(void)
{
	uint16_t stop[2] = { MIC_PORT, 0 };

	if (mic_stream_open) {
		(void)redwood_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_STREAM_CMD_CLOSE, NULL, 0, APR_WAIT_MS, NULL);
		say("mic: the stream, closed\n");
		mic_stream_open = false;
	}
	if (mic_copp_open) {
		(void)redwood_adsp_apr(APR_SVC_ADM, (uint16_t)mic_copp, 0, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, APR_WAIT_MS, NULL);
		say("mic: the device path, closed\n");
		mic_copp_open = false;
	}
	if (mic_port_on) {
		(void)command("the port 0xb037, stopped", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL);
		mic_port_on = false;
	}
	if (mic_mapped) {
		(void)command("our periods, unmapped", APR_SVC_ASM, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &mic_map_handle,
		    sizeof(mic_map_handle), 0, NULL);
		mic_mapped = false;
	}
	redwood_adsp_apr_events_second(NULL);
	if (memory_taken) {
		redwood_audio_dsp_memory_put();
		memory_taken = false;
	}
}

/* ---- mic=4: the microphone for iOS ------------------------------------------------------------------- */

/*
 * What the microphone hears goes into a ring, and /dev/redwood-mic gives it to iOS's audio server
 * (RedwoodAudio.driver's device "Codec", userland/src/audio_plugin.c): 16-bit samples, one channel,
 * 48 kHz. The path is up only while the device is open (the audio server opens it when its input
 * starts and closes it when it stops), and a read never waits: it gives what has come, keeping the
 * reader about 40 ms behind the DSP (it comes in 20 ms periods), and nothing until that much is
 * there; the audio server makes up the rest with silence.
 */
#define RING_FRAMES             16384           /* 341 ms; a power of two */
#define RING_KEEP               1920            /* 40 ms: how far behind the DSP a reader is kept */
#define RING_MOST               4800            /* 100 ms: more than this waiting, and the oldest is skipped */

static int16_t *ring;
static volatile uint64_t ring_written;          /* frames put into the ring since the device was made */
static uint64_t ring_read;                      /* frames taken from it (one reader: the audio server) */
static bool ring_primed;                        /* the reader is RING_KEEP behind; false after it ran dry */
static uint64_t ring_skipped, ring_dry;         /* frames skipped, reads that found too little: said when iOS stops */
static volatile uint32_t ios_wants;             /* the device is open */
static volatile bool stopping;                  /* the run ends (redwood_mic_dsp_down()) */

static void
ring_put(const int16_t *samples, uint32_t count)
{
	uint64_t at = ring_written;

	for (uint32_t i = 0; i < count; i++) {
		ring[(at + i) % RING_FRAMES] = samples[i];
	}
	__atomic_store_n(&ring_written, at + count, __ATOMIC_RELEASE);
}

static int
mic_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint64_t written = __atomic_load_n(&ring_written, __ATOMIC_ACQUIRE), avail, want;
	int error = 0;

	if (ring == NULL || uio_resid(uio) < 2) {
		return 0;
	}
	want = (uint64_t)uio_resid(uio) / 2;
	avail = written - ring_read;
	if (avail > RING_MOST) {
		ring_skipped += avail - RING_KEEP;
		ring_read = written - RING_KEEP;
		avail = RING_KEEP;
	}
	if (!ring_primed) {
		if (avail < RING_KEEP) {
			return 0;
		}
		ring_primed = true;
	}
	if (avail < want) {
		ring_dry++;
		ring_primed = false;
	}
	while (want > 0 && avail > 0 && error == 0) {
		uint64_t at = ring_read % RING_FRAMES, n = want < avail ? want : avail;

		n = n < RING_FRAMES - at ? n : RING_FRAMES - at;
		error = uiomove((caddr_t)&ring[at], (int)(n * 2), uio);
		ring_read += n;
		want -= n;
		avail -= n;
	}
	return error;
}

static int
mic_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_wants = 1;
	return 0;
}

/* (on the last close) */
static int
mic_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_wants = 0;
	return 0;
}

static const struct cdevsw mic_cdevsw = {
	.d_open = mic_dev_open, .d_close = mic_dev_close, .d_read = mic_dev_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- listening ---------------------------------------------------------------------------------------------- */

static uint32_t mic_secs = 10;

/*
 * The stream run, read for mic_secs (for_ios: while iOS has the device open): how loud it is, every
 * second (for iOS: the first three, then every 30th); into the recording with rec_sound_mic=1; for
 * iOS into the ring.
 */
static void
listen(bool for_ios)
{
	uint32_t run[3] = { 0, 0, 0 }, frames = 0, seconds = 0, peak = 0, periods = 0, empty = 0;
	int64_t sum = 0;
	uint64_t squares = 0;
	unsigned next = 0;
	bool unmuted = false, settled = false;
	uint64_t began = mach_absolute_time(), now, elapsed_ms;

	struct redwood_apr_answer answer;

	read_ready = 0;
	reads_done = read_errors = 0;
	if (!redwood_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), APR_WAIT_MS, &answer) ||
	    answer.opcode != APR_BASIC_RSP_RESULT || answer.words[1] != 0) {
		say("mic: the session, run: REFUSED OR NO ANSWER (%#x %#x)\n", answer.opcode, answer.words[1]);
		return;
	}
	say("mic: the session 2, run: done\n");
	for (unsigned p = 0; p < MIC_PERIODS; p++) {
		if (!mic_read(p)) {
			say("mic: a read could not be sent\n");
			return;
		}
	}
	if (for_ios) {
		say("mic: LISTENING for iOS: %u reads of 20 ms given to the DSP\n", MIC_PERIODS);
	} else {
		say("mic: LISTENING: %u reads of 20 ms given to the DSP; for %u s\n", MIC_PERIODS, mic_secs);
	}
	for (;;) {
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &now);
		elapsed_ms = now / NSEC_PER_MSEC;
		if (!unmuted && elapsed_ms >= 200) {            /* TX_MACRO_AMIC_UNMUTE_DELAY_MS */
			unmuted = dec1_unmute();
		}
		if (!settled && elapsed_ms >= 300) {            /* TX_MACRO_AMIC_HPF_DELAY_MS */
			settled = dec1_settled();
		}
		if (tap && elapsed_ms >= 2000 && elapsed_ms < 3000) {
			redwood_haptic_pretend(20000);
		} else if (tap == 1 && elapsed_ms >= 3000) {
			redwood_haptic_pretend(0);
			tap = 2;
		}
		if ((for_ios ? (!ios_wants || stopping) : elapsed_ms >= mic_secs * 1000ull) || broken) {
			break;
		}
		if (!(read_ready & (1u << next))) {
			if (elapsed_ms > 1500 && reads_done == 0) {
				say("mic: NO READ HAS COME BACK in 1.5 s\n");
				break;
			}
			sleep_ms(2);
			continue;
		}
		OSBitAndAtomic(~(1u << next), &read_ready);
		{
			int16_t *samples = (int16_t *)(mic_memory + 4096 * next);
			uint32_t got = read_sizes[next] / 2;
			int16_t stereo[2 * 240];

			for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + MIC_PERIOD_BYTES; line += 64) {
				__asm__ volatile ("dc ivac, %0" : : "r" (line));
			}
			__asm__ volatile ("dsb sy");
			got = got > MIC_PERIOD_FRAMES ? MIC_PERIOD_FRAMES : got;
			empty += got == 0;
			for (uint32_t i = 0; i < got; i++) {
				int32_t v = samples[i];
				uint32_t a = (uint32_t)(v < 0 ? -v : v);

				sum += v;
				squares += (uint64_t)((int64_t)v * v);
				peak = a > peak ? a : peak;
			}
			frames += got;
			if (for_ios) {
				ring_put(samples, got);
			}
			if (redwood_audio_recording_mic()) {
				for (uint32_t i = 0; i < got; i += 240) {
					uint32_t n = got - i < 240 ? got - i : 240;

					for (uint32_t j = 0; j < n; j++) {
						stereo[2 * j] = stereo[2 * j + 1] = samples[i + j];
					}
					redwood_audio_record_fed(stereo, n);
				}
			}
		}
		periods++;
		(void)mic_read(next);
		next = (next + 1) % MIC_PERIODS;
		if (frames >= MIC_RATE) {
			uint64_t mean_square = squares / frames;
			uint32_t rms = 0;
			int db10 = -999;

			while ((uint64_t)(rms + 1) * (rms + 1) <= mean_square) {
				rms++;
			}
			if (rms > 0) {
				/* 20 log10(rms / 32768), in tenths, by a table of halvings: good enough to tell loud from quiet */
				uint32_t r = rms;
				int tenths = 0;

				while (r < 16384) {
					r *= 2;
					tenths -= 60;   /* 6.02 dB a halving, near enough */
				}
				db10 = tenths - (int)((32768 - r) * 60 / 16384);
			}
			seconds++;
			if (!for_ios || seconds <= 3 || seconds % 30 == 0) {
				say("mic: second %u: %u frames (%u periods, %u empty), level %d.%d dBFS (rms %u), peak %u, mean %lld; "
				    "reads %u, errors %u%s%s\n", seconds, frames, periods, empty, db10 / 10, db10 < 0 ? -db10 % 10 : db10 % 10,
				    rms, peak, sum / (int64_t)frames, reads_done, read_errors, unmuted ? "" : " (muted)",
				    settled ? "" : " (settling)");
			}
			frames = periods = empty = peak = 0;
			sum = 0;
			squares = 0;
		}
	}
}

static void
undo(void)
{
	dsp_off();
	dec1_off();
	va_mic_off();
	master_off();
	tx_clocks_off();
	pins_restore();
	reset_restore();
	clocks_off();
}

/* Everything up to the stream's run, in a recording's order; false (and `broken') if any of it fails. */
static bool
path_up(void)
{
	if (!clocks_on()) {
		say("mic: the clocks were not all given: the blocks are not touched\n");
		return false;
	}
	if (!reset_release() || !pins_on() || !tx_clocks_on() || !master_on()) {
		return false;
	}
	sleep_ms(5);    /* ("5 ms for SoundWire's auto-enumeration", wcd938x_bind()) */
	if ((codec_device = codec_wait()) == 0) {
		return false;
	}
	swr_write(0xf, SWRS_SCP_INT_STATUS_MASK_1, 0x04, 0xf);  /* (swrm_runtime_resume(): broadcast) */
	sleep_ms(10);
	if (!quiet) {
		codec_look();
	}
	if (level < 3) {
		return true;
	}
	/* the stream and path, the port's format and start, the codec's widgets */
	return bolero_look() && codec_init() && dsp_path_on() && dsp_port_on() && dec1_on() && (!adc_on || adc1_on()) &&
	       datapath_on();
}

/*
 * mic=4: the path up whenever iOS opens /dev/redwood-mic, down when it closes it; after three
 * recordings that broke off, no more until the run ends.
 */
static void
for_ios(void)
{
	int major = cdevsw_add(-1, &mic_cdevsw);
	unsigned recordings = 0, failures = 0;

	ring = kalloc_data(RING_FRAMES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	if (ring == NULL || major < 0 ||
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-mic") == NULL) {
		say("mic: /dev/redwood-mic cannot be made\n");
		return;
	}
	say("mic: THE MICROPHONE IS IOS'S (/dev/redwood-mic) from here on, until the run ends: it listens only while iOS records\n");
	while (!stopping) {
		uint64_t began, ns;
		bool failed;

		if (!ios_wants) {
			sleep_ms(20);
			continue;
		}
		lck_mtx_lock(&mic_lock);
		if (stopping) {
			lck_mtx_unlock(&mic_lock);
			break;
		}
		recordings++;
		quiet = recordings > 1;
		ring_primed = false;
		ring_skipped = ring_dry = 0;
		ring_read = __atomic_load_n(&ring_written, __ATOMIC_ACQUIRE);
		began = mach_absolute_time();
		say("mic: iOS records (%u): the path up%s\n", recordings, quiet ? " (the lines that say all went well are not said again)" :
		    "");
		if (path_up()) {
			listen(true);
		}
		failed = broken;
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		say("mic: iOS's recording %u %s after %llu.%llu s; last: %s; %llu frames skipped as too late, %llu reads found too little\n",
		    recordings, failed ? "BROKE OFF" : "ended", ns / NSEC_PER_SEC, ns % NSEC_PER_SEC / (NSEC_PER_SEC / 10), last_write,
		    ring_skipped, ring_dry);
		undo();
		lck_mtx_unlock(&mic_lock);
		if (failed) {
			broken = false;
			if (++failures >= 3) {
				say("mic: three recordings broke off: the microphone stays off until the run ends\n");
				break;
			}
			/* (not again until iOS has closed the device and opened it again) */
			while (ios_wants && !stopping) {
				sleep_ms(20);
			}
		}
	}
}

static void
mic_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("mic: level %u: %s\n", level, level == 1 ? "a look, after the LPASS's votes and clocks; nothing written to the codec" :
	    level == 2 ? "the codec brought up to answer on SoundWire, nothing of it set; then all undone" :
	    level == 3 ? "the main microphone, as Android records from it, listened to; then all undone" :
	    "the main microphone for iOS (/dev/redwood-mic), up while iOS records");
	if (level == 4) {
		for_ios();
	} else {
		lck_mtx_lock(&mic_lock);
		if (level == 1) {
			if (clocks_on()) {
				sleep_ms(10);
				look();
			} else {
				say("mic: the clocks were not all given: the blocks are not touched\n");
			}
		} else {
			if (path_up() && level >= 3) {
				listen(false);
			}
			say("mic: %s; last: %s\n", broken ? "STOPPED" : level >= 3 ? "listened" : "done", last_write);
		}
		undo();
		lck_mtx_unlock(&mic_lock);
	}
	running = false;
	say("mic: done\n");
	thread_terminate(current_thread());
}

void
redwood_mic_dsp_up(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("mic", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 4 ? 4 : level;
	PE_parse_boot_argn("mic_secs", &mic_secs, sizeof(mic_secs));
	PE_parse_boot_argn("mic_input", &smic_input, sizeof(smic_input));
	PE_parse_boot_argn("mic_adc", &adc_on, sizeof(adc_on));
	PE_parse_boot_argn("mic_main", &main_timing, sizeof(main_timing));
	PE_parse_boot_argn("mic_test", &test_bits, sizeof(test_bits));
	PE_parse_boot_argn("mic_tap", &tap, sizeof(tap));
	mic_secs = mic_secs < 1 ? 1 : mic_secs > 600 ? 600 : mic_secs;
	running = true;
	if (kernel_thread_start(mic_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		return;
	}
	thread_deallocate(thread);
}

void
redwood_mic_dsp_down(void)
{
	stopping = true;
	if (running) {
		lck_mtx_lock(&mic_lock);
		undo();
		lck_mtx_unlock(&mic_lock);
	}
}
