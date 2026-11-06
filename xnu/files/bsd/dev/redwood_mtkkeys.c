/*
 * The buttons of the Infinix Hot 20 (X6826B), as a source of button presses for
 * /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg buttons=2: every address below
 * belongs to this phone; buttons=1 is the other phone's, in redwood_buttons.c.
 *
 * Android's own device tree says where they are (the kp@10010000 node):
 *   volume up    the SoC's keypad controller, key 0 of its map ("kpd-hw-init-map" 0x73)
 *   volume down  the same, key 1 (0x72)
 *   power        the PMIC, an MT6358, reached through the PMIC wrapper: bit 1 of TOPSTATUS
 * The keypad keeps a bit per key in KP_MEM1..5, clear while the key is down; the PMIC's bit is the
 * same way round (Linux drivers/input/keyboard/mtk-pmic-keys.c). Nothing interrupts: a thread
 * looks every 20 ms, and a key has to read the same twice in a row before it counts.
 *
 * Boot-arg keylog=1: a line for every change, to check the wiring against.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define KPD_PHYS        0x10010000
#define KP_MEM1         0x0004          /* keys 0..15, a bit clear while the key is down */

#define PWRAP_PHYS      0x1000d000
#define PWRAP_WACS2_CMD     0xc20
#define PWRAP_WACS2_RDATA   0xc24
#define PWRAP_WACS2_VLDCLR  0xc28
#define PWRAP_FSM(r)        (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE      0
#define PWRAP_FSM_WFVLDCLR  6

#define MT6358_TOPSTATUS    0x28
#define PWRKEY_DEB          (1u << 1)   /* clear while the power key is down */

