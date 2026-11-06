/*
 * fbdiag: does code that LK boots run, where is the framebuffer the panel shows, and how is
 * OVL0 set up? Packed in front of Android's kernel (tools/chainboot.py). Read-only on the display
 * hardware. Results go on the screen and into the ramoops console; after FBDIAG_SECONDS it
 * starts Android's kernel, and the report is /sys/fs/pstore/console-ramoops-0.
 */
#include <stdint.h>
#include "mtkdbg.h"

#define FBDIAG_SECONDS 5

/*
 * The boot image's kernel is this program followed, at CHAIN_OFFSET, by Android's own kernel
 * Image (tools/chainboot.py). When done, that kernel is moved to where LK would have put it
 * and started, so Android boots as usual and its ramoops keeps this report.
 */
#define CHAIN_OFFSET  0x40000u
#define KERNEL_PA     0x40080000ULL
#define STUB_PA       0x4a000000ULL

extern char _start[];
extern char chain_stub[], chain_stub_end[];
static uint64_t chain_dtb;

static uint64_t sysreg_el(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return (v >> 2) & 3;
}

static void report_sysregs(void)
{
	uint64_t el = sysreg_el(), sctlr, cpacr = 0, mpidr;

	if (el == 2) {
		__asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
	} else {
		__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	}
	__asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	dbg_puts("EL");
	dbg_dec(el);
	dbg_puts(" SCTLR=");
	dbg_hex(sctlr, 8);
	dbg_puts(" CPACR=");
	dbg_hex(cpacr, 8);
	dbg_puts(" MPIDR=");
	dbg_hex(mpidr, 8);
	dbg_puts(" CNTFRQ=");
	dbg_dec(dbg_freq());
	dbg_putc('\n');
}

static void bar(uint32_t y, uint32_t color, const char *label)
{
	fb_rect(0, y, PANEL_W, 48, color);
	uint32_t saved_y = fbc.y, saved_x = fbc.x, saved_bg = fbc.bg, saved_fg = fbc.fg;
	uint64_t saved_base = fbc.base;
	fbc.x = 16;
	fbc.y = y + 8;
	fbc.bg = 0xff000000u;
	fbc.fg = 0xffffffffu;
	while (*label) {
		fbcon_putc(*label++);
	}
	fbc.x = saved_x;
	fbc.y = saved_y;
	fbc.bg = saved_bg;
	fbc.fg = saved_fg;
	fbc.base = saved_base;
}

static void __attribute__((noreturn)) chain_android(void)
{
	const volatile uint32_t *img = (const volatile uint32_t *)((uint64_t)_start + CHAIN_OFFSET);
	uint64_t image_size;

	if (img[14] != 0x644d5241u) { /* "ARM\x64" */
		dbg_puts("no Linux Image to chain to: warm reset\n");
		dbg_delay_ms(3000);
		wdt_reset();
	}
	/* Copy text and data; Linux clears its own BSS. The packer stored the Image's file length
	 * in this program's own header (res2, offset 0x28). */
	const volatile uint32_t *hdr = (const volatile uint32_t *)_start;
	image_size = ((uint64_t)hdr[11] << 32 | hdr[10]);
	image_size = (image_size + 7) & ~7ULL;
	dbg_puts("chaining to Android kernel, ");
	dbg_dec(image_size);
	dbg_puts(" bytes\n");

	volatile uint32_t *dst = (volatile uint32_t *)STUB_PA;
	const volatile uint32_t *src = (const volatile uint32_t *)chain_stub;
	for (uint64_t i = 0; i < (uint64_t)(chain_stub_end - chain_stub) / 4; i++) {
		dst[i] = src[i];
	}
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	void (*stub)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = (void *)STUB_PA;
	stub(chain_dtb, KERNEL_PA, (uint64_t)img, image_size, KERNEL_PA);
	__builtin_unreachable();
}

void fbdiag_fault(uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far)
{
	fbc.fg = 0xffff4040u;
	dbg_puts("\n*** FAULT vec=");
	dbg_hex(vec, 3);
	dbg_puts(" ESR=");
	dbg_hex(esr, 8);
	dbg_puts(" ELR=");
	dbg_hex(elr, 10);
	dbg_puts(" FAR=");
	dbg_hex(far, 10);
	dbg_puts("\n");
	dbg_delay_ms(5000);
	chain_android();
}

