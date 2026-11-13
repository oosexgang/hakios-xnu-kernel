/*
 * Return-to-loader debug channel.
 *
 * On hardware without a serial port there is no way to get the kernel log
 * out. The XNU loader therefore leaves UEFI intact (no ExitBootServices) and
 * publishes the physical address of a return stub in /chosen/loader-return.
 * When the kernel has nothing more to do (panic, halt, timer) it collects its
 * message buffer, the panic buffer and any kprintf output, maps the stub 1:1
 * through TTBR0 and jumps to it with that log. The loader then restores the UEFI environment and writes
 * the log to storage.
 *
 * Only valid while the kernel does not drive hardware that UEFI also uses.
 */
#include <mach/mach_types.h>
#include <machine/atomic.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/processor.h>
#include <kern/clock.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <vm/vm_page.h>
#include <vm/vm_compressor_xnu.h>  /* xnu-12377 split */

/*
 * The console log handed back to the loader. A long session writes more than fits, and both
 * ends matter: the start of the boot is kept as it is, and of everything after it the most
 * recent part (a ring); room is left for what is appended on the way out (the kernel message
 * buffer, a panic's text). loader_log_linearize() puts the pieces in order before returning.
 */
#define LOADER_LOG_SIZE (1536 * 1024)
#define LOADER_LOG_HEAD (256 * 1024)
#define LOADER_LOG_RING (1024 * 1024)

#if defined(GENERIC_GIC_V3_IRQ)
extern void gic_v3_irq_quiesce(void);
extern void gic_v3_irq_cpu_shutdown(void);
#endif

void loader_log_putc(char c);
void loader_return(void);
extern void hakios_adreno_quiesce(void);
extern void hakios_adreno_stop(void);
extern void hakios_adreno_minute(void);
extern void hakios_wifi_stop(void);
extern void hakios_wifi_quiesce(void);
extern void hakios_adsp_stop(void);
extern void hakios_adsp_quiesce(void);
extern void hakios_ddr_quiesce(void);
extern void hakios_haptic_quiesce(void);
extern void hakios_torch_quiesce(void);
extern void hakios_backlight_quiesce(void);
extern void hakios_thermal_quiesce(void);
extern void hakios_camera_quiesce(void);
extern void hakios_mic_quiesce(void);
extern void hakios_bt_uart_quiesce(void);
extern void hakios_ufs_quiesce(void);
extern void hakios_ufs_close(void);
extern void hakios_var_save_on_return(void);   /* bsd/dev/board/ufs.c: the data volume into the flash */
void loader_checkpoint(uint64_t n);

/* Also referenced from the early exception vector in start.s. */
char     loader_log[LOADER_LOG_SIZE];
uint32_t loader_log_len;
static bool     loader_return_entered;

/* One L1 table and one L2 table are enough to map a single 2 MB block. */
static uint64_t loader_l1[512] __attribute__((aligned(4096)));
static uint64_t loader_l2[512] __attribute__((aligned(4096)));

static uint32_t loader_log_ring_at;      /* where the next character goes in the ring */
static bool     loader_log_wrapped;
static bool     loader_log_linear;       /* the pieces have been put in order: only appending from now on */
static char     loader_log_scratch[LOADER_LOG_RING];

/*
 * Characters come from every processor, and from interrupt handlers on top of whatever was
 * writing one. The lock is of the simplest kind, good from the first line of C code to the
 * last of a panic; whoever cannot have it for a long while (its holder is gone: a panic
 * inside this very function) goes ahead without it.
 */
static uint32_t loader_log_lock;

static void loader_log_put_locked(char c);

/*
 * Lines worth keeping whatever the ring loses: a driver's or a library's of ours being brought up
 * ("dtouch: ", "dgpu: ", "hakios26: ", "exec: ") are copied here as they pass, and appended to the log on
 * the way out (the stock daemons' chatter can push them out of the ring in a minute).
 */
#define LOADER_KEEP_SIZE (96 * 1024)
static char     loader_keep[LOADER_KEEP_SIZE];
static uint32_t loader_keep_len;
static char     loader_keep_line[512];
static uint32_t loader_keep_line_len;
static volatile bool loader_keep_replaying;

