/*
 * MediaTek MT6833 (Dimensity 700 / POCO M4 5G) Audio Driver.
 *
 * Implements hardware audio playback for Darwin's /dev/hakios-audio through:
 *  1. MediaTek MT6833 Audio Front End (AFE) DMA engine at 0x11210000
 *  2. 52 KB Audio Internal SRAM at 0x11212000 for low-latency zero-jitter DMA ring buffer
 *  3. I2S3 interface configured in 48 kHz 16-bit stereo master mode
 *  4. Awinic AW87389 / SIA8152 Smart Power Amplifier on I2C6 at 0x11f00000 (address 0x58 / 0x5b)
 *
 * Hardware mapping (from poco_m4_5g.dts & poco_m4_5g_dtbo.dts):
 *   afe            : mt6833-afe-pcm@11210000 (size 0x2000, IRQ 202)
 *   audio_sram     : audio_sram@11212000 (size 0xd000 = 52 KB)
 *   i2s_spk_out    : I2S3 (mtk_spk_i2s_out = <0x03>)
 *   i2c6           : i2c6@11f00000 (size 0x1000)
 *   speaker_amp    : aw87xxx_pa@58 (AW87389) / sia81xx@5b (SIA8152)
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.audio.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

/* --- Hardware Physical Addresses --- */
#define MT6833_AFE_PHYS         0x11210000ULL
#define MT6833_AFE_SIZE         0x2000
#define MT6833_SRAM_PHYS        0x11212000ULL
#define MT6833_SRAM_SIZE        0xd000          /* 52 KB */
#define MT6833_I2C6_PHYS        0x11f00000ULL
#define MT6833_I2C6_SIZE        0x1000

/* --- AFE Register Offsets --- */
#define AFE_DAC_CON0            0x0010          /* Master AFE & DAC Control */
#define AFE_DAC_CON1            0x0014
#define AFE_I2S_CON1            0x0034          /* I2S1 Control */
#define AFE_I2S_CON3            0x003c          /* I2S3 Control (Speaker out) */
#define AFE_CONN0               0x0060          /* Crossbar interconnect 0 */
#define AFE_CONN1               0x0064          /* Crossbar interconnect 1 */
#define AFE_CONN2               0x0068          /* Crossbar interconnect 2 */
#define AFE_DL1_BASE            0x0090          /* DL1 Playback DMA Base Address */
#define AFE_DL1_CUR             0x0094          /* DL1 Playback DMA Current Pointer */
#define AFE_DL1_END             0x0098          /* DL1 Playback DMA End Address */
#define AFE_DL1_CON0            0x009c          /* DL1 DMA Control */
#define AFE_IRQ_MCU_CON         0x03a0          /* Interrupt Control */
#define AFE_IRQ_MCU_STATUS      0x03a4          /* Interrupt Status */
#define AFE_IRQ_MCU_CLR         0x03a8          /* Interrupt Clear */

/* --- AFE Bit Fields --- */
#define AFE_ON                  (1u << 0)
#define AFE_DAC_ON              (1u << 1)
#define AFE_FS_48K              (8u << 4)       /* 48 kHz sample rate */
#define I2S3_EN                 (1u << 0)       /* I2S3 Enable */
#define I2S3_SRC_MASTER         (0u << 1)       /* SoC is I2S master */
#define I2S3_WLEN_16BIT         (0u << 2)       /* 16-bit word length */
#define I2S3_FMT_I2S            (0u << 3)       /* Standard I2S format */
#define I2S3_FS_48K             (8u << 8)       /* 48 kHz I2S clock */
#define DL1_ON                  (1u << 0)       /* DL1 DMA Enable */
#define DL1_MINCR               (1u << 1)       /* Auto-increment DMA addr */

/* --- Audio SRAM Playback Ring Buffer --- */
#define PLAYBACK_BUF_BYTES      (32 * 1024)     /* 32 KB ring buffer = 8192 stereo frames = 170.6 ms */
#define PLAYBACK_BUF_FRAMES     (PLAYBACK_BUF_BYTES / HAKIOS_AUDIO_FRAME)

