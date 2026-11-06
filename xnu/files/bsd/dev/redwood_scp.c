/*
 * The SCP of the Infinix Hot 20's MT6768: the Cortex-M co-processor ("tinysys") whose firmware is the
 * sensor hub -- the accelerometer, gyroscope, light and proximity sensors are behind it, not on an
 * I2C bus of the application processor (docs/sensors-x6826b.md, run 276). Step 1, reading only:
 * whether it runs when XNU starts. Android's kernel does not load its firmware (no request_firmware
 * in drivers/misc/mediatek/scp/v01/scp_helper.c of MiCode/Xiaomi_Kernel_OpenSource merlin-r-oss): LK
 * does, and Android waits for the firmware's "ready" message. Step 1 writes nothing; step 2 writes one register.
 *
 * The addresses are the device tree's (scp@10500000: SRAM, cfg, clkctrl, l1cctrl, in that order, as
 * scp_helper.c maps them); the registers are scp/mt6768/scp_reg.h's:
 *   cfg (0x105C0000)   +0x1C SCP_A_TO_HOST_REG  bit 0 a message for the host, bit 8 the watchdog
 *                      +0x50..+0x6C SCP_A_GENERAL_REG0..7 (+0x64 the reboot handshake, 0x34 / 1)
 *                      +0x84 SCP_A_WDT_REG
 *                      +0xA8 SP, +0xAC LR, +0xB0 PSP, +0xB4 PC of the Cortex-M (debug copies)
 *                      +0x114 SCP_SLEEP_STATUS_REG bit 0 asleep, bit 1 in deep sleep
 *   infracfg (0x10001000) +0x148 MODULE_RESET_STATUS bit 3 the SCP held in reset
 * The PC read twice 20 ms apart: different, it runs.
 *
 * Boot-arg scp_step=1 (reading), 2 (and started), 3 (and its messages read), 4 (and the
 * accelerometer read), 5 (and enabled through nanohub first),
 * 6 (and the hub given its DRAM FIFO first), 7 (all of it in a thread, which then keeps the hub's
 * messages taken; the last sample as kern.redwood.accel): see below. What it finds is said once and kept for the end of the log.
 */

#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <kern/thread.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void redwood_keep(const char *format, ...) __printflike(1, 2);
extern void delay(int usec);

#define SCP_CFG_PHYS            0x105C0000u
#define SCP_A_TO_HOST_REG       0x1C
#define SCP_A_GENERAL_REG0      0x50
#define SCP_A_WDT_REG           0x84
#define SCP_A_DEBUG_LR_REG      0xAC
#define SCP_A_DEBUG_PC_REG      0xB4
#define SCP_SLEEP_STATUS_REG    0x114
#define SCP_SRAM_PHYS           0x10500000u     /* the Cortex-M's TCM, where LK puts the firmware */
#define SCP_REGION_INFO_OFFSET  0x400           /* struct scp_region_info_st (scp/mt6768/scp_helper.h) */
#define SCP_CLKCTRL_PHYS        0x105C4000u
#define SCP_SRAM_PDN            0x2C            /* clkctrl: SRAM banks powered down */
#define TOPCKGEN_PHYS           0x10000000u
#define CLK_CFG_UPDATE          0x004           /* bit 3: scp_sel's new setting taken */
#define CLK_CFG_0               0x040           /* scp_sel: bits 24-26 the parent (0: clk26m), bit 31 off */
#define CLK_CFG_0_CLR           0x048
#define SCP_SEL_SHIFT           24
#define INFRACFG_PHYS           0x10001000u
#define MODULE_RESET_STATUS     0x148
#define SCP_RESET_BIT           (1u << 3)

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