static vm_offset_t kpd, pwrap;

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
}

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 100000; spin++) {
		if (PWRAP_FSM(rd(pwrap, PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

/* One PMIC register through the wrapper's own channel: idle, command, wait for the answer, take it. */
static bool
pmic_read(uint32_t reg, uint32_t *value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return false;
	}
	wr(pwrap, PWRAP_WACS2_CMD, (reg >> 1) << 16);
	if (!pwrap_wait(PWRAP_FSM_WFVLDCLR)) {
		return false;
	}
	*value = rd(pwrap, PWRAP_WACS2_RDATA) & 0xffff;
	wr(pwrap, PWRAP_WACS2_VLDCLR, 1);
	return true;
}

/* The same channel the other way: a PMIC register written. */
static bool
pmic_write(uint32_t reg, uint32_t value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return false;
	}
	wr(pwrap, PWRAP_WACS2_CMD, (1u << 31) | (reg >> 1) << 16 | (value & 0xffff));
	return pwrap_wait(PWRAP_FSM_IDLE);
}

/*
 * The processors' clock (boot-arg mtk_cpufreq=1: what it is, said; mtk_cpufreq=2: raised -- not cpufreq=, which is the POCO's (redwood_cpufreq.c) and writes where this SoC has nothing of the kind). The boot loader
 * leaves both clusters at 1175 MHz and 0.8 V, and nothing under this kernel changes that: iOS ran
 * at some 60 in 100 of what the phone can do (MiniIOMobileFramebuffer's measurement, 24 October).
 * Android's driver for this SoC (drivers/misc/mediatek/base/power/cpufreq_v1, mach/mt6768) has the
 * table: the Cortex-A55s (ARMPLL1, Vproc12) to 1700 MHz at 0.9625 V, the Cortex-A75s (ARMPLL2,
 * Vproc11) to 2000 MHz at 1.0875 V, from the MT6358 PMIC's bucks, 0.5 V + 6.25 mV a step.
 * The order is its own: the voltage up first (VSRAM before Vproc, so that it never falls under
 * it), then the PLL -- its new DDS with the CHG bit, then the post divider. Nothing is touched
 * unless every register reads what the boot loader is known to leave there.
 */
#define APMIXED_PHYS            0x1000c000
#define ARMPLL_LL_CON1          0x21c   /* ARMPLL1: the A55s */
#define ARMPLL_L_CON1           0x20c   /* ARMPLL2: the A75s */
#define MT6358_VPROC11_DBG0     0x139e  /* what the buck is set to (7 bits) */
#define MT6358_VPROC11_ELR0     0x13a6  /* where it is set */
#define MT6358_VPROC12_DBG0     0x141e
#define MT6358_VPROC12_ELR0     0x1426
#define MT6358_VSRAM11_TRACK    0x1b64
#define MT6358_VSRAM12_TRACK    0x1b6c
#define MT6358_VSRAM11_DBG0     0x1b60  /* bits 14:8 */
#define MT6358_VSRAM12_DBG0     0x1ba2
#define MT6358_VSRAM_CON0       0x1bee  /* VSRAM_PROC11's setting */
#define MT6358_VSRAM_CON1       0x1bf0  /* VSRAM_PROC12's */
#define VOLT_STEP_MAX           99      /* 1.11875 V: Android's MAX_VPROC_VOLT, 1.12 V */

extern void redwood_keep(const char *format, ...) __printflike(1, 2);

static uint32_t
pll_mhz(uint32_t con1)
{
	uint32_t posdiv = 1u << ((con1 >> 24) & 7);

	return (uint32_t)((uint64_t)(con1 & 0x3fffff) * 26 / (1u << 14) / posdiv);
}

static uint32_t
pll_dds(uint32_t vco_mhz)
{
	return ((vco_mhz << 14) / 26) & 0x3fffff;
}

static bool
cluster_raise(vm_offset_t apmixed, const char *name, uint32_t con1_at, uint32_t elr, uint32_t dbg,
    uint32_t vsram_con, uint32_t vsram_dbg, uint32_t track, uint32_t mhz, uint32_t posdiv_sel, uint32_t step)
{
	uint32_t con1 = rd(apmixed, con1_at), vproc = 0, vsram = 0, tracking = 0, elr_was = 0;

	if (!pmic_read(dbg, &vproc) || !pmic_read(vsram_dbg, &vsram) || !pmic_read(track, &tracking) ||
	    !pmic_read(elr, &elr_was)) {
		redwood_keep("cpufreq: %s: the PMIC does not answer; left as it is", name);
		return false;
	}
	vproc &= 0x7f;
	vsram = vsram >> 8 & 0x7f;
	redwood_keep("cpufreq: %s at %u MHz (CON1 %#x), Vproc step %u (%u mV), VSRAM step %u (%u mV), tracking %#x",
	    name, pll_mhz(con1), con1, vproc, 500 + vproc * 25 / 4, vsram, 500 + vsram * 25 / 4, tracking);
	if (step == 0) {
		return true;
	}
	/* what the boot loader leaves: 1175 MHz from a VCO of 2350 halved, 0.8 V */
	if ((con1 >> 24 & 7) != 1 || pll_mhz(con1) < 1150 || pll_mhz(con1) > 1200 || vproc < 44 || vproc > 52) {
		redwood_keep("cpufreq: %s is not as the boot loader leaves it; left as it is", name);
		return false;
	}
	uint32_t want_sram = step + 16 > VOLT_STEP_MAX ? VOLT_STEP_MAX : step + 16;

	if (!(tracking & 1)) {
		uint32_t con = 0;

		if (!pmic_read(vsram_con, &con) || !pmic_write(vsram_con, (con & ~0x7fu) | want_sram)) {
			redwood_keep("cpufreq: %s: VSRAM could not be raised; left as it is", name);
			return false;
		}
	}
	if (!pmic_write(elr, (elr_was & ~0x7fu) | step)) {
		redwood_keep("cpufreq: %s: Vproc could not be raised; left as it is", name);
		return false;
	}
	delay(1000);    /* 10 mV a microsecond up, and the PMIC's own delay: far more than enough */
	if (!pmic_read(dbg, &vproc) || (vproc & 0x7f) != step) {
		redwood_keep("cpufreq: %s: Vproc reads step %u, not %u; the clock left as it is", name, vproc & 0x7f, step);
		return false;
	}
	/* the VCO for the new clock under the old post divider, then the divider */
	uint32_t posdiv = 1u << posdiv_sel;

	wr(apmixed, con1_at, (rd(apmixed, con1_at) & ~0x3fffffu) | pll_dds(mhz * posdiv) | (1u << 31));
	delay(20);
	wr(apmixed, con1_at, (rd(apmixed, con1_at) & ~(7u << 24)) | posdiv_sel << 24);
	delay(1);
	con1 = rd(apmixed, con1_at);
	(void)pmic_read(vsram_dbg, &vsram);
	redwood_keep("cpufreq: %s now at %u MHz (CON1 %#x), Vproc step %u, VSRAM step %u", name, pll_mhz(con1), con1,
	    step, vsram >> 8 & 0x7f);
	return true;
}

/*
 * The GPU's clock (boot-arg mali_mhz=N, 375 to 950): the Mali-G52 runs from syspll_d3, 364 MHz, as
 * redwood_mali.c's bring-up leaves it -- a third of what MediaTek's table has it do. As Android's
 * driver (drivers/gpu/mediatek/gpufreq/mt6768): VSRAM_GPU then VGPU up (the two 100 to 250 mV
 * apart), the mux parked on syspll_d3, GPUPLL's DDS and post divider (/4 for 375 to 950 MHz, a VCO
 * of 1.5 to 3.8 GHz), the mux to the PLL.
 */
#define GPUPLL_CON0             0x248
#define GPUPLL_CON1             0x24c
#define GPUPLL_PWR_CON0         0x254
#define TOPCKGEN_PHYS           0x10000000
#define CLK_CFG_1               0x50            /* mfg_sel in bits 1:0: 1 GPUPLL, 2 syspll_d3 */
#define MT6358_VGPU_DBG0        0x151e
#define MT6358_VGPU_ELR0        0x1526
#define MT6358_VSRAM_GPU_DBG0   0x1be2          /* bits 14:8 */
#define MT6358_VSRAM_CON3       0x1bf4          /* VSRAM_GPU's setting */

extern bool redwood_mali_ready(void);

static void
gpu_raise(uint32_t mhz)
{
	vm_offset_t apmixed = ml_io_map(APMIXED_PHYS, 0x1000), topckgen = ml_io_map(TOPCKGEN_PHYS, 0x1000);
	uint32_t vgpu = 0, vsram = 0, elr = 0, con = 0;

	for (unsigned i = 0; i < 600 && !redwood_mali_ready(); i++) {
		delay(100000);                  /* the GPU brought up first (redwood_mali.c) */
	}
	if (!pmic_read(MT6358_VGPU_DBG0, &vgpu) || !pmic_read(MT6358_VSRAM_GPU_DBG0, &vsram) ||
	    !pmic_read(MT6358_VGPU_ELR0, &elr) || !pmic_read(MT6358_VSRAM_CON3, &con)) {
		redwood_keep("gpufreq: the PMIC does not answer; left as it is");
		return;
	}
	vgpu &= 0x7f;
	vsram = vsram >> 8 & 0x7f;
	uint32_t mux = rd(topckgen, CLK_CFG_1) & 3;

	redwood_keep("gpufreq: mux %u, GPUPLL CON0 %#x CON1 %#x PWR %#x, VGPU step %u (%u mV), VSRAM_GPU step %u (%u mV)",
	    mux, rd(apmixed, GPUPLL_CON0), rd(apmixed, GPUPLL_CON1), rd(apmixed, GPUPLL_PWR_CON0), vgpu,
	    500 + vgpu * 25 / 4, vsram, 500 + vsram * 25 / 4);
	if (mhz < 375 || mhz > 950 || mux != 2 || vgpu < 8 || vgpu > 56 || !redwood_mali_ready()) {
		redwood_keep("gpufreq: not as redwood_mali.c leaves it, or no clock asked for; left as it is");
		return;
	}
	/* MediaTek's voltages, two steps over: 0.78 V at 796 MHz, 0.90 at 950 */
	uint32_t want_mv = mhz >= 900 ? 913 : mhz >= 800 ? 800 : mhz >= 700 ? 760 : 725;
	uint32_t want = (want_mv - 500) * 4 / 25, want_sram = want + 16;   /* VSRAM 100 mV over */

	if (want_sram < 56) {
		want_sram = 56;                 /* 0.85 V at least, MediaTek's floor */
	}
	if (!pmic_write(MT6358_VSRAM_CON3, (con & ~0x7fu) | want_sram) || !pmic_write(MT6358_VGPU_ELR0, (elr & ~0x7fu) | want)) {
		redwood_keep("gpufreq: the voltages could not be set; left as it is");
		return;
	}
	delay(1000);
	(void)pmic_read(MT6358_VGPU_DBG0, &vgpu);
	if ((vgpu & 0x7f) != want) {
		redwood_keep("gpufreq: VGPU reads step %u, not %u; the clock left as it is", vgpu & 0x7f, want);
		return;
	}
	/* the PLL: on (as Linux's clk-mtk does), its frequency, then the mux to it */
	if (!(rd(apmixed, GPUPLL_PWR_CON0) & 1)) {
		wr(apmixed, GPUPLL_PWR_CON0, rd(apmixed, GPUPLL_PWR_CON0) | 1);
		delay(1);
		wr(apmixed, GPUPLL_PWR_CON0, rd(apmixed, GPUPLL_PWR_CON0) & ~2u);
		delay(1);
	}
	uint32_t dds = ((((mhz * 10) * 4) << 14) / 26 + 5) / 10;

	wr(apmixed, GPUPLL_CON1, 0x80000000u | (2u << 24) | (dds & 0x3fffff));
	if (!(rd(apmixed, GPUPLL_CON0) & 1)) {
		wr(apmixed, GPUPLL_CON0, rd(apmixed, GPUPLL_CON0) | 1);
	}
	delay(20);
	wr(topckgen, CLK_CFG_1 + 8, 3u);        /* CLR the mux field */
	wr(topckgen, CLK_CFG_1 + 4, 1u);        /* SET: GPUPLL */
	wr(topckgen, 0x004, 1u << 4);           /* CLK_CFG_UPDATE: mfg_sel's bit (clk-mt6768.c) */
	(void)pmic_read(MT6358_VSRAM_GPU_DBG0, &vsram);
	redwood_keep("gpufreq: the GPU at %u MHz (CON1 %#x, mux %u), VGPU step %u, VSRAM_GPU step %u", mhz,
	    rd(apmixed, GPUPLL_CON1), rd(topckgen, CLK_CFG_1) & 3, want, vsram >> 8 & 0x7f);
}

static void
cpufreq_set(uint32_t how)
{
	vm_offset_t apmixed = ml_io_map(APMIXED_PHYS, 0x1000);

	/* the A75s first: the window server and SpringBoard run there (core_order) */
	if (cluster_raise(apmixed, "A75s", ARMPLL_L_CON1, MT6358_VPROC11_ELR0, MT6358_VPROC11_DBG0, MT6358_VSRAM_CON0,
	    MT6358_VSRAM11_DBG0, MT6358_VSRAM11_TRACK, 2000, 0, how >= 2 ? 94 + 2 : 0)) {
		(void)cluster_raise(apmixed, "A55s", ARMPLL_LL_CON1, MT6358_VPROC12_ELR0, MT6358_VPROC12_DBG0,
		    MT6358_VSRAM_CON1, MT6358_VSRAM12_DBG0, MT6358_VSRAM12_TRACK, 1700, 1, how >= 3 ? 74 + 2 : 0);
	}
}

struct key {
	const char *name;
	uint32_t button;                /* REDWOOD_BUTTON_* */
	bool down, seen;                /* what was reported, and what the last look found */
};

static struct key keys[3] = {
	{ "volume up", REDWOOD_BUTTON_VOLUME_UP, false, false },
	{ "volume down", REDWOOD_BUTTON_VOLUME_DOWN, false, false },
	{ "power", REDWOOD_BUTTON_LOCK, false, false },
};

static void
keys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t log = 0, presses = 0;

	PE_parse_boot_argn("keylog", &log, sizeof(log));
	kpd = ml_io_map(KPD_PHYS, 0x1000);
	pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	printf("mtkkeys: the buttons are looked at every 20 ms; keypad %#x, PMIC status %s\n",
	    rd(kpd, KP_MEM1), pwrap_wait(PWRAP_FSM_IDLE) ? "reachable" : "NOT REACHABLE");
	{
		uint32_t how = 0;

		if (PE_parse_boot_argn("mtk_cpufreq", &how, sizeof(how)) && how != 0) {
			cpufreq_set(how);
		}
		if (PE_parse_boot_argn("mali_mhz", &how, sizeof(how)) && how != 0) {
			gpu_raise(how);
		}
	}

	for (unsigned tick = 1;; tick++) {
		uint32_t mem = rd(kpd, KP_MEM1), top = PWRKEY_DEB;
		bool now[3];

		(void)pmic_read(MT6358_TOPSTATUS, &top);
		now[0] = !(mem & (1u << 0));
		now[1] = !(mem & (1u << 1));
		now[2] = !(top & PWRKEY_DEB);

		for (unsigned i = 0; i < 3; i++) {
			struct key *k = &keys[i];

			/* the same twice in a row, or it is a bounce */
			if (now[i] == k->seen && now[i] != k->down) {
				k->down = now[i];
				redwood_touch_post(k->down ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, k->button, 0, 0);
				presses += k->down;
				if (log) {
					printf("mtkkeys: %s %s (keypad %#x, PMIC %#x)\n", k->name, k->down ? "down" : "up", mem, top);
				}
			}
			k->seen = now[i];
		}
		if (tick % 3000 == 0) {
			printf("mtkkeys: so far %u presses; keypad %#x, PMIC status %#x\n", presses, mem, top);
		}
		delay(20000);
	}
}

void
redwood_mtkkeys_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("buttons", &which, sizeof(which)) || which != 2) {
		return;
	}
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("mtkkeys: cannot start the thread that reads the buttons\n");
	}
}