static bool
loader_keep_wanted(const char *line)
{
	static const char *const prefixes[] = { "dtouch: ", "dgpu: ", "hakios26: ", "exec: ", "hakios: crash ", "hakios:   x0 ",
		"hakios:   frame " };
	static unsigned dyld_lines;

	/* boot-arg keep_words="a,b": any line containing one of them (e.g. the wallpaper's: keep_words=oster,allpaper) */
	{
		static char words[128];
		static int known;

		if (!known) {
			known = 1;
			if (!PE_parse_boot_arg_str("keep_words", words, sizeof(words))) {
				words[0] = 0;
			}
		}
		for (const char *w = words; *w != 0;) {
			const char *end = w;
			size_t n;

			while (*end != 0 && *end != ',') {
				end++;
			}
			n = (size_t)(end - w);
			for (const char *l = line; n != 0 && *l != 0; l++) {
				if (strncmp(l, w, n) == 0) {
					return true;
				}
			}
			w = *end == ',' ? end + 1 : end;
		}
	}
	/* launchd on SpringBoard or backboardd going away: why (the respring the user sees as a log out) */
	{
		bool exited = false, ours = false;

		for (const char *l = line; *l != 0; l++) {
			exited |= l[0] == 'e' && l[1] == 'x' && l[2] == 'i' && l[3] == 't' && l[4] == 'e' && l[5] == 'd' && l[6] == ' ';
			ours |= (l[0] == 'S' && l[1] == 'p' && l[2] == 'r' && l[3] == 'i' && l[4] == 'n' && l[5] == 'g' && l[6] == 'B') ||
			    (l[0] == 'b' && l[1] == 'a' && l[2] == 'c' && l[3] == 'k' && l[4] == 'b' && l[5] == 'o' && l[6] == 'a');
		}
		if (exited && ours) {
			return true;
		}
		/* ... and what either says when it gives up: an assertion, an exception, an abort */
		if (ours) {
			static const char *const words[] = { "Assertion failed", "uncaught exception", "Terminating app",
				"RBSTerminateContext", "watchdog", "Watchdog", "8badf00d", "Fatal error", "BUG IN CLIENT",
				"abort() called", "exit reason" };

			for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
				for (const char *l = line; *l != 0; l++) {
					const char *a = l, *b = words[w];

					while (*b != 0 && *a == *b) {
						a++;
						b++;
					}
					if (*b == 0) {
						return true;
					}
				}
			}
		}
		/* ... and what either says when it gives up: an assertion, an exception, an abort */
		if (ours) {
			static const char *const words[] = { "ssert", "xception", "erminat", "atal", "bort", "BUG IN", "rash" };

			for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
				for (const char *l = line; *l != 0; l++) {
					const char *a = l, *b = words[w];

					while (*b != 0 && *a == *b) {
						a++;
						b++;
					}
					if (*b == 0) {
						return true;
					}
				}
			}
		}
	}

	/* dyld's own lines (DYLD_PRINT_LIBRARIES, inject_print=1): the first few, and any about our library */
	if (line[0] == 'd' && line[1] == 'y' && line[2] == 'l' && line[3] == 'd' && line[4] == '[') {
		for (const char *l = line; *l != 0; l++) {
			if (l[0] == 'r' && l[1] == 'e' && l[2] == 'd' && l[3] == 'w' && l[4] == 'o' && l[5] == 'o' && l[6] == 'd') {
				return true;
			}
		}
		return dyld_lines++ < 6;
	}

	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		const char *p = prefixes[i], *l = line;

		while (*p != 0 && *p == *l) {
			p++;
			l++;
		}
		if (*p == 0) {
			return true;
		}
	}
	return false;
}

/* (under the log's lock) one more character of the line being written */
static void
loader_keep_char(char c)
{
	if (c != '\n') {
		if (loader_keep_line_len < sizeof(loader_keep_line) - 2) {
			loader_keep_line[loader_keep_line_len++] = c;
		}
		return;
	}
	loader_keep_line[loader_keep_line_len] = 0;
	if (loader_keep_wanted(loader_keep_line) && loader_keep_len + loader_keep_line_len + 1 < LOADER_KEEP_SIZE) {
		__builtin_memcpy(loader_keep + loader_keep_len, loader_keep_line, loader_keep_line_len);
		loader_keep_len += loader_keep_line_len;
		loader_keep[loader_keep_len++] = '\n';
	}
	loader_keep_line_len = 0;
}

void
loader_log_putc(char c)
{
	uint64_t daif;
	bool locked = false;

	if (c == '\r') {
		return;
	}
	daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
		locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	loader_log_put_locked(c);
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
}

/*
 * A whole line at once, under the lock: lines written from several processors at the same time
 * (tools/latency.py's "tl:" lines, a frame's on one and a touch's on another) are not mixed up.
 */
void loader_log_line(const char *line);

void
loader_log_line(const char *line)
{
	uint64_t daif;
	bool locked = false;

	daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
		locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	for (; *line != 0; line++) {
		if (*line != '\r') {
			loader_log_put_locked(*line);
		}
	}
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
}

static void
loader_log_put_locked(char c)
{
	if (!loader_log_linear && !loader_keep_replaying) {
		loader_keep_char(c);
	}
	if (loader_log_linear || loader_log_len < LOADER_LOG_HEAD) {
		if (loader_log_len < LOADER_LOG_SIZE) {
			loader_log[loader_log_len++] = c;
		}
	} else {
		loader_log[LOADER_LOG_HEAD + loader_log_ring_at] = c;
		if (++loader_log_ring_at == LOADER_LOG_RING) {
			loader_log_ring_at = 0;
			loader_log_wrapped = true;
		}
		if (!loader_log_wrapped) {
			loader_log_len = LOADER_LOG_HEAD + loader_log_ring_at;
		}
	}
}

