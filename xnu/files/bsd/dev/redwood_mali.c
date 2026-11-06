/*
 * The GPU of the Infinix Hot 20 (X6826B), a Mali-G52 MC2, brought up from the kernel. Only with
 * boot-arg mali=1: every address below belongs to this phone (the other phone's GPU is an Adreno,
 * bsd/dev/redwood_adreno.c).
 *
 * A Bifrost Mali needs no firmware, so bringing it up is power and clocks and nothing else, in the
 * order MediaTek's own kernel uses -- the same sequence the loader's GPU probe proved on the phone
 * (mtkloader/mtkmali.h, where it is written out with its sources):
 *
 *   1  the "mfg_sel" clock mux in topckgen, to the main PLL's third (364 MHz)
 *   2  the four power domains in the SPM, MFG_ASYNC, MFG, MFG_CORE0 and _CORE1, each with its SRAM,
 *      and once MFG has power the bus protection between the GPU block and the rest released
 *   3  the block's own clock gate (BG3D in mfgcfg)
 *   4  the GPU: which one it is, a soft reset, and its L2, tiler and two shader cores powered
 *   5  one job, the smallest there is -- write a value into memory -- through address space 0 in
 *      identity mode (a GPU address is the physical one), and the processor reads the value back
 *   6  the same job again through the GPU's own page tables, at a GPU address of their making
 *
 * The PMIC's two GPU rails, VGPU and VSRAM_GPU, are left on by the preloader; this does not touch
 * them. Afterwards the GPU stays powered, for what is to drive it next. sysctl kern.redwood.mali
 * says how far it got: 6 means every step above worked.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern vm_offset_t phystokv(uint64_t pa);
extern void RedwoodMaliStart(void);
extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;   /* RedwoodMali.cpp */
extern struct mali_made { uint64_t address, size; uint32_t asked, flags; } gMaliMade[32];

#define TOPCKGEN_PHYS   0x10000000
#define INFRACFG_PHYS   0x10001000
#define SPM_PHYS        0x10006000
#define MFGCFG_PHYS     0x13000000
#define MALI_PHYS       0x13040000

SYSCTL_DECL(_kern_redwood);

static vm_offset_t topckgen, infracfg, spm, mfgcfg, mali;
static uint32_t reached, gpu_id, shader_mask;

SYSCTL_UINT(_kern_redwood, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 6 is a job run through page tables");
/*
 * What a program said about the GPU (userland/src/mali_test.c writes it): repeated in the log once a
 * minute, so that it lands in the end of a run's log, which is the part that is kept.
 */
static uint32_t user_said;
SYSCTL_UINT(_kern_redwood, OID_AUTO, mali_user, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &user_said, 0,
    "what user space found: 1 a job ran, 2 no service, 3 no client, 4 no buffer, 5 no mapping, 6 the job did nothing");

/*
 * What gl_test drew, for the log's end as well: who drew it (1 the Mali, 2 the processor) times 1000
 * plus the share of the picture inside the triangle in percent; and the middle pixel, red in the
 * low byte.
 */
/*
 * kern.redwood.say: a line from a program, into the kernel's log (a job's own output is kept nowhere):
 * sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line)).
 */
#define KEPT_LINES 96
static char kept_lines[KEPT_LINES][200];
static uint32_t kept_next;
static LCK_GRP_DECLARE(kept_lock_group, "redwood-said");
static LCK_MTX_DECLARE(kept_lock, &kept_lock_group);

/* A line from the kernel itself, kept and said again every minute as kern.redwood.say's "!" ones are. */
void redwood_keep(const char *format, ...) __printflike(1, 2);
void
redwood_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % KEPT_LINES], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

static int
say_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 1) {
		length = sizeof(line) - 1;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	line[length] = 0;
	printf("%s%s", line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	if (line[0] == '!') {                   /* kept, and said again every minute (mali_thread) */
		lck_mtx_lock(&kept_lock);
		strlcpy(kept_lines[kept_next % KEPT_LINES], line + 1, sizeof(kept_lines[0]));
		kept_next++;
		lck_mtx_unlock(&kept_lock);
	}
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    say_line, "A", "");

/*
 * kern.redwood.host_keep: lines for the host that must not be lost with the log's middle. The log the
 * host gets is a 256 KB head and tail of a run's (mtkloader save_log): the pipeline recipes a two-hour
 * run built (run 352) were in neither. A line written here goes into this buffer only (the writer says
 * in the log what it wants said there), and mtkloader puts the buffer into ramoops' pmsg zone when the
 * kernel comes back, by whatever way it does (its end, a panic, the watchdog), where Android shows it
 * as /sys/fs/pstore/pmsg-ramoops-0 and tools/xnu-cycle.sh appends it to the log. Lines that do not fit
 * are counted, not kept.
 */
#define HOST_KEEP_SIZE 0xff00          /* the pmsg zone is 64 KB, less its header and mtkloader's first line */
char redwood_host_keep[HOST_KEEP_SIZE];
uint32_t redwood_host_keep_len, redwood_host_keep_dropped;

static int
host_keep_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 2) {
		length = sizeof(line) - 2;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	if (length == 0 || line[length - 1] != '\n') {
		line[length++] = '\n';
	}
	line[length] = 0;
	lck_mtx_lock(&kept_lock);
	if (redwood_host_keep_len + length <= sizeof(redwood_host_keep)) {
		memcpy(redwood_host_keep + redwood_host_keep_len, line, length);
		redwood_host_keep_len += length;
	} else {
		redwood_host_keep_dropped++;
	}
	lck_mtx_unlock(&kept_lock);
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, host_keep, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    host_keep_line, "A", "a line for the host, kept whole however long the log grows");