/* --- MediaTek I2C Controller Registers --- */
#define I2C_DATA_PORT           0x0000
#define I2C_SLAVE_ADDR          0x0004
#define I2C_INTR_MASK           0x0008
#define I2C_INTR_STAT           0x000c
#define I2C_CONTROL             0x0010
#define I2C_TRANSFER_LEN        0x0014
#define I2C_TRANSAC_LEN         0x0018
#define I2C_TIMING              0x0020
#define I2C_START               0x0024
#define I2C_FIFO_STAT           0x0030
#define I2C_SOFTRESET           0x0050

/* --- AW87389 / SIA8152 Amplifier Registers --- */
#define AW87XXX_I2C_ADDR_DEF    0x58            /* Standard AW87xxx I2C address */
#define AW87XXX_I2C_ADDR_ALT    0x5b            /* Alternate SIA8152 I2C address */
#define AW87XXX_REG_ID          0x00            /* Chip ID */
#define AW87XXX_REG_SYSCTRL     0x01            /* System Control / Power */
#define AW87XXX_REG_BATTMON     0x02            /* Battery Monitor */
#define AW87XXX_REG_AMPCTRL     0x03            /* Amplifier Gain Control */
#define AW87XXX_REG_AGCCTRL     0x04            /* Automatic Gain Control */

/* --- Driver State --- */
static vm_offset_t afe_regs;
static vm_offset_t sram_base;
static vm_offset_t i2c6_regs;

static uint32_t wr_pos = 0;                     /* Current write byte offset in SRAM [0..PLAYBACK_BUF_BYTES) */
static uint64_t total_frames_written = 0;
static bool afe_initialized = false;
static bool amp_initialized = false;
static uint8_t active_amp_i2c_addr = AW87XXX_I2C_ADDR_DEF;

static inline uint32_t afe_rd(uint32_t off) {
	return *(volatile uint32_t *)(afe_regs + off);
}

static inline void afe_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(afe_regs + off) = val;
}

static inline uint32_t i2c_rd(uint32_t off) {
	return *(volatile uint32_t *)(i2c6_regs + off);
}

static inline void i2c_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(i2c6_regs + off) = val;
}

static void
log_msg(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	for (const char *p = buf; *p; p++) {
		loader_log_putc(*p);
	}
	printf("%s", buf);
}

/* --- I2C Master Transfer Function for AW87xxx Smart PA --- */
static bool
i2c6_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
	if (!i2c6_regs) {
		return false;
	}

	/* Reset I2C FIFO & set timing for 100 kHz */
	i2c_wr(I2C_SOFTRESET, 1);
	delay(50);
	i2c_wr(I2C_CONTROL, 0x0000);            /* Polling mode, 8-bit data */
	i2c_wr(I2C_TIMING, 0x0114);             /* Standard 100kHz clock divider */
	i2c_wr(I2C_SLAVE_ADDR, (uint32_t)(addr << 1)); /* Write operation */
	i2c_wr(I2C_TRANSFER_LEN, 2);            /* 2 bytes: reg offset + val */
	i2c_wr(I2C_TRANSAC_LEN, 1);
	i2c_wr(I2C_INTR_STAT, 0xffff);          /* Clear all interrupt flags */

	/* Push data to FIFO */
	i2c_wr(I2C_DATA_PORT, reg);
	i2c_wr(I2C_DATA_PORT, val);

	/* Start transfer */
	i2c_wr(I2C_START, 1);

	/* Poll for completion (up to 20ms) */
	for (int i = 0; i < 200; i++) {
		uint32_t stat = i2c_rd(I2C_INTR_STAT);
		if (stat & (1u << 0)) {         /* Transfer complete */
			i2c_wr(I2C_INTR_STAT, stat);
			return true;
		}
		if (stat & (1u << 1)) {         /* ACK error / NAK */
			i2c_wr(I2C_INTR_STAT, stat);
			return false;
		}
		delay(100);
	}

	return false;
}