/*
 * Step 4: a message to the SCP and its answer, as scp_ipi_send() and scp_awake_lock() do it
 * (scp/v01/scp_ipi.c, scp/mt6768/scp_awake.c, scp_reg.h): the SCP woken by a direct interrupt
 * (0xA0 | 1 << AP_AWAKE_LOCK into infracfg's INFRA_IRQ_SET, 0x10001b14, until it takes it), the
 * message into its send buffer at 0x6e0 (data at +0x10, then the length, the id last), bit 0 of
 * SCP_GIPC_IN_REG (cfg+0x28) until it has taken it, and let go (AP_AWAKE_UNLOCK). The answer comes
 * as its next message at 0x5c0.
 */
#define SCP_GIPC_IN_REG         0x28
#define INFRA_IRQ_SET           0xB14
#define INFRA_IRQ_CLEAR         0xB18
#define SCP_SEND_OBJ            0x6e0
#define SCP_RECV_OBJ            0x5c0
#define IPI_SENSOR              8

static vm_offset_t scp_cfg, scp_infra, scp_tcm;
static unsigned scp_said;
static vm_offset_t scp_fifo;            /* the hub's DRAM FIFO, once step 6 has given it one */               /* messages said so far: the first 40, then none (scp_step=7 runs for good) */

static bool
scp_awake(unsigned which)               /* 0 lock, 1 unlock */
{
	bool taken = false;

	*(volatile uint32_t *)(scp_infra + INFRA_IRQ_SET) = 0xA0u | (1u << which);
	for (unsigned n = 0; n < 100000; n++) {
		uint32_t v = rd(scp_infra, INFRA_IRQ_SET);

		if ((v & 0xf0) != 0xA0 || !(v & (1u << which))) {
			taken = (v & 0xf0) == 0xA0;
			break;
		}
		delay(10);
	}
	*(volatile uint32_t *)(scp_infra + INFRA_IRQ_CLEAR) = rd(scp_infra, INFRA_IRQ_SET);
	return taken;
}

/* send `words` 32-bit words to the sensor hub and wait up to 1 s for its answer; say both */
static void
scp_sensor_request(const char *what, const uint32_t *words, unsigned count)
{
	bool awake = scp_awake(0), taken = false, answered = false;

	if ((rd(scp_cfg, SCP_GIPC_IN_REG) & 1) == 0) {
		for (unsigned i = 0; i < count; i++) {
			*(volatile uint32_t *)(scp_tcm + SCP_SEND_OBJ + 0x10 + 4 * i) = words[i];
		}
		*(volatile uint32_t *)(scp_tcm + SCP_SEND_OBJ + 4) = 4 * count;
		*(volatile uint32_t *)(scp_tcm + SCP_SEND_OBJ) = IPI_SENSOR;
		__asm__ volatile ("dsb sy" ::: "memory");
		*(volatile uint32_t *)(scp_cfg + SCP_GIPC_IN_REG) = 1;
		for (unsigned n = 0; n < 100000; n++) {
			if ((rd(scp_cfg, SCP_GIPC_IN_REG) & 1) == 0) {
				taken = true;
				break;
			}
			delay(10);
		}
	}
	(void)scp_awake(1);
	for (unsigned n = 0; n < 100 && taken; n++) {
		if (rd(scp_cfg, SCP_A_TO_HOST_REG) & 1) {
			answered = true;
			break;
		}
		delay(10000);
	}
	if (!answered) {
		redwood_keep("scp: step 4: %s: awake %d, taken %d, no answer (to host %#x, GIPC %#x)", what, awake, taken,
		    rd(scp_cfg, SCP_A_TO_HOST_REG), rd(scp_cfg, SCP_GIPC_IN_REG));
		return;
	}
	redwood_keep("scp: step 4: %s: id %u, %u bytes: %#x %#x %#x %#x %#x %#x %#x %#x", what, rd(scp_tcm, SCP_RECV_OBJ),
	    rd(scp_tcm, SCP_RECV_OBJ + 4), rd(scp_tcm, SCP_RECV_OBJ + 0x10), rd(scp_tcm, SCP_RECV_OBJ + 0x14),
	    rd(scp_tcm, SCP_RECV_OBJ + 0x18), rd(scp_tcm, SCP_RECV_OBJ + 0x1c), rd(scp_tcm, SCP_RECV_OBJ + 0x20),
	    rd(scp_tcm, SCP_RECV_OBJ + 0x24), rd(scp_tcm, SCP_RECV_OBJ + 0x28), rd(scp_tcm, SCP_RECV_OBJ + 0x2c));
	*(volatile uint32_t *)(scp_cfg + SCP_A_TO_HOST_REG) = 1;      /* taken */
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * Step 5: Google's nanohub protocol over IPI_CHRE, as Android's kernel enables a sensor
 * (drivers/staging/nanohub: comms.c, nanohub-mtk.c; SCP_nanoHub.c init_sensor_config_cmd): a
 * packet -- sync 0x31, a sequence number, the reason (CMD_COMMS_WRITE 0x1091), the length, the data,
 * and nanohub's own CRC32 (polynomial 0x04C11DB7, a nibble at a time, the bytes taken as
 * little-endian words, zero-padded, from ~0) -- as one IPI_CHRE (24) message; the SCP answers with a
 * packet as IPI_CHRE, and each one it sends is acknowledged with IPI_CHREX (25) carrying its size.
 * The data of the write is a struct ConfigCmd (inc_v1/SCP_sensorHub.h, packed, 20 bytes).
 */
