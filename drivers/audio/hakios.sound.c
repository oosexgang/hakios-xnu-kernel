/*
 * The phone's sound, step 3: what the audio DSP's services are told so that the speakers play.
 * Every address and number below belongs to the POCO X5 Pro (redwood), and nothing here happens
 * unless boot-arg sound=N asks for it, with adsp=1 (hakios.adsp.c), which starts the DSP and
 * calls hakios_sound_dsp_up() once its audio services are up.
 *
 * Under Android the speakers' sound goes from the audio server through three of the DSP's
 * services: ASM, a stream the samples are written into; ADM, the matrix that routes a stream to
 * a device path ("COPP"); and AFE, the port at the SoC's interface. The two amplifiers
 * (hakios.speaker.c) sit on the primary TDM interface, which the DSP runs: Android's
 * PRI_TDM_RX_0, AFE port 0x9000 in group 0x9100 (with 0x9002, unused). The samples themselves
 * lie in memory of ours that the DSP reads through the application processors' SMMU, as
 * stream 0x1801 (Android: qcom,msm-audio-ion, iommus <&apps_smmu 0x1801 0>, its addresses from
 * 0x10000000, and the stream's low four bits in bits 32 to 35 of every address the DSP is given:
 * qcom,smmu-sid-mask 0xf, msm_audio_ion.c).
 *
 *   sound=1  the port: the TDM pins to the interface, the interface's bit clock, the group, the
 *            port, started; whether the bit clock and the frame sync then toggle at the pins is
 *            looked at; held for sound_secs (default 3), then everything stopped and put back
 *   sound=2  ... and a stream: our memory shown to the DSP through the SMMU, a device path
 *            (ADM) to the port, an ASM stream routed to it, and a test tone -- 1 kHz at -20 dBFS,
 *            both channels -- written into it for sound_secs (at most 3); the DSP's "written"
 *            events say whether it takes the samples at 48 kHz. The amplifiers stay off.
 *   sound=3  ... and the amplifiers, with the tone playing: set up as Android's driver sets them
 *            up, started once the clock runs, faded in to sound_below eighths of a dB under
 *            Android's own level (default 160: 20 dB under, the least it may be is 0), faded
 *            out and powered down at the end. Audible.
 *   sound=4  what Darwin plays (/dev/hakios-audio, hakios.audio.c), until the run ends: the port
 *            and the stream as above, kept running (silence when Darwin writes nothing); the
 *            amplifiers set up once and powered down, started and faded in (the driver's quick
 *            fade) whenever a write is not silence, stopped after three seconds of silence; their
 *            level is what is set on the plug-in's volume control (ioctl HAKIOS_AUDIO_SET_VOLUME):
 *            so far under sound_below, which stays the loudest, and at 0 they stay off. (Darwin sets
 *            that control only once, to the top, and applies its volume to the samples itself --
 *            see userland/src/audio_plugin.c --; at its volume 0 it writes silence, and the
 *            amplifiers stay off for that too.) On their way the samples go through the speakers'
 *            guard (hakios.guard.c), in place of the DSP's speaker protection that Android has:
 *            sound_gain eighths of a dB of gain (default and most 96, 12 dB: Darwin's own processing
 *            keeps its top that far under full scale), a high-pass filter, three averages of the
 *            power at the speakers and a peak limiter. sound_guard_as=N, for a test: the guard
 *            reckons the amplifiers N eighths of a dB under Android's level (louder than they are)
 *            and holds back as it would there -- with sound_below=720 (the amplifiers at -90 dB)
 *            what it does at the loudest can be seen in the log and the recording without a sound.
 *
 * The numbers are Android's own, from the phone's live device tree (qcom,msm-dai-tdm-pri-rx and
 * its port, the pins pri_tdm_*_active), its mixer paths (the speaker: PRI_TDM_RX_0 at 48 kHz,
 * two channels, S32_LE) and its kernel (Xiaomi's hakios-s-oss, techpack/audio: asoc/lahaina.c,
 * asoc/msm-dai-q6-v2.c, dsp/q6afe.c, dsp/q6adm.c, dsp/q6asm.c):
 *   - the bit clock: the interface's internal one (clock 0x200, "PRI_TDM_IBIT"), attribute 4
 *     ("invert, couple no", msm-dai-q6-v2.c's default; the tree names none), at 48000 x 32 bits x
 *     2 slots = 3.072 MHz -- the machine driver sets that rate itself (lahaina.c, which on redwood
 *     halves its four slots to two: "Using default tdm max slot: 4" in Android's log), not the
 *     tree's 1.536 MHz
 *   - the group: ports 0x9000 and 0x9002, two 32-bit slots, mask 3, 48 kHz
 *   - the port: two channels of 32 bits at 48 kHz, linear PCM; sync mode 0, sync source 1, data
 *     out 0, inverted sync 1, data delay 1; slot mapping: channels at byte offsets 0 and 4 (slot
 *     0, the amplifier at 0x35; slot 1, the one at 0x34), MSB-aligned
 *   - the pins: GPIO 97 (bit clock) and 100 (frame sync) function mi2s0, 8 mA, no pull, output
 *     high; 98 and 99 (data in and out) function mi2s0, 8 mA, no pull (pinctrl-yupik.c: function
 *     1 for all four)
 *   - the path: ADM's device open (version 5) for playback to port 0x9000 with no processing
 *     (topology 0x10312), two channels, 24 bits, 48 kHz; the stream: ASM session 1, stream 1,
 *     opened for writing linear PCM of 16 bits, no processing (0x10c68), routed to that path
 *
 * What is started is stopped, and what is written is put back, when the time is over -- or, if
 * the DSP is to be shut down earlier (the run's end), before that (hakios_sound_dsp_down()).
 * The amplifiers are powered down on a panic's way out (hakios_sound_quiesce()).
 *
 * Message layouts: the files above, and Linux's sound/soc/qcom/qdsp6/ (q6afe.c, q6adm.c,
 * q6asm.c); the SMMU: hakios.wifi.c, which does the same for the Wi-Fi's stream.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.audio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

/* The SMMU's match registers and contexts are found free and taken by the Wi-Fi's start as well (hakios.wifi.c). */
extern lck_mtx_t hakios_smmu_lock;

static LCK_GRP_DECLARE(sound_lock_group, "hakios-sound");
static LCK_MTX_DECLARE(sound_lock, &sound_lock_group);  /* the path being set up or taken down */

static uint32_t level, seconds = 3, below = 240, pause_ms = 1000, gain = HAKIOS_GUARD_MAX_GAIN;
static uint32_t guard_as = UINT32_MAX;  /* boot-arg sound_guard_as=N: the guard reckons the amplifiers at most N under (a test) */
static char last_write[96] = "nothing written";

/* ---- what is said: to the loader's log as well, as it happens ---------------------------------------- */

static int say(const char *format, ...) __printflike(1, 2);

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
	return length;
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* The time of day (UTC, as the host gave it: boot-arg epoch) and since the kernel began, for whoever listens. */
static void
say_when(const char *what)
{
	clock_sec_t secs;
	clock_usec_t usecs;
	uint64_t up = now_us();

	clock_get_calendar_microtime(&secs, &usecs);
	say("sound: %s at %02lu:%02lu:%02lu.%01u UTC (%llu.%01llu s after the kernel began)\n", what, (unsigned long)(secs / 3600 % 24),
	    (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60), (unsigned)(usecs / 100000), up / 1000000, up / 100000 % 10);
}

/* Time for the loader to note what has just been said, before something not yet seen to be harmless. */
static void
announce(const char *what)
{
	say("sound: %s\n", what);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
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
	say("sound: %s %s (%#x) = %#x\n", b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	say("sound: about to write %s %s at %#x: %#x\n", b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "wrote %#x to %s %s at %#x", value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

#ifdef HAKIOS_ADRENO7
/* the Xiaomi 12S speaks AudioReach (GPR) to the DSP, not APR: the four functions below that talk to it are ar_*() there */
static bool ar_afe_on(void);
static void ar_afe_off(void);
static bool ar_stream_on(void);
static void ar_stream_off(void);
static bool ar_period_write(unsigned period);
static bool ar_events(uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length);
#endif

/* ---- the TDM pins ------------------------------------------------------------------------------------- */

static struct block tlmm = { "pin controller", 0x0f100000, 0x100000 };
#define GPIO_CFG(n)             (0x1000u * (n))         /* [1:0] pull, [5:2] function, [8:6] drive (mA / 2 - 1), [9] output */
#define GPIO_IO(n)              (0x1000u * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_MI2S0               (1u << 2)
#define CFG_8MA                 (3u << 6)
#define CFG_OUTPUT              (1u << 9)

static const struct {
	unsigned pin;
	const char *name;
	uint32_t cfg, io;               /* Android's active state */
} tdm_pins[] = {
#ifdef HAKIOS_ADRENO7
	/* the Xiaomi 12S: the tertiary MI2S (dt tert_tdm_pinctrl; function 1 is mi2s2_* for all four, pinctrl-sm8450.c) */
	{ 121, "gpio121 (bit clock)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 123, "gpio123 (word select)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 122, "gpio122 (data 0)", CFG_MI2S0 | CFG_8MA, 0 },
	{ 124, "gpio124 (data 1)", CFG_MI2S0 | CFG_8MA, 0 },
#else
	{ 97, "gpio97 (bit clock)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 100, "gpio100 (frame sync)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 98, "gpio98 (data in)", CFG_MI2S0 | CFG_8MA, 0 },
	{ 99, "gpio99 (data out)", CFG_MI2S0 | CFG_8MA, 0 },
#endif
};
#define TDM_PINS                (sizeof(tdm_pins) / sizeof(tdm_pins[0]))
static uint32_t found_cfg[TDM_PINS], found_io[TDM_PINS];
static bool pins_taken;

static void
pins_on(void)
{
	for (unsigned i = 0; i < TDM_PINS; i++) {
		found_cfg[i] = get(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin));
		found_io[i] = get(&tlmm, tdm_pins[i].name, GPIO_IO(tdm_pins[i].pin));
	}
	pins_taken = true;
	for (unsigned i = 0; i < TDM_PINS; i++) {
		put(&tlmm, tdm_pins[i].name, GPIO_IO(tdm_pins[i].pin), tdm_pins[i].io);
		put(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin), tdm_pins[i].cfg);
	}
}

static void
pins_off(void)
{
	if (!pins_taken) {
		return;
	}
	for (unsigned i = 0; i < TDM_PINS; i++) {
		put(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin), found_cfg[i]);
		put(&tlmm, tdm_pins[i].name, GPIO_IO(tdm_pins[i].pin), found_io[i] & 2);
		get(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin));
	}
	pins_taken = false;
}