/*
 * v2 runs twice. Phase 1 (no marker): writes patterns at the addresses an XNU run would leave
 * its log at, tries clearing OVL0 layer 0's per-pixel alpha, arms the TOPRGU watchdog and
 * waits for it. Phase 2 (marker set, after the watchdog reset): checks the patterns, i.e.
 * whether DRAM survives the reset that will end XNU runs, and then starts Android.
 * The marker is the header word of ramoops' ftrace zone: Android's ramoops rewrites it.
 */
#define MARKER_PA     0x4d0df000ULL
#define MARKER_PHASE2 0x32474946u /* "FIG2" */
#define WDT_SECONDS   10

static const uint64_t probe_pa[] = { 0x42000000ULL, 0x47b1c9c0ULL, 0x80000000ULL, 0xc0000000ULL, 0xc7b1c9c0ULL, 0xd0000000ULL, 0x140000000ULL };
#define PROBE_WORDS 256

static uint32_t pattern(uint64_t pa, uint32_t i)
{
	return (uint32_t)(pa >> 4) ^ (i * 0x9e3779b9u) ^ 0x5a17c0deu;
}

static void phase2(void)
{
	dbg_puts("\n== fbdiag v3 phase 2: after the watchdog reset ==\n");
	report_sysregs();
	dbg_puts("WDT MODE=");
	dbg_hex(rd32(TOPRGU_MODE), 8);
	dbg_puts(" LENGTH=");
	dbg_hex(rd32(TOPRGU_LENGTH), 8);
	dbg_puts(" STATUS=");
	dbg_hex(rd32(TOPRGU_BASE + 0x0c), 8);
	dbg_puts(" NONRST=");
	dbg_hex(rd32(TOPRGU_BASE + 0x20), 8);
	dbg_putc(' ');
	dbg_hex(rd32(TOPRGU_BASE + 0x24), 8);
	dbg_puts("\nmarker=");
	dbg_hex(rd32(MARKER_PA), 8);
	dbg_putc('\n');
	for (unsigned p = 0; p < sizeof(probe_pa) / sizeof(probe_pa[0]); p++) {
		volatile uint32_t *w = (volatile uint32_t *)probe_pa[p];
		uint32_t ok = 0, bad_i = 0, bad_x = 0;
		for (uint32_t i = 0; i < PROBE_WORDS; i++) {
			uint32_t x = w[i] ^ pattern(probe_pa[p], i);
			ok += x == 0;
			if (x && !bad_x) {
				bad_i = i;
				bad_x = x;
			}
		}
		dbg_puts("DRAM ");
		dbg_hex(probe_pa[p], 9);
		dbg_puts(": ");
		dbg_dec(ok);
		dbg_puts("/256 kept, first bad #");
		dbg_dec(bad_i);
		dbg_puts(" xor=");
		dbg_hex(bad_x, 8);
		dbg_putc('\n');
	}
	wr32(MARKER_PA, 0);
	dbg_puts("phase 2 done\n");
}

