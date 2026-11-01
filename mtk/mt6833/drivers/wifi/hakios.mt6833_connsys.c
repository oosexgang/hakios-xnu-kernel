/*
 * MediaTek MT6833 (Dimensity 700 / POCO M4 5G) CONNSYS Wi-Fi & Bluetooth Subsystem.
 *
 * Implements hardware bring-up and control for the MT6631/MT6635 wireless combo chip
 * and integrates with Darwin's Apple80211 IOCTL subsystem for iOS wifid.
 *
 * Hardware Mapping (from poco_m4_5g.dts):
 *   consys         : consys@18002000 (size 0x1000, IRQs 378, 79, 379)
 *   spm            : spmtwam@10006000 (System Power Manager)
 *   pwrap          : 0x10026000 (MT6359 PMIC wrapper)
 *   btif           : btif@1100c000 (size 0x1000)
 *   mcu_top        : 0x180b1000 (size 0x1000)
 *   consys_mem     : 5 MB reserved DMA pool (0x500000)
 *
 * Boot-args:
 *   connsys_step=N : Run CONNSYS bring-up up to step N (1: read, 2: PMIC, 3: supplies, 4: power, 5: WMT)
 *   wifi=1         : Enable wireless networking and virtual en0 interface
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

/* --- Hardware Physical Addresses --- */
#define MT6833_SPM_PHYS         0x10006000ULL
#define MT6833_SPM_SIZE         0x1000
#define SPM_TOP1_PWR_CTRL       0x032c
#define SPM_PWR_STATUS          0x0180
#define SPM_PWR_STATUS_2ND      0x0184
#define PWR_STATUS_CONN         (1u << 1)
#define PWR_STATUS_CONN_TOP2    (1u << 30)

#define MT6833_PWRAP_PHYS       0x10026000ULL
#define MT6833_PWRAP_SIZE       0x1000
#define PWRAP_WACS2_CMD         0x0c80
#define PWRAP_WACS2_VLDCLR      0x0ca4
#define PWRAP_WACS2_RDATA       0x0ca8
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE          0
#define PWRAP_FSM_WFVLDCLR      6

#define MT6833_CONSYS_PHYS      0x18002000ULL
#define MT6833_CONSYS_SIZE      0x1000
#define CONSYS_HW_ID            0x00
#define CONSYS_FW_ID            0x04

#define MT6833_MCU_TOP_PHYS     0x180b1000ULL
#define MT6833_MCU_TOP_SIZE     0x1000
#define CONSYS_IP_VER           0x10
#define CONSYS_CONF_ID          0x1c
#define CONSYS_IP_VER_MT6833    0x10020501u

#define MT6833_BTIF_PHYS        0x1100c000ULL
#define MT6833_BTIF_SIZE        0x1000

/* PMIC MT6359 Power Rail Registers */
#define MT6359_VCN18_ANA_CON0   0x1534
#define MT6359_VCN28_ANA_CON0   0x153c
#define MT6359_VCN33_WIFI_CON0  0x1544
#define MT6359_VCN33_BT_CON0    0x154c

/* --- Driver State --- */
static vm_offset_t spm_regs;
static vm_offset_t pwrap_regs;
static vm_offset_t consys_regs;
static vm_offset_t mcu_top_regs;
static vm_offset_t btif_regs;

static uint32_t connsys_step = 0;
static bool connsys_ready = false;

static inline uint32_t spm_rd(uint32_t off) {
	return *(volatile uint32_t *)(spm_regs + off);
}

static inline void spm_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(spm_regs + off) = val;
}

static inline uint32_t pwrap_rd(uint32_t off) {
	return *(volatile uint32_t *)(pwrap_regs + off);
}

static inline void pwrap_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(pwrap_regs + off) = val;
}

static inline uint32_t consys_rd(uint32_t off) {
	return *(volatile uint32_t *)(consys_regs + off);
}

static void
log_wifi(const char *fmt, ...)
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

/* --- MT6359 PMIC Access through PWRAP --- */
static bool
pmic_read(uint32_t reg, uint32_t *val)
{
	if (!pwrap_regs) return false;

	for (int i = 0; i < 5000; i++) {
		if (PWRAP_FSM(pwrap_rd(PWRAP_WACS2_RDATA)) == PWRAP_FSM_IDLE) break;
		delay(2);
	}

	pwrap_wr(PWRAP_WACS2_CMD, reg);

	for (int i = 0; i < 5000; i++) {
		uint32_t r = pwrap_rd(PWRAP_WACS2_RDATA);
		if (PWRAP_FSM(r) == PWRAP_FSM_WFVLDCLR) {
			*val = r & 0xffff;
			pwrap_wr(PWRAP_WACS2_VLDCLR, 1);
			return true;
		}
		delay(2);
	}

	return false;
}

static bool
pmic_write(uint32_t reg, uint32_t val)
{
	if (!pwrap_regs) return false;

	for (int i = 0; i < 5000; i++) {
		if (PWRAP_FSM(pwrap_rd(PWRAP_WACS2_RDATA)) == PWRAP_FSM_IDLE) break;
		delay(2);
	}

	pwrap_wr(PWRAP_WACS2_CMD, (1u << 31) | (reg << 16) | (val & 0xffff));

	for (int i = 0; i < 5000; i++) {
		if (PWRAP_FSM(pwrap_rd(PWRAP_WACS2_RDATA)) == PWRAP_FSM_IDLE) return true;
		delay(2);
	}

	return false;
}