/* ... and from the kernel itself: a line said, kept for the end of the log, and kept for the host (a bring-up's results,
 * which the minute's 96 kept lines lose to the display's in a run of some minutes: CONNSYS's in run 355) */
void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
void
redwood_host_keep_add(const char *format, ...)
{
	char line[200];
	size_t length;
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line) - 1, format, ap);
	va_end(ap);
	redwood_keep("%s", line);
	length = strlen(line);
	line[length++] = '\n';
	lck_mtx_lock(&kept_lock);
	if (redwood_host_keep_len + length <= sizeof(redwood_host_keep)) {
		memcpy(redwood_host_keep + redwood_host_keep_len, line, length);
		redwood_host_keep_len += (uint32_t)length;
	} else {
		redwood_host_keep_dropped++;
	}
	lck_mtx_unlock(&kept_lock);
}
SYSCTL_UINT(_kern_redwood, OID_AUTO, host_keep_len, CTLFLAG_RD | CTLFLAG_LOCKED, &redwood_host_keep_len, 0, "");
SYSCTL_UINT(_kern_redwood, OID_AUTO, host_keep_dropped, CTLFLAG_RD | CTLFLAG_LOCKED, &redwood_host_keep_dropped, 0, "");

static uint32_t gl_said, gl_middle;
/*
 * sysctl kern.redwood.bind_cpu=N: the calling thread runs on logical processor N from now on (-1: on
 * any). The scheduler of this kernel does not tell the two Cortex-A75s (logical 1 and 2, cores 7
 * and 6: mtkloader's core_order) from the A55s, and CoreAnimation's render thread spent 14 to 18
 * ms a frame wherever it happened to be (run 147, 26 October: the GPU busy a third of the time).
 * The Metal driver puts that thread and its own worker on the big cores.
 */
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern unsigned int real_ncpus;

static int
bind_cpu(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int cpu = -1, error;

	error = SYSCTL_IN(req, &cpu, sizeof(cpu));
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (cpu >= (int)real_ncpus) {
		return EINVAL;
	}
	(void)thread_bind(cpu < 0 ? NULL : cpu_to_processor(cpu));
	(void)thread_block(NULL);             /* over to it now */
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, bind_cpu, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    bind_cpu, "I", "bind the calling thread to a logical processor");