/* Whether a pin toggles: read many times as fast as it goes. */
static void
pin_activity(unsigned pin, const char *name, uint32_t *changes, uint32_t *ones)
{
	uint32_t last = rd(tlmm.base + GPIO_IO(pin)) & 1, reads = 20000;
	uint64_t began = now_us();

	*changes = *ones = 0;
	for (uint32_t i = 0; i < reads; i++) {
		uint32_t now = rd(tlmm.base + GPIO_IO(pin)) & 1;

		*changes += now != last;
		*ones += now;
		last = now;
	}
	say("sound: %s: %u changes and %u ones in %u reads (%llu us)\n", name, *changes, *ones, reads, now_us() - began);
}

/* ---- APR ------------------------------------------------------------------------------------------- */

#define APR_SVC_AFE             4
#define APR_SVC_ASM             7
#define APR_SVC_ADM             8
#define APR_BASIC_RSP_RESULT    0x000110e8u
#define APR_WAIT_MS             1000

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "an error this does not know";
}

/*
 * One command, its answer waited for: true if the answer is the basic one with status 0, or the
 * opcode `wanted' (then in *answer).
 */
static bool
command(const char *what, uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct hakios_apr_answer *answer)
{
	struct hakios_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	snprintf(last_write, sizeof(last_write), "sent %s (opcode %#x)", what, opcode);
	if (!hakios_adsp_apr(service, port, 0, opcode, payload, length, APR_WAIT_MS, answer)) {
		say("sound: %s (opcode %#x to service %u, port %#x): NO ANSWER\n", what, opcode, service, port);
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->words[0] == opcode) {
		say("sound: %s: %s%s\n", what, answer->words[1] == 0 ? "done" : "REFUSED: ", answer->words[1] == 0 ? "" :
		    dsp_error(answer->words[1]));
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		return true;
	}
	say("sound: %s: an answer not expected: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
	    answer->words[0], answer->words[1]);
	return false;
}

/* ---- AFE: the port ---------------------------------------------------------------------------------- */

#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_TDM_CONFIG                 0x0001029du
#define AFE_MODULE_TDM                          0x0001028au
#define AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG   0x00010297u
#define AFE_MODULE_GROUP_DEVICE                 0x00010254u
#define AFE_PARAM_ID_GROUP_DEVICE_ENABLE        0x00010256u
#define AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG    0x0001029eu
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u

#define PORT                    0x9000          /* PRI_TDM_RX_0 */
#define GROUP                   0x9100          /* PRI_TDM_RX */
#define GROUP_OTHER_PORT        0x9002
#define PORT_INVALID            0xffff
#define CLOCK_PRI_TDM_IBIT      0x200
#define CLOCK_INVERT_COUPLE_NO  4
#ifdef HAKIOS_ADRENO7
/* Android's speaker on this phone: TDM-LPAIF-RX-TERTIARY-VIRT-0 at 96 kHz, and the amplifiers' profile says so (0x06); sound_matrix=1 tries others */
static uint32_t bus_rate = 48000;       /* (what Darwin plays; the amplifiers told so: speaker_sr=8. sound_matrix=2 at 96000 is what first made them run) */
/*
 * The bus as Android has it (its log: TDM-LPAIF-RX-TERTIARY 96000, 32 bit; the tree's tert TDM node: internal clock and sync, sync mode 1,
 * inverted, one bit of delay): MODULE_ID_TDM_SINK in place of the I2S sink, sound_matrix=2 trying the frame's layout.
 */
static struct { bool on; uint16_t nslots; uint32_t mask; uint16_t sync_mode, invert, delay; } bus_tdm = { true, 4, 3, 1, 1, 1 };
#define CLOCK_TER_TDM_IBIT      0x204u
#define CLOCK_TER_MI2S_IBIT     0x104u
#define RATE                    bus_rate
#else
#define RATE                    48000
#endif
#define SLOTS                   2
#define SLOT_BITS               32
#define PORT_BITS               32              /* the mixer path's S32_LE */

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_group_tdm {
	uint32_t version;
	uint16_t group, reserved;
	uint16_t ports[8];
	uint32_t channels, rate, bits;
	uint16_t slots, slot_bits;
	uint32_t slot_mask;
} __attribute__((packed));

struct afe_group_enable {
	uint16_t group, enable;
} __attribute__((packed));

struct afe_tdm {
	uint32_t version, channels, rate, bits;
	uint16_t data_format, sync_mode, sync_source, slots, data_out, invert_sync, data_delay, slot_bits;
	uint32_t slot_mask;
} __attribute__((packed));

struct afe_slot_mapping {
	uint32_t version;
	uint16_t channels, bits;
	uint32_t align;
	uint16_t offsets[8];
} __attribute__((packed));

/* A parameter for the AFE as a whole (its clocks, its groups): in the packet, not in shared memory. */
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
	return command(what, APR_SVC_AFE, 0, AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

/* A parameter of the port. */
static bool
afe_port_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = PORT;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

static bool clock_on, group_on, port_on;

static bool
afe_clock(bool on)
{
	struct afe_clock_set clock = { 1, CLOCK_PRI_TDM_IBIT, RATE * SLOT_BITS * SLOTS, CLOCK_INVERT_COUPLE_NO, 0, on };

	return afe_service_param(on ? "the TDM bit clock (0x200) at 3.072 MHz, on" : "the TDM bit clock, off", AFE_MODULE_CLOCK_SET,
	           AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static bool
afe_on(void)
{
#ifdef HAKIOS_ADRENO7
	return ar_afe_on();
#else
	struct afe_group_tdm group = {
		.version = 1, .group = GROUP, .ports = { PORT, GROUP_OTHER_PORT, PORT_INVALID, PORT_INVALID, PORT_INVALID,
		                                         PORT_INVALID, PORT_INVALID, PORT_INVALID },
		.channels = SLOTS, .rate = RATE, .bits = SLOT_BITS, .slots = SLOTS, .slot_bits = SLOT_BITS, .slot_mask = 3,
	};
	struct afe_group_enable enable = { GROUP, 1 };
	struct afe_tdm tdm = {
		.version = 1, .channels = 2, .rate = RATE, .bits = PORT_BITS, .data_format = 0, .sync_mode = 0, .sync_source = 1,
		.slots = SLOTS, .data_out = 0, .invert_sync = 1, .data_delay = 1, .slot_bits = SLOT_BITS, .slot_mask = 3,
	};
	struct afe_slot_mapping mapping = { .version = 1, .channels = 2, .bits = PORT_BITS, .align = 0,
		                            .offsets = { 0, 4, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff } };
	uint16_t start[2] = { PORT, 0 };

	announce("the DSP is asked for the TDM interface's bit clock, the group, the port and its start (AFE)");
	clock_on = afe_clock(true);
	if (!clock_on) {
		return false;
	}
	group_on = afe_service_param("the group 0x9100: ports 0x9000 0x9002, 2 slots of 32 bits, 48 kHz", AFE_MODULE_GROUP_DEVICE,
	    AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG, &group, sizeof(group)) &&
	    afe_service_param("the group 0x9100, on", AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_ENABLE, &enable,
	    sizeof(enable));
	if (!group_on) {
		return false;
	}
	if (!afe_port_param("the port 0x9000: 2 channels of 32 bits at 48 kHz in 2 slots, sync 0 1, inverted, delay 1",
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_TDM_CONFIG, &tdm, sizeof(tdm)) ||
	    !afe_port_param("the port's slots: channels at bytes 0 and 4", AFE_MODULE_TDM, AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG,
	    &mapping, sizeof(mapping))) {
		return false;
	}
	port_on = command("the port 0x9000, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL);
	return port_on;
#endif
}

static void
afe_off(void)
{
#ifdef HAKIOS_ADRENO7
	ar_afe_off();
#else
	uint16_t stop[2] = { PORT, 0 };
	struct afe_group_enable disable = { GROUP, 0 };

	if (port_on) {
		command("the port 0x9000, stopped", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL);
		port_on = false;
	}
	if (group_on) {
		afe_service_param("the group 0x9100, off", AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_ENABLE, &disable,
		    sizeof(disable));
		group_on = false;
	}
	if (clock_on) {
		afe_clock(false);
		clock_on = false;
	}
#endif
}

/* ---- the DSP's view of our memory: stream 0x1801 of the SMMU ------------------------------------------- */

static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };              /* the second page: a word per context */
static struct block apps_smmu_context = { "apps smmu, the audio context", 0, 0x1000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define PTE_ADDRESS             0x0000fffffffff000ull
#define AUDIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as hakios.wifi.c's */

#define AUDIO_STREAM            0x1801
#define AUDIO_ADDRESSES         0x10000000ull   /* dt: qcom,iommu-dma-addr-pool of qcom,msm-audio-ion */
#define AUDIO_STREAM_BITS       (1ull << 32)    /* the stream's low bits (0x1801 & 0xf), where the DSP takes them from */

#define PERIODS                 8
#define PERIOD_FRAMES           960             /* 20 ms */
#define PERIOD_BYTES            (PERIOD_FRAMES * 4)
#define BUFFER_BYTES            (PERIODS * 4096)        /* a page each */
#define MIC_BYTES               (16 * 4096)             /* the microphones' (hakios.mic.c), behind the periods */
#define SHARED_BYTES            (BUFFER_BYTES + MIC_BYTES)

static struct {
	bool found, on, passed;         /* passed: the boot chain lets the stream through untranslated */
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables;             /* three of 4K: the root, one of the second level, one of the third */
} space;
static vm_offset_t buffer;              /* BUFFER_BYTES of ours, the periods (and MIC_BYTES behind them: the microphones') */
static uint64_t buffer_address;         /* as the DSP is to know it */

static bool
space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;

	if (scr0 & 1u << 3) {
		say("sound: this SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * space.pages * space.page != 0x100000) {
		say("sound: the SMMU says it has 2 x %u pages of %#x bytes, not a megabyte: its contexts are not looked for\n",
		    space.pages, space.page);
		return false;
	}
	space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ AUDIO_STREAM) & ~mask) == 0) {
				say("sound: match register %u covers the audio stream already: stream %#x mask %#x, s2cr %#x\n", n,
				    stream, mask, s2cr);
				if ((s2cr >> 16 & 3) == 1) {
					space.passed = true;
					return true;
				}
				say("sound: ... and does not let it through untranslated: it is somebody's, and is left alone\n");
				return false;
			}
		} else if (space.match == ~0u) {
			space.match = n;
			space.old_smr = smr;
			space.old_s2cr = s2cr;
		}
	}
