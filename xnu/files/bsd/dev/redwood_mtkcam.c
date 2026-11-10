/*
 * The main camera of the Infinix Hot 20 (X6826B, MT6768): a Samsung S5K JN1 (chip ID 0x38e1) on I2C4's pins,
 * its MIPI CSI-2 received by SENINF and written to memory by CAMSV_1. The sequence is the one mtkloader
 * proved (mtkloader/mtkcam.h, docs/camera-x6826b.md) -- the same registers in the same order, only the way
 * to reach them differs (ml_io_map windows instead of the loader's identity map):
 *
 *   PMIC (MT6358, through the PMIC wrapper's WACS2): VCAMIO on, VCAMD's voltage 0x0500 (1.1 V, as Android
 *     writes it; LK leaves 0x0600) written AND READ BACK, VCAMD on; GPIO23 (the analog supply) high
 *   the CAM power domain (SPM 0x10006344) and its bus protection, the clocks (topckgen CLK_CFG_1/6/7, mmsys,
 *     camsys, the two MIPI blocks' 26 MHz), reset GPIO102, MCLK GPIO100 (function 1)
 *   the sensor's table (496 writes) by I2C done by hand on GPIO105/106, stream on (register 0x0100)
 *   SENINF/PHY table (66 registers) and CAMSV_1 (26), two buffers queued the way the HAL does it
 *
 * Nothing happens unless the boot-arg asks: mtkcam=1 runs ONE capture some seconds after boot (mtkcam_start,
 * default 10) and says what arrived in memory through redwood_host_keep_add; everything is put back
 * afterwards. Stage by stage, a line is said BEFORE each step that could hang the bus, so the last line
 * of a log that ends is the step that hung.
 *
 * Run it without connsys_step and without the GPU: the PMIC wrapper's channel is shared and nothing here
 * takes a lock against the other drivers.
 *
 * The tables are the vendor's (made on the user's machine from the phone's own capture by tools/camtable.py and
 * tools/camrx.py, never in git): without redwood_mtkcam_jn1.h and redwood_mtkcam_rx.h beside this file the
 * driver says so and does nothing.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <sys/redwood_camera.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern void delay(int usec);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);

#if __has_include("redwood_mtkcam_jn1.h") && __has_include("redwood_mtkcam_rx.h")
#include "redwood_mtkcam_jn1.h"
#include "redwood_mtkcam_rx.h"
#define MTKCAM_TABLES 1
#else
#define MTKCAM_TABLES 0
#endif

#define say(...) redwood_host_keep_add("mtkcam: " __VA_ARGS__)

#if MTKCAM_TABLES

/* ---- the register windows: physical address -> the kernel's mapping ---- */
#define CAM_BUF_SIZE    0x03300000ULL   /* three images of 16 MB (5100 bytes x 3072 lines = 14.9 MiB), then three headers of 1 MB */
#define CAM_NBUF_MAX    3u
#define CAM_IMG_OFF(k)  ((uint64_t)(k) * 0x01000000ULL)
#define CAM_HDR_OFF(k)  (0x03000000ULL + (uint64_t)(k) * 0x00100000ULL)
#define CAM_PITCH       4080u           /* a byte a pixel: with CAMSV_1 +0x4c bit 0 clear the DMA gets the top 8 of the 10 bits (runs 496-499); bit 0 set (mtkcam_raw10=1) gives 5100 bytes a line */
#define CAM_LINES       3072u

static struct {
	uint64_t pa;
	vm_size_t size;
	vm_offset_t va;
} cam_win[] = {
	{ 0x10000000ULL, 0x10000, 0 },          /* topckgen, infracfg, GPIO, SPM, APMIXED, the PMIC wrapper */
	{ 0x11c10000ULL, 0x6000, 0 },           /* the six MIPI RX analog blocks */
	{ 0x14000000ULL, 0x3000, 0 },           /* mmsys, smi_common */
	{ 0x1a000000ULL, 0x53000, 0 },          /* camsys, smi_larb3, SENINF, CAMSV */
};
#define CAM_NWIN (sizeof(cam_win) / sizeof(cam_win[0]))

static vm_offset_t cam_buf;                     /* the frame memory, mapped */
static uint64_t cam_pa = CAM_RX_BUF;            /* ... and where it is (boot-arg mtkcam_buf: to tell an address-bound stop from another) */
static uint64_t cam_bad_pa;                     /* the first physical address asked for that no window covers */

static volatile uint32_t *
cam_reg(uint64_t pa)
{
	for (unsigned i = 0; i < CAM_NWIN; i++) {
		if (pa >= cam_win[i].pa && pa + 4 <= cam_win[i].pa + cam_win[i].size && cam_win[i].va) {
			return (volatile uint32_t *)(cam_win[i].va + (pa - cam_win[i].pa));
		}
	}
	if (!cam_bad_pa) {
		cam_bad_pa = pa;
	}
	return NULL;
}

static uint32_t
rd32(uint64_t pa)
{
	volatile uint32_t *p = cam_reg(pa);

	return p ? *p : 0;
}

static void
wr32(uint64_t pa, uint32_t value)
{
	volatile uint32_t *p = cam_reg(pa);

	if (p) {
		*p = value;
	}
}