#define IPI_CHRE                24
#define IPI_CHREX               25

static const uint32_t nanohub_crc_table[16] = {
	0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9, 0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
	0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61, 0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
};

static uint32_t
nanohub_crc32(const uint8_t *buffer, unsigned length)
{
	uint32_t crc = ~0u;

	for (unsigned i = 0; i < length; i += 4) {
		uint32_t word = 0;

		for (unsigned k = 0; k < 4 && i + k < length; k++) {
			word |= (uint32_t)buffer[i + k] << (8 * k);
		}
		crc ^= word;
		for (unsigned n = 0; n < 8; n++) {
			crc = (crc << 4) ^ nanohub_crc_table[crc >> 28];
		}
	}
	return crc;
}

/* one message of `length` bytes to the SCP as IPI `id` (woken, written, taken, let go); whether it took it */
static bool
scp_send(uint32_t id, const uint8_t *bytes, unsigned length)
{
	bool taken = false;

	(void)scp_awake(0);
	if ((rd(scp_cfg, SCP_GIPC_IN_REG) & 1) == 0) {
		for (unsigned i = 0; i < length; i += 4) {
			uint32_t word = 0;

			for (unsigned k = 0; k < 4 && i + k < length; k++) {
				word |= (uint32_t)bytes[i + k] << (8 * k);
			}
			*(volatile uint32_t *)(scp_tcm + SCP_SEND_OBJ + 0x10 + i) = word;
		}
		*(volatile uint32_t *)(scp_tcm + SCP_SEND_OBJ + 4) = length;
		*(volatile uint32_t *)(scp_tcm + SCP_SEND_OBJ) = id;
		__asm__ volatile ("dsb sy" ::: "memory");
		*(volatile uint32_t *)(scp_cfg + SCP_GIPC_IN_REG) = 1;
		for (unsigned n = 0; n < 100000; n++) {
			if ((rd(scp_cfg, SCP_GIPC_IN_REG) & 1) == 0) {
				taken = true;
				break;
			}
			delay(10);
		}
	}
	(void)scp_awake(1);
	return taken;
}

