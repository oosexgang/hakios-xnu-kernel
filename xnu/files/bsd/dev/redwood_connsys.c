/*
 * CONNSYS, the Wi-Fi/Bluetooth/GPS subsystem of the Infinix Hot 20's MT6768 (with the MT6631 radio):
 * docs/wifi-x6826b.md. Steps 1 and 2 read only; 3 and 4 write (the PMIC's supplies, then the power domain).
 *
 * The addresses are the ones Android's device tree gives MediaTek's driver for this SoC
 * (consys@18002000, "mediatek,mt6768-consys", printed by mtkloader/mtkconnsys.h), and what is read
 * is what that driver reads (common_main/platform/mt6768.c and include/mt6768.h in
 * MiCode/MTK_kernel_modules, merlin-r-oss):
 *
 *   SPM (0x10006000)     +0x32C  TOP1_PWR_CTRL  bit 0 reset released, 1 isolation, 2 power on, 3 power on (second),
 *                                               4 clock
 *                        +0x180  PWR_STATUS     bit 1 CONNSYS on, bit 30 its second (TOP2) domain on
 *                        +0x184  PWR_STATUS_2ND the same, from the other half of the power switch
 *   MCU_TOP_MISC_OFF (0x180B1000) +0x10  CONSYS_IP_VER: 0x10020501 once CONNSYS runs; +0x1C its configuration
 *   MCU (0x18002000)     +0x00  hardware version, +0x04 firmware (ROM) version
 *
 * The CONNSYS registers (the last two blocks) are read only when SPM says both of its domains are on:
 * a read of a powered-off block of this kind can hang the bus. Android powers CONNSYS on at its own
 * start and LK does not, so after a boot of XNU straight from LK it is expected to be off.
 *
 * Boot-arg connsys_step=1; 2 also reads its supplies, 3 turns them on, 4 powers CONNSYS up, 5 puts its firmware in and lets its CPU go, 6 also
 * listens on BTIF, 7 asks WMT for the chip ID through it, 8 puts STP into its full mode (below); 9 reads the MCU's
 * patch and says what would be sent, 10 sends it the PDA way and resets WMT before the full mode, as Android does
 * on this chip (connsys_pda_patch), 11 sends what Android's WMT does after the full mode (connsys_sw_init); 12 and on
 * are the Wi-Fi's own (redwood_mtkwifi.c). What it finds is said once and kept for the end of the log.
 */

#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <kern/thread.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/ttycom.h>
#include <sys/termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/sys_domain.h>
#include <uuid/uuid.h>
#include <miscfs/devfs/devfs.h>
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
/* (osfmk/arm64/machine_routines.c: a window kmem_free can take back, out of the kernel map and not the 8 MB I/O map) */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern void delay(int usec);

#define SPM_PHYS                0x10006000u
#define SPM_TOP1_PWR_CTRL       0x32C
#define SPM_PWR_STATUS          0x180
#define SPM_PWR_STATUS_2ND      0x184
#define PWR_STATUS_CONN         (1u << 1)
#define PWR_STATUS_CONN_TOP2    (1u << 30)

#define MCU_TOP_MISC_OFF_PHYS   0x180B1000u
#define CONSYS_IP_VER           0x10
#define CONSYS_CONF_ID          0x1C
#define CONSYS_IP_VER_ID        0x10020501u

#define MCU_PHYS                0x18002000u
#define CONSYS_HW_ID            0x00
#define CONSYS_FW_ID            0x04

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static uint32_t
connsys_ms(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/*
 * Step 2a (connsys_step=2), reading only: CONNSYS's supplies as LK leaves them -- the PMIC MT6358's
 * LDOs VCN18, VCN28 and VCN33 (two enables: Bluetooth's and Wi-Fi's), each its enable register and its
 * analog register with the voltage selection (include/linux/mfd/mt6358/registers.h of the MT6768's
 * vendor kernel), read through the PMIC wrapper's WACS2 channel as redwood_mtkkeys.c reads the PMIC.
 * Android turns VCN18 and VCN33 on (consys_hw_vcn18_ctrl in mt6768.c) before it powers CONNSYS up.
 */
#define PWRAP_PHYS              0x1000d000u
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)

static vm_offset_t connsys_pwrap;

static bool
connsys_pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 100000; spin++) {
		if (PWRAP_FSM(rd(connsys_pwrap, PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
connsys_pmic_read_raw(uint32_t reg, uint32_t *value)
{
	if (!connsys_pwrap_wait(0)) {
		return false;
	}
	*(volatile uint32_t *)(connsys_pwrap + PWRAP_WACS2_CMD) = (reg >> 1) << 16;
	if (!connsys_pwrap_wait(6)) {
		return false;
	}
	*value = rd(connsys_pwrap, PWRAP_WACS2_RDATA) & 0xffff;
	*(volatile uint32_t *)(connsys_pwrap + PWRAP_WACS2_VLDCLR) = 1;
	return true;
}

static bool
connsys_pmic_read(uint32_t reg, uint32_t *value)
{
	bool ok;

	redwood_pwrap_lock();
	ok = connsys_pmic_read_raw(reg, value);
	redwood_pwrap_unlock();
	return ok;
}

static void
connsys_power_report(void)
{
	static const struct { const char *name; uint32_t enable, analog; } ldo[] = {
		{ "VCN18", 0x1c58, 0x1e8c },
		{ "VCN28", 0x1d88, 0x1e14 },
		{ "VCN33 (BT)", 0x1d1c, 0x1e28 },
		{ "VCN33 (Wi-Fi)", 0x1d2a, 0x1e28 },
	};

	connsys_pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t enable = 0xdead, analog = 0xdead;
		bool read = connsys_pmic_read(ldo[i].enable, &enable) && connsys_pmic_read(ldo[i].analog, &analog);

		redwood_host_keep_add("connsys: step 2: %s: %s, enable register %#x = %#x, analog %#x = %#x", ldo[i].name,
		    read ? ((enable & 1) ? "ON" : "off") : "NOT READ", ldo[i].enable, enable, ldo[i].analog, analog);
	}
}

static bool
connsys_pmic_write_raw(uint32_t reg, uint32_t value)
{
	if (!connsys_pwrap_wait(0)) {
		return false;
	}
	*(volatile uint32_t *)(connsys_pwrap + PWRAP_WACS2_CMD) = (1u << 31) | (reg >> 1) << 16 | (value & 0xffff);
	return connsys_pwrap_wait(0);
}

static bool
connsys_pmic_write(uint32_t reg, uint32_t value)
{
	bool ok;

	redwood_pwrap_lock();
	ok = connsys_pmic_write_raw(reg, value);
	redwood_pwrap_unlock();
	return ok;
}

/* a PMIC register's bits under `mask` set to `value`, read back; false if the wrapper did not answer */
static bool
connsys_pmic_update(uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t now;

	return connsys_pmic_read(reg, &now) && connsys_pmic_write(reg, (now & ~mask) | (value & mask));
}

/*
 * XO_WCN, the PMIC's 26 MHz buffer for CONNSYS in co-clock mode (WMT_SOC.cfg co_clock_flag=1): Android's log at boot
 * has "PMIC_CLK_BUF?_STATUS=2 1 1 2 0 0 0" -- buffer 2, XO_WCN, under software control -- and "DCXO_CW00/01/02/11/
 * 14/16/23=0x4e1d 6d55 ...", and turns it on before CONNSYS's power-on ("co clock type(1),turn on clk buf",
 * mtk_wcn_consys_hw_reg_ctrl -> clk_buf_ctrl(CLK_BUF_CONN, true)): XO_EXTBUF2_EN_M, bit 5 of DCXO_CW00. Until run
 * 428 nothing here did; the MCU dies within 5 ms of entering the patch, awake (HCLK, OSC on, SLEEP off), pc 0 -- as
 * would a patch that starts the radio's die with no clock. DCXO_CW00 is 0x788 and CW01 0x78e in the MT6358's map as
 * I read it: the bit is set only when 0x788 holds what Android printed (bit 5 aside), so that a wrong address is
 * read and said, never written. Boot-arg connsys_xo=0: left as it is.
 */
static void
connsys_xo_wcn_on(void)
{
	uint32_t cw00 = 0, cw01 = 0, after = 0, wanted = 1;
	bool known;

	(void)PE_parse_boot_argn("connsys_xo", &wanted, sizeof(wanted));
	if (!connsys_pmic_read(0x788, &cw00) || !connsys_pmic_read(0x78e, &cw01)) {
		redwood_host_keep_add("connsys: step 3: XO_WCN: DCXO_CW00/CW01 not read (the wrapper did not answer)");
		return;
	}
	known = (cw00 & ~0x20u) == (0x4e1du & ~0x20u);
	if (wanted != 0 && known && (cw00 & 0x20u) == 0) {
		(void)connsys_pmic_update(0x788, 0x20u, 0x20u);
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	(void)connsys_pmic_read(0x788, &after);
	redwood_host_keep_add("connsys: step 3: XO_WCN: DCXO_CW00 %#x CW01 %#x (%s) -> CW00 %#x, XO_EXTBUF2_EN_M %u%s", cw00, cw01,
	    known ? "Android's boot value" : "NOT Android's 0x4e1d: not written", after, (after >> 5) & 1,
	    wanted == 0 ? " (connsys_xo=0)" : "");
}

/*
 * Step 3 (connsys_step=3): the supplies on, as consys_hw_vcn18_ctrl, consys_hw_wifi/bt_vcn33_ctrl and
 * mtk_wcn_consys_hw_efuse_paldo_ctrl do it (mt6768.c, mtk_wcn_consys_hw.c) with values from the MT6358
 * regulator driver: VCN18 (fixed 1.8 V) and VCN33 to software control (their OP_EN registers = 1),
 * VCN33's voltage selection to 3.3 V (VCN33_ANA_CON0 bits 8-9 = 1; LK leaves 3, 3.5 V), its Bluetooth
 * and Wi-Fi enables, and VCN28 (fixed 2.8 V, for the efuse). Then all four read again.
 */
static void
connsys_supplies_on(void)
{
	bool ok = connsys_pmic_write(0x1c5a, 1) &&              /* LDO_VCN18_OP_EN: SW */
	    connsys_pmic_update(0x1c58, 1, 1) &&                /* LDO_VCN18_CON0: EN */
	    connsys_pmic_write(0x1d1e, 1) &&                    /* LDO_VCN33_OP_EN: SW */
	    connsys_pmic_update(0x1e28, 0x300, 0x100) &&        /* VCN33_ANA_CON0: VOSEL 3.3 V */
	    connsys_pmic_update(0x1d1c, 1, 1) &&                /* LDO_VCN33_CON0_0: EN (Bluetooth's) */
	    connsys_pmic_update(0x1d2a, 1, 1) &&                /* LDO_VCN33_CON0_1: EN (Wi-Fi's) */
	    connsys_pmic_update(0x1d88, 1, 1);                  /* LDO_VCN28_CON0: EN */

	delay(1000);
	redwood_host_keep_add("connsys: step 3: VCN18, VCN33 (3.3 V) and VCN28 turned on: %s", ok ? "written" : "THE WRAPPER DID NOT ANSWER");
	connsys_power_report();
	connsys_xo_wcn_on();
}

/*
 * Step 4 (connsys_step=4): CONNSYS powered up, as consys_set_if_pinmux, consys_hw_reset_bit_set and the
 * hand-made branch of consys_hw_power_ctrl do it (mt6768.c; Android goes through the clock framework's
 * clk_scp_conn_main, which does the same), every wait bounded and said instead of endless: the pins to
 * the radio, the CPU held in reset, SPM's key, the on-domain's two parts powered (each waiting for its
 * ack), the bus clock on, isolation off, the off-domain's acks, the software reset let go, the bus
 * protections (AP to CONNSYS, CONNSYS to AP) taken off, SPM's apsrc/ddr settings; then CONNSYS's IP
 * version polled -- 0x10020501 is the MT6768's and says it lives. The clock buffer of co-clock mode
 * (WMT_SOC.cfg: co_clock_flag=1, Android's clk_buf driver) is not done yet.
 */
static bool
connsys_wait(vm_offset_t base, uint32_t offset, uint32_t bit, const char *what)
{
	for (unsigned n = 0; n < 10000; n++) {
		if (rd(base, offset) & bit) {
			return true;
		}
		delay(10);
	}
	redwood_host_keep_add("connsys: step 4 [%u ms]: %s: no ack after 100 ms (%#x = %#x)", connsys_ms(), what, offset, rd(base, offset));
	return false;
}

static void
connsys_power_on(vm_offset_t spm)
{
	vm_offset_t pinmux = ml_io_map(0x10005000u, 0x1000), rgu = ml_io_map(0x10007000u, 0x1000);
	vm_offset_t infra = ml_io_map(0x10001000u, 0x1000), misc = ml_io_map(MCU_TOP_MISC_OFF_PHYS, 0x1000);
	uint32_t version = 0;

#define W(base, offset, value) (*(volatile uint32_t *)((base) + (offset)) = (value))
	W(pinmux, 0x3f0, (rd(pinmux, 0x3f0) & 0xffffff88u) | 0x11u);
	W(pinmux, 0x3e0, (rd(pinmux, 0x3e0) & 0x8888888fu) | 0x11111110u);
	delay(150);
	W(rgu, 0x18, rd(rgu, 0x18) | (1u << 12) | (0x88u << 24));            /* the CPU's software reset on */
	W(spm, 0x000, (rd(spm, 0x000) & 0xffffu) | 0x0b160001u);              /* SPM's write key */
	W(spm, SPM_TOP1_PWR_CTRL, rd(spm, SPM_TOP1_PWR_CTRL) | (1u << 2));    /* on-domain power */
	if (!connsys_wait(spm, SPM_PWR_STATUS, 1u << 1, "on-domain power")) {
		return;
	}
	W(spm, SPM_TOP1_PWR_CTRL, rd(spm, SPM_TOP1_PWR_CTRL) | (1u << 3));    /* ... its second part */
	if (!connsys_wait(spm, SPM_PWR_STATUS_2ND, 1u << 1, "on-domain power, second part")) {
		return;
	}
	W(spm, SPM_TOP1_PWR_CTRL, rd(spm, SPM_TOP1_PWR_CTRL) & ~(1u << 4));   /* the bus clock */
	W(spm, SPM_TOP1_PWR_CTRL, rd(spm, SPM_TOP1_PWR_CTRL) & ~(1u << 1));   /* isolation off */
	/*
	 * The domain's own reset let go (PWR_RST_B, bit 0), as Linux's scpsys does it
	 * (spm_mtcmos_ctrl_conn_pwr, drivers/clk/mediatek/clk-mt6768-pg.c) -- WMT's hand-made branch
	 * leaves it out, and without it the off-domain's ack (bit 30) never came (run 305).
	 */
	W(spm, SPM_TOP1_PWR_CTRL, rd(spm, SPM_TOP1_PWR_CTRL) | (1u << 0));
	(void)connsys_wait(spm, SPM_PWR_STATUS, 1u << 30, "off-domain power");
	(void)connsys_wait(spm, SPM_PWR_STATUS_2ND, 1u << 30, "off-domain power, second part");
	W(rgu, 0x18, (rd(rgu, 0x18) & ~(1u << 9)) | (0x88u << 24));          /* ap_sw_rst_b: let go */
	/* the bus protections, through their CLR registers as spm_mtcmos_ctrl_conn_bus_prot does */
	W(infra, 0x2a4, (1u << 14) | (1u << 16));
	W(infra, 0x2a4, 1u << 13);
	W(infra, 0x2ac, 1u << 18);
	delay(10);
	/* (run 307 wrote these two, unexplained, and Android's power-up -- spm_mtcmos_ctrl_conn_pwr/_bus_prot of
	 * clk-mt6768-pg.c -- has neither: boot-arg connsys_spmx=0 leaves them out) */
	{
		uint32_t spmx = 1;

		(void)PE_parse_boot_argn("connsys_spmx", &spmx, sizeof(spmx));
		if (spmx != 0) {
			W(spm, 0x6f8, 0x5);
			W(spm, 0x6fc, 0x50505);
		}
	}
#undef W
	delay(10);
	redwood_host_keep_add("connsys: step 4 [%u ms]: powered: TOP1_PWR_CTRL %#x, PWR_STATUS %#x/%#x, bus protection %#x (%#x) and %#x (%#x)",
	    connsys_ms(), rd(spm, SPM_TOP1_PWR_CTRL), rd(spm, SPM_PWR_STATUS), rd(spm, SPM_PWR_STATUS_2ND), rd(infra, 0x220), rd(infra, 0x228),
	    rd(infra, 0x250), rd(infra, 0x258));
	/*
	 * Linux takes the domain as on with bit 1 alone (CONN_PWR_STA_MASK, clk-mt6768-pg.c) and Android reads
	 * the IP version right after; WMT's header has the off-domain's ack at bit 30, but on this phone it is
	 * bit 31 that comes once PWR_RST_B is let go (0x06c57c2e -> 0x86c57c2e, run 306). Either will do.
	 */
	{
		uint32_t both = rd(spm, SPM_PWR_STATUS) & rd(spm, SPM_PWR_STATUS_2ND);

		if (!(both & (1u << 1)) || !(both & ((1u << 30) | (1u << 31)))) {
			/* (a read of a block that is not powered can hang the bus, and the phone with it) */
			redwood_host_keep_add("connsys: step 4 [%u ms]: the off-domain is not powered (%#x): its registers are not read", connsys_ms(), both);
			return;
		}
		redwood_host_keep_add("connsys: step 4 [%u ms]: CONN_HIF_ON CPUPCR %#x", connsys_ms(), rd(ml_io_map(0x18007000u, 0x1000), 0x104));
	}
	for (unsigned n = 0; n < 10 && version != CONSYS_IP_VER_ID; n++) {
		version = rd(misc, CONSYS_IP_VER);
		delay(20000);
	}
	redwood_host_keep_add("connsys: step 4 [%u ms]: IP version %#x: %s", connsys_ms(), version,
	    version == CONSYS_IP_VER_ID ? "CONNSYS LIVES (the MT6768's)" : "not the expected 0x10020501");
}

/*
 * Step 5 (connsys_step=5): the firmware into CONNSYS's memory, as mtk_wcn_soc_rom_patch_dwn
 * (core/wmt_ic_soc.c) does it, and its CPU let go. Each of the phone's soc1_0_*_hdr.bin files
 * (/vendor/firmware, in the system image as /fw/connsys: userland/build.sh) is a 48-byte struct
 * wmt_rom_patch (core/include/wmt_core.h) -- whose u4PatchAddr is where CONNSYS sees it, 0xf0000000 being
 * the start of its memory (CONSYS_EMI_FW_PHY_BASE against CONSYS_EMI_AP_PHY_BASE, include/mt6768.h) --
 * and then its body, copied there: the MCU's RAM code at 0, its ROM patch at 0x1c000, Bluetooth's at
 * 0x80000, Wi-Fi's at 0x140000. The memory is Android's 4 MB consys-reserve-memory; here at 0x51000000
 * (16 MB aligned, as INFRA's remapping register takes it: bits 24 and up), clear of the loader's and the
 * sensor hub's. Then INFRA+0x380 told where it is and +0x388 the peripheral window
 * (consys_emi_set_remapping_reg), the CPU's software reset let go (consys_hw_reset_bit_set), and
 * MCU+0x600 polled for 0x1D1E -- what Android waits for. In a thread: the files are there only once
 * the root is mounted.
 */
/* kern.redwood.connsys: how step 5 ended, for a job to read (the kept lines run out on a long run) */
static char connsys_said[1000] = "not run";
SYSCTL_DECL(_kern_redwood);
SYSCTL_STRING(_kern_redwood, OID_AUTO, connsys, CTLFLAG_RD | CTLFLAG_LOCKED, connsys_said, sizeof(connsys_said), "");

#define CONNSYS_EMI_PHYS        REDWOOD_CONNSYS_EMI_PHYS
#define CONNSYS_EMI_SIZE        REDWOOD_CONNSYS_EMI_SIZE

static uint8_t *
connsys_file(const char *name, uint32_t *size)
{
	char path[96];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0;

	snprintf(path, sizeof(path), "/fw/connsys/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size < 48 ||
	    attributes.va_data_size > CONNSYS_EMI_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data != NULL && (vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context)) != 0 || left != 0)) {
		kfree_data(data, attributes.va_data_size);
		data = NULL;
	}
	vnode_put(vnode);
	*size = (uint32_t)attributes.va_data_size;
	return data;
}

uint8_t *
redwood_connsys_file(const char *name, uint32_t *size)
{
	return connsys_file(name, size);
}

extern void delay_for_interval(uint32_t interval, uint32_t scale_factor);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Step 6 (connsys_step=6): BTIF, the bridge WMT talks to CONNSYS through (btif@1100c000 in the device
 * tree, run 313): its clock on (infracfg's IFR2 gate, bit 31: CLR at +0x84 -- clk-mt6768.c, ifr_btif)
 * and the hardware set up as hal_btif_hw_init does it (drivers/misc/mediatek/btif/common/btif_plat.c):
 * normal mode, the new handshake, both FIFOs cleared, no DMA, auto-reset -- before CONNSYS is powered,
 * as Android does it; once CONNSYS runs, whatever it sends is read for a second (PIO: LSR bit 0, RBR).
 */
#define BTIF_PHYS               0x1100c000u
#define BTIF_TX_DMA_PHYS        0x11000b80u
/* (in the Wi-Fi's DMA memory since run 441: at CONNSYS's +0x200000, until then, its firmware's last region goes --
 * WIFI_RAM_CODE's 0xf01fe000-0xf0221570, redwood_mtkwifi.c) */
#define BTIF_TX_VFF_PHYS        REDWOOD_WIFI_DMA_PHYS
#define BTIF_TX_VFF_SIZE        8192u

static vm_offset_t connsys_btif;
static vm_offset_t connsys_dma_tx;
static vm_offset_t connsys_dma_vff;
static uint32_t connsys_dma_wpt;
static uint32_t connsys_dma_wrap;
static uint32_t connsys_dma = 1;

static void
connsys_btif_dma_init(void)
{
	vm_offset_t infra = ml_io_map(0x10001000u, 0x1000);

	*(volatile uint32_t *)(infra + 0x8c) = 1u << 18;        /* ifr_ap_dma ungated */
	delay(10);
	if (connsys_dma_tx == 0) {
		connsys_dma_tx = ml_io_map(BTIF_TX_DMA_PHYS, 0x1000);
	}
	if (connsys_dma_vff == 0) {
		connsys_dma_vff = ml_io_map(BTIF_TX_VFF_PHYS, BTIF_TX_VFF_SIZE);
	}
	/* Warm reset */
	*(volatile uint32_t *)(connsys_dma_tx + 0x0c) |= 1;
	for (unsigned i = 0; i < 100 && (*(volatile uint32_t *)(connsys_dma_tx + 0x08) & 1); i++) {
		delay(10);
	}
	if (*(volatile uint32_t *)(connsys_dma_tx + 0x08) & 1) {
		*(volatile uint32_t *)(connsys_dma_tx + 0x0c) |= 2;    /* hard reset */
		delay(10);
		*(volatile uint32_t *)(connsys_dma_tx + 0x0c) &= ~2;
	}
	*(volatile uint32_t *)(connsys_dma_tx + 0x1c) = BTIF_TX_VFF_PHYS;
	*(volatile uint32_t *)(connsys_dma_tx + 0x54) = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x24) = BTIF_TX_VFF_SIZE;
	*(volatile uint32_t *)(connsys_dma_tx + 0x2c) = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x30) = 0;
	connsys_dma_wpt = 0;
	connsys_dma_wrap = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x28) = BTIF_TX_VFF_SIZE - 7;  /* thre = 8185 */
	*(volatile uint32_t *)(connsys_dma_tx + 0x00) = 0;
	*(volatile uint32_t *)(connsys_dma_tx + 0x04) = 0;
	redwood_host_keep_add("connsys: step 6 [%u ms]: AP DMA TX ready: EN %#x, WPT %#x, RPT %#x, LEFT %u",
	    connsys_ms(), rd(connsys_dma_tx, 0x08), rd(connsys_dma_tx, 0x2c), rd(connsys_dma_tx, 0x30), rd(connsys_dma_tx, 0x40));
}