SYSCTL_UINT(_kern_redwood, OID_AUTO, mali_gl, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_said, 0, "");
SYSCTL_UINT(_kern_redwood, OID_AUTO, mali_gl_middle, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_middle, 0, "");
SYSCTL_UINT(_kern_redwood, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `ms`. */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t us = 0; us < ms * 1000; us += 10) {
		if ((rd(base, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (rd(base, at) & mask) == want;
}

/* ---- power ---- */

#define PWR_STATUS      0x180
#define PWR_STATUS_2ND  0x184
#define PWR_RST_B       (1u << 0)
#define PWR_ISO         (1u << 1)
#define PWR_ON          (1u << 2)
#define PWR_ON_2ND      (1u << 3)
#define PWR_CLK_DIS     (1u << 4)
#define PWR_SRAM_PDN    (1u << 8)
#define PWR_SRAM_ACK    (1u << 12)

static const struct {
	const char *name;
	uint32_t con, status;
	bool sram;
} domains[4] = {
	{ "MFG_ASYNC", 0x334, 1u << 14, false },
	{ "MFG", 0x338, 1u << 11, true },
	{ "MFG_CORE0", 0x34c, 1u << 12, true },
	{ "MFG_CORE1", 0x310, 1u << 13, true },
};

static bool
domain_on(unsigned i)
{
	uint32_t con = domains[i].con, bit = domains[i].status;

	wr(spm, con, rd(spm, con) | PWR_ON);
	wr(spm, con, rd(spm, con) | PWR_ON_2ND);
	if (!wait_for(spm, PWR_STATUS, bit, bit, 20) || !wait_for(spm, PWR_STATUS_2ND, bit, bit, 20)) {
		return false;
	}
	wr(spm, con, rd(spm, con) & ~PWR_CLK_DIS);
	wr(spm, con, rd(spm, con) & ~PWR_ISO);
	wr(spm, con, rd(spm, con) | PWR_RST_B);
	if (domains[i].sram) {
		wr(spm, con, rd(spm, con) & ~PWR_SRAM_PDN);
		return wait_for(spm, con, PWR_SRAM_ACK, 0, 20);
	}
	return true;
}

/* ---- the GPU's registers (Linux drivers/gpu/drm/panfrost/panfrost_regs.h) ---- */

#define GPU_ID              0x000
#define GPU_INT_RAWSTAT     0x020
#define GPU_INT_CLEAR       0x024
#define GPU_INT_MASK        0x028
#define GPU_CMD             0x030
#define SHADER_PRESENT      0x100
#define TILER_PRESENT       0x110
#define L2_PRESENT          0x120
#define SHADER_READY        0x140
#define TILER_READY         0x150
#define L2_READY            0x160
#define SHADER_PWRON        0x180
#define TILER_PWRON         0x190
#define L2_PWRON            0x1a0
#define IRQ_RESET_COMPLETED (1u << 8)

#define JOB_INT_RAWSTAT     0x1000
#define JOB_INT_CLEAR       0x1004
#define JOB_INT_MASK        0x1008
#define JS1                 0x1880      /* job slot 1, where compute-like jobs go */
#define JS_STATUS           0x24
#define JS_HEAD_NEXT_LO     0x40
#define JS_HEAD_NEXT_HI     0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT      0x58
#define JS_COMMAND_NEXT     0x60
#define JS_CONFIG           ((3u << 8) | (3u << 12) | (8u << 16))  /* AS 0, caches flushed both ends */

#define AS0                 0x2400
#define AS_TRANSTAB_LO      0x00
#define AS_TRANSTAB_HI      0x04
#define AS_MEMATTR_LO       0x08
#define AS_MEMATTR_HI       0x0c
#define AS_COMMAND          0x18
#define AS_FAULTSTATUS      0x1c
#define AS_STATUS           0x28
#define AS_TRANSCFG_LO      0x30
#define AS_TRANSCFG_HI      0x34

/* ---- one job ---- */

/*
 * The smallest job there is, in a page of the kernel's own: a job header saying "write value", and
 * its payload -- where, what kind of value (a 32-bit immediate), and the value -- the "where"
 * being the same page 0x800 further on. `gpu` is the address the GPU knows the page by: its
 * physical address in identity mode, or wherever the page tables put it. The processor's caches
 * are flushed either side, since the GPU here does not look into them.
 */
static bool
run_write_job(vm_offset_t page, uint64_t gpu, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page, *target = (volatile uint32_t *)(page + 0x800);
	bool ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	job[10] = 6;                            /* a 32-bit immediate */
	job[12] = 0xc0ffee01u;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	wr(mali, JOB_INT_MASK, 0);
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu);
	wr(mali, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	wr(mali, JS1 + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, JS1 + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, JS1 + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, JS1 + JS_COMMAND_NEXT, 1);     /* start */
	(void)wait_for(mali, JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 1u << 1, 100);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	ok = *target == 0xc0ffee01u;
	printf("mali: %s: interrupts %#x, slot status %#x, MMU fault %#x, job status %#x, the word %#x%s\n", what,
	    rd(mali, JOB_INT_RAWSTAT), rd(mali, JS1 + JS_STATUS), rd(mali, AS0 + AS_FAULTSTATUS), job[0], *target,
	    ok ? " -- the GPU ran it" : " -- NOT WRITTEN");
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	return ok;
}

static void
as0_update(uint64_t transtab, uint32_t transcfg)
{
	wr(mali, AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	wr(mali, AS0 + AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	wr(mali, AS0 + AS_TRANSCFG_LO, transcfg);
	wr(mali, AS0 + AS_TRANSCFG_HI, 0);
	wr(mali, AS0 + AS_MEMATTR_LO, 0x88888888u);     /* every attribute: write-back, the GPU's own caches */
	wr(mali, AS0 + AS_MEMATTR_HI, 0x88888888u);
	wr(mali, AS0 + AS_COMMAND, 1);                  /* update */
	(void)wait_for(mali, AS0 + AS_STATUS, 1, 0, 50);
}

/* ---- the GPU's own page tables ---- */

/*
 * The format Linux's panfrost gives this GPU: ordinary AArch64 stage-1 tables with 4 KB pages
 * (AS_TRANSCFG mode 6). The MMU says it translates 35 bits of address (MMU_FEATURES 0x2823), which
 * makes three levels: the top table is indexed by bits 34..30, then 29..21, then 20..12. A page's
 * entry is its address, the access flag, inner shareable, readable and writable (AP 01), memory
 * attribute 0, and the low bits 11 ("page").
 */
#define PTE_TABLE           3ull
#define PTE_PAGE            3ull
#define PTE_AF              (1ull << 10)
#define PTE_SH_INNER        (3ull << 8)
#define PTE_AP_RW           (1ull << 6)
#define PTE_ADDRESS         0x0000fffffffff000ull
#define TRANSCFG_WALK_WB    (6u | (20u << 6) | (2u << 24) | (1u << 30))  /* 55 - 35 address bits; walks write-back */
#define TRANSCFG_WALK_NC    (6u | (20u << 6) | (1u << 24))              /* ... walks not cached */
/*
 * Boot-arg mali_fastmap (1 unless =0): the GPU walks its tables without caching them, so an entry
 * the processor has written (and flushed to memory) is what the next walk reads -- and a buffer
 * newly entered needs no command to the MMU at all, an empty entry being one no TLB keeps. Each new
 * buffer used to be an UPDATE of address space 0 and a wait for it: 2.4 ms a buffer, 320 buffers
 * every five seconds in the window server (run 139, 26 October). Taking a buffer out still updates.
 */
static int mali_fastmap = 1;
#define TRANSCFG_AARCH64_4K (mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB)

static uint64_t *pt_top;
static uint64_t pt_top_phys;

static uint64_t *
pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	FlushPoC_DcacheRegion(page, PAGE_SIZE);  /* its zeroes in memory, where the GPU's walk reads them */
	return (uint64_t *)page;
}

/* The table below `entry`, made if there is none yet. */
static uint64_t *
pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1) {
		return (uint64_t *)phystokv(*entry & PTE_ADDRESS);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* One 4 KB page of physical memory at GPU address `gpu` (with the lock held, or before anyone else runs). */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *middle, *bottom, *entry;

	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	if ((middle = pt_below(&pt_top[(gpu >> 30) & 31])) == NULL || (bottom = pt_below(&middle[(gpu >> 21) & 511])) == NULL) {
		return false;
	}
	entry = &bottom[(gpu >> 12) & 511];
	*entry = (phys & PTE_ADDRESS) | PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return true;
}

/*
 * Two jobs: first in identity mode, which needs no tables and so says whether the job manager works
 * at all; then the same job at a GPU address the tables give the page, which says whether they do.
 * Afterwards address space 0 stays on the tables, which is how anything that draws will need it.
 */
#define FIRST_JOB_GPU       0x00100000ull

static unsigned
first_jobs(void)
{
	vm_offset_t page = 0;
	unsigned done = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mali: no page for the first jobs\n");
		return 0;
	}
	as0_update(0, 2);                       /* identity: a GPU address is the physical one */
	if (run_write_job(page, kvtophys(page), "first job, identity mode")) {
		done = 1;
		if (map_page(FIRST_JOB_GPU, kvtophys(page))) {
			as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
			if (run_write_job(page, FIRST_JOB_GPU, "second job, through the GPU's page tables")) {
				done = 2;
			}
		} else {
			printf("mali: no memory for page tables\n");
		}
	}
	kmem_free(kernel_map, page, PAGE_SIZE);
	return done;
}

static void
mali_up(void)
{
	uint32_t shaders, tiler, l2;

	topckgen = ml_io_map(TOPCKGEN_PHYS, 0x1000);
	infracfg = ml_io_map(INFRACFG_PHYS, 0x1000);
	spm = ml_io_map(SPM_PHYS, 0x1000);

	/* 1: the clock; 2: power */
	wr(topckgen, 0x58, 3u | (1u << 7));     /* CLK_CFG_1_CLR: mfg_sel's source and its gate */
	wr(topckgen, 0x54, 2u);                 /* CLK_CFG_1_SET: syspll_d3 */
	reached = 1;
	for (unsigned i = 0; i < 4; i++) {
		if (!domain_on(i)) {
			printf("mali: power domain %s did not come up (status %#x)\n", domains[i].name, rd(spm, PWR_STATUS));
			return;
		}
		if (i == 1) {
			wr(infracfg, 0x2a4, (1u << 21) | (1u << 22));   /* bus protection, released */
			wr(infracfg, 0x2a4, 1u << 25);
		}
	}
	reached = 2;

	/* 3: inside the block, which now has power and a bus */
	mfgcfg = ml_io_map(MFGCFG_PHYS, 0x1000);
	mali = ml_io_map(MALI_PHYS, 0x4000);
	wr(mfgcfg, 0x008, 1u);                  /* MFG_CG_CLR: BG3D runs */
	reached = 3;

	/* 4: the GPU itself */
	gpu_id = rd(mali, GPU_ID);
	shaders = shader_mask = rd(mali, SHADER_PRESENT);
	tiler = rd(mali, TILER_PRESENT);
	l2 = rd(mali, L2_PRESENT);
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, GPU_CMD, 1);                   /* soft reset */
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, 100)) {
		printf("mali: GPU_ID %#x, but it did not finish its reset\n", gpu_id);
		return;
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, L2_PWRON, l2);
	wr(mali, TILER_PWRON, tiler);
	wr(mali, SHADER_PWRON, shaders);
	if (!wait_for(mali, L2_READY, l2, l2, 50) || !wait_for(mali, TILER_READY, tiler, tiler, 50) ||
	    !wait_for(mali, SHADER_READY, shaders, shaders, 50)) {
		printf("mali: GPU_ID %#x, but its cores did not all power up (shaders ready %#x)\n", gpu_id,
		    rd(mali, SHADER_READY));
		return;
	}
	reached = 4;
	printf("mali: GPU_ID %#x (Mali-G52 r%up%u), shader cores %#x, tiler %#x, L2 %#x: all powered\n", gpu_id,
	    (gpu_id >> 12) & 0xf, (gpu_id >> 4) & 0xff, shaders, tiler, l2);

	/* 5: something for it to do */
	switch (first_jobs()) {
	case 2:
		reached = 6;
		break;
	case 1:
		reached = 5;
		break;
	default:
		break;
	}
}