#ifdef HAKIOS_ADRENO7
	/*
	 * the Xiaomi 12S: from the top, as the Wi-Fi's start chooses (devices/mayfly/drivers/gpu/hakios.adreno7.c): the low ones
	 * are likeliest the boot chain's, and a context that is switched on (the Wi-Fi's) is passed by
	 */
	space.context = contexts;
	for (uint32_t c = contexts; c-- > 1;) {
		if (!used[c] && (rd(ml_io_map(apps_smmu.phys + (space.pages + c) * (vm_offset_t)space.page, 0x1000)) & 1) == 0) {
			space.context = c;
			break;
		}
	}
#else
	for (space.context = 0; space.context < contexts && used[space.context]; space.context++) {
	}
#endif
	if (space.match == ~0u || space.context >= contexts) {
		say("sound: the SMMU has no match register or no context left\n");
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + space.page;
	apps_smmu_context.phys = apps_smmu.phys + (space.pages + space.context) * space.page;
	say("sound: the boot chain has set nothing up for stream %#x; free: match register %u (smr %#x, s2cr %#x) and context %u, at "
	    "%#x\n", AUDIO_STREAM, space.match, space.old_smr, space.old_s2cr, space.context, apps_smmu_context.phys);
	space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	if (space.old_sctlr & 1) {
		say("sound: CONTEXT %u IS SWITCHED ON although no stream is sent through it: it is somebody's, and is left alone\n",
		    space.context);
		return false;
	}
	space.found = true;
	return true;
}

static void
space_faults(void)
{
	uint32_t status, global;

	if (!space.on) {
		return;
	}
	status = rd(apps_smmu_context.base + SMMU_CB_FSR);
	global = rd(apps_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("sound: the SMMU has refused the DSP nothing (context %u: fsr %#x; sgfsr 0)\n", space.context, status);
		return;
	}
	say("sound: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: status "
	    "%#x, stream %#x\n", space.context, status, (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 |
	    rd(apps_smmu_context.base + SMMU_CB_FAR), rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global,
	    rd(apps_smmu.base + SMMU_SGFSYNR1));
}

/* Our buffer behind the DSP's addresses from AUDIO_ADDRESSES on, through a context of ours. */
static bool
space_on(void)
{
	uint64_t *root, *second, *third, table_root;
	uint32_t smr = SMMU_SMR(space.match), s2cr = SMMU_S2CR(space.match), sees = 1u << 31 | AUDIO_STREAM;

	/*
	 * (the page tables are allocated once and reused: this runs at every first user of the DSP's buffers, and nothing frees
	 * the tables, so a new allocation each time would lose 12 KB of wired memory per recording or run)
	 */
	if (space.tables != 0) {
		bzero((void *)space.tables, 3 * 4096);
	} else if (kmem_alloc(kernel_map, &space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		space.tables = 0;
		return false;
	}
	root = (uint64_t *)space.tables;
	second = (uint64_t *)(space.tables + 4096);
	third = (uint64_t *)(space.tables + 8192);
	root[AUDIO_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[AUDIO_ADDRESSES >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned page = 0; page < SHARED_BYTES / 4096; page++) {
		third[(AUDIO_ADDRESSES >> 12 & 0x1ff) + page] = kvtophys(buffer + 4096 * page) | AUDIO_PTE;
	}
	for (vm_offset_t line = space.tables; line < space.tables + 3 * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(space.tables) | (uint64_t)space.context << 48;
	say("sound: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) for the %u KB of the periods and the microphones' "
	    "at %#llx, and match register %u sends stream %#x through it\n", space.context, table_root & PTE_ADDRESS,
	    SHARED_BYTES / 1024, AUDIO_ADDRESSES, space.match, AUDIO_STREAM);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(space.context), SMMU_CBA2R_64_BIT);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(space.context), SMMU_CBAR_STAGE_1);
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)table_root);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("sound: the SMMU does not finish forgetting old translations\n");
			return false;
		}
		sleep_ms(1);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	space.on = true;
	put(&apps_smmu, "s2cr", s2cr, space.context);
	put(&apps_smmu, "smr", smr, sees);
	if (get(&apps_smmu, "smr", smr) != sees || (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) != space.context ||
	    !(get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1) ||
	    get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0) != (uint32_t)table_root) {
		say("sound: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u\n", AUDIO_STREAM,
		    space.context);
		return false;
	}
	say("sound: THE AUDIO STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", space.context);
	return true;
}

static void
space_off(void)
{
	if (!space.on) {
		return;
	}
	space_faults();
	say("sound: the audio stream and context %u of the SMMU are put back as they were found\n", space.context);
	put(&apps_smmu, "smr", SMMU_SMR(space.match), space.old_smr);
	put(&apps_smmu, "s2cr", SMMU_S2CR(space.match), space.old_s2cr);
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, space.old_sctlr);
	get(&apps_smmu, "smr", SMMU_SMR(space.match));
	space.on = false;
}

/*
 * The DSP's view of our memory is one: the sound's periods and the microphones' buffers behind them
 * (hakios.mic.c), through the same stream and context. Whoever comes first sets it up, whoever
 * leaves last takes it down. user: 0 the sound, 1 the microphones. With hakios_smmu_lock held.
 */
static uint32_t space_users;

static bool
shared_get(unsigned user)
{
	if (space_users == 0) {
		if (buffer == 0 && kmem_alloc(kernel_map, &buffer, SHARED_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			buffer = 0;
			say("sound: no memory for the DSP's buffers\n");
			return false;
		}
		if (!space.found && !space.passed && !space_find()) {
			return false;
		}
		if (!space.passed && !space.on && !space_on()) {
			return false;
		}
		buffer_address = space.passed ? (kvtophys(buffer) | AUDIO_STREAM_BITS) : (AUDIO_ADDRESSES | AUDIO_STREAM_BITS);
		if (space.passed) {
			for (unsigned page = 1; page < SHARED_BYTES / 4096; page++) {
				if (kvtophys(buffer + 4096 * page) != kvtophys(buffer) + 4096 * page) {
					say("sound: the DSP's buffers are not one piece of memory, and the stream is not translated\n");
					return false;
				}
			}
		}
	}
	space_users |= 1u << user;
	return true;
}

static void
shared_put(unsigned user)
{
	if (!(space_users & 1u << user)) {
		return;
	}
	space_users &= ~(1u << user);
	if (space_users == 0) {
		space_off();
	}
}

bool
hakios_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes)
{
	bool ok;

	lck_mtx_lock(&hakios_smmu_lock);
	ok = shared_get(1);
	lck_mtx_unlock(&hakios_smmu_lock);
	if (ok) {
		*memory = buffer + BUFFER_BYTES;
		*dsp_address = buffer_address + BUFFER_BYTES;
		*bytes = MIC_BYTES;
	}
	return ok;
}

void
hakios_audio_dsp_memory_put(void)
{
	lck_mtx_lock(&hakios_smmu_lock);
	shared_put(1);
	lck_mtx_unlock(&hakios_smmu_lock);
}

/* ---- ADM and ASM: the path and the stream --------------------------------------------------------------- */

#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_PATH_PLAYBACK                       1
#define ADM_MATRIX_AUDIO_RX                     0
#define ADM_TOPOLOGY_NONE                       0x00010312u
#define ADM_BITS                                24

#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_WRITE_V3            0x00010db3u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2        0x00010d98u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_WRITE_V2                   0x00010dabu
#define ASM_DATA_EVENT_WRITE_DONE_V2            0x00010d99u
#define ASM_SESSION_EVENT_RX_UNDERFLOW          0x00010c17u
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define ASM_SHMEM_4K_POOL                       3
#define SESSION                                 1
#define STREAM_PORT                             (SESSION << 8 | 1)
#define WRITE_TOKEN                             0x80000000u     /* | the period: what a write's "done" carries back */
#define PCM_CHANNEL_FL                          1
#define PCM_CHANNEL_FR                          2

static volatile bool stopping;          /* the run is ending: whatever plays stops */
static bool mapped, copp_open, stream_open;
static uint32_t map_handle, copp;
static volatile uint32_t returned;      /* the periods the DSP has said it is done with (a bit each) */
static volatile uint32_t written_done, underflows, write_errors;

/* The DSP's events: the "done" of every write, and underflows. On the link's thread. */
static bool
events(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
#ifdef HAKIOS_ADRENO7
	return ar_events(service, port, opcode, token, payload, length);
#else
	if (service != APR_SVC_ASM) {
		return false;
	}
	if (opcode == ASM_DATA_EVENT_WRITE_DONE_V2 && (token & WRITE_TOKEN)) {
		if (length >= 16 && payload[3] != 0) {
			OSIncrementAtomic((volatile SInt32 *)&write_errors);
		}
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (opcode == APR_BASIC_RSP_RESULT && (token & WRITE_TOKEN)) {
		/* (a write the DSP refused outright: it answers it as a command) */
		OSIncrementAtomic((volatile SInt32 *)&write_errors);
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		OSIncrementAtomic((volatile SInt32 *)&underflows);
		return true;
	}
	return false;
#endif
}

static bool
stream_on(void)
{
#ifdef HAKIOS_ADRENO7
	return ar_stream_on();
#else
	struct {
		uint16_t pool, regions;
		uint32_t property;
		uint32_t low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)buffer_address, (uint32_t)(buffer_address >> 32),
		                          BUFFER_BYTES };
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_PLAYBACK, PORT, 0xffff, ADM_TOPOLOGY_NONE, 2, ADM_BITS, RATE,
		                           { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_RX, 1, SESSION, 1, { 0, 0 } };
	struct {
		uint32_t flags;
		uint16_t sink, bits;
		uint32_t topology, format;
	} __attribute__((packed)) write = { 0, 0, 16, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
	struct {
		uint32_t size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) format = { 20, 2, 16, RATE, 1, 0, { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	uint32_t run[3] = { 0, 0, 0 };
	struct hakios_apr_answer answer;

	announce("the DSP is given our periods (ASM), a device path to the port (ADM) and a stream routed to it (ASM)");
	if (!command("our periods, mapped for the DSP", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map),
	    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, &answer)) {
		return false;
	}
	mapped = true;
	map_handle = answer.words[0];
	say("sound: our %u KB at %#llx are the DSP's handle %#x\n", BUFFER_BYTES / 1024, buffer_address, map_handle);
	if (!command("a device path to the port: playback, no processing, 2 channels of 24 bits at 48 kHz", APR_SVC_ADM, PORT,
	    ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), ADM_CMDRSP_DEVICE_OPEN_V5, &answer)) {
		return false;
	}
	if (answer.words[0] != 0) {
		say("sound: the device path: REFUSED: %s\n", dsp_error(answer.words[0]));
		return false;
	}
	copp_open = true;
	copp = answer.words[1] & 0xffff;        /* (the upper half is reserved) */
	route.copp[0] = (uint16_t)copp;
	say("sound: the device path is open: COPP %u\n", copp);
	if (!command("the stream, opened for writing: 16-bit linear PCM, no processing", APR_SVC_ASM, STREAM_PORT,
	    ASM_STREAM_CMD_OPEN_WRITE_V3, &write, sizeof(write), 0, NULL)) {
		return false;
	}
	stream_open = true;
	return command("session 1 routed to the device path", APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route),
	           0, NULL) &&
	       command("the stream's format: 2 channels of 16 bits at 48 kHz, left and right", APR_SVC_ASM, STREAM_PORT,
	           ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &format, sizeof(format), 0, NULL) &&
	       command("the session, run", APR_SVC_ASM, STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0, NULL);
#endif
}