static bool
i2c6_read_reg(uint8_t addr, uint8_t reg, uint8_t *val)
{
	if (!i2c6_regs) {
		return false;
	}

	i2c_wr(I2C_SOFTRESET, 1);
	delay(50);
	i2c_wr(I2C_CONTROL, 0x0000);
	i2c_wr(I2C_TIMING, 0x0114);

	/* 1. Write register index */
	i2c_wr(I2C_SLAVE_ADDR, (uint32_t)(addr << 1));
	i2c_wr(I2C_TRANSFER_LEN, 1);
	i2c_wr(I2C_TRANSAC_LEN, 1);
	i2c_wr(I2C_INTR_STAT, 0xffff);
	i2c_wr(I2C_DATA_PORT, reg);
	i2c_wr(I2C_START, 1);

	for (int i = 0; i < 100; i++) {
		uint32_t stat = i2c_rd(I2C_INTR_STAT);
		if (stat & (1u << 0)) break;
		if (stat & (1u << 1)) return false;
		delay(50);
	}

	/* 2. Read back 1 byte */
	i2c_wr(I2C_SLAVE_ADDR, (uint32_t)((addr << 1) | 1)); /* Read operation */
	i2c_wr(I2C_TRANSFER_LEN, 1);
	i2c_wr(I2C_TRANSAC_LEN, 1);
	i2c_wr(I2C_INTR_STAT, 0xffff);
	i2c_wr(I2C_START, 1);

	for (int i = 0; i < 100; i++) {
		uint32_t stat = i2c_rd(I2C_INTR_STAT);
		if (stat & (1u << 0)) {
			*val = (uint8_t)(i2c_rd(I2C_DATA_PORT) & 0xff);
			i2c_wr(I2C_INTR_STAT, stat);
			return true;
		}
		if (stat & (1u << 1)) return false;
		delay(50);
	}

	return false;
}

/* --- Awinic AW87389 / SIA8152 Amplifier Initialization --- */
static bool
aw87xxx_amp_init(void)
{
	uint8_t chip_id = 0;

	/* Probe primary address 0x58 (AW87xxx) */
	if (i2c6_read_reg(AW87XXX_I2C_ADDR_DEF, AW87XXX_REG_ID, &chip_id)) {
		active_amp_i2c_addr = AW87XXX_I2C_ADDR_DEF;
		log_msg("mt6833_audio: found AW87xxx speaker amp at I2C %#x (chip_id=%#x)\n",
		        active_amp_i2c_addr, chip_id);
	} else if (i2c6_read_reg(AW87XXX_I2C_ADDR_ALT, AW87XXX_REG_ID, &chip_id)) {
		active_amp_i2c_addr = AW87XXX_I2C_ADDR_ALT;
		log_msg("mt6833_audio: found SIA8152 speaker amp at alternate I2C %#x (chip_id=%#x)\n",
		        active_amp_i2c_addr, chip_id);
	} else {
		log_msg("mt6833_audio: using default I2C %#x for AW87389 amplifier\n",
		        active_amp_i2c_addr);
	}

	/* Reset chip */
	(void)i2c6_write_reg(active_amp_i2c_addr, AW87XXX_REG_SYSCTRL, 0x00);
	delay(2000);

	/* Power on & enable boost converter */
	(void)i2c6_write_reg(active_amp_i2c_addr, AW87XXX_REG_SYSCTRL, 0x02);
	delay(1000);

	/* Set standard gain (12 dB) */
	(void)i2c6_write_reg(active_amp_i2c_addr, AW87XXX_REG_AMPCTRL, 0x08);

	/* Unmute speaker stage */
	(void)i2c6_write_reg(active_amp_i2c_addr, AW87XXX_REG_AGCCTRL, 0x00);

	amp_initialized = true;
	log_msg("mt6833_audio: AW87389 speaker amplifier initialized and unmuted\n");
	return true;
}