static void
connsys_dma_copy(vm_offset_t dst, const uint8_t *src, unsigned len)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;

	for (unsigned i = 0; i < len; i++) {
		d[i] = src[i];
	}
}

static void
connsys_btif_dma_write(const uint8_t *bytes, unsigned length)
{
	unsigned at = 0, stuck = 0;

	/* Wait for any in-flight flush */
	for (unsigned t = 0; t < 50000 && (rd(connsys_dma_tx, 0x14) & 1); t++) {
		delay(10);
	}
	while (at < length) {
		unsigned chunk = length - at;
		unsigned left = rd(connsys_dma_tx, 0x40);

		if (left == 0) {
			unsigned t;
			for (t = 0; t < 50000 && rd(connsys_dma_tx, 0x40) == 0; t++) {
				delay(10);
			}
			if (t >= 50000) {
				stuck = 1;
				break;
			}
			left = rd(connsys_dma_tx, 0x40);
		}
		if (chunk > left) {
			chunk = left;
		}
		if (connsys_dma_wpt + chunk >= BTIF_TX_VFF_SIZE) {
			unsigned tail = BTIF_TX_VFF_SIZE - connsys_dma_wpt;
			connsys_dma_copy(connsys_dma_vff + connsys_dma_wpt, bytes + at, tail);
			connsys_dma_copy(connsys_dma_vff, bytes + at + tail, chunk - tail);
			connsys_dma_wpt = connsys_dma_wpt + chunk - BTIF_TX_VFF_SIZE;
			connsys_dma_wrap ^= 0x10000;
		} else {
			connsys_dma_copy(connsys_dma_vff + connsys_dma_wpt, bytes + at, chunk);
			connsys_dma_wpt += chunk;
		}
		at += chunk;
		__asm__ volatile ("dsb sy" ::: "memory");

		*(volatile uint32_t *)(connsys_dma_tx + 0x08) |= 1;             /* enable DMA */
		*(volatile uint32_t *)(connsys_dma_tx + 0x2c) = connsys_dma_wpt | connsys_dma_wrap;

		uint32_t valid = rd(connsys_dma_tx, 0x3c);
		if (valid > 0 && valid < 8) {
			*(volatile uint32_t *)(connsys_dma_tx + 0x14) |= 1;     /* DMA_FLUSH */
		}
	}
	for (unsigned t = 0; !stuck && t < 50000 && rd(connsys_dma_tx, 0x3c) != 0; t++) {
		uint32_t valid = rd(connsys_dma_tx, 0x3c);
		if (valid > 0 && valid < 8) {
			*(volatile uint32_t *)(connsys_dma_tx + 0x14) |= 1;
		}
		delay(10);
	}
	for (unsigned t = 0; !stuck && t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
		delay(10);
	}
	if (stuck) {
		redwood_host_keep_add("connsys: BTIF DMA TX timeout at %u/%u (LSR %#x, DMA_EN %#x, VALID %u, LEFT %u)",
		    at, length, rd(connsys_btif, 0x14), rd(connsys_dma_tx, 0x08),
		    rd(connsys_dma_tx, 0x3c), rd(connsys_dma_tx, 0x40));
	}
}

static void
connsys_btif_init(void)
{
	static bool asked;

	if (!asked) {
		asked = true;
		(void)PE_parse_boot_argn("connsys_dma", &connsys_dma, sizeof(connsys_dma));
	}
	vm_offset_t infra = ml_io_map(0x10001000u, 0x1000);

	*(volatile uint32_t *)(infra + 0x84) = 1u << 31;                /* ifr_btif ungated */
	delay(10);
	connsys_btif = ml_io_map(BTIF_PHYS, 0x1000);
	*(volatile uint32_t *)(connsys_btif + 0x0c) = 0;                /* FAKELCR: normal mode */
	*(volatile uint32_t *)(connsys_btif + 0x6c) |= 1;               /* HANDSHAKE: the new one */
	*(volatile uint32_t *)(connsys_btif + 0x60) = 0x18;             /* TRI_LVL: TX 8 (16/2), RX 1 */
	*(volatile uint32_t *)(connsys_btif + 0x08) = 1u << 1;          /* FIFOCTRL: clear Rx */
	*(volatile uint32_t *)(connsys_btif + 0x08) = 0;
	*(volatile uint32_t *)(connsys_btif + 0x08) = 1u << 2;          /* ... and Tx */
	*(volatile uint32_t *)(connsys_btif + 0x08) = 0;
	if (connsys_dma) {
		connsys_btif_dma_init();
		*(volatile uint32_t *)(connsys_btif + 0x4c) = (1u << 2) | (1u << 1); /* DMA_EN: TX DMA + auto-reset */
	} else {
		*(volatile uint32_t *)(connsys_btif + 0x4c) = 1u << 2;          /* DMA_EN: no DMA, auto-reset */
	}
	*(volatile uint32_t *)(connsys_btif + 0x04) = 0;                /* IER: none (polled) */
	redwood_host_keep_add("connsys: step 6 [%u ms]: BTIF on (%s): clock status %#x, LSR %#x",
	    connsys_ms(), connsys_dma ? "DMA TX" : "PIO", rd(infra, 0x90), rd(connsys_btif, 0x14));
}

static uint32_t connsys_step;

/*
 * The build time of each image in CONNSYS's EMI, where Android puts it (platform/mt6768.c:1058-1065,
 * CONSYS_EMI_*_BUILDTIME_OFFSET of mt6768.h): ram_bt 0x68300, ram_wifi 0x68310, ram_mcu 0x68320 as it copies the
 * RAM images in (wmt_ic_soc.c:3615-3633), the MCU's patch 0x68330 after the patch and before the WMT reset
 * (wmt_ic_soc.c:3285) -- the first 16 bytes of the file's header, the 16th made 0. "For debugging", MediaTek says;
 * runs 356-379 went without it, and the WMT reset after the patch was never answered.
 */