static void
stream_off(void)
{
#ifdef HAKIOS_ADRENO7
	ar_stream_off();
#else
	uint32_t handle = map_handle;

	if (stream_open) {
		command("the stream, closed", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0, NULL);
		stream_open = false;
	}
	if (copp_open) {
		command("the device path, closed", APR_SVC_ADM, (uint16_t)copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0, NULL);
		copp_open = false;
	}
	if (mapped) {
		command("our periods, unmapped", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &handle, sizeof(handle), 0, NULL);
		mapped = false;
	}
#endif
}

/* ---- the test tone ----------------------------------------------------------------------------------- */

/* 1 kHz at 48 kHz, -20 dBFS (a peak of 3277): one cycle */
static const int16_t tone[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249,
	3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638, 1254, 848, 428,
	0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249,
	-3277, -3249, -3165, -3028, -2838, -2600, -2317, -1995, -1638, -1254, -848, -428,
};
static uint32_t tone_at;
static unsigned looks_during_tone;
static bool matrix_runs, matrix_looked;
static uint16_t matrix_status, matrix_pvdd;

static void
period_fill(unsigned period, bool silent)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);

	for (unsigned frame = 0; frame < PERIOD_FRAMES; frame++) {
		int16_t sample = silent ? 0 : tone[tone_at++ % 48];

		samples[2 * frame] = sample;
		samples[2 * frame + 1] = sample;
	}
	for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));        /* the DSP reads memory, not our caches */
	}
	__asm__ volatile ("dsb sy");
}

static bool
period_write(unsigned period)
{
#ifdef HAKIOS_ADRENO7
	return ar_period_write(period);
#else
	uint32_t write[8] = { (uint32_t)(buffer_address + 4096 * period), (uint32_t)((buffer_address + 4096 * period) >> 32),
		              map_handle, PERIOD_BYTES, period, 0, 0, 0 };

	return hakios_adsp_apr(APR_SVC_ASM, STREAM_PORT, WRITE_TOKEN | period, ASM_DATA_CMD_WRITE_V2, write, sizeof(write), 0, NULL);
#endif
}

/*
 * The tone, written period after period as the DSP gives them back, for `ms'; then silence until
 * the DSP has given every period back. What came back and how fast is said.
 */
static bool
play(uint32_t ms, bool amplifiers)
{
	uint64_t began, last_look, ended;
	uint32_t sent = 0, done_before = written_done, healthy_looks = 0;
	bool ok = true, tone_over = false;

	returned = 0;
	tone_at = 0;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill(p, false);
	}
	began = last_look = now_us();
	for (unsigned p = 0; p < PERIODS; p++) {
		if (!period_write(p)) {
			say("sound: the write of period %u could not be sent\n", p);
			return false;
		}
		sent++;
	}
	for (;;) {
		uint32_t back = returned;
		uint64_t now = now_us();

		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			for (unsigned p = 0; p < PERIODS; p++) {
				if (!(back & 1u << p) || tone_over) {
					continue;
				}
				period_fill(p, false);
				if (!period_write(p)) {
					say("sound: the write of period %u could not be sent\n", p);
					ok = false;
					tone_over = true;
					break;
				}
				sent++;
			}
		}
		if (!tone_over && (now - began >= ms * 1000ull || stopping)) {
			tone_over = true;
		}
		if (tone_over && written_done - done_before >= sent) {
			break;
		}
		if (now - began > ms * 1000ull + 2000000) {
			say("sound: THE DSP HAS NOT GIVEN EVERY PERIOD BACK 2 s after the end (%u of %u)\n", written_done - done_before,
			    sent);
			ok = false;
			break;
		}
		if (written_done == done_before && now - began > 1000000) {
			say("sound: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND\n");
			ok = false;
			break;
		}
		if (amplifiers && now - last_look >= 200000) {
			last_look = now;
			healthy_looks++;
			if (!hakios_speaker_healthy()) {
				ok = false;
				tone_over = true;
			}
		}
		if (amplifiers && looks_during_tone < 2 && now_us() - began > (looks_during_tone == 0 ? 1000000u : 2000000u)) {
			looks_during_tone++;
			if (!matrix_runs) {
				hakios_speaker_dump(looks_during_tone == 1 ? "during the tone, 1 s" : "during the tone, 2 s");
			}
		}
		if (amplifiers && matrix_runs && !matrix_looked && now_us() - began > 800000u) {
			matrix_looked = true;
			hakios_speaker_now(&matrix_status, &matrix_pvdd);
		}
		sleep_ms(2);
	}
	ended = now_us();
	{
		uint32_t done = written_done - done_before;
		uint64_t frames = (uint64_t)done * PERIOD_FRAMES, us = ended - began;

		say("sound: %u periods written, %u given back (%u with an error), %u underflows, in %llu ms: the DSP took %llu frames a "
		    "second%s\n", sent, done, write_errors, underflows, us / 1000, us ? frames * 1000000 / us : 0,
		    amplifiers ? "" : " (the amplifiers off: nothing to hear)");
		if (amplifiers) {
			say("sound: the amplifiers' status looked at %u times while it played\n", healthy_looks);
		}
	}
	return ok;
}

#ifdef HAKIOS_ADRENO7
/* ---- the Xiaomi 12S: AudioReach -------------------------------------------------------------------------------- */

/*
 * The same path as above in AudioReach's terms (Linux: sound/soc/qcom/qdsp6/audioreach.c, q6apm.c, q6prm.c; the graph is
 * that of linux-msm/audioreach-topology's ar-sm8250-rb5-i2s.m4, a stream and a tertiary MI2S device):
 *   - "the port": the resource manager (PRM, GPR port 2) is asked for the LPASS core and the tertiary MI2S's internal bit
 *     clock (clock 0x104) at 48 kHz x 32 bits x 2 = 3.072 MHz (ar_afe_on())
 *   - "the stream": one graph of two sub-graphs. The stream's (0x4001, a container of capability CD): the shared memory
 *     endpoint we write into (0x6001) -> the PCM decoder (0x6002, our 16-bit interleaved samples) -> the PCM converter
 *     (0x6003, out as 32 bits deinterleaved). The device's (0x4002, capability EP): the I2S sink (0x6004) on the tertiary
 *     interface, data line 0, word select made here, 2 channels of 32 bits at 48 kHz. Opened, configured, our periods
 *     mapped (the 4K pool), prepared and started (ar_stream_on()); each period is a "write shared memory buffer" to 0x6001,
 *     whose "done" comes back to the events' handler with our token (ar_period_write(), ar_events()).
 * Every command with its payload in band: an APM command header (address 0, 0, map handle 0, the payload's size) and
 * parameters, each a header (module instance, parameter id, size, error) and its data, padded to 8 bytes.
 */
#define AR_APM                  1u
#define AR_PRM                  2u
#define AR_BASIC_RSP            0x02001005u
#define AR_GRAPH_OPEN           0x01001000u
#define AR_GRAPH_PREPARE        0x01001001u
#define AR_GRAPH_START          0x01001002u
#define AR_GRAPH_STOP           0x01001003u
#define AR_GRAPH_CLOSE          0x01001004u
#define AR_SET_CFG              0x01001006u
#define AR_MAP                  0x0100100cu
#define AR_UNMAP                0x0100100du
#define AR_MAP_RSP              0x02001001u
#define AR_PRM_REQUEST          0x0100100fu
#define AR_PRM_RELEASE          0x01001010u
#define AR_WRITE                0x04001000u
#define AR_WRITE_DONE           0x05001000u
#define AR_WRITE_V2             0x0400100au
#define AR_WRITE_DONE_V2        0x05001004u
#define AR_SG_STREAM            0x00004001u
#define AR_SG_DEVICE            0x00004002u
#define AR_CONT_STREAM          0x00004001u
#define AR_CONT_DEVICE          0x00004002u
#define AR_WRSH                 0x00006001u
#define AR_PCMDEC               0x00006002u
#define AR_PCMCNV               0x00006003u
#define AR_I2S                  0x00006004u

static uint32_t ar_packet[384];
static unsigned ar_at;                  /* words written into ar_packet */
static bool ar_core_on, ar_clock_on, ar_graph_open, ar_started;

static void
ar_word(uint32_t word)
{
	if (ar_at < sizeof(ar_packet) / 4) {
		ar_packet[ar_at++] = word;
	}
}

/* A parameter's header; its size is filled in by ar_param_end(), which pads it to 8 bytes. Returns where it begins. */
static unsigned
ar_param(uint32_t instance, uint32_t id)
{
	unsigned at = ar_at;

	ar_word(instance);
	ar_word(id);
	ar_word(0);
	ar_word(0);
	return at;
}