/* ---- what the service that programs talk to uses (iokit/Kernel/RedwoodMali.cpp) ---- */

/*
 * Linux's panfrost interface, as far as Mesa's panfrost needs it to draw, is answered by the IOKit
 * service RedwoodMali; this is its half in the hardware: parameters, GPU addresses, pages entered
 * into and taken out of the tables, and jobs. One address space (0) for everyone, and one job at a
 * time, waited for before the call returns -- simple, and enough to see Mesa draw; overlapping
 * jobs and per-client address spaces come after.
 */
static LCK_GRP_DECLARE(mali_lock_group, "redwood-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
/*
 * Held while a job runs, and by what must not change the address space under one (an UPDATE when
 * entries are taken out); taken before mali_lock, never after. mali_lock itself is no longer held
 * for the length of a job: making a buffer had to wait for the job running to finish (2.5 ms a
 * buffer, 26 October).
 */
static LCK_MTX_DECLARE(mali_job_lock, &mali_lock_group);

/*
 * Until a job is done, without keeping a core busy: a moment of polling for the short ones, then
 * asleep 200 us at a time. The wait used to spin (delay(10) in a loop) for as long as the GPU
 * worked -- a whole core of the four lost to every frame's rendering.
 */
static int mali_wait_event;
static LCK_MTX_DECLARE(mali_wait_lock, &mali_lock_group);

static bool
wait_job(uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t deadline;

	for (int i = 0; i < 20; i++) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		uint64_t nap;

		clock_interval_to_deadline(200, NSEC_PER_USEC, &nap);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_wait_event, THREAD_UNINT, nap);
		lck_mtx_unlock(&mali_wait_lock);
	}
	return (rd(mali, at) & mask) == want;
}