static void
connsys_build_time(const uint8_t *file, uint32_t offset)
{
	static vm_offset_t times;
	static uint32_t wanted = 1, asked;
	uint8_t text[16];

	if (!asked) {
		asked = 1;
		(void)PE_parse_boot_argn("connsys_times", &wanted, sizeof(wanted));    /* connsys_times=0: none written */
	}
	if (wanted == 0) {
		return;
	}
	if (times == 0) {
		times = ml_io_map(CONNSYS_EMI_PHYS + 0x68000u, 0x1000);
	}
	memcpy(text, file, 15);
	text[15] = 0;
	for (unsigned i = 0; i < 16; i += 4) {
		uint32_t word;

		memcpy(&word, text + i, 4);
		*(volatile uint32_t *)(times + (offset - 0x68000u) + i) = word;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
connsys_btif_listen(const char *what)
{
	char line[200];
	size_t used = 0;
	unsigned got = 0;

	if (connsys_btif == 0) {
		return;
	}
	used = (size_t)snprintf(line, sizeof(line), "%s:", what);
	for (unsigned t = 0; t < 1000 && got < 48; t++) {
		while ((rd(connsys_btif, 0x14) & 1) && got < 48) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x", rd(connsys_btif, 0x0) & 0xff);
			got++;
		}
		delay(1000);
	}
	if (got == 0) {
		snprintf(line + used, sizeof(line) - used, " nothing (LSR %#x)", rd(connsys_btif, 0x14));
	}
	redwood_host_keep_add("connsys: [%u ms] %s", connsys_ms(), line);
	strlcat(connsys_said, " | ", sizeof(connsys_said));
	strlcat(connsys_said, line, sizeof(connsys_said));
}

/*
 * Step 7 (connsys_step=7): the first thing WMT asks CONNSYS (wmt_core_hw_check): its chip ID, a register
 * read of 0x80000008 (GEN_HCR) -- WMT_SET_REG_CMD with op 2. Before full mode STP is in its "mandatory"
 * mode on BTIF (wmt_core_stp_init: MTKSTP_BTIF_MAND_MODE): a 4-byte header -- 0x80 + seq << 3, WMT's task
 * (4) << 4 + the length's high bits, its low byte, 0 -- the payload and a CRC of 00 00 (stp_core.c,
 * mtk_wcn_stp_send_data, the mand branch). CONNSYS is woken first as hal_btif_raise_wak_sig does it
 * (BTIF_WAK +0x64 low, ~150 us, high); bytes go to THR 16 at a time, each time the FIFO is empty (LSR bit 6).
 */
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

static void
connsys_btif_write(const uint8_t *bytes, unsigned length)
{
	if (connsys_dma && connsys_dma_tx != 0) {
		connsys_btif_dma_write(bytes, length);
		return;
	}
	/*
	 * A frame goes out in bursts of 16, the FIFO emptied in between; taken away from the CPU between two (a busy
	 * start-up: runs 381-385 lost the first frame every time, and their user space came up 3-9 s later than in
	 * 373-380), the ROM would see a frame with a gap in it. Android's AP DMA sends a frame without one.
	 * Boot-arg connsys_irqs_off=1 sends each with interrupts off; run 387 did, and the first frame was lost as in
	 * 381-385 -- so it is off by default.
	 */
	static uint32_t quiet = 0, asked;
	boolean_t was = TRUE;

	if (!asked) {
		asked = 1;
		(void)PE_parse_boot_argn("connsys_irqs_off", &quiet, sizeof(quiet));
	}
	if (quiet) {
		was = ml_set_interrupts_enabled(FALSE);
	}
	unsigned at = 0, stuck = 0;

	while (at < length) {
		unsigned t;
		for (t = 0; t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
			delay(10);
		}
		if (t >= 50000) {
			stuck = 1;
			break;
		}
		for (unsigned n = 0; n < 16 && at < length; n++, at++) {
			*(volatile uint8_t *)(connsys_btif + 0x0) = bytes[at];
		}
	}
	for (unsigned t = 0; !stuck && t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
		delay(10);
	}
	if (quiet) {
		(void)ml_set_interrupts_enabled(was);
	}
	if (stuck) {
		redwood_host_keep_add("connsys: BTIF write timeout at %u/%u (LSR %#x)", at, length, rd(connsys_btif, 0x14));
	}
}

static void
connsys_btif_wake(void)
{
	*(volatile uint32_t *)(connsys_btif + 0x64) &= ~1u;
	delay(150);
	*(volatile uint32_t *)(connsys_btif + 0x64) |= 1u;
	delay_for_interval(20, NSEC_PER_MSEC);
}


/*
 * Step 8 (connsys_step=8): STP into its full mode, as _wmt_soc_mode_ctrl (wmt_ic_soc.c) does it: the chip's STP
 * defaults asked for (init_table_1_2) and set (init_table_4) in the mandatory mode, the host switched, and
 * the defaults asked for again (init_table_5) in full mode -- where a frame's header is 0x80 + seq << 3 + ack,
 * the task << 4 + the length's high bits, its low byte and the sum of those three; then the payload and its
 * CRC-16 (the kernel's crc16, 0xA001 reflected, from 0), low byte first. A frame with a payload is answered by
 * a header-only one (stp_send_ack). Each answer is said.
 *
 * The ack is the sequence number of the last frame taken -- "txack (=last rx seq)", stp_core.c:716; 7 before any,
 * :632 -- and a frame that is not the one expected is dropped and the last ack sent again (:1338, :1434). Until run
 * 438 ours was that number + 1, an ack CONNSYS took for none of its frames: with the window full it said 7f 7f 7f 7f
 * (its resync, :708) and sent its first frame of the full mode again, in the middle of the RF calibration.
 */
static bool connsys_stp_full;
static uint8_t connsys_stp_seq, connsys_stp_ack, connsys_stp_expect;
static unsigned connsys_stp_task = 4;     /* STP's channel a frame goes on and an answer is waited on: 4 WMT, 0 Bluetooth (BT_TASK_INDX) */
static vm_offset_t connsys_hif;         /* CONN_HIF_ON (0x18007000): CPUPCR at +0x104, the MCU's program counter */
static vm_offset_t connsys_mcu;         /* MCU (0x18002000): mapped once for register dumps */
static vm_offset_t connsys_rgu;         /* RGU (0x10007000): software reset at +0x18 */
static vm_offset_t connsys_wbg;         /* WBG (0x180B3000): AFE PLL at +0x34 */
static uint8_t *connsys_patch_file;
static uint32_t connsys_patch_size;
static bool connsys_chip_id_done;
static unsigned connsys_doing = 7;     /* the step a WMT exchange is said under */

static bool
connsys_mcu_restart(void)
{
	uint32_t version = 0;

	if (connsys_rgu == 0 || connsys_mcu == 0) {
		return false;
	}
	redwood_host_keep_add("connsys: [%u ms] MCU restart via RGU reset...", connsys_ms());
	*(volatile uint32_t *)(connsys_rgu + 0x18) = (rd(connsys_rgu, 0x18) | (1u << 12)) | (0x88u << 24);
	delay_for_interval(10, NSEC_PER_MSEC);
	if (connsys_wbg != 0) {
		*(volatile uint32_t *)(connsys_wbg + 0x34) = (rd(connsys_wbg, 0x34) & 0xffe7ffffu) | 0x80000u;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	*(volatile uint32_t *)(connsys_rgu + 0x18) = (rd(connsys_rgu, 0x18) & ~(1u << 12)) | (0x88u << 24);
	for (unsigned n = 0; n < 50 && version != 0x1D1E; n++) {
		delay_for_interval(20, NSEC_PER_MSEC);
		version = rd(connsys_mcu, 0x600);
	}
	for (unsigned n = 0; n < 100 && (connsys_hif != 0 && rd(connsys_hif, 0x104) != 0xb04); n++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	delay_for_interval(20, NSEC_PER_MSEC);
	if (connsys_dma && connsys_dma_tx != 0) {
		*(volatile uint32_t *)(connsys_dma_tx + 0x0c) |= 1;
		for (unsigned i = 0; i < 100 && (*(volatile uint32_t *)(connsys_dma_tx + 0x08) & 1); i++) {
			delay(10);
		}
		connsys_dma_wpt = 0;
		connsys_dma_wrap = 0;
		*(volatile uint32_t *)(connsys_dma_tx + 0x2c) = 0;
		*(volatile uint32_t *)(connsys_dma_tx + 0x30) = 0;
	}
	connsys_btif_init();
	redwood_host_keep_add("connsys: [%u ms] MCU restarted: 0x600=%#x, CPUPCR=%#x",
	    connsys_ms(), rd(connsys_mcu, 0x600), connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	return version == 0x1D1E;
}

static uint16_t
connsys_crc16(const uint8_t *bytes, unsigned length)
{
	uint16_t crc = 0;

	for (unsigned i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static int
connsys_btif_byte(unsigned wait_ms)
{
	for (unsigned t = 0; t <= wait_ms * 10; t++) {
		if (rd(connsys_btif, 0x14) & 1) {
			return (int)(rd(connsys_btif, 0x0) & 0xff);
		}
		delay(100);
	}
	return -1;
}

static void
connsys_stp_send(const uint8_t *payload, unsigned length)
{
	uint8_t frame[4 + 1100 + 2];
	uint16_t crc = connsys_stp_full ? connsys_crc16(payload, length) : 0;

	if (length > 1100) {
		return;
	}

	/* In mandatory mode, MediaTek STP header byte 0 is fixed 0x80 (no seq increment). */
	frame[0] = (uint8_t)(0x80 | (connsys_stp_full ? ((connsys_stp_seq << 3) | connsys_stp_ack) : 0));
	frame[1] = (uint8_t)((connsys_stp_task << 4) | ((length >> 8) & 0xf));
	frame[2] = (uint8_t)length;
	frame[3] = connsys_stp_full ? (uint8_t)(frame[0] + frame[1] + frame[2]) : 0;
	memcpy(frame + 4, payload, length);
	frame[4 + length] = (uint8_t)crc;
	frame[5 + length] = (uint8_t)(crc >> 8);
	connsys_btif_write(frame, 6 + length);
	if (connsys_stp_full) {
		connsys_stp_seq = (connsys_stp_seq + 1) & 7;
	}
}

/* the next frame with a payload (header-only ones, CONNSYS's acks, skipped); its payload's length, or -1 */
static int
connsys_stp_receive(uint8_t *payload, unsigned room, char *heard, size_t heard_room, unsigned first_ms)
{
	size_t used = strlen(heard);

	for (;;) {
		int b0 = connsys_btif_byte(first_ms), b1, b2, b3;
		unsigned length;

		if (b0 < 0) {
			return -1;
		}
		if (b0 == 0x55 || !(b0 & 0x80) || (b0 & 0x40)) {
			used += (size_t)snprintf(heard + used, heard_room - used, " ?%02x", b0);
			continue;
		}
		if ((b1 = connsys_btif_byte(50)) < 0 || (b2 = connsys_btif_byte(50)) < 0 || (b3 = connsys_btif_byte(50)) < 0) {
			return -1;
		}
		length = ((unsigned)(b1 & 0xf) << 8) | (unsigned)b2;
		if (length == 0) {
			used += (size_t)snprintf(heard + used, heard_room - used, " [ack %u]", b0 & 7);
			continue;
		}
		for (unsigned i = 0; i < length + 2; i++) {
			int b = connsys_btif_byte(50);

			if (b < 0) {
				return -1;
			}
			if (i < length && i < room) {
				payload[i] = (uint8_t)b;
			}
		}
		if (connsys_stp_full) {
			const uint8_t seq = (uint8_t)((b0 >> 3) & 7);
			const bool again = seq != connsys_stp_expect;
			uint8_t ack[4];

			if (!again) {
				connsys_stp_ack = seq;
				connsys_stp_expect = (uint8_t)((seq + 1) & 7);
			}
			ack[0] = (uint8_t)(0x80 | connsys_stp_ack);
			ack[1] = 0;
			ack[2] = 0;
			ack[3] = ack[0];
			connsys_btif_write(ack, 4);    /* (no wake-up: it is awake, it has just spoken) */
			if (again) {
				used += (size_t)snprintf(heard + used, heard_room - used, " [seq %u again, %u bytes]", seq, length);
				continue;
			}
		}
		/* another of STP's channels (WMT 4, Bluetooth 0, GPS 2, its own 5...) than the one waited on is not the answer: taken, acked and said */
		if (((b1 >> 4) & 7) != connsys_stp_task) {
			used += (size_t)snprintf(heard + used, heard_room - used, " [type %u, %u bytes]", (b1 >> 4) & 7, length);
			continue;
		}
		return (int)(length < room ? length : room);
	}
}

static void
connsys_btif_dump_state(const char *tag)
{
	uint32_t lsr, iir, ier, fakelcr, sleep_en, dma_en, tri_lvl, wak, handshake;
	uint32_t cpupcr = 0, mcu600 = 0, mcu16c = 0;
	vm_offset_t mcu = ml_io_map(MCU_PHYS, 0x1000);

	if (connsys_btif != 0) {
		lsr = rd(connsys_btif, 0x14);
		iir = rd(connsys_btif, 0x08);
		ier = rd(connsys_btif, 0x04);
		fakelcr = rd(connsys_btif, 0x0c);
		sleep_en = rd(connsys_btif, 0x48);
		dma_en = rd(connsys_btif, 0x4c);
		tri_lvl = rd(connsys_btif, 0x60);
		wak = rd(connsys_btif, 0x64);
		handshake = rd(connsys_btif, 0x6c);
	} else {
		lsr = iir = ier = fakelcr = sleep_en = dma_en = tri_lvl = wak = handshake = 0xdead;
	}
	if (connsys_hif != 0) {
		cpupcr = rd(connsys_hif, 0x104);
	}
	if (mcu != 0) {
		mcu600 = rd(mcu, 0x600);
		mcu16c = rd(mcu, 0x16c);
	}

	redwood_host_keep_add("connsys: BTIF [%s] [%u ms]: LSR %#x (DR=%u OE=%u PE=%u FE=%u THRE=%u TEMT=%u) IIR %#x IER %#x DMA_EN %#x SLEEP %#x TRI %#x WAK %#x HSHK %#x FLCR %#x | MCU pc %#x 600=%#x 16c=%#x",
	    tag, connsys_ms(), lsr, lsr & 1, (lsr >> 1) & 1, (lsr >> 2) & 1, (lsr >> 3) & 1, (lsr >> 5) & 1, (lsr >> 6) & 1,
	    iir, ier, dma_en, sleep_en, tri_lvl, wak, handshake, fakelcr, cpupcr, mcu600, mcu16c);
}

static bool connsys_skip_foreign;       /* WMT answers: an event of another opcode than the command's is not its answer (firmware's own, e.g. 02 16 ..) */
static unsigned connsys_answer_ms;      /* when not 0, how long the next answer may take in full mode (else 1 s) */

static bool
connsys_wmt(const char *what, const uint8_t *command, unsigned length, const uint8_t *expected, unsigned expected_length)
{
	char line[220];
	uint8_t answer[32];
	int got = -1;
	size_t used;
	static bool woken;
	const unsigned wait_ms = connsys_stp_full ? (connsys_answer_ms != 0 ? connsys_answer_ms : 1000) : 3000;

	used = (size_t)snprintf(line, sizeof(line), "%s: (MCU pc %#x)", what, connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	/*
	 * CONNSYS is woken (BTIF_WAK) before each command and again before a resend.
	 * In run 364, connsys_btif_wake had a 1 ms delay which was too short on cold start;
	 * with 20 ms settle time and fresh FIFOs, CONNSYS enters STP receive reliably.
	 */
	/* boot-arg connsys_wake=N: 0 never (Android, which raises it only out of sleep), 1 before each command and a
	 * resend (the default), 2 before every frame (runs 356-358) */
	static uint32_t wake = 2;
	static bool asked;
	if (!asked) {
		asked = true;
		(void)PE_parse_boot_argn("connsys_wake", &wake, sizeof(wake));
	}
	for (unsigned try = 0; try < (connsys_stp_full ? 1 : 3); try++) {
		if (try > 0) {
			connsys_btif_dump_state("before retry resend");
			if (!connsys_stp_full && !connsys_chip_id_done) {
				connsys_mcu_restart();
			}
		}
		if (wake == 2 || (wake == 1 && (!woken || try > 0))) {
			connsys_btif_wake();
			woken = true;
		}
		/* (connsys_wake=3, the command only after the wakeup event, and 4, a 0x00 ahead of it: runs 373-377, gone) */
		connsys_stp_send(command, length);
		got = connsys_stp_receive(answer, sizeof(answer), line, sizeof(line), wait_ms);
		/*
		 * CONNSYS's power-saving events -- 02 03 02 00 00 then 01 sleep, 02 host awake, 03 wakeup (WMT_*_EVT,
		 * core/wmt_core.c:184-191) -- are not answers: Android's STP layer takes them (wmt_ctrl_evt_parser) and
		 * the answer comes after. Run 373: the WMT reset after the patch was "answered" 02 03 02 00 00 03, the
		 * wakeup BTIF_WAK asked for.
		 */
		for (unsigned more = 0; more < 4 && got == 6 && memcmp(answer, "\x02\x03\x02\x00\x00", 5) == 0 &&
		    !((unsigned)got >= expected_length && memcmp(answer, expected, expected_length) == 0); more++) {
			used = strlen(line);
			snprintf(line + used, sizeof(line) - used, " [event %02x]", answer[5]);
			got = connsys_stp_receive(answer, sizeof(answer), line, sizeof(line), wait_ms);
		}
		for (unsigned more = 0; more < 8 && connsys_skip_foreign && connsys_stp_task == 4 && got >= 5 && answer[0] == 0x02 &&
		    expected_length >= 2 && answer[1] != expected[1]; more++) {
			used = strlen(line);
			snprintf(line + used, sizeof(line) - used, " [event %02x, %d bytes]", answer[1], got);
			got = connsys_stp_receive(answer, sizeof(answer), line, sizeof(line), wait_ms);
		}
		if (got >= 0) {
			break;
		}
		used = strlen(line);
		snprintf(line + used, sizeof(line) - used, " . (pc %#x)", connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	}
	used = strlen(line);
	if (got < 0) {
		snprintf(line + used, sizeof(line) - used, " no answer (LSR %#x)", rd(connsys_btif, 0x14));
	}
	for (int i = 0; i < got; i++) {
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x", answer[i]);
	}
	if (got >= 0) {
		snprintf(line + used, sizeof(line) - used, (unsigned)got >= expected_length && memcmp(answer, expected, expected_length) == 0 ?
		    " (as expected)" : " (NOT as expected)");
	}
	redwood_host_keep_add("connsys: step %u [%u ms]: %s", connsys_doing, connsys_ms(), line);
	strlcat(connsys_said, " | ", sizeof(connsys_said));
	strlcat(connsys_said, line, sizeof(connsys_said));
	return got >= 0 && (unsigned)got >= expected_length && memcmp(answer, expected, expected_length) == 0;
}

/*
 * Steps 9 and 10 (connsys_step=9, 10): the MCU's patch, which the MT6768 takes over BTIF and not in its memory
 * (pda_dl_patch_flag = 1 in platform/mt6768.c): in STP's mandatory mode, before the full one, as
 * mtk_wcn_soc_sw_init does it for a chip with that flag -- patch, WMT reset, and only then _wmt_soc_mode_ctrl.
 * The options of this chip (wmt_core_hw_check, "options = 3180da8" in Android's log) have no DLM table and no
 * MCU clock tables around it, and OPT_PATCH_CHECKSUM: the patch is the file less its 28-byte header (WMT_PATCH)
 * and 2 bytes of checksum, 25680 bytes of soc1_0_patch_mcu_1a_1_hdr.bin -- the "frag(26, 680)" Android logs.
 *
 * Where it goes is what Android's wmt_launcher gives the driver (WMT_IOCTL_SET_PATCH_INFO, addRess[4]): the
 * phone's /vendor/bin/wmt_launcher (sha256 39772aec...) reads 16 bytes, seeks to 22, reads 2 (the sequence,
 * its second byte compared with the patch's number) and then the 4 at offset 24 -- 11 c0 01 00 in this file,
 * whose upper half is the "PhVer=0x0001" the driver prints of the same field. mtk_wcn_soc_pda_patch_dwn
 * then sends, all in the mandatory mode:
 *   WMT_PATCH_PDA_CFG_CMD  01 01 11 00 00, the address (4), the size (4, little-endian), a scramble key of 0,
 *                          2f 02 02 00 -- answered 02 01 01 00 00;
 *   a 12-byte header       the size + 12, little-endian, then zeros -- raw, no STP frame around it;
 *   the patch              raw, in pieces of 1000 bytes;
 * and then init_table_3: WMT reset, 01 07 01 00 04, answered 02 07 01 00 00.
 * Step 9 reads the file and says what would be sent; step 10 sends it. True when that went as it should.
 */
/*
 * What CONNSYS's firmware says of itself in its EMI: the control area at EMI+0x68000 (CONSYS_EMI_COREDUMP_OFFSET;
 * ENUM_EMI_CTRL_STATE_OFFSET in platform/include/mtk_wcn_consys_hw.h: state +0, host and chip sync +4..+0x18, print
 * buffer start/len/index +0x1c/+0x20/+0x24, interrupt status +0x28, assert flag +0x100) and the printable text of its
 * print buffer and of the paged trace (+0x400) -- what Android's coredump reads. Run 430: the MCU dies the moment the
 * ROM hands over to the RAM image; whatever it managed to write first is here.
 */
static void
connsys_firmware_said(const char *when)
{
	vm_offset_t ctl = ml_io_map_unmappable(CONNSYS_EMI_PHYS + 0x68000u, 0x4000, 0x7 /* VM_WIMG_IO */);
	char text[170];

	if (ctl == 0) {
		return;
	}
#define C(o) rd(ctl, (o))
	redwood_host_keep_add("connsys: %s: EMI ctl state %#x sync %#x/%#x %#x/%#x/%#x/%#x print %#x+%#x@%#x int %#x dump_end %#x dbglog %#x "
	    "pages %#x sleep %#x assert %#x", when, C(0x0), C(0x4), C(0x8), C(0xc), C(0x10), C(0x14), C(0x18), C(0x1c), C(0x20), C(0x24),
	    C(0x28), C(0x2c), C(0x40), C(0x44), C(0x4c), C(0x100));
	/* the paged trace's first bytes, as text */
	for (unsigned part = 0; part < 3; part++) {
		unsigned n = 0;

		for (unsigned i = 0; i < 160; i++) {
			uint8_t c = *(volatile uint8_t *)(ctl + 0x400 + part * 160 + i);

			text[n++] = c >= 0x20 && c < 0x7f ? (char)c : (c == 0 ? '_' : '.');
		}
		text[n] = 0;
		redwood_host_keep_add("connsys: %s: trace %u: %s", when, part, text);
	}
	/* and the print buffer, where the control area says it is (CONNSYS's 0xf0000000 = EMI+0) */
	uint32_t start = C(0x1c), length = C(0x20);
#undef C
	ml_io_unmap(ctl, 0x4000);
	if (start >= 0xf0000000u && start < 0xf0200000u && length != 0 && length <= 0x10000u) {
		uint32_t at = start - 0xf0000000u, page = at & ~0x3fffu;
		vm_offset_t buf = ml_io_map_unmappable(CONNSYS_EMI_PHYS + page, 0x8000, 0x7);

		if (buf != 0) {
			for (unsigned part = 0; part < 3; part++) {
				unsigned n = 0;

				for (unsigned i = 0; i < 160 && (at - page) + part * 160 + i < 0x8000; i++) {
					uint8_t c = *(volatile uint8_t *)(buf + (at - page) + part * 160 + i);

					text[n++] = c >= 0x20 && c < 0x7f ? (char)c : (c == 0 ? '_' : '.');
				}
				text[n] = 0;
				redwood_host_keep_add("connsys: %s: print %u: %s", when, part, text);
			}
			ml_io_unmap(buf, 0x8000);
		}
	}
}

static bool
connsys_pda_patch(void)
{
	static const uint8_t pda_done[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
	static const uint8_t reset[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
	static const uint8_t reset_done[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };
	uint8_t pda[21] = {
		0x01, 0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f, 0x02, 0x02, 0x00,
	};
	uint8_t head[12] = { 0 };
	uint32_t size = connsys_patch_size, body, pieces = 0;
	uint8_t *file = connsys_patch_file;
	bool free_file = false;
	bool ok;

	connsys_doing = 9;
	if (file == NULL) {
		file = connsys_file("soc1_0_patch_mcu_1a_1_hdr.bin", &size);
		free_file = true;
	}
	if (file == NULL || size <= 28 + 2) {
		redwood_host_keep_add("connsys: step 9 [%u ms]: /fw/connsys/soc1_0_patch_mcu_1a_1_hdr.bin: not read", connsys_ms());
		if (file != NULL && free_file) {
			kfree_data(file, size);
		}
		return false;
	}
	body = size - 28 - 2;
	/*
	 * Where the patch goes: the file's u4PatchVer, 0x0001c011, without its low byte -- 0x1c000. The patch is linked to run
	 * from 0x1c000: 28 of the 30 replacement addresses of its ROM patch table (0x3fffffff, 30 ROM addresses, then at
	 * +0x108 the 30 replacements: ROM 0x5200 -> 0x1c808 ...) begin a function only if its first byte is at 0x1c000, none if
	 * it is at 0x1c010 (nds32le-elf-objdump of the file, 2 November). Sent to 0x1c011 as it is, every hook jumped into the
	 * middle of other code: the MCU's exception in ROM 0x52A2, inside the hooked 0x5200, runs 398-436. To 0x1c000, run
	 * 437: the MCU back at 0xb04, WMT reset answered, STP in its full mode. Boot-arg connsys_pda_addr=0xN: N instead.
	 */
	{
		uint32_t target = 0, override = 0;

		memcpy(&target, file + 24, 4);
		target &= ~0xffu;
		if (PE_parse_boot_argn("connsys_pda_addr", &override, sizeof(override)) && override != 0) {
			target = override;
		}
		memcpy(&pda[5], &target, 4);
	}
	for (unsigned i = 0; i < 4; i++) {
		pda[9 + i] = (uint8_t)(body >> (8 * i));
		head[i] = (uint8_t)((body + sizeof(head)) >> (8 * i));
	}
	redwood_host_keep_add("connsys: step 9 [%u ms]: patch %.16s (%.4s, versions %02x%02x/%02x%02x): %u bytes, %u pieces of 1000, the last %u; "
	    "to %02x %02x %02x %02x", connsys_ms(), (const char *)file, (const char *)file + 16, file[20], file[21], file[22], file[23], body,
	    (body + 999) / 1000, body - (body - 1) / 1000 * 1000, pda[5], pda[6], pda[7], pda[8]);
	if (connsys_step < 10) {
		if (free_file) {
			kfree_data(file, size);
		}
		return false;
	}
	connsys_doing = 10;
	connsys_firmware_said("before the patch");
	/*
	 * BTIF_WAK, the AP's wake-up line to CONNSYS, down before the patch and left down. connsys_btif_wake pulses it
	 * low then high and leaves it HIGH (run 428: "WAK was 0x1"); after the patch the interrupts go to the RAM image's
	 * handlers, and a wake-up held asserted is an interrupt pending at once -- run 431's exception is in the handler
	 * of interrupt 8, before the RAM image has set anything up. (Run 432: the ROM does not read through WMT_SET_REG,
	 * all zeros at 0x5280-0x52bc; the patch's target 0x1c010 keeps a word; SPM+0x6f8/0x6fc are not it.)
	 * Boot-arg connsys_wakelow=0: left as it is.
	 */
	{
		uint32_t low = 1, before = rd(connsys_btif, 0x64);

		(void)PE_parse_boot_argn("connsys_wakelow", &low, sizeof(low));
		if (low != 0) {
			*(volatile uint32_t *)(connsys_btif + 0x64) = before & ~1u;
			delay_for_interval(5, NSEC_PER_MSEC);
		}
		redwood_host_keep_add("connsys: step 10 [%u ms]: BTIF_WAK %#x -> %#x before the patch", connsys_ms(), before,
		    rd(connsys_btif, 0x64));
	}
	ok = connsys_wmt("PDA configuration", pda, sizeof(pda), pda_done, sizeof(pda_done));
	if (ok) {
		/* Stream head then 26 pieces of the patch continuously; the MCU's pc after each piece and, every 5 ms,
		 * after the last -- when it leaves the ROM's 0xb04 for the 0 of a CPU in reset (run 398) */
		char pcs[400];
		size_t used = 0;

		pcs[0] = 0;
		connsys_btif_write(head, sizeof(head));
		for (uint32_t at = 0; at < body; at += 1000, pieces++) {
			connsys_btif_write(file + 28 + 2 + at, body - at < 1000 ? body - at : 1000);
			used += (size_t)snprintf(pcs + used, sizeof(pcs) - used, " %x", rd(connsys_hif, 0x104));
		}
		for (unsigned t = 0; t < 50000 && !(rd(connsys_btif, 0x14) & (1u << 6)); t++) {
			delay(10);
		}
		used += (size_t)snprintf(pcs + used, sizeof(pcs) - used, " | after:");
		for (unsigned n = 0; n < 10 && used < sizeof(pcs); n++) {
			delay_for_interval(5, NSEC_PER_MSEC);
			used += (size_t)snprintf(pcs + used, sizeof(pcs) - used, " %x", rd(connsys_hif, 0x104));
		}
		redwood_host_keep_add("connsys: step 10 [%u ms]: %u pieces of the patch sent, LSR %#x", connsys_ms(), pieces, rd(connsys_btif, 0x14));
		redwood_host_keep_add("connsys: step 10: MCU pc per piece:%s", pcs);
		connsys_firmware_said("after the patch");
		/* (run 428: then CONN_HIF_ON 0x30001 -- HCLK, OSC on, SLEEP off: awake, not asleep -- and BTIF_WAK raised and let
		 * fall changes nothing; the pc stays 0) */
		/* (the AP's own reset of CONNSYS's CPU: TOPRGU+0x18 bit 12, connsys_mcu_restart -- set would be ours) */
		redwood_host_keep_add("connsys: step 10: TOPRGU+0x18 %#x (CONNSYS CPU reset by the AP: %s)", rd(connsys_rgu, 0x18),
		    (rd(connsys_rgu, 0x18) & (1u << 12)) ? "HELD" : "not held");
		connsys_build_time(file, 0x68330u);
		delay_for_interval(50, NSEC_PER_MSEC);
		connsys_btif_dump_state("before WMT reset");
		ok = connsys_wmt("WMT reset", reset, sizeof(reset), reset_done, sizeof(reset_done));
	}
	if (free_file) {
		kfree_data(file, size);
	}
	return ok;
}

/*
 * Step 11 (connsys_step=11): what mtk_wcn_soc_sw_init sends after the full mode (core/wmt_ic_soc.c), for this chip's
 * options (0x3180da8) and this phone's /vendor/firmware/WMT_SOC.cfg (coex_wmt_ant_mode=1, co_clock_flag=1,
 * bt_tssi_from_wifi=3, bt_tssi_target=3776, wifi_ant_swap_mode=0; no wifi_config, no elna_gain_p1_support), in its
 * order -- Android's dmesg of run 354 (logs/android-dmesg-354.txt, 7.266-8.349 s) agrees with each branch taken:
 *   set chip ID          01 02 05 00 11 68 67 00 00       02 02 01 00 00
 *   A-die                01 02 01 00 12                   02 02 05 00 00 + its ID (OPT_QUERY_ADIE)
 *   LTE coex, table 3    01 10 02 00 15 01                02 10 01 00 00 (coex_wmt_filter_mode 0; 5 ms before it)
 *   BT TSSI from Wi-Fi   01 02 04 00 10 03 c0 0e          02 02 01 00 00 (the new op ID 0x10; 3776 = 0x0ec0)
 *   RF calibration       01 14 01 00 01                   02 14 + data -- 1.08 s on Android and here (439), 3 s allowed
 *   coex adjust (3)      01 10 04 00 1e|1f|20 00 00 00    02 10 01 00 00 (its new flags; Android's values all 0)
 *   FM strap             01 05 02 00 02 02                02 05 02 00 00 02 (WMT_FM_COMM)
 *   UTC time             01 f0 09 00 02, s, us            02 f0 02 00 02 00 (opfunc_utc_time_sync, after a function on)
 * It stops where Android's does (A-die, calibration, FM strap) and goes on past the rest, which Android only logs.
 * Not sent, as Android does not for these options and this file: Wi-Fi's config, the external component, EPA, eLNA,
 * coex before the calibration, antenna swap, the oscillator type, the core-dump level. The BT and Wi-Fi PALDO it
 * raises around the calibration are #if 0 on this chip (platform/mt6768.c): step 3's VCN33 is all there is.
 * Wi-Fi is not a WMT command on a SoC: mtk_wcn_wmt_func_on(WIFI) calls the Wi-Fi driver's probe (gen4m, chips/connac
 * on AXI), which loads WIFI_RAM_CODE itself -- the next step.
 */
static void connsys_bt_start(void);
extern bool RedwoodBluetoothNode(void);        /* iokit/Kernel/RedwoodBluetooth.cpp */

static void
connsys_sw_init(void)
{
	static const uint8_t chip_id[] = { 0x01, 0x02, 0x05, 0x00, 0x11, 0x68, 0x67, 0x00, 0x00 };
	static const uint8_t a_die[] = { 0x01, 0x02, 0x01, 0x00, 0x12 };
	static const uint8_t a_die_said[] = { 0x02, 0x02, 0x05, 0x00, 0x00 };
	static const uint8_t lte_coex[] = { 0x01, 0x10, 0x02, 0x00, 0x15, 0x01 };
	static const uint8_t tssi[] = { 0x01, 0x02, 0x04, 0x00, 0x10, 0x03, 0xc0, 0x0e };
	static const uint8_t calibration[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
	/* (its answer is the opcode and data -- run 439: 450 bytes, 00 01 and Bluetooth's 448 of calibration, Android's
	 * gBTCalResultSize -- and Android compares only that it is 0x14's, wmt_core_init_script_retry: "do not care") */
	static const uint8_t calibrated[] = { 0x02, 0x14 };
	static const uint8_t adjust_bt[] = { 0x01, 0x10, 0x04, 0x00, 0x1e, 0x00, 0x00, 0x00 };
	static const uint8_t adjust_opp[] = { 0x01, 0x10, 0x04, 0x00, 0x1f, 0x00, 0x00, 0x00 };
	static const uint8_t adjust_ble[] = { 0x01, 0x10, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00 };
	static const uint8_t fm_strap[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
	static const uint8_t fm_strap_said[] = { 0x02, 0x05, 0x02, 0x00, 0x00, 0x02 };
	static const uint8_t wmt_done[] = { 0x02, 0x02, 0x01, 0x00, 0x00 };
	static const uint8_t coex_done[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };
	static const uint8_t utc_said[] = { 0x02, 0xf0, 0x02, 0x00, 0x02, 0x00 };
	static const struct {
		const char *what;
		const uint8_t *command, *expected;
		uint8_t length, expected_length;
		bool must;
		unsigned wait_ms;
	} script[] = {
#define S(what, c, e, must, wait) { what, c, e, sizeof(c), sizeof(e), must, wait }
		S("set chip ID 0x6768", chip_id, wmt_done, false, 0),
		S("A-die", a_die, a_die_said, true, 0),
		S("LTE coex (table 3)", lte_coex, coex_done, false, 0),
		S("BT TSSI from Wi-Fi", tssi, wmt_done, false, 0),
		S("RF calibration", calibration, calibrated, true, 3000),
		S("coex: BT control", adjust_bt, coex_done, false, 0),
		S("coex: opp time ratio", adjust_opp, coex_done, false, 0),
		S("coex: BLE scan time ratio", adjust_ble, coex_done, false, 0),
		S("FM strap (COMM)", fm_strap, fm_strap_said, true, 0),
#undef S
	};
	uint8_t utc[] = { 0x01, 0xf0, 0x09, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0 };
	clock_sec_t secs;
	clock_usec_t usecs;
	unsigned answered = 0;

	connsys_doing = 11;
	for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		bool ok;

		if (script[i].command == lte_coex) {
			delay(5000);                    /* (wmt_stp_wifi_lte_coex's osal_sleep_ms(5)) */
		}
		connsys_answer_ms = script[i].wait_ms;
		ok = connsys_wmt(script[i].what, script[i].command, script[i].length, script[i].expected, script[i].expected_length);
		connsys_answer_ms = 0;
		answered += ok;
		if (!ok && script[i].must) {
			redwood_host_keep_add("connsys: step 11 [%u ms]: stopped at \"%s\", as Android would", connsys_ms(), script[i].what);
			connsys_firmware_said("step 11 stopped");
			return;
		}
	}
	clock_get_calendar_microtime(&secs, &usecs);
	for (unsigned i = 0; i < 4; i++) {
		utc[5 + i] = (uint8_t)((uint32_t)secs >> (8 * i));
		utc[9 + i] = (uint8_t)((uint32_t)usecs >> (8 * i));
	}
	answered += connsys_wmt("UTC time", utc, sizeof(utc), utc_said, sizeof(utc_said));
	redwood_host_keep_add("connsys: step 11 [%u ms]: %u of %u answered as Android's are -- WMT is up; Wi-Fi itself is its driver's (gen4m)",
	    connsys_ms(), answered, (unsigned)(sizeof(script) / sizeof(script[0])) + 1);
	connsys_firmware_said("after step 11");
	if (connsys_step >= 12) {
		redwood_mtkwifi_start(connsys_step);
	}
	connsys_bt_start();
}


/*
 * Bluetooth, second step (connsys_bt=2): the controller behind the serial ports iOS's bluetoothd looks for
 * (/dev/cu.bluetooth, then /dev/uart.bluetooth: "No valid serial port found (tried cu.bluetooth and uart.bluetooth)" in
 * bluetoothd's strings; H4, a type byte before each packet). A character device pair of the kernel's own: what the daemon
 * writes is cut into H4 packets and sent on STP's channel 0, what the controller sends comes back as a byte stream; the
 * terminal ioctls (tcgetattr, tcsetattr, a baud rate) are taken and said, a line each time the command is new.
 * One thread (the one that brought the controller up) does all of BTIF: a write is sent within the 2 ms its read waits.
 */
static LCK_GRP_DECLARE(bt_lck_grp, "redwood-bt");
static LCK_MTX_DECLARE(bt_lock, &bt_lck_grp);
#define BT_RING         65536u
#define BT_TXQ          8192u
static uint8_t bt_ring[BT_RING];
static unsigned bt_ring_head, bt_ring_count;    /* the bytes the controller sent, not yet read */
static uint8_t bt_txq[BT_TXQ];
static unsigned bt_txq_count;                   /* what has been written, not yet sent (maybe half a packet) */
static struct selinfo bt_sel;
static unsigned bt_opens;
static uint32_t bt_said_packets_in, bt_said_packets_out;

static void
bt_dump(const char *direction, const uint8_t *bytes, unsigned length, uint32_t *counter)
{
	char text[3 * 32 + 1];

	if (direction[0] == 'i' && length == 7 && bytes[1] == 0x0e && bytes[2] == 4 && bytes[6] == 0 && (bytes[5] >> 2) != 0x3f && false) {
		return;                 /* a plain Command Complete, status 0: the command above it says enough (the vendor ones are said) */
	}
	{
		/* everything, for 30 s after a classic connection is asked for (Create_Connection 0x0405), up to 500 lines: what comes of it */
		static uint32_t until_ms;
		static unsigned connection_said;

		if (direction[0] == 'o' && length >= 4 && bytes[0] == 1 && bytes[1] == 0x05 && bytes[2] == 0x04) {
			until_ms = connsys_ms() + 30000;
		}
		if (until_ms != 0 && (int32_t)(until_ms - connsys_ms()) > 0 && connection_said < 500 && *counter >= 130 && length < 400) {
			connection_said++;
			(*counter)--;
		}
	}
	{
		/* beyond the first of each direction, only what a scan is made of: its commands (LE_Set_Scan_Parameters 0x200b, _Enable 0x200c, the
		 * extended ones 0x2041, 0x2042) and the LE Meta events (0x3e) that come of it, a few dozen */
		static unsigned scan_said;
		/* ... and a connection's: the link-control commands (OGF 1: Inquiry, Create_Connection, pairing's answers), LE_Create_Connection (0x200d), and their events */
		const bool scan = (length >= 4 && bytes[0] == 1 && bytes[2] == 0x20 && (bytes[1] == 0x0b || bytes[1] == 0x0c || bytes[1] == 0x0d || bytes[1] == 0x41 || bytes[1] == 0x42)) ||
		    (length >= 4 && bytes[0] == 1 && bytes[2] == 0x04) ||
		    (length >= 4 && bytes[0] == 4 && (bytes[1] == 0x3e || bytes[1] == 0x03 || bytes[1] == 0x04 || bytes[1] == 0x05 || bytes[1] == 0x06 || bytes[1] == 0x07 || bytes[1] == 0x0b || bytes[1] == 0x0c ||
		    bytes[1] == 0x17 || bytes[1] == 0x18 || bytes[1] == 0x1d || bytes[1] == 0x22 || bytes[1] == 0x31 || bytes[1] == 0x32 || bytes[1] == 0x33 || bytes[1] == 0x36 || bytes[1] == 0x3d)) ||
		    (length >= 4 && bytes[0] == 4 && bytes[1] == 0x0f && !(bytes[4] == 0 && bytes[5] == 0x01 && bytes[6] == 0x04 && length == 7)) ||
		    (length >= 6 && bytes[0] == 2);          /* ACL data, both ways */

		const bool le = (length >= 4 && bytes[0] == 1 && bytes[2] == 0x20) || (length >= 4 && bytes[0] == 4 && bytes[1] == 0x3e);
		static unsigned le_said;

		if (*counter >= 130 && scan && le && le_said < 60) {
			le_said++;
			(*counter)--;
		} else if (*counter >= 130 && scan && !le && scan_said < 300) {
			scan_said++;
			(*counter)--;
		}
	}
	if ((*counter)++ >= 130) {              /* the first of each direction: the host buffer is 64 KB and bluetoothd's own lines (src/inject.c) want it */
		return;
	}
	for (unsigned at = 0; at < length && at < (bytes[0] == 4 && bytes[1] == 0x3e ? 96u : 32u); at += 32) {
		unsigned n = length - at < 32 ? length - at : 32;

		for (unsigned i = 0; i < n; i++) {
			snprintf(text + 3 * i, 4, " %02x", bytes[at + i]);
		}
		text[3 * n] = 0;
		redwood_host_keep_add("connsys: bt %s %u bytes%s%s", direction, length, at == 0 ? ":" : " (cont.):", text);
	}
}

/*
 * What this controller says of itself that Apple's stack will not take as it is. Read_Local_Supported_Commands (the event
 * 04 0e 44 01 02 10 00 and 64 octets): the MediaTek controller leaves the LE commands of octets 25-28 clear although it
 * answers them (run 602: LE_Read_Buffer_Size), and bluetoothd, which looks in that mask before it sends one, does not send
 * LE_Read_Local_Supported_Features and gives up ("HCI initialization failed", run 615). The standard Core 5.2 bits of those
 * octets are set where the controller has none (OR, never cleared).
 */
static uint8_t bt_vsc_sub;              /* the first parameter of the last 0xfce2 command sent: its kind (0x0b, 0x1c: the antenna's, which have a byte in their answers; 0x15: LE features, which has none) */

static unsigned
bt_quirks(uint8_t *packet, unsigned length)
{
	static const uint8_t le_commands[4] = { 0xf7, 0xff, 0xff, 0x7f };

	/*
	 * Write_Default_Erroneous_Data_Reporting (0x0c5b) is a Core 2.1 command the MediaTek controller answers "Unknown HCI Command"
	 * (Command Status 04 0f 04 01 01 5b 0c, run 617), and bluetoothd, which sends it in its start-up, takes that for a controller
	 * that does not work ("HCI initialization failed", every 10 s). The setting means nothing to a controller that does not
	 * report erroneous data in the first place, so it is answered as a done one: Command Complete, status 0.
	 */
	/*
	 * The vendor commands of Apple's stack (OGF 0x3f: 0xfce9, 0xfce2, 0xfd47, 0xfd48 ... in run 627) are Broadcom's, for a chip
	 * this one is not; the MediaTek controller says "Unknown HCI Command" to each (Command Status, status 1) and bluetoothd,
	 * a few seconds after the last one, exits with 1 and is started again (every ~17 s, runs 626 and 627). They configure
	 * Broadcom's antenna, coexistence and address handling, which has no meaning here: each is answered as done, Command
	 * Complete with status 0 and nothing else.
	 */
	if (length == 7 && packet[0] == 0x04 && packet[1] == 0x0f && packet[2] == 0x04 && packet[3] == 0x01 && packet[4] == 0x01 &&
	    (packet[6] >> 2) == 0x3f) {
		/*
		 * Status 0, and for 0xfd48 one more byte of 0: bluetoothd reads a byte of data after that one's status (AssertFail
		 * bcm_vsc.c:642 ByteStream_NumReadBytesAvail >= 1, run 629), and takes one after the others' for a failure -- with the byte
		 * on all of them BD_VSC_LE_META_ENA_FEATURES (0xfce2) failed with reason 3602 and bluetoothd restarted (run 638).
		 */
		/*
		 * Some of them are read for data after the status: 0xfd48 for at least a byte (AssertFail bcm_vsc.c:642 ByteStream_NumReadBytesAvail
		 * >= 1, run 629), the antenna's -- 0xfce2 with sub-command 0x0b or 0x1c, and 0xfce9 (BD_VSC_OLYMPIC_ANTENNA_PREF_B is the first
		 * of the vendor commands to be sent, and its answer is a byte stream: "Invalid data size 0" in run 627, then "VSC stall
		 * detected", reason 3602, which restarts bluetoothd, runs 626-646) -- for some. A parser asks that enough bytes are there, not
		 * that no more are, so these get 16 of zeros; 0xfce2 with sub-command 0x15 (BD_VSC_LE_META_ENA_FEATURES) takes none (with a
		 * byte it was the one that failed, run 638).
		 */
		/*
		 * Which is which, by bluetoothd's own "Sending: BD_VSC_..." lines against the wire (run 659, src/inject.c's watcher): 0xfce9 is
		 * BD_VSC_LE_META_ENA_FEATURES, 0xfce2 with 0x0b CHANGE_LE_SETTINGS, with 0x1c SET_MIN_AFH_LE_CHANNELS, 0xfd47
		 * ENABLE_EXTENDED_DUPLICATE_FILTER -- each completed by the status alone (data on 0xfce9 left the next one unsent, runs
		 * 650-658) -- and 0xfce2 with 0x15 (header 15 00 00 00, then six bytes of BT_VSCAntPrefParams_t) is
		 * BD_VSC_OLYMPIC_ANTENNA_PREF_B, whose callback is a byte stream: with the status alone it stalled and bluetoothd restarted
		 * (VSC stall, reason 3602, runs 626-659), so it gets data after the status; 0xfd48 reads a byte after the status.
		 */
		const bool antenna = packet[5] == 0xe2 && packet[6] == 0xfc && (bt_vsc_sub == 0x15 || bt_vsc_sub == 0x36);     /* (0x36: BT_VSC_OLYMPIC_SET_RETAIN_LE_SCAN_DUPLICATES_ON_START, run 673: a scan's start stalled on it) */
		const bool cal = packet[5] == 0x1b && packet[6] == 0xfd;          /* 0xfd1b with 0x51: BD_VSC_FACTORY_CAL_READ_TABLE (run 669): a table is read */
		const bool data = antenna || cal || (packet[5] == 0x48 && packet[6] == 0xfd);
		const unsigned extra = cal ? 64 : (antenna ? 16 : (data ? 1 : 0));

		packet[1] = 0x0e;
		packet[2] = (uint8_t)(4 + extra);
		packet[4] = packet[5];
		packet[5] = packet[6];
		packet[6] = 0x00;
		for (unsigned i = 0; i < extra; i++) {
			packet[7 + i] = 0x00;
		}
		if (cal) {
			packet[7] = 0x51;               /* the table's number, as the Broadcom ones answer a read of a table (a guess) */
		}
		return 7 + extra;
	}
	if (length == 7 && memcmp(packet, "\x04\x0f\x04\x01\x01\x5b\x0c", 7) == 0) {
		packet[1] = 0x0e;
		packet[4] = 0x5b;
		packet[5] = 0x0c;
		packet[6] = 0x00;
		return 7;
	}
	if (length >= 7 + 64 && memcmp(packet, "\x04\x0e\x44\x01\x02\x10\x00", 7) == 0) {
		for (unsigned i = 0; i < 4; i++) {
			packet[7 + 25 + i] |= le_commands[i];
		}
		packet[7 + 24] |= 0x60;         /* octet 24 bits 5, 6: Read and Write LE Host Support (run 616: WriteLeHostSupported, status 628) */
	}
	return length;
}

static int
bt_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	lck_mtx_lock(&bt_lock);
	bt_opens++;
	lck_mtx_unlock(&bt_lock);
	redwood_host_keep_add("connsys: bt device %u opened by %s (pid %d) with flags %#x, %u open", minor(dev), name, proc_selfpid(), flags, bt_opens);
	return 0;
}

static int
bt_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&bt_lock);
	if (bt_opens > 0) {
		bt_opens--;
	}
	lck_mtx_unlock(&bt_lock);
	redwood_host_keep_add("connsys: bt device %u closed (pid %d), %u open", minor(dev), proc_selfpid(), bt_opens);
	return 0;
}

#define BT_INERT(dev)   (minor(dev) >= 2)      /* btwake, btreset, btpoweroff: BlueTool opens them, nothing is behind them */

static int
bt_read(dev_t dev, struct uio *uio, int ioflag)
{
	int error = 0;

	if (BT_INERT(dev)) {
		return 0;
	}
	lck_mtx_lock(&bt_lock);
	while (bt_ring_count == 0) {
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };

		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&bt_lock);
			return EWOULDBLOCK;
		}
		error = msleep(&bt_ring_count, &bt_lock, PZERO | PCATCH, "redwood-bt", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&bt_lock);
			return error;
		}
	}
	while (bt_ring_count > 0 && uio_resid(uio) > 0) {
		uint8_t chunk[512];
		unsigned n = bt_ring_count < sizeof(chunk) ? bt_ring_count : (unsigned)sizeof(chunk);

		if ((user_ssize_t)n > uio_resid(uio)) {
			n = (unsigned)uio_resid(uio);
		}
		for (unsigned i = 0; i < n; i++) {
			chunk[i] = bt_ring[(bt_ring_head + i) % BT_RING];
		}
		lck_mtx_unlock(&bt_lock);
		error = uiomove((caddr_t)chunk, (int)n, uio);
		lck_mtx_lock(&bt_lock);
		if (error != 0) {
			break;
		}
		bt_ring_head = (bt_ring_head + n) % BT_RING;
		bt_ring_count -= n;
	}
	lck_mtx_unlock(&bt_lock);
	return error;
}

static int
bt_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (BT_INERT(dev)) {
		char text[48];
		int n = (int)(uio_resid(uio) < 40 ? uio_resid(uio) : 40);

		if (uiomove(text, n, uio) == 0) {
			redwood_host_keep_add("connsys: bt device %u written %d byte(s): %02x", minor(dev), n, n > 0 ? (uint8_t)text[0] : 0);
		}
		uio_setresid(uio, 0);
		return 0;
	}
	while (uio_resid(uio) > 0) {
		uint8_t chunk[512];
		unsigned n = (unsigned)(uio_resid(uio) < (user_ssize_t)sizeof(chunk) ? uio_resid(uio) : (user_ssize_t)sizeof(chunk));
		int error = uiomove((caddr_t)chunk, (int)n, uio);

		if (error != 0) {
			return error;
		}
		lck_mtx_lock(&bt_lock);
		if (bt_txq_count + n > BT_TXQ) {
			lck_mtx_unlock(&bt_lock);
			return ENOBUFS;
		}
		memcpy(bt_txq + bt_txq_count, chunk, n);
		bt_txq_count += n;
		lck_mtx_unlock(&bt_lock);
	}
	return 0;
}

static int
bt_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	static u_long said[48];
	static unsigned said_count;
	bool known = false;
	int result = 0;

	switch (cmd) {
	case TIOCGETA: {
		struct termios *t = (struct termios *)data;

		bzero(t, sizeof(*t));
		t->c_cflag = CS8 | CREAD | CLOCAL | HUPCL;
		t->c_ispeed = t->c_ospeed = 3000000;
		known = true;
		break;
	}
	case TIOCSETA: case TIOCSETAW: case TIOCSETAF: case TIOCEXCL: case TIOCNXCL: case TIOCSDTR: case TIOCCDTR:
	case TIOCMBIS: case TIOCMBIC: case TIOCMSET: case TIOCFLUSH: case TIOCSBRK: case TIOCCBRK: case TIOCDRAIN:
	case TIOCSCTTY: case TIOCSTI:
		known = true;
		break;
	case TIOCMGET:
		*(int *)data = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR | TIOCM_RTS | TIOCM_DTR;
		known = true;
		break;
	case FIONREAD:
		lck_mtx_lock(&bt_lock);
		*(int *)data = (int)bt_ring_count;
		lck_mtx_unlock(&bt_lock);
		known = true;
		break;
	case FIONBIO: case FIOASYNC:
		known = true;
		break;
	default:
		result = ENOTTY;
		break;
	}
	{
		bool seen = false;

		for (unsigned i = 0; i < said_count; i++) {
			seen |= said[i] == cmd;
		}
		if (!seen && said_count < sizeof(said) / sizeof(said[0])) {
			said[said_count++] = cmd;
			redwood_host_keep_add("connsys: bt ioctl %#lx (group '%c' number %lu, %lu bytes): %s", cmd, (char)((cmd >> 8) & 0xff),
			    cmd & 0xff, (cmd >> 16) & 0x1fff, known ? "taken" : "not a command of this device (ENOTTY)");
		}
	}
	return result;
}

static int
bt_select(dev_t dev, int which, void *wql, struct proc *p)
{
	int ready = 0;

	if (BT_INERT(dev)) {
		return 1;
	}
	if (which == FREAD) {
		lck_mtx_lock(&bt_lock);
		ready = bt_ring_count != 0;
		if (!ready) {
			selrecord(p, &bt_sel, wql);
		}
		lck_mtx_unlock(&bt_lock);
	} else if (which == FWRITE) {
		ready = 1;
	}
	return ready;
}

static const struct cdevsw bt_cdevsw = {
	.d_open = bt_open, .d_close = bt_close, .d_read = bt_read, .d_write = bt_write,
	.d_ioctl = bt_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = bt_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* the length of the H4 packet that begins the bytes, or 0 while its header is not whole */
static unsigned
bt_h4_length(const uint8_t *b, unsigned have)
{
	if (have < 1) {
		return 0;
	}
	switch (b[0]) {
	case 1:                                 /* command: opcode (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 2:                                 /* ACL: handle (2), length (2) */
		return have >= 5 ? 5u + (b[3] | ((unsigned)b[4] << 8)) : 0;
	case 3:                                 /* SCO: handle (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 5:                                 /* ISO: handle (2), length (2, 14 bits) */
		return have >= 5 ? 5u + ((b[3] | ((unsigned)b[4] << 8)) & 0x3fff) : 0;
	default:
		return 1;                       /* not H4: dropped a byte at a time */
	}
}


/*
 * The way bluetoothd's own transport reaches the controller (run 611): its default, H4BC ("UART", BM3UART in its strings), looks
 * up the kernel control com.apple.uart.sk.bluetooth (ioctl CTLIOCGINFO on a PF_SYSTEM socket) -- Apple's Bluetooth UART as a
 * Skywalk provider -- and the plain "H4" transport does not match any driver. This is that control: what bluetoothd sends on the
 * socket is H4 for the controller (the same queue /dev/cu.bluetooth's writes go to), and what the controller sends is enqueued to
 * it. Every control request (connect, options, disconnect) is said; unknown options are taken and ignored.
 */
extern void redwood_btsk_init(void);
extern bool redwood_btsk_uuid(uuid_t out);
extern void redwood_btsk_deliver(const uint8_t *bytes, unsigned length);

static kern_ctl_ref bt_ctl_ref;
static u_int32_t bt_ctl_unit;
static bool bt_ctl_connected;

static errno_t
bt_ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	lck_mtx_lock(&bt_lock);
	bt_ctl_unit = sac->sc_unit;
	bt_ctl_connected = true;
	lck_mtx_unlock(&bt_lock);
	redwood_host_keep_add("connsys: bt control connected by %s (pid %d): unit %u", name, proc_selfpid(), sac->sc_unit);
	return 0;
}

static errno_t
bt_ctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	lck_mtx_lock(&bt_lock);
	bt_ctl_connected = false;
	lck_mtx_unlock(&bt_lock);
	redwood_host_keep_add("connsys: bt control: unit %u disconnected", unit);
	return 0;
}

static errno_t
bt_ctl_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	errno_t error = 0;

	lck_mtx_lock(&bt_lock);
	for (mbuf_t part = m; part != NULL; part = mbuf_next(part)) {
		size_t n = mbuf_len(part);

		if (bt_txq_count + n > BT_TXQ) {
			error = ENOBUFS;
			break;
		}
		memcpy(bt_txq + bt_txq_count, mbuf_data(part), n);
		bt_txq_count += (unsigned)n;
	}
	lck_mtx_unlock(&bt_lock);
	mbuf_freem(m);
	return error;
}