void fbdiag_main(uint64_t dtb)
{
	/* Phase 2 also when only the reset status says "watchdog": no reset loop if DRAM is lost. */
	uint32_t wdt_status = rd32(TOPRGU_BASE + 0x0c);
	int second = rd32(MARKER_PA) == MARKER_PHASE2 || (wdt_status & 0x80000000u);

	wdt_disable();
	if (!second) {
		plog_reset();
	}
	chain_dtb = dtb;

	/* Where does OVL0 layer 0 read from? Use it only if it is inside LK's framebuffer. */
	uint32_t l0_addr = rd32(OVL0_BASE + OVL_L0_ADDR);
	uint32_t l0_pitch = rd32(OVL0_BASE + OVL_L0_PITCH) & 0xffff;
	uint32_t l0_con = rd32(OVL0_BASE + OVL_L0_CON);
	uint64_t fb = LK_FB_BASE;
	uint32_t pitch = LK_FB_PITCH;
	if (l0_addr >= LK_FB_BASE && l0_addr < LK_FB_BASE + LK_FB_SIZE && l0_pitch >= PANEL_W * 4 && l0_pitch <= 4096) {
		fb = l0_addr;
		pitch = l0_pitch;
	}

	/* Leave the logo's top part; the report goes below a band of test bars. */
	fbcon_init(fb, pitch, 560, 1);
	fb_rect(0, 0, PANEL_W, 560, 0xff000000u);
	bar(0, 0xffff0000u, "A: red   alpha=ff");
	bar(56, 0xff00ff00u, "B: green alpha=ff");
	bar(112, 0xff0000ffu, "C: blue  alpha=ff");
	bar(168, 0x00ffffffu, "D: white alpha=00 (invisible if per-pixel alpha)");
	bar(224, 0x80ffffffu, "E: white alpha=80");
	bar(280, 0xffffffffu, "F: white alpha=ff");
	fbc.scale = 2;
	fbc.y = 360;
	fbc.x = 0;
	fbc.fg = 0xff40ff40u;
	dbg_puts(second ? "fbdiag: phase 2\n" : "fbdiag: phase 1\n");
	fbc.scale = 1;
	fbc.y = 560;
	fbc.x = 0;
	fbc.fg = 0xffffffffu;

	if (second) {
		phase2();
		for (int s = FBDIAG_SECONDS; s > 0; s--) {
			dbg_puts("Android in ");
			dbg_dec(s);
			dbg_puts(" s\n");
			dbg_delay_ms(1000);
		}
		chain_android();
	}

	dbg_puts("== fbdiag v3 phase 1 (X6826B MT6768) ==\nx0(dtb)=");
	dbg_hex(dtb, 10);
	dbg_putc('\n');
	report_sysregs();
	dbg_puts("OVL0 L0_ADDR=");
	dbg_hex(l0_addr, 8);
	dbg_puts(" L0_PITCH=");
	dbg_hex(l0_pitch, 4);
	dbg_puts(" L0_CON=");
	dbg_hex(l0_con, 8);
	dbg_puts("\n");
	dbg_dump("DAL  ", 0x78c5c000ULL, 0, 0x20, 0);
	dbg_dump("DAL  ", 0x78c5c000ULL + 800 * 2880, 0, 0x20, 0);

	/* Per-pixel alpha off on layer 0: constant alpha 0xff stays. Bar D shows if it takes. */
	wr32(OVL0_BASE + OVL_L0_CON, l0_con & ~(1u << 8));
	dbg_delay_ms(100);
	dbg_puts("L0_CON after clearing AEN: ");
	dbg_hex(rd32(OVL0_BASE + OVL_L0_CON), 8);
	dbg_putc('\n');

	for (unsigned p = 0; p < sizeof(probe_pa) / sizeof(probe_pa[0]); p++) {
		volatile uint32_t *w = (volatile uint32_t *)probe_pa[p];
		for (uint32_t i = 0; i < PROBE_WORDS; i++) {
			w[i] = pattern(probe_pa[p], i);
		}
	}
	dbg_puts("patterns written\nWDT before: STATUS=");
	dbg_hex(wdt_status, 8);
	dbg_puts(" NONRST=");
	dbg_hex(rd32(TOPRGU_BASE + 0x20), 8);
	dbg_putc(' ');
	dbg_hex(rd32(TOPRGU_BASE + 0x24), 8);
	dbg_puts(" MODE=");
	dbg_hex(rd32(TOPRGU_MODE), 8);
	dbg_puts(" LENGTH=");
	dbg_hex(rd32(TOPRGU_LENGTH), 8);
	dbg_putc('\n');

	/* LENGTH: timeout in units of 512 ticks of 32 kHz in bits 15:5, key 0x08. MODE: key, EN. */
	wr32(MARKER_PA, MARKER_PHASE2);
	wr32(TOPRGU_LENGTH, ((WDT_SECONDS * 32768u / 512u) << 5) | 0x8u);
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
	wr32(TOPRGU_MODE, TOPRGU_MODE_KEY | 0x80u | 0x1u); /* DDR_RESERVE | EN */
	dbg_puts("WDT armed: MODE=");
	dbg_hex(rd32(TOPRGU_MODE), 8);
	dbg_puts(" LENGTH=");
	dbg_hex(rd32(TOPRGU_LENGTH), 8);
	dbg_puts("\nwaiting for the watchdog\n");
	for (int s = 0; s < 60; s++) {
		dbg_dec(s);
		dbg_putc(' ');
		dbg_delay_ms(1000);
	}
	dbg_puts("\nwatchdog did not fire: software reset\n");
	wdt_reset();
}