static void
ar_param_end(unsigned at)
{
	if ((ar_at - at) & 1) {
		ar_word(0);
	}
	ar_packet[at + 2] = (ar_at - at - 4) * 4;
}

static void
ar_begin(bool command_header)
{
	ar_at = 0;
	bzero(ar_packet, sizeof(ar_packet));
	if (command_header) {
		ar_word(0);
		ar_word(0);
		ar_word(0);
		ar_word(0);
	}
}

/* The packet sent to `port', its answer waited for: true if it is the basic answer with status 0, or `wanted' (then in *answer). */
static bool
ar_send(const char *what, uint32_t port, uint32_t opcode, bool command_header, uint32_t wanted, struct hakios_apr_answer *answer)
{
	struct hakios_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	if (command_header) {
		ar_packet[3] = (ar_at - 4) * 4;
	}
	snprintf(last_write, sizeof(last_write), "sent %s (opcode %#x)", what, opcode);
	if (!hakios_adsp_gpr(port, opcode, 0, ar_packet, ar_at * 4, APR_WAIT_MS, answer)) {
		say("sound: %s (opcode %#x to port %#x): NO ANSWER\n", what, opcode, port);
		return false;
	}
	if (answer->opcode == AR_BASIC_RSP && answer->words[0] == opcode) {
		say("sound: %s: %s (%#x)\n", what, answer->words[1] == 0 ? "done" : "REFUSED", answer->words[1]);
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		say("sound: %s: done\n", what);
		return true;
	}
	say("sound: %s: an answer not expected: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
	    answer->words[0], answer->words[1]);
	return false;
}

static bool
ar_prm(bool request, uint32_t param, const uint32_t *words, unsigned count, const char *what)
{
	unsigned p;

	ar_begin(true);
	p = ar_param(AR_PRM, param);
	for (unsigned i = 0; i < count; i++) {
		ar_word(words[i]);
	}
	ar_param_end(p);
	return ar_send(what, AR_PRM, request ? AR_PRM_REQUEST : AR_PRM_RELEASE, true, request ? 0x02001002u : 0x02001003u, NULL);
}

static bool
ar_afe_on(void)
{
	uint32_t core[1] = { 1 }, clock[5] = { 1, bus_tdm.on ? CLOCK_TER_TDM_IBIT : CLOCK_TER_MI2S_IBIT, RATE * 32 * (bus_tdm.on ? bus_tdm.nslots : 2u), 1, 0 };

	announce("the DSP's resource manager is asked for the LPASS core and the tertiary MI2S bit clock (PRM)");
	ar_core_on = ar_prm(true, 0x08001032u, core, 1, "the LPASS core");
	ar_clock_on = ar_core_on && ar_prm(true, 0x0800102cu, clock, 5, "the tertiary bit clock");
	return ar_clock_on;
}

static void
ar_afe_off(void)
{
	uint32_t core[1] = { 1 }, clock[2] = { 1, bus_tdm.on ? CLOCK_TER_TDM_IBIT : CLOCK_TER_MI2S_IBIT };

	if (ar_clock_on) {
		(void)ar_prm(false, 0x0800102cu, clock, 2, "the bit clock given back");
		ar_clock_on = false;
	}
	if (ar_core_on) {
		(void)ar_prm(false, 0x08001032u, core, 1, "the LPASS core given back");
		ar_core_on = false;
	}
}

static void
ar_sub_graph(uint32_t id, uint32_t perf, uint32_t direction)
{
	ar_word(id);
	ar_word(3);
	ar_word(0x0800100eu); ar_word(4); ar_word(perf);           /* perf mode */
	ar_word(0x0800100fu); ar_word(4); ar_word(direction);      /* direction */
	ar_word(0x08001010u); ar_word(4); ar_word(1);              /* scenario: playback */
}

static void
ar_container(uint32_t id, uint32_t capability)
{
	ar_word(id);
	ar_word(4);
	ar_word(0x08001011u); ar_word(8); ar_word(1); ar_word(capability);   /* capability list */
	ar_word(0x08001012u); ar_word(4); ar_word(1);              /* graph position: stream */
	ar_word(0x08001013u); ar_word(4); ar_word(8192);           /* stack */
	ar_word(0x08001014u); ar_word(4); ar_word(2);              /* processor domain: ADSP */
}

static void
ar_prop(uint32_t instance, uint32_t inputs, uint32_t outputs)
{
	ar_word(instance);
	ar_word(1);
	ar_word(0x08001015u);
	ar_word(8);
	ar_word(inputs);
	ar_word(outputs);
}

static void
ar_connection(uint32_t from, uint32_t to)
{
	ar_word(from);
	ar_word(1);             /* the first output port */
	ar_word(to);
	ar_word(2);             /* the first input port */
}

static bool
ar_open(void)
{
	unsigned p;

	ar_begin(true);
	p = ar_param(AR_APM, 0x08001001u);              /* sub-graphs */
	ar_word(2);
	ar_sub_graph(AR_SG_STREAM, 2, 2);               /* (the stream: low latency, direction RX) */
	ar_sub_graph(AR_SG_DEVICE, 2, 1);               /* (low latency, direction 1, as the topology has them) */
	ar_param_end(p);
	p = ar_param(AR_APM, 0x08001000u);              /* containers */
	ar_word(2);
	ar_container(AR_CONT_STREAM, 2);                /* CD */
	ar_container(AR_CONT_DEVICE, 3);                /* EP */
	ar_param_end(p);
	p = ar_param(AR_APM, 0x08001002u);              /* modules, by sub-graph and container */
	ar_word(2);
	ar_word(AR_SG_STREAM); ar_word(AR_CONT_STREAM); ar_word(3);
	ar_word(0x07001000u); ar_word(AR_WRSH);         /* WR_SHARED_MEM_EP */
	ar_word(0x07001005u); ar_word(AR_PCMDEC);       /* PCM_DEC */
	ar_word(0x07001003u); ar_word(AR_PCMCNV);       /* PCM_CNV */
	ar_word(AR_SG_DEVICE); ar_word(AR_CONT_DEVICE); ar_word(1);
	ar_word(bus_tdm.on ? 0x0700100eu : 0x0700100au); ar_word(AR_I2S);       /* TDM_SINK or I2S_SINK */
	ar_param_end(p);
	p = ar_param(AR_APM, 0x08001003u);              /* the modules' ports */
	ar_word(4);
	ar_prop(AR_WRSH, 0, 1);
	ar_prop(AR_PCMDEC, 1, 1);
	ar_prop(AR_PCMCNV, 1, 1);
	ar_prop(AR_I2S, 1, 0);
	ar_param_end(p);
	p = ar_param(AR_APM, 0x08001004u);              /* connections */
	ar_word(3);
	ar_connection(AR_WRSH, AR_PCMDEC);
	ar_connection(AR_PCMDEC, AR_PCMCNV);
	ar_connection(AR_PCMCNV, AR_I2S);
	ar_param_end(p);
	return ar_send("the graph, opened: shared memory -> PCM decoder -> converter | I2S sink (tertiary MI2S)", AR_APM, AR_GRAPH_OPEN,
	           true, 0, NULL);
}

/* PARAM_ID_PCM_OUTPUT_FORMAT_CFG of a decoder or converter: PCM, `bits', 2 channels, left and right */
static void
ar_pcm_output(uint32_t instance, uint16_t bits, uint16_t interleave)
{
	unsigned p = ar_param(instance, 0x08001008u);

	ar_word(1);                             /* fixed point */
	ar_word(0x09001000u);                   /* PCM */
	ar_word(20);                            /* the payload's bytes: 8 halves and 2 channels' map, to 4 (as Linux) */
	ar_word((uint32_t)bits | 1u << 16);     /* bit width, LSB-aligned */
	ar_word((uint32_t)bits | (uint32_t)(bits - 1) << 16);   /* bits per sample, q factor */
	ar_word(1u | (uint32_t)interleave << 16);               /* little endian, interleaving */
	ar_word(0u | 2u << 16);                 /* reserved, 2 channels */
	ar_word(1u | 2u << 8);                  /* left, right */
	ar_param_end(p);
}

static bool
ar_configure(void)
{
	unsigned p;

	/* the shared memory endpoint's input: our 16-bit interleaved stereo at 48 kHz */
	ar_begin(true);
	p = ar_param(AR_WRSH, 0x0800100cu);
	ar_word(1);
	ar_word(0x09001000u);
	ar_word(20);
	ar_word(RATE);
	ar_word(16u | 1u << 16);                /* bit width 16, LSB-aligned */
	ar_word(16u | 15u << 16);               /* 16 bits a sample, q15 */
	ar_word(1u | 2u << 16);                 /* little endian, 2 channels */
	ar_word(1u | 2u << 8);                  /* left, right */
	ar_param_end(p);
	if (!ar_send("the shared memory endpoint's format: 2 channels of 16 bits at 48 kHz", AR_WRSH, AR_SET_CFG, true, 0, NULL)) {
		return false;
	}
	ar_begin(true);
	ar_pcm_output(AR_PCMDEC, 16, 1);        /* interleaved */
	ar_pcm_output(AR_PCMCNV, 32, 3);        /* deinterleaved, unpacked */
	if (!ar_send("the decoder and the converter: out as 16 bits interleaved, then 32 bits", AR_APM, AR_SET_CFG, true, 0, NULL)) {
		return false;
	}
	ar_begin(true);
	if (bus_tdm.on) {
		p = ar_param(AR_I2S, 0x0800101bu);      /* PARAM_ID_TDM_INTF_CFG (AudioReach pcm_tdm_api.h) */
		ar_word(0);                             /* LPAIF */
		ar_word(2);                             /* tertiary */
		ar_word(1u | 1u << 16);                 /* sync made here; data out enabled */
		ar_word(bus_tdm.mask);
		ar_word((uint32_t)bus_tdm.nslots | 32u << 16);  /* slots a frame, 32 bits each */
		ar_word((uint32_t)bus_tdm.sync_mode | (uint32_t)bus_tdm.invert << 16);
		ar_word(bus_tdm.delay);                 /* delay; reserved */
		ar_param_end(p);
	} else {
		p = ar_param(AR_I2S, 0x08001019u);      /* PARAM_ID_I2S_INTF_CFG */
		ar_word(0);                             /* LPAIF */
		ar_word(2);                             /* tertiary */
		{
			uint32_t line = 1;              /* boot-arg sound_sd=N: the data line (1: SD0, gpio122; 2: SD1, gpio124) */
	
			PE_parse_boot_argn("sound_sd", &line, sizeof(line));
			ar_word((line & 0xffff) | 1u << 16);    /* the data line; word select made here */
		}
		ar_param_end(p);
	}
	p = ar_param(AR_I2S, 0x08001017u);      /* PARAM_ID_HW_EP_MF_CFG */
	ar_word(RATE);
	ar_word(32u | 2u << 16);                /* 32 bits, 2 channels */
	ar_word(1);                             /* fixed point */
	ar_param_end(p);
	p = ar_param(AR_I2S, 0x08001018u);      /* PARAM_ID_HW_EP_FRAME_SIZE_FACTOR */
	ar_word(1);
	ar_param_end(p);
	return ar_send("the I2S sink: tertiary MI2S, data line 0, 2 channels of 32 bits at 48 kHz", AR_APM, AR_SET_CFG, true, 0, NULL);
}