static errno_t
bt_ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	char text[3 * 24 + 1];

	redwood_host_keep_add("connsys: bt control: setsockopt %#x, %zu bytes", (unsigned)opt, len);
	for (size_t at = 0; at < len && at < 96; at += 24) {
		size_t n = len - at < 24 ? len - at : 24;

		for (size_t i = 0; i < n; i++) {
			snprintf(text + 3 * i, 4, " %02x", ((uint8_t *)data)[at + i]);
		}
		text[3 * n] = 0;
		redwood_host_keep_add("connsys: bt control:   +%zu:%s", at, text);
	}
	return 0;
}

/* IOAOSSKYGETCHANNELUUID (getsockopt 0x40105412, 16 bytes): the Skywalk channel's nexus */
static errno_t
bt_ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	uuid_t uuid;
	bool have = (unsigned)opt == 0x40105412u && *len >= sizeof(uuid_t) && redwood_btsk_uuid(uuid);

	redwood_host_keep_add("connsys: bt control: getsockopt %#x, room for %zu bytes: %s", (unsigned)opt, *len, have ? "the channel's UUID" : "nothing");
	if (have) {
		memcpy(data, uuid, sizeof(uuid_t));
		*len = sizeof(uuid_t);
	} else {
		*len = 0;
	}
	return 0;
}