/* Oldest first: the head, a note about what was dropped, then the ring from its oldest character. */
static void
loader_log_linearize(void)
{
	static const char note[] = "\n\n[... the log between the first 256 KB and the last 1 MB was dropped ...]\n\n";
	uint32_t older = LOADER_LOG_RING - loader_log_ring_at, n = LOADER_LOG_HEAD;

	if (loader_log_linear) {
		return;
	}
	loader_log_linear = true;
	if (!loader_log_wrapped) {
		return;
	}
	__builtin_memcpy(loader_log_scratch, loader_log + LOADER_LOG_HEAD + loader_log_ring_at, older);
	__builtin_memcpy(loader_log_scratch + older, loader_log + LOADER_LOG_HEAD, loader_log_ring_at);
	/* the note takes the place of the oldest characters of the ring */
	__builtin_memcpy(loader_log + n, note, sizeof(note) - 1);
	n += sizeof(note) - 1;
	__builtin_memcpy(loader_log + n, loader_log_scratch + sizeof(note) - 1, LOADER_LOG_RING - (sizeof(note) - 1));
	loader_log_len = LOADER_LOG_HEAD + LOADER_LOG_RING;
}

/* Patched by the loader: { magic, PA of its return stub, checkpoint to stop at, PA - VA }. See start.s. */
extern uint64_t loader_stub_slot[4];

/*
 * For a loader that watches from another processor core while the kernel runs (it starts the
 * kernel on a core of its choice and stays on the first one): a sign of life, and where the log
 * is, so that the log of a kernel that hangs -- or dies before it has said a word through any
 * other channel -- is not lost with it. The loader finds this by its first word in the kernel
 * it has loaded. The addresses are the kernel's virtual ones, filled in by the linker and so
 * valid from the first instruction; the loader knows the distance to the physical ones.
 */
struct loader_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every HV_WDT_PET_SECONDS while the kernel is alive */
	const char *log;                /* loader_log */
	const uint32_t *log_len;        /* how much of it is in use */
	const uint32_t *log_ring_at;    /* where the ring part is being written */
	const bool *log_wrapped;        /* the ring has gone round */
	uint64_t log_head, log_ring;    /* sizes of the two parts */
	const uint64_t *recording;      /* loader_recording: { physical address, length } of a screen recording */
	volatile uint64_t stop;         /* the loader's to write: the host has asked for the run to end */
	uint64_t magic2;                /* "XNULIVE2": what follows is there (a loader may meet an older kernel) */
	const uint64_t *sound;          /* loader_sound: the same for a recording of the sound */
	volatile uint64_t crumb;        /* the last loader_checkpoint(n) passed, for a watching loader */
};

/*
 * A screen recording (the display driver makes one with boot-arg rec_secs): where it is, for the
 * loader to hand to the host once the kernel is back. { physical address, bytes, kernel address }.
 */
uint64_t loader_recording[3];
/* ... and of the sound (/dev/hakios-audio makes one with boot-arg rec_sound). */
uint64_t loader_sound[3];

struct loader_live loader_live = {
	.magic = 0x214556494c554e58ULL, .log = loader_log, .log_len = &loader_log_len, .log_ring_at = &loader_log_ring_at,
	.log_wrapped = &loader_log_wrapped, .log_head = LOADER_LOG_HEAD, .log_ring = LOADER_LOG_RING,
	.recording = loader_recording,
	.magic2 = 0x324556494c554e58ULL, .sound = loader_sound,
};

/* Valid from the first instruction of C code: does not depend on gVirtBase/gPhysBase being set up. */
static inline uint64_t
static_kvtophys(const void *va)
{
	return (uint64_t)va + loader_stub_slot[3];
}

/*
 * Jump to the loader's return stub. reason: 0 = orderly end, 2 = checkpoint.
 * Returns only if no loader is listening.
 */