#define MMU_INT_RAWSTAT     0x2000
#define MMU_INT_CLEAR       0x2004
#define AS_LOCKADDR_LO      0x10
#define AS_LOCKADDR_HI      0x14
#define AS_COMMAND_LOCK     2
#define AS_COMMAND_FLUSH_PT 4
#define JS0                 0x1800      /* fragment jobs; everything else goes to slot 1 */
#define GPU_ADDRESS_FIRST   0x01000000ull
#define GPU_ADDRESS_END     (1ull << 35)

static uint64_t next_address = GPU_ADDRESS_FIRST;

/*
 * The last buffers handed GPU addresses, and whether they have been taken out of the tables
 * since: addresses are never given twice, so a translation fault is a job writing into a
 * buffer already freed or past the end of one, and this says which. With mali_lock held.
 */
#define MALI_RECENT 4096
static struct {
	uint64_t address, size;
	uint32_t freed, kind;                   /* kind: 0 not said, 1 a panfrost BO, 2 an IOSurface */
	uint32_t width, height, row_bytes, format, offset;
} mali_recent[MALI_RECENT];
static uint32_t mali_recent_next;

/* What a buffer just made is, for describe_address. */
void
redwood_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == address && mali_recent[i].size != 0) {
			mali_recent[i].kind = kind;
			mali_recent[i].width = width;
			mali_recent[i].height = height;
			mali_recent[i].row_bytes = row_bytes;
			mali_recent[i].format = format;
			mali_recent[i].offset = offset;
		}
	}
	lck_mtx_unlock(&mali_lock);
}

static void
say_kind(int i)
{
	if (mali_recent[i].kind == 2) {
		printf("mali:     an IOSurface %ux%u, %u bytes a row, format %#x, from byte %u\n", mali_recent[i].width,
		    mali_recent[i].height, mali_recent[i].row_bytes, mali_recent[i].format, mali_recent[i].offset);
	} else {
		printf("mali:     %s\n", mali_recent[i].kind == 1 ? "a panfrost BO" : "made by nothing that said");
	}
}

static void
describe_address(uint64_t address)
{
	int below = -1;

	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].size == 0) {
			continue;
		}
		if (address >= mali_recent[i].address && address < mali_recent[i].address + mali_recent[i].size) {
			printf("mali:   the fault is %#llx into a buffer of %#llx bytes at %#llx, %s\n",
			    address - mali_recent[i].address, mali_recent[i].size, mali_recent[i].address,
			    mali_recent[i].freed ? "FREED already" : "still there (a page not entered?)");
			say_kind(i);
			return;
		}
		if (mali_recent[i].address <= address && (below < 0 || mali_recent[i].address > mali_recent[below].address)) {
			below = i;
		}
	}
	if (below >= 0) {
		printf("mali:   the fault is %#llx past the end of a buffer of %#llx bytes at %#llx (%s)\n",
		    address - (mali_recent[below].address + mali_recent[below].size), mali_recent[below].size,
		    mali_recent[below].address, mali_recent[below].freed ? "freed" : "live");
		say_kind(below);
	} else {
		printf("mali:   the fault is in no buffer of the last %u\n", MALI_RECENT);
	}
}