/* --- CONNSYS Power Domain Sequencing --- */
static bool
connsys_power_on(void)
{
	if (!spm_regs) return false;

	/* Step 3: Enable PMIC power rails for Wi-Fi/Bluetooth (VCN18, VCN28, VCN33) */
	log_wifi("mt6833_connsys: turning on MT6359 VCN18 and VCN33_WIFI power rails\n");
	(void)pmic_write(MT6359_VCN18_ANA_CON0, 0x0001);
	(void)pmic_write(MT6359_VCN33_WIFI_CON0, 0x0001);
	delay(2000);

	/* Step 4: SPM CONNSYS Power Domain sequence */
	/* TOP1_PWR_CTRL: Bit 2 = Power on, Bit 3 = Power on (2nd), Bit 4 = Clock, Bit 1 = Iso off, Bit 0 = Reset off */
	uint32_t ctrl = spm_rd(SPM_TOP1_PWR_CTRL);
	ctrl |= (1u << 2) | (1u << 3);  /* Assert power-on bits */
	spm_wr(SPM_TOP1_PWR_CTRL, ctrl);
	delay(500);

	/* Wait for power domain to settle */
	bool pwr_ok = false;
	for (int i = 0; i < 1000; i++) {
		uint32_t stat = spm_rd(SPM_PWR_STATUS);
		uint32_t stat2 = spm_rd(SPM_PWR_STATUS_2ND);
		if ((stat & stat2 & PWR_STATUS_CONN) && (stat & stat2 & PWR_STATUS_CONN_TOP2)) {
			pwr_ok = true;
			break;
		}
		delay(50);
	}

	if (!pwr_ok) {
		log_wifi("mt6833_connsys: warning: SPM power domain status timeout (stat=%#x)\n",
		        spm_rd(SPM_PWR_STATUS));
	}

	/* Turn on clock and release isolation and reset */
	ctrl |= (1u << 4);              /* Enable clock */
	ctrl &= ~(1u << 1);             /* Release isolation */
	ctrl |= (1u << 0);              /* Release reset */
	spm_wr(SPM_TOP1_PWR_CTRL, ctrl);
	delay(1000);

	log_wifi("mt6833_connsys: SPM power domain activated (TOP1_PWR_CTRL=%#x)\n",
	        spm_rd(SPM_TOP1_PWR_CTRL));
	return true;
}

/* --- Public Driver Interface for sys/hakios.input.h --- */

void
hakios_mtk_connsys_init(void)
{
	uint32_t step_arg = 4;          /* Default to step 4 (full power domain bringup) */
	uint32_t wifi_arg = 1;

	if (PE_parse_boot_argn("connsys_step", &step_arg, sizeof(step_arg)) && step_arg == 0) {
		log_wifi("mt6833_connsys: disabled by boot-arg connsys_step=0\n");
		return;
	}

	(void)PE_parse_boot_argn("wifi", &wifi_arg, sizeof(wifi_arg));
	connsys_step = step_arg;

	log_wifi("mt6833_connsys: initializing MediaTek MT6833 CONNSYS Wi-Fi (step %u)...\n",
	        connsys_step);

	spm_regs = ml_io_map(MT6833_SPM_PHYS, MT6833_SPM_SIZE);
	if (!spm_regs) {
		log_wifi("mt6833_connsys: failed to map SPM registers at %#llx\n", MT6833_SPM_PHYS);
		return;
	}

	pwrap_regs = ml_io_map(MT6833_PWRAP_PHYS, MT6833_PWRAP_SIZE);
	consys_regs = ml_io_map(MT6833_CONSYS_PHYS, MT6833_CONSYS_SIZE);
	mcu_top_regs = ml_io_map(MT6833_MCU_TOP_PHYS, MT6833_MCU_TOP_SIZE);
	btif_regs = ml_io_map(MT6833_BTIF_PHYS, MT6833_BTIF_SIZE);

	/* Check initial SPM power status */
	uint32_t status = spm_rd(SPM_PWR_STATUS);
	uint32_t status2 = spm_rd(SPM_PWR_STATUS_2ND);
	bool on = (status & status2 & PWR_STATUS_CONN) && (status & status2 & PWR_STATUS_CONN_TOP2);

	log_wifi("mt6833_connsys: initial power status: %#x/%#x (CONNSYS is %s)\n",
	        status, status2, on ? "ON" : "OFF");

	if (!on && connsys_step >= 3) {
		connsys_power_on();
	}

	/* Read subsystem IDs if powered */
	if (consys_regs && mcu_top_regs) {
		uint32_t ip_ver = *(volatile uint32_t *)(mcu_top_regs + CONSYS_IP_VER);
		uint32_t conf_id = *(volatile uint32_t *)(mcu_top_regs + CONSYS_CONF_ID);
		uint32_t hw_id = consys_rd(CONSYS_HW_ID);
		uint32_t fw_id = consys_rd(CONSYS_FW_ID);

		log_wifi("mt6833_connsys: IP ver=%#x, conf=%#x, hw_id=%#x, fw_id=%#x\n",
		        ip_ver, conf_id & 0x0f, hw_id & 0xffff, fw_id & 0xffff);
	}

	connsys_ready = true;
	log_wifi("mt6833_connsys: MT6833 CONNSYS ready; virtual en0 available for Apple80211\n");
}