static void
loader_jump(uint64_t reason, uint64_t code)
{
#if ARM_PGSHIFT == 12
	uint64_t stub = loader_stub_slot[1];
	uint64_t tcr, t0sz;
	uint32_t own_core = 0;

	if (stub == 0 || loader_return_entered) {
		return;
	}

	tcr = __builtin_arm_rsr64("TCR_EL1");
	t0sz = tcr & 0x3f;
	/* The TTBR0 walk must start at L1 (1 GB per entry) and reach the stub. */
	if (t0sz < 25 || t0sz > 33 || (stub >> (64 - t0sz)) != 0) {
		return;
	}
	loader_return_entered = true;
#if defined(GENERIC_GIC_V3_IRQ)
	gic_v3_irq_quiesce();
	/*
	 * With a core of its own (bootcpu=) the kernel does not go back into the loader's code on
	 * this processor: the stub leaves the log where the loader's core finds it and has the
	 * firmware switch this one off. It must go as the other cores do (psci_smp_cpu_off()), with
	 * nothing pending: after a panic the "stop" signal and the timer are.
	 */
	if (PE_parse_boot_argn("bootcpu", &own_core, sizeof(own_core))) {
		__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
		gic_v3_irq_cpu_shutdown();
	}
#endif

	loader_l1[stub >> 30] = static_kvtophys(loader_l2) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
	loader_l2[(stub >> 21) & 0x1ff] = (stub & ~0x1fffffULL) | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |
	    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_AP(AP_RWNA) |
	    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);

	/*
	 * The stub turns the MMU off, and on real hardware dirty cache lines do
	 * not survive that: write the log back to RAM first.
	 */
	for (uintptr_t p = (uintptr_t)loader_log & ~63UL; p < (uintptr_t)loader_log + loader_log_len; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
	}
	/* ... and a screen recording, and the note of where it is */
	for (uintptr_t p = loader_recording[2] & ~63UL; p < loader_recording[2] + loader_recording[1]; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_recording) : "memory");
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_recording + 16) : "memory");
	for (uintptr_t p = loader_sound[2] & ~63UL; p < loader_sound[2] + loader_sound[1]; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_sound) : "memory");
	__asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_sound + 16) : "memory");
	__asm__ volatile ("dsb sy" ::: "memory");

	register uint64_t r_ttbr0 __asm__("x0") = static_kvtophys(loader_l1);
	register uint64_t r_log __asm__("x2") = static_kvtophys(loader_log);
	register uint64_t r_len __asm__("x3") = loader_log_len;
	register uint64_t r_tcr __asm__("x4") = tcr;
	register uint64_t r_stub __asm__("x5") = stub;
	register uint64_t r_reason __asm__("x6") = reason;
	register uint64_t r_code __asm__("x7") = code;

	__asm__ volatile (
                "msr DAIFSet, #0xf\n"
                "bic x4, x4, #0x80\n"           /* TCR_EL1.EPD0: allow TTBR0 walks */
                "msr TCR_EL1, x4\n"
                "msr TTBR0_EL1, x0\n"
                "isb\n"
                "tlbi vmalle1\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, x2\n"
                "mov x1, x3\n"
                "mov x2, x6\n"
                "mov x3, x7\n"
                "br x5\n"
                : : "r" (r_ttbr0), "r" (r_log), "r" (r_len), "r" (r_tcr), "r" (r_stub), "r" (r_reason), "r" (r_code));
	__builtin_unreachable();
#else
	(void)reason; (void)code;
#endif /* ARM_PGSHIFT == 12 */
}

extern char *debug_buf_base, *debug_buf_ptr;

/* bsd/sys/msgbuf.h (not includable from osfmk) */
struct msgbuf {
	int   msg_magic;
	int   msg_size;
	int   msg_bufx;         /* write pointer */
	int   msg_bufr;         /* read pointer */
	char *msg_bufc;         /* buffer */
};
extern struct msgbuf *msgbufp;

static void
loader_log_puts(const char *s)
{
	for (; *s; s++) {
		loader_log_putc(*s);
	}
}

/*
 * Everything printf()/IOLog()/os_log wrote ends up in the kernel message
 * buffer (what dmesg shows), whether or not a console displayed it.
 */
static void
loader_log_append_msgbuf(void)
{
	struct msgbuf *mb = msgbufp;

	if (mb == NULL || mb->msg_bufc == NULL || mb->msg_size <= 0 || mb->msg_bufx < 0 || mb->msg_bufx > mb->msg_size) {
		return;
	}
	loader_log_puts("\n--- kernel message buffer ---\n");
	/* oldest data first: [bufx, size) is only meaningful once the buffer has wrapped */
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? mb->msg_bufx : 0;
		int to = pass == 0 ? mb->msg_size : mb->msg_bufx;
		for (int i = from; i < to; i++) {
			if (mb->msg_bufc[i] != 0) {
				loader_log_putc(mb->msg_bufc[i]);
			}
		}
	}
}