static bool
ar_sub_graphs(const char *what, uint32_t opcode)
{
	unsigned p;

	ar_begin(true);
	p = ar_param(AR_APM, 0x08001005u);      /* APM_PARAM_ID_SUB_GRAPH_LIST */
	ar_word(2);
	ar_word(AR_SG_STREAM);
	ar_word(AR_SG_DEVICE);
	ar_param_end(p);
	return ar_send(what, AR_APM, opcode, true, 0, NULL);
}

static bool
ar_stream_on(void)
{
	struct hakios_apr_answer answer;

	announce("the DSP is given a graph: our periods -> PCM decoder -> converter -> the tertiary MI2S (AudioReach)");
	ar_graph_open = ar_open();
	if (!ar_graph_open || !ar_configure()) {
		return false;
	}
	ar_begin(false);
	ar_word(3u | 1u << 16);                 /* the 4K pool, one region */
	ar_word(0);
	ar_word((uint32_t)buffer_address);
	ar_word((uint32_t)(buffer_address >> 32));
	ar_word(BUFFER_BYTES);
	if (!ar_send("our periods, mapped for the DSP", AR_APM, AR_MAP, false, AR_MAP_RSP, &answer)) {
		return false;
	}
	mapped = true;
	map_handle = answer.words[0];
	say("sound: our %u KB at %#llx are the DSP's handle %#x\n", BUFFER_BYTES / 1024, buffer_address, map_handle);
	stream_open = ar_sub_graphs("the graph, prepared", AR_GRAPH_PREPARE) &&
	    ar_sub_graphs("the graph, started", AR_GRAPH_START);
	ar_started = stream_open;
	return stream_open;
}

static void
ar_stream_off(void)
{
	if (ar_started) {
		(void)ar_sub_graphs("the graph, stopped", AR_GRAPH_STOP);
		ar_started = false;
	}
	stream_open = false;
	if (ar_graph_open) {
		(void)ar_sub_graphs("the graph, closed", AR_GRAPH_CLOSE);
		ar_graph_open = false;
	}
	if (mapped) {
		ar_begin(false);
		ar_word(map_handle);
		(void)ar_send("our periods, unmapped", AR_APM, AR_UNMAP, false, 0, NULL);
		mapped = false;
	}
}

static bool
ar_period_write(unsigned period)
{
	/* DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 (this firmware answers the first version "unsupported"): no metadata buffer */
	uint32_t write[11] = { (uint32_t)(buffer_address + 4096 * period), (uint32_t)((buffer_address + 4096 * period) >> 32),
		               map_handle, PERIOD_BYTES, 0, 0, 0, 0, 0, 0, 0 };

	return hakios_adsp_gpr(AR_WRSH, AR_WRITE_V2, WRITE_TOKEN | period, write, sizeof(write), 0, NULL);
}

static bool
ar_events(__unused uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if ((opcode == AR_WRITE_DONE || opcode == AR_WRITE_DONE_V2) && (token & WRITE_TOKEN)) {
		if (length >= 16 && payload[3] != 0) {
			OSIncrementAtomic((volatile SInt32 *)&write_errors);
		}
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (opcode == AR_BASIC_RSP && (token & WRITE_TOKEN)) {
		OSIncrementAtomic((volatile SInt32 *)&write_errors);
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (port == AR_WRSH || port == AR_I2S) {
		static unsigned told;

		if (told++ < 10) {
			say("sound: from module %#x: opcode %#x, token %#x, %u bytes: %#x %#x\n", port, opcode, token, length,
			    length >= 4 ? payload[0] : 0, length >= 8 ? payload[1] : 0);
		}
		return true;
	}
	return false;
}
#endif

/* ---- sound=4: what Darwin plays ------------------------------------------------------------------------------ */

/*
 * /dev/hakios-audio's writer (the audio server's real-time thread, hakios.audio.c) puts frames
 * into a ring and does not wait; the feeder, a thread of ours, takes them from there into each
 * period the DSP gives back (and silence when there is nothing), so the stream never stops
 * while the run lasts. The amplifiers are only on while there is sound: the watcher starts them
 * (and fades them in to sound_below) when a write is not silence, and stops them after three
 * seconds of silence or no writes.
 */
#define RING_FRAMES             32768           /* 0.68 s; a power of two, the indices run on and wrap */
#define SILENCE                 8               /* samples no larger than this are silence (as hakios.audio.c's) */
#ifdef HAKIOS_ADRENO7
/*
 * The Xiaomi 12S: started again after a stop, an amplifier often does not lock and needs setting up from the beginning (a gap
 * or a lost sound): kept running through 30 s of silence instead of three seconds (not longer: in silence they still read
 * 55 to 63 degrees, the phone warm round them), so that sounds close together never wait for a start.
 */
#define QUIET_BEFORE_OFF_US     30000000ull
#else
#define QUIET_BEFORE_OFF_US     3000000ull
#endif

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;  /* frames: written by the sink, taken by the feeder */
static volatile uint64_t last_sound_us;         /* when the latest write that was not silence came */
static volatile bool feeder_runs, watcher_runs;
static uint64_t fed_frames, silent_frames, darwin_frames;
static volatile uint32_t darwin_peak;              /* the largest sample since the amplifiers last started */

/* From the writer's thread, under hakios.audio.c's lock: never waits. */
static uint32_t
darwin_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t left = frames[2 * i], right = frames[2 * i + 1];
		uint32_t l = left < 0 ? (uint32_t)-(int32_t)left : (uint32_t)left, r = right < 0 ? (uint32_t)-(int32_t)right : (uint32_t)right;

		ring[2 * ((head + i) % RING_FRAMES)] = left;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = right;
		sound |= l > SILENCE || r > SILENCE;
		if (l > darwin_peak || r > darwin_peak) {
			darwin_peak = l > r ? l : r;
		}
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	darwin_frames += taken;
	if (sound) {
		last_sound_us = now_us();
	}
	return taken;
}

static uint32_t guard_periods;                  /* periods through the guard */
static volatile bool guard_on;                  /* the amplifiers are on: what the guard lets through is heard */

/* tenths of a dB as text: "-5.7" */
static const char *
db_text(char *text, int tenths)
{
	snprintf(text, 16, "%s%d.%d", tenths < 0 ? "-" : "", (tenths < 0 ? -tenths : tenths) / 10, (tenths < 0 ? -tenths : tenths) % 10);
	return text;
}

/* What the guard has done since it was last said (hakios.guard.c), as one line; nothing if `quiet'. */
static void
guard_say(const char *when, bool quiet)
{
	struct hakios_guard_report r;
	char t[6][16];

	hakios_guard_take_report(&r);
	if (r.blocks == 0 || quiet) {
		return;
	}
	/* (levels at the speakers: dB under a full-scale square wave at Android's level; gains in dB) */
	say("sound: the guard%s: peaks in %u, out %u (of 32768); gain now %s dB, least %s dB, held down by the 0.2 s/3 s/60 s "
	    "averages in %u/%u/%u of %u periods; peaks limited in %u frames (by %s dB at most); averages %s/%s/%s dB\n",
	    when, r.peak_in, r.peak_out, db_text(t[0], hakios_guard_decibels(r.gain * r.gain >> 30)),
	    db_text(t[1], hakios_guard_decibels(r.least_gain * r.least_gain >> 30)), r.rms_blocks, r.fast_blocks, r.slow_blocks,
	    r.blocks, r.limited_frames, db_text(t[2], hakios_guard_decibels((uint64_t)r.least_peak_gain * r.least_peak_gain >> 2)),
	    db_text(t[3], hakios_guard_decibels(r.rms)), db_text(t[4], hakios_guard_decibels(r.fast)),
	    db_text(t[5], hakios_guard_decibels(r.slow)));
}

/* A period from the ring, the rest of it silence. */
static void
period_fill_darwin(unsigned period)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);
	uint32_t tail = ring_tail, available = ring_head - tail, n = available < PERIOD_FRAMES ? available : PERIOD_FRAMES;

	OSMemoryBarrier();
	for (uint32_t i = 0; i < n; i++) {
		samples[2 * i] = ring[2 * ((tail + i) % RING_FRAMES)];
		samples[2 * i + 1] = ring[2 * ((tail + i) % RING_FRAMES) + 1];
	}
	bzero(samples + 2 * n, (PERIOD_FRAMES - n) * 4);
	OSMemoryBarrier();
	ring_tail = tail + n;
	fed_frames += n;
	silent_frames += PERIOD_FRAMES - n;
	/* the guard, on every period, silence too: its filter and its averages go on (hakios.guard.c) */
	hakios_guard_process(samples, PERIOD_FRAMES);
	if (n != 0 && hakios_audio_recording_fed()) {
		hakios_audio_record_fed(samples, n);
	}
	guard_periods++;
	for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

static void
feeder_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t sent = 0, failed = 0;

	returned = 0;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill_darwin(p);
		sent += period_write(p);
	}
	while (!stopping) {
		uint32_t back = returned;
		static bool was_on;

		if (guard_on && !was_on) {
			guard_say("", true);    /* (what it did to the silence meanwhile is of no interest) */
			guard_periods = 0;
		} else if (was_on && !guard_on) {
			guard_say(" until the amplifiers stopped", false);
		} else if (guard_on && guard_periods >= 250) {
			guard_say(", 5 s", false);
			guard_periods = 0;
		}
		was_on = guard_on;
		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			for (unsigned p = 0; p < PERIODS; p++) {
				if (back & 1u << p) {
					period_fill_darwin(p);
					if (period_write(p)) {
						sent++;
					} else {
						failed++;
					}
				}
			}
		}
		sleep_ms(2);
	}
	say("sound: the feeder stops: %u periods written (%u could not be sent), %u given back (%u with an error), %u underflows; "
	    "from Darwin %llu frames came, %llu went to the DSP, %llu of silence filled in\n", sent, failed, written_done, write_errors,
	    underflows, darwin_frames, fed_frames, silent_frames);
	feeder_runs = false;
	thread_terminate(current_thread());
}