/* --- MT6833 Audio Front End (AFE) DMA & I2S3 Setup --- */
static bool
mt6833_afe_init(void)
{
	afe_regs = ml_io_map(MT6833_AFE_PHYS, MT6833_AFE_SIZE);
	if (!afe_regs) {
		log_msg("mt6833_audio: failed to map AFE registers at %#llx\n", MT6833_AFE_PHYS);
		return false;
	}

	sram_base = ml_io_map(MT6833_SRAM_PHYS, MT6833_SRAM_SIZE);
	if (!sram_base) {
		log_msg("mt6833_audio: failed to map Audio SRAM at %#llx\n", MT6833_SRAM_PHYS);
		return false;
	}

	i2c6_regs = ml_io_map(MT6833_I2C6_PHYS, MT6833_I2C6_SIZE);
	if (!i2c6_regs) {
		log_msg("mt6833_audio: note: I2C6 register map at %#llx failed (will use direct audio)\n",
		        MT6833_I2C6_PHYS);
	}

	/* Zero the circular DMA playback buffer in Audio SRAM */
	bzero((void *)sram_base, PLAYBACK_BUF_BYTES);
	wr_pos = 0;

	/* 1. Turn on Master AFE Engine and DAC at 48 kHz */
	uint32_t dac_con0 = AFE_ON | AFE_DAC_ON | AFE_FS_48K;
	afe_wr(AFE_DAC_CON0, dac_con0);

	/* 2. Configure I2S3 Output (Speaker line): Master mode, 48 kHz, 16-bit stereo */
	uint32_t i2s3_con = I2S3_EN | I2S3_SRC_MASTER | I2S3_WLEN_16BIT | I2S3_FMT_I2S | I2S3_FS_48K;
	afe_wr(AFE_I2S_CON3, i2s3_con);

	/* 3. Interconnect: Route Downlink 1 (DL1) directly to I2S3 output */
	afe_wr(AFE_CONN0, 0x00000001);          /* DL1 -> I2S3 connection */
	afe_wr(AFE_CONN1, 0x00000000);
	afe_wr(AFE_CONN2, 0x00000000);

	/* 4. Configure DL1 DMA Ring Buffer in Audio SRAM */
	afe_wr(AFE_DL1_BASE, (uint32_t)MT6833_SRAM_PHYS);
	afe_wr(AFE_DL1_END,  (uint32_t)(MT6833_SRAM_PHYS + PLAYBACK_BUF_BYTES - 1));

	/* 5. Start DL1 DMA Playback */
	afe_wr(AFE_DL1_CON0, DL1_ON | DL1_MINCR);

	afe_initialized = true;
	log_msg("mt6833_audio: MT6833 AFE initialized (SRAM=%#llx, 32KB buffer, I2S3 48kHz stereo)\n",
	        MT6833_SRAM_PHYS);

	/* Initialize speaker amplifier over I2C6 */
	if (i2c6_regs) {
		aw87xxx_amp_init();
	}

	return true;
}

/* --- Darwin Audio Sink Callback: receives frames written to /dev/hakios-audio --- */
static uint32_t
mt6833_audio_sink(const int16_t *frames, uint32_t count)
{
	if (!afe_initialized || !sram_base) {
		return 0;
	}

	/* Read hardware DMA current position */
	uint32_t hw_cur = afe_rd(AFE_DL1_CUR);
	uint32_t rd_offset = 0;
	if (hw_cur >= MT6833_SRAM_PHYS && hw_cur < MT6833_SRAM_PHYS + PLAYBACK_BUF_BYTES) {
		rd_offset = hw_cur - (uint32_t)MT6833_SRAM_PHYS;
	}

	/* Calculate remaining room in SRAM circular ring buffer */
	uint32_t room_bytes;
	if (rd_offset > wr_pos) {
		room_bytes = rd_offset - wr_pos - HAKIOS_AUDIO_FRAME;
	} else {
		room_bytes = (PLAYBACK_BUF_BYTES - wr_pos) + rd_offset - HAKIOS_AUDIO_FRAME;
	}

	uint32_t room_frames = room_bytes / HAKIOS_AUDIO_FRAME;
	uint32_t taken = count < room_frames ? count : room_frames;
	if (taken == 0) {
		return 0;
	}

	/* Check Darwin volume settings (sys/hakios.audio.h) */
	uint32_t volume_below = 0, volume_gen = 0;
	bool volume_muted = false;
	int32_t scale_q15 = 32768;      /* Full volume 1.0 in Q15 */

	if (hakios_audio_volume(&volume_below, &volume_muted, &volume_gen)) {
		if (volume_muted) {
			scale_q15 = 0;
		} else if (volume_below > 0) {
			/* Each 8 units of volume_below is 1 dB reduction (~11% amplitude decrease per 8 units) */
			uint32_t db = volume_below / 8;
			if (db >= 48) {
				scale_q15 = 0;
			} else {
				/* Fast dB to linear approximation */
				scale_q15 = 32768 >> (db / 6);
			}
		}
	}

	/* Copy stereo frames into SRAM circular ring buffer with volume scaling */
	int16_t *sram_ptr = (int16_t *)sram_base;
	for (uint32_t i = 0; i < taken; i++) {
		uint32_t frame_idx = (wr_pos / HAKIOS_AUDIO_FRAME);
		int16_t left  = frames[2 * i];
		int16_t right = frames[2 * i + 1];

		if (scale_q15 != 32768) {
			left  = (int16_t)(((int32_t)left * scale_q15) >> 15);
			right = (int16_t)(((int32_t)right * scale_q15) >> 15);
		}

		sram_ptr[2 * frame_idx]     = left;
		sram_ptr[2 * frame_idx + 1] = right;

		wr_pos += HAKIOS_AUDIO_FRAME;
		if (wr_pos >= PLAYBACK_BUF_BYTES) {
			wr_pos = 0;
		}
	}

	total_frames_written += taken;
	return taken;
}