/* H4 bytes the Skywalk channel's user wrote, for the controller: the queue /dev/cu.bluetooth's writes go to */
void
redwood_connsys_bt_send(const uint8_t *bytes, unsigned length)
{
	lck_mtx_lock(&bt_lock);
	if (bt_txq_count + length <= BT_TXQ) {
		memcpy(bt_txq + bt_txq_count, bytes, length);
		bt_txq_count += length;
	}
	lck_mtx_unlock(&bt_lock);
}

static void
bt_ctl_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;        /* bluetoothd connects a SOCK_STREAM (a datagram socket: "Protocol wrong type for socket", run 613) */
	reg.ctl_sendsize = 65536;
	reg.ctl_recvsize = 65536;
	reg.ctl_connect = bt_ctl_connect;
	reg.ctl_disconnect = bt_ctl_disconnect;
	reg.ctl_send = bt_ctl_send;
	reg.ctl_setopt = bt_ctl_setopt;
	reg.ctl_getopt = bt_ctl_getopt;
	error = ctl_register(&reg, &bt_ctl_ref);
	redwood_host_keep_add("connsys: step 21: kernel control com.apple.uart.sk.bluetooth %s (%d)", error == 0 ? "registered" : "NOT REGISTERED", error);
}

static void
bt_ctl_deliver(const uint8_t *bytes, unsigned length)
{
	bool connected;
	u_int32_t unit;

	lck_mtx_lock(&bt_lock);
	connected = bt_ctl_connected;
	unit = bt_ctl_unit;
	lck_mtx_unlock(&bt_lock);
	redwood_btsk_deliver(bytes, length);
	if (connected && bt_ctl_ref != NULL && false) {
		errno_t error = ctl_enqueuedata(bt_ctl_ref, unit, (void *)(uintptr_t)bytes, length, 0);

		if (error != 0) {
			redwood_host_keep_add("connsys: bt control: enqueue of %u bytes failed (%d)", length, error);
		}
	}
}