/* The amplifiers' level as the guard is to reckon with it (sound_guard_as: louder than it is, for a test). */
static void
guard_level(uint32_t level_below)
{
	uint32_t as = level_below < guard_as ? level_below : guard_as;

	/* (HAKIOS_GUARD_SILENT means "the amplifiers are off": a level must never be taken for it, it would turn the power limits off) */
	hakios_guard_attenuation(as >= HAKIOS_GUARD_SILENT ? HAKIOS_GUARD_SILENT - 1 : as);
}

/*
 * sound_below and the volume Darwin has set (`extra', eighths of a dB under it), added up: saturating, so that a
 * huge sound_below or `extra' cannot wrap the sum round to a small number, that is to a loud level.
 */
static uint32_t
below_with(uint32_t extra)
{
	uint64_t total = (uint64_t)below + extra;

	return total > HAKIOS_GUARD_SILENT - 1 ? HAKIOS_GUARD_SILENT - 1 : (uint32_t)total;
}

/*
 * The amplifiers are off. The guard goes on at their level all the same: what it lets through now reaches them
 * 160 ms later, behind the periods queued at the DSP, and if that is sound, they will have been started by then
 * (70 ms after it comes). While they are off nothing but silence comes, and the averages fall as a speaker cools.
 * Only when nothing can be heard at all -- Darwin's volume at 0, the amplifiers given up on -- is the guard told so.
 */
static void
guard_off(bool unheard)
{
	guard_on = false;
	if (unheard) {
		hakios_guard_attenuation(HAKIOS_GUARD_SILENT);
	}
}

/*
 * The amplifiers on while there is sound, at the volume Darwin has set (hakios_audio_volume(): how
 * far under the loudest, sound_below under Android's level), and off while Darwin's volume is 0;
 * their status looked at every 200 ms while they are on. On the sound's thread.
 */
#ifdef HAKIOS_ADRENO7
/*
 * The Xiaomi 12S: started again after a stop, an amplifier may not lock (status 0, every read of it): set up from the beginning, as at the
 * first start, it does. A soft reset and the profile are some 50 ms each. Tries three times; the next sound tries again.
 */
static const uint8_t *amps_acf;
static uint64_t amps_acf_size;

static bool
amps_up(uint32_t level_below)
{
	for (unsigned tries = 0; tries < 3; tries++) {
		if (tries != 0) {
			say("sound: the amplifiers are set up again (try %u)\n", tries + 1);
			sleep_ms(20);
			if (amps_acf == NULL || !hakios_speaker_prepare(amps_acf, amps_acf_size)) {
				continue;
			}
		}
		if (hakios_speaker_start() && hakios_speaker_volume(level_below, 1)) {
			return true;
		}
	}
	return false;
}
#else
#define amps_up(level_below) (hakios_speaker_start() && hakios_speaker_volume(level_below, 1))
#endif

static void
watch(void)
{
	bool on = false, given_up = false, muted = false;
	uint64_t last_look = 0, last_monitor = 0;
	uint32_t starts = 0, changes = 0, generation = 0, extra = 0, monitor = 0, monitor_looks = 0;
	uint32_t faults = 0;				/* (counted on the Xiaomi 12S: see below) */
	(void)faults;

	guard_level(below);                     /* (from the first sample on: see guard_off()) */

	while (!stopping) {
		uint64_t now = now_us(), sounded = last_sound_us;
		uint32_t extra_now, generation_now;
		bool muted_now;

		if (hakios_audio_volume(&extra_now, &muted_now, &generation_now) && generation_now != generation) {
			generation = generation_now;
			extra = extra_now;
			muted = muted_now;
			changes++;
			say("sound: Darwin sets the volume: %s%u.%03u dB under the loudest, which is %u.%03u dB under Android's level%s\n",
			    muted ? "muted (0), " : "", extra / 8, extra % 8 * 125, below / 8, below % 8 * 125,
			    on ? (muted ? ": the amplifiers are stopped" : ": the amplifiers follow") : "");
			if (muted) {
				guard_off(true);
				if (on) {
					hakios_speaker_stop();
					on = false;
				}
			} else if (on && !hakios_speaker_volume(below_with(extra), 1)) {
				guard_off(true);
				on = false;
				given_up = true;
			} else if (!given_up) {
				guard_level(below_with(extra));
			}
		}
		if (!on && !given_up && !muted && sounded != 0 && now - sounded < 100000) {
			say_when("Darwin plays: the amplifiers are started");
			starts++;
			darwin_peak = 0;
			if (!amps_up(below_with(extra))) {
#ifdef HAKIOS_ADRENO7
				say("sound: THE AMPLIFIERS DID NOT START; the next sound tries again\n");
				guard_off(true);
				sleep_ms(1000);
#else
				say("sound: THE AMPLIFIERS DID NOT START; Darwin's sound goes on to the DSP, unheard\n");
				guard_off(true);
				given_up = true;
#endif
			} else {
				on = true;
				guard_on = true;
				last_look = now_us();
				last_monitor = 0;
				monitor_looks = 0;
				/* (what is written now is heard after the periods already queued at the DSP: PERIODS x 20 ms) */
				say("sound: the amplifiers are up %llu ms after the first write that was not silence; that write reaches "
				    "them %u ms after it was made, behind the periods queued at the DSP\n", (last_look - sounded) / 1000,
				    PERIODS * PERIOD_FRAMES / 48);
			}
		} else if (on && now - sounded > QUIET_BEFORE_OFF_US) {
			say_when("three seconds of silence: the amplifiers are stopped");
			say("sound: what Darwin wrote while they were on peaked at %u%% of full scale (%u)\n", darwin_peak * 100 / 32768,
			    darwin_peak);
			guard_off(false);
			hakios_speaker_stop();
			on = false;
		} else if (on && now - last_look >= 200000) {
			last_look = now;
			if (!hakios_speaker_healthy()) {
				guard_off(true);
				on = false;
#ifdef HAKIOS_ADRENO7
				/* the Xiaomi 12S: powered down (speaker.c), then set up again by the next sound; given up after five faults */
				if (++faults >= 5) {
					say("sound: five faults: the amplifiers are given up on\n");
					given_up = true;
				} else {
					say("sound: a fault (%u of 5): the next sound sets the amplifiers up again\n", faults);
					sleep_ms(1000);
				}
#else
				given_up = true;
#endif
			} else if (now - last_monitor >= 1000000) {
				/* the amplifiers' temperature and the battery as they see it, every second: a reduction if Android's table asks for one */
				uint32_t wanted;

				last_monitor = now;
				wanted = hakios_speaker_monitor(monitor_looks++ % 10 == 0);
				if (wanted != monitor) {
					say("sound: the amplifiers' watch asks for %u.%03u dB less (it was %u.%03u)\n", wanted / 8, wanted % 8 * 125,
					    monitor / 8, monitor % 8 * 125);
					monitor = wanted;
					hakios_guard_monitor(monitor);
				}
			}
		}
		sleep_ms(5);
	}
	guard_off(true);
	say("sound: the watcher stops; the amplifiers were started %u times, Darwin set the volume %u times%s\n", starts, changes,
	    given_up ? ", and the amplifiers were then given up on" : "");
	watcher_runs = false;
}

/* ---- the whole ---------------------------------------------------------------------------------------- */

static bool running, taken_down;

static void
take_down(void)
{
	stopping = true;
	if (level >= 4) {
		hakios_audio_set_sink("nothing", NULL);
	}
	for (unsigned ms = 0; ms < 3000 && (feeder_runs || watcher_runs); ms++) {
		sleep_ms(1);
	}
	lck_mtx_lock(&sound_lock);
	if (running && !taken_down) {
		taken_down = true;
		hakios_speaker_stop();
		stream_off();
		afe_off();
		pins_off();
		lck_mtx_lock(&hakios_smmu_lock);
		shared_put(0);
		lck_mtx_unlock(&hakios_smmu_lock);
		hakios_adsp_apr_events(NULL);
		say("sound: everything of the sound's is stopped and put back\n");
	}
	lck_mtx_unlock(&sound_lock);
}

#ifdef HAKIOS_ADRENO7
/* sound_matrix=2: the TDM sink, Android's rate, every layout of the frame still in question; the bus built anew for each. */
static bool
sound_matrix_tdm(const uint8_t *acf, uint64_t acf_size, uint32_t below)
{
	static const uint16_t slots[] = { 4, 2 };
	static const uint16_t syncs[] = { 1, 0, 2 };    /* long, short (bit), short (slot) */
	static char table[48][96];
	unsigned rows = 0;
	bool found = false;

	matrix_runs = true;
	bus_rate = 96000;
	for (unsigned n = 0; n < 2 && !found && !stopping; n++) {
		for (unsigned y = 0; y < 3 && !found && !stopping; y++) {
			for (unsigned i = 0; i < 2 && !found && !stopping; i++) {
				for (unsigned d = 0; d < 2 && !found && !stopping; d++) {
					bool bus, started = false;

					stream_off();
					afe_off();
					bus_tdm.on = true;
					bus_tdm.nslots = slots[n];
					bus_tdm.mask = 3;
					bus_tdm.sync_mode = syncs[y];
					bus_tdm.invert = i == 0;
					bus_tdm.delay = d == 0;
					bus = afe_on() && stream_on();
					if (bus) {
						hakios_speaker_options(0, 0, 9, false, 0);
						started = hakios_speaker_prepare(acf, acf_size) && hakios_speaker_start() && hakios_speaker_volume(below, 1);
					}
					matrix_looked = false;
					matrix_status = 0;
					matrix_pvdd = 0;
					looks_during_tone = 0;
					if (started) {
						(void)play(1200, true);
					}
					found = (matrix_status & (1u << 8 | 1u << 9)) != 0;
					if (rows < 48) {
						snprintf(table[rows++], sizeof(table[0]), "tdm %u slots sync %u invert %u delay %u: %s status %04x pvdd %03x%s",
						    bus_tdm.nslots, bus_tdm.sync_mode, bus_tdm.invert, bus_tdm.delay,
						    !bus ? "BUS REFUSED" : started ? "ran" : "FAILED TO START", matrix_status, matrix_pvdd, found ? " <== SWITCHES" : "");
						say("sound: matrix: %s\n", table[rows - 1]);
					}
					if (found) {
						say("sound: THE OUTPUT STAGE SWITCHES WITH THIS LAYOUT: five more seconds of tone\n");
						(void)play(5000, true);
					}
					hakios_speaker_stop();
				}
			}
		}
	}
	matrix_runs = false;
	say("sound: MATRIX DONE, %u tried, %s\n", rows, found ? "ONE SWITCHES" : "none switches");
	for (unsigned i = 0; i < rows; i++) {
		say("sound: table: %s\n", table[i]);
	}
	return true;
}