/* The last job that did not finish, to be repeated where the end of the log keeps it. */
static struct {
	uint32_t count, slot, raw, status, mmu, fault, fault_address;
	uint64_t chain, transtab, walk[3];      /* the tables the GPU had, and their entries for the fault's address */
} last_failure;

bool
redwood_mali_map(uint64_t gpu, uint64_t phys)
{
	bool done;

	lck_mtx_lock(&mali_lock);
	done = map_page(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return done;
}

void redwood_mali_mapped(void);

bool
redwood_mali_ready(void)
{
	return reached >= 6;
}

/* Linux's DRM_PANFROST_PARAM_*, in its order, from the registers they come from. */
bool
redwood_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00,    /* STACK_PRESENT (none on Bifrost: reads 0) */
		[6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c, [11] = 0x010, [12] = 0x014,
		[13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8, [17] = 0x300,
		[18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!redwood_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:
		*value = gpu_id >> 16;
		return true;
	case 1:
		*value = gpu_id & 0xffff;
		return true;
	case 2:
	case 3:
	case 4:
		*value = rd(mali, 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali, 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:                                /* NR_CORE_GROUPS */
		*value = 1;
		return true;
	case 39:
		/*
		 * THREAD_TLS_ALLOC, which Bifrost has (0x310) and the G52 here needs: answered 0, panfrost
		 * sized the stack of a spilling shader by THREAD_MAX_THREADS instead, and the GPU, which
		 * gives each core more stacks than that, wrote past the end of it (25 October: every write
		 * fault of the uber shaders, up to 0x15870 past a 0x60000 buffer). Never less than that
		 * fallback, whatever the register says.
		 */
		*value = rd(mali, 0x310);
		if (*value < rd(mali, 0x0a0)) {
			*value = rd(mali, 0x0a0);
		}
		{
			static int said;
			if (!said++) {
				printf("mali: THREAD_TLS_ALLOC %#x, THREAD_MAX_THREADS %#x, SHADER_PRESENT %#x\n", rd(mali, 0x310),
				    rd(mali, 0x0a0), rd(mali, 0x100));
			}
		}
		return true;
	case 40:                                /* AFBC_FEATURES: not on this GPU */
		*value = 0;
		return true;
	default:
		if (which >= 22 && which <= 37) {
			*value = rd(mali, 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali, simple[which]);
			return true;
		}
		return false;
	}
}

/* `size` bytes of GPU addresses, never given out twice: there are 32 GB of them. 0: none left. */
uint64_t
redwood_mali_address(uint64_t size)
{
	uint64_t address;

	lck_mtx_lock(&mali_lock);
	address = next_address;
	if (address + size > GPU_ADDRESS_END) {
		address = 0;
	} else {
		next_address += (size + 0xffff) & ~0xffffull;   /* 64 KB between buffers: an overrun faults */
		mali_recent[mali_recent_next % MALI_RECENT].address = address;
		mali_recent[mali_recent_next % MALI_RECENT].size = size;
		mali_recent[mali_recent_next % MALI_RECENT].freed = 0;
		mali_recent[mali_recent_next % MALI_RECENT].kind = 0;
		mali_recent_next++;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

void
redwood_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == gpu && mali_recent[i].size != 0) {
			mali_recent[i].freed = 1;
		}
	}
	for (uint64_t at = gpu; at < gpu + size; at += 4096) {        /* the GPU's pages, not the kernel's 16 KB */
		uint64_t *entry = &pt_top[(at >> 30) & 31];

		if (!(*entry & 1)) {
			continue;
		}
		entry = &((uint64_t *)phystokv(*entry & PTE_ADDRESS))[(at >> 21) & 511];
		if (!(*entry & 1)) {
			continue;
		}
		entry = &((uint64_t *)phystokv(*entry & PTE_ADDRESS))[(at >> 12) & 511];
		*entry = 0;
		FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	}
	lck_mtx_unlock(&mali_lock);
	lck_mtx_lock(&mali_job_lock);                   /* an entry taken out: the TLB may hold it */
	lck_mtx_lock(&mali_lock);
	as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/*
 * After entries change: the GPU's MMU forgets what it remembered of the tables. The address space is
 * set up again from the top -- an UPDATE drops everything the MMU had cached for it -- rather than a
 * region locked and its tables flushed: with the region flush a fragment job faulted on a page of a
 * buffer whose entry was there, written and pushed out to memory. Jobs are run one at a time and to
 * their end, so the GPU is idle whenever this runs.
 */
void
redwood_mali_mapped(void)
{
	if (mali_fastmap) {
		return;         /* see TRANSCFG_WALK_NC */
	}
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/* The entries of each level for GPU address `gpu`, as far as there are tables (with the lock held). */
static void
walk_tables(uint64_t gpu, uint64_t walk[3])
{
	uint64_t *table = pt_top;
	static const unsigned shift[3] = { 30, 21, 12 };

	walk[0] = walk[1] = walk[2] = 0;
	for (int level = 0; level < 3 && table != NULL; level++) {
		walk[level] = table[(gpu >> shift[level]) & (level == 0 ? 31 : 511)];
		table = (walk[level] & 1) && level < 2 ? (uint64_t *)phystokv(walk[level] & PTE_ADDRESS) : NULL;
	}
}

/*
 * One job chain, from GPU address `chain`, run to its end before this returns: fragment jobs
 * (`fragment`) on slot 0, the rest on slot 1. 0 when it finished; otherwise the slot's status, a
 * fault or 0xff for no answer within a second -- and then the GPU is reset, so the next one finds
 * it working.
 */
uint32_t
redwood_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1, js = fragment ? JS0 : JS1, raw, status = 0;
	bool finished;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(mali, js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, js + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, js + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, js + JS_COMMAND_NEXT, 1);
	lck_mtx_unlock(&mali_lock);
	finished = wait_job(JOB_INT_RAWSTAT, (1u << slot) | (1u << (slot + 16)), 1u << slot, 1000);
	lck_mtx_lock(&mali_lock);
	if (!finished) {
		raw = rd(mali, JOB_INT_RAWSTAT);
		status = (raw & (1u << (slot + 16))) ? rd(mali, js + JS_STATUS) : 0xff;
		last_failure.count++;
		last_failure.slot = slot;
		last_failure.raw = raw;
		last_failure.status = rd(mali, js + JS_STATUS);
		last_failure.mmu = rd(mali, MMU_INT_RAWSTAT);
		last_failure.fault = rd(mali, AS0 + AS_FAULTSTATUS);
		last_failure.fault_address = rd(mali, AS0 + 0x20);
		last_failure.chain = chain;
		last_failure.transtab = rd(mali, AS0 + AS_TRANSTAB_LO) | ((uint64_t)rd(mali, AS0 + AS_TRANSTAB_HI) << 32);
		walk_tables(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32), last_failure.walk);
		printf("mali: a job on slot %u did not finish: interrupts %#x, status %#x, MMU %#x fault %#x at %#x\n", slot, raw,
		    last_failure.status, last_failure.mmu, last_failure.fault, last_failure.fault_address);
		describe_address(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32));
		wr(mali, GPU_INT_CLEAR, 0xffffffffu);
		wr(mali, GPU_CMD, 1);                   /* soft reset, then everything back as it was */
		(void)wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, 100);
		wr(mali, L2_PWRON, rd(mali, L2_PRESENT));
		wr(mali, TILER_PWRON, rd(mali, TILER_PRESENT));
		wr(mali, SHADER_PWRON, shader_mask);
		(void)wait_for(mali, SHADER_READY, shader_mask, shader_mask, 50);
		as0_update(pt_top_phys, TRANSCFG_AARCH64_4K);
		wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	}
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

/*
 * The jobs in a queue, run by a thread of their own: a submission returns at once with the job's
 * number, and whoever needs a job done waits for that number (redwood_mali_wait). Run one by one
 * in the order given, which is the order panfrost needs -- a pass's vertex and tiler chain before
 * its fragment chain, one pass before the next that reads it. Before this every submission ran its
 * job to the end before returning: a frame of CoreAnimation's with its blur passes had the window
 * server waiting for the GPU at every one of them, the processor and the GPU taking turns (24
 * October, 11 to 17 ms a frame of the processor's).
 */
#define MALI_QUEUE      64
static struct {
	uint64_t chain, seq, queued;
	bool fragment;
} mali_queue[MALI_QUEUE];
/* boot-arg mali_trace=1: a line for every job, "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" in us of
 * mach_absolute_time, beside touch_lat's lines of the display's and the Metal driver's */
static uint32_t mali_trace;
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lock_group);

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t chain, seq;
		bool fragment;

		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
		}
		chain = mali_queue[mali_queue_head].chain;
		seq = mali_queue[mali_queue_head].seq;
		uint64_t queued = mali_queue[mali_queue_head].queued;
		fragment = mali_queue[mali_queue_head].fragment;
		lck_mtx_unlock(&mali_queue_lock);

		{
			/* how busy the GPU is: every five seconds, its share of the time and by which slot */
			static uint64_t busy[2], since, jobs[2];
			uint64_t began = mach_absolute_time(), now, ns;

			if (redwood_mali_submit(chain, fragment) != 0) {
				gMaliFailed++;
			}
			now = mach_absolute_time();
			if (mali_trace) {
				uint64_t q, b, e;

				absolutetime_to_nanoseconds(queued, &q);
				absolutetime_to_nanoseconds(began, &b);
				absolutetime_to_nanoseconds(now, &e);
				printf("tl: j %llu %c q %llu b %llu e %llu\n", seq, fragment ? 'f' : 'v', q / 1000, b / 1000, e / 1000);
			}
			absolutetime_to_nanoseconds(now - began, &ns);
			busy[fragment] += ns;
			jobs[fragment]++;
			if (since == 0) {
				since = now;
			} else {
				absolutetime_to_nanoseconds(now - since, &ns);
				if (ns >= 5ull * NSEC_PER_SEC) {
					printf("mali: GPU busy %llu%% of %llu s: vertex and tiler %llu jobs %llu ms (%llu us each), "
					    "fragment %llu jobs %llu ms (%llu us each)\n", (busy[0] + busy[1]) * 100 / ns,
					    ns / NSEC_PER_SEC, jobs[0], busy[0] / 1000000, jobs[0] ? busy[0] / 1000 / jobs[0] : 0,
					    jobs[1], busy[1] / 1000000, jobs[1] ? busy[1] / 1000 / jobs[1] : 0);
					busy[0] = busy[1] = jobs[0] = jobs[1] = 0;
					since = now;
				}
			}
		}

		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);       /* (room in the queue) */
	}
}