/* the SCP's messages for up to `ms`: each said, taken, and an IPI_CHRE one acknowledged with IPI_CHREX */
static void
scp_take_messages(const char *what, unsigned ms)
{
	for (unsigned t = 0; t < ms; t += 10) {
		uint32_t id, length;

		if ((rd(scp_cfg, SCP_A_TO_HOST_REG) & 1) == 0) {
			delay(10000);
			continue;
		}
		id = rd(scp_tcm, SCP_RECV_OBJ);
		length = rd(scp_tcm, SCP_RECV_OBJ + 4);
		if (what != NULL && scp_said++ < 40) redwood_keep("scp: step 5: %s: id %u, %u bytes: %08x %08x %08x %08x %08x %08x %08x %08x", what, id, length,
		    rd(scp_tcm, SCP_RECV_OBJ + 0x10), rd(scp_tcm, SCP_RECV_OBJ + 0x14), rd(scp_tcm, SCP_RECV_OBJ + 0x18),
		    rd(scp_tcm, SCP_RECV_OBJ + 0x1c), rd(scp_tcm, SCP_RECV_OBJ + 0x20), rd(scp_tcm, SCP_RECV_OBJ + 0x24),
		    rd(scp_tcm, SCP_RECV_OBJ + 0x28), rd(scp_tcm, SCP_RECV_OBJ + 0x2c));
		*(volatile uint32_t *)(scp_cfg + SCP_A_TO_HOST_REG) = 1;      /* taken */
		__asm__ volatile ("dsb sy" ::: "memory");
		if (id == IPI_CHRE) {
			uint8_t token[4] = { (uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24) };

			(void)scp_send(IPI_CHREX, token, 4);
		}
	}
}

/* enable (1) or disable (0) a sensor of the hub at `rate` (Hz times 1024), through nanohub */
static void
nanohub_config(uint8_t sensor_type, uint8_t enable, uint32_t rate, uint32_t seq)
{
	uint8_t packet[10 + 20 + 4];
	uint8_t *config = packet + 10;
	uint32_t crc;

	memset(packet, 0, sizeof(packet));
	packet[0] = 0x31;                                               /* COMMS_SYNC */
	memcpy(packet + 1, &seq, 4);
	packet[5] = 0x91; packet[6] = 0x10;                             /* CMD_COMMS_WRITE 0x00001091 */
	packet[9] = 20;                                                 /* the ConfigCmd's length */
	config[0] = 0x00; config[1] = 0x03;                             /* evtType EVT_NO_SENSOR_CONFIG_EVENT 0x300 */
	/* latency, 8 bytes at config+4: 0 */
	memcpy(config + 12, &rate, 4);
	config[16] = sensor_type;
	config[17] = enable;                                            /* CONFIG_CMD_ENABLE 1, _DISABLE 0 */
	crc = nanohub_crc32(packet, 30);
	memcpy(packet + 30, &crc, 4);
	redwood_keep("scp: step 5: nanohub config sensor %u enable %u rate %u: taken %d", sensor_type, enable, rate,
	    scp_send(IPI_CHRE, packet, sizeof(packet)));
}