/*
 * sound_matrix=1 (with sound=3): every combination still in question, one after the other in one run, a short tone each and the
 * amplifiers' status during it, a table at the end. SWS or BSTS seen: said loudly, that one held for three more seconds, done.
 */
static bool
sound_matrix(const uint8_t *acf, uint64_t acf_size, uint32_t below)
{
	static const struct { uint32_t bus; int amp; } rates[] = { { 96000, 9 }, { 48000, 8 }, { 48000, 9 } };
	static const unsigned profiles[] = { 0, 6 };
	static const struct { uint32_t only, spksw; const char *name; } amps[] = {
		{ 0x35, 0, "bottom" }, { 0x34, 0, "top" }, { 0, 0, "both" },
	};
	static char table[48][96];
	unsigned rows = 0;
	bool found = false;

	matrix_runs = true;
	for (unsigned r = 0; r < 3 && !found && !stopping; r++) {
		if (bus_rate != rates[r].bus) {
			stream_off();
			afe_off();
			bus_rate = rates[r].bus;
			if (!afe_on() || !stream_on()) {
				say("sound: matrix: the bus does not come up at %u\n", bus_rate);
				break;
			}
		}
		for (unsigned p = 0; p < 2 && !found && !stopping; p++) {
			for (unsigned force = 0; force < 2 && !found && !stopping; force++) {
				for (unsigned a = 0; a < 3 && !found && !stopping; a++) {
					bool started;

					hakios_speaker_options(amps[a].only, amps[a].spksw, rates[r].amp, force != 0, profiles[p]);
					started = hakios_speaker_prepare(acf, acf_size) && hakios_speaker_start() && hakios_speaker_volume(below, 1);
					matrix_looked = false;
					matrix_status = 0;
					matrix_pvdd = 0;
					looks_during_tone = 0;
					if (started) {
						(void)play(1200, true);
					}
					found = (matrix_status & (1u << 8 | 1u << 9)) != 0;
					if (rows < 48) {
						snprintf(table[rows++], sizeof(table[0]), "bus %u amp-rate %d profile %u force %u %s: %s status %04x pvdd %03x%s",
						    rates[r].bus, rates[r].amp, profiles[p], force, amps[a].name, started ? "ran" : "FAILED TO START",
						    matrix_status, matrix_pvdd, found ? " <== SWITCHES" : "");
						say("sound: matrix: %s\n", table[rows - 1]);
					}
					if (found) {
						say("sound: THE OUTPUT STAGE SWITCHES WITH THIS COMBINATION: three more seconds of tone\n");
						(void)play(3000, true);
					}
					hakios_speaker_stop();
				}
			}
		}
	}
	matrix_runs = false;
	say("sound: MATRIX DONE, %u tried, %s\n", rows, found ? "ONE SWITCHES" : "none switches");
	for (unsigned i = 0; i < rows; i++) {
		say("sound: table: %s\n", table[i]);
	}
	return true;
}
#endif

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	const uint8_t *acf = NULL;
	uint64_t acf_size = 0;
	uint32_t changes, ones, sync_changes, sync_ones;
	bool ok = false;

	lck_mtx_lock(&sound_lock);
	if (taken_down) {
		lck_mtx_unlock(&sound_lock);
		thread_terminate(current_thread());
	}
	if (level == 4) {
		say("sound: level 4: what Darwin plays, through the port, a stream and the amplifiers, until the run ends\n");
	} else {
		say("sound: level %u: %s, for %u s\n", level, level == 1 ? "the speakers' TDM port" : level == 2 ?
		    "the port and a stream with a test tone, the amplifiers off" :
		    "the port, a stream with a test tone, and the amplifiers", seconds);
	}
	if (level >= 3) {
		acf = hakios_adsp_fw_file("aw882xx_acf.bin", &acf_size);
		if (acf == NULL) {
			say("sound: no /fw/aw882xx_acf.bin: put the phone's /vendor/etc/aw882xx_acf.bin into fw-audio/ and run SYSTEM=1 "
			    "userland/build.sh\n");
			goto out;
		}
	}
	if (level >= 2) {
		lck_mtx_lock(&hakios_smmu_lock);
		ok = shared_get(0);
		lck_mtx_unlock(&hakios_smmu_lock);
		if (!ok) {
			goto out;
		}
		ok = false;
		hakios_adsp_apr_events(events);
	}
	announce("the TDM pins to the interface, as Android's active state has them");
	pins_on();
	if (!afe_on()) {
		goto out;
	}
	sleep_ms(20);
	pin_activity(tdm_pins[0].pin, tdm_pins[0].name, &changes, &ones);
	pin_activity(tdm_pins[1].pin, tdm_pins[1].name, &sync_changes, &sync_ones);
	say("sound: %s\n", changes > 100 && sync_changes > 4 ? "THE TDM CLOCK AND FRAME SYNC RUN at the pins" :
	    "THE PINS DO NOT SHOW A RUNNING TDM CLOCK");
	if (level == 1) {
		sleep_ms(seconds * 1000);
		ok = true;
		goto out;
	}
	if (!stream_on()) {
		goto out;
	}
	if (level == 4) {
		thread_t feeder;

		announce("the amplifiers, set up from Android's Music profile, powered down until Darwin plays");
		if (!hakios_speaker_prepare(acf, acf_size)) {
			goto out;
		}
#ifdef HAKIOS_ADRENO7
		amps_acf = acf;         /* (kept: amps_up() may need it again) */
		amps_acf_size = acf_size;
#else
		hakios_adsp_fw_free(acf, acf_size);
#endif
		acf = NULL;
		feeder_runs = true;
		if (kernel_thread_start(feeder_thread, NULL, &feeder) != KERN_SUCCESS) {
			feeder_runs = false;
			say("sound: no thread for the feeder\n");
			goto out;
		}
		thread_deallocate(feeder);
		watcher_runs = true;
		hakios_audio_set_sink("the speakers (the audio DSP, hakios.sound.c)", darwin_sink);
		say("sound: WHAT DARWIN PLAYS GOES TO THE SPEAKERS from here on, %u.%03u dB under Android's level, until the run ends\n",
		    below / 8, below % 8 * 125);
		say("sound: through the guard (hakios.guard.c): %u.%03u dB of gain first (sound_gain), then high-pass at 200 Hz; peaks "
		    "3 dB, the 0.2 s/3 s/60 s power 6/9/12 dB under full scale at Android's level\n", gain / 8, gain % 8 * 125);
		if (guard_as < below) {
			say("sound: sound_guard_as=%u: FOR A TEST the guard reckons the amplifiers %u.%03u dB under Android's level, louder "
			    "than they are, and holds back as much as it would there\n", guard_as, guard_as / 8, guard_as % 8 * 125);
		}
		lck_mtx_unlock(&sound_lock);
		watch();
		thread_terminate(current_thread());
	}
#ifdef HAKIOS_ADRENO7
	{
		uint32_t matrix = 0;

		PE_parse_boot_argn("sound_matrix", &matrix, sizeof(matrix));
		if (matrix != 0 && level == 3) {
			ok = matrix == 2 ? sound_matrix_tdm(acf, acf_size, below) : sound_matrix(acf, acf_size, below);
			goto out;
		}
	}
#endif
	if (level >= 3) {
		/* the clock runs (the port is started, the stream as well): the amplifiers can lock to it */
		announce("the amplifiers, set up from Android's Music profile, then started at the lowest volume");
		if (!hakios_speaker_prepare(acf, acf_size) || !hakios_speaker_start()) {
			goto out;
		}
	}
	/* (the amplifiers are faded in before the tone begins: they fade in on silence, and the tone starts at that level) */
	if (level >= 3 && !hakios_speaker_volume(below, 50)) {
		goto out;
	}
	if (level >= 3) {
		say("sound: THE TONE PLAYS NOW for %u s: 1 kHz at -20 dBFS, both speakers, %u.%03u dB under Android's level\n",
		    seconds, below / 8, below % 8 * 125);
	}
	say_when("the tone begins");
	ok = play(seconds * 1000, level >= 3);
	say_when("the tone has ended");
	space_faults();
out:
	if (acf != NULL) {
		hakios_adsp_fw_free(acf, acf_size);
	}
	lck_mtx_unlock(&sound_lock);
	take_down();
	say("sound: %s; last: %s\n", ok ? "LEVEL DONE" : "STOPPED SHORT", last_write);
	thread_terminate(current_thread());
}

void
hakios_sound_dsp_up(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("sound", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 4) {
		level = 4;
	}
	PE_parse_boot_argn("sound_secs", &seconds, sizeof(seconds));
	PE_parse_boot_argn("sound_below", &below, sizeof(below));
	PE_parse_boot_argn("sound_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("sound_guard_as", &guard_as, sizeof(guard_as));
	PE_parse_boot_argn("sound_gain", &gain, sizeof(gain));
	if (gain > HAKIOS_GUARD_MAX_GAIN) {
		gain = HAKIOS_GUARD_MAX_GAIN;
	}
	hakios_guard_configure(true, gain);
	if (level >= 2 && level <= 3 && seconds > 3) {
		seconds = 3;            /* the test tone: three seconds at most */
	}
	running = true;
	if (kernel_thread_start(sound_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		say("sound: no thread\n");
		return;
	}
	thread_deallocate(thread);
}

void
hakios_sound_dsp_down(void)
{
	if (running) {
		take_down();
	}
}

void
hakios_sound_quiesce(void)
{
	hakios_speaker_quiesce();
}