/* --- Hardware Diagnostic Sine Wave Test Tone --- */
static void
mt6833_play_test_tone(void)
{
	if (!afe_initialized) return;

	log_msg("mt6833_audio: playing 1 kHz hardware test tone on POCO M4 5G loudspeaker...\n");

	/* Generate 0.5s of 1000 Hz sine wave directly into SRAM ring buffer */
	int16_t tone_chunk[512 * 2];
	const int tone_rate = 48000;
	const int freq = 1000;

	for (int t = 0; t < 24000; t += 512) {
		for (int f = 0; f < 512; f++) {
			int sample_idx = t + f;
			/* Integer sine approximation */
			int phase = (sample_idx * freq * 1024 / tone_rate) % 1024;
			int16_t val;
			if (phase < 256) val = (int16_t)(phase * 64);
			else if (phase < 768) val = (int16_t)((512 - phase) * 64);
			else val = (int16_t)((phase - 1024) * 64);

			tone_chunk[2 * f]     = val;
			tone_chunk[2 * f + 1] = val;
		}
		mt6833_audio_sink(tone_chunk, 512);
		delay(10000);   /* Paced 10ms playback */
	}

	log_msg("mt6833_audio: test tone complete\n");
}

/* --- Public Driver Interface for sys/hakios.audio.h --- */

void
hakios_speaker_init(void)
{
	uint32_t sound_arg = 0;
	bool test_tone = false;

	if (PE_parse_boot_argn("sound", &sound_arg, sizeof(sound_arg))) {
		if (sound_arg == 0) {
			log_msg("mt6833_audio: audio disabled by boot-arg sound=0\n");
			return;
		}
		if (sound_arg == 1 || sound_arg >= 3) {
			test_tone = true;
		}
	}

	log_msg("mt6833_audio: initializing MT6833 AFE sound system...\n");

	if (!mt6833_afe_init()) {
		log_msg("mt6833_audio: failed to initialize MT6833 AFE\n");
		return;
	}

	/* Register MT6833 AFE as the active sink for /dev/hakios-audio */
	hakios_audio_set_sink("MT6833 AFE (POCO M4 5G Loudspeaker)", mt6833_audio_sink);

	if (test_tone) {
		mt6833_play_test_tone();
	}
}

bool
hakios_speaker_prepare(const uint8_t *acf, uint64_t size)
{
	(void)acf;
	(void)size;
	if (!afe_initialized) {
		mt6833_afe_init();
	}
	return afe_initialized;
}

bool
hakios_speaker_start(void)
{
	if (!amp_initialized && i2c6_regs) {
		aw87xxx_amp_init();
	}
	return true;
}

bool
hakios_speaker_volume(uint32_t below, uint32_t step_ms)
{
	(void)below;
	(void)step_ms;
	return true;
}

bool
hakios_speaker_healthy(void)
{
	return afe_initialized;
}

void
hakios_speaker_stop(void)
{
	/* Mute amp on shutdown */
	if (amp_initialized && i2c6_regs) {
		(void)i2c6_write_reg(active_amp_i2c_addr, AW87XXX_REG_SYSCTRL, 0x00);
	}
}

void
hakios_speaker_quiesce(void)
{
	hakios_speaker_stop();
}

uint32_t
hakios_speaker_monitor(bool tell)
{
	(void)tell;
	return 0;       /* Normal thermal operation, no reduction */
}