void
loader_return(void)
{
	hakios_ufs_close();             /* no read of the storage in flight from here on */
#if defined(GENERIC_PSCI_SMP)
	/*
	 * With more than one processor: the others switch themselves off first, and the rest of
	 * this is done by the boot processor, the one the loader gave us. This call comes back
	 * on that processor only (on another one it has it call loader_return() in its turn).
	 */
	psci_smp_stop_others();
#endif
	hakios_bt_uart_quiesce();     /* Bluetooth enable low and its power/clock votes dropped */
	hakios_ufs_quiesce();          /* this kernel's requests to the storage done, UEFI's list as it was, for the log */
	hakios_adreno_quiesce();       /* the GPU block, if it was started, has tables in memory that is about to be reused */
	hakios_wifi_quiesce();         /* the Wi-Fi DSP, if it still runs (a panic), is not left to itself */
	hakios_adsp_quiesce();         /* nor the audio DSP */
	hakios_ddr_quiesce();          /* and the vote for the memory's bandwidth taken away */
	hakios_haptic_quiesce();       /* the vibration motor's chip back into reset, if it was let out */
	hakios_torch_quiesce();        /* the flashlight off, if it was lit */
	hakios_backlight_quiesce();     /* the panel's light back where the loader expects it */
	hakios_thermal_quiesce();       /* the processors' groups back at the rows they were found at */
	hakios_camera_quiesce();       /* the camera powered down, if it was powered up */
	hakios_mic_quiesce();          /* (what the microphones' path did, said again: it happens in the log's middle) */
	/*
	 * A panic's text is first collected in the panic buffer and only later
	 * flushed to the console; if the console itself is what failed, that
	 * never happens. Append the buffer so the loader gets it either way.
	 */
	loader_log_linearize();
	if (!loader_return_entered && loader_keep_len != 0) {
		loader_log_puts("\n--- kept lines (dtouch, hakios26, exec) ---\n");
		for (uint32_t i = 0; i < loader_keep_len; i++) {
			loader_log_putc(loader_keep[i]);
		}
	}
	if (!loader_return_entered) {
		loader_log_append_msgbuf();
	}
	if (!loader_return_entered && debug_buf_base != NULL && debug_buf_ptr > debug_buf_base &&
	    (size_t)(debug_buf_ptr - debug_buf_base) < 256 * 1024) {
		loader_log_puts("\n--- panic buffer ---\n");
		for (const char *c = debug_buf_base; c < debug_buf_ptr; c++) {
			loader_log_putc(*c);
		}
	}
	loader_jump(0, 0);
}

/* Bring-up aid: stop here if the loader asked for checkpoint n. */
void
loader_checkpoint(uint64_t n)
{
	loader_live.crumb = n;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (loader_stub_slot[2] == n) {
		loader_jump(2, n);
	}
}

/*
 * boot-arg loader_return_after=<seconds>: hand control back to the loader
 * after that long, whatever the kernel is doing. This is how the log gets out
 * when the kernel does not panic but simply waits (e.g. for a root device).
 */
/*
 * For trying the ways out when there is more than one processor: boot-arg return_cpu=<n> has
 * that processor make the timed return, from an interrupt handler -- any but the boot processor
 * has to hand it over (psci_smp_stop_others()) --, and return_panic=1 makes a panic of it.
 */
static void
loader_return_test(void *as_panic)
{
	if (as_panic != (void *)1) {
		panic("return_panic: the panic that was asked for, on cpu %d", cpu_number());
	}
	loader_return();
}

static void hk_stack_snapshot(void);

/*
 * The kept lines once more, into the ring this time: the host fetches the log only while the
 * kernel runs, so what is appended on the way out never reaches it; 10 s before the end it does.
 */
static void
loader_keep_replay(void)
{
	char line[200];
	uint32_t at = 0, end = loader_keep_len;

	loader_keep_replaying = true;
	loader_log_line("\n--- kept lines (dtouch, hakios26, exec), again ---\n");
	while (at < end) {
		uint32_t n = 0;

		while (at < end && loader_keep[at] != '\n' && n < sizeof(line) - 2) {
			line[n++] = loader_keep[at++];
		}
		at++;
		line[n++] = '\n';
		line[n] = 0;
		loader_log_line(line);
	}
	loader_log_line("--- end of kept lines ---\n");
	loader_keep_replaying = false;
}

static void
hk_stack_snapshot_call(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	loader_keep_replay();
	hk_stack_snapshot();
}

static void
loader_return_timer(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static const char banner[] = "\n--- loader_return_after expired ---\n";
	uint32_t cpu = 0, as_panic = 0;

	for (const char *c = banner; *c; c++) {
		loader_log_putc(*c);
	}
	PE_parse_boot_argn("return_panic", &as_panic, sizeof(as_panic));
	/* the watched processes once more, as the run ends: what is written now reaches the host in the
	 * log the loader stages after the return, and the one of ten seconds ago may have been pushed out
	 * of the ring by then (a megabyte of the window server's lines in ten seconds) */
	if (PE_parse_boot_arg_str("stacks", (char [8]){ 0 }, 8)) {
		hk_stack_snapshot();
	}
	if (!as_panic) {
		hakios_var_save_on_return();   /* the data volume kept, while the system is still whole */
		hakios_adreno_stop();          /* a GPU kept for the system is given back while there is a thread to wait in */
		hakios_wifi_stop();            /* and the Wi-Fi DSP, if it runs, shut down */
		hakios_adsp_stop();            /* and the audio DSP */
	}
	/*
	 * The return is the boot processor's to make (it is the core the loader gave the kernel; the others
	 * are switched off first). A timer that fires on another core hands it over and waits: making the
	 * hand-over itself (psci_smp_stop_others) from a secondary core died in a data abort.
	 */
	{
		int boot_cpu = master_processor->cpu_id;      /* (not master_cpu: that one is not set on this path) */

		if (!PE_parse_boot_argn("return_cpu", &cpu, sizeof(cpu)) && cpu_number() != boot_cpu) {
			cpu = (uint32_t)boot_cpu;
			if (cpu_xcall((int)cpu, loader_return_test, (void *)(uintptr_t)(as_panic ? 2 : 1)) == KERN_SUCCESS) {
				for (;;) {
					delay_for_interval(1000, NSEC_PER_MSEC);
				}
			}
		}
	}
	if (PE_parse_boot_argn("return_cpu", &cpu, sizeof(cpu)) && cpu <= (uint32_t)ml_get_max_cpu_number() &&
	    cpu_xcall((int)cpu, loader_return_test, (void *)(uintptr_t)(as_panic ? 2 : 1)) == KERN_SUCCESS) {
		for (;;) {
			delay_for_interval(1000, NSEC_PER_MSEC);        /* that processor ends it */
		}
	}
	loader_return_test((void *)(uintptr_t)(as_panic ? 2 : 1));
}