static void
scp_bring_up(uint32_t step)
{
	uint32_t reset, pc0, pc1, sleep, general[8];
	vm_offset_t cfg, infra;

	infra = ml_io_map(INFRACFG_PHYS, 0x1000);
	reset = rd(infra, MODULE_RESET_STATUS);
	redwood_keep("scp: step 1: infracfg MODULE_RESET_STATUS %#x: the SCP %s", reset,
	    (reset & SCP_RESET_BIT) ? "HELD IN RESET" : "out of reset");
	if (reset & SCP_RESET_BIT) {
		return;         /* (its registers are not read while it is held) */
	}
	cfg = ml_io_map(SCP_CFG_PHYS, 0x1000);
	pc0 = rd(cfg, SCP_A_DEBUG_PC_REG);
	delay(20000);
	pc1 = rd(cfg, SCP_A_DEBUG_PC_REG);
	sleep = rd(cfg, SCP_SLEEP_STATUS_REG);
	for (unsigned i = 0; i < 8; i++) {
		general[i] = rd(cfg, SCP_A_GENERAL_REG0 + 4 * i);
	}
	redwood_keep("scp: step 1: PC %#x then %#x (%s), LR %#x, sleep status %#x, to host %#x, watchdog %#x", pc0, pc1,
	    pc0 != pc1 ? "it runs" : "the same", rd(cfg, SCP_A_DEBUG_LR_REG), sleep, rd(cfg, SCP_A_TO_HOST_REG),
	    rd(cfg, SCP_A_WDT_REG));
	redwood_keep("scp: step 1: general registers %#x %#x %#x %#x %#x %#x %#x %#x", general[0], general[1], general[2],
	    general[3], general[4], general[5], general[6], general[7]);
	/*
	 * cfg+0x0 is what lets the Cortex-M run: Android's kernel writes 1 there at the end of its probe
	 * (reset_scp(SCP_ALL_ENABLE) in scp_helper.c) -- LK only loads the firmware. Then the TCM, if
	 * its banks are powered: the firmware's first words (its vector table: the stack, then the reset
	 * handler) and the region info LK wrote at 0x400 -- where the loader and the firmware were, and
	 * the DRAM the firmware will use (ap_dram_start, ap_dram_size), which XNU must not be using.
	 */
	{
		vm_offset_t clkctrl = ml_io_map(SCP_CLKCTRL_PHYS, 0x1000), sram;
		uint32_t pdn = rd(clkctrl, SCP_SRAM_PDN), info[7];

		redwood_keep("scp: step 1: run register (cfg+0) %#x, SRAM banks powered down %#x", rd(cfg, 0), pdn);
		if (pdn != 0) {
			return;
		}
		sram = ml_io_map(SCP_SRAM_PHYS, 0x1000);
		for (unsigned i = 0; i < 7; i++) {
			info[i] = rd(sram, SCP_REGION_INFO_OFFSET + 4 * i);
		}
		redwood_keep("scp: step 1: TCM %#x %#x %#x %#x; region info: loader %#x+%#x, firmware %#x+%#x, DRAM %#x+%#x, "
		    "backup %#x", rd(sram, 0), rd(sram, 4), rd(sram, 0x800), rd(sram, 0x804), info[0], info[1], info[2], info[3],
		    info[4], info[5], info[6]);
		/*
		 * Step 2 (scp_step=2): let it run, as Android's kernel does at the end of its probe -- one
		 * write of 1 to cfg+0 -- when LK has put a firmware there (its region info says where) and the
		 * DRAM it will use lies below XNU's (0xc0000000 up: mtkloader.c, XNU_PHYS_BASE; run 278: the
		 * loader at 0x9f900000, the firmware's DRAM at 0x9f980000). Then what it does: its PC, its
		 * general registers, a message for the host, sleep, after 0.2 s and 1 s. Android is booted
		 * by a reset after the run, which takes the SCP back with everything else.
		 */
		if (step < 2) {
			return;
		}
		if (rd(cfg, 0) != 0 || info[0] == 0 || info[2] == 0 || info[4] == 0 || (uint64_t)info[4] + info[5] > 0xc0000000ULL) {
			redwood_keep("scp: step 2: not started (run register %#x, region info as above)", rd(cfg, 0));
			return;
		}
		/*
		 * Its clock first, as Android's kernel does before it lets it run (scp_pll_ctrl_set(PLL_ENABLE,
		 * CLK_26M) in reset_scp, scp/mt6768/scp_dvfs.c): topckgen's scp_sel on clk26m and not gated
		 * (drivers/clk/mediatek/clk-mt6768.c: CLK_CFG_0 bits 24-26, gate bit 31, update bit 3). Without
		 * it the firmware sat in its frequency meter's loop for good (0xa132: waiting for bit 4 of its
		 * 0xa0000220 to clear, run 280).
		 */
		{
			vm_offset_t topck = ml_io_map(TOPCKGEN_PHYS, 0x1000);
			uint32_t before = rd(topck, CLK_CFG_0);

			*(volatile uint32_t *)(topck + CLK_CFG_0_CLR) = (7u << SCP_SEL_SHIFT) | (1u << 31);
			*(volatile uint32_t *)(topck + CLK_CFG_UPDATE) = 1u << 3;
			__asm__ volatile ("dsb sy" ::: "memory");
			delay(100);
			redwood_keep("scp: step 2: topckgen CLK_CFG_0 %#x -> %#x (scp_sel parent %u, %s)", before, rd(topck, CLK_CFG_0),
			    (rd(topck, CLK_CFG_0) >> SCP_SEL_SHIFT) & 7, (rd(topck, CLK_CFG_0) >> 31) ? "GATED" : "on");
		}
		*(volatile uint32_t *)(cfg + 0) = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
		/* at 0.2, 1, 5 and 15 s: its frequency meter's loop may be a long calibration (run 282: still in it after 1 s) */
		static const int waits[] = { 200000, 800000, 4000000, 10000000 };
		static const char *const when[] = { "0.2", "1", "5", "15" };

		for (unsigned wait = 0; wait < 4; wait++) {
			delay(waits[wait]);
			for (unsigned i = 0; i < 8; i++) {
				general[i] = rd(cfg, SCP_A_GENERAL_REG0 + 4 * i);
			}
			pc0 = rd(cfg, SCP_A_DEBUG_PC_REG);
			delay(1000);
			pc1 = rd(cfg, SCP_A_DEBUG_PC_REG);
			redwood_keep("scp: step 2: %s s after the start: PC %#x then %#x, LR %#x, sleep %#x, to host %#x, general %#x %#x %#x "
			    "%#x %#x %#x %#x %#x", when[wait], pc0, pc1, rd(cfg, SCP_A_DEBUG_LR_REG), rd(cfg, SCP_SLEEP_STATUS_REG),
			    rd(cfg, SCP_A_TO_HOST_REG), general[0], general[1], general[2], general[3], general[4], general[5],
			    general[6], general[7]);
		}
		/*
		 * Step 3 (scp_step=3): its messages. The SCP's message to the host is in its TCM at 0x5c0
		 * (struct share_obj: id, length, 8 reserved bytes, the data; SCP_A_SHARE_BUFFER, 288 bytes
		 * before SCP_RTOS_START at 0x800, scp/mt6768/scp_helper.h and scp_ipi.h); bit 0 of
		 * SCP_A_TO_HOST_REG says there is one, and writing that bit back takes it (scp_irq.c). The
		 * first should be IPI_SCP_A_READY (12) with the size of its TCM. Up to 12 are read, 0.2 s apart.
		 */
		if (step >= 3) {
			vm_offset_t tcm0 = ml_io_map(SCP_SRAM_PHYS, 0x1000);

			for (unsigned n = 0; n < 12; n++) {
				if ((rd(cfg, SCP_A_TO_HOST_REG) & 1) == 0) {
					delay(200000);
					continue;
				}
				redwood_keep("scp: step 3: message id %u, %u bytes: %#x %#x %#x %#x %#x %#x", rd(tcm0, 0x5c0), rd(tcm0, 0x5c4),
				    rd(tcm0, 0x5d0), rd(tcm0, 0x5d4), rd(tcm0, 0x5d8), rd(tcm0, 0x5dc), rd(tcm0, 0x5e0), rd(tcm0, 0x5e4));
				*(volatile uint32_t *)(cfg + SCP_A_TO_HOST_REG) = 1;      /* taken */
				__asm__ volatile ("dsb sy" ::: "memory");
				delay(200000);
			}
			redwood_keep("scp: step 3: after the messages: PC %#x, to host %#x, sleep %#x", rd(cfg, SCP_A_DEBUG_PC_REG),
			    rd(cfg, SCP_A_TO_HOST_REG), rd(cfg, SCP_SLEEP_STATUS_REG));
		}
		/*
		 * Step 4 (scp_step=4): the accelerometer, as accelhub.c and SCP_nanoHub.c ask for it -- struct
		 * SCP_SENSOR_HUB_REQ (sensors-1.0/sensorHub/inc_v1/SCP_sensorHub.h): the sensor type in byte 0
		 * (ID_ACCELEROMETER, 0), the action in byte 1 (ACTIVATE 0, SET_DELAY 1, GET_DATA 2), then its
		 * argument. A GET_DATA answer is the header word, then struct data_unit_t: type, a 64-bit time
		 * stamp, x, y, z (int32, in the hub's units: 1000 per m/s^2 on MediaTek's hub, to be checked).
		 */
		scp_cfg = cfg;
		scp_infra = infra;
		scp_tcm = ml_io_map(SCP_SRAM_PHYS, 0x1000);
		if (step == 4 || step == 5) {
			static const uint32_t activate[] = { 0x00000000, 1 }, set_delay[] = { 0x00000100, 20 }, get_data[] = { 0x00000200 };

			scp_sensor_request("activate the accelerometer", activate, 2);
			scp_sensor_request("its delay 20 ms", set_delay, 2);
			for (unsigned n = 0; n < 3; n++) {
				delay(300000);
				scp_sensor_request("its data", get_data, 1);
			}
		}
		/* Step 5 (scp_step=5): the accelerometer (SENSOR_TYPE_ACCELEROMETER, 1) enabled through nanohub at 50 Hz */
		if (step == 5) {
			static const uint32_t get_data[] = { 0x00000200 };

			nanohub_config(1, 1, 50 * 1024, 1);
			scp_take_messages("after the config", 1000);
			for (unsigned n = 0; n < 3; n++) {
				delay(300000);
				scp_sensor_request("its data now", get_data, 1);
				scp_take_messages("then", 100);
			}
		}
		/*
		 * Step 6 (scp_step=6): first the DRAM the hub writes its samples into, as Android's kernel
		 * gives it before any sensor is enabled (SCP_nanoHub.c: the FIFO header -- read and write
		 * pointers, its size in whole 44-byte struct data_unit_t -- then SENSOR_HUB_SET_CONFIG with the
		 * buffer's address, sensor_send_dram_info_to_hub). Without it the SCP's watchdog fired as soon
		 * as the accelerometer was on (run 288: nanohub answered the config with success, then
		 * SCP_A_TO_HOST_REG bit 8). Android takes 1 MB of the SCP's reserved memory; here 1 MB at
		 * 0x4e000000 -- in the range the device tree gives the SCP's shared memory (0x40000000-0x50000000),
		 * clear of ATF, ramoops and the loader. Then the accelerometer
		 * through nanohub, and what the FIFO holds after 1 and 2 s: x, y, z at bytes 12, 16, 20 of a
		 * record.
		 */
		if (step >= 6) {
			static const uint32_t fifo_phys = 0x4e000000u, fifo_size = 0x40000u;   /* 256 KB: the I/O map is small (run 309) */
			vm_offset_t fifo = scp_fifo = ml_io_map(fifo_phys, fifo_size);
			uint32_t set_config[8] = { 0x00000400, fifo_phys, fifo_size, 0, 0, 0, 0, 0 };

			((volatile uint32_t *)fifo)[0] = 0;                             /* rp */
			((volatile uint32_t *)fifo)[1] = 0;                             /* wp */
			((volatile uint32_t *)fifo)[2] = (fifo_size - 16) / 44 * 44;    /* FIFOSize */
			((volatile uint32_t *)fifo)[3] = 0;
			__asm__ volatile ("dsb sy" ::: "memory");
			scp_sensor_request("the DRAM FIFO (SET_CONFIG)", set_config, 8);
			nanohub_config(1, 1, 50 * 1024, 1);
			scp_take_messages("after the config", 1000);
			for (unsigned n = 0; n < 2; n++) {
				uint32_t wp = ((volatile uint32_t *)fifo)[1], at;

				at = 16 + (wp >= 44 ? wp - 44 : 0);
				redwood_keep("scp: step 6: %u s: FIFO rp %u wp %u size %u; the last record: type %u, x %d y %d z %d; to host %#x",
				    n + 1, ((volatile uint32_t *)fifo)[0], wp, ((volatile uint32_t *)fifo)[2],
				    *(volatile uint8_t *)(fifo + at), *(volatile int32_t *)(fifo + at + 12), *(volatile int32_t *)(fifo + at + 16),
				    *(volatile int32_t *)(fifo + at + 20), rd(cfg, SCP_A_TO_HOST_REG));
				scp_take_messages("then", 1000);
			}
		}
		/* the code around where it is (the TCM from 0x800 on is the firmware, Thumb): to be disassembled on the Mac */
		{
			uint32_t at = (pc1 & ~0x7fu) - 0x40;
			vm_offset_t tcm = ml_io_map(SCP_SRAM_PHYS + (at & ~0xfffu), 0x2000);

			for (uint32_t line = 0; line < 8 && at >= 0x800 && at < 0x80000; line++) {
				uint32_t o = (at & 0xfffu) + line * 32;

				redwood_keep("scp: code %#x: %08x %08x %08x %08x %08x %08x %08x %08x", at + line * 32, rd(tcm, o),
				    rd(tcm, o + 4), rd(tcm, o + 8), rd(tcm, o + 12), rd(tcm, o + 16), rd(tcm, o + 20), rd(tcm, o + 24),
				    rd(tcm, o + 28));
			}
		}
	}
}