static uint8_t bt_frame[2048];

/* what is written is sent a packet at a time; what comes is put in the ring. Never returns. */
static void
bt_bridge(void)
{
	static bool made;
	char heard[96];

	if (!made) {
		int major = cdevsw_add(-1, &bt_cdevsw);

		if (major < 0) {
			redwood_host_keep_add("connsys: bt: cdevsw_add failed (%d)", major);
			return;
		}
		(void)cdevsw_setkqueueok(major, &bt_cdevsw, CDEVSW_SELECT_KQUEUE);
		made = devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth") != NULL;
		made = devfs_make_node(makedev(major, 1), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth") != NULL && made;
		made = devfs_make_node(makedev(major, 2), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btwake") != NULL && made;
		made = devfs_make_node(makedev(major, 3), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btreset") != NULL && made;
		made = devfs_make_node(makedev(major, 4), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btpoweroff") != NULL && made;
		redwood_host_keep_add("connsys: step 21 [%u ms]: /dev/cu.bluetooth and /dev/uart.bluetooth %s (major %d)", connsys_ms(),
		    made ? "made" : "NOT MADE", major);
		if (!made) {
			return;
		}
	}
	bt_ctl_register();
	redwood_btsk_init();
	connsys_stp_task = 0;
	for (;;) {
		int length;

		for (;;) {
			uint8_t packet[1100];
			unsigned n;

			lck_mtx_lock(&bt_lock);
			n = bt_h4_length(bt_txq, bt_txq_count);
			if (n == 0 || n > bt_txq_count) {
				lck_mtx_unlock(&bt_lock);
				break;
			}
			if (n > sizeof(packet)) {
				n = bt_txq_count;
				redwood_host_keep_add("connsys: bt: a packet of %u bytes is too long for STP; dropped", n);
				bt_txq_count = 0;
				lck_mtx_unlock(&bt_lock);
				break;
			}
			memcpy(packet, bt_txq, n);
			memmove(bt_txq, bt_txq + n, bt_txq_count - n);
			bt_txq_count -= n;
			lck_mtx_unlock(&bt_lock);
			if (n >= 5 && packet[0] == 1 && packet[1] == 0xe2 && packet[2] == 0xfc) {
				bt_vsc_sub = packet[4];
			}
			bt_dump("out", packet, n, &bt_said_packets_out);
			connsys_btif_wake();
			connsys_stp_send(packet, n);
		}
		{
			/* bytes written but not a whole H4 packet for 2 s: said once, the first 24 of them (a packet this driver cannot frame) */
			static unsigned waited;
			static bool said_waiting;
			unsigned pending;

			lck_mtx_lock(&bt_lock);
			pending = bt_txq_count;
			lck_mtx_unlock(&bt_lock);
			if (pending == 0) {
				waited = 0;
			} else if (++waited == 1000 && !said_waiting) {
				char text[3 * 24 + 1];
				unsigned n = pending < 24 ? pending : 24;

				said_waiting = true;
				for (unsigned i = 0; i < n; i++) {
					snprintf(text + 3 * i, 4, " %02x", bt_txq[i]);
				}
				text[3 * n] = 0;
				redwood_host_keep_add("connsys: bt: %u bytes written and not a whole H4 packet in 2 s:%s", pending, text);
			}
		}
		heard[0] = 0;
		length = connsys_stp_receive(bt_frame, sizeof(bt_frame), heard, sizeof(heard), 2);
		if (length > 0) {
			length = (int)bt_quirks(bt_frame, (unsigned)length);
			bt_dump("in ", bt_frame, (unsigned)length, &bt_said_packets_in);
			lck_mtx_lock(&bt_lock);
			for (int i = 0; i < length && bt_ring_count < BT_RING; i++) {
				bt_ring[(bt_ring_head + bt_ring_count) % BT_RING] = bt_frame[i];
				bt_ring_count++;
			}
			wakeup(&bt_ring_count);
			selwakeup(&bt_sel);
			lck_mtx_unlock(&bt_lock);
			bt_ctl_deliver(bt_frame, (unsigned)length);
		} else if (length < 0) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
}

/*
 * Bluetooth, first step (boot-arg connsys_bt=1, after WMT is up): what mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) does on an SoC
 * (wmt_func_bt_on: wmt_core_func_ctrl_cmd; the PALDO is the VCN33 of step 3) -- WMT_FUNC_CTRL, 01 06 02 00 <type 0 = BT> <on 1>,
 * answered 02 06 01 00 00 -- and then what Android's Bluetooth driver (bt/mt66xx/legacy/stp_chrdev_bt.c) does with the
 * controller: raw HCI packets, H4 type byte first, as STP's payload on its channel 0 (BT_TASK_INDX, stp_exp.h; the HCI
 * event comes back the same way). HCI_Reset and the controller's identity are asked for; each answer is said.
 */
static void
connsys_bt_start(void)
{
	static const uint8_t on[] = { 0x01, 0x06, 0x02, 0x00, 0x00, 0x01 };
	static const uint8_t on_done[] = { 0x02, 0x06, 0x01, 0x00, 0x00 };
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t reset_done[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t version[] = { 0x01, 0x01, 0x10, 0x00 };
	static const uint8_t version_done[] = { 0x04, 0x0e, 0x0c, 0x01, 0x01, 0x10, 0x00 };
	static const uint8_t address[] = { 0x01, 0x09, 0x10, 0x00 };
	static const uint8_t address_done[] = { 0x04, 0x0e, 0x0a, 0x01, 0x09, 0x10, 0x00 };
	static const uint8_t features[] = { 0x01, 0x03, 0x10, 0x00 };
	static const uint8_t features_done[] = { 0x04, 0x0e, 0x0c, 0x01, 0x03, 0x10, 0x00 };
	static const uint8_t buffers[] = { 0x01, 0x05, 0x10, 0x00 };
	static const uint8_t buffers_done[] = { 0x04, 0x0e, 0x0b, 0x01, 0x05, 0x10, 0x00 };
	static const uint8_t le_buffers[] = { 0x01, 0x02, 0x20, 0x00 };
	static const uint8_t le_buffers_done[] = { 0x04, 0x0e, 0x07, 0x01, 0x02, 0x20, 0x00 };
	uint32_t want = 0;
	unsigned good = 0;

	if (!PE_parse_boot_argn("connsys_bt", &want, sizeof(want)) || want == 0) {
		return;
	}
	connsys_doing = 20;
	connsys_stp_task = 4;
	connsys_skip_foreign = true;
	good = connsys_wmt("BT on (WMT_FUNC_CTRL)", on, sizeof(on), on_done, sizeof(on_done));
	connsys_skip_foreign = false;
	if (!good) {
		redwood_host_keep_add("connsys: step 20 [%u ms]: Bluetooth's function did not come on", connsys_ms());
		connsys_firmware_said("bt on failed");
		return;
	}
	good = 0;
	delay_for_interval(50, NSEC_PER_MSEC);
	connsys_stp_task = 0;
	good += connsys_wmt("HCI_Reset", reset, sizeof(reset), reset_done, sizeof(reset_done));
	good += connsys_wmt("HCI Read_Local_Version_Information", version, sizeof(version), version_done, sizeof(version_done));
	good += connsys_wmt("HCI Read_BD_ADDR", address, sizeof(address), address_done, sizeof(address_done));
	good += connsys_wmt("HCI Read_Local_Supported_Features", features, sizeof(features), features_done, sizeof(features_done));
	good += connsys_wmt("HCI Read_Buffer_Size", buffers, sizeof(buffers), buffers_done, sizeof(buffers_done));
	good += connsys_wmt("HCI LE_Read_Buffer_Size", le_buffers, sizeof(le_buffers), le_buffers_done, sizeof(le_buffers_done));
	redwood_host_keep_add("connsys: step 20 [%u ms]: Bluetooth on; %u of 6 HCI commands answered as a controller answers", connsys_ms(), good);
	connsys_firmware_said("after bt");
	if (want >= 2 && good == 6) {
		/* iOS looks for the controller in the registry ("bluetooth"): without it BluetoothManager's attach fails with 9 and Settings' switch stays grey */
		for (unsigned tries = 0; tries < 40 && !RedwoodBluetoothNode(); tries++) {
			delay_for_interval(250, NSEC_PER_MSEC);
		}
		bt_bridge();
	}
	connsys_stp_task = 4;
}

static void
connsys_stp_full_mode(void)
{
	static const uint8_t query[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	static const uint8_t query_default[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00 };
	static const uint8_t set[] = { 0x01, 0x04, 0x05, 0x00, 0x03, 0xDB, 0x0E, 0x68, 0x01 };
	static const uint8_t set_done[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
	static const uint8_t query_full[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xDB, 0x0E, 0x68, 0x01 };

	static const uint8_t chip_id[] = {
		0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01, 0x08, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	static const uint8_t chip_id_read[] = { 0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x80 };

	/*
	 * Can CONNSYS's CPU read its EMI (0xf0000000 = EMI+0 through the remap of step 5)? The RAM image it jumps to after
	 * the patch's WMT reset is there; runs 371-375 sent the patch whole and the reset went unanswered, the MCU staying
	 * at the ROM's 0xb04. Read with the same WMT_SET_REG_CMD as the chip ID: soc1_0_ram_mcu_1a_1_hdr.bin begins
	 * (byte 48) 46 1f 00 05, so the answer should end 46 1f 00 05.
	 */
	static const uint8_t emi_read[] = {
		0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	static const uint8_t emi_read_done[] = { 0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x46, 0x1f, 0x00, 0x05 };

	connsys_chip_id_done = false;
	if (!connsys_wmt("WMT read 0x80000008", chip_id, sizeof(chip_id), chip_id_read, sizeof(chip_id_read))) {
		return;
	}
	connsys_chip_id_done = true;
	delay_for_interval(10, NSEC_PER_MSEC);
	if (connsys_step >= 9) {
		connsys_wmt("WMT read 0xf0000000 (EMI+0)", emi_read, sizeof(emi_read), emi_read_done, sizeof(emi_read_done));
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	/*
	 * (Runs 426, 427: CONNSYS's own writes by WMT_SET_REG, read by the AP -- EMI+0x60000 and +0x1f0000 written,
	 * +0x300000 not: it writes its memory in the first 2 MB, so no EMI MPU stands between it and the patch.)
	 */

	/* (boot-arg connsys_lpbk=N, WMT loopbacks before the patch, was here until run 431: by PIO 0 of 20 came back
	 * whole, run 392; by AP DMA 20 of 20, runs 398 and 425) */
	if (connsys_step < 8 || (connsys_step >= 9 && !connsys_pda_patch())) {
		return;
	}
	connsys_doing = 8;
	if (!connsys_wmt("STP defaults", query, sizeof(query), query_default, sizeof(query_default)) ||
	    !connsys_wmt("STP set", set, sizeof(set), set_done, sizeof(set_done))) {
		return;
	}
	connsys_stp_full = true;
	connsys_stp_seq = 0;
	connsys_stp_ack = 7;                    /* (none taken yet) */
	connsys_stp_expect = 0;
	delay(10000);
	if (connsys_wmt("full mode, STP asked again", query, sizeof(query), query_full, sizeof(query_full)) && connsys_step >= 11) {
		connsys_sw_init();
	}
}

/*
 * CONNSYS's memory zeroed, as Android has it before the first power-on (mtk_wmt_probe, "Clearing Connsys EMI ...
 * 4194304 bytes", platform/mtk_wcn_consys_hw.c) and its control and paged-dump areas again at every power-on
 * (consys_reset_emi_coredump: EMI+0x68000-0x68400, +0x70400-0x78400). Ours, at 0x51000000, holds whatever the RAM
 * held before -- and that is where CONNSYS's firmware keeps its state, its assert flags and its print buffer
 * (EXP_APMEM_CTRL_*, platform/mt6768.c), and its data. Run 398: the patch whole (DMA), then the MCU's pc 0, as in
 * reset, and the WMT reset unanswered. Through windows the kernel can take back (512 KB at a time: the 8 MB I/O map
 * did not have 4 MB, run 308), with 64-bit stores (device memory: no DC ZVA). How many words of the control area
 * were not zero is said, to know whether that is what it was.
 */
static void
connsys_emi_clear(void)
{
	const uint32_t chunk = 512u * 1024u;
	const uint32_t began = connsys_ms();
	unsigned dirty = 0;

	for (uint32_t at = 0; at < CONNSYS_EMI_SIZE; at += chunk) {
		vm_offset_t window = ml_io_map_unmappable(CONNSYS_EMI_PHYS + at, chunk, 0x7 /* VM_WIMG_IO */);
		volatile uint64_t *words = (volatile uint64_t *)window;

		if (window == 0) {
			redwood_host_keep_add("connsys: step 5 [%u ms]: EMI+%#x could not be mapped to clear it", connsys_ms(), at);
			return;
		}
		if (at <= 0x68000u && 0x68400u <= at + chunk) {
			for (uint32_t i = (0x68000u - at) / 8; i < (0x68400u - at) / 8; i++) {
				dirty += words[i] != 0;
			}
		}
		for (uint32_t i = 0; i < chunk / 8; i++) {
			words[i] = 0;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		ml_io_unmap(window, chunk);
	}
	redwood_host_keep_add("connsys: step 5 [%u ms]: EMI cleared, 4 MB in %u ms (the control area EMI+0x68000 had %u of 128 "
	    "words not zero)", connsys_ms(), connsys_ms() - began, dirty);
}

/*
 * The firmware's dedicated log path, as Android sets it up before the first power-on (consys_dedicated_log_path_init ->
 * connsys_dedicated_log_path_apsoc_init, common/debug_utility/connsys_debug_utility.c, with mt6768.c's
 * connsys_fw_log_parameter: EMI+0x36500, 192 KB -- MCU 16 KB, Wi-Fi and BT 64 KB, GPS 32 KB): the area zeroed (done by
 * connsys_emi_clear), at its start each ring's offset and size (connlog_set_ring_buffer_base_addr: base 64, then each
 * ring after the last + 32 reserved + 32 for alignment) and at +56 the ready pattern "EMIFWLOG" (connlog_set_ring_ready).
 * Ours had zeros there through run 433, and the firmware dies in an interrupt handler as soon as it runs from EMI.
 * Boot-arg connsys_log=0: not written.
 */
static void
connsys_log_ring(void)
{
	static const uint32_t header[8] = {
		0x40, 16 * 1024,                                        /* MCU: base 64 */
		0x40 + 16 * 1024 + 64, 64 * 1024,                       /* Wi-Fi: 0x4080 */
		0x40 + 16 * 1024 + 64 + 64 * 1024 + 64, 64 * 1024,      /* BT: 0x140c0 */
		0x40 + 16 * 1024 + 64 + 2 * (64 * 1024 + 64), 32 * 1024, /* GPS: 0x24100 */
	};
	uint32_t wanted = 1;
	vm_offset_t log;

	(void)PE_parse_boot_argn("connsys_log", &wanted, sizeof(wanted));
	/* (the kernel's pages are 16 KB: a window and its release on 16 KB, or ml_io_unmap writes half a page's PTEs and
	 * pmap panics -- run 434, "write_pte_fast: PTE write is unaligned", a 4 KB window at EMI+0x36000) */
	if (wanted == 0 || (log = ml_io_map_unmappable(CONNSYS_EMI_PHYS + 0x34000u, 0x4000, 0x7 /* VM_WIMG_IO */)) == 0) {
		return;
	}
	log += 0x2000;          /* EMI+0x36000 */
	for (unsigned i = 0; i < 8; i++) {
		*(volatile uint32_t *)(log + 0x500 + 4 * i) = header[i];
	}
	*(volatile uint32_t *)(log + 0x500 + 56) = 0x46494d45u;    /* "EMIF" */
	*(volatile uint32_t *)(log + 0x500 + 60) = 0x474f4c57u;    /* "WLOG" */
	__asm__ volatile ("dsb sy" ::: "memory");
	redwood_host_keep_add("connsys: step 5 [%u ms]: log rings at EMI+0x36500: %#x+%#x %#x+%#x %#x+%#x %#x+%#x, ready %.8s", connsys_ms(),
	    rd(log, 0x500), rd(log, 0x504), rd(log, 0x508), rd(log, 0x50c), rd(log, 0x510), rd(log, 0x514), rd(log, 0x518),
	    rd(log, 0x51c), (const char *)(log + 0x500 + 56));
	ml_io_unmap(log - 0x2000, 0x4000);
}

static void
connsys_firmware_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const char *const files[] = {
		"soc1_0_ram_mcu_1a_1_hdr.bin", "soc1_0_patch_mcu_1a_1_hdr.bin", "soc1_0_ram_bt_1a_1_hdr.bin",
		"soc1_0_ram_wifi_1a_1_hdr.bin",
	};
	vm_offset_t emi, infra = ml_io_map(0x10001000u, 0x1000), rgu = ml_io_map(0x10007000u, 0x1000);
	vm_offset_t mcu = ml_io_map(MCU_PHYS, 0x1000), hif = ml_io_map(0x18007000u, 0x1000);
	uint32_t version = 0, copied = 0;

	connsys_hif = hif;
	connsys_mcu = mcu;
	connsys_rgu = rgu;

	for (unsigned s = 0; s < 240 && rootvnode == NULLVP; s++) {
		delay_for_interval(1, NSEC_PER_SEC);
	}
	delay_for_interval(5, NSEC_PER_SEC);
	/* boot-arg connsys_clear=0: CONNSYS's memory left as it was found, as until run 398 */
	{
		uint32_t clear = 1;

		(void)PE_parse_boot_argn("connsys_clear", &clear, sizeof(clear));
		if (clear != 0) {
			connsys_emi_clear();
		}
	}
	connsys_log_ring();
	/* (each file's own window: 4 MB at once was more than the kernel's I/O map had, run 308) */
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		uint32_t size = 0, address, offset;
		uint8_t *file;

		/* from step 9 the MCU's patch goes over BTIF (connsys_pda_patch), not into EMI: Android puts only the RAM
		 * images there ("[Rom Patch]" ram_bt, ram_wifi, ram_mcu in its log) */
		if (connsys_step >= 9 && strcmp(files[i], "soc1_0_patch_mcu_1a_1_hdr.bin") == 0) {
			continue;
		}
		file = connsys_file(files[i], &size);

		if (file == NULL) {
			redwood_host_keep_add("connsys: step 5 [%u ms]: /fw/connsys/%s: not read", connsys_ms(), files[i]);
			thread_terminate(current_thread());
			return;
		}
		memcpy(&address, file + 0x18, 4);
		offset = address & 0x00ffff00u;
		if (offset + size - 48 > CONNSYS_EMI_SIZE) {
			redwood_host_keep_add("connsys: step 5 [%u ms]: %s: %u bytes at %#x do not fit", connsys_ms(), files[i], size - 48, offset);
			kfree_data(file, size);
			thread_terminate(current_thread());
			return;
		}
		emi = ml_io_map(CONNSYS_EMI_PHYS + offset, round_page(size - 48));
		for (uint32_t at = 48; at < size; at += 4) {
			uint32_t word = 0;

			memcpy(&word, file + at, size - at >= 4 ? 4 : size - at);
			*(volatile uint32_t *)(emi + (at - 48)) = word;
		}
		/* (not unmapped: ml_io_map's entries are permanent, ml_io_unmap panics on them -- run 310; the windows,
		 * ~900 KB in all, and the sensor hub's 256 KB FIFO fit the I/O map together) */
		redwood_host_keep_add("connsys: step 5 [%u ms]: %s (%.14s): %u bytes at EMI+%#x (its address %#x)", connsys_ms(), files[i], (const char *)file,
		    size - 48, offset, address);
		copied++;
		if (connsys_step >= 9) {
			connsys_build_time(file, strcmp(files[i], "soc1_0_ram_bt_1a_1_hdr.bin") == 0 ? 0x68300u :
			    strcmp(files[i], "soc1_0_ram_wifi_1a_1_hdr.bin") == 0 ? 0x68310u : 0x68320u);
		}
		kfree_data(file, size);
	}
	if (connsys_step >= 9 && connsys_patch_file == NULL) {
		connsys_patch_file = connsys_file("soc1_0_patch_mcu_1a_1_hdr.bin", &connsys_patch_size);
		if (connsys_patch_file != NULL && connsys_patch_size > 28 + 2) {
			redwood_host_keep_add("connsys: step 5 [%u ms]: patch file preloaded (%u bytes)", connsys_ms(), connsys_patch_size);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(infra + 0x380) = (rd(infra, 0x380) & 0xfffff000u) | ((CONNSYS_EMI_PHYS >> 24) & 0xfffu);
	*(volatile uint32_t *)(infra + 0x388) = (rd(infra, 0x388) & 0xf000ffffu) | (((0x10003000u >> 24) & 0xfffu) << 16);
	/*
	 * The radio PLL's setting Android makes just before it lets CONNSYS's CPU go: consys_afe_reg_setting of
	 * platform/mt6768.c (CONSYS_AFE_REG_SETTING 1 in mt6768.h, "update WPLL setting for WPLL issue@LT"),
	 * RG_WBG_PLL_03 at CONSYS_AFE_REG_BASE 0x180B3000 + 0x34, bits 19-20 to 01; called from
	 * mtk_wcn_consys_hw_reg_ctrl after acr_reg_setting (empty here) and before hw_reset_bit_set(DISABLE).
	 * Boot-arg connsys_afe=0 leaves it as it is.
	 */
	{
		uint32_t afe = 1;

		(void)PE_parse_boot_argn("connsys_afe", &afe, sizeof(afe));
		if (afe != 0) {
			vm_offset_t wbg = ml_io_map(0x180B3000u, 0x1000);
			connsys_wbg = wbg;
			uint32_t before = rd(wbg, 0x34);

			*(volatile uint32_t *)(wbg + 0x34) = (before & 0xffe7ffffu) | 0x80000u;
			__asm__ volatile ("dsb sy" ::: "memory");
			redwood_host_keep_add("connsys: step 5 [%u ms]: WBG PLL_03 (0x180b3034) %#x -> %#x", connsys_ms(), before, rd(wbg, 0x34));
		}
	}
	/*
	 * What polling_consys_chipid (platform/mt6768.c) does once the chip ID is read and the RAM images are in, before
	 * the CPU is let go -- none of which was done here until run 430, and the MCU died as soon as the ROM handed over
	 * to the RAM image in EMI after the patch (run 429: last pc 0xf000d73e, then 0):
	 *   - co-clock mode (co_clock_flag=1): MCU_TOP_MISC_ON+0x200 (CONSYS_COCLOCK_STABLE_TIME_BASE 0x180C1200) low half
	 *     0x708 -- "vcore ready stable time", "XO initial stable time", "XO bg stable time" -- and +0x204 bits 0 and
	 *     8-15 to 0x600: "clear 'source clock enable ack to XO state' mask";
	 *   - "EMI control CR setting (hw mode)": MCU+0x150 (CONSYS_SW_IRQ_OFFSET) bit 21 cleared;
	 *   - "toppose_restore_done rollback": MCU_TOP_MISC_ON+0x600 bit 23 set.
	 * Boot-arg connsys_post=0: none of it.
	 */
	{
		uint32_t post = 1;

		(void)PE_parse_boot_argn("connsys_post", &post, sizeof(post));
		if (post != 0) {
			/* (a window taken back after: one more permanent one filled the kernel's 8 MB I/O map, and another driver's
			 * later mapping panicked, "kmem_alloc ... failed with 3" -- runs 430-435; 16 KB aligned) */
			vm_offset_t misc_on_window = ml_io_map_unmappable(0x180C0000u, 0x4000, 0x7 /* VM_WIMG_IO */);
			vm_offset_t misc_on = misc_on_window + 0x1000;
			uint32_t stable = rd(misc_on, 0x200), ack = rd(misc_on, 0x204), emi = rd(mcu, 0x150), toppose = rd(misc_on, 0x600);

			*(volatile uint32_t *)(misc_on + 0x200) = (stable & 0xffff0000u) | 0x708u;
			*(volatile uint32_t *)(misc_on + 0x204) = (ack & 0xffff00feu) | 0x600u;
			*(volatile uint32_t *)(mcu + 0x150) = emi & ~(1u << 21);
			*(volatile uint32_t *)(misc_on + 0x600) = (toppose & 0xff7fffffu) | (1u << 23);
			__asm__ volatile ("dsb sy" ::: "memory");
			redwood_host_keep_add("connsys: step 5 [%u ms]: as polling_consys_chipid: XO stable time %#x -> %#x, ack mask %#x -> %#x, "
			    "EMI control (MCU+0x150) %#x -> %#x, toppose restore %#x -> %#x", connsys_ms(), stable, rd(misc_on, 0x200), ack,
			    rd(misc_on, 0x204), emi, rd(mcu, 0x150), toppose, rd(misc_on, 0x600));
			ml_io_unmap(misc_on_window, 0x4000);
		}
	}
	uint32_t ms_rel = connsys_ms();
	*(volatile uint32_t *)(rgu + 0x18) = (rd(rgu, 0x18) & ~(1u << 12)) | (0x88u << 24);     /* the CPU let go */
	for (unsigned n = 0; n < 50 && version != 0x1D1E; n++) {
		delay_for_interval(20, NSEC_PER_MSEC);
		version = rd(mcu, 0x600);
	}
	snprintf(connsys_said, sizeof(connsys_said), "step 5 [%u ms, rel at %u ms]: %u files in; remap %#x/%#x; MCU+0x600 %#x (%s), CPUPCR %#x then %#x",
	    connsys_ms(), ms_rel, copied, rd(infra, 0x380), rd(infra, 0x388), version, version == 0x1D1E ? "CONNSYS'S ROM TOOK IT" : "not 0x1D1E",
	    rd(hif, 0x104), (delay_for_interval(10, NSEC_PER_MSEC), rd(hif, 0x104)));
	redwood_host_keep_add("connsys: %s", connsys_said);
	for (unsigned n = 0; n < 100 && (connsys_hif != 0 && rd(connsys_hif, 0x104) != 0xb04); n++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	redwood_host_keep_add("connsys: step 5 [%u ms]: MCU reached 0xb04 (CPUPCR %#x)", connsys_ms(), connsys_hif != 0 ? rd(connsys_hif, 0x104) : 0);
	if (connsys_step >= 7) {
		delay_for_interval(20, NSEC_PER_MSEC);          /* Android's msleep(20) after the reset bit */
		connsys_btif_init();
		connsys_stp_full_mode();
	} else {
		connsys_btif_listen("step 6: BTIF in a second");
	}
	thread_terminate(current_thread());
}

void
redwood_connsys_init(void)
{
	uint32_t step = 0, ctrl, status, status2;
	vm_offset_t spm, misc, mcu;
	bool on;

	if (!PE_parse_boot_argn("connsys_step", &step, sizeof(step)) || step < 1) {
		return;
	}
	connsys_step = step;
	if (step >= 16) {
		redwood_mtkwifi_net_early();
	}
	spm = ml_io_map(SPM_PHYS, 0x1000);
	ctrl = rd(spm, SPM_TOP1_PWR_CTRL);
	status = rd(spm, SPM_PWR_STATUS);
	status2 = rd(spm, SPM_PWR_STATUS_2ND);
	on = (status & status2 & PWR_STATUS_CONN) && (status & status2 & PWR_STATUS_CONN_TOP2);
	redwood_host_keep_add("connsys: step 1: SPM TOP1_PWR_CTRL %#x (reset %s, iso %s, on %u/%u, clock %u), PWR_STATUS %#x/%#x: "
	    "CONNSYS %s", ctrl, (ctrl & 1) ? "released" : "held", (ctrl & 2) ? "on" : "off", (ctrl >> 2) & 1, (ctrl >> 3) & 1,
	    (ctrl >> 4) & 1, status, status2, on ? "ON" : "off");
	if (step >= 2) {
		connsys_power_report();
	}
	if (step >= 3 && !on) {
		connsys_supplies_on();
	}
	if (step >= 4 && !on) {
		/* from step 7 BTIF comes up only once CONNSYS's ROM has (connsys_firmware_thread), as Android's btif_open
		 * after CONSYS-HW-PWR-ON: up before the power-on, its line was there while the ROM started, and the first
		 * command after was lost (runs 357-372, all but 356, 366, 371) */
		if (step == 6) {
			connsys_btif_init();
		}
		connsys_power_on(spm);
		if (step >= 5 && (rd(spm, SPM_PWR_STATUS) & 2)) {
			thread_t thread;

			if (kernel_thread_start_priority(connsys_firmware_thread, NULL, 95 /* MAXPRI_KERNEL */, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
		}
		return;
	}
	if (!on) {
		redwood_host_keep_add("connsys: step 1: powered off, so its own registers are not read (as expected straight from LK)");
		return;
	}
	misc = ml_io_map(MCU_TOP_MISC_OFF_PHYS, 0x1000);
	mcu = ml_io_map(MCU_PHYS, 0x1000);
	redwood_host_keep_add("connsys: step 1: IP version %#x (%s), configuration %#x, hardware %#x, firmware %#x",
	    rd(misc, CONSYS_IP_VER), rd(misc, CONSYS_IP_VER) == CONSYS_IP_VER_ID ? "the MT6768's" : "NOT the expected 0x10020501",
	    rd(misc, CONSYS_CONF_ID) & 0xF, rd(mcu, CONSYS_HW_ID) & 0xFFFF, rd(mcu, CONSYS_FW_ID) & 0xFFFF);
}