/* A job chain into the queue: its number. */
uint64_t redwood_mali_enqueue(uint64_t chain, bool fragment);
uint64_t
redwood_mali_enqueue(uint64_t chain, bool fragment)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread;

		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
	}
	seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

	mali_queue[at].chain = chain;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq = seq;
	mali_queue[at].queued = mach_absolute_time();
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

/* Wait until job `seq` is done: true when it is; with timeout 0 only a look. */
bool redwood_mali_wait(uint64_t seq, uint64_t timeout_ns);
bool
redwood_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	uint64_t deadline;
	bool done;

	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;     /* (an "infinite" one, INT64_MAX, would wrap) */
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	done = mali_done_seq >= seq;
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

static void
mali_thread(__unused void *parameter, __unused wait_result_t result)
{
	mali_up();
	printf("mali: came up to step %u of 6\n", reached);
	if (reached >= 6) {
		RedwoodMaliStart();             /* programs can have it now (iokit/Kernel/RedwoodMali.cpp) */
	}
	for (;;) {
		delay(60 * 1000 * 1000);
		printf("mali: step %u of 6; user space says %u%s; %u clients, %u buffers, %u job chains (%u failed), %u pictures shown\n",
		    reached, user_said, user_said == 1 ? " -- a program ran a job on the GPU" : "", gMaliClients, gMaliBuffers,
		    gMaliSubmits, gMaliFailed, gMaliShown);
		lck_mtx_lock(&kept_lock);
		for (uint32_t i = kept_next > KEPT_LINES ? kept_next - KEPT_LINES : 0; i < kept_next; i++) {
			printf("kept: %s\n", kept_lines[i % KEPT_LINES]);
		}
		lck_mtx_unlock(&kept_lock);
		if (gl_said != 0) {
			printf("mali: gl_test drew on %s; %u%% of the picture inside the triangle (26%% by geometry); "
			    "the middle pixel red %u green %u blue %u alpha %u\n", gl_said / 1000 == 1 ? "THE MALI" : "the processor",
			    gl_said % 1000, gl_middle & 0xff, (gl_middle >> 8) & 0xff, (gl_middle >> 16) & 0xff, gl_middle >> 24);
		}
		if (last_failure.count != 0) {
			printf("mali: the last of %u unfinished: slot %u, chain at %#llx, interrupts %#x, slot status %#x, "
			    "MMU interrupts %#x, fault status %#x at address %#x\n", last_failure.count, last_failure.slot,
			    last_failure.chain, last_failure.raw, last_failure.status, last_failure.mmu, last_failure.fault,
			    last_failure.fault_address);
			printf("mali: the GPU's tables at %#llx (ours at %#llx); for the fault's address: %#llx, %#llx, %#llx\n",
			    last_failure.transtab, pt_top_phys, last_failure.walk[0], last_failure.walk[1], last_failure.walk[2]);
			for (uint32_t i = 0; i < gMaliBuffers && i < 32; i++) {
				printf("mali:   buffer %u at %#llx..%#llx (asked %#x, flags %#x)%s\n", i, gMaliMade[i].address,
				    gMaliMade[i].address + gMaliMade[i].size, gMaliMade[i].asked, gMaliMade[i].flags,
				    last_failure.fault_address >= gMaliMade[i].address &&
				    last_failure.fault_address < gMaliMade[i].address + gMaliMade[i].size ? "  <- the fault" : "");
			}
		}
	}
}

void
redwood_mali_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	(void)PE_parse_boot_argn("mali_fastmap", &mali_fastmap, sizeof(mali_fastmap));
	(void)PE_parse_boot_argn("mali_trace", &mali_trace, sizeof(mali_trace));
	if (!PE_parse_boot_argn("mali", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