__startup_func
static void
loader_return_timer_init(void)
{
	uint32_t seconds = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	thread_call_t call = thread_call_allocate(loader_return_timer, NULL);
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
	/* the snapshot of the watched processes ten seconds before: the host's last look at the live log has it */
	if (seconds > 20) {
		thread_call_t snap = thread_call_allocate(hk_stack_snapshot_call, NULL);
		clock_interval_to_deadline(seconds - 10, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(snap, deadline);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_return_timer_init);

/*
 * A loader that watches from another core also listens to the host, and passes on a request to
 * end the run ("xl stop") by setting loader_live.stop. Look once a second.
 */
static thread_call_t loader_stop_call;

static void
loader_stop_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (loader_live.stop) {
		static const char banner[] = "\n--- the host asked for the end of the run: back to the loader ---\n";
		for (const char *c = banner; *c; c++) {
			loader_log_putc(*c);
		}
		hakios_var_save_on_return();
		hakios_adreno_stop();
		hakios_wifi_stop();
		hakios_adsp_stop();
		loader_return();
	}
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(loader_stop_call, deadline);
}

__startup_func
static void
loader_stop_poll_init(void)
{
	loader_stop_call = thread_call_allocate(loader_stop_poll, NULL);
	loader_stop_poll(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_stop_poll_init);

/*
 * boot-arg hv_wdt_pet=1: the loader has armed the Qualcomm hypervisor's
 * virtual watchdog (it resets the phone when a kernel hangs, so that test
 * runs need nobody at the device). Its timeout is at most 65 seconds; feed
 * it from a timer so that a healthy kernel can run for as long as it likes.
 */
#define VIRT_WDT_PET            0x86000007u
#define HV_WDT_PET_SECONDS      10

static thread_call_t hv_wdt_call;
static uint32_t hv_wdt_enabled;

#define MEMORY_REPORT_BEATS     6               /* a line about memory every so many heartbeats */

extern unsigned int memorystatus_available_pages;
/* xnu-12377 keeps the jetsam thresholds behind these; the old memorystatus_available_pages_critical
 * names are only this port's stand-ins for the closed KernelSupport library and stay 0. */
extern uint32_t memorystatus_get_idle_exit_page_shortage_threshold(void);
extern uint32_t memorystatus_get_critical_page_shortage_threshold(void);
extern uint32_t c_segment_pages_compressed;
extern _Atomic uint64_t ramfs_pages_in_use;
extern void MiniIOSurfaceUsage(uint32_t *count, uint64_t *bytes);

/*
 * Where the memory is, in megabytes: for reading a session's log afterwards. "available" is what
 * the kernel's own count says (free and file-backed): below the first threshold it kills idle
 * daemons, and once none is left and it is below the second, whatever is next in line -- the
 * window server included.
 */
void hakios_memory_line(char *buf, size_t len);

/* The line itself, as the heartbeat prints it */
void
hakios_memory_line(char *buf, size_t len)
{
#define MB(pages)       ((unsigned int)(((uint64_t)(pages) * PAGE_SIZE) >> 20))
	uint32_t surfaces = 0;
	uint64_t surface_bytes = 0;

	MiniIOSurfaceUsage(&surfaces, &surface_bytes);
	snprintf(buf, len, "memory: free %u, files %u, programs %u, wired %u (%u surfaces %u, ramfs %u), compressor %u "
	    "(holding %u) MB; available %u, kills below %u (the idle) and %u (anybody)\n",
	    MB(vm_page_free_count), MB(vm_page_pageable_external_count), MB(vm_page_pageable_internal_count),
	    MB(vm_page_wire_count), surfaces, (unsigned int)(surface_bytes >> 20),
	    MB(os_atomic_load(&ramfs_pages_in_use, relaxed)),
	    MB(VM_PAGE_COMPRESSOR_COUNT), MB(c_segment_pages_compressed),
	    MB(memorystatus_available_pages),
	    MB(memorystatus_get_idle_exit_page_shortage_threshold()),
	    MB(memorystatus_get_critical_page_shortage_threshold()));
#undef MB
}

static void
memory_report(void)
{
	char line[256];

	hakios_memory_line(line, sizeof(line));
	printf("%s", line);
	hakios_adreno_minute();                /* (and how warm the GPU is, if it is in use) */
}

/* Every HV_WDT_PET_SECONDS: feed the watchdog if there is one, and show any watching loader a sign of life. */
static void
hv_wdt_pet(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (hv_wdt_enabled) {
		register uint64_t x0 __asm__("x0") = VIRT_WDT_PET;
		register uint64_t x1 __asm__("x1") = 0;
		register uint64_t x2 __asm__("x2") = 0;
		register uint64_t x3 __asm__("x3") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	if (++loader_live.heartbeat % MEMORY_REPORT_BEATS == 0) {
		memory_report();
	}
#if HAS_STD_PAC
	if (loader_live.heartbeat == 1) {
		extern void hk_ib_report(const char *where);
		hk_ib_report("first heartbeat");
	}
#endif

	clock_interval_to_deadline(HV_WDT_PET_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(hv_wdt_call, deadline);
}

__startup_func
static void
hv_wdt_pet_init(void)
{
	PE_parse_boot_argn("hv_wdt_pet", &hv_wdt_enabled, sizeof(hv_wdt_enabled));
	hv_wdt_call = thread_call_allocate(hv_wdt_pet, NULL);
	hv_wdt_pet(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hv_wdt_pet_init);

/*
 * hakios: a crash report in the loader's log. A process that dies of a signal that would dump core
 * (SIGABRT, SIGSEGV, SIGBUS, SIGTRAP, SIGILL) leaves its registers and the return addresses of its
 * frame chain here, with the shared cache's slide, so they can be named offline (ipsw dyld a2s); Darwin's
 * own crash reports need daemons that are not running yet. Called on the dying thread, from postsig.
 */
#include <kern/task.h>
#include <arm/thread.h>
#include <libkern/copyio.h>

#define HK_USER_ADDR(a) ((a) & 0x0000007fffffffffULL)  /* without the pointer's signature */

void hk_crash_report(const char *name, int pid, int sig);

void
hk_crash_report(const char *name, int pid, int sig)
{
	thread_t thread = current_thread();
	task_t task = current_task();
	struct arm_saved_state *ss = find_user_regs(thread);
	char line[200];
	uint64_t fp, frame[2];

	if (ss == NULL || !is_saved_state64(ss)) {
		return;
	}
	snprintf(line, sizeof(line), "hakios: crash %s[%d] signal %d pc %llx lr %llx sp %llx far %llx esr %llx slide %llx\n",
	    name, pid, sig, HK_USER_ADDR(get_saved_state_pc(ss)), HK_USER_ADDR(get_saved_state_lr(ss)),
	    get_saved_state_sp(ss), get_saved_state_far(ss), (unsigned long long)get_saved_state_esr(ss),
	    (unsigned long long)task->task_shared_region_slide);
	loader_log_line(line);
	snprintf(line, sizeof(line), "hakios:   x0 %llx x1 %llx x2 %llx x3 %llx x8 %llx x16 %llx\n",
	    get_saved_state_reg(ss, 0), get_saved_state_reg(ss, 1), get_saved_state_reg(ss, 2),
	    get_saved_state_reg(ss, 3), get_saved_state_reg(ss, 8), get_saved_state_reg(ss, 16));
	loader_log_line(line);
	fp = get_saved_state_fp(ss);
	for (int i = 0; i < 24 && fp != 0 && (fp & 7) == 0; i++) {
		if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
			break;
		}
		snprintf(line, sizeof(line), "hakios:   frame %d: %llx\n", i, HK_USER_ADDR(frame[1]));
		loader_log_line(line);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
	/* and where this project's own libraries were loaded in it, for the frames that are in them
	 * (dyld's all_image_infos: version, count, then the array of { load address, path, date });
	 * ours are those of the small image, which stage0 mounts at /hakios */
	if (task->all_image_info_addr != 0) {
		uint32_t head[2];
		uint64_t array = 0;

		if (copyin((user_addr_t)task->all_image_info_addr, head, sizeof(head)) == 0 &&
		    copyin((user_addr_t)task->all_image_info_addr + 8, &array, sizeof(array)) == 0) {
			for (uint32_t i = 0; i < head[1] && i < 1024; i++) {
				uint64_t info[3];
				char path[96] = "";

				if (copyin((user_addr_t)(array + 24 * i), info, sizeof(info)) != 0) {
					break;
				}
				if (info[0] >= 0x180000000ull && info[0] < 0x300000000ull) {
					continue;
				}
				size_t got = 0;

				(void)copyinstr((user_addr_t)info[1], path, sizeof(path) - 1, &got);
				path[sizeof(path) - 1] = '\0';
				if (strnstr(path, "/hakios/", sizeof(path)) == NULL) {
					continue;
				}
				snprintf(line, sizeof(line), "hakios:   frame image %llx %s\n", info[0], path);
				loader_log_line(line);
			}
		}
	}
}

/*
 * hakios: where the threads of a few processes are when the run ends: each thread's user pc and lr
 * and the return addresses of its frame chain (read from its own memory), with the shared cache's
 * slide, for naming offline. Boot-arg stacks="name name ..." (p_comm, at most 16 characters each);
 * by default the window server and the system shell.
 */
#include <kern/processor.h>
#include <vm/vm_map_xnu.h>

extern char *proc_name_address(void *p);
extern void *get_bsdtask_info(task_t);

static void
hk_stack_snapshot(void)
{
	/*
	 * Only references and registers are taken under the locks; the user memory (dyld's image
	 * list, the frame chains) is read after they are let go of. Read under tasks_threads_lock,
	 * a page of it that had to come from the phone's storage (the DSU root is read on demand)
	 * stalled the snapshot -- most runs ended without one.
	 */
	enum { HK_TASKS = 4, HK_THREADS = 96 };
	char wanted[128] = "backboardd SpringBoard";
	char line[200];
	task_t task, found[HK_TASKS];
	unsigned nfound = 0;

	PE_parse_boot_arg_str("stacks", wanted, sizeof(wanted));
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *p = get_bsdtask_info(task);
		const char *name = p ? proc_name_address(p) : NULL;
		size_t n;

		if (name == NULL || (n = strlen(name)) == 0 || nfound == HK_TASKS) {
			continue;
		}
		const char *w = wanted, *hit = NULL;
		while ((w = strnstr(w, name, strlen(w))) != NULL) {
			if ((w == wanted || w[-1] == ' ') && (w[n] == ' ' || w[n] == '\0')) {
				hit = w;
				break;
			}
			w += n;
		}
		if (hit != NULL) {
			task_reference(task);
			found[nfound++] = task;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);

	for (unsigned t = 0; t < nfound; t++) {
		static struct { uint64_t id, pc, lr, fp; } regs[HK_THREADS];
		unsigned nregs = 0;
		thread_t thread;
		void *p;

		task = found[t];
		p = get_bsdtask_info(task);
		snprintf(line, sizeof(line), "hakios: stacks of %s (slide %x)\n", p ? proc_name_address(p) : "?",
		    task->task_shared_region_slide);
		loader_log_line(line);
		task_lock(task);
		queue_iterate(&task->threads, thread, thread_t, task_threads) {
			struct arm_saved_state *ss = find_user_regs(thread);

			if (ss == NULL || !is_saved_state64(ss) || nregs == HK_THREADS) {
				continue;
			}
			regs[nregs].id = thread->thread_id;
			regs[nregs].pc = get_saved_state_pc(ss);
			regs[nregs].lr = get_saved_state_lr(ss);
			regs[nregs].fp = get_saved_state_fp(ss);
			nregs++;
		}
		task_unlock(task);
		/* the images outside the shared cache, where they were loaded: dyld's all_image_infos
		 * (version, count, then a pointer to the array of { load address, path, mod date }) */
		if (task->all_image_info_addr != 0) {
			uint32_t head[2];
			uint64_t array = 0;

			if (vm_map_read_user(task->map, task->all_image_info_addr, head, sizeof(head)) == KERN_SUCCESS &&
			    vm_map_read_user(task->map, task->all_image_info_addr + 8, &array, sizeof(array)) == KERN_SUCCESS) {
				for (uint32_t i = 0; i < head[1] && i < 1024; i++) {
					uint64_t info[3];
					char path[96] = "";

					if (vm_map_read_user(task->map, array + 24 * i, info, sizeof(info)) != KERN_SUCCESS) {
						break;
					}
					if (info[0] >= 0x180000000ull && info[0] < 0x300000000ull) {
						continue;       /* in the shared cache */
					}
					(void)vm_map_read_user(task->map, info[1], path, sizeof(path) - 1);
					path[sizeof(path) - 1] = '\0';
					snprintf(line, sizeof(line), "hakios:   image %llx %s\n", info[0], path);
					loader_log_line(line);
				}
			}
		}
		for (unsigned r = 0; r < nregs; r++) {
			uint64_t fp = regs[r].fp, frame[2];
			char chain[120];
			size_t used = 0;

			chain[0] = '\0';
			for (int i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && used < sizeof(chain) - 12; i++) {
				if (vm_map_read_user(task->map, (vm_map_address_t)fp, frame, sizeof(frame)) != KERN_SUCCESS) {
					break;
				}
				used += (size_t)snprintf(chain + used, sizeof(chain) - used, " %llx", HK_USER_ADDR(frame[1]));
				if (frame[0] <= fp) {
					break;
				}
				fp = frame[0];
			}
			snprintf(line, sizeof(line), "hakios:   thread %llx pc %llx lr %llx:%s\n", regs[r].id,
			    HK_USER_ADDR(regs[r].pc), HK_USER_ADDR(regs[r].lr), chain);
			loader_log_line(line);
		}
		task_deallocate(task);
	}
}