/*
 * Step 7 (scp_step=7): all of it in a thread of its own, the boot not waiting the ~20 s it takes,
 * and that thread then stays: every 10 ms it takes the hub's messages (a DIRECT_PUSH for each
 * sample, which the hub waits to have taken) and acknowledges nanohub's. The last sample is
 * kern.redwood.accel: "x y z count", x, y, z in 1/1000 m/s^2, count the records written so far.
 */
static int
accel_sample(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[64] = "none";

	if (scp_fifo != 0) {
		uint32_t wp = ((volatile uint32_t *)scp_fifo)[1], at = 16 + (wp >= 44 ? wp - 44 : 0);

		snprintf(text, sizeof(text), "%d %d %d %u", *(volatile int32_t *)(scp_fifo + at + 12),
		    *(volatile int32_t *)(scp_fifo + at + 16), *(volatile int32_t *)(scp_fifo + at + 20), wp / 44);
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, accel, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    accel_sample, "A", "");

/*
 * kern.redwood.orientation_state: where backboardd's orientation feed is (userland/src/inject.c,
 * orientation_start), written by it, read by a job: bits 0-7 the orientation last sent, bit 8 its
 * class found, 9 the manager caught, 10 the update method's type right, 11 the timer running, 16
 * and up the timer's seconds.
 */
static int orientation_state;
SYSCTL_INT(_kern_redwood, OID_AUTO, orientation_state, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &orientation_state, 0, "");

extern void delay_for_interval(uint32_t interval, uint32_t scale_factor);
extern kern_return_t thread_terminate(thread_t thread);

static void
scp_thread(__unused void *parameter, __unused wait_result_t result)
{
	scp_bring_up(6);
	redwood_keep("scp: step 7: the hub runs; its messages taken every 10 ms from now on (kern.redwood.accel)");
	for (;;) {
		scp_take_messages(NULL, 10);
		if ((rd(scp_cfg, SCP_A_TO_HOST_REG) & 0x100) != 0) {
			redwood_keep("scp: step 7: the SCP's watchdog fired (to host %#x): stopped", rd(scp_cfg, SCP_A_TO_HOST_REG));
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	thread_terminate(current_thread());
}

void
redwood_scp_init(void)
{
	uint32_t step = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("scp_step", &step, sizeof(step)) || step < 1) {
		return;
	}
	if (step < 7) {
		scp_bring_up(step);
		return;
	}
	if (kernel_thread_start(scp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