static inline void
dsb(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ---- the PMIC through the wrapper ---- */
#define PWRAP_BASE              0x1000d000ULL
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define MT6358_HWCID            0x0008

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PWRAP_FSM(rd32(PWRAP_BASE + PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pwrap_read_raw(uint32_t reg, uint16_t *value)
{
	if (!pwrap_wait(0)) {
		return false;
	}
	wr32(PWRAP_BASE + PWRAP_WACS2_CMD, (reg >> 1) << 16);
	if (!pwrap_wait(6)) {
		return false;
	}
	*value = (uint16_t)rd32(PWRAP_BASE + PWRAP_WACS2_RDATA);
	wr32(PWRAP_BASE + PWRAP_WACS2_VLDCLR, 1);
	return true;
}

static bool
pwrap_write_raw(uint32_t reg, uint16_t value)
{
	if (!pwrap_wait(0)) {
		return false;
	}
	wr32(PWRAP_BASE + PWRAP_WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | value);
	return pwrap_wait(0);
}

/* (one transaction at a time on the wrapper: the keys/GPU-voltage and CONNSYS drivers use it too) */
static bool
pwrap_read(uint32_t reg, uint16_t *value)
{
	bool ok;

	redwood_pwrap_lock();
	ok = pwrap_read_raw(reg, value);
	redwood_pwrap_unlock();
	return ok;
}

static bool
pwrap_write(uint32_t reg, uint16_t value)
{
	bool ok;

	redwood_pwrap_lock();
	ok = pwrap_write_raw(reg, value);
	redwood_pwrap_unlock();
	return ok;
}

#define CAM_VCAMIO_EN           0x1cb0
#define CAM_VCAMD_EN            0x1c9c
#define CAM_VCAMD_VOSEL         0x1eae
#define CAM_VCAMD_1V1           0x0500

/* bit 0 of a rail's enable register, then read back: true if it holds */
static bool
cam_rail(uint32_t reg, bool on)
{
	uint16_t v = 0;

	if (!pwrap_read(reg, &v)) {
		return false;
	}
	pwrap_write(reg, on ? (v | 1u) : (v & ~1u));
	delay(200);
	if (!pwrap_read(reg, &v)) {
		return false;
	}
	return (v & 1u) == (on ? 1u : 0u);
}

/* ---- the pins ---- */
#define GPIO_BASE               0x10005000ULL
#define CAM_PIN_AVDD            23
#define CAM_PIN_PDN             98
#define CAM_PIN_RST             102
#define CAM_PIN_MCLK            100
#define CAM_BB_SCL              105
#define CAM_BB_SDA              106

struct cam_pin_state {
	uint32_t pin, mode, dir, dout;
};

static void
cam_pin_save(struct cam_pin_state *s, uint32_t pin)
{
	s->pin = pin;
	s->mode = (rd32(GPIO_BASE + 0x300 + 0x10 * (pin / 8)) >> (4 * (pin % 8))) & 0xfu;
	s->dir = (rd32(GPIO_BASE + 0x000 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
	s->dout = (rd32(GPIO_BASE + 0x100 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

static void
cam_pin_mode(uint32_t pin, uint32_t mode)
{
	uint64_t a = GPIO_BASE + 0x300 + 0x10 * (uint64_t)(pin / 8);
	uint32_t shift = 4 * (pin % 8);

	wr32(a, (rd32(a) & ~(0xfu << shift)) | (mode << shift));
}

/* output: the level first, then the direction, so the pin never drives the wrong way round */
static void
cam_pin_out(uint32_t pin, int level)
{
	uint32_t bank = pin / 32, bit = 1u << (pin % 32);

	wr32(GPIO_BASE + (level ? 0x104 : 0x108) + 0x10 * bank, bit);
	wr32(GPIO_BASE + 0x004 + 0x10 * bank, bit);
}

static void
cam_pin_restore(const struct cam_pin_state *s)
{
	uint32_t bank = s->pin / 32, bit = 1u << (s->pin % 32);

	wr32(GPIO_BASE + (s->dout ? 0x104 : 0x108) + 0x10 * bank, bit);
	wr32(GPIO_BASE + (s->dir ? 0x004 : 0x008) + 0x10 * bank, bit);
	cam_pin_mode(s->pin, s->mode);
}

static int
cam_pin_read(uint32_t pin)
{
	return (rd32(GPIO_BASE + 0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

/* ---- I2C by hand on the camera bus's pins ---- */
static void
cam_bb_line(uint32_t pin, int high)
{
	uint32_t bank = pin / 32, bit = 1u << (pin % 32);

	if (high) {
		wr32(GPIO_BASE + 0x008 + 0x10 * bank, bit);     /* input: the pull-up takes it high */
	} else {
		wr32(GPIO_BASE + 0x108 + 0x10 * bank, bit);     /* output 0 ... */
		wr32(GPIO_BASE + 0x004 + 0x10 * bank, bit);     /* ... then drive it */
	}
}

static void
cam_bb_scl(int high)
{
	cam_bb_line(CAM_BB_SCL, high);
	if (high) {
		for (int i = 0; i < 200 && !cam_pin_read(CAM_BB_SCL); i++) {    /* a slave may hold it low */
			delay(5);
		}
	}
	delay(5);
}

static void
cam_bb_sda(int high)
{
	cam_bb_line(CAM_BB_SDA, high);
	delay(5);
}

static void
cam_bb_start(void)
{
	cam_bb_sda(1);
	cam_bb_scl(1);
	cam_bb_sda(0);
	cam_bb_scl(0);
}

static void
cam_bb_stop(void)
{
	cam_bb_sda(0);
	cam_bb_scl(1);
	cam_bb_sda(1);
}

/* one byte out; 1 if the slave pulled SDA low for the ninth clock (an acknowledge) */
static int
cam_bb_write(uint8_t byte)
{
	int ack;

	for (int i = 7; i >= 0; i--) {
		cam_bb_sda((byte >> i) & 1);
		cam_bb_scl(1);
		cam_bb_scl(0);
	}
	cam_bb_sda(1);
	cam_bb_scl(1);
	ack = !cam_pin_read(CAM_BB_SDA);
	cam_bb_scl(0);
	return ack;
}

static uint8_t
cam_bb_read(int ack)
{
	uint8_t byte = 0;

	cam_bb_sda(1);
	for (int i = 0; i < 8; i++) {
		cam_bb_scl(1);
		byte = (uint8_t)((byte << 1) | (cam_pin_read(CAM_BB_SDA) ? 1 : 0));
		cam_bb_scl(0);
	}
	cam_bb_sda(!ack);
	cam_bb_scl(1);
	cam_bb_scl(0);
	cam_bb_sda(1);
	return byte;
}

/* the 16-bit register address out, then one byte in; a bit per acknowledge missed (0: all answered) */
static int
cam_bb_read8(uint32_t addr7, uint16_t reg, uint8_t *byte)
{
	int missed = 0;

	cam_bb_start();
	missed |= !cam_bb_write((uint8_t)(addr7 << 1)) << 0;
	missed |= !cam_bb_write((uint8_t)(reg >> 8)) << 1;
	missed |= !cam_bb_write((uint8_t)(reg & 0xffu)) << 2;
	cam_bb_start();                                 /* a repeated start */
	missed |= !cam_bb_write((uint8_t)((addr7 << 1) | 1)) << 3;
	*byte = cam_bb_read(0);
	cam_bb_stop();
	return missed;
}

/* a register write: 16-bit address, one or two data bytes; true if every byte was acknowledged */
static bool
cam_bb_write_reg(uint32_t addr7, uint16_t reg, uint16_t val, int width)
{
	int ok;

	cam_bb_start();
	ok = cam_bb_write((uint8_t)(addr7 << 1));
	ok &= cam_bb_write((uint8_t)(reg >> 8));
	ok &= cam_bb_write((uint8_t)(reg & 0xffu));
	if (width == 2) {
		ok &= cam_bb_write((uint8_t)(val >> 8));
	}
	ok &= cam_bb_write((uint8_t)(val & 0xffu));
	cam_bb_stop();
	return ok != 0;
}

/* ---- the CAM power domain and the camera's clocks ---- */
#define TOPCKGEN_BASE           0x10000000ULL
#define INFRACFG_BASE           0x10001000ULL
#define CAM_CFG1                0x50
#define CAM_CFG1_SET            0x54
#define CAM_CFG1_CLR            0x58
#define CAM_CFG1_CAMTG_PDN      0x00800000u
#define CAM_CFG1_CAMTG1_PDN     0x80000000u
#define CAM_CFG6                0xa0
#define CAM_CFG6_SET            0xa4
#define CAM_CFG6_CLR            0xa8
#define CAM_CFG6_SENINF_PDN     0x00800000u
#define CAM_CFG7                0xb0
#define CAM_CFG7_SET            0xb4
#define CAM_CFG7_CLR            0xb8
#define CAM_CFG7_PDN            (0x80000000u | 0x00008000u)

#define CAM_SPM_BASE            0x10006000ULL
#define CAM_SPM_CON             0x344
#define CAM_SPM_STA             0x180
#define CAM_SPM_STA2            0x184
#define CAM_STA_BIT             (1u << 7)
#define CAM_PWR_RST_B           (1u << 0)
#define CAM_PWR_ISO             (1u << 1)
#define CAM_PWR_ON              (1u << 2)
#define CAM_PWR_ON_2ND          (1u << 3)
#define CAM_PWR_CLK_DIS         (1u << 4)
#define CAM_SRAM_PDN            (3u << 8)
#define CAM_SRAM_ACK            (3u << 12)

#define CAM_INFRA_PROT_STA1     0x228
#define CAM_INFRA_PROT_STA2     0x258
#define CAM_INFRA_PROT_SET      0x2a0
#define CAM_INFRA_PROT_CLR      0x2a4
#define CAM_INFRA_PROT1_SET     0x2a8
#define CAM_INFRA_PROT1_CLR     0x2ac
#define CAM_PROT_A              0x00100000u
#define CAM_PROT_B              0x00280000u
#define CAM_SMI_BASE            0x14002000ULL
#define CAM_SMI_STA             0x3c0
#define CAM_SMI_SET             0x3c4
#define CAM_SMI_CLR             0x3c8

#define CAM_MMSYS_BASE          0x14000000ULL
#define CAM_MMSYS_CG_CON0       0x100
#define CAM_MMSYS_CG_SET0       0x104
#define CAM_MMSYS_CG_CLR0       0x108
#define CAM_MMSYS_SMI_CAM       0x02000000u

#define CAM_CAMSYS_BASE         0x1a000000ULL
#define CAM_CAMSYS_CG_CLR       0x8
#define CAM_CAMSYS_GATES        0x00001fc3u

#define CAM_APMIXED_MIPI        0x1000c014ULL
#define CAM_APMIXED_BITS        0x00000840u
#define CAM_MIPIRX_BASE         0x11c10000ULL

static bool
cam_poll(uint64_t addr, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd32(addr) & mask) == want) {
			return true;
		}
		delay(100);
	}
	return false;
}

static void
cam_con_set(uint32_t bits)
{
	wr32(CAM_SPM_BASE + CAM_SPM_CON, rd32(CAM_SPM_BASE + CAM_SPM_CON) | bits);
	dsb();
}

static void
cam_con_clr(uint32_t bits)
{
	wr32(CAM_SPM_BASE + CAM_SPM_CON, rd32(CAM_SPM_BASE + CAM_SPM_CON) & ~bits);
	dsb();
}

/* 1: switched on by this; 0: it did not come up (what is half done is the caller's to put back); -1: it was on already */
static int
cam_domain_on(void)
{
	if ((rd32(CAM_SPM_BASE + CAM_SPM_STA) & CAM_STA_BIT) || (rd32(CAM_SPM_BASE + CAM_SPM_STA2) & CAM_STA_BIT)) {
		say("the CAM domain is on already: left alone");
		return -1;
	}
	say("CAM domain: PWR_ON, PWR_ON_2ND");
	cam_con_set(CAM_PWR_ON);
	cam_con_set(CAM_PWR_ON_2ND);
	if (!cam_poll(CAM_SPM_BASE + CAM_SPM_STA, CAM_STA_BIT, CAM_STA_BIT, 20) ||
	    !cam_poll(CAM_SPM_BASE + CAM_SPM_STA2, CAM_STA_BIT, CAM_STA_BIT, 20)) {
		say("  the status bit did not come up");
		return 0;
	}
	say("CAM domain: clock on, isolation off, reset released");
	cam_con_clr(CAM_PWR_CLK_DIS);
	cam_con_clr(CAM_PWR_ISO);
	cam_con_set(CAM_PWR_RST_B);
	say("CAM domain: SRAM awake");
	cam_con_clr(1u << 8);
	if (!cam_poll(CAM_SPM_BASE + CAM_SPM_CON, 1u << 12, 0, 20)) {
		say("  SRAM 0 did not acknowledge");
		return 0;
	}
	cam_con_clr(1u << 9);
	if (!cam_poll(CAM_SPM_BASE + CAM_SPM_CON, 1u << 13, 0, 20)) {
		say("  SRAM 1 did not acknowledge");
		return 0;
	}
	say("CAM domain: bus protection released");
	wr32(INFRACFG_BASE + CAM_INFRA_PROT_CLR, CAM_PROT_A);
	dsb();
	wr32(CAM_SMI_BASE + CAM_SMI_CLR, 8);
	dsb();
	wr32(INFRACFG_BASE + CAM_INFRA_PROT1_CLR, CAM_PROT_B);
	dsb();
	return 1;
}

/* the same in the other order; true if it ended with the status bit down */
static bool
cam_domain_off(void)
{
	bool ok = true;

	say("CAM domain: bus protection on");
	wr32(INFRACFG_BASE + CAM_INFRA_PROT1_SET, CAM_PROT_B);
	dsb();
	ok &= cam_poll(INFRACFG_BASE + CAM_INFRA_PROT_STA2, CAM_PROT_B, CAM_PROT_B, 20);
	wr32(INFRACFG_BASE + CAM_INFRA_PROT_SET, CAM_PROT_A);
	dsb();
	ok &= cam_poll(INFRACFG_BASE + CAM_INFRA_PROT_STA1, CAM_PROT_A, CAM_PROT_A, 20);
	wr32(CAM_SMI_BASE + CAM_SMI_SET, 8);
	dsb();
	ok &= cam_poll(CAM_SMI_BASE + CAM_SMI_STA, 8, 8, 20);
	if (!ok) {
		say("  bus protection did not acknowledge: leaving the domain as it is");
		return false;
	}
	say("CAM domain: SRAM to sleep, isolation on, clock off, reset, power off");
	cam_con_set(CAM_SRAM_PDN);
	cam_poll(CAM_SPM_BASE + CAM_SPM_CON, CAM_SRAM_ACK, CAM_SRAM_ACK, 20);
	cam_con_set(CAM_PWR_ISO);
	cam_con_set(CAM_PWR_CLK_DIS);
	cam_con_clr(CAM_PWR_RST_B);
	cam_con_clr(CAM_PWR_ON);
	cam_con_clr(CAM_PWR_ON_2ND);
	return cam_poll(CAM_SPM_BASE + CAM_SPM_STA, CAM_STA_BIT, 0, 20) && cam_poll(CAM_SPM_BASE + CAM_SPM_STA2, CAM_STA_BIT, 0, 20);
}

struct cam_sysst {
	uint32_t cfg7, cfg6, mm_cg, apmix, csr[6], mm_sel;
	bool camsel_changed, mmsel_changed;
};

/*
 * cam_sel, the ISP's clock mux (topckgen CLK_CFG_7 bits 26:24, gate bit 31): Android's mtk_clk_mux_set_parent_setclr_lock
 * (descriptor read out of the running kernel, 7 Nov: mux 0xb0, set 0xb4, clr 0xb8, update register 0x004 bit 31) clears the
 * field in the clr register, sets the new index in the set register, then writes the update bit. Its parent index is 1
 * (syspll_d2) while the display is busy and 6 (univpll1_d2) when it is idle (disp_idlemgr, clk_set_parent events).
 */
#define CAM_CFG_UPDATE          0x004
#define CAM_CAMSEL_SHIFT        24
#define CAM_CAMSEL_MASK         (7u << CAM_CAMSEL_SHIFT)
#define CAM_CAMSEL_UPDATE       (1u << 31)

/*
 * mm_sel (the multimedia clock: the SMI bus and the display; topckgen CLK_CFG_0 bits 18:16, descriptor read out of Android's kernel the
 * same way: mux 0x40, set 0x44, clr 0x48, update register 0x004 bit 2). The display driver of Android keeps it on parent 1 (mmpll_ck)
 * while busy and 6 (univpll1_d2) idle. The camera's DMA writes through the SMI bus on this clock. Boot-arg mtkcam_mmsel=N.
 */
#define CAM_MMSEL_MUX           0x40
#define CAM_MMSEL_SET           0x44
#define CAM_MMSEL_CLR           0x48
#define CAM_MMSEL_SHIFT         16
#define CAM_MMSEL_MASK          (7u << CAM_MMSEL_SHIFT)
#define CAM_MMSEL_UPDATE        (1u << 2)
static uint32_t cam_mmsel_arg = 0xffffffffu;

static void
cam_mmsel_set(uint32_t index)
{
	wr32(TOPCKGEN_BASE + CAM_MMSEL_CLR, CAM_MMSEL_MASK);
	wr32(TOPCKGEN_BASE + CAM_MMSEL_SET, (index << CAM_MMSEL_SHIFT) & CAM_MMSEL_MASK);
	wr32(TOPCKGEN_BASE + CAM_CFG_UPDATE, CAM_MMSEL_UPDATE);
	dsb();
	delay(200);
}

static uint32_t cam_camsel_arg = 0xffffffffu;   /* boot-arg mtkcam_camsel=N: the parent index to run the capture with */

static void
cam_camsel_set(uint32_t index)
{
	wr32(TOPCKGEN_BASE + CAM_CFG7_CLR, CAM_CAMSEL_MASK);
	wr32(TOPCKGEN_BASE + CAM_CFG7_SET, (index << CAM_CAMSEL_SHIFT) & CAM_CAMSEL_MASK);
	wr32(TOPCKGEN_BASE + CAM_CFG_UPDATE, CAM_CAMSEL_UPDATE);
	dsb();
	delay(200);
}

static bool
cam_sys_up(struct cam_sysst *st)
{
	int dom = cam_domain_on();

	if (dom <= 0) {
		if (dom == 0) {
			cam_domain_off();
		}
		return false;
	}
	st->cfg7 = rd32(TOPCKGEN_BASE + CAM_CFG7);
	st->cfg6 = rd32(TOPCKGEN_BASE + CAM_CFG6);
	st->mm_cg = rd32(CAM_MMSYS_BASE + CAM_MMSYS_CG_CON0);
	say("clocks: cam_sel, camtm_sel, seninf_sel, mm_smi_cam_ck, the camera gates");
	wr32(TOPCKGEN_BASE + CAM_CFG7_CLR, CAM_CFG7_PDN);
	wr32(TOPCKGEN_BASE + CAM_CFG6_CLR, CAM_CFG6_SENINF_PDN);
	wr32(CAM_MMSYS_BASE + CAM_MMSYS_CG_CLR0, CAM_MMSYS_SMI_CAM);
	dsb();
	delay(100);
	say("CLK_CFG_7 %#x: cam_sel parent index %u (Android: 1 syspll_d2 busy, 6 univpll1_d2 idle)", rd32(TOPCKGEN_BASE + CAM_CFG7),
	    (rd32(TOPCKGEN_BASE + CAM_CFG7) & CAM_CAMSEL_MASK) >> CAM_CAMSEL_SHIFT);
	st->camsel_changed = false;
	st->mmsel_changed = false;
	say("CLK_CFG_0 %#x: mm_sel parent index %u (Android: 1 mmpll_ck busy, 6 univpll1_d2 idle)", rd32(TOPCKGEN_BASE + CAM_MMSEL_MUX),
	    (rd32(TOPCKGEN_BASE + CAM_MMSEL_MUX) & CAM_MMSEL_MASK) >> CAM_MMSEL_SHIFT);
	if (cam_mmsel_arg <= 7 && cam_mmsel_arg != ((rd32(TOPCKGEN_BASE + CAM_MMSEL_MUX) & CAM_MMSEL_MASK) >> CAM_MMSEL_SHIFT)) {
		st->mm_sel = (rd32(TOPCKGEN_BASE + CAM_MMSEL_MUX) & CAM_MMSEL_MASK) >> CAM_MMSEL_SHIFT;
		say("mm_sel -> parent index %u (mtkcam_mmsel)", cam_mmsel_arg);
		cam_mmsel_set(cam_mmsel_arg);
		st->mmsel_changed = true;
		say("  CLK_CFG_0 now %#x", rd32(TOPCKGEN_BASE + CAM_MMSEL_MUX));
	}
	if (cam_camsel_arg <= 7 && cam_camsel_arg != ((st->cfg7 & CAM_CAMSEL_MASK) >> CAM_CAMSEL_SHIFT)) {
		say("cam_sel -> parent index %u (mtkcam_camsel)", cam_camsel_arg);
		cam_camsel_set(cam_camsel_arg);
		st->camsel_changed = true;
		say("  CLK_CFG_7 now %#x", rd32(TOPCKGEN_BASE + CAM_CFG7));
	}
	wr32(CAM_CAMSYS_BASE + CAM_CAMSYS_CG_CLR, CAM_CAMSYS_GATES);
	dsb();
	delay(100);
	st->apmix = rd32(CAM_APMIXED_MIPI);
	wr32(CAM_APMIXED_MIPI, st->apmix | CAM_APMIXED_BITS);
	for (int n = 0; n < 6; n++) {
		uint64_t a = CAM_MIPIRX_BASE + 0x1000 * (uint64_t)n + 0x80;

		st->csr[n] = rd32(a);
		wr32(a, st->csr[n] | 2u);
	}
	dsb();
	say("the camera subsystem is up (MIPI 26 MHz clocks %#x -> %#x)", st->apmix, rd32(CAM_APMIXED_MIPI));
	return true;
}

static void
cam_sys_down(const struct cam_sysst *st)
{
	for (int n = 0; n < 6; n++) {
		wr32(CAM_MIPIRX_BASE + 0x1000 * (uint64_t)n + 0x80, st->csr[n]);
	}
	wr32(CAM_APMIXED_MIPI, st->apmix);
	dsb();
	/* the domain goes down FIRST, with its clocks still running: the bus acknowledges its protection with them */
	say("%s", cam_domain_off() ? "the CAM domain is off again" : "THE CAM DOMAIN DID NOT GO DOWN");
	if (st->mmsel_changed) {
		cam_mmsel_set(st->mm_sel);
		say("mm_sel put back: CLK_CFG_0 %#x", rd32(TOPCKGEN_BASE + CAM_MMSEL_MUX));
	}
	if (st->camsel_changed) {
		cam_camsel_set((st->cfg7 & CAM_CAMSEL_MASK) >> CAM_CAMSEL_SHIFT);
		say("cam_sel put back: CLK_CFG_7 %#x", rd32(TOPCKGEN_BASE + CAM_CFG7));
	}
	wr32(CAM_MMSYS_BASE + CAM_MMSYS_CG_SET0, (st->mm_cg & CAM_MMSYS_SMI_CAM));
	if (st->cfg7 & CAM_CFG7_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG7_SET, st->cfg7 & CAM_CFG7_PDN);
	}
	if (st->cfg6 & CAM_CFG6_SENINF_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG6_SET, CAM_CFG6_SENINF_PDN);
	}
	dsb();
}

/* ---- the receiver and the frame memory ---- */
#define CAM_SENINF_BASE         0x1a040000ULL
#define CAM_SENINF_WORDS        (0x5000 / 4)
#define CAM_SV_BASE             0x1a051000ULL
#define CAM_SV_WORDS            (0x1000 / 4)
#define CAM_LARB3_BASE          0x1a002000ULL

/*
 * smi_larb3 as Android's kernel leaves it while the camera previews (read out of the running kernel through the larb's mapping
 * with kprobe fetches, 7 Nov; the registers that are not status or the IOMMU switch): the outstanding limits per port (+0x200..), the
 * bandwidth/priority setup (+0x0b0..) and the port table (+0x300..). Our ports come out of reset without them. Boot-arg mtkcam_larb=1.
 */
static const struct {
	uint16_t off;
	uint32_t val;
} cam_larb3_android[] = {
	{ 0x010, 0xff3 },
	{ 0x024, 0x300223 },
	{ 0x02c, 0x7 },
	{ 0x040, 0x1 },
	{ 0x060, 0x1fffff },
	{ 0x080, 0x1fffff },
	{ 0x0b0, 0xaaaaa },
	{ 0x0b4, 0xd83e0 },
	{ 0x0b8, 0x200020 },
	{ 0x0c8, 0x1 },
	{ 0x0d0, 0x1 },
	{ 0x200, 0x1414 },
	{ 0x204, 0x606 },
	{ 0x208, 0x202 },
	{ 0x20c, 0x202 },
	{ 0x210, 0x202 },
	{ 0x214, 0x202 },
	{ 0x218, 0x404 },
	{ 0x21c, 0x202 },
	{ 0x220, 0x202 },
	{ 0x224, 0x202 },
	{ 0x228, 0x202 },
	{ 0x22c, 0x202 },
	{ 0x230, 0x404 },
	{ 0x234, 0x404 },
	{ 0x238, 0x404 },
	{ 0x23c, 0x202 },
	{ 0x240, 0x202 },
	{ 0x244, 0x202 },
	{ 0x248, 0x202 },
	{ 0x24c, 0x202 },
	{ 0x250, 0x202 },
	{ 0x300, 0x820020 },
	{ 0x304, 0x860021 },
	{ 0x308, 0x8a0022 },
	{ 0x30c, 0x8e0023 },
	{ 0x310, 0x920024 },
	{ 0x314, 0x960025 },
	{ 0x318, 0x9a0026 },
	{ 0x31c, 0x9e0027 },
	{ 0x320, 0x821028 },
	{ 0x324, 0x861029 },
	{ 0x328, 0x8a102a },
	{ 0x32c, 0x8e102b },
	{ 0x330, 0x92102c },
	{ 0x334, 0x96102d },
	{ 0x338, 0x9a102e },
	{ 0x33c, 0x9e102f },
	{ 0x340, 0xa21030 },
	{ 0x344, 0x822031 },
	{ 0x348, 0x862032 },
	{ 0x34c, 0x8a2033 },
	{ 0x350, 0x8e2034 },
};
static uint32_t cam_larb_arg;
/*
 * The two settings in which SENINF's mux0 (the ISP's path for this very image, live in Android) differs from mux3 (the phase-detect
 * channel's, the one this retargets): bit 0 of the variant puts mux3 +0x00 bit 8 up (0x...80 -> 0x...180), bit 1 gives mux3 +0x3c
 * mux0's 0x101 (it has 0x111). Boot-arg mtkcam_var=MASK runs one capture for each variant whose bit is up in MASK, one after the other.
 */
static uint32_t cam_variant;
static uint32_t cam_legacy;             /* the variant number as the old experiments (bits 1, 2, 4, 8, 16) read it: 0 while a register/gain sweep uses the number */
static uint32_t cam_fa, cam_fm, cam_fs; /* boot-args mtkcam_fa/fm/fs: CAMSV_1 register offset, mask, shift: variant v writes (v & mask) there */
static uint32_t cam_xp;                 /* boot-arg mtkcam_xp=1: variant v captures with a (TG lines, DMA lines) pair from a table (the 10-bit DMA stops 4 lines short) */
static uint32_t cam_gain_arg;           /* boot-arg mtkcam_gain=N: register 0x0204 <- N << variant (two captures at N and 2N: the gain unity) */
static uint32_t cam_dma_lines = CAM_LINES;
static bool cam_raw10;                  /* boot-arg mtkcam_raw10=1: 10 bits a pixel (CAMSV_1 +0x4c bit 0). NOT the default: with it the DMA flags an error in the first frame (run 508) */
static bool cam_serve;                  /* mtkcam=5: after stream on, serve /dev/redwood-camera until nobody reads */
static bool cam_stream_ok;              /* the session got as far as the stream */
static uint32_t cam_pitch = CAM_PITCH;  /* the DMA's line stride in bytes: variant 8 aligns it to 5120 (5100 is not a multiple of 16) */

static void
cam_larb_apply(void)
{
	unsigned changed = 0;

	for (unsigned i = 0; i < sizeof(cam_larb3_android) / sizeof(cam_larb3_android[0]); i++) {
		uint32_t was = rd32(CAM_LARB3_BASE + cam_larb3_android[i].off);

		if (was != cam_larb3_android[i].val) {
			say("  larb3 +%#x: %#x -> %#x", cam_larb3_android[i].off, was, cam_larb3_android[i].val);
			changed++;
		}
		wr32(CAM_LARB3_BASE + cam_larb3_android[i].off, cam_larb3_android[i].val);
	}
	dsb();
	say("smi_larb3: Android's configuration written (%u of %u registers differed)", changed,
	    (unsigned)(sizeof(cam_larb3_android) / sizeof(cam_larb3_android[0])));
}

static uint32_t cam_seninf_before[CAM_SENINF_WORDS];
static uint32_t cam_sv_before[CAM_SV_WORDS];

static volatile uint8_t *
cam_img(unsigned k)
{
	return (volatile uint8_t *)(cam_buf + CAM_IMG_OFF(k));
}

static bool cam_cached;                 /* the frame memory is mapped cacheable (serving): the DMA's writes go past the caches */

static void
cam_inval(volatile void *base, uint64_t n)
{
	if (!cam_cached) {
		return;
	}
	for (uint64_t a = (uint64_t)base & ~63ULL; a < (uint64_t)base + n; a += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (a) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

static void
cam_buf_clear(void)
{
	for (uint64_t off = 0; off < CAM_BUF_SIZE; off += 8) {
		*(volatile uint64_t *)(cam_buf + off) = 0;
	}
	dsb();
	cam_inval((volatile void *)cam_buf, CAM_BUF_SIZE);
}

/* the sensor interface and the view-finder (+0x500, +0x504): the receiver takes frames from here on */
static void
cam_rx_late(void)
{
	for (int i = 0; i < CAM_RX_SV_N; i++) {
		if (cam_rx_sv_table[i].addr == CAM_SV_BASE + 0x500 || cam_rx_sv_table[i].addr == CAM_SV_BASE + 0x504) {
			wr32(cam_rx_sv_table[i].addr, cam_rx_sv_table[i].val);
			dsb();
		}
	}
}

static void
cam_rx_program(void)
{
	cam_pitch = (cam_legacy & 8) ? 5120u : cam_raw10 ? 5100u : 4080u;
	{
		/* boot-arg mtkcam_pitch=N: the DMA's bytes per line. The cut is at 80% of the DMA frame whatever its height (2452 of 3072,
		 * 1916 of 2400): the DMA looks to be getting 4080 bytes a line (a byte a pixel), not 5100. */
		uint32_t pp = 0;

		if (PE_parse_boot_argn("mtkcam_pitch", &pp, sizeof(pp)) && pp >= 1024 && pp <= 8192) {
			cam_pitch = pp;
		}
	}
	/* 10 bits a pixel: the stream is ~3067.x lines of 5100 bytes, not 3072 (runs 506-513, +0x4c = 0x21); a DMA told to expect more never finishes and
	 * flags an error, one told 3066 or less has data left over and flags one too; 3067 let two frames through (run 513). 8 bits a pixel carries the whole 3072. */
	cam_dma_lines = cam_raw10 ? 3067u : CAM_LINES;
	say("clearing the frame memory at %#llx, %#llx bytes", (unsigned long long)cam_pa, (unsigned long long)CAM_BUF_SIZE);
	cam_buf_clear();
	/* the camera's SMI ports come out of reset translating (MMU_EN): no IOMMU tables here, so the DMA addresses must be physical */
	say("smi_larb3: MMU_EN off for the camera's ports (they were %#x)", rd32(CAM_LARB3_BASE + 0x380));
	for (int port = 0; port < 21; port++) {
		wr32(CAM_LARB3_BASE + 0x380 + 4 * (uint64_t)port, 0);
	}
	dsb();
	if (cam_larb_arg) {
		cam_larb_apply();
	}
	say("programming the receiver: %d registers, then CAMSV_1: %d", CAM_RX_N, CAM_RX_SV_N);
	for (int i = 0; i < CAM_RX_N; i++) {
		wr32(cam_rx_table[i].addr, cam_rx_table[i].val);
		dsb();
	}
	/* everything but the queue's own registers and the last two (+0x500, +0x504: the sensor interface and the view-finder) */
	for (int i = 0; i < CAM_RX_SV_N; i++) {
		if (cam_rx_sv_table[i].addr == CAM_SV_BASE + 0x500 || cam_rx_sv_table[i].addr == CAM_SV_BASE + 0x504) {
			continue;
		}
		wr32(cam_rx_sv_table[i].addr, cam_rx_sv_table[i].val);
		dsb();
	}
	if (cam_pa != CAM_RX_BUF) {
		wr32(CAM_SV_BASE + 0x218, (uint32_t)(cam_pa + CAM_IMG_OFF(1)));   /* the table's own copy of the second image's address */
	}
	/* the main image (VC0, 4080x3072) instead of the phase-detect channel: mux3's source 0xa -> 0x8, CAMSV_1's window and DMA geometry */
	say("main image: mux3 source 0xa -> 0x8, CAMSV_1 grab 4080x3072, DMA %u bytes x %u", CAM_PITCH, CAM_LINES);
	wr32(CAM_SENINF_BASE + 0x3d00, (rd32(CAM_SENINF_BASE + 0x3d00) & ~0xf000u) | 0x8000u);
	wr32(CAM_SV_BASE + 0x508, 0x0ff00000);
	wr32(CAM_SV_BASE + 0x50c, 0x0c000000);
	{
		/* boot-args mtkcam_ly=START mtkcam_lh=HEIGHT: grab only lines START..START+HEIGHT of the sensor's 3072 and make the DMA that tall.
		 * The cut at ~2452 lines (12.5 MB) is the same for every line length, stride, address and clock tried: a shorter or shifted
		 * frame says whether it is a size the DMA cannot pass or a place in the sensor's frame. */
		uint32_t ly = 0, lh = cam_dma_lines, full = cam_dma_lines;

		(void)PE_parse_boot_argn("mtkcam_ly", &ly, sizeof(ly));
		(void)PE_parse_boot_argn("mtkcam_lh", &lh, sizeof(lh));
		if (lh == 0 || ly + lh > CAM_LINES) {
			lh = CAM_LINES - ly;
		}
		if (cam_xp) {
			static const uint32_t pairs[8][2] = { { 3072, 3068 }, { 3068, 3064 }, { 3060, 3056 }, { 3072, 3067 }, { 3072, 3066 }, { 3072, 3065 }, { 3072, 3064 }, { 3072, 3070 } };

			wr32(CAM_SV_BASE + 0x50c, pairs[cam_variant & 7][0] << 16);
			cam_dma_lines = pairs[cam_variant & 7][1];
			say("  experiment %u: TG lines %u, DMA lines %u", cam_variant & 7, pairs[cam_variant & 7][0], cam_dma_lines);
		} else if (ly || lh != full) {
			say("  crop: sensor lines %u..%u, DMA %u lines", ly, ly + lh, lh);
			wr32(CAM_SV_BASE + 0x50c, ((ly + lh) << 16) | ly);
			cam_dma_lines = lh;
		}
	}
	wr32(CAM_SV_BASE + 0x230, cam_pitch - 1);
	wr32(CAM_SV_BASE + 0x234, cam_dma_lines - 1);
	wr32(CAM_SV_BASE + 0x238, (rd32(CAM_SV_BASE + 0x238) & 0xffff0000u) | cam_pitch);
	dsb();
	/* two buffers queued as the HAL's CAMSV_BUF_CTRL::enqueueHwBuf does: data address, header address, THEN the ready pulse (+0x2c bit 0) */
	if (cam_legacy & 1) {
		wr32(CAM_SENINF_BASE + 0x3d00, rd32(CAM_SENINF_BASE + 0x3d00) | 0x100u);
	}
	if (cam_legacy & 2) {
		wr32(CAM_SENINF_BASE + 0x3d3c, 0x101u);
	}
	if (cam_legacy & 4) {
		/* CAMSV_1 +0x240/+0x244 as the idle CAMSV_0/2/3 have them (0x800080); the PD channel's table gave them 0x40004/0x20002 */
		wr32(CAM_SV_BASE + 0x240, 0x00800080u);
		wr32(CAM_SV_BASE + 0x244, 0x00800080u);
	}
	dsb();
	if (cam_raw10) {
		/* CAMSV_1 +0x4c bits 5:4 pick which 8 of the pixel's bits the byte-a-pixel DMA takes (0x20: the top eight); bit 0 set: the DMA gets 10 bits a pixel,
		 * a continuous little-endian bit stream (5100 bytes a line; the bytes of lines 0 and 1536 decode to 72 66 72 68 ... and 95 76 89 ...) */
		wr32(CAM_SV_BASE + 0x4c, rd32(CAM_SV_BASE + 0x4c) | 1u);
		dsb();
		say("RAW10: CAMSV_1 +0x4c %#x", rd32(CAM_SV_BASE + 0x4c));
	}
	if (cam_fa && cam_fm) {
		uint32_t old = rd32(CAM_SV_BASE + cam_fa), now = (old & ~(cam_fm << cam_fs)) | ((cam_variant & cam_fm) << cam_fs);

		wr32(CAM_SV_BASE + cam_fa, now);
		dsb();
		say("sweep: CAMSV_1 +%#x %#x -> %#x (reads %#x)", cam_fa, old, now, rd32(CAM_SV_BASE + cam_fa));
	}
	say("variant %u: stride %u; mux3 +0x00 %#x, +0x3c %#x; CAMSV_1 +0x240 %#x, +0x244 %#x", cam_variant, cam_pitch, rd32(CAM_SENINF_BASE + 0x3d00),
	    rd32(CAM_SENINF_BASE + 0x3d3c), rd32(CAM_SV_BASE + 0x240), rd32(CAM_SV_BASE + 0x244));
	for (unsigned k = 0; k < (cam_serve ? CAM_NBUF_MAX : 2u); k++) {
		uint64_t img = cam_pa + CAM_IMG_OFF(k), hdr = cam_pa + CAM_HDR_OFF(k);

		wr32(CAM_SV_BASE + 0x118, (uint32_t)img);
		wr32(CAM_SV_BASE + 0xe04, (uint32_t)hdr);
		dsb();
		wr32(CAM_SV_BASE + 0x2c, rd32(CAM_SV_BASE + 0x2c) | 1u);
		dsb();
		delay(1000);
	}
	if (!(cam_legacy & 16)) {
		cam_rx_late();
	}
	delay(2000);
	for (uint32_t i = 0; i < CAM_SENINF_WORDS; i++) {
		cam_seninf_before[i] = rd32(CAM_SENINF_BASE + 4 * i);
	}
	for (uint32_t i = 0; i < CAM_SV_WORDS; i++) {
		cam_sv_before[i] = rd32(CAM_SV_BASE + 4 * i);
	}
}

/*
 * What a buffer holds, line by line (RAW10 packed: 4 pixels in 5 bytes, bytes 0..3 the high 8 bits of each pixel, byte 4 the
 * low 2 bits of all four): how many lines have a non-zero byte, the high bytes' min/max/mean over the whole frame (every 16th line)
 * and over lines 0..63, and a few lines' bytes.
 */
/* a 64x48 picture of a frame held at a byte a pixel: each cell is the mean of 16 samples inside its 63x64-pixel block; one log line a row,
 * "THUMB nn: " and 64 hex pairs, for tools/camthumb.py to make a PNG of (a whole 12 MB frame does not fit the log) */
static void
cam_thumb(const char *name, volatile uint8_t *base)
{
	char row[64 * 2 + 1];
	static const char hex[] = "0123456789abcdef";

	say("%s: thumbnail 64x48 follows", name);
	for (uint32_t cy = 0; cy < 48; cy++) {
		for (uint32_t cx = 0; cx < 64; cx++) {
			uint32_t sum = 0, v;

			for (uint32_t sy = 0; sy < 4; sy++) {
				for (uint32_t sx = 0; sx < 4; sx++) {
					uint32_t y = cy * 64 + 8 + sy * 16, x = (cx * 63 + 4 + sx * 14) & ~1u;     /* (even: one colour of the cell, no stripes) */

					if (cam_pitch == 5100u) {       /* 10 bits a pixel, a little-endian bit stream: the top eight bits of the pixel */
						volatile uint8_t *q = base + (uint64_t)y * cam_pitch + (x * 10) / 8;
						uint32_t w = (uint32_t)q[0] | ((uint32_t)q[1] << 8);

						sum += ((w >> ((x * 10) % 8)) & 0x3ff) >> 2;
					} else {
						sum += base[(uint64_t)y * cam_pitch + x];
					}
				}
			}
			v = sum / 16;
			row[cx * 2] = hex[(v >> 4) & 15];
			row[cx * 2 + 1] = hex[v & 15];
		}
		row[128] = 0;
		say("THUMB %02u: %s", cy, row);
	}
}

static uint32_t
cam_stats(const char *name, volatile uint8_t *base)
{
	uint32_t written = 0, first = 0xffffffffu, last = 0, mn = 255, mx = 0;
	uint64_t sum = 0, cnt = 0, sum_all = 0, cnt_all = 0, sum4 = 0, cnt4 = 0;

	for (uint32_t y = 0; y < CAM_LINES; y++) {
		volatile uint8_t *p = base + (uint64_t)y * cam_pitch;
		bool any = false;

		for (uint32_t x = 0; x < cam_pitch; x += 16) {
			if (p[x] | p[x + 1] | p[x + 2] | p[x + 3]) {
				any = true;
				break;
			}
		}
		if (any) {
			written++;
			if (first == 0xffffffffu) {
				first = y;
			}
			last = y;
		}
		if (y < 64 || y % 16 == 0) {
			for (uint32_t x = 0; x < CAM_PITCH; x++) {      /* (the padding of a wider stride is not data) */
				uint32_t v;

				if (x % 5 == 4) {
					if (y < 64) {
						sum4 += p[x];
						cnt4++;
					}
					continue;
				}
				v = p[x];
				if (y < 64) {
					if (v < mn) {
						mn = v;
					}
					if (v > mx) {
						mx = v;
					}
					sum += v;
					cnt++;
				}
				sum_all += v;
				cnt_all++;
			}
		}
	}
	say("%s: %u of %u lines written (first %u, last %u); high bytes lines 0..63: min %u max %u mean %u; whole frame (every 16th line) mean %u",
	    name, written, CAM_LINES, first == 0xffffffffu ? 0 : first, last, mn, mx, cnt ? (uint32_t)(sum / cnt) : 0,
	    cnt_all ? (uint32_t)(sum_all / cnt_all) : 0);
	say("%s: every fifth byte (lines 0..63) mean %u: a byte a pixel makes it equal to the others (%u), four pixels in five bytes makes it the low bits (anything, mean ~127)",
	    name, cnt4 ? (uint32_t)(sum4 / cnt4) : 0, cnt ? (uint32_t)(sum / cnt) : 0);
	for (unsigned r = 0; r < 4; r++) {
		static const uint32_t rows[4] = { 0, 1, 1536, 3071 };
		volatile uint8_t *p = base + (uint64_t)rows[r] * cam_pitch + 2000;

		say("  line %u [2000..2011]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", rows[r],
		    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
	}
	return written;
}

/* does the second buffer's first line hold anything yet: the first frame is then whole */
static bool
cam_second_started(void)
{
	volatile uint8_t *p = cam_img(1);

	for (unsigned i = 0; i < 64; i++) {
		if (p[i]) {
			return true;
		}
	}
	return false;
}

/* ---- /dev/redwood-camera (mtkcam=5): the frames of the stream, as RAW10 for the userland's ISP ---- */

#define CAM_IDLE_MS     4000u           /* no read for this long: the stream stops and everything is switched off */
#define CAM_BLACK10     64u             /* the black level in 10-bit steps (Android's blackLevelPattern) */
#define CAM_BAYER       2u              /* GBRG: android.sensor.info.colorFilterArrangement of the rear camera (dumpsys media.camera) */
#define CAM_RAW10_STRIDE 5100u          /* 4080 pixels, four in five bytes */
#define CAM_GAIN_UNITY  0x100u          /* ASSUMPTION (8.8 fixed point, as Samsung S5K sensors use it): the 1x of register 0x0204. Run 510: the signal above black doubles with every doubling from 0x200 to 0x800 (no saturation). 16x = 0x1000 = CAM_GAIN_MAX fits Android's maxAnalogSensitivity 1600 (ISO 100 = 1x) */
#define CAM_GAIN_MAX    0x1000u
#define CAM_FRAME_MIN   0x0d9eu         /* the table's first frame length (0x0340); the table ends with 0x1c9e */
#define CAM_FRAME_MAX   0xfff0u
#define CAM_EXP_MARGIN  8u              /* ASSUMPTION: exposure lines at most frame length minus this */

static LCK_GRP_DECLARE(cam_lck_grp, "redwood-mtkcam");
static LCK_MTX_DECLARE(cam_lock, &cam_lck_grp);
enum { SLOT_FILL, SLOT_READY, SLOT_READING, SLOT_FREE };
static struct {
	int             state;
	uint64_t        sequence, time;
	uint32_t        exposure, gain, frame_lines;
} cam_slot[CAM_NBUF_MAX];
static bool cam_dev_made, cam_dev_wanted, cam_dev_reading, cam_broken, cam_streaming;
static int cam_dev_opens;
static uint64_t cam_seq, cam_given, cam_last_read_ms, cam_line_ns;
struct cam_askst {
	bool            pending;
	uint32_t        exposure, gain, frame_lines;
};
static struct cam_askst cam_ask;
static uint32_t cam_cur_exposure = 0x1c9a, cam_cur_gain = 0x800, cam_cur_frame = 0x1c9e;       /* what the table leaves in the sensor */
static uint8_t cam_chunk[CAM_RAW10_STRIDE * 8];

static uint64_t
cam_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000u;
}

static bool
cam_tail_written(unsigned k)
{
	volatile uint64_t *p = (volatile uint64_t *)(cam_img(k) + (uint64_t)cam_pitch * cam_dma_lines - 64);
	uint64_t o = 0;

	cam_inval(p, 64);
	for (unsigned i = 0; i < 8; i++) {
		o |= p[i];
	}
	return o != 0;
}

static void
cam_enqueue(unsigned k)
{
	uint64_t img = cam_pa + CAM_IMG_OFF(k), hdr = cam_pa + CAM_HDR_OFF(k);
	volatile uint64_t *tail = (volatile uint64_t *)(cam_img(k) + (uint64_t)cam_pitch * cam_dma_lines - 64);

	for (unsigned i = 0; i < 8; i++) {
		tail[i] = 0;
	}
	cam_inval(tail, 64);
	dsb();
	wr32(CAM_SV_BASE + 0x118, (uint32_t)img);
	wr32(CAM_SV_BASE + 0xe04, (uint32_t)hdr);
	dsb();
	wr32(CAM_SV_BASE + 0x2c, rd32(CAM_SV_BASE + 0x2c) | 1u);
	dsb();
}

/* the stream is on: finished frames go to "ready", the ones read go back to the DMA's queue, until nobody reads */
static void
cam_serve_loop(void)
{
	uint64_t t0 = cam_ms(), last_time = 0;
	uint32_t errors = 0;
	bool stall_said = false;
	uint64_t cam_last_frame_ms = cam_ms();

	lck_mtx_lock(&cam_lock);
	cam_streaming = true;
	cam_seq = cam_given = 0;
	for (unsigned k = 0; k < CAM_NBUF_MAX; k++) {
		cam_slot[k].state = SLOT_FILL;
	}
	cam_cur_exposure = 0x1c9a;
	cam_cur_gain = 0x800;
	cam_cur_frame = 0x1c9e;
	lck_mtx_unlock(&cam_lock);
	say("serving: the stream is on");
	for (;;) {
		bool stop;
		struct cam_askst ask;

		lck_mtx_lock(&cam_lock);
		stop = cam_dev_opens == 0 || (cam_ms() - cam_last_read_ms > CAM_IDLE_MS && !cam_dev_reading);
		for (unsigned k = 0; k < CAM_NBUF_MAX; k++) {
			if (cam_slot[k].state == SLOT_FILL && cam_tail_written(k)) {
				uint64_t now = mach_absolute_time();

				delay(2000);
				cam_slot[k].state = SLOT_READY;
				cam_slot[k].sequence = ++cam_seq;
				cam_slot[k].time = now;
				cam_slot[k].exposure = cam_cur_exposure;
				cam_slot[k].gain = cam_cur_gain;
				cam_slot[k].frame_lines = cam_cur_frame;
				if (last_time && cam_seq >= 2) {
					uint64_t ns;

					absolutetime_to_nanoseconds(now - last_time, &ns);
					cam_line_ns = ns / cam_cur_frame;
				}
				last_time = now;
				cam_last_frame_ms = cam_ms();
				stall_said = false;
				if (cam_seq <= 3) {
					say("serving: frame %llu whole at %llu ms (a line is about %llu ns)", cam_seq, cam_ms() - t0, cam_line_ns);
				}
				wakeup(&cam_seq);
			}
		}
		/* a frame older than the newest ready one is of no use: its place goes back to the DMA */
		{
			int newest = -1;

			for (unsigned k = 0; k < CAM_NBUF_MAX; k++) {
				if (cam_slot[k].state == SLOT_READY && (newest < 0 || cam_slot[k].sequence > cam_slot[newest].sequence)) {
					newest = (int)k;
				}
			}
			for (unsigned k = 0; k < CAM_NBUF_MAX; k++) {
				if (cam_slot[k].state == SLOT_READY && (int)k != newest) {
					cam_slot[k].state = SLOT_FREE;
				}
			}
		}
		for (unsigned k = 0; k < CAM_NBUF_MAX; k++) {
			if (cam_slot[k].state == SLOT_FREE) {
				cam_enqueue(k);
				cam_slot[k].state = SLOT_FILL;
			}
		}
		if (cam_seq && cam_ms() - cam_last_frame_ms > 1000 && !stall_said) {
			stall_said = true;
			say("serving: NO FRAME FOR 1 s (frame %llu): slots %d %d %d; CAMSV_1 INT %#x %#x, DMA error %#x %#x, FBC %#x, TG %#x %#x %#x %#x, +0x2c %#x, +0x118 %#x",
			    cam_seq, cam_slot[0].state, cam_slot[1].state, cam_slot[2].state, rd32(CAM_SV_BASE + 0x1c), rd32(CAM_SV_BASE + 0x20),
			    rd32(CAM_SV_BASE + 0x440), rd32(CAM_SV_BASE + 0x444), rd32(CAM_SV_BASE + 0x114), rd32(CAM_SV_BASE + 0x500),
			    rd32(CAM_SV_BASE + 0x504), rd32(CAM_SV_BASE + 0x538), rd32(CAM_SV_BASE + 0x57c), rd32(CAM_SV_BASE + 0x2c), rd32(CAM_SV_BASE + 0x118));
		}
		if (rd32(CAM_SV_BASE + 0x440) && errors++ < 3) {
			say("serving: DMA error flags %#x %#x (frame %llu)", rd32(CAM_SV_BASE + 0x440), rd32(CAM_SV_BASE + 0x444), cam_seq);
		}
		ask = cam_ask;
		cam_ask.pending = false;
		lck_mtx_unlock(&cam_lock);
		if (ask.pending) {
			uint32_t frame = ask.frame_lines ? ask.frame_lines : cam_cur_frame;

			if (frame < CAM_FRAME_MIN) {
				frame = CAM_FRAME_MIN;
			}
			if (frame > CAM_FRAME_MAX) {
				frame = CAM_FRAME_MAX;
			}
			if (ask.exposure < 1) {
				ask.exposure = 1;
			}
			if (ask.exposure + CAM_EXP_MARGIN > frame) {
				if (ask.frame_lines == 0 && ask.exposure + CAM_EXP_MARGIN <= CAM_FRAME_MAX) {
					frame = ask.exposure + CAM_EXP_MARGIN;      /* (0: the shortest frame that holds the exposure) */
				} else {
					ask.exposure = frame - CAM_EXP_MARGIN;
				}
			}
			if (ask.gain > CAM_GAIN_MAX) {
				ask.gain = CAM_GAIN_MAX;
			}
			cam_bb_write_reg(0x10, 0x0340, (uint16_t)frame, 2);
			cam_bb_write_reg(0x10, 0x0202, (uint16_t)ask.exposure, 2);
			cam_bb_write_reg(0x10, 0x0204, (uint16_t)ask.gain, 2);
			lck_mtx_lock(&cam_lock);
			cam_cur_frame = frame;
			cam_cur_exposure = ask.exposure;
			cam_cur_gain = ask.gain;
			lck_mtx_unlock(&cam_lock);
		}
		if (stop) {
			break;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	lck_mtx_lock(&cam_lock);
	cam_streaming = false;
	lck_mtx_unlock(&cam_lock);
	say("serving: nobody reads (%u open): the stream stops after %llu frames", (unsigned)cam_dev_opens, cam_seq);
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (cam_broken) {
		return EIO;
	}
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	lck_mtx_lock(&cam_lock);
	cam_dev_opens++;
	lck_mtx_unlock(&cam_lock);
	say("/dev/redwood-camera opened by %s (pid %d) at uptime %llu ms, %d open", name, proc_selfpid(), cam_ms(), cam_dev_opens);
	return 0;
}

static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	lck_mtx_lock(&cam_lock);
	if (cam_dev_opens > 0) {
		cam_dev_opens--;
	}
	lck_mtx_unlock(&cam_lock);
	say("/dev/redwood-camera closed by %s (pid %d) at uptime %llu ms, %d open, frame %llu given", name, proc_selfpid(), cam_ms(), cam_dev_opens, cam_given);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_frame f;
	uint64_t since = cam_ms();
	int b, error = 0;

	lck_mtx_lock(&cam_lock);
	if (cam_dev_reading) {
		lck_mtx_unlock(&cam_lock);
		return EBUSY;
	}
	cam_dev_reading = true;
	cam_last_read_ms = cam_ms();
	if (cam_given == 0 || cam_ms() - since > 0) {
		static uint32_t said;

		if (said++ < 6) {
			say("read at uptime %llu ms by pid %d: %lld bytes asked", cam_ms(), proc_selfpid(), (long long)uio_resid(uio));
		}
	}
	for (;;) {
		if (cam_broken) {
			error = EIO;
			break;
		}
		b = -1;
		for (int i = 0; i < (int)CAM_NBUF_MAX; i++) {
			if (cam_slot[i].state == SLOT_READY && cam_slot[i].sequence > cam_given && (b < 0 || cam_slot[i].sequence > cam_slot[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0 && cam_streaming) {
			break;
		}
		if (!cam_streaming && !cam_dev_wanted) {
			cam_dev_wanted = true;
			wakeup(&cam_dev_wanted);
		}
		if (cam_ms() - since > (cam_streaming ? 3000u : 20000u)) {
			error = EAGAIN;
			break;
		}
		struct timespec ts = { 0, 50 * NSEC_PER_MSEC };
		error = msleep(&cam_seq, &cam_lock, PZERO | PCATCH, "redwood-mtkcam", &ts);
		if (error == EWOULDBLOCK) {
			error = 0;
		}
		if (error) {
			break;
		}
		cam_last_read_ms = cam_ms();
	}
	if (error) {
		cam_dev_reading = false;
		lck_mtx_unlock(&cam_lock);
		return error;
	}
	cam_slot[b].state = SLOT_READING;
	cam_given = cam_slot[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, REDWOOD_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = 4080;
	f.height = cam_dma_lines & ~1u;         /* (even: whole Bayer cells) */
	f.stride = CAM_RAW10_STRIDE;
	f.format = 1;
	f.bayer = CAM_BAYER;
	f.black = CAM_BLACK10;
	f.sequence = cam_slot[b].sequence;
	f.time = cam_slot[b].time;
	f.exposure_lines = cam_slot[b].exposure;
	f.line_ns = (uint32_t)cam_line_ns;
	f.gain = cam_slot[b].gain;
	f.gain_unity = CAM_GAIN_UNITY;
	f.gain_max = CAM_GAIN_MAX;
	f.frame_lines = cam_slot[b].frame_lines;
	f.frame_lines_min = CAM_FRAME_MIN;
	f.frame_lines_max = CAM_FRAME_MAX;
	f.margin = CAM_EXP_MARGIN;
	f.sensor = 1;
	lck_mtx_unlock(&cam_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		/* the frame holds 10 bits a pixel as a little-endian bit stream (or, mtkcam_raw8=1, a byte a pixel); the RAW10 the userland takes is four
		 * pixels in five bytes: the high 8 bits of each, then a byte of their low 2 bits */
		volatile uint8_t *src = cam_img((unsigned)b);
		uint64_t t0 = mach_absolute_time(), t1;

		cam_inval(src, (uint64_t)cam_pitch * cam_dma_lines);
		for (uint32_t y = 0; y < (cam_dma_lines & ~1u) && error == 0 && uio_resid(uio) > 0; y += 8) {
			uint32_t n = MIN(8u, (cam_dma_lines & ~1u) - y);

			for (uint32_t l = 0; l < n; l++) {
				volatile uint8_t *s8 = src + (uint64_t)(y + l) * cam_pitch;
				uint8_t *d = cam_chunk + l * CAM_RAW10_STRIDE;

				if (cam_pitch == 5100u) {
					for (uint32_t i = 0; i < 1020; i++) {
						uint64_t v = (uint64_t)s8[0] | ((uint64_t)s8[1] << 8) | ((uint64_t)s8[2] << 16) | ((uint64_t)s8[3] << 24) | ((uint64_t)s8[4] << 32);
						uint32_t p0 = (uint32_t)(v & 0x3ff), p1 = (uint32_t)((v >> 10) & 0x3ff), p2 = (uint32_t)((v >> 20) & 0x3ff), p3 = (uint32_t)((v >> 30) & 0x3ff);

						d[0] = (uint8_t)(p0 >> 2);
						d[1] = (uint8_t)(p1 >> 2);
						d[2] = (uint8_t)(p2 >> 2);
						d[3] = (uint8_t)(p3 >> 2);
						d[4] = (uint8_t)((p0 & 3) | ((p1 & 3) << 2) | ((p2 & 3) << 4) | ((p3 & 3) << 6));
						s8 += 5;
						d += 5;
					}
				} else {
					volatile uint32_t *s = (volatile uint32_t *)s8;

					for (uint32_t i = 0; i < cam_pitch / 4; i++) {
						uint32_t v = s[i];

						d[i * 5 + 0] = (uint8_t)v;
						d[i * 5 + 1] = (uint8_t)(v >> 8);
						d[i * 5 + 2] = (uint8_t)(v >> 16);
						d[i * 5 + 3] = (uint8_t)(v >> 24);
						d[i * 5 + 4] = 0;
					}
				}
			}
			error = uiomove((caddr_t)cam_chunk, (int)MIN((user_ssize_t)(n * CAM_RAW10_STRIDE), uio_resid(uio)), uio);
		}
		t1 = mach_absolute_time();
		if (f.sequence <= 3) {
			uint64_t ns;

			absolutetime_to_nanoseconds(t1 - t0, &ns);
			say("serving: frame %llu read out in %llu ms", f.sequence, ns / 1000000u);
		}
		if (f.sequence == 2 || f.sequence == 20) {
			uint32_t th = 0;

			if (PE_parse_boot_argn("mtkcam_thumb", &th, sizeof(th)) && th) {
				cam_thumb("served frame", src);
			}
		}
	}
	lck_mtx_lock(&cam_lock);
	if (cam_slot[b].state == SLOT_READING) {
		cam_slot[b].state = SLOT_FREE;
	}
	cam_dev_reading = false;
	cam_last_read_ms = cam_ms();
	lck_mtx_unlock(&cam_lock);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	if (c.command == REDWOOD_CAMERA_EXPOSURE) {
		lck_mtx_lock(&cam_lock);
		cam_ask.exposure = c.a;
		cam_ask.gain = c.b;
		cam_ask.frame_lines = c.c;
		cam_ask.pending = true;
		lck_mtx_unlock(&cam_lock);
		return 0;
	}
	if (c.command == REDWOOD_CAMERA_KEEP) {
		return ENOSPC;                  /* (no memory in one piece is kept for the host here) */
	}
	if (c.command == REDWOOD_CAMERA_SENSOR) {
		return c.a == 1 ? 0 : EINVAL;   /* only the wide (main) camera is brought up */
	}
	return EINVAL;
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- the whole capture ---- */
static void
cam_capture(void)
{
	struct cam_pin_state avdd, pdn, rst, mclk, scl, sda;
	struct cam_sysst st;
	uint16_t hwcid = 0, e1 = 0, e2 = 0, vosel_before = 0, vosel = 0;
	uint32_t cfg1_before;
	bool have_io = false, have_d = false, up;
	uint8_t hi = 0, lo = 0;
	int m1, m2;

	say("capture: the main camera (S5K JN1), variant %u", cam_variant);
	if (!pwrap_read(MT6358_HWCID, &hwcid) || hwcid < 0x5830 || hwcid > 0x5832) {
		say("the PMIC is not the one this was written for (HWCID %#x): stopping", hwcid);
		return;
	}
	if (!pwrap_read(CAM_VCAMIO_EN, &e1) || !pwrap_read(CAM_VCAMD_EN, &e2) || (e1 & 1u) || (e2 & 1u) ||
	    !pwrap_read(CAM_VCAMD_VOSEL, &vosel_before)) {
		say("a camera rail is already on or unreadable (VCAMIO %#x, VCAMD %#x): stopping", e1, e2);
		return;
	}
	say("PMIC %#x; rails off; VCAMD's voltage register %#x", hwcid, vosel_before);

	say("stage 1: the camera subsystem");
	up = cam_sys_up(&st);
	if (!up || cam_bad_pa) {
		say("the camera subsystem did not come up (an address no window covers: %#llx): stopping", (unsigned long long)cam_bad_pa);
		if (up) {
			cam_sys_down(&st);
		}
		return;
	}

	cam_pin_save(&avdd, CAM_PIN_AVDD);
	cam_pin_save(&pdn, CAM_PIN_PDN);
	cam_pin_save(&rst, CAM_PIN_RST);
	cam_pin_save(&mclk, CAM_PIN_MCLK);
	cam_pin_save(&scl, CAM_BB_SCL);
	cam_pin_save(&sda, CAM_BB_SDA);
	cfg1_before = rd32(TOPCKGEN_BASE + CAM_CFG1);

	say("stage 2: pins quiet (reset, power-down, analog supply low; MCLK a plain pin), camtg_sel and camtg1_sel open");
	cam_pin_out(CAM_PIN_RST, 0);
	cam_pin_out(CAM_PIN_PDN, 0);
	cam_pin_out(CAM_PIN_AVDD, 0);
	cam_pin_mode(CAM_PIN_MCLK, 0);
	cam_pin_out(CAM_PIN_MCLK, 0);
	delay(1000);
	wr32(TOPCKGEN_BASE + CAM_CFG1_CLR, CAM_CFG1_CAMTG_PDN | CAM_CFG1_CAMTG1_PDN);
	dsb();

	say("VCAMIO (1.8 V) on");
	have_io = cam_rail(CAM_VCAMIO_EN, true);
	if (!have_io) {
		say("  DOES NOT READ BACK: stopping");
		goto down;
	}
	delay(500);
	say("VCAMD voltage %#x -> 0x0500 (1.1 V, as Android)", vosel_before);
	pwrap_write(CAM_VCAMD_VOSEL, CAM_VCAMD_1V1);
	delay(200);
	if (!pwrap_read(CAM_VCAMD_VOSEL, &vosel) || vosel != CAM_VCAMD_1V1) {
		say("  READS BACK %#x: VCAMD stays off, stopping", vosel);
		goto down;
	}
	say("  reads back 0x0500; VCAMD on");
	have_d = cam_rail(CAM_VCAMD_EN, true);
	if (!have_d) {
		say("  DOES NOT READ BACK: stopping");
		goto down;
	}
	delay(1500);
	cam_pin_out(CAM_PIN_AVDD, 1);
	delay(1000);
	say("analog supply GPIO23 high (reads %d); reset GPIO102 high", cam_pin_read(CAM_PIN_AVDD));
	cam_pin_out(CAM_PIN_RST, 1);
	delay(1000);
	say("  reset reads %d; MCLK: GPIO100 to function 1", cam_pin_read(CAM_PIN_RST));
	cam_pin_mode(CAM_PIN_MCLK, 1);
	delay(12000);

	say("stage 3: the sensor by hand on GPIO105/106 (were function %u and %u)", scl.mode, sda.mode);
	cam_pin_mode(CAM_BB_SCL, 0);
	cam_pin_mode(CAM_BB_SDA, 0);
	cam_bb_line(CAM_BB_SCL, 1);
	cam_bb_line(CAM_BB_SDA, 1);
	delay(50);
	say("bus idle: SCL reads %d, SDA reads %d (1 and 1 expected)", cam_pin_read(CAM_BB_SCL), cam_pin_read(CAM_BB_SDA));
	m1 = cam_bb_read8(0x10, 0x0000, &hi);
	m2 = cam_bb_read8(0x10, 0x0001, &lo);
	if (m1 || m2) {
		say("the sensor did not answer (acknowledges missed: %#x, %#x): stopping", m1, m2);
		goto bus;
	}
	say("sensor ID %#x%s", ((uint32_t)hi << 8) | lo, ((hi << 8) | lo) == 0x38e1 ? " = S5K JN1" : " (NOT 0x38e1): stopping");
	if (((hi << 8) | lo) != 0x38e1) {
		goto bus;
	}

	say("stage 4: the sensor's table, %d writes", CAM_JN1_OPS);
	{
		int bad = -1;

		for (int i = 0; i < CAM_JN1_OPS; i++) {
			if (!cam_bb_write_reg(0x10, cam_jn1_table[i].reg, cam_jn1_table[i].val, cam_jn1_table[i].width)) {
				bad = i;
				break;
			}
			if (cam_jn1_table[i].pause_us) {
				delay((int)cam_jn1_table[i].pause_us);
			}
		}
		if (bad >= 0) {
			say("  a write was not acknowledged: number %d, register %#x: stopping", bad, cam_jn1_table[bad].reg);
			goto bus;
		}
		say("  all written and acknowledged");
	}

	say("stage 5: the receiver (SENINF, MIPI PHY, CAMSV_1) and the frame memory");
	cam_rx_program();
	if (cam_bad_pa) {
		say("an address no window covers: %#llx: stopping", (unsigned long long)cam_bad_pa);
		goto stream_off;
	}

	{
		uint8_t mode = 0xff, c1 = 0xff, c2 = 0xff, c3 = 0xff;
		uint32_t waited = 0;
		bool whole = false;

		{
			/* boot-arg mtkcam_ll=N: the sensor's line length in pixel clocks (register 0x0342; the table has 0x1450). A longer line is a
			 * longer pause between lines: a test whether the cut at 2452 lines is a time, a rate, or a place in the frame. */
			uint32_t ll = 0;

			if (PE_parse_boot_argn("mtkcam_ll", &ll, sizeof(ll)) && ll) {
				say("  line length (0x0342) <- %#x (table: 0x1450)", ll);
				cam_bb_write_reg(0x10, 0x0342, (uint16_t)ll, 2);
			}
		}
		if (cam_gain_arg) {
			say("  gain register 0x0204 <- %#x", cam_gain_arg << cam_variant);
			cam_bb_write_reg(0x10, 0x0204, (uint16_t)(cam_gain_arg << cam_variant), 2);
		}
		say("stage 6: stream on (register 0x0100 <- 1)");
		cam_bb_write_reg(0x10, 0x0100, 1, 1);
		if (cam_serve) {
			cam_stream_ok = true;
			cam_serve_loop();
			goto stream_off;
		}
		if (cam_legacy & 16) {
			/* variant 16: the sensor streams for a while first; the receiver is let in then, at the start of a frame it has not missed */
			delay(100000);
			say("  variant 16: the receiver's TG/VF on now (sensor streaming for 100 ms)");
			cam_rx_late();
			delay(2000);
			for (uint32_t i = 0; i < CAM_SENINF_WORDS; i++) {
				cam_seninf_before[i] = rd32(CAM_SENINF_BASE + 4 * i);
			}
			for (uint32_t i = 0; i < CAM_SV_WORDS; i++) {
				cam_sv_before[i] = rd32(CAM_SV_BASE + 4 * i);
			}
		}
		delay(40000);
		cam_bb_read8(0x10, 0x0100, &mode);
		cam_bb_read8(0x10, 0x0005, &c1);
		delay(30000);
		cam_bb_read8(0x10, 0x0005, &c2);
		delay(30000);
		cam_bb_read8(0x10, 0x0005, &c3);
		say("  mode register %#x, frame counter %#x %#x %#x%s", mode, c1, c2, c3, c1 != c2 || c2 != c3 ? " <- THE SENSOR STREAMS" : " (the counter does not move)");
		/* the first frame is whole when the second has begun to arrive; at most 700 ms */
		while (waited < 700) {
			if (cam_second_started()) {
				whole = true;
				break;
			}
			delay(10000);
			waited += 10;
		}
		delay(5000);
		say("  frame 1 %s (waited %u ms more)", whole ? "is whole: the second has begun" : "NOT confirmed whole (the second never began)", waited);
		{
			uint32_t changed_in = 0, changed_sv = 0;

			for (uint32_t i = 0; i < CAM_SENINF_WORDS; i++) {
				changed_in += rd32(CAM_SENINF_BASE + 4 * i) != cam_seninf_before[i];
			}
			for (uint32_t i = 0; i < CAM_SV_WORDS; i++) {
				changed_sv += rd32(CAM_SV_BASE + 4 * i) != cam_sv_before[i];
			}
			say("  registers that changed since written: SENINF %u, CAMSV_1 %u", changed_in, changed_sv);
		}
		say("  SENINF mux3 status +0x3d30 %#x, mux0 +0xd30 %#x", rd32(CAM_SENINF_BASE + 0x3d30), rd32(CAM_SENINF_BASE + 0xd30));
		say("  CAMSV_1: INT +0x1c %#x, +0x20 %#x, DMA error +0x440 %#x +0x444 %#x, FBC +0x114 %#x, TG lines +0x57c %#x",
		    rd32(CAM_SV_BASE + 0x1c), rd32(CAM_SV_BASE + 0x20), rd32(CAM_SV_BASE + 0x440), rd32(CAM_SV_BASE + 0x444),
		    rd32(CAM_SV_BASE + 0x114), rd32(CAM_SV_BASE + 0x57c));
		{
			uint32_t w0 = cam_stats("image 0", cam_img(0));
			if (cam_pitch == 4080u || cam_pitch == 5100u) {
				cam_thumb("image 0", cam_img(0));
			}
			uint32_t w1 = cam_stats("image 1", cam_img(1));

			if (w0 >= CAM_LINES - 1) {
				say("RESULT[%u]: a FULL FRAME (4080x3072 RAW10, %u of %u lines) reached memory from XNU", cam_variant, w0, CAM_LINES);
			} else if (w0 || w1) {
				say("RESULT[%u]: PARTIAL (%u / %u lines): the receiver works, the frame is not whole", cam_variant, w0, w1);
			} else {
				say("RESULT[%u]: NOTHING in memory", cam_variant);
			}
		}
	}

stream_off:
	say("stream off (register 0x0100 <- 0)");
	{
		uint8_t mode = 0xff;

		cam_bb_write_reg(0x10, 0x0100, 0, 1);
		delay(50000);
		cam_bb_read8(0x10, 0x0100, &mode);
		say("  mode register now %#x", mode);
	}
bus:
	cam_pin_restore(&sda);
	cam_pin_restore(&scl);
down:
	say("switching it all off again");
	cam_pin_mode(CAM_PIN_MCLK, 0);
	cam_pin_out(CAM_PIN_RST, 0);
	delay(500);
	cam_pin_out(CAM_PIN_AVDD, 0);
	delay(500);
	if (have_d) {
		say("  VCAMD off: %s", cam_rail(CAM_VCAMD_EN, false) ? "ok" : "NO");
	}
	pwrap_write(CAM_VCAMD_VOSEL, vosel_before);
	if (have_io) {
		say("  VCAMIO off: %s", cam_rail(CAM_VCAMIO_EN, false) ? "ok" : "NO");
	}
	if (cfg1_before & CAM_CFG1_CAMTG_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG1_SET, CAM_CFG1_CAMTG_PDN);
	}
	if (cfg1_before & CAM_CFG1_CAMTG1_PDN) {
		wr32(TOPCKGEN_BASE + CAM_CFG1_SET, CAM_CFG1_CAMTG1_PDN);
	}
	cam_pin_restore(&mclk);
	cam_pin_restore(&rst);
	cam_pin_restore(&pdn);
	cam_pin_restore(&avdd);
	dsb();
	say("pins and clocks put back; CLK_CFG_1 %#x", rd32(TOPCKGEN_BASE + CAM_CFG1));
	cam_sys_down(&st);
	say("capture: done");
}

static uint32_t cam_level;

static void
mtkcam_thread(__unused void *parameter, __unused wait_result_t wr)
{
	uint32_t start = 10;

	(void)PE_parse_boot_argn("mtkcam_start", &start, sizeof(start));
	(void)PE_parse_boot_argn("mtkcam_camsel", &cam_camsel_arg, sizeof(cam_camsel_arg));
	(void)PE_parse_boot_argn("mtkcam_mmsel", &cam_mmsel_arg, sizeof(cam_mmsel_arg));
	(void)PE_parse_boot_argn("mtkcam_larb", &cam_larb_arg, sizeof(cam_larb_arg));
	(void)PE_parse_boot_argn("mtkcam_fa", &cam_fa, sizeof(cam_fa));
	(void)PE_parse_boot_argn("mtkcam_fm", &cam_fm, sizeof(cam_fm));
	(void)PE_parse_boot_argn("mtkcam_fs", &cam_fs, sizeof(cam_fs));
	(void)PE_parse_boot_argn("mtkcam_gain", &cam_gain_arg, sizeof(cam_gain_arg));
	(void)PE_parse_boot_argn("mtkcam_xp", &cam_xp, sizeof(cam_xp));
	{
		uint32_t r10 = 0;

		if (PE_parse_boot_argn("mtkcam_raw10", &r10, sizeof(r10)) && r10) {
			cam_raw10 = true;
		}
	}
	if (cam_fa >= 0x1000 || (cam_fa & 3) || cam_fs > 31) {
		cam_fa = cam_fm = 0;
	}
	{
		uint32_t buf = 0;

		if (PE_parse_boot_argn("mtkcam_buf", &buf, sizeof(buf)) && buf >= 0x50000000u && buf <= 0x72000000u && !(buf & 0xfffff)) {
			cam_pa = buf;
		}
	}
	for (uint32_t s = 0; s < (cam_level >= 5 ? 0 : start); s++) {
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	for (unsigned i = 0; i < CAM_NWIN; i++) {
		cam_win[i].va = ml_io_map((vm_offset_t)cam_win[i].pa, cam_win[i].size);
		if (!cam_win[i].va) {
			say("cannot map %#llx: stopping", (unsigned long long)cam_win[i].pa);
			thread_terminate(current_thread());
			return;
		}
	}
	/* the frame memory is DRAM XNU does not own (the loader proved 0x60000000 free): a window the kernel can take back */
	{
		uint32_t wc = 0;

		(void)PE_parse_boot_argn("mtkcam_wc", &wc, sizeof(wc));
		cam_cached = cam_level >= 5 && !wc;
	}
	cam_buf = ml_io_map_unmappable((vm_offset_t)cam_pa, (vm_size_t)CAM_BUF_SIZE, cam_cached ? VM_WIMG_DEFAULT : VM_WIMG_WCOMB);
	if (!cam_buf) {
		say("cannot map the frame memory at %#llx: stopping", (unsigned long long)cam_pa);
		thread_terminate(current_thread());
		return;
	}
	if (cam_level >= 5) {
		/* mtkcam=5: /dev/redwood-camera; a session whenever a read wants frames, until nobody reads; after three sessions that
		 * never got to the stream the camera is given up (reads fail with EIO) */
		uint32_t failed = 0;
		int major = cdevsw_add(-1, &camdev_cdevsw);

		cam_dev_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-camera") != NULL;
		say("/dev/redwood-camera %s", cam_dev_made ? "made" : "CANNOT BE MADE");
		while (cam_dev_made && !cam_broken) {
			struct timespec ts = { 1, 0 };

			lck_mtx_lock(&cam_lock);
			while (!cam_dev_wanted) {
				(void)msleep(&cam_dev_wanted, &cam_lock, PZERO, "redwood-mtkcam-idle", &ts);
			}
			cam_dev_wanted = false;
			if (cam_dev_opens == 0) {
				lck_mtx_unlock(&cam_lock);
				continue;               /* (a read that came while the last session was ending) */
			}
			lck_mtx_unlock(&cam_lock);
			cam_serve = true;
			cam_stream_ok = false;
			cam_variant = 0;
			cam_legacy = 0;
			cam_capture();
			if (!cam_stream_ok && ++failed >= 3) {
				cam_broken = true;
				say("the camera could not be brought up three times: given up");
			} else if (cam_stream_ok) {
				failed = 0;
			}
			delay_for_interval(500, NSEC_PER_MSEC);
		}
		ml_io_unmap(cam_buf, (vm_size_t)CAM_BUF_SIZE);
		thread_terminate(current_thread());
		return;
	}
	{
		uint32_t mask = 1;

		(void)PE_parse_boot_argn("mtkcam_var", &mask, sizeof(mask));
		for (uint32_t v = 0; v < 32; v++) {
			if (mask & (1u << v)) {
				cam_variant = v;
				cam_legacy = (cam_fa || cam_gain_arg || cam_xp) ? 0 : v;
				cam_capture();
				delay_for_interval(500, NSEC_PER_MSEC);
			}
		}
	}
	ml_io_unmap(cam_buf, (vm_size_t)CAM_BUF_SIZE);
	thread_terminate(current_thread());
}

void
redwood_mtkcam_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkcam", &level, sizeof(level)) || level < 1) {
		return;
	}
	cam_level = level;
	if (kernel_thread_start_priority(mtkcam_thread, NULL, 81 /* BASEPRI_KERNEL */, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#else /* !MTKCAM_TABLES */

void
redwood_mtkcam_init(void)
{
	uint32_t level = 0;

	if (PE_parse_boot_argn("mtkcam", &level, sizeof(level)) && level >= 1) {
		say("the sensor's tables are not in this build (redwood_mtkcam_jn1.h and redwood_mtkcam_rx.h: tools/camtable.py, tools/camrx.py): nothing done");
	}
}

#endif /* MTKCAM_TABLES */
